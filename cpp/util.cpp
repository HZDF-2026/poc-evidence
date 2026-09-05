// util.cpp — see util.h.
#include "util.h"

#include "jsjson.h"
#include "sha256.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/wait.h>
#include <cwchar>
#ifdef __APPLE__
#include <crt_externs.h>
static char** envpEnviron() { return *_NSGetEnviron(); }
#else
extern "C" char** environ;
static char** envpEnviron() { return environ; }
#endif
#endif

namespace pocev {

const char* const VERSION = "0.1.0";

namespace {

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

}  // namespace

// WHATWG UTF-8 decode: each maximal subpart of an ill-formed sequence becomes
// one U+FFFD, matching Buffer.prototype.toString('utf8').
std::string utf8Sanitize(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    const size_t n = in.size();
    while (i < n) {
        unsigned char b = static_cast<unsigned char>(in[i]);
        if (b < 0x80) {
            out += static_cast<char>(b);
            i++;
            continue;
        }
        int need;
        uint32_t cp;
        if ((b & 0xE0) == 0xC0) {
            need = 1;
            cp = b & 0x1F;
        } else if ((b & 0xF0) == 0xE0) {
            need = 2;
            cp = b & 0x0F;
        } else if ((b & 0xF8) == 0xF0) {
            need = 3;
            cp = b & 0x07;
        } else {
            out += "\xEF\xBF\xBD";
            i++;
            continue;
        }
        size_t valid = 0;
        while (valid < static_cast<size_t>(need) && i + 1 + valid < n &&
               (static_cast<unsigned char>(in[i + 1 + valid]) & 0xC0) == 0x80) {
            cp = (cp << 6) | (static_cast<unsigned char>(in[i + 1 + valid]) & 0x3F);
            valid++;
        }
        if (valid < static_cast<size_t>(need)) {
            // truncated (EOF) or invalid continuation: one replacement for the
            // maximal subpart, then reprocess the offending byte.
            out += "\xEF\xBF\xBD";
            i += 1 + valid;
            continue;
        }
        bool illFormed = false;
        if (need == 1 && cp < 0x80) illFormed = true;
        if (need == 2 && cp < 0x800) illFormed = true;
        if (need == 3 && cp < 0x10000) illFormed = true;
        if (cp >= 0xD800 && cp <= 0xDFFF) illFormed = true;
        if (cp > 0x10FFFF) illFormed = true;
        if (illFormed) {
            out += "\xEF\xBF\xBD";
            i += 1 + need;
            continue;
        }
        appendUtf8(out, cp);
        i += 1 + need;
    }
    return out;
}

std::string nowIso() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count();
    time_t secs = static_cast<time_t>(ms / 1000);
    int msec = static_cast<int>(ms % 1000);
    std::tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &secs);
#else
    gmtime_r(&secs, &tmv);
#endif
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tmv.tm_year + 1900,
                  tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec, msec);
    return buf;
}

std::string sha256File(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("ENOENT: no such file or directory, open '" + path + "'");
    }
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return sha256Hex(data);
}

// ------------------------------------------------------------- path helpers

namespace {

#ifdef _WIN32
bool hasDriveSpec(const std::string& s) {
    return s.size() >= 2 && s[1] == ':' &&
           ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z'));
}
#endif

std::string normalizeNative(const std::string& p) {
    namespace fs = std::filesystem;
    fs::path out = fs::path(p).lexically_normal();
#ifdef _WIN32
    out.make_preferred();
#endif
    return out.string();
}

std::vector<std::string> splitComponents(const std::string& p, bool withDrive) {
#ifndef _WIN32
    (void)withDrive;  // only consulted for drive-letter paths
#endif
    std::vector<std::string> parts;
    size_t start = 0;
#ifdef _WIN32
    if (withDrive && hasDriveSpec(p)) start = 2;
#endif
    if (start < p.size() && (p[start] == '/' || p[start] == '\\')) start++;
    std::string cur;
    for (size_t i = start; i < p.size(); i++) {
        char c = p[i];
        if (c == '/' || c == '\\') {
            if (!cur.empty()) parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}

}  // namespace

std::string pathSep() {
#ifdef _WIN32
    return "\\";
#else
    return "/";
#endif
}

std::string pathJoin(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    std::string base = a;
    while (base.size() > 1 &&
           (base.back() == '/' || base.back() == '\\')
#ifdef _WIN32
           && !(base.size() == 3 && base[1] == ':')
#else
           && base != "/"
#endif
    ) {
        base.pop_back();
    }
    std::string joined = base + pathSep() + b;
    return normalizeNative(joined);
}

std::string pathResolve(const std::string& base, const std::string& target) {
    namespace fs = std::filesystem;
    std::string t = target;
#ifdef _WIN32
    if (!t.empty() && (t[0] == '/' || t[0] == '\\') && !hasDriveSpec(t)) {
        std::string drive;
        if (base.size() >= 2 && base[1] == ':') drive = base.substr(0, 2);
        t = drive + t;
    }
    if (hasDriveSpec(t)) return normalizeNative(t);
#else
    if (!t.empty() && t[0] == '/') return normalizeNative(t);
#endif
    return normalizeNative(pathJoin(base, t));
}

std::string pathRelative(const std::string& from, const std::string& to) {
#ifdef _WIN32
    if (hasDriveSpec(from) && hasDriveSpec(to)) {
        std::string fd = from.substr(0, 2), td = to.substr(0, 2);
        for (auto& c : fd) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (auto& c : td) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (fd != td) return to;
    }
#endif
    std::vector<std::string> fc = splitComponents(from, true);
    std::vector<std::string> tc = splitComponents(to, true);
    size_t k = 0;
    while (k < fc.size() && k < tc.size()) {
#ifdef _WIN32
        std::string a = fc[k], b = tc[k];
        for (auto& c : a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (auto& c : b) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (a != b) break;
#else
        if (fc[k] != tc[k]) break;
#endif
        k++;
    }
    if (k == fc.size() && k == tc.size()) return "";
    if (k == fc.size()) {
        std::string out;
        for (size_t i = k; i < tc.size(); i++) {
            if (!out.empty()) out += pathSep();
            out += tc[i];
        }
        return out;
    }
    std::string out;
    for (size_t i = k; i < fc.size(); i++) {
        if (!out.empty()) out += pathSep();
        out += "..";
    }
    for (size_t i = k; i < tc.size(); i++) {
        if (!out.empty()) out += pathSep();
        out += tc[i];
    }
    return out;
}

bool pathExists(const std::string& p) {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(p), ec);
}

void writeFileBytes(const std::string& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("EACCES: cannot write '" + path + "'");
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

void appendFileBytes(const std::string& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) throw std::runtime_error("EACCES: cannot append '" + path + "'");
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::string readFileBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("ENOENT: no such file or directory, open '" + path + "'");
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<std::string> collectFiles(const std::string& target, const std::string& baseDir) {
    namespace fs = std::filesystem;
    std::string absStr = pathResolve(baseDir, target);
    fs::path absolute(absStr);
    std::error_code ec;
    fs::file_status st = fs::status(absolute, ec);
    if (ec || st.type() == fs::file_type::not_found) {
        throw std::runtime_error("path not found: " + target);
    }
    std::vector<std::string> files;
    auto toRel = [&](const fs::path& p) {
        std::string rel = pathRelative(baseDir, p.string());
        for (auto& c : rel) {
            if (c == '\\') c = '/';
        }
        return rel;
    };
    if (st.type() == fs::file_type::regular) {
        files.push_back(toRel(absolute));
    } else if (st.type() == fs::file_type::directory) {
        std::function<void(const fs::path&)> walk = [&](const fs::path& dir) {
            std::error_code lec;
            fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, lec);
            if (lec) return;
            for (const auto& entry : it) {
                // readdir(withFileTypes) types come from the entry itself
                // (lstat), so symlinks are neither files nor directories.
                std::error_code tec;
                fs::file_status es = entry.symlink_status(tec);
                if (tec) continue;
                if (es.type() == fs::file_type::directory) {
                    std::string name = entry.path().filename().string();
                    if (name == ".git" || name == "node_modules" || name == ".poc-evidence") {
                        continue;
                    }
                    walk(entry.path());
                } else if (es.type() == fs::file_type::regular) {
                    files.push_back(toRel(entry.path()));
                }
            }
        };
        walk(absolute);
    } else {
        throw std::runtime_error("unsupported file type: " + target);
    }
    std::stable_sort(files.begin(), files.end(), jsStringLess);
    return files;
}

std::vector<std::pair<std::string, std::string>> sortedEnv() {
    std::vector<std::pair<std::string, std::string>> vars;
#ifdef _WIN32
    wchar_t* block = GetEnvironmentStringsW();
    if (block) {
        for (wchar_t* p = block; *p; p += std::wcslen(p) + 1) {
            wchar_t* eq = std::wcschr(p, L'=');
            if (!eq) continue;
            std::wstring name(p, eq);
            if (name.empty() || name[0] == L'=') continue;
            std::wstring value(eq + 1);
            int nb = WideCharToMultiByte(CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()),
                                         nullptr, 0, nullptr, nullptr);
            int vb = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
            std::string n(static_cast<size_t>(nb), '\0'), v(static_cast<size_t>(vb), '\0');
            WideCharToMultiByte(CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()), &n[0], nb,
                                nullptr, nullptr);
            WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), &v[0],
                                vb, nullptr, nullptr);
            vars.emplace_back(std::move(n), std::move(v));
        }
        FreeEnvironmentStringsW(block);
    }
#else
    for (char** e = envpEnviron(); e && *e; e++) {
        const char* begin = *e;
        const char* eq = std::strchr(begin, '=');
        if (!eq) continue;
        vars.emplace_back(std::string(begin, eq), std::string(eq + 1));
    }
#endif
    std::stable_sort(vars.begin(), vars.end(),
                     [](const auto& a, const auto& b) { return jsStringLess(a.first, b.first); });
    return vars;
}

// ------------------------------------------------------------------ spawning

#ifdef _WIN32

namespace {

std::wstring toWide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

bool fileExistsAt(const std::string& p) {
    std::error_code ec;
    std::filesystem::file_status st = std::filesystem::status(std::filesystem::path(p), ec);
    return !ec && st.type() == std::filesystem::file_type::regular;
}

bool hasExt(const std::string& name) {
    size_t slash = name.find_last_of("/\\");
    std::string base = slash == std::string::npos ? name : name.substr(slash + 1);
    return base.find('.') != std::string::npos;
}

std::string lowerAscii(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// libuv-style lookup: separators mean a literal path, otherwise search PATH
// with the .com/.exe extensions libuv tries (plus batch files, which spawn
// through cmd.exe like Node does).
std::string resolveExecutable(const std::string& name, const std::string& parentCwd) {
    auto tryOrder = [&](const std::vector<std::string>& candidates) -> std::string {
        for (const auto& c : candidates) {
            if (fileExistsAt(c)) return c;
        }
        return std::string();
    };
    bool hasSep = name.find('/') != std::string::npos || name.find('\\') != std::string::npos;
    if (hasSep) {
        std::string p = (name.size() >= 2 && name[1] == ':') ? name : pathJoin(parentCwd, name);
        if (hasExt(name)) return fileExistsAt(p) ? p : std::string();
        std::string hit = tryOrder({p, p + ".exe", p + ".com"});
        return hit;
    }
    std::string pathEnv;
    {
        wchar_t* pv = _wgetenv(L"PATH");
        if (pv) {
            int n = WideCharToMultiByte(CP_UTF8, 0, pv, -1, nullptr, 0, nullptr, nullptr);
            pathEnv.assign(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
            if (n > 0) {
                WideCharToMultiByte(CP_UTF8, 0, pv, -1, &pathEnv[0], n, nullptr, nullptr);
            }
        }
    }
    std::vector<std::string> dirs;
    std::string cur;
    for (char c : pathEnv + ";") {
        if (c == ';') {
            dirs.push_back(cur.empty() ? "." : cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    for (const auto& dir : dirs) {
        std::string p = pathJoin(dir, name);
        if (hasExt(name)) {
            if (fileExistsAt(p)) return p;
            continue;
        }
        std::string hit = tryOrder({p, p + ".com", p + ".exe", p + ".bat", p + ".cmd"});
        if (!hit.empty()) return hit;
    }
    return std::string();
}

std::wstring quoteArg(const std::wstring& a) {
    if (a.empty() || a.find_first_of(L" \t") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            backslashes++;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out += L'"';
        } else {
            out.append(backslashes, L'\\');
            out += c;
        }
        backslashes = 0;
    }
    out.append(backslashes * 2, L'\\');
    out += L"\"";
    return out;
}

struct PipePair {
    HANDLE read = nullptr;
    HANDLE write = nullptr;
};

void drainPipe(HANDLE h, std::string* out) {
    char buf[8192];
    DWORD got = 0;
    for (;;) {
        if (!ReadFile(h, buf, sizeof buf, &got, nullptr) || got == 0) break;
        out->append(buf, got);
    }
}

}  // namespace

CommandResult runCommand(const std::vector<std::string>& command, const std::string& cwd) {
    if (command.empty()) throw std::runtime_error("spawn: empty command");
    std::string parentCwd = std::filesystem::current_path().string();
    std::string exe = resolveExecutable(command[0], parentCwd);
    if (exe.empty()) {
        throw std::runtime_error("spawn " + command[0] + " ENOENT");
    }
    bool isBatch = false;
    {
        std::string low = lowerAscii(exe);
        if (low.size() >= 4 &&
            (low.substr(low.size() - 4) == ".bat" || low.substr(low.size() - 4) == ".cmd")) {
            isBatch = true;
        }
    }

    std::vector<std::wstring> wargs;
    for (const auto& a : command) wargs.push_back(toWide(a));
    std::wstring cmdline;
    if (isBatch) {
        // libuv routes batch files through cmd.exe with /d /s /c.
        wchar_t sysdir[MAX_PATH];
        GetSystemDirectoryW(sysdir, MAX_PATH);
        cmdline = std::wstring(sysdir) + L"\\cmd.exe /d /s /c \"";
        for (size_t i = 0; i < wargs.size(); i++) {
            if (i) cmdline += L" ";
            cmdline += quoteArg(wargs[i]);
        }
        cmdline += L"\"";
    } else {
        cmdline = quoteArg(toWide(command[0]));
        for (size_t i = 1; i < wargs.size(); i++) {
            cmdline += L" ";
            cmdline += quoteArg(wargs[i]);
        }
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    PipePair outP, errP;
    if (!CreatePipe(&outP.read, &outP.write, &sa, 0) ||
        !CreatePipe(&errP.read, &errP.write, &sa, 0)) {
        throw std::runtime_error("spawn " + command[0] + " EPIPE");
    }
    SetHandleInformation(outP.read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errP.read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outP.write;
    si.hStdError = errP.write;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring exeW = toWide(exe);
    std::wstring cwdW = toWide(cwd);
    std::vector<wchar_t> cmdBuf(cmdline.begin(), cmdline.end());
    cmdBuf.push_back(L'\0');
    BOOL ok = CreateProcessW(isBatch ? nullptr : exeW.c_str(), cmdBuf.data(), nullptr, nullptr,
                             TRUE, 0, nullptr, cwd.empty() ? nullptr : cwdW.c_str(), &si, &pi);
    if (!ok) {
        CloseHandle(outP.read);
        CloseHandle(outP.write);
        CloseHandle(errP.read);
        CloseHandle(errP.write);
        throw std::runtime_error("spawn " + command[0] + " ENOENT");
    }
    CloseHandle(pi.hThread);
    CloseHandle(outP.write);
    CloseHandle(errP.write);

    std::string outBytes, errBytes;
    std::thread t1(drainPipe, outP.read, &outBytes);
    std::thread t2(drainPipe, errP.read, &errBytes);
    t1.join();
    t2.join();
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(outP.read);
    CloseHandle(errP.read);

    CommandResult r;
    r.out = utf8Sanitize(outBytes);
    r.err = utf8Sanitize(errBytes);
    r.code = static_cast<long long>(code);
    r.signaled = false;
    return r;
}

#else  // POSIX

namespace {

const char* errnoName(int e) {
    switch (e) {
        case ENOENT: return "ENOENT";
        case EACCES: return "EACCES";
        case EPERM: return "EPERM";
        case EISDIR: return "EISDIR";
        case ENOTDIR: return "ENOTDIR";
        case ENOEXEC: return "ENOEXEC";
        default: return "EUNKNOWN";
    }
}

}  // namespace

CommandResult runCommand(const std::vector<std::string>& command, const std::string& cwd) {
    if (command.empty()) throw std::runtime_error("spawn: empty command");
    int outp[2], errp[2], execp[2];
    if (pipe(outp) != 0 || pipe(errp) != 0 || pipe(execp) != 0) {
        throw std::runtime_error("spawn " + command[0] + " EPIPE");
    }
    fcntl(execp[1], F_SETFD, FD_CLOEXEC);

    std::vector<char*> argv;
    for (const auto& a : command) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid == 0) {
        while ((dup2(outp[1], STDOUT_FILENO) == -1) && (errno == EINTR)) {}
        while ((dup2(errp[1], STDERR_FILENO) == -1) && (errno == EINTR)) {}
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull != -1) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        close(outp[0]);
        close(outp[1]);
        close(errp[0]);
        close(errp[1]);
        close(execp[0]);
        if (!cwd.empty()) chdir(cwd.c_str());
        execvp(argv[0], argv.data());
        int e = errno;
        ssize_t ignored = write(execp[1], &e, sizeof e);
        (void)ignored;
        _exit(127);
    }
    if (pid < 0) throw std::runtime_error("spawn " + command[0] + " EAGAIN");
    close(outp[1]);
    close(errp[1]);
    close(execp[1]);

    std::string outBytes, errBytes;
    int execErr = 0;
    bool execFailed = false;
    {
        bool outOpen = true, errOpen = true, execOpen = true;
        char buf[8192];
        while (outOpen || errOpen || execOpen) {
            struct pollfd active[3];
            int m = 0;
            if (outOpen) active[m++] = {outp[0], POLLIN, 0};
            if (errOpen) active[m++] = {errp[0], POLLIN, 0};
            if (execOpen) active[m++] = {execp[0], POLLIN, 0};
            int n = poll(active, static_cast<nfds_t>(m), -1);
            if (n <= 0) continue;
            for (int i = 0; i < m; i++) {
                if (active[i].revents & (POLLIN | POLLHUP)) {
                    ssize_t got = read(active[i].fd, buf, sizeof buf);
                    if (got <= 0) {
                        if (active[i].fd == outp[0]) outOpen = false;
                        else if (active[i].fd == errp[0]) errOpen = false;
                        else execOpen = false;
                        continue;
                    }
                    if (active[i].fd == outp[0]) outBytes.append(buf, got);
                    else if (active[i].fd == errp[0]) errBytes.append(buf, got);
                    else {
                        int stored = 0;
                        for (ssize_t j = 0; j < got; j++) {
                            unsigned char b = static_cast<unsigned char>(buf[j]);
                            if (j < static_cast<ssize_t>(sizeof stored)) {
                                reinterpret_cast<unsigned char*>(&stored)[j] = b;
                            }
                        }
                        if (got >= static_cast<ssize_t>(sizeof execErr)) execErr = stored;
                        execFailed = true;
                    }
                }
            }
        }
    }
    close(outp[0]);
    close(errp[0]);
    close(execp[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (execFailed) {
        throw std::runtime_error("spawn " + command[0] + " " + errnoName(execErr));
    }

    CommandResult r;
    r.out = utf8Sanitize(outBytes);
    r.err = utf8Sanitize(errBytes);
    if (WIFEXITED(status)) {
        r.code = WEXITSTATUS(status);
        r.signaled = false;
    } else {
        r.code = 0;
        r.signaled = true;
    }
    return r;
}

#endif  // _WIN32

}  // namespace pocev
