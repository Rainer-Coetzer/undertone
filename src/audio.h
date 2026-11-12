// Getting audio in and out through ffmpeg: tags, decoding to stereo float, cover art, and
// writing stems and exports.
#pragma once
#include <atomic>
#include <string>
#include <vector>

struct TrackInfo {
    std::string path, title, artist, album;
    double duration = 0;
    bool hasCover = false;
};

std::string fileNameOf(const std::string& path);
std::string stemOf(const std::string& path);  // path without its extension

// Reads tags and duration. When the file has no title tag, "Artist - Title.mp3" is split at the
// dash, and failing that the file name becomes the title.
bool probeTrack(const std::string& path, TrackInfo& out, std::string& error);

// Interleaved stereo at the given rate. expectedSeconds only drives the progress value.
bool decodeStereo(const std::string& path, int rate, double expectedSeconds, std::vector<float>& out, std::atomic<float>& progress, std::string& error);

// Writes interleaved stereo float to any format ffmpeg knows; the extension picks the container
// and codecArgs the codec, e.g. {"-c:a", "flac"}.
bool encodeStereo(const std::string& path, const float* samples, size_t frames, int rate, const std::vector<std::string>& codecArgs, std::string& error);

// Embedded cover art as a square BMP, which SDL loads without another library.
bool extractCover(const std::string& audioPath, const std::string& bmpPath, int size);
