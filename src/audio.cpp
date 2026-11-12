#include "audio.h"

#include <algorithm>
#include <cstdio>

#include "../third_party/json.hpp"
#include "proc.h"

std::string fileNameOf(const std::string& path) {
    const size_t s = path.find_last_of("\\/");
    return s == std::string::npos ? path : path.substr(s + 1);
}

std::string stemOf(const std::string& path) {
    const size_t dot = path.find_last_of('.'), s = path.find_last_of("\\/");
    return dot == std::string::npos || (s != std::string::npos && dot < s) ? path : path.substr(0, dot);
}

static std::string trimmed(std::string s) {
    const char* ws = " \t\r\n";
    s.erase(0, s.find_first_not_of(ws));
    s.erase(s.find_last_not_of(ws) + 1);
    return s;
}

static std::string lastLines(const std::string& s, int lines) {
    size_t pos = s.size();
    while (pos > 0 && lines > 0) {
        pos = s.find_last_not_of("\r\n", pos - 1);
        if (pos == std::string::npos) return s;
        const size_t nl = s.rfind('\n', pos);
        if (nl == std::string::npos) return s;
        pos = nl; --lines;
    }
    return s.substr(pos ? pos + 1 : 0);
}

bool probeTrack(const std::string& path, TrackInfo& out, std::string& error) {
    const std::string probe = findTool("ffprobe");
    if (probe.empty()) { error = "ffprobe.exe was not found. It must sit next to Undertone.exe."; return false; }
    int code = 0;
    const std::string text = runCapture(probe, {"-v", "error", "-show_entries", "format=duration:format_tags:stream=codec_type,duration:stream_tags:stream_disposition=attached_pic",
                                                "-of", "json", path}, code);
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (code != 0 || j.is_discarded() || !j.contains("streams")) { error = "This file could not be read as audio."; return false; }

    out = TrackInfo{};
    out.path = path;
    bool hasAudio = false;
    // Tag names differ in case between containers (TITLE in FLAC, title in MP3), and Ogg keeps
    // its tags on the stream rather than the file.
    auto readTags = [&](const nlohmann::json& tags) {
        if (!tags.is_object()) return;
        for (auto it = tags.begin(); it != tags.end(); ++it) {
            if (!it.value().is_string()) continue;
            std::string k = it.key();
            std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            const std::string v = trimmed(it.value().get<std::string>());
            if (k == "title" && out.title.empty()) out.title = v;
            else if (k == "artist" && out.artist.empty()) out.artist = v;
            else if (k == "album_artist" && out.artist.empty()) out.artist = v;
            else if (k == "album" && out.album.empty()) out.album = v;
        }
    };
    auto readDuration = [&](const nlohmann::json& node) {
        if (out.duration <= 0 && node.contains("duration") && node["duration"].is_string()) out.duration = std::atof(node["duration"].get<std::string>().c_str());
    };
    if (j.contains("format")) {
        readDuration(j["format"]);
        if (j["format"].contains("tags")) readTags(j["format"]["tags"]);
    }
    for (const auto& s : j["streams"]) {
        const std::string type = s.value("codec_type", "");
        if (type == "audio") {
            hasAudio = true;
            readDuration(s);
            if (s.contains("tags")) readTags(s["tags"]);
        } else if (type == "video") {
            out.hasCover = true;
        }
    }
    if (!hasAudio) { error = "There is no audio in this file."; return false; }

    if (out.title.empty()) {
        const std::string name = fileNameOf(stemOf(path));
        const size_t dash = name.find(" - ");
        if (dash != std::string::npos && out.artist.empty()) {
            out.artist = trimmed(name.substr(0, dash));
            out.title = trimmed(name.substr(dash + 3));
        } else {
            out.title = name;
        }
    }
    return true;
}

bool decodeStereo(const std::string& path, int rate, double expectedSeconds, std::vector<float>& out, std::atomic<float>& progress, std::string& error) {
    const std::string ffmpeg = findTool("ffmpeg");
    if (ffmpeg.empty()) { error = "ffmpeg.exe was not found. It must sit next to Undertone.exe."; return false; }
    const std::string errFile = tempPath(".decode.log");
    Process p;
    if (!p.start(ffmpeg, {"-v", "error", "-nostdin", "-i", path, "-map", "0:a:0", "-vn", "-ac", "2", "-ar", std::to_string(rate),
                          "-c:a", "pcm_f32le", "-f", "f32le", "pipe:1"}, false, true, errFile)) {
        error = p.error();
        return false;
    }
    // A pipe can stop in the middle of a float, so this counts bytes rather than samples.
    const double expected = expectedSeconds > 0 ? expectedSeconds * rate * 2 * sizeof(float) : 0;
    out.resize(std::max<size_t>((size_t)(expected / sizeof(float)) + 4096, 1 << 18));
    size_t got = 0;
    long long n;
    for (;;) {
        if (out.size() * sizeof(float) - got < (1 << 16)) out.resize(out.size() + out.size() / 2);
        n = p.read(reinterpret_cast<char*>(out.data()) + got, out.size() * sizeof(float) - got);
        if (n <= 0) break;
        got += (size_t)n;
        if (expected > 0) progress = std::min(0.99f, (float)(got / expected));
    }
    const int code = p.wait();
    const std::string log = readTextFile(errFile);
    removeFile(errFile);
    if (n < 0 || code != 0 || got < 2 * sizeof(float)) {
        error = "Could not decode this file.\n" + lastLines(log, 2);
        return false;
    }
    out.resize(got / sizeof(float) / 2 * 2);
    out.shrink_to_fit();
    progress = 1.f;
    return true;
}

bool encodeStereo(const std::string& path, const float* samples, size_t frames, int rate, const std::vector<std::string>& codecArgs, std::string& error) {
    const std::string ffmpeg = findTool("ffmpeg");
    if (ffmpeg.empty()) { error = "ffmpeg.exe was not found. It must sit next to Undertone.exe."; return false; }
    std::vector<std::string> args = {"-v", "error", "-y", "-f", "f32le", "-ar", std::to_string(rate), "-ac", "2", "-i", "pipe:0"};
    args.insert(args.end(), codecArgs.begin(), codecArgs.end());
    args.push_back(path);
    const std::string errFile = tempPath(".encode.log");
    Process p;
    if (!p.start(ffmpeg, args, true, false, errFile)) { error = p.error(); return false; }
    const bool wrote = p.writeAll(samples, frames * 2 * sizeof(float));
    p.closeStdin();
    const int code = p.wait();
    const std::string log = readTextFile(errFile);
    removeFile(errFile);
    if (!wrote || code != 0) {
        error = "Could not write " + fileNameOf(path) + ".\n" + lastLines(log, 2);
        return false;
    }
    return true;
}

bool extractCover(const std::string& audioPath, const std::string& bmpPath, int size) {
    const std::string ffmpeg = findTool("ffmpeg");
    if (ffmpeg.empty()) return false;
    const std::string sz = std::to_string(size);
    Process p;
    if (!p.start(ffmpeg, {"-v", "error", "-nostdin", "-y", "-i", audioPath, "-an", "-frames:v", "1",
                          "-vf", "scale=" + sz + ":" + sz + ":force_original_aspect_ratio=increase,crop=" + sz + ":" + sz,
                          "-pix_fmt", "bgr24", bmpPath}, false, false, "")) return false;
    return p.wait() == 0;
}
