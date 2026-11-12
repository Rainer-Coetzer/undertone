// Command-line entry points that never open a window, so the engine can be tested by a script:
//   Undertone.exe --separate IN OUTDIR [--cpu]   writes instrumental.wav and vocals.wav, prints timings
//   Undertone.exe --selftest                     transform and parser checks
//   Undertone.exe --lyrics IN [--whisper]        prints the lyrics the app would show
#include "headless.h"

#include <chrono>
#include <cmath>
#include <cstdio>

#include "audio.h"
#include "lyrics.h"
#include "player.h"
#include "proc.h"
#include "separate.h"

namespace {

double secondsSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

double rms(const std::vector<float>& v) {
    double s = 0;
    for (float x : v) s += (double)x * x;
    return v.empty() ? 0 : std::sqrt(s / v.size());
}

int separateCommand(const std::string& in, const std::string& outDir, bool cpu) {
    std::string error;
    TrackInfo info;
    if (!probeTrack(in, info, error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    std::printf("title=%s\nartist=%s\nalbum=%s\nduration=%.2f\n", info.title.c_str(), info.artist.c_str(), info.album.c_str(), info.duration);

    std::vector<float> mix, inst, voc;
    std::atomic<float> progress{0};
    std::atomic<bool> cancel{false};
    auto t = std::chrono::steady_clock::now();
    if (!decodeStereo(in, kModelRate, info.duration, mix, progress, error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    std::printf("decode_seconds=%.2f\n", secondsSince(t));

    const std::string model = findModel("UVR-MDX-NET-Inst_HQ_3.onnx");
    if (model.empty()) { std::fprintf(stderr, "The separation model is missing from the models folder.\n"); return 1; }
    Separator sep;
    t = std::chrono::steady_clock::now();
    if (!sep.load(model, !cpu, error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    std::printf("device=%s\nload_seconds=%.2f\n", sep.device().c_str(), secondsSince(t));
    t = std::chrono::steady_clock::now();
    if (!sep.run(mix, inst, voc, progress, cancel, error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    std::printf("separate_seconds=%.2f\n", secondsSince(t));

    float worst = 0;
    for (size_t i = 0; i < mix.size(); ++i) worst = std::max(worst, std::abs(mix[i] - inst[i] - voc[i]));
    std::printf("mix_rms=%.5f\ninstrumental_rms=%.5f\nvocals_rms=%.5f\nsum_error=%.7f\n", rms(mix), rms(inst), rms(voc), worst);

    if (!encodeStereo(outDir + "\\instrumental.wav", inst.data(), inst.size() / 2, kModelRate, {"-c:a", "pcm_s16le"}, error) ||
        !encodeStereo(outDir + "\\vocals.wav", voc.data(), voc.size() / 2, kModelRate, {"-c:a", "pcm_s16le"}, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    return 0;
}

int selfTest() {
    int bad = 0;
    const float err = stftRoundTripError(6144, 1024, 256);
    std::printf("stft_round_trip_error=%.7f\n", err);
    if (err > 1e-4f) ++bad;

    const Lyrics l = parseLrc("[ar:Someone]\n[offset:+500]\n[00:12.50]First line\n[00:15.00][01:05.20]Repeated\n[00:20.123] Third \n[00:25.00]\nno timestamp here\n");
    std::printf("lrc_lines=%d\n", (int)l.lines.size());
    for (const LyricLine& line : l.lines) std::printf("  %.3f|%s\n", line.time, line.text.c_str());
    // [offset:+500] means the lyrics should appear half a second sooner.
    if (l.lines.size() != 5 || std::abs(l.lines[0].time - 12.0) > 1e-6 || l.lines[0].text != "First line" || l.lines[4].text != "Repeated" || !l.lines[3].text.empty()) ++bad;
    const Lyrics plain = parseLrc("just words\nmore words\n");
    if (!plain.lines.empty()) ++bad;

    // Key and tempo: a 440 Hz tone up two semitones at 80% speed should come out near 493.9 Hz
    // and 25% longer.
    std::vector<float> tone((size_t)kModelRate * 4 * 2);
    for (size_t i = 0; i < tone.size() / 2; ++i) tone[i * 2] = tone[i * 2 + 1] = 0.5f * (float)std::sin(2 * 3.14159265358979 * 440.0 * i / kModelRate);
    const std::vector<float> shifted = renderStretched(tone, 2.f, 0.8f);
    const double lengthRatio = (double)shifted.size() / tone.size();
    size_t crossings = 0;
    const size_t from = shifted.size() / 2 / 4, to = shifted.size() / 2 * 3 / 4;  // the steady middle
    for (size_t i = from + 1; i < to; ++i) if ((shifted[(i - 1) * 2] < 0) != (shifted[i * 2] < 0)) ++crossings;
    const double freq = crossings / 2.0 / ((double)(to - from) / kModelRate);
    std::printf("stretch_length_ratio=%.3f\nstretch_frequency=%.1f\n", lengthRatio, freq);
    if (std::abs(lengthRatio - 1.25) > 0.01 || std::abs(freq - 493.9) > 6) ++bad;
    std::printf(bad ? "SELFTEST FAILED\n" : "SELFTEST OK\n");
    return bad ? 1 : 0;
}

int lyricsCommand(const std::string& in, bool whisperOnly) {
    std::string error;
    TrackInfo info;
    if (!probeTrack(in, info, error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    Lyrics l;
    if (!whisperOnly) {
        l = loadLrcBeside(in);
        if (l.lines.empty()) l = fetchLrclib(info.title, info.artist, info.album, info.duration, error);
    } else {
        std::atomic<float> progress{0};
        std::atomic<bool> cancel{false};
        l = transcribeWithWhisper(in, progress, cancel, error);
    }
    std::printf("source=%s\nlines=%d\n", l.source.c_str(), (int)l.lines.size());
    if (!error.empty()) std::printf("note=%s\n", error.c_str());
    for (const LyricLine& line : l.lines) std::printf("  %7.2f|%s\n", line.time, line.text.c_str());
    return l.lines.empty() ? 1 : 0;
}

}  // namespace

int runHeadless(const std::vector<std::string>& args) {
    bool cpu = false, whisper = false;
    std::vector<std::string> rest;
    std::string command;
    for (const std::string& a : args) {
        if (a == "--cpu") cpu = true;
        else if (a == "--whisper") whisper = true;
        else if (a == "--separate" || a == "--selftest" || a == "--lyrics") command = a;
        else rest.push_back(a);
    }
    if (command.empty()) return -1;
    int code = 2;
    if (command == "--selftest") code = selfTest();
    else if (command == "--separate" && rest.size() == 2) code = separateCommand(rest[0], rest[1], cpu);
    else if (command == "--lyrics" && rest.size() == 1) code = lyricsCommand(rest[0], whisper);
    else std::fprintf(stderr, "Usage: --separate IN OUTDIR [--cpu] | --lyrics IN [--whisper] | --selftest\n");
    std::fflush(stdout);
    return code;
}
