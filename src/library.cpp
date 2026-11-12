#include "library.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "../third_party/json.hpp"
#include "proc.h"

std::string contentId(const std::string& path) {
    FILE* f = _wfopen(widen(path).c_str(), L"rb");
    if (!f) return "";
    _fseeki64(f, 0, SEEK_END);
    const long long size = _ftelli64(f);
    unsigned long long h = 14695981039346656037ull;
    auto mix = [&](const unsigned char* p, size_t n) { for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull; };
    mix((const unsigned char*)&size, sizeof size);
    std::vector<unsigned char> buf(1 << 20);
    _fseeki64(f, 0, SEEK_SET);
    mix(buf.data(), fread(buf.data(), 1, buf.size(), f));
    if (size > (long long)buf.size()) {
        _fseeki64(f, size - (long long)buf.size(), SEEK_SET);
        mix(buf.data(), fread(buf.data(), 1, buf.size(), f));
    }
    fclose(f);
    char out[20];
    std::snprintf(out, sizeof out, "%016llx", h);
    return out;
}

bool makeDirs(const std::string& dir) {
    const int r = SHCreateDirectoryExW(nullptr, widen(dir).c_str(), nullptr);
    return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS;
}

void Library::setRoot(const std::string& dir) {
    // SHCreateDirectoryEx refuses relative paths and forward slashes.
    wchar_t full[MAX_PATH * 4];
    const DWORD n = GetFullPathNameW(widen(dir).c_str(), sizeof full / sizeof full[0], full, nullptr);
    rootDir = n ? narrow(std::wstring(full, n)) : dir;
    makeDirs(rootDir);
}

static bool readEntry(const std::string& dir, const std::string& id, SongEntry& e) {
    const nlohmann::json j = nlohmann::json::parse(readTextFile(dir + "\\meta.json"), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    // A folder without both stems is a song whose processing was interrupted.
    if (!pathExists(dir + "\\instrumental.flac") || !pathExists(dir + "\\vocals.flac")) return false;
    e = SongEntry{};
    e.id = id; e.dir = dir;
    e.title = j.value("title", ""); e.artist = j.value("artist", ""); e.album = j.value("album", "");
    e.sourcePath = j.value("source", ""); e.lyricsSource = j.value("lyrics", "");
    e.duration = j.value("duration", 0.0); e.added = j.value("added", 0LL);
    e.vocalLevel = j.value("vocal_level", 0.f); e.semitones = j.value("semitones", 0.f);
    e.tempo = j.value("tempo", 1.f); e.syncOffset = j.value("sync_offset", 0.f);
    e.hasCover = pathExists(dir + "\\cover.bmp");
    return true;
}

void Library::scan() {
    std::vector<SongEntry> found;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(widen(rootDir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        const std::string id = narrow(fd.cFileName);
        SongEntry e;
        if (readEntry(rootDir + "\\" + id, id, e)) found.push_back(e);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(found.begin(), found.end(), [](const SongEntry& a, const SongEntry& b) { return a.added > b.added; });
    std::lock_guard<std::mutex> lock(m);
    entries = found;
}

std::vector<SongEntry> Library::songs() const {
    std::lock_guard<std::mutex> lock(m);
    return entries;
}

bool Library::get(const std::string& id, SongEntry& out) const {
    std::lock_guard<std::mutex> lock(m);
    for (const SongEntry& e : entries) if (e.id == id) { out = e; return true; }
    return false;
}

bool Library::save(const SongEntry& entry) {
    SongEntry e = entry;
    e.dir = dirFor(e.id);
    if (e.added == 0) e.added = (long long)std::time(nullptr);
    makeDirs(e.dir);
    nlohmann::json j;
    j["title"] = e.title; j["artist"] = e.artist; j["album"] = e.album;
    j["source"] = e.sourcePath; j["lyrics"] = e.lyricsSource;
    j["duration"] = e.duration; j["added"] = e.added;
    j["vocal_level"] = e.vocalLevel; j["semitones"] = e.semitones; j["tempo"] = e.tempo; j["sync_offset"] = e.syncOffset;
    // Tags are not guaranteed to be valid UTF-8; replace what isn't rather than throw.
    if (!writeTextFile(e.dir + "\\meta.json", j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace))) return false;
    e.hasCover = pathExists(e.dir + "\\cover.bmp");
    std::lock_guard<std::mutex> lock(m);
    for (SongEntry& existing : entries) if (existing.id == e.id) { existing = e; return true; }
    entries.insert(entries.begin(), e);
    return true;
}

void Library::remove(const std::string& id) {
    // Only ever deletes the files this app writes, inside a folder named by a 16-digit id under
    // the library root, so a bad id cannot reach anything else.
    if (id.size() != 16 || id.find_first_not_of("0123456789abcdef") != std::string::npos) return;
    const std::string dir = dirFor(id);
    for (const char* f : {"instrumental.flac", "vocals.flac", "lyrics.lrc", "cover.bmp", "meta.json"}) removeFile(dir + "\\" + f);
    RemoveDirectoryW(widen(dir).c_str());
    std::lock_guard<std::mutex> lock(m);
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const SongEntry& e) { return e.id == id; }), entries.end());
}

Lyrics Library::loadLyrics(const SongEntry& entry) {
    Lyrics l = parseLrc(readTextFile(entry.dir + "\\lyrics.lrc"));
    if (!l.lines.empty()) l.source = entry.lyricsSource.empty() ? "file" : entry.lyricsSource;
    return l;
}

bool Library::saveLyrics(const SongEntry& entry, const Lyrics& lyrics) {
    if (lyrics.lines.empty()) { removeFile(entry.dir + "\\lyrics.lrc"); return true; }
    return writeTextFile(entry.dir + "\\lyrics.lrc", toLrc(lyrics));
}
