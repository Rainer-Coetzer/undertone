// Songs that have already been processed. Each lives in its own folder beside the exe:
//   library/<id>/instrumental.flac  vocals.flac  lyrics.lrc  cover.bmp  meta.json
// so reopening one skips the separation entirely.
#pragma once
#include <mutex>
#include <string>
#include <vector>

#include "lyrics.h"

struct SongEntry {
    std::string id, dir;  // id is a hash of the source file's contents, so a renamed or moved file is still recognised
    std::string title, artist, album, sourcePath, lyricsSource;
    double duration = 0;
    long long added = 0;  // unix seconds
    bool hasCover = false;
    // Per-song choices, restored when the song is reopened.
    float vocalLevel = 0, semitones = 0, tempo = 1, syncOffset = 0;
};

class Library {
public:
    void setRoot(const std::string& dir);  // default: <exe folder>\library
    const std::string& root() const { return rootDir; }
    void scan();
    // The window and the processing thread both use the library, so every call takes a lock
    // and hands out copies.
    std::vector<SongEntry> songs() const;  // newest first
    bool get(const std::string& id, SongEntry& out) const;

    std::string dirFor(const std::string& id) const { return rootDir + "\\" + id; }
    bool save(const SongEntry& entry);  // writes meta.json and updates the list
    void remove(const std::string& id);

    static Lyrics loadLyrics(const SongEntry& entry);
    static bool saveLyrics(const SongEntry& entry, const Lyrics& lyrics);

private:
    std::string rootDir;
    std::vector<SongEntry> entries;
    mutable std::mutex m;
};

// 16 hex digits from the file's size and its first and last megabyte. Empty if unreadable.
std::string contentId(const std::string& path);
bool makeDirs(const std::string& dir);
