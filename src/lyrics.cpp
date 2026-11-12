#include "lyrics.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <thread>

#include "../third_party/json.hpp"
#include "audio.h"
#include "proc.h"

namespace {

std::string trimmed(std::string s) {
    const char* ws = " \t\r\n";
    s.erase(0, s.find_first_not_of(ws));
    s.erase(s.find_last_not_of(ws) + 1);
    return s;
}

// "mm:ss", "mm:ss.xx" or "mm:ss:xx". Returns false for metadata tags such as [ar:Name].
bool parseStamp(const std::string& tag, double& seconds) {
    const size_t colon = tag.find(':');
    if (colon == std::string::npos || colon == 0) return false;
    for (size_t i = 0; i < colon; ++i) if (!std::isdigit((unsigned char)tag[i])) return false;
    std::string rest = tag.substr(colon + 1);
    if (rest.empty() || !std::isdigit((unsigned char)rest[0])) return false;
    const size_t second = rest.find(':');
    if (second != std::string::npos) rest[second] = '.';
    for (char c : rest) if (!std::isdigit((unsigned char)c) && c != '.') return false;
    seconds = std::atoi(tag.substr(0, colon).c_str()) * 60.0 + std::atof(rest.c_str());
    return true;
}

std::string urlEncode(const std::string& s) {
    std::string out;
    char b[4];
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else { std::snprintf(b, sizeof b, "%%%02X", c); out += b; }
    }
    return out;
}

// HTTPS GET. Returns the status code, or 0 when the request never got an answer.
int httpsGet(const std::wstring& host, const std::string& pathAndQuery, std::string& body) {
    body.clear();
    int status = 0;
    HINTERNET session = WinHttpOpen(L"Undertone/1.0 (desktop karaoke player)", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return 0;
    WinHttpSetTimeouts(session, 8000, 8000, 10000, 15000);
    HINTERNET conn = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", widen(pathAndQuery).c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
        DWORD code = 0, size = sizeof code;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
        status = (int)code;
        char buf[8192];
        DWORD got = 0;
        while (WinHttpReadData(req, buf, sizeof buf, &got) && got > 0) {
            body.append(buf, got);
            if (body.size() > (8u << 20)) break;  // lyrics are a few KB; anything huge is not what was asked for
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return status;
}

Lyrics fromLrclibEntry(const nlohmann::json& j) {
    Lyrics l;
    if (j.is_object() && j.contains("syncedLyrics") && j["syncedLyrics"].is_string()) l = parseLrc(j["syncedLyrics"].get<std::string>());
    if (!l.lines.empty()) l.source = "lrclib";
    return l;
}

}  // namespace

Lyrics parseLrc(const std::string& text) {
    Lyrics out;
    double offsetMs = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        std::vector<double> stamps;
        size_t at = 0;
        while (at < line.size() && line[at] == '[') {
            const size_t close = line.find(']', at);
            if (close == std::string::npos) break;
            const std::string tag = line.substr(at + 1, close - at - 1);
            double t;
            if (parseStamp(tag, t)) stamps.push_back(t);
            else if (tag.rfind("offset:", 0) == 0) offsetMs = std::atof(tag.c_str() + 7);
            at = close + 1;
        }
        if (stamps.empty()) continue;
        const std::string words = trimmed(line.substr(at));
        for (double t : stamps) out.lines.push_back({t, words});
    }
    // A positive offset in the file means "show everything that much sooner".
    for (LyricLine& l : out.lines) l.time = std::max(0.0, l.time - offsetMs / 1000.0);
    std::stable_sort(out.lines.begin(), out.lines.end(), [](const LyricLine& a, const LyricLine& b) { return a.time < b.time; });
    // Gaps at the very end add nothing, and a file made only of gaps has no lyrics in it.
    while (!out.lines.empty() && out.lines.back().text.empty()) out.lines.pop_back();
    if (!out.lines.empty()) out.source = "file";
    return out;
}

std::string toLrc(const Lyrics& lyrics) {
    std::string out;
    char b[32];
    for (const LyricLine& l : lyrics.lines) {
        const double t = std::max(0.0, l.time);
        std::snprintf(b, sizeof b, "[%02d:%05.2f]", (int)(t / 60), std::fmod(t, 60.0));
        out += b + l.text + "\n";
    }
    return out;
}

Lyrics loadLrcBeside(const std::string& audioPath) {
    const std::string text = readTextFile(stemOf(audioPath) + ".lrc");
    return text.empty() ? Lyrics{} : parseLrc(text);
}

Lyrics fetchLrclib(const std::string& title, const std::string& artist, const std::string& album, double duration, std::string& error) {
    error.clear();
    if (title.empty()) return {};
    std::string body;
    // The exact lookup needs every field; it is the one that matches on length, so it is tried
    // first whenever the tags are complete.
    if (!artist.empty() && duration > 0) {
        std::string q = "/api/get?track_name=" + urlEncode(title) + "&artist_name=" + urlEncode(artist) + "&duration=" + std::to_string((int)std::lround(duration));
        if (!album.empty()) q += "&album_name=" + urlEncode(album);
        const int status = httpsGet(L"lrclib.net", q, body);
        if (status == 0) { error = "Could not reach lrclib.net to look up lyrics."; return {}; }
        if (status == 200) {
            const Lyrics l = fromLrclibEntry(nlohmann::json::parse(body, nullptr, false));
            if (!l.lines.empty()) return l;
        }
    }
    // Otherwise search, and take the synced result closest in length. A different edit of the
    // song (live, extended, radio cut) would drift out of time, so far-off lengths are refused.
    std::string q = "/api/search?track_name=" + urlEncode(title);
    if (!artist.empty()) q += "&artist_name=" + urlEncode(artist);
    const int status = httpsGet(L"lrclib.net", q, body);
    if (status == 0) { error = "Could not reach lrclib.net to look up lyrics."; return {}; }
    if (status != 200) { if (status >= 500) error = "lrclib.net had a problem answering."; return {}; }
    const nlohmann::json arr = nlohmann::json::parse(body, nullptr, false);
    if (!arr.is_array()) return {};
    Lyrics best;
    double bestGap = 4.0;
    for (const auto& entry : arr) {
        const double d = entry.is_object() && entry.contains("duration") && entry["duration"].is_number() ? entry["duration"].get<double>() : -1;
        const double gap = duration > 0 && d > 0 ? std::abs(d - duration) : 3.9;  // unknown length: accept, but prefer a known match
        if (gap >= bestGap) continue;
        const Lyrics l = fromLrclibEntry(entry);
        if (!l.lines.empty()) { best = l; bestGap = gap; }
    }
    return best;
}

Lyrics transcribeWithWhisper(const std::string& audioPath, std::atomic<float>& progress, const std::atomic<bool>& cancel, std::string& error) {
    error.clear();
    progress = 0.f;
    const std::string whisper = findTool("whisper-cli"), ffmpeg = findTool("ffmpeg"), model = findModel("ggml-small.bin");
    if (whisper.empty() || model.empty() || ffmpeg.empty()) { error = "The transcription model is not installed."; return {}; }

    // Whisper only reads 16 kHz mono WAV.
    const std::string base = tempPath(".whisper"), wav = base + ".wav", log = base + ".log", json = base + ".json";
    {
        Process conv;
        if (!conv.start(ffmpeg, {"-v", "error", "-nostdin", "-y", "-i", audioPath, "-map", "0:a:0", "-ac", "1", "-ar", "16000", "-c:a", "pcm_s16le", wav}, false, false, "") || conv.wait() != 0) {
            removeFile(wav);
            error = "Could not prepare the vocals for transcription.";
            return {};
        }
    }
    const int threads = std::clamp((int)std::thread::hardware_concurrency() - 2, 2, 8);
    Process p;
    // -ml/-sow break the text into short lines at word boundaries, which is what a karaoke
    // screen needs; Whisper's own segments run to whole sentences. -sns stops it writing "[Music]".
    const bool started = p.start(whisper, {"-m", model, "-f", wav, "-l", "auto", "-t", std::to_string(threads), "-ml", "46", "-sow", "-sns",
                                           "-oj", "-of", base, "-pp"}, false, false, log);
    if (!started) { removeFile(wav); error = p.error(); return {}; }
    while (!p.finished(250)) {
        if (cancel) { p.kill(); p.wait(); break; }
        // Progress arrives as lines like "... progress =  45%" in the log.
        const std::string text = readTextFile(log);
        const size_t at = text.rfind("progress =");
        if (at != std::string::npos) progress = std::clamp(std::atoi(text.c_str() + at + 10) / 100.f, 0.f, 0.99f);
    }
    const int code = p.wait();
    const std::string out = readTextFile(json);
    removeFile(wav); removeFile(log); removeFile(json);
    if (cancel) { error = "Cancelled."; return {}; }
    const nlohmann::json j = nlohmann::json::parse(out, nullptr, false);
    if (code != 0 || j.is_discarded() || !j.contains("transcription")) { error = "Transcription failed."; return {}; }

    Lyrics l;
    for (const auto& seg : j["transcription"]) {
        if (!seg.contains("text") || !seg["text"].is_string() || !seg.contains("offsets")) continue;
        std::string words = trimmed(seg["text"].get<std::string>());
        // Whisper describes sounds in brackets and fills silence with musical notes. Neither is a lyric.
        if (words.empty() || words.front() == '[' || words.front() == '(' || words.find("\xE2\x99\xAA") != std::string::npos) continue;
        const double from = seg["offsets"].value("from", 0) / 1000.0;
        // The same line repeated back to back at one instant is the model stuttering.
        if (!l.lines.empty() && l.lines.back().text == words && from - l.lines.back().time < 0.5) continue;
        l.lines.push_back({from, words});
    }
    if (!l.lines.empty()) l.source = "whisper";
    progress = 1.f;
    return l;
}

int lineAt(const Lyrics& lyrics, double time) {
    int lo = 0, hi = (int)lyrics.lines.size();
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (lyrics.lines[mid].time <= time) lo = mid + 1; else hi = mid;
    }
    return lo - 1;
}
