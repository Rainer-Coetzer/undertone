// Runs a song through every step on a worker thread: read, strip vocals, save to the library,
// find lyrics. The window polls the public fields to draw progress.
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "library.h"
#include "lyrics.h"
#include "player.h"

class Separator;

enum JobStep { STEP_READ = 0, STEP_MODEL, STEP_SEPARATE, STEP_SAVE, STEP_LYRICS, STEP_TRANSCRIBE, STEP_DONE };
const char* stepName(int step);

struct Job {
    // Inputs. Either a file to process, or a library song to reopen.
    std::string sourcePath;
    std::string reopenId;
    bool allowGpu = true;
    bool allowWhisper = true;

    // Progress, readable from any thread.
    std::atomic<int> step{STEP_READ};
    std::atomic<float> progress{0.f};
    std::atomic<bool> finished{false}, cancel{false};

    // Results, valid once finished is true.
    bool ok = false;
    std::string error, note, device;
    SongEntry entry;
    std::shared_ptr<Stems> stems;
    Lyrics lyrics;

    void start(Library& lib, std::shared_ptr<Separator> sep);
    void join() { if (worker.joinable()) worker.join(); }
    ~Job() { cancel = true; join(); }

private:
    void run(Library& lib, std::shared_ptr<Separator> sep);
    bool loadStems(const std::string& dir);
    std::thread worker;
};

// Finds lyrics for a song already in the library: .lrc beside the source, then LRCLIB, then
// Whisper on the saved vocal stem. Used by processing and by "search again".
Lyrics findLyrics(const SongEntry& e, bool allowWhisper, std::atomic<int>& step, std::atomic<float>& progress,
                  const std::atomic<bool>& cancel, std::string& note);
