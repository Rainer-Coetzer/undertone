// Time-stamped lyrics and the three places they come from: an .lrc file beside the song, the
// LRCLIB database, and a transcription of the isolated vocals by Whisper.
#pragma once
#include <atomic>
#include <string>
#include <vector>

struct LyricLine {
    double time = 0;   // seconds from the start of the song
    std::string text;  // empty marks an instrumental gap
};

struct Lyrics {
    std::vector<LyricLine> lines;
    std::string source;  // "file", "lrclib", "whisper", or "" when there are none
};

// Lines without a [mm:ss.xx] stamp are dropped, so plain unsynced lyrics parse to nothing.
Lyrics parseLrc(const std::string& text);
std::string toLrc(const Lyrics& lyrics);

Lyrics loadLrcBeside(const std::string& audioPath);

// Sends the song's title, artist, album and length to lrclib.net and nothing else. error is set
// when the lookup itself failed (offline, server error), and left empty for a plain "not found".
Lyrics fetchLrclib(const std::string& title, const std::string& artist, const std::string& album, double duration, std::string& error);

// Runs whisper-cli on an audio file. Best given the isolated vocal stem.
Lyrics transcribeWithWhisper(const std::string& audioPath, std::atomic<float>& progress, const std::atomic<bool>& cancel, std::string& error);

// The line being sung at a given time, or -1 before the first one.
int lineAt(const Lyrics& lyrics, double time);
