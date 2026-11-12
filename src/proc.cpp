#include "proc.h"

#include <windows.h>

#include <atomic>
#include <cstdio>

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::string exeDir() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, sizeof buf / sizeof buf[0]);
    std::wstring p(buf, n);
    const size_t slash = p.find_last_of(L"\\/");
    return narrow(slash == std::wstring::npos ? L"." : p.substr(0, slash));
}

static bool fileExists(const std::string& p) {
    const DWORD a = GetFileAttributesW(widen(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string findTool(const std::string& name) {
    const std::string dir = exeDir();
    for (const std::string& c : {dir + "\\" + name + ".exe", dir + "\\ffmpeg\\" + name + ".exe", dir + "\\whisper\\" + name + ".exe",
                                 dir + "\\..\\..\\deps\\ffmpeg\\" + name + ".exe", dir + "\\..\\..\\deps\\whisper\\" + name + ".exe"})
        if (fileExists(c)) return c;
    wchar_t found[MAX_PATH * 4];
    if (SearchPathW(nullptr, widen(name).c_str(), L".exe", sizeof found / sizeof found[0], found, nullptr)) return narrow(found);
    return "";
}

std::string findModel(const std::string& file) {
    const std::string dir = exeDir();
    for (const std::string& c : {dir + "\\models\\" + file, dir + "\\..\\..\\models\\" + file})
        if (fileExists(c)) return c;
    return "";
}

bool pathExists(const std::string& path) { return GetFileAttributesW(widen(path).c_str()) != INVALID_FILE_ATTRIBUTES; }

bool writeTextFile(const std::string& path, const std::string& text) {
    FILE* f = _wfopen(widen(path).c_str(), L"wb");
    if (!f) return false;
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    fclose(f);
    return ok;
}

std::string tempPath(const std::string& suffix) {
    static std::atomic<int> counter{0};
    wchar_t dir[MAX_PATH + 1];
    GetTempPathW(MAX_PATH + 1, dir);
    return narrow(dir) + "undertone-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(counter++) + suffix;
}

std::string readTextFile(const std::string& path) {
    std::string out;
    FILE* f = _wfopen(widen(path).c_str(), L"rb");
    if (!f) return out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

void removeFile(const std::string& path) { DeleteFileW(widen(path).c_str()); }

// The rules CommandLineToArgvW parses by: backslashes only matter when they precede a quote.
static std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'"') { out.append(slashes * 2 + 1, L'\\'); out += c; }
        else { out.append(slashes, L'\\'); out += c; }
        slashes = 0;
    }
    out.append(slashes * 2, L'\\');
    return out + L"\"";
}

Process::~Process() {
    closeStdin();
    if (outRead) CloseHandle(outRead);
    if (proc) CloseHandle(proc);
}

bool Process::start(const std::string& exe, const std::vector<std::string>& args, bool wantStdin, bool wantStdout, const std::string& stderrFile) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE inRead = nullptr, outWrite = nullptr, errHandle = nullptr;
    HANDLE nullIn = nullptr, nullOut = nullptr;

    // Only the child's ends are inheritable. If our ends leaked into the child, it would hold its
    // own stdin open and never see end of stream, so a finished export would hang at 100%.
    if (wantStdin) {
        HANDLE w = nullptr;
        if (!CreatePipe(&inRead, &w, &sa, 1 << 20)) { err = "Could not create a pipe to ffmpeg."; return false; }
        SetHandleInformation(w, HANDLE_FLAG_INHERIT, 0);
        inWrite = w;
    } else {
        nullIn = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    }
    if (wantStdout) {
        HANDLE r = nullptr;
        if (!CreatePipe(&r, &outWrite, &sa, 1 << 20)) { err = "Could not create a pipe from ffmpeg."; return false; }
        SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
        outRead = r;
    } else {
        nullOut = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    }
    if (!stderrFile.empty())
        errHandle = CreateFileW(widen(stderrFile).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
    if (!errHandle || errHandle == INVALID_HANDLE_VALUE)
        errHandle = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    std::wstring cmd = quoteArg(widen(exe));
    for (const auto& a : args) cmd += L" " + quoteArg(widen(a));

    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = wantStdin ? inRead : nullIn;
    si.hStdOutput = wantStdout ? outWrite : nullOut;
    si.hStdError = errHandle;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

    if (inRead) CloseHandle(inRead);
    if (outWrite) CloseHandle(outWrite);
    if (nullIn) CloseHandle(nullIn);
    if (nullOut) CloseHandle(nullOut);
    CloseHandle(errHandle);
    if (!ok) { err = "Could not start " + exe; return false; }
    CloseHandle(pi.hThread);
    proc = pi.hProcess;
    return true;
}

bool Process::writeAll(const void* data, size_t size) {
    const char* p = (const char*)data;
    while (size > 0) {
        DWORD wrote = 0;
        const DWORD chunk = size > (1u << 24) ? (1u << 24) : (DWORD)size;
        if (!WriteFile((HANDLE)inWrite, p, chunk, &wrote, nullptr)) return false;
        p += wrote; size -= wrote;
    }
    return true;
}

long long Process::read(void* data, size_t size) {
    DWORD got = 0;
    const DWORD chunk = size > (1u << 24) ? (1u << 24) : (DWORD)size;
    if (!ReadFile((HANDLE)outRead, data, chunk, &got, nullptr))
        return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
    return got;
}

void Process::closeStdin() {
    if (inWrite) { CloseHandle((HANDLE)inWrite); inWrite = nullptr; }
}

int Process::wait() {
    if (!proc) return -1;
    WaitForSingleObject((HANDLE)proc, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess((HANDLE)proc, &code);
    return (int)code;
}

bool Process::finished(int waitMs) {
    return !proc || WaitForSingleObject((HANDLE)proc, (DWORD)waitMs) == WAIT_OBJECT_0;
}

void Process::kill() {
    if (proc) TerminateProcess((HANDLE)proc, 1);
}

std::string runCapture(const std::string& exe, const std::vector<std::string>& args, int& exitCode) {
    Process p;
    std::string out;
    if (!p.start(exe, args, false, true, "")) { exitCode = -1; return out; }
    char buf[8192];
    long long n;
    while ((n = p.read(buf, sizeof buf)) > 0) out.append(buf, (size_t)n);
    exitCode = p.wait();
    return out;
}
