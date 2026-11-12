// Undertone: strips the vocals from a song and shows the lyrics in time as it plays.
//
// The look follows Dusk Reader and Sandscope (their dark theme and Liquid Glass panels) with a
// white accent. Separation runs an MDX-Net model through ONNX Runtime; lyrics come from an .lrc
// file beside the song, then LRCLIB, then Whisper listening to the separated vocals.
//
// Test flags, so a run can be checked without anyone clicking:
//   Undertone.exe [FILE] [--song ID | --last] [--size WxH] [--time SECONDS] [--play SECONDS]
//       [--click X Y]... [--modal export|lyrics|settings] [--library DIR] [--shot out.bmp]
//   Undertone.exe --separate IN OUTDIR [--cpu] | --lyrics IN [--whisper] | --selftest
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_syswm.h>
#include <SDL_ttf.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../third_party/json.hpp"
#include "audio.h"
#include "headless.h"
#include "job.h"
#include "library.h"
#include "lyrics.h"
#include "player.h"
#include "proc.h"
#include "separate.h"
#include "threads.h"
#include "ui.h"

namespace {

// Segoe MDL2 Assets ships with Windows 10 and 11.
const char* kIconPlay = "\xEE\x9D\xA8";
const char* kIconPause = "\xEE\x9D\xA9";
const char* kIconDelete = "\xEE\x9D\x8D";
const char* kIconSettings = "\xEE\x9C\x93";
const char* kIconSearch = "\xEE\x9C\xA1";
const char* kIconMusic = "\xEE\xA3\x96";

std::string formatTime(double s) {
    if (s < 0) s = 0;
    const int t = (int)s;
    char b[32];
    if (t >= 3600) std::snprintf(b, sizeof b, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else std::snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
    return b;
}

std::string lowerAscii(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Characters that Windows refuses in a file name.
std::string safeFileName(std::string s) {
    for (char& c : s) if (std::string("<>:\"/\\|?*").find(c) != std::string::npos || (unsigned char)c < 32) c = '_';
    while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
    return s.empty() ? "Song" : s;
}

struct Settings {
    int winW = 1280, winH = 800;
    bool useGpu = true, useWhisper = true;
    std::string lastSong;
};

std::string settingsPath() { return exeDir() + "\\settings.json"; }

void loadSettings(Settings& s) {
    const nlohmann::json j = nlohmann::json::parse(readTextFile(settingsPath()), nullptr, false);
    if (!j.is_object()) return;
    s.winW = std::clamp(j.value("win_w", s.winW), 1000, 7680);
    s.winH = std::clamp(j.value("win_h", s.winH), 660, 4320);
    s.useGpu = j.value("use_gpu", s.useGpu);
    s.useWhisper = j.value("use_whisper", s.useWhisper);
    s.lastSong = j.value("last_song", std::string());
}

void saveSettings(const Settings& s) {
    nlohmann::json j;
    j["win_w"] = s.winW; j["win_h"] = s.winH;
    j["use_gpu"] = s.useGpu; j["use_whisper"] = s.useWhisper;
    j["last_song"] = s.lastSong;
    writeTextFile(settingsPath(), j.dump(2));
}

// Looking lyrics up again, or transcribing, for the song on screen.
struct LyricsTask {
    SongEntry entry;
    std::string title, artist;
    bool transcribe = false;
    std::atomic<int> step{STEP_LYRICS};
    std::atomic<float> progress{0.f};
    std::atomic<bool> finished{false}, cancel{false};
    Lyrics result;
    std::string note;
    std::thread worker;

    void start() {
        worker = std::thread([this] {
            if (transcribe) {
                step = STEP_TRANSCRIBE;
                result = transcribeWithWhisper(entry.dir + "\\vocals.flac", progress, cancel, note);
                if (result.lines.empty() && note.empty()) note = "Nothing could be heard clearly enough to write down.";
            } else {
                result = fetchLrclib(title, artist, entry.album, entry.duration, note);
                if (result.lines.empty() && note.empty()) note = "LRCLIB has no synced lyrics for that title and artist.";
            }
            finished = true;
        });
    }
    ~LyricsTask() { cancel = true; if (worker.joinable()) worker.join(); }
};

enum ExportWhat { EX_INSTRUMENTAL = 0, EX_VOCALS, EX_BOTH };
enum ExportFormat { FMT_WAV = 0, FMT_FLAC, FMT_MP3 };

struct ExportTask {
    std::shared_ptr<const Stems> stems;
    int what = EX_INSTRUMENTAL, format = FMT_WAV;
    float guide = 0, semitones = 0, tempo = 1;
    std::string path;
    std::atomic<bool> finished{false};
    std::atomic<float> progress{0.f};
    bool ok = false;
    std::string error, savedFirst;
    std::thread worker;

    static std::vector<std::string> codecArgs(int format) {
        if (format == FMT_FLAC) return {"-c:a", "flac", "-sample_fmt", "s32", "-bits_per_raw_sample", "24"};
        if (format == FMT_MP3) return {"-c:a", "libmp3lame", "-b:a", "320k"};
        return {"-c:a", "pcm_s24le"};
    }

    bool writeOne(const std::string& file, bool vocals) {
        const size_t frames = stems->frames();
        std::vector<float> buf(frames * 2);
        const float* in = stems->instrumental.data();
        const float* vo = stems->vocals.data();
        for (size_t i = 0; i < frames * 2; ++i) buf[i] = vocals ? vo[i] : in[i] + guide * vo[i];
        buf = renderStretched(buf, semitones, tempo);
        // Key changes and summing a guide vocal can push peaks over full scale; only ever turned down.
        float peak = 0;
        for (float v : buf) peak = std::max(peak, std::abs(v));
        if (peak > 0.999f) for (float& v : buf) v *= 0.999f / peak;
        return encodeStereo(file, buf.data(), buf.size() / 2, kModelRate, codecArgs(format), error);
    }

    void start() {
        worker = std::thread([this] {
            if (what == EX_INSTRUMENTAL || what == EX_VOCALS) {
                ok = writeOne(path, what == EX_VOCALS);
                savedFirst = path;
            } else {
                const std::string stem = stemOf(path), ext = path.substr(stem.size());
                std::string base = stem;
                const size_t tag = base.rfind(" (Instrumental)");
                if (tag != std::string::npos) base.erase(tag);
                ok = writeOne(base + " (Instrumental)" + ext, false);
                progress = 0.5f;
                if (ok) ok = writeOne(base + " (Vocals)" + ext, true);
                savedFirst = base + " (Instrumental)" + ext;
            }
            progress = 1.f;
            finished = true;
        });
    }
    ~ExportTask() { if (worker.joinable()) worker.join(); }
};

// One lyric line as laid out on the stage.
struct LyricItem {
    double time = 0, end = 0;
    std::string text;
    bool gap = false;  // an instrumental break, drawn as three dots
    std::vector<std::string> rows;
    int y = 0, h = 0;
    float emphasis = 0;
};

enum Modal { MODAL_NONE = 0, MODAL_EXPORT, MODAL_LYRICS, MODAL_SETTINGS, MODAL_REMOVE };

struct App {
    SDL_Window* window = nullptr;
    Painter p;
    SDL_Renderer* ren = nullptr;
    int W = 1280, H = 800;
    float scale = 1.f;
    Settings s;
    bool glassOn = true, motionOn = true;

    Library lib;
    std::shared_ptr<Separator> separator = std::make_shared<Separator>();
    Player player;
    std::unique_ptr<Job> procJob, openJob;
    bool procTaken = false;  // the processing job's stems have been handed to the player
    std::unique_ptr<LyricsTask> lyricsTask;
    std::unique_ptr<ExportTask> exportTask;

    // ------------------------------------------------------------ the song on screen
    bool hasSong = false;
    SongEntry song;
    std::shared_ptr<Stems> stems;
    Lyrics lyrics;
    std::vector<LyricItem> items;
    int itemsWidth = -1;
    std::vector<float> peaksInst, peaksVoc;
    float level = 0, semis = 0, tempo = 1, syncOffset = 0;
    bool songSettingsDirty = false;
    std::string lyricsNote;
    float lyricScroll = 0;
    float manualScroll = 0;
    Uint32 manualUntil = 0;

    // ------------------------------------------------------------ look
    SDL_Rect sidebar{}, topBar{}, transport{}, stage{};
    std::vector<Uint32> scene;
    SDL_Texture* sceneTex = nullptr;
    unsigned sceneVersion = 1;
    bool layoutDirty = true, wallDirty = true;
    Glass glassSide, glassTop, glassTransport, glassModal;
    std::unordered_map<std::string, SDL_Texture*> thumbs;  // song id -> rounded cover
    SDL_Texture* bigCover = nullptr;
    std::string bigCoverId;

    // ------------------------------------------------------------ immediate-mode input
    int mx = -1, my = -1;
    bool mouseDown = false, mousePressed = false, mouseReleased = false;
    float wheel = 0;
    unsigned activeId = 0, focusId = 0;
    std::string typed;
    bool keyEnter = false, keyBackspace = false, keyEscape = false, keyTab = false;
    Modal modal = MODAL_NONE;
    std::string removeId, removeTitle;
    std::string search;
    float listScroll = 0, listScrollNow = 0;
    int listContentH = 0;
    int uiFrames = 3;
    Uint32 lastTick = 0;
    float dtMs = 16.f;

    // Lyrics dialog fields, and the export dialog's choices.
    std::string findTitle, findArtist;
    int exWhat = EX_INSTRUMENTAL, exFormat = FMT_WAV;
    bool exGuide = false, exStretch = true;
    std::string exportDone;

    std::string toast;
    Uint32 toastUntil = 0;

    // ------------------------------------------------------------ test hooks
    std::string shotPath, openOnStart, songOnStart, modalOnStart, libraryOverride;
    double setTime = -1, playFor = 0;
    Uint32 playUntil = 0;
    std::vector<std::pair<int, int>> pendingClicks;
    int exitCode = 0;
    bool running = true;

    int px(float v) const { return p.px(v); }

    // ------------------------------------------------------------ small helpers
    bool inside(SDL_Rect r) const { return mx >= r.x && my >= r.y && mx < r.x + r.w && my < r.y + r.h; }

    static unsigned hashId(const std::string& s) {
        unsigned h = 2166136261u;
        for (unsigned char c : s) h = (h ^ c) * 16777619u;
        return h ? h : 1;
    }

    bool blockedByModal(const std::string& id) const { return modal != MODAL_NONE && id.rfind("modal", 0) != 0; }

    void showToast(const std::string& m, Uint32 ms = 4500) {
        toast = m;
        toastUntil = SDL_GetTicks() + ms;
        std::fprintf(stderr, "%s\n", m.c_str());
        uiFrames = 3;
    }

    HWND windowHandle() {
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        return SDL_GetWindowWMInfo(window, &info) ? info.info.win.window : nullptr;
    }

    // ------------------------------------------------------------ widgets
    // A pressed-then-released click on r. Buttons in an open dialog stay live; everything
    // behind it is locked.
    bool clicked(const std::string& id, SDL_Rect r, bool enabled = true) {
        const unsigned uid = hashId(id);
        const bool hot = enabled && !blockedByModal(id) && inside(r);
        if (hot && mousePressed) { activeId = uid; focusId = 0; SDL_StopTextInput(); }
        if (activeId == uid && mouseReleased) { activeId = 0; return hot; }
        return false;
    }

    bool hot(const std::string& id, SDL_Rect r) const { return !blockedByModal(id) && inside(r); }

    bool button(const std::string& id, SDL_Rect r, const std::string& label, bool primary, bool enabled = true, int font = F_BODY) {
        const bool h = enabled && hot(id, r);
        const bool down = h && activeId == hashId(id);
        SDL_Color bg = primary ? kAccent : SDL_Color{255, 255, 255, 26};
        if (!enabled) bg = SDL_Color{255, 255, 255, 18};
        else if (down) bg = primary ? SDL_Color{205, 205, 205, 255} : SDL_Color{255, 255, 255, 60};
        else if (h) bg = primary ? SDL_Color{236, 236, 236, 255} : SDL_Color{255, 255, 255, 46};
        p.rounded(r, std::min(px(12), r.h / 2), bg);
        p.textCentred(label, r, font, !enabled ? withAlpha(kMuted, 120) : primary ? kOnAccent : kText);
        return clicked(id, r, enabled);
    }

    bool iconButton(const std::string& id, SDL_Rect r, const char* glyph, bool enabled = true, SDL_Color colour = kText) {
        const bool h = enabled && hot(id, r);
        if (h) p.rounded(r, r.h / 2, SDL_Color{255, 255, 255, (Uint8)(activeId == hashId(id) ? 56 : 34)});
        p.textCentred(glyph, r, F_ICON, enabled ? colour : withAlpha(kMuted, 110));
        return clicked(id, r, enabled);
    }

    // A slider in its own rectangle: label left, value right, track underneath.
    bool slider(const std::string& id, SDL_Rect r, const std::string& label, const std::string& value, float& norm, bool enabled = true) {
        const unsigned uid = hashId(id);
        const Uint8 dim = enabled ? 255 : 90;
        p.text(label, r.x, r.y, F_SMALL, withAlpha(kMuted, dim));
        const int vw = p.textWidth(value, F_SMALL);
        p.text(value, r.x + r.w - vw, r.y, F_SMALL, withAlpha(activeId == uid ? kText : kMuted, dim));
        const bool active = activeId == uid;
        const int trackH = px(5), thumbW = px(active ? 22 : 18), thumbH = px(active ? 16 : 13);
        const int ty = r.y + p.fontHeight(F_SMALL) + px(11);
        const int tx = r.x + thumbW / 2, tw = r.w - thumbW;
        SDL_Rect hitRow{r.x, ty - px(10), r.w, px(20)};
        bool changed = false;
        if (enabled && !blockedByModal(id) && mousePressed && inside(hitRow)) { activeId = uid; focusId = 0; }
        if (activeId == uid) {
            const float n = std::clamp((float)(mx - tx) / std::max(tw, 1), 0.f, 1.f);
            if (n != norm) { norm = n; changed = true; }
            if (mouseReleased) activeId = 0;
        }
        p.rounded(SDL_Rect{r.x, ty - trackH / 2, r.w, trackH}, trackH / 2, SDL_Color{255, 255, 255, 34});
        p.rounded(SDL_Rect{r.x, ty - trackH / 2, tx - r.x + (int)(norm * tw), trackH}, trackH / 2, SDL_Color{255, 255, 255, (Uint8)(enabled ? 215 : 60)});
        SDL_Rect thumb{tx + (int)(norm * tw) - thumbW / 2, ty - thumbH / 2, thumbW, thumbH};
        if (enabled) p.softShadow(thumb, thumbH / 2, px(5), 130);
        p.rounded(thumb, thumbH / 2, SDL_Color{255, 255, 255, (Uint8)(enabled ? 255 : 80)});
        // A hairline keeps the white thumb visible against the white filled track.
        p.rounded(SDL_Rect{thumb.x + thumb.w / 2 - px(1), thumb.y + px(3), std::max(1, px(2)), thumb.h - px(6)}, px(1), SDL_Color{0, 0, 0, 70});
        return changed;
    }

    bool segmented(const std::string& id, SDL_Rect r, const std::vector<std::string>& items2, int& value) {
        bool changed = false;
        p.rounded(SDL_Rect{r.x - px(2), r.y - px(2), r.w + px(4), r.h + px(4)}, px(11), SDL_Color{0, 0, 0, 50});
        const int n = (int)items2.size(), g = px(4);
        const int w = (r.w - (n - 1) * g) / n;
        for (int i = 0; i < n; ++i) {
            SDL_Rect c{r.x + i * (w + g), r.y, w, r.h};
            const std::string cid = id + std::to_string(i);
            const bool sel = value == i, h = hot(cid, c);
            if (sel) p.rounded(c, px(9), SDL_Color{255, 255, 255, 230});
            else if (h) p.rounded(c, px(9), SDL_Color{255, 255, 255, 26});
            p.textCentred(items2[i], c, F_BODY, sel ? kOnAccent : kText);
            if (clicked(cid, c) && value != i) { value = i; changed = true; }
        }
        return changed;
    }

    bool toggle(const std::string& id, SDL_Rect row, const std::string& label, bool& v) {
        const int tw = px(44), th = px(24);
        p.text(label, row.x, row.y + (row.h - p.fontHeight(F_BODY)) / 2, F_BODY, kText);
        SDL_Rect sw{row.x + row.w - tw, row.y + (row.h - th) / 2, tw, th};
        p.rounded(sw, th / 2, v ? SDL_Color{255, 255, 255, 230} : SDL_Color{255, 255, 255, 34});
        const int knob = th - px(4);
        SDL_Rect k{v ? sw.x + tw - knob - px(2) : sw.x + px(2), sw.y + px(2), knob, knob};
        p.rounded(k, knob / 2, v ? kOnAccent : SDL_Color{235, 235, 235, 255});
        if (clicked(id, row)) { v = !v; return true; }
        return false;
    }

    // Single-line entry. Returns true when Enter is pressed in it.
    bool textField(const std::string& id, SDL_Rect r, std::string& value, const std::string& placeholder, const char* glyph = nullptr) {
        const unsigned uid = hashId(id);
        bool submitted = false;
        if (!blockedByModal(id) && mousePressed) {
            if (inside(r)) { focusId = uid; SDL_StartTextInput(); }
            else if (focusId == uid) { focusId = 0; SDL_StopTextInput(); }
        }
        const bool focused = focusId == uid;
        if (focused) {
            if (value.size() < 200) value += typed;
            if (keyBackspace && !value.empty()) {
                size_t i = value.size();
                while (i > 0) { --i; if (((unsigned char)value[i] & 0xC0) != 0x80) break; }
                value.erase(i);
            }
            if (keyEnter) submitted = true;
            if (keyEscape) { focusId = 0; SDL_StopTextInput(); }
        }
        p.rounded(r, px(9), focused ? SDL_Color{0, 0, 0, 100} : SDL_Color{0, 0, 0, 55});
        if (focused) p.rounded(SDL_Rect{r.x, r.y + r.h - px(2), r.w, px(2)}, px(1), SDL_Color{255, 255, 255, 200});
        int x = r.x + px(10);
        if (glyph) { p.text(glyph, x, r.y + (r.h - p.fontHeight(F_ICON)) / 2, F_ICON, kMuted); x += px(24); }
        const int ty = r.y + (r.h - p.fontHeight(F_BODY)) / 2;
        const int room = r.x + r.w - x - px(10);
        if (value.empty() && !focused) p.text(p.fit(placeholder, F_BODY, room), x, ty, F_BODY, withAlpha(kMuted, 170));
        else {
            // Show the end of long text, where the typing happens.
            std::string shown = value;
            while (!shown.empty() && p.textWidth(shown, F_BODY) > room - px(4)) {
                size_t i = 1;
                while (i < shown.size() && ((unsigned char)shown[i] & 0xC0) == 0x80) ++i;
                shown.erase(0, i);
            }
            p.text(shown, x, ty, F_BODY, kText);
            if (focused && (SDL_GetTicks() / 530) % 2 == 0) p.fill(SDL_Rect{x + p.textWidth(shown, F_BODY) + 1, ty + px(2), std::max(1, px(1)), p.fontHeight(F_BODY) - px(4)}, kText);
        }
        return submitted;
    }

    // ------------------------------------------------------------ the song on screen
    void saveSongSettings() {
        if (!hasSong || !songSettingsDirty) return;
        song.vocalLevel = level; song.semitones = semis; song.tempo = tempo; song.syncOffset = syncOffset;
        lib.save(song);
        songSettingsDirty = false;
    }

    void buildPeaks() {
        const int n = 800;
        peaksInst.assign(n, 0.f);
        peaksVoc.assign(n, 0.f);
        if (!stems || stems->frames() == 0) return;
        const size_t frames = stems->frames(), per = std::max<size_t>(1, frames / n), stride = std::max<size_t>(1, per / 256);
        float top = 1e-4f;
        for (int i = 0; i < n; ++i) {
            const size_t a = (size_t)((double)i / n * frames), b = std::min(a + per, frames);
            float pi = 0, pv = 0;
            for (size_t j = a; j < b; j += stride) {
                pi = std::max({pi, std::abs(stems->instrumental[j * 2]), std::abs(stems->instrumental[j * 2 + 1])});
                pv = std::max({pv, std::abs(stems->vocals[j * 2]), std::abs(stems->vocals[j * 2 + 1])});
            }
            peaksInst[i] = pi; peaksVoc[i] = pv;
            top = std::max(top, pi + pv);
        }
        for (int i = 0; i < n; ++i) { peaksInst[i] /= top; peaksVoc[i] /= top; }
    }

    void showSong(const SongEntry& e, std::shared_ptr<Stems> st, const Lyrics& l, const std::string& note) {
        saveSongSettings();
        song = e;
        stems = std::move(st);
        hasSong = true;
        level = e.vocalLevel; semis = e.semitones; tempo = e.tempo; syncOffset = e.syncOffset;
        player.vocalLevel = level; player.semitones = semis; player.tempo = tempo;
        player.setStems(stems);
        setLyrics(l);
        lyricsNote = note;
        buildPeaks();
        s.lastSong = e.id;
        wallDirty = true;
        lyricScroll = -1e9f;  // jump straight to the first line rather than scroll from wherever the last song was
        uiFrames = 3;
    }

    void setLyrics(const Lyrics& l) {
        lyrics = l;
        items.clear();
        itemsWidth = -1;
        lyricsNote.clear();
    }

    // Lines become items; long instrumental breaks become a row of dots so the screen shows
    // the singer is resting rather than looking stuck.
    void layoutLyrics(int width) {
        if (width == itemsWidth && !items.empty()) return;
        itemsWidth = width;
        items.clear();
        const double songEnd = hasSong ? (double)stems->frames() / kModelRate : 0;
        const auto& L = lyrics.lines;
        for (size_t i = 0; i < L.size(); ++i) {
            LyricItem it;
            it.time = L[i].time;
            it.end = i + 1 < L.size() ? L[i + 1].time : songEnd;
            it.text = L[i].text;
            it.gap = it.text.empty();
            if (it.gap && it.end - it.time < 4.0) continue;
            items.push_back(it);
        }
        if (!items.empty() && items.front().time > 4.0) {
            LyricItem intro;
            intro.time = 0; intro.end = items.front().time; intro.gap = true;
            items.insert(items.begin(), intro);
        }
        const int lineH = p.fontHeight(F_LYRIC), gapBetween = px(22);
        int y = 0;
        for (LyricItem& it : items) {
            it.rows = it.gap ? std::vector<std::string>{} : p.wrap(it.text, F_LYRIC, width);
            it.h = it.gap ? px(34) : (int)it.rows.size() * (int)(lineH * 0.98f);
            it.y = y;
            y += it.h + gapBetween;
        }
    }

    double lyricTime() const { return player.position() - syncOffset; }

    // ------------------------------------------------------------ jobs
    void processFile(const std::string& path) {
        if (procJob && !procJob->finished) { showToast("One song at a time. Wait until this one is finished."); return; }
        procJob = std::make_unique<Job>();
        procJob->sourcePath = path;
        procJob->allowGpu = s.useGpu;
        procJob->allowWhisper = s.useWhisper;
        procTaken = false;
        procJob->start(lib, separator);
        uiFrames = 3;
    }

    void openSong(const std::string& id) {
        if (hasSong && song.id == id) return;
        if (openJob && !openJob->finished) return;
        // A song still being processed opens when it's ready, not from a half-written folder.
        if (procJob && !procJob->finished && procJob->step >= STEP_LYRICS && procJob->entry.id == id) return;
        openJob = std::make_unique<Job>();
        openJob->reopenId = id;
        openJob->start(lib, separator);
        uiFrames = 3;
    }

    void pollJobs() {
        if (openJob && openJob->finished) {
            openJob->join();
            if (openJob->ok) {
                showSong(openJob->entry, openJob->stems, openJob->lyrics, openJob->note);
                if (setTime >= 0) { player.seek(setTime); setTime = -1; }
            } else showToast(openJob->error, 7000);
            openJob.reset();
            uiFrames = 3;
        }
        if (procJob && !procTaken && procJob->step >= STEP_LYRICS && procJob->stems && !procJob->finished) {
            // The stems are saved and the lyrics are still being looked for: start singing now.
            if (!player.playing()) { showSong(procJob->entry, procJob->stems, Lyrics{}, ""); procTaken = true; }
        }
        if (procJob && procJob->finished) {
            procJob->join();
            Job& j = *procJob;
            if (j.ok) {
                // The worker stops writing entry once the stems exist, so it is updated here.
                if (!j.lyrics.source.empty()) j.entry.lyricsSource = j.lyrics.source;
                if (procTaken && hasSong && song.id == j.entry.id) {
                    setLyrics(j.lyrics);
                    lyricsNote = j.note;
                    song.lyricsSource = j.lyrics.source;
                } else if (!player.playing() || !hasSong) {
                    showSong(j.entry, j.stems, j.lyrics, j.note);
                } else {
                    showToast("\xE2\x80\x9C" + j.entry.title + "\xE2\x80\x9D is ready in your library.");
                }
                if (!j.device.empty()) std::fprintf(stderr, "vocal remover ran on %s\n", j.device.c_str());
            } else if (!j.cancel) {
                showToast(j.error, 8000);
            }
            procJob.reset();
            uiFrames = 3;
        }
        if (lyricsTask && lyricsTask->finished) {
            lyricsTask->worker.join();
            LyricsTask& t = *lyricsTask;
            if (hasSong && song.id == t.entry.id && !t.result.lines.empty()) {
                setLyrics(t.result);
                song.lyricsSource = t.result.source;
                Library::saveLyrics(song, lyrics);
                lib.save(song);
                syncOffset = 0;
                songSettingsDirty = true;
                if (modal == MODAL_LYRICS) modal = MODAL_NONE;
            } else if (!t.result.lines.empty()) {
                SongEntry e;
                if (lib.get(t.entry.id, e)) { e.lyricsSource = t.result.source; Library::saveLyrics(e, t.result); lib.save(e); }
            }
            if (t.result.lines.empty()) showToast(t.note, 6000);
            lyricsTask.reset();
            uiFrames = 3;
        }
        if (exportTask && exportTask->finished && exportDone.empty()) {
            exportTask->worker.join();
            exportDone = exportTask->ok ? "ok" : "fail";
            uiFrames = 3;
        }
    }

    // A job counts until pollJobs has collected its result, not just until its thread is done;
    // otherwise there is a frame where everything looks idle but the result isn't on screen yet.
    bool busy() const {
        return procJob || openJob || lyricsTask || (exportTask && exportDone.empty());
    }

    // ------------------------------------------------------------ backdrop
    // Behind everything: the cover art, enormous, blurred and dimmed, so each song tints the
    // whole window the way it does in a music app. Without a cover it is a quiet dark gradient.
    void buildWall() {
        scene.assign((size_t)W * H, 0xFF000000u);
        std::vector<float> rgb;
        const int n = 40;
        const bool cover = hasSong && song.hasCover && loadBlurredPixels(song.dir + "\\cover.bmp", n, rgb);
        parallelRange(H, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                for (int x = 0; x < W; ++x) {
                    const float u = (x + 0.5f) / W, v = (y + 0.5f) / H;
                    // Darker toward the bottom and the edges, where the panels sit.
                    const float dx = u - 0.55f, dy = v - 0.4f;
                    const float vignette = std::clamp(1.f - (dx * dx + dy * dy) * 1.1f, 0.25f, 1.f);
                    float c[3];
                    if (cover) {
                        const float fx = std::clamp(u * n - 0.5f, 0.f, n - 1.001f), fy = std::clamp(v * n - 0.5f, 0.f, n - 1.001f);
                        const int ix = (int)fx, iy = (int)fy;
                        const float tx = fx - ix, ty = fy - iy;
                        for (int k = 0; k < 3; ++k) {
                            auto at = [&](int xx, int yy) { return rgb[((size_t)yy * n + xx) * 3 + k]; };
                            const float val = (at(ix, iy) * (1 - tx) + at(ix + 1, iy) * tx) * (1 - ty) + (at(ix, iy + 1) * (1 - tx) + at(ix + 1, iy + 1) * tx) * ty;
                            c[k] = 14.f + val * 0.42f * vignette;
                        }
                    } else {
                        const float lift = 22.f * vignette;
                        c[0] = 16.f + lift; c[1] = 16.f + lift; c[2] = 18.f + lift;
                    }
                    // A little noise breaks up banding in the smooth gradient.
                    // A full integer hash: a linear one lines up into visible stripes.
                    unsigned h = (unsigned)x * 0x8da6b343u ^ (unsigned)y * 0xd8163841u;
                    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
                    const float noise = (h >> 24) / 255.f * 2.4f - 1.2f;
                    Uint32 out = 0xFF000000u;
                    for (int k = 0; k < 3; ++k) out |= (Uint32)std::clamp(c[k] + noise, 0.f, 255.f) << (16 - 8 * k);
                    scene[(size_t)y * W + x] = out;
                }
            }
        });
        if (sceneTex) SDL_UpdateTexture(sceneTex, nullptr, scene.data(), W * 4);
        ++sceneVersion;
        wallDirty = false;
    }

    void layout() {
        SDL_GetRendererOutputSize(ren, &W, &H);
        const int m = px(12), sideW = px(300), barH = px(46), transH = px(118);
        sidebar = SDL_Rect{m, m, sideW, H - 2 * m};
        const int x0 = sidebar.x + sidebar.w + m, w = W - x0 - m;
        topBar = SDL_Rect{x0, m, w, barH};
        transport = SDL_Rect{x0, H - m - transH, w, transH};
        stage = SDL_Rect{x0, topBar.y + barH, w, transport.y - topBar.y - barH};
        if (!sceneTex || scene.size() != (size_t)W * H) {
            if (sceneTex) SDL_DestroyTexture(sceneTex);
            sceneTex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);
        }
        glassSide.setGeometry(sidebar, px(20), true, scale);
        glassTop.setGeometry(topBar, topBar.h / 2, false, scale);
        glassTransport.setGeometry(transport, px(22), false, scale);
        itemsWidth = -1;
        wallDirty = true;
        layoutDirty = false;
    }

    void panel(Glass& g, bool surface) {
        p.softShadow(g.r, g.radius, px(surface ? 22 : 10), surface ? 150 : 90);
        if (glassOn) {
            g.refresh(ren, scene, W, H, scale, sceneVersion);
            if (g.tex) { SDL_RenderCopy(ren, g.tex, nullptr, &g.r); return; }
        }
        p.rounded(g.r, g.radius, kBar);
    }

    SDL_Texture* thumbFor(const SongEntry& e, int size) {
        auto it = thumbs.find(e.id);
        if (it != thumbs.end()) return it->second;
        SDL_Texture* t = e.hasCover ? loadRoundedPicture(ren, e.dir + "\\cover.bmp", size, px(8)) : nullptr;
        thumbs[e.id] = t;
        return t;
    }

    void dropThumb(const std::string& id) {
        auto it = thumbs.find(id);
        if (it != thumbs.end()) { if (it->second) SDL_DestroyTexture(it->second); thumbs.erase(it); }
        if (bigCoverId == id && bigCover) { SDL_DestroyTexture(bigCover); bigCover = nullptr; bigCoverId.clear(); }
    }

    // ------------------------------------------------------------ sidebar
    void drawSidebar() {
        panel(glassSide, true);
        const int pad = px(16);
        const int x = sidebar.x + pad, w = sidebar.w - 2 * pad;
        int y = sidebar.y + pad;
        p.text("Undertone", x, y + px(2), F_TITLE, kText);
        if (iconButton("settings", SDL_Rect{x + w - px(32), y - px(2), px(32), px(32)}, kIconSettings)) { modal = MODAL_SETTINGS; }
        y += px(40);
        SDL_Rect add{x, y, w, px(38)};
        if (button("add", add, "Add a song", true)) askOpen();
        y += add.h + px(10);
        textField("search", SDL_Rect{x, y, w, px(34)}, search, "Search your library", kIconSearch);
        y += px(34) + px(12);

        // A song being processed sits at the top of the list with its progress.
        if (procJob && !procJob->finished) {
            const int rowH = px(58);
            SDL_Rect row{x - px(6), y, w + px(12), rowH};
            p.rounded(row, px(12), SDL_Color{255, 255, 255, 22});
            const int ring = px(18);
            p.ring(x + px(20), y + rowH / 2, ring, px(3), 1.f, SDL_Color{255, 255, 255, 40});
            p.ring(x + px(20), y + rowH / 2, ring, px(3), procJob->step == STEP_TRANSCRIBE || procJob->step == STEP_SEPARATE || procJob->step == STEP_READ ? procJob->progress.load() : 1.f, kAccent);
            const int tx = x + px(50);
            p.text(p.fit(fileNameOf(stemOf(procJob->sourcePath)), F_BODY, w - px(84)), tx, y + px(10), F_BODY, kText);
            p.text(stepName(procJob->step), tx, y + px(31), F_SMALL, kMuted);
            if (iconButton("cancelproc", SDL_Rect{x + w - px(30), y + (rowH - px(30)) / 2, px(30), px(30)}, "\xEE\x9C\x91")) procJob->cancel = true;
            y += rowH + px(6);
        }

        std::vector<SongEntry> songs = lib.songs();
        const std::string q = lowerAscii(search);
        if (!q.empty())
            songs.erase(std::remove_if(songs.begin(), songs.end(), [&](const SongEntry& e) {
                return lowerAscii(e.title).find(q) == std::string::npos && lowerAscii(e.artist).find(q) == std::string::npos;
            }), songs.end());

        const int footerH = px(30);
        SDL_Rect area{sidebar.x, y, sidebar.w, sidebar.y + sidebar.h - footerH - y};
        if (modal == MODAL_NONE && wheel != 0 && inside(area)) listScroll -= wheel * px(64);
        const int rowH = px(56);
        listContentH = (int)songs.size() * rowH;
        listScroll = std::clamp(listScroll, 0.f, (float)std::max(0, listContentH - area.h));
        if (smoothTo(listScrollNow, listScroll, dtMs, motionOn ? 70.f : 0.f)) uiFrames = std::max(uiFrames, 2);

        SDL_RenderSetClipRect(ren, &area);
        int ry = y - (int)listScrollNow;
        for (const SongEntry& e : songs) {
            SDL_Rect row{x - px(6), ry, w + px(12), rowH - px(4)};
            ry += rowH;
            if (row.y + row.h < area.y || row.y > area.y + area.h) continue;
            const bool rowHot = hot("song" + e.id, row) && inside(area);
            const bool current = hasSong && e.id == song.id;
            if (current) p.rounded(row, px(12), SDL_Color{255, 255, 255, 38});
            else if (rowHot) p.rounded(row, px(12), SDL_Color{255, 255, 255, 18});
            const int ts = px(40);
            SDL_Rect thumb{x, row.y + (row.h - ts) / 2, ts, ts};
            if (SDL_Texture* t = thumbFor(e, ts)) SDL_RenderCopy(ren, t, nullptr, &thumb);
            else { p.rounded(thumb, px(8), SDL_Color{255, 255, 255, 30}); p.textCentred(kIconMusic, thumb, F_ICON, kMuted); }
            const int tx = thumb.x + ts + px(12), room = x + w - tx - (rowHot ? px(34) : px(4));
            p.text(p.fit(e.title, F_BODY, room), tx, row.y + px(8), F_BODY, current ? kAccent : kText);
            const std::string sub = (e.artist.empty() ? std::string("Unknown artist") : e.artist) + "  \xC2\xB7  " + formatTime(e.duration);
            p.text(p.fit(sub, F_SMALL, room), tx, row.y + px(28), F_SMALL, kMuted);
            SDL_Rect del{x + w - px(30), row.y + (row.h - px(28)) / 2, px(28), px(28)};
            if (rowHot && iconButton("del" + e.id, del, kIconDelete, true, kMuted)) {
                removeId = e.id; removeTitle = e.title; modal = MODAL_REMOVE;
            } else if (inside(area) && !inside(del) && clicked("song" + e.id, row)) {
                openSong(e.id);
            }
        }
        SDL_RenderSetClipRect(ren, nullptr);
        if (songs.empty() && !(procJob && !procJob->finished))
            p.text(search.empty() ? "Songs you add appear here." : "No songs match.", x, y + px(6), F_SMALL, kMuted);
        if (listContentH > area.h) {
            const int barH = std::max(px(30), area.h * area.h / std::max(listContentH, 1));
            const int barY = area.y + (int)((area.h - barH) * (listScrollNow / std::max(1, listContentH - area.h)));
            p.rounded(SDL_Rect{sidebar.x + sidebar.w - px(7), barY, px(3), barH}, px(2), SDL_Color{255, 255, 255, 60});
        }
        // Where the work happens, so a slow separation is explained before anyone wonders.
        const std::string dev = separator->loaded() ? "Vocal remover on " + separator->device() : s.useGpu ? "Vocal remover uses the graphics card" : "Vocal remover uses the processor";
        p.text(p.fit(dev, F_SMALL, w), x, sidebar.y + sidebar.h - footerH, F_SMALL, withAlpha(kMuted, 170));
    }

    // ------------------------------------------------------------ top bar
    void drawTopBar() {
        panel(glassTop, false);
        const int midY = topBar.y + topBar.h / 2;
        int right = topBar.x + topBar.w - px(8);
        if (hasSong) {
            SDL_Rect lyr{right - px(84), topBar.y + px(7), px(84), topBar.h - px(14)};
            if (button("lyricsbtn", lyr, "Lyrics", false)) openLyricsModal();
            right = lyr.x - px(10);
            if (!lyrics.lines.empty()) {
                // Nudging: earlier and later in quarter seconds, for lyrics that are a touch off.
                SDL_Rect later{right - px(30), midY - px(15), px(30), px(30)};
                SDL_Rect earlier{later.x - px(78), later.y, px(30), px(30)};
                if (iconButton("later", later, "\xEE\x9C\x90")) { syncOffset += 0.25f; songSettingsDirty = true; }
                if (iconButton("earlier", earlier, "\xEE\x9C\xB8")) { syncOffset -= 0.25f; songSettingsDirty = true; }
                char b[32];
                if (std::abs(syncOffset) < 0.01f) std::snprintf(b, sizeof b, "In sync");
                else std::snprintf(b, sizeof b, "%+.2f s", syncOffset);
                p.textCentred(b, SDL_Rect{earlier.x + earlier.w, earlier.y, later.x - earlier.x - earlier.w, earlier.h}, F_SMALL, std::abs(syncOffset) < 0.01f ? kMuted : kText);
                right = earlier.x - px(12);
                const char* src = lyrics.source == "lrclib" ? "Synced lyrics from LRCLIB" : lyrics.source == "whisper" ? "Auto-transcribed" : "Lyrics from your .lrc file";
                const int sw = p.textWidth(src, F_SMALL) + px(20);
                SDL_Rect chip{right - sw, midY - px(12), sw, px(24)};
                p.rounded(chip, chip.h / 2, SDL_Color{255, 255, 255, lyrics.source == "whisper" ? (Uint8)18 : (Uint8)28});
                p.textCentred(src, chip, F_SMALL, lyrics.source == "whisper" ? kMuted : kText);
                right = chip.x - px(12);
            }
            const int left = topBar.x + px(20);
            const std::string title = song.title, artist = song.artist;
            const int room = right - left;
            const int tw = std::min(p.textWidth(title, F_TITLE), room);
            p.text(p.fit(title, F_TITLE, room), left, midY - p.fontHeight(F_TITLE) / 2, F_TITLE, kText);
            if (!artist.empty() && room - tw > px(60))
                p.text(p.fit(artist, F_BODY, room - tw - px(14)), left + tw + px(14), midY - p.fontHeight(F_BODY) / 2, F_BODY, kMuted);
        } else {
            p.text("Nothing playing", topBar.x + px(20), midY - p.fontHeight(F_BODY) / 2, F_BODY, kMuted);
        }
    }

    // ------------------------------------------------------------ stage
    void drawEmptyStage() {
        const int cx = stage.x + stage.w / 2, cy = stage.y + stage.h / 2;
        const bool dropHot = inside(stage);
        SDL_Rect box{cx - px(250), cy - px(120), px(500), px(230)};
        p.rounded(box, px(24), SDL_Color{255, 255, 255, (Uint8)(dropHot ? 14 : 8)});
        // Dashed outline: a drop target, not a button.
        const int dash = px(10), gapPx = px(7), t = std::max(1, px(1.5f));
        const SDL_Color dc{255, 255, 255, 60};
        for (int x = box.x + px(24); x < box.x + box.w - px(24); x += dash + gapPx) {
            p.fill(SDL_Rect{x, box.y, std::min(dash, box.x + box.w - px(24) - x), t}, dc);
            p.fill(SDL_Rect{x, box.y + box.h - t, std::min(dash, box.x + box.w - px(24) - x), t}, dc);
        }
        for (int y = box.y + px(24); y < box.y + box.h - px(24); y += dash + gapPx) {
            p.fill(SDL_Rect{box.x, y, t, std::min(dash, box.y + box.h - px(24) - y)}, dc);
            p.fill(SDL_Rect{box.x + box.w - t, y, t, std::min(dash, box.y + box.h - px(24) - y)}, dc);
        }
        p.textCentred(kIconMusic, SDL_Rect{box.x, box.y + px(26), box.w, px(40)}, F_ICON_BIG, kText);
        p.textCentred("Drop a song here to sing along", SDL_Rect{box.x, box.y + px(78), box.w, px(32)}, F_HERO, kText);
        p.textCentred("The vocals are removed and the lyrics follow the music.", SDL_Rect{box.x, box.y + px(112), box.w, px(22)}, F_BODY, kMuted);
        SDL_Rect btn{cx - px(70), box.y + px(150), px(140), px(38)};
        if (button("emptyadd", btn, "Choose a file", true)) askOpen();
        p.textCentred("MP3, FLAC, WAV, M4A, OGG and most other formats", SDL_Rect{box.x, box.y + box.h + px(14), box.w, px(20)}, F_SMALL, withAlpha(kMuted, 170));
    }

    void drawProcessingStage() {
        const Job& j = *procJob;
        const int cx = stage.x + stage.w / 2, cy = stage.y + stage.h / 2 - px(20);
        const int r = px(64);
        p.ring(cx, cy, r, px(6), 1.f, SDL_Color{255, 255, 255, 34});
        const int step = j.step;
        const bool measurable = step == STEP_READ || step == STEP_SEPARATE || step == STEP_SAVE || step == STEP_TRANSCRIBE;
        if (measurable) p.ring(cx, cy, r, px(6), j.progress.load(), kAccent);
        else {
            // A short arc that turns, for the steps with no measurable progress.
            const float spin = (SDL_GetTicks() % 1400) / 1400.f;
            std::vector<SDL_Vertex> v;
            std::vector<int> idx;
            const int segs = 28;
            for (int i = 0; i <= segs; ++i) {
                const float a = 6.2831853f * (spin + 0.22f * i / segs) - 1.5707963f;
                v.push_back(SDL_Vertex{{cx + std::cos(a) * r, cy + std::sin(a) * r}, kAccent, {0, 0}});
                v.push_back(SDL_Vertex{{cx + std::cos(a) * (r - px(6)), cy + std::sin(a) * (r - px(6))}, kAccent, {0, 0}});
                if (i) { const int b = (i - 1) * 2; idx.insert(idx.end(), {b, b + 1, b + 2, b + 1, b + 3, b + 2}); }
            }
            SDL_RenderGeometry(ren, nullptr, v.data(), (int)v.size(), idx.data(), (int)idx.size());
        }
        char pct[16];
        std::snprintf(pct, sizeof pct, "%d%%", (int)(j.progress.load() * 100));
        p.textCentred(measurable ? pct : "", SDL_Rect{cx - r, cy - r, 2 * r, 2 * r}, F_HERO, kText);
        p.textCentred(stepName(step), SDL_Rect{stage.x, cy + r + px(22), stage.w, px(30)}, F_HERO, kText);
        p.textCentred(p.fit(fileNameOf(j.sourcePath), F_BODY, stage.w - px(80)), SDL_Rect{stage.x, cy + r + px(56), stage.w, px(22)}, F_BODY, kMuted);
        static const char* hints[] = {"", "The first song takes a few seconds longer while the model starts.",
                                      "This is the machine-learning part. It runs once per song.", "", "", "No lyrics online, so Undertone is writing them down from the vocals.", ""};
        if (step < 7 && hints[step][0]) p.textCentred(hints[step], SDL_Rect{stage.x, cy + r + px(82), stage.w, px(22)}, F_SMALL, withAlpha(kMuted, 180));
        SDL_Rect cancel{cx - px(55), cy + r + px(118), px(110), px(34)};
        if (button("cancelbig", cancel, procJob->cancel ? "Stopping" : "Cancel", false, !procJob->cancel)) procJob->cancel = true;
    }

    void drawLyrics() {
        const int padX = px(56);
        const int width = std::min(stage.w - 2 * padX, px(820));
        layoutLyrics(width);
        const int lx = stage.x + padX;
        SDL_RenderSetClipRect(ren, &stage);

        if (items.empty()) {
            const int cy = stage.y + stage.h / 2;
            const bool listening = (procJob && procTaken && !procJob->finished) || (lyricsTask && !lyricsTask->finished);
            if (listening) {
                const int step = procJob && procTaken && !procJob->finished ? procJob->step.load() : lyricsTask->step.load();
                const float prog = procJob && procTaken && !procJob->finished ? procJob->progress.load() : lyricsTask->progress.load();
                p.ring(stage.x + stage.w / 2, cy - px(44), px(22), px(4), 1.f, SDL_Color{255, 255, 255, 34});
                p.ring(stage.x + stage.w / 2, cy - px(44), px(22), px(4), step == STEP_TRANSCRIBE ? prog : (SDL_GetTicks() % 1200) / 1200.f, kAccent);
                p.textCentred(stepName(step), SDL_Rect{stage.x, cy - px(6), stage.w, px(32)}, F_HERO, kText);
                p.textCentred(step == STEP_TRANSCRIBE ? "None were found online, so they're being transcribed from the vocals. You can sing already."
                                                      : "The music is ready. You can start singing already.",
                              SDL_Rect{stage.x, cy + px(28), stage.w, px(22)}, F_BODY, kMuted);
            } else {
                p.textCentred("No lyrics for this song", SDL_Rect{stage.x, cy - px(40), stage.w, px(32)}, F_HERO, kText);
                p.textCentred(lyricsNote.empty() ? "Search with a different title, or have them transcribed from the vocals." : lyricsNote,
                              SDL_Rect{stage.x, cy - px(4), stage.w, px(22)}, F_BODY, kMuted);
                SDL_Rect b{stage.x + stage.w / 2 - px(75), cy + px(32), px(150), px(36)};
                if (button("findlyrics", b, "Find lyrics", true)) openLyricsModal();
            }
            SDL_RenderSetClipRect(ren, nullptr);
            return;
        }

        const double t = lyricTime();
        int cur = -1;
        for (int i = 0; i < (int)items.size(); ++i) if (items[i].time <= t) cur = i;
        const int focus = std::max(cur, 0);
        const float anchor = stage.h * 0.36f;
        float target = items[focus].y + items[focus].h / 2.f - anchor;
        const Uint32 now = SDL_GetTicks();
        if (modal == MODAL_NONE && wheel != 0 && inside(stage)) {
            if (now >= manualUntil) manualScroll = lyricScroll;
            manualScroll -= wheel * px(80);
            manualUntil = now + 3500;
        }
        if (now < manualUntil) { target = manualScroll; uiFrames = std::max(uiFrames, 2); }
        if (lyricScroll < -1e8f) lyricScroll = target;
        if (smoothTo(lyricScroll, target, dtMs, motionOn ? 160.f : 0.f)) uiFrames = std::max(uiFrames, 2);

        const int lineH = (int)(p.fontHeight(F_LYRIC) * 0.98f);
        const int fade = px(80);
        for (int i = 0; i < (int)items.size(); ++i) {
            LyricItem& it = items[i];
            const int y = stage.y + it.y - (int)lyricScroll;
            if (y + it.h < stage.y || y > stage.y + stage.h) { it.emphasis = i == cur ? 1.f : 0.f; continue; }
            if (smoothTo(it.emphasis, i == cur ? 1.f : 0.f, dtMs, motionOn ? 110.f : 0.f, 0.01f)) uiFrames = std::max(uiFrames, 2);
            // Lines fade out toward the top and bottom of the stage instead of being cut off.
            const float edge = std::min(std::clamp((float)(y + it.h - stage.y) / fade, 0.f, 1.f), std::clamp((float)(stage.y + stage.h - y) / fade, 0.f, 1.f));
            SDL_Rect hit{lx - px(12), y - px(6), width + px(24), it.h + px(12)};
            const bool h = hot("line" + std::to_string(i), hit) && inside(stage);
            if (h) p.rounded(hit, px(12), SDL_Color{255, 255, 255, 14});
            if (clicked("line" + std::to_string(i), hit)) { player.seek(it.time + syncOffset); manualUntil = 0; }
            const float e = it.emphasis;
            if (it.gap) {
                // Three dots that light up one by one across the break.
                const float prog = i == cur ? (float)std::clamp((t - it.time) / std::max(0.1, it.end - it.time), 0.0, 1.0) : 0.f;
                for (int d = 0; d < 3; ++d) {
                    const float lit = std::clamp(prog * 3 - d, 0.f, 1.f);
                    const int size = px(10 + 3 * e);
                    const int a = (int)((70 + 60 * e + 120 * lit * e) * edge);
                    p.rounded(SDL_Rect{lx + d * px(22), y + (it.h - size) / 2, size, size}, size / 2, SDL_Color{255, 255, 255, (Uint8)a});
                }
                continue;
            }
            const int baseA = (int)((h ? 120 : 92) * edge + 60 * e * edge);
            // The sung part of the current line wipes from dim to white, karaoke style. Line
            // timestamps only mark when a line starts, so how long it takes to sing is estimated
            // from its length and capped by the next line's start.
            float sung = 0;
            if (i == cur) {
                const double span = std::min(std::max(1.2, 0.075 * it.text.size()), std::max(0.3, it.end - it.time - 0.15));
                sung = (float)std::clamp((t - it.time) / span, 0.0, 1.0);
            }
            int total = 0;
            for (const std::string& r : it.rows) total += p.textWidth(r, F_LYRIC);
            float remaining = sung * total;
            for (size_t r = 0; r < it.rows.size(); ++r) {
                const int ry = y + (int)r * lineH;
                p.text(it.rows[r], lx, ry, F_LYRIC, SDL_Color{255, 255, 255, (Uint8)baseA});
                const int rw = p.textWidth(it.rows[r], F_LYRIC);
                const int filled = (int)std::clamp(remaining, 0.f, (float)rw);
                remaining -= rw;
                if (filled > 0 && e > 0.01f) {
                    SDL_Rect c{lx, ry, filled, lineH + px(8)};
                    SDL_Rect both;
                    if (SDL_IntersectRect(&c, &stage, &both)) {
                        SDL_RenderSetClipRect(ren, &both);
                        p.text(it.rows[r], lx, ry, F_LYRIC, SDL_Color{255, 255, 255, (Uint8)(255 * e * edge)});
                        SDL_RenderSetClipRect(ren, &stage);
                    }
                }
            }
        }
        SDL_RenderSetClipRect(ren, nullptr);
        if (now < manualUntil) {
            SDL_Rect back{stage.x + stage.w / 2 - px(80), stage.y + stage.h - px(46), px(160), px(32)};
            p.softShadow(back, back.h / 2, px(8), 90);
            if (button("backtolive", back, "Back to the music", true)) manualUntil = 0;
        }
    }

    // ------------------------------------------------------------ transport
    void drawTransport() {
        panel(glassTransport, false);
        const bool on = hasSong;
        const int pad = px(18);
        const double dur = player.duration(), now = on ? player.position() : 0;

        // Row one: the timeline, drawn as the song's loudness so the choruses are easy to find.
        const int r1 = transport.y + px(12), r1h = px(34);
        const std::string tNow = formatTime(now), tDur = formatTime(dur);
        const int tw = std::max(p.textWidth("00:00", F_SMALL), p.textWidth(tDur, F_SMALL)) + px(4);
        p.text(tNow, transport.x + pad, r1 + (r1h - p.fontHeight(F_SMALL)) / 2, F_SMALL, on ? kText : kMuted);
        p.text(tDur, transport.x + transport.w - pad - p.textWidth(tDur, F_SMALL), r1 + (r1h - p.fontHeight(F_SMALL)) / 2, F_SMALL, kMuted);
        SDL_Rect sc{transport.x + pad + tw + px(8), r1, transport.w - 2 * pad - 2 * tw - px(16), r1h};
        const unsigned scrubId = hashId("scrub");
        if (on && modal == MODAL_NONE && mousePressed && inside(sc)) activeId = scrubId;
        if (activeId == scrubId) {
            player.seek(std::clamp((double)(mx - sc.x) / std::max(sc.w, 1), 0.0, 1.0) * dur);
            manualUntil = 0;
            if (mouseReleased) activeId = 0;
        }
        const int bars = std::max(10, sc.w / px(3));
        const float played = dur > 0 ? (float)(now / dur) : 0;
        for (int i = 0; i < bars; ++i) {
            float v = 0, vv = 0;
            if (!peaksInst.empty() && on) {
                const int a = i * (int)peaksInst.size() / bars, b = std::max(a + 1, (i + 1) * (int)peaksInst.size() / bars);
                for (int k = a; k < b; ++k) { v = std::max(v, peaksInst[k]); vv = std::max(vv, peaksVoc[k]); }
            }
            // The vocal part of each bar shows how much singing there is, at the slider's level.
            const int bh = std::max(px(2), (int)(v * sc.h));
            const int vh = (int)(vv * sc.h * (0.25f + 0.75f * level));
            const bool past = (float)i / bars < played;
            const SDL_Rect r{sc.x + i * sc.w / bars, sc.y + (sc.h - bh) / 2, std::max(1, px(2)), bh};
            p.fill(r, past ? SDL_Color{255, 255, 255, 235} : SDL_Color{255, 255, 255, 70});
            if (vh > 0) {
                const int top = std::max(sc.y, r.y - vh / 2);
                p.fill(SDL_Rect{r.x, top, r.w, r.y - top}, past ? SDL_Color{255, 255, 255, 120} : SDL_Color{255, 255, 255, 34});
                p.fill(SDL_Rect{r.x, r.y + r.h, r.w, std::min(vh / 2, sc.y + sc.h - r.y - r.h)}, past ? SDL_Color{255, 255, 255, 120} : SDL_Color{255, 255, 255, 34});
            }
        }
        if (on) p.rounded(SDL_Rect{sc.x + (int)(played * sc.w) - px(1), sc.y - px(3), px(3), sc.h + px(6)}, px(1), SDL_Color{255, 255, 255, 255});

        // Row two: play, then the three things a singer adjusts, then export.
        const int r2 = transport.y + px(58), r2h = transport.h - px(58) - px(12);
        const int size = px(46);
        SDL_Rect play{transport.x + pad, r2 + (r2h - size) / 2, size, size};
        const bool ph = on && hot("play", play);
        p.softShadow(play, size / 2, px(6), 90);
        p.rounded(play, size / 2, !on ? SDL_Color{255, 255, 255, 60} : ph ? SDL_Color{236, 236, 236, 255} : kAccent);
        p.textCentred(player.playing() ? kIconPause : kIconPlay, play, F_ICON_BIG, kOnAccent);
        if (clicked("play", play, on)) togglePlay();

        SDL_Rect exp{transport.x + transport.w - pad - px(108), r2 + (r2h - px(36)) / 2, px(108), px(36)};
        if (button("export", exp, "Export", false, on && !exportTask)) { modal = MODAL_EXPORT; exportDone.clear(); exGuide = level > 0.01f; }

        int x = play.x + play.w + px(24);
        const int ctrlRoom = exp.x - px(24) - x;
        const int gapC = px(26);
        const int keyW = px(150), vocW = std::max(px(120), (ctrlRoom - keyW - 2 * gapC) * 55 / 100), tempoW = std::max(px(110), ctrlRoom - keyW - vocW - 2 * gapC);
        const int sy = r2 + (r2h - px(40)) / 2;

        float n = level;
        char b[48];
        if (level < 0.005f) std::snprintf(b, sizeof b, "Off");
        else std::snprintf(b, sizeof b, "%d%%", (int)std::lround(level * 100));
        if (slider("vocals", SDL_Rect{x, sy, vocW, px(40)}, "Guide vocal", b, n, on)) { level = n < 0.02f ? 0.f : n; player.vocalLevel = level; songSettingsDirty = true; }
        x += vocW + gapC;

        // Key: half steps, the way singers think about it.
        p.text("Key", x, sy, F_SMALL, withAlpha(kMuted, on ? 255 : 90));
        SDL_Rect minus{x, sy + p.fontHeight(F_SMALL) + px(1), px(30), px(26)};
        SDL_Rect plus{x + keyW - px(30), minus.y, px(30), px(26)};
        if (iconButton("keydown", minus, "\xEE\x9C\xB8", on && semis > -6)) { semis -= 1; player.semitones = semis; songSettingsDirty = true; }
        if (iconButton("keyup", plus, "\xEE\x9C\x90", on && semis < 6)) { semis += 1; player.semitones = semis; songSettingsDirty = true; }
        if (std::abs(semis) < 0.5f) std::snprintf(b, sizeof b, "Original");
        else std::snprintf(b, sizeof b, "%+d", (int)std::lround(semis));
        SDL_Rect keyVal{minus.x + minus.w, minus.y, plus.x - minus.x - minus.w, minus.h};
        p.textCentred(b, keyVal, F_BODY, withAlpha(std::abs(semis) < 0.5f ? kMuted : kText, on ? 255 : 90));
        if (std::abs(semis) >= 0.5f && clicked("keyreset", keyVal, on)) { semis = 0; player.semitones = 0; songSettingsDirty = true; }
        x += keyW + gapC;

        // Tempo snaps to 5% steps, 50% to 125%.
        float tn = (tempo - 0.5f) / 0.75f;
        std::snprintf(b, sizeof b, "%d%%", (int)std::lround(tempo * 100));
        if (slider("tempo", SDL_Rect{x, sy, tempoW, px(40)}, "Tempo", b, tn, on)) {
            tempo = std::round((0.5f + tn * 0.75f) * 20.f) / 20.f;
            player.tempo = tempo;
            songSettingsDirty = true;
        }
    }

    void togglePlay() {
        if (!hasSong) return;
        if (player.playing()) player.pause();
        else player.play();
        uiFrames = 3;
    }

    // ------------------------------------------------------------ dialogs
    void openLyricsModal() {
        findTitle = song.title;
        findArtist = song.artist;
        modal = MODAL_LYRICS;
    }

    SDL_Rect modalFrame(int w, int h) {
        p.fill(SDL_Rect{0, 0, W, H}, SDL_Color{0, 0, 0, 120});
        SDL_Rect r{(W - w) / 2, (H - h) / 2, w, h};
        glassModal.setGeometry(r, px(22), true, scale);
        panel(glassModal, true);
        return r;
    }

    void drawModal() {
        if (modal == MODAL_NONE) return;
        if (keyEscape && focusId == 0 && !(exportTask && !exportTask->finished)) { modal = MODAL_NONE; return; }
        const int pad = px(24);
        if (modal == MODAL_REMOVE) {
            SDL_Rect r = modalFrame(px(440), px(200));
            const int x = r.x + pad, w = r.w - 2 * pad;
            p.text("Remove from your library?", x, r.y + pad, F_TITLE, kText);
            int y = r.y + pad + px(34);
            for (const std::string& line : p.wrap("\xE2\x80\x9C" + removeTitle + "\xE2\x80\x9D: the stripped audio and lyrics Undertone saved are deleted. Your original music file is not touched.", F_BODY, w)) {
                p.text(line, x, y, F_BODY, kMuted);
                y += p.fontHeight(F_BODY);
            }
            SDL_Rect del{r.x + r.w - pad - px(110), r.y + r.h - pad - px(36), px(110), px(36)};
            SDL_Rect cancel{del.x - px(120), del.y, px(110), px(36)};
            if (button("modalcancel", cancel, "Keep it", false)) modal = MODAL_NONE;
            if (button("modalremove", del, "Remove", true)) {
                if (hasSong && song.id == removeId) {
                    player.setStems(nullptr);
                    hasSong = false; stems.reset(); setLyrics({}); wallDirty = true;
                }
                dropThumb(removeId);
                lib.remove(removeId);
                modal = MODAL_NONE;
            }
            return;
        }
        if (modal == MODAL_SETTINGS) {
            SDL_Rect r = modalFrame(px(480), px(318));
            const int x = r.x + pad, w = r.w - 2 * pad;
            p.text("Settings", x, r.y + pad, F_TITLE, kText);
            int y = r.y + pad + px(44);
            bool gpu = s.useGpu, wh = s.useWhisper;
            if (toggle("modalgpu", SDL_Rect{x, y, w, px(30)}, "Use the graphics card to remove vocals", gpu)) {
                s.useGpu = gpu;
                // The model is reloaded on the next song, on the newly chosen device.
                if (!(procJob && !procJob->finished)) separator = std::make_shared<Separator>();
            }
            y += px(32);
            p.text("Much faster. Turn off if separation fails on your PC.", x, y, F_SMALL, kMuted);
            y += px(34);
            if (toggle("modalwhisper", SDL_Rect{x, y, w, px(30)}, "Transcribe lyrics when none are found", wh)) s.useWhisper = wh;
            y += px(32);
            p.text("Runs on your PC. Slower, and some words will be misheard.", x, y, F_SMALL, kMuted);
            y += px(36);
            p.text(p.fit("Library: " + lib.root(), F_SMALL, w), x, y, F_SMALL, kMuted);
            SDL_Rect close{r.x + r.w - pad - px(100), r.y + r.h - pad - px(36), px(100), px(36)};
            SDL_Rect folder{x, close.y, px(170), px(36)};
            if (button("modalfolder", folder, "Open library folder", false)) ShellExecuteW(nullptr, L"open", widen(lib.root()).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if (button("modalclose", close, "Done", true)) { modal = MODAL_NONE; saveSettings(s); }
            return;
        }
        if (modal == MODAL_LYRICS) {
            SDL_Rect r = modalFrame(px(500), px(338));
            const int x = r.x + pad, w = r.w - 2 * pad;
            p.text("Find lyrics", x, r.y + pad, F_TITLE, kText);
            int y = r.y + pad + px(40);
            const bool runningTask = lyricsTask && !lyricsTask->finished;
            p.text("Title", x, y, F_SMALL, kMuted);
            y += px(18);
            bool go = textField("modaltitle", SDL_Rect{x, y, w, px(34)}, findTitle, "Song title");
            y += px(44);
            p.text("Artist", x, y, F_SMALL, kMuted);
            y += px(18);
            go |= textField("modalartist", SDL_Rect{x, y, w, px(34)}, findArtist, "Artist");
            y += px(48);
            if (runningTask) {
                p.ring(x + px(10), y + px(10), px(10), px(3), lyricsTask->transcribe ? lyricsTask->progress.load() : (SDL_GetTicks() % 1200) / 1200.f, kAccent);
                char b[96];
                if (lyricsTask->transcribe) std::snprintf(b, sizeof b, "Listening to the vocals, %d%%", (int)(lyricsTask->progress * 100));
                else std::snprintf(b, sizeof b, "Searching LRCLIB");
                p.text(b, x + px(30), y, F_BODY, kText);
            } else {
                p.text(p.fit("Search looks up synced lyrics online. Transcribe listens to the vocals on this PC.", F_SMALL, w), x, y, F_SMALL, kMuted);
            }
            const int by = r.y + r.h - pad - px(36);
            SDL_Rect searchBtn{r.x + r.w - pad - px(110), by, px(110), px(36)};
            SDL_Rect trans{searchBtn.x - px(130), by, px(120), px(36)};
            SDL_Rect close{x, by, px(90), px(36)};
            if (button("modalclose2", close, runningTask ? "Stop" : "Close", false)) {
                if (runningTask) lyricsTask->cancel = true;
                else modal = MODAL_NONE;
            }
            const bool canTranscribe = !findModel("ggml-small.bin").empty();
            if (button("modaltranscribe", trans, "Transcribe", false, !runningTask && canTranscribe)) startLyricsTask(true);
            if ((button("modalsearch", searchBtn, "Search", true, !runningTask && !findTitle.empty()) || (go && !runningTask && !findTitle.empty()))) startLyricsTask(false);
            return;
        }
        if (modal == MODAL_EXPORT) {
            const bool showGuide = exWhat != EX_VOCALS && level > 0.01f;
            const bool changed = std::abs(semis) >= 0.5f || std::abs(tempo - 1) > 0.01f;
            SDL_Rect r = modalFrame(px(500), exportDone.empty() ? px(272 + (showGuide ? 36 : 0) + (changed ? 36 : 0)) : px(220));
            const int x = r.x + pad, w = r.w - 2 * pad;
            const bool runningTask = exportTask && !exportTask->finished;
            if (!exportDone.empty()) {
                const bool ok = exportDone == "ok";
                p.text(ok ? "Exported" : "Export failed", x, r.y + pad, F_TITLE, ok ? kText : kWarn);
                int y = r.y + pad + px(40);
                for (const std::string& line : p.wrap(ok ? exportTask->savedFirst : exportTask->error, F_BODY, w)) { p.text(line, x, y, F_BODY, kMuted); y += p.fontHeight(F_BODY); }
                SDL_Rect close{r.x + r.w - pad - px(100), r.y + r.h - pad - px(36), px(100), px(36)};
                if (ok) {
                    SDL_Rect show{close.x - px(160), close.y, px(150), px(36)};
                    if (button("modalshow", show, "Show in folder", false)) {
                        const std::wstring args = L"/select,\"" + widen(exportTask->savedFirst) + L"\"";
                        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
                    }
                }
                if (button("modaldone", close, "Done", true)) { modal = MODAL_NONE; exportTask.reset(); exportDone.clear(); }
                return;
            }
            p.text("Export", x, r.y + pad, F_TITLE, kText);
            int y = r.y + pad + px(42);
            p.text("What", x, y, F_SMALL, kMuted);
            y += px(20);
            segmented("modalwhat", SDL_Rect{x, y, w, px(32)}, {"Instrumental", "Vocals only", "Both"}, exWhat);
            y += px(46);
            p.text("Format", x, y, F_SMALL, kMuted);
            y += px(20);
            segmented("modalfmt", SDL_Rect{x, y, w, px(32)}, {"WAV", "FLAC", "MP3"}, exFormat);
            y += px(46);
            char b[96];
            if (showGuide) {
                std::snprintf(b, sizeof b, "Keep the guide vocal at %d%%", (int)std::lround(level * 100));
                toggle("modalguide", SDL_Rect{x, y, w, px(30)}, b, exGuide);
                y += px(36);
            }
            if (changed) {
                std::snprintf(b, sizeof b, "Apply key %+d and tempo %d%%", (int)std::lround(semis), (int)std::lround(tempo * 100));
                toggle("modalstretch", SDL_Rect{x, y, w, px(30)}, b, exStretch);
            }
            const int by = r.y + r.h - pad - px(36);
            SDL_Rect go{r.x + r.w - pad - px(120), by, px(120), px(36)};
            SDL_Rect cancel{go.x - px(110), by, px(100), px(36)};
            if (runningTask) {
                p.ring(x + px(12), by + px(18), px(11), px(3), (SDL_GetTicks() % 1200) / 1200.f, kAccent);
                p.text("Writing the file", x + px(32), by + (px(36) - p.fontHeight(F_BODY)) / 2, F_BODY, kText);
            } else if (button("modalcancel2", cancel, "Cancel", false)) modal = MODAL_NONE;
            if (button("modalgo", go, "Export\xE2\x80\xA6", true, !runningTask)) {
                const std::string path = askSavePath();
                if (!path.empty()) {
                    exportTask = std::make_unique<ExportTask>();
                    exportTask->stems = stems;
                    exportTask->what = exWhat;
                    exportTask->format = exFormat;
                    exportTask->guide = exGuide && exWhat != EX_VOCALS ? level : 0.f;
                    exportTask->semitones = changed && exStretch ? semis : 0.f;
                    exportTask->tempo = changed && exStretch ? tempo : 1.f;
                    exportTask->path = path;
                    exportTask->start();
                }
            }
        }
    }

    void startLyricsTask(bool transcribe) {
        if (!hasSong) return;
        lyricsTask = std::make_unique<LyricsTask>();
        lyricsTask->entry = song;
        lyricsTask->title = findTitle;
        lyricsTask->artist = findArtist;
        lyricsTask->transcribe = transcribe;
        lyricsTask->start();
    }

    // ------------------------------------------------------------ files
    void askOpen() {
        wchar_t file[32768] = L"";
        static const wchar_t filter[] = L"Music\0*.mp3;*.flac;*.wav;*.m4a;*.aac;*.ogg;*.opus;*.aif;*.aiff;*.wma;*.alac;*.ape;*.wv;*.mka;*.mp4;*.webm\0All files\0*.*\0";
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof ofn;
        ofn.hwndOwner = windowHandle();
        ofn.lpstrFile = file;
        ofn.nMaxFile = 32768;
        ofn.lpstrFilter = filter;
        ofn.lpstrTitle = L"Add a song";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ofn)) processFile(narrow(file));
        uiFrames = 3;
    }

    std::string askSavePath() {
        static const char* exts[] = {"wav", "flac", "mp3"};
        const std::string ext = exts[std::clamp(exFormat, 0, 2)];
        const std::string suffix = exWhat == EX_VOCALS ? " (Vocals)" : " (Instrumental)";
        std::wstring name = widen(safeFileName(song.title) + suffix + "." + ext);
        name.resize(32768, L'\0');
        const std::wstring wext = widen(ext);
        const std::wstring filter = L"Audio (*." + wext + L")" + std::wstring(1, L'\0') + L"*." + wext + std::wstring(2, L'\0');
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof ofn;
        ofn.hwndOwner = windowHandle();
        ofn.lpstrFile = &name[0];
        ofn.nMaxFile = (DWORD)name.size();
        ofn.lpstrFilter = filter.c_str();
        ofn.lpstrDefExt = wext.c_str();
        ofn.lpstrTitle = L"Export as";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        uiFrames = 3;
        if (!GetSaveFileNameW(&ofn)) return "";
        return narrow(name.c_str());
    }

    // ------------------------------------------------------------ the frame
    void drawFrame() {
        SDL_SetRenderDrawColor(ren, kBase.r, kBase.g, kBase.b, 255);
        SDL_RenderClear(ren);
        if (sceneTex) SDL_RenderCopy(ren, sceneTex, nullptr, nullptr);
        if (procJob && !procJob->finished && !hasSong) drawProcessingStage();
        else if (hasSong) drawLyrics();
        else if (openJob) p.textCentred("Opening", stage, F_HERO, kMuted);
        else drawEmptyStage();
        drawTopBar();
        drawTransport();
        drawSidebar();
        drawModal();
        if (SDL_GetTicks() < toastUntil && !toast.empty()) {
            const std::string msg = p.fit(toast.substr(0, toast.find('\n')), F_BODY, stage.w - px(80));
            const int w = p.textWidth(msg, F_BODY) + px(36);
            SDL_Rect r{stage.x + stage.w / 2 - w / 2, stage.y + px(12), w, px(38)};
            p.softShadow(r, r.h / 2, px(10), 110);
            p.rounded(r, r.h / 2, SDL_Color{48, 48, 48, 245});
            p.textCentred(msg, r, F_BODY, kText);
        }
        SDL_RenderPresent(ren);
    }

    // Events the test hooks inject carry this window id. During a scripted run everything else
    // from the mouse and keyboard is ignored, so a click on the test window can't change the
    // result (or the song's saved settings).
    static constexpr Uint32 kScriptedWindow = 0x5C21;
    bool scriptedRun = false;

    static bool isUserInput(const SDL_Event& e) {
        switch (e.type) {
        case SDL_MOUSEMOTION: return e.motion.windowID != kScriptedWindow;
        case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: return e.button.windowID != kScriptedWindow;
        case SDL_MOUSEWHEEL: case SDL_KEYDOWN: case SDL_KEYUP: case SDL_TEXTINPUT: case SDL_DROPFILE: return true;
        default: return false;
        }
    }

    void handleEvent(const SDL_Event& e) {
        if (scriptedRun && isUserInput(e)) {
            if (e.type == SDL_DROPFILE) SDL_free(e.drop.file);
            return;
        }
        switch (e.type) {
        case SDL_QUIT: running = false; break;
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) layoutDirty = true;
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                // The Windows switches may have changed while the app was in the background.
                glassOn = systemAllowsTransparency();
                motionOn = systemAllowsAnimation();
            }
            uiFrames = 3;
            break;
        case SDL_MOUSEMOTION: mx = e.motion.x; my = e.motion.y; uiFrames = std::max(uiFrames, 1); break;
        case SDL_MOUSEBUTTONDOWN:
            if (e.button.button == SDL_BUTTON_LEFT) { mx = e.button.x; my = e.button.y; mouseDown = true; mousePressed = true; }
            uiFrames = 3;
            break;
        case SDL_MOUSEBUTTONUP:
            if (e.button.button == SDL_BUTTON_LEFT) { mx = e.button.x; my = e.button.y; mouseDown = false; mouseReleased = true; }
            uiFrames = 3;
            break;
        case SDL_MOUSEWHEEL: wheel += e.wheel.preciseY; uiFrames = 3; break;
        case SDL_TEXTINPUT: typed += e.text.text; uiFrames = 3; break;
        case SDL_DROPFILE:
            processFile(e.drop.file);
            SDL_free(e.drop.file);
            break;
        case SDL_KEYDOWN: {
            uiFrames = 3;
            const SDL_Keycode k = e.key.keysym.sym;
            if (k == SDLK_ESCAPE) keyEscape = true;
            if (focusId) {
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER) keyEnter = true;
                if (k == SDLK_BACKSPACE) keyBackspace = true;
                if (k == SDLK_v && (e.key.keysym.mod & KMOD_CTRL)) {
                    if (char* clip = SDL_GetClipboardText()) {
                        std::string t = clip;
                        SDL_free(clip);
                        t.erase(std::remove_if(t.begin(), t.end(), [](char c) { return c == '\r' || c == '\n'; }), t.end());
                        typed += t;
                    }
                }
                break;
            }
            if (modal != MODAL_NONE) break;
            if (k == SDLK_SPACE) togglePlay();
            if (k == SDLK_LEFT && hasSong) { player.seek(player.position() - 5); manualUntil = 0; }
            if (k == SDLK_RIGHT && hasSong) { player.seek(player.position() + 5); manualUntil = 0; }
            if (k == SDLK_UP && hasSong) { level = std::min(1.f, level + 0.1f); player.vocalLevel = level; songSettingsDirty = true; }
            if (k == SDLK_DOWN && hasSong) { level = std::max(0.f, level - 0.1f); if (level < 0.02f) level = 0; player.vocalLevel = level; songSettingsDirty = true; }
            if (k == SDLK_o && (e.key.keysym.mod & KMOD_CTRL)) askOpen();
            break;
        }
        default: break;
        }
    }

    bool init(int argc, char** argv) {
        loadSettings(s);
        int winW = s.winW, winH = s.winH;
        // Windows hands main() its arguments in the ANSI code page, which mangles non-Latin file
        // names, so the wide command line is read instead.
        std::vector<std::string> args;
        int wargc = 0;
        if (LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc)) {
            for (int i = 1; i < wargc; ++i) args.push_back(narrow(wargv[i]));
            LocalFree(wargv);
        } else {
            for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
        }
        for (size_t i = 0; i < args.size(); ++i) {
            const std::string& a = args[i];
            auto next = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : ""; };
            if (a == "--shot") shotPath = next();
            else if (a == "--size") { const std::string v = next(); std::sscanf(v.c_str(), "%dx%d", &winW, &winH); }
            else if (a == "--time") setTime = std::atof(next().c_str());
            else if (a == "--play") playFor = std::atof(next().c_str());
            else if (a == "--song") songOnStart = next();
            else if (a == "--last") songOnStart = "last";
            else if (a == "--modal") modalOnStart = next();
            else if (a == "--library") libraryOverride = next();
            else if (a == "--click") { const int x = std::atoi(next().c_str()); const int y = std::atoi(next().c_str()); pendingClicks.push_back({x, y}); }
            else if (!a.empty() && a[0] != '-') openOnStart = a;
        }

        scriptedRun = !shotPath.empty() || !pendingClicks.empty() || playFor > 0;
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) { fatal(std::string("SDL could not start: ") + SDL_GetError()); return false; }
        if (TTF_Init() != 0) { fatal("The font system could not start."); return false; }
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
        SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
        float ddpi = 96.f;
        if (SDL_GetDisplayDPI(0, &ddpi, nullptr, nullptr) == 0) scale = std::clamp(ddpi / 96.f, 1.f, 3.f);
        p.scale = scale;

        window = SDL_CreateWindow("Undertone", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, std::max(winW, 1000), std::max(winH, 660),
                                  SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN);
        if (!window) { fatal(std::string("Could not open a window: ") + SDL_GetError()); return false; }
        SDL_SetWindowMinimumSize(window, 1000, 660);
        ren = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!ren) ren = SDL_CreateRenderer(window, -1, 0);
        if (!ren) { fatal(std::string("Could not start drawing: ") + SDL_GetError()); return false; }
        p.ren = ren;
        BOOL dark = TRUE;
        if (HWND hwnd = windowHandle())
            if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof dark))) DwmSetWindowAttribute(hwnd, 19, &dark, sizeof dark);

        wchar_t winDir[MAX_PATH];
        GetWindowsDirectoryW(winDir, MAX_PATH);
        const std::string fontDir = narrow(winDir) + "\\Fonts\\";
        const auto load = [&](int id, const char* file, const char* fallback, float size) {
            p.fonts[id] = TTF_OpenFont((fontDir + file).c_str(), px(size));
            if (!p.fonts[id]) p.fonts[id] = TTF_OpenFont((fontDir + fallback).c_str(), px(size));
            if (p.fonts[id]) TTF_SetFontHinting(p.fonts[id], TTF_HINTING_LIGHT);
        };
        load(F_BODY, "segoeui.ttf", "arial.ttf", 13.5f);
        load(F_SMALL, "segoeui.ttf", "arial.ttf", 12.f);
        load(F_LABEL, "seguisb.ttf", "arialbd.ttf", 11.f);
        load(F_TITLE, "seguisb.ttf", "arialbd.ttf", 15.f);
        load(F_ICON, "segmdl2.ttf", "seguisym.ttf", 14.f);
        load(F_ICON_BIG, "segmdl2.ttf", "seguisym.ttf", 20.f);
        load(F_HERO, "seguisb.ttf", "arialbd.ttf", 20.f);
        load(F_LYRIC, "segoeuib.ttf", "arialbd.ttf", 32.f);
        if (!p.fonts[F_BODY]) { fatal("Could not load a font from the Windows fonts folder."); return false; }

        glassOn = systemAllowsTransparency();
        motionOn = systemAllowsAnimation();
        lib.setRoot(libraryOverride.empty() ? exeDir() + "\\library" : libraryOverride);
        lib.scan();
        std::string err;
        if (!player.open(err)) showToast(err, 8000);
        if (findTool("ffmpeg").empty() || findTool("ffprobe").empty()) showToast("ffmpeg.exe and ffprobe.exe are missing. They must sit next to Undertone.exe.", 15000);
        else if (findModel("UVR-MDX-NET-Inst_HQ_3.onnx").empty()) showToast("The vocal removal model is missing from the models folder.", 15000);

        layout();
        buildWall();
        SDL_ShowWindow(window);
        lastTick = SDL_GetTicks();

        if (!openOnStart.empty()) processFile(openOnStart);
        else if (songOnStart == "last" && !s.lastSong.empty()) openSong(s.lastSong);
        else if (!songOnStart.empty() && songOnStart != "last") openSong(songOnStart);
        else if (shotPath.empty() && !s.lastSong.empty()) {
            SongEntry e;
            if (lib.get(s.lastSong, e)) openSong(e.id);  // pick up where you left off
        }
        return true;
    }

    void fatal(const std::string& m) {
        std::fprintf(stderr, "%s\n", m.c_str());
        MessageBoxW(nullptr, widen(m).c_str(), L"Undertone", MB_ICONERROR | MB_OK);
        exitCode = 1;
    }

    void run() {
        bool startedPlayTest = false, wasPlaying = false;
        int settle = 0;
        while (running) {
            const bool scripted = !shotPath.empty() || !pendingClicks.empty() || playUntil || playFor > 0;
            const bool animating = player.playing() || activeId != 0 || uiFrames > 0 || scripted || focusId != 0;
            SDL_Event e;
            if (animating) { while (SDL_PollEvent(&e)) handleEvent(e); }
            else if (SDL_WaitEventTimeout(&e, busy() ? 60 : 250)) {
                handleEvent(e);
                while (SDL_PollEvent(&e)) handleEvent(e);
            } else if (!busy()) {
                // Woke on the timeout with nothing running: only a toast expiring can have changed.
                if (toastUntil && SDL_GetTicks() >= toastUntil) { toastUntil = 0; uiFrames = 1; }
                else continue;
            }

            const Uint32 nowTick = SDL_GetTicks();
            dtMs = (float)std::clamp<Uint32>(nowTick - lastTick, 1, 100);
            lastTick = nowTick;
            pollJobs();
            if (layoutDirty) layout();
            if (wallDirty) buildWall();
            drawFrame();

            mousePressed = mouseReleased = false;
            wheel = 0;
            typed.clear();
            keyEnter = keyBackspace = keyEscape = keyTab = false;
            if (uiFrames > 0) --uiFrames;
            if (wasPlaying && !player.playing()) uiFrames = 3;
            wasPlaying = player.playing();
            if (songSettingsDirty && !mouseDown && !player.playing()) saveSongSettings();

            // Test hooks run once nothing is loading.
            ++settle;
            if (busy()) { settle = 0; continue; }
            if (settle < 3) continue;
            if (!modalOnStart.empty() && hasSong) {
                if (modalOnStart == "export") { modal = MODAL_EXPORT; exGuide = level > 0.01f; }
                else if (modalOnStart == "lyrics") openLyricsModal();
                else if (modalOnStart == "settings") modal = MODAL_SETTINGS;
                modalOnStart.clear();
                settle = 0;
                continue;
            }
            if (!pendingClicks.empty()) {
                const auto c = pendingClicks.front();
                pendingClicks.erase(pendingClicks.begin());
                SDL_Event ev{};
                ev.type = SDL_MOUSEMOTION; ev.motion.x = c.first; ev.motion.y = c.second; ev.motion.windowID = kScriptedWindow;
                SDL_PushEvent(&ev);
                ev = SDL_Event{};
                ev.type = SDL_MOUSEBUTTONDOWN; ev.button.button = SDL_BUTTON_LEFT; ev.button.x = c.first; ev.button.y = c.second; ev.button.windowID = kScriptedWindow;
                SDL_PushEvent(&ev);
                ev.type = SDL_MOUSEBUTTONUP;
                SDL_PushEvent(&ev);
                settle = 0;
                continue;
            }
            if (setTime >= 0 && hasSong) { player.seek(setTime); setTime = -1; settle = 0; continue; }
            if (playFor > 0 && !startedPlayTest && hasSong) {
                startedPlayTest = true;
                player.play();
                playUntil = SDL_GetTicks() + (Uint32)(playFor * 1000);
            }
            if (playUntil && SDL_GetTicks() >= playUntil) {
                playUntil = 0;
                playFor = 0;
                std::fprintf(stderr, "position %.3f s after playing\n", player.position());
                player.pause();
                settle = 0;
                continue;
            }
            if (playUntil) continue;
            if (!shotPath.empty() && settle > 8) {
                drawFrame();
                SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888);
                if (surf && SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch) == 0) {
                    if (SDL_SaveBMP(surf, shotPath.c_str()) != 0) { std::fprintf(stderr, "Could not save %s\n", shotPath.c_str()); exitCode = 1; }
                } else { std::fprintf(stderr, "Could not read the window back: %s\n", SDL_GetError()); exitCode = 1; }
                if (surf) SDL_FreeSurface(surf);
                running = false;
            }
        }
    }

    void shutdown() {
        player.pause();
        saveSongSettings();
        if (procJob) { procJob->cancel = true; procJob->join(); }
        if (openJob) openJob->join();
        lyricsTask.reset();
        exportTask.reset();
        player.close();
        if (window) {
            int w, h;
            SDL_GetWindowSize(window, &w, &h);
            s.winW = w; s.winH = h;
            // Scripted runs must not overwrite real preferences.
            if (shotPath.empty() && pendingClicks.empty() && playFor <= 0 && libraryOverride.empty()) saveSettings(s);
        }
        for (Glass* g : {&glassSide, &glassTop, &glassTransport, &glassModal}) g->destroy();
        for (auto& t : thumbs) if (t.second) SDL_DestroyTexture(t.second);
        if (bigCover) SDL_DestroyTexture(bigCover);
        if (sceneTex) SDL_DestroyTexture(sceneTex);
        p.destroy();
        for (TTF_Font* f : p.fonts) if (f) TTF_CloseFont(f);
        if (ren) SDL_DestroyRenderer(ren);
        if (window) SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
    }
};

}  // namespace

int main(int argc, char** argv) {
    SDL_SetMainReady();
    {
        // Command-line tasks need a console for their output; the app itself has none.
        std::vector<std::string> args(argv + 1, argv + argc);
        bool headless = false;
        for (const std::string& a : args) if (a == "--separate" || a == "--selftest" || a == "--lyrics") headless = true;
        if (headless) {
            // Piped or redirected output already works; only a bare console window needs attaching.
            const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
            const bool redirected = out && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN;
            if (!redirected && AttachConsole(ATTACH_PARENT_PROCESS)) {
                std::freopen("CONOUT$", "w", stdout);
                std::freopen("CONOUT$", "w", stderr);
            }
            return runHeadless(args);
        }
    }
    static App app;
    if (app.init(argc, argv)) app.run();
    app.shutdown();
    return app.exitCode;
}
