// Runs ffmpeg and ffprobe as child processes with no console window. Paths and arguments are
// UTF-8 here and UTF-16 at the Windows boundary, so music in folders with accents or non-Latin
// names still opens.
#pragma once
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& wide);

std::string exeDir();
// A helper program (ffmpeg, ffprobe, whisper-cli): next to the exe first, then its own folder
// there, then deps/ (dev builds), then the PATH. Returns "" when not found.
std::string findTool(const std::string& name);
// A file in the models folder beside the exe, or the project's models folder in a dev build.
std::string findModel(const std::string& file);
bool pathExists(const std::string& path);
bool writeTextFile(const std::string& path, const std::string& text);
std::string tempPath(const std::string& suffix);
std::string readTextFile(const std::string& path);
void removeFile(const std::string& path);

class Process {
public:
    Process() = default;
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // stderr goes to a file: ffmpeg writes plenty to it, and an undrained pipe would block the
    // child forever once its buffer fills, which looks like a hung export.
    bool start(const std::string& exe, const std::vector<std::string>& args, bool wantStdin, bool wantStdout, const std::string& stderrFile);
    bool writeAll(const void* data, size_t size);
    long long read(void* data, size_t size);  // 0 at end of stream, -1 on error
    void closeStdin();
    int wait();  // exit code
    bool finished(int waitMs);  // waits up to waitMs, so a caller can show progress or cancel
    void kill();
    const std::string& error() const { return err; }

private:
    void* proc = nullptr;
    void* inWrite = nullptr;
    void* outRead = nullptr;
    std::string err;
};

// Runs to completion and returns stdout. exitCode is -1 if it could not start.
std::string runCapture(const std::string& exe, const std::vector<std::string>& args, int& exitCode);
