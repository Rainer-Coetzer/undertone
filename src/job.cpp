#include "job.h"

#include <ctime>

#include "audio.h"
#include "proc.h"
#include "separate.h"

const char* stepName(int step) {
    switch (step) {
    case STEP_READ: return "Reading the song";
    case STEP_MODEL: return "Starting the vocal remover";
    case STEP_SEPARATE: return "Removing the vocals";
    case STEP_SAVE: return "Saving to your library";
    case STEP_LYRICS: return "Looking up the lyrics";
    case STEP_TRANSCRIBE: return "Listening for the lyrics";
    default: return "Ready";
    }
}

Lyrics findLyrics(const SongEntry& e, bool allowWhisper, std::atomic<int>& step, std::atomic<float>& progress,
                  const std::atomic<bool>& cancel, std::string& note) {
    note.clear();
    step = STEP_LYRICS;
    progress = 0.f;
    if (!e.sourcePath.empty()) {
        Lyrics l = loadLrcBeside(e.sourcePath);
        if (!l.lines.empty()) return l;
    }
    std::string err;
    Lyrics l = fetchLrclib(e.title, e.artist, e.album, e.duration, err);
    if (!l.lines.empty()) return l;
    if (cancel || !allowWhisper) { note = err; return {}; }
    step = STEP_TRANSCRIBE;
    std::string werr;
    l = transcribeWithWhisper(e.dir + "\\vocals.flac", progress, cancel, werr);
    if (l.lines.empty()) note = !werr.empty() ? werr : !err.empty() ? err : "No lyrics were found, and none could be heard.";
    else if (!err.empty()) note = "Offline, so these lyrics were transcribed from the vocals.";
    return l;
}

void Job::start(Library& lib, std::shared_ptr<Separator> sep) {
    worker = std::thread([this, &lib, sep] { run(lib, sep); finished = true; });
}

bool Job::loadStems(const std::string& dir) {
    stems = std::make_shared<Stems>();
    std::atomic<float> p{0};
    if (!decodeStereo(dir + "\\instrumental.flac", kModelRate, entry.duration, stems->instrumental, p, error)) return false;
    progress = 0.5f;
    if (!decodeStereo(dir + "\\vocals.flac", kModelRate, entry.duration, stems->vocals, p, error)) return false;
    // FLAC round trips can differ by a frame at the end; play only what both have.
    const size_t n = std::min(stems->instrumental.size(), stems->vocals.size());
    stems->instrumental.resize(n);
    stems->vocals.resize(n);
    return true;
}

void Job::run(Library& lib, std::shared_ptr<Separator> sep) {
    if (!reopenId.empty()) {
        step = STEP_READ;
        if (!lib.get(reopenId, entry)) { error = "That song is no longer in the library."; return; }
        if (!loadStems(entry.dir)) return;
        lyrics = Library::loadLyrics(entry);
        ok = true;
        step = STEP_DONE;
        return;
    }

    step = STEP_READ;
    TrackInfo info;
    if (!probeTrack(sourcePath, info, error)) return;
    const std::string id = contentId(sourcePath);
    if (id.empty()) { error = "This file could not be opened."; return; }

    // The same file again (even renamed or moved) opens straight from the library.
    if (lib.get(id, entry)) {
        entry.sourcePath = sourcePath;
        lib.save(entry);
        if (!loadStems(entry.dir)) return;
        lyrics = Library::loadLyrics(entry);
        note = "Already in your library.";
        ok = true;
        step = STEP_DONE;
        return;
    }

    std::vector<float> mix;
    if (!decodeStereo(sourcePath, kModelRate, info.duration, mix, progress, error)) return;
    if (cancel) { error = "Cancelled."; return; }

    step = STEP_MODEL;
    progress = 0.f;
    if (!sep->loaded()) {
        const std::string model = findModel("UVR-MDX-NET-Inst_HQ_3.onnx");
        if (model.empty()) { error = "The vocal removal model is missing from the models folder."; return; }
        if (!sep->load(model, allowGpu, error)) return;
    }
    device = sep->device();

    step = STEP_SEPARATE;
    stems = std::make_shared<Stems>();
    if (!sep->run(mix, stems->instrumental, stems->vocals, progress, cancel, error)) return;
    mix.clear();
    mix.shrink_to_fit();

    step = STEP_SAVE;
    progress = 0.f;
    entry = SongEntry{};
    entry.id = id;
    entry.dir = lib.dirFor(id);
    entry.title = info.title;
    entry.artist = info.artist;
    entry.album = info.album;
    entry.sourcePath = sourcePath;
    entry.duration = (double)stems->frames() / kModelRate;
    entry.added = (long long)std::time(nullptr);
    if (!makeDirs(entry.dir)) { error = "Could not create the library folder: " + entry.dir; return; }
    // 24-bit FLAC: lossless for anything a listener can hear, about half the size of WAV.
    const std::vector<std::string> flac = {"-c:a", "flac", "-sample_fmt", "s32", "-bits_per_raw_sample", "24", "-compression_level", "5"};
    if (!encodeStereo(entry.dir + "\\instrumental.flac", stems->instrumental.data(), stems->frames(), kModelRate, flac, error)) return;
    progress = 0.5f;
    if (!encodeStereo(entry.dir + "\\vocals.flac", stems->vocals.data(), stems->frames(), kModelRate, flac, error)) return;
    if (info.hasCover) extractCover(sourcePath, entry.dir + "\\cover.bmp", 512);
    lib.save(entry);
    entry.hasCover = pathExists(entry.dir + "\\cover.bmp");

    // From here the window may already be playing this song from entry and stems, so neither is
    // written again; the lyrics' source goes to the library through a copy.
    lyrics = findLyrics(entry, allowWhisper, step, progress, cancel, note);
    SongEntry withLyrics = entry;
    withLyrics.lyricsSource = lyrics.source;
    Library::saveLyrics(withLyrics, lyrics);
    lib.save(withLyrics);
    ok = true;
    step = STEP_DONE;
}
