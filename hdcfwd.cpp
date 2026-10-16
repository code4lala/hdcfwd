/*
 * hdcfwd - Transparent LAN port forwarder for OpenHarmony hdc
 *
 * Inspired by the Android "adbportforward" tool (chabernac/portforward):
 *   - server mode : listen on 0.0.0.0:<tunnel-port>, forward to 127.0.0.1:<hdc-port>
 *                   (the hdc server always binds loopback only, so we expose it)
 *   - client mode : take over 127.0.0.1:<hdc-port> locally, forward to
 *                   <server-host>:<tunnel-port>. Local hdc clients then work
 *                   with zero configuration - "hdc shell" just works, exactly
 *                   like the device were plugged into the local machine.
 *
 * The hdc client<->server protocol is a plain TCP byte stream (banner
 * handshake "OHOS HDC" + channelId + optional version string, then
 * multiplexed session traffic). No field depends on IP addresses, so a dumb
 * bidirectional TCP pump is fully transparent.
 *
 * Extra care taken to feel "native" on the client machine:
 *   - Local hdc clients check for a running hdc server *before* trying to
 *     connect (named mutex "Global\HDCServer" on Windows / fcntl write lock
 *     on <tmp>/.HDCServer.pid on POSIX, see Base::ProgramMutex in
 *     hdc/src/common/base.cpp). The client mode forwarder acquires exactly
 *     that mutex/lock and keeps it for its lifetime, so local clients never
 *     try to auto-spawn a doomed local hdc server on the busy port.
 *   - The forwarder writes "0" into <tmp>/.HDCServer.pid so that a stray
 *     "hdc kill" on the client machine fails gracefully instead of
 *     SIGKILLing the forwarder itself (hdc kill works by killing the pid
 *     found in that file).
 *
 * Build (no dependencies beyond libc/libpthread/Winsock):
 *   Linux : g++ -O2 -std=c++17 -o hdcfwd hdcfwd.cpp -lpthread
 *   Win   : x86_64-w64-mingw32-clang++ -O2 -std=c++17 -o hdcfwd.exe hdcfwd.cpp \
 *              -lws2_32 -static -lpthread
 *
 * Copyright (C) 2026
 * Licensed under the Apache License, Version 2.0
 */

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
typedef SOCKET socket_t;
#define INVALID_SOCK INVALID_SOCKET
#define SOCK_ERROR SOCKET_ERROR
#define CLOSE_SOCK closesocket
#define SHUT_WR SD_SEND
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <poll.h>
#include <dirent.h>
typedef int socket_t;
#define INVALID_SOCK (-1)
#define SOCK_ERROR (-1)
#define CLOSE_SOCK close
#endif

namespace {

// ---------------------------------------------------------------------------
// Constants kept in sync with hdc/src/common/define.h
// ---------------------------------------------------------------------------
constexpr uint16_t DEFAULT_HDC_PORT = 8710;     // define.h DEFAULT_PORT
constexpr uint16_t DEFAULT_TUNNEL_PORT = 9710;  // 8710 + 1000, private range
const char *const kHdcServerMutexName = "Global\\HDCServer";  // SERVER_NAME
const char *const kPidFileName = ".HDCServer.pid";            // '.' + SERVER_NAME + ".pid"
const char *const kEnvHdcPort = "OHOS_HDC_SERVER_PORT";       // env for hdc server port
const char *const kMarkerFileName = ".hdcfwd.marker";         // our own bookkeeping
constexpr int CONNECT_TIMEOUT_MS = 10000;
constexpr int SHORT_CONNECT_MS = 2000;
constexpr size_t IO_BUF_SIZE = 256 * 1024;

std::atomic<bool> g_stop(false);
bool g_verbose = false;

// for the signal handler to unblock a blocking accept()
struct Options;
Options *g_opt = nullptr;
socket_t g_listenerForWake = INVALID_SOCK;

// ---------------------------------------------------------------------------
// Small utilities
// ---------------------------------------------------------------------------
std::string NowStamp()
{
    char buf[32];
    time_t t = time(nullptr);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

// printf-style logging helpers (gcc/clang checked, msvc just copies args)
#if defined(__GNUC__)
#define HDCFWD_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#define HDCFWD_PRINTF(a, b)
#endif

void LogInfo(const char *fmt, ...) HDCFWD_PRINTF(1, 2);
void LogInfo(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s] [info] ", NowStamp().c_str());
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    fflush(stderr);
    va_end(ap);
}

void LogWarn(const char *fmt, ...) HDCFWD_PRINTF(1, 2);
void LogWarn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s] [warn] ", NowStamp().c_str());
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    fflush(stderr);
    va_end(ap);
}

void LogError(const char *fmt, ...) HDCFWD_PRINTF(1, 2);
void LogError(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s] [error] ", NowStamp().c_str());
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    fflush(stderr);
    va_end(ap);
}

void LogDebug(const char *fmt, ...) HDCFWD_PRINTF(1, 2);
void LogDebug(const char *fmt, ...)
{
    if (!g_verbose) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s] [debug] ", NowStamp().c_str());
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    fflush(stderr);
    va_end(ap);
}

std::string LastSockError()
{
    char buf[256] = {0};
#ifdef _WIN32
    int e = WSAGetLastError();
    strerror_s(buf, sizeof(buf), e);
    (void)e;
#else
    const char *r = strerror_r(errno, buf, sizeof(buf));
    if (r != buf) {
        snprintf(buf, sizeof(buf), "%s", r);
    }
#endif
    return buf;
}

#ifndef _WIN32
void SleepMS(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, nullptr);
}
#else
void SleepMS(int ms)
{
    Sleep((DWORD)ms);
}
#endif

// Temporary directory, mirroring libuv's uv_os_tmpdir() used by hdc:
//   TMPDIR on POSIX, GetTempPathW() on Windows.
std::string TmpDir()
{
#ifdef _WIN32
    wchar_t wpath[MAX_PATH] = {0};
    DWORD n = GetTempPathW(MAX_PATH, wpath);
    if (n == 0 || n >= MAX_PATH) {
        return ".";
    }
    std::string out;
    int len = WideCharToMultiByte(CP_UTF8, 0, wpath, (int)n, nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        return ".";
    }
    out.resize(len);
    WideCharToMultiByte(CP_UTF8, 0, wpath, (int)n, &out[0], len, nullptr, nullptr);
    while (!out.empty() && (out.back() == '\\' || out.back() == '/')) {
        out.pop_back();
    }
    return out.empty() ? "." : out;
#else
    const char *t = getenv("TMPDIR");
    if (t && *t) {
        std::string s = t;
        while (!s.empty() && s.back() == '/') {
            s.pop_back();
        }
        return s.empty() ? "/" : s;
    }
    return "/tmp";
#endif
}

std::string PathJoin(const std::string &a, const std::string &b)
{
    if (a.empty()) {
        return b;
    }
#ifdef _WIN32
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    if (a.back() == sep || a.back() == '/' || a.back() == '\\') {
        return a + b;
    }
    return a + std::string(1, sep) + b;
}

std::string DirName(const std::string &path)
{
    size_t p = path.find_last_of("/\\");
    if (p == std::string::npos) {
        return ".";
    }
    if (p == 0) {
        return path.substr(0, 1);
    }
    return path.substr(0, p);
}

#ifdef _WIN32
std::wstring Utf8ToW16(const std::string &s)
{
    if (s.empty()) {
        return std::wstring();
    }
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w;
    w.resize(len);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], len);
    return w;
}
#endif

bool FileExists(const std::string &p)
{
#ifdef _WIN32
    DWORD attr = GetFileAttributesW(Utf8ToW16(p).c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(p.c_str(), &st) == 0;
#endif
}

// ---------------------------------------------------------------------------
// Sockets
// ---------------------------------------------------------------------------
class SocketInit {
public:
    SocketInit()
    {
#ifdef _WIN32
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
#endif
#ifndef _WIN32
        signal(SIGPIPE, SIG_IGN);
#endif
    }
    ~SocketInit()
    {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};void SetNoDelay(socket_t s)
{
    int one = 1;
#ifdef _WIN32
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
#else
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif
}

void SetNonBlock(socket_t s, bool nonblock)
{
#ifdef _WIN32
    u_long mode = nonblock ? 1 : 0;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) {
        return;
    }
    if (nonblock) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    fcntl(s, F_SETFL, flags);
#endif
}

// Connect to host:port with timeout (ms). Returns valid socket or INVALID_SOCK.
socket_t ConnectHost(const std::string &host, uint16_t port, int timeoutMs, std::string *err)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    struct addrinfo *result = nullptr;
    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%u", (unsigned)port);
    int rc = getaddrinfo(host.c_str(), portStr, &hints, &result);
    if (rc != 0 || result == nullptr) {
        if (err) {
            *err = "cannot resolve '" + host + "'";
        }
        return INVALID_SOCK;
    }
    socket_t s = INVALID_SOCK;
    std::string lastErr = "no address";
    for (struct addrinfo *ai = result; ai != nullptr; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCK) {
            lastErr = "socket() failed";
            continue;
        }
        SetNonBlock(s, true);
        int c = connect(s, ai->ai_addr, (int)ai->ai_addrlen);
        if (c == 0) {
            break;  // connected instantly
        }
        bool inProgress = false;
#ifdef _WIN32
        int e = WSAGetLastError();
        inProgress = (e == WSAEWOULDBLOCK);
#else
        inProgress = (errno == EINPROGRESS);
#endif
        if (!inProgress) {
            lastErr = LastSockError();
            CLOSE_SOCK(s);
            s = INVALID_SOCK;
            continue;
        }
        // wait for writability
#ifdef _WIN32
        fd_set wset, eset;
        FD_ZERO(&wset);
        FD_ZERO(&eset);
        FD_SET(s, &wset);
        FD_SET(s, &eset);
        struct timeval tv;
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (long)(timeoutMs % 1000) * 1000;
        int r = select((int)s + 1, nullptr, &wset, &eset, &tv);
        int soerr = 0;
        int len = sizeof(soerr);
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soerr, &len);
        if (r > 0 && soerr == 0) {
            break;  // connected
        }
        lastErr = soerr != 0 ? "connect failed" : "connect timeout";
#else
        struct pollfd pfd;
        pfd.fd = s;
        pfd.events = POLLOUT;
        int r = poll(&pfd, 1, timeoutMs);
        int soerr = 0;
        socklen_t len = sizeof(soerr);
        getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &len);
        if (r > 0 && soerr == 0) {
            break;
        }
        lastErr = soerr != 0 ? "connect failed" : "connect timeout";
#endif
        CLOSE_SOCK(s);
        s = INVALID_SOCK;
    }
    freeaddrinfo(result);
    if (s != INVALID_SOCK) {
        SetNonBlock(s, false);
        SetNoDelay(s);
        if (err) {
            err->clear();
        }
    } else if (err) {
        *err = lastErr;
    }
    return s;
}

// True when something is listening on 127.0.0.1:port.
bool PortListening(uint16_t port)
{
    std::string err;
    socket_t s = ConnectHost("127.0.0.1", port, SHORT_CONNECT_MS, &err);
    if (s == INVALID_SOCK) {
        return false;
    }
    CLOSE_SOCK(s);
    return true;
}

// ---------------------------------------------------------------------------
// Process helpers (spawn hdc server, kill by pid, image name check)
// ---------------------------------------------------------------------------
std::string SelfPath()
{
#ifdef _WIN32
    wchar_t wbuf[MAX_PATH * 2] = {0};
    DWORD n = GetModuleFileNameW(nullptr, wbuf, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2) {
        return std::string();
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, wbuf, (int)n, nullptr, 0, nullptr, nullptr);
    std::string out;
    out.resize(len);
    WideCharToMultiByte(CP_UTF8, 0, wbuf, (int)n, &out[0], len, nullptr, nullptr);
    return out;
#else
    char buf[4096] = {0};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        return std::string();
    }
    buf[n] = '\0';
    return buf;
#endif
}

// Find the hdc binary: explicit --hdc, then next to hdcfwd itself, then PATH.
std::string ResolveHdcBinary(const std::string &explicitPath)
{
    if (!explicitPath.empty()) {
        if (FileExists(explicitPath)) {
            return explicitPath;
        }
        LogWarn("given --hdc path does not exist: %s", explicitPath.c_str());
    }
    std::string self = SelfPath();
    if (!self.empty()) {
        std::string dir = DirName(self);
#ifdef _WIN32
        std::string cand = PathJoin(dir, "hdc.exe");
#else
        std::string cand = PathJoin(dir, "hdc");
#endif
        if (FileExists(cand)) {
            return cand;
        }
    }
    // search PATH
    const char *pathEnv = getenv("PATH");
    if (pathEnv != nullptr) {
        std::string p = pathEnv;
        size_t begin = 0;
        while (begin <= p.size()) {
            size_t end = p.find(':', begin);
            if (end == std::string::npos) {
                end = p.size();
            }
            std::string dir = p.substr(begin, end - begin);
#ifdef _WIN32
            const char *sep = ";";
#else
            const char *sep = ":";
#endif
            (void)sep;
            if (!dir.empty()) {
#ifdef _WIN32
                std::string cand = PathJoin(dir, "hdc.exe");
                if (FileExists(cand)) {
                    return cand;
                }
#else
                std::string cand = PathJoin(dir, "hdc");
                if (FileExists(cand)) {
                    return cand;
                }
#endif
            }
            if (end == p.size()) {
                break;
            }
            begin = end + 1;
        }
    }
    return std::string();
}

// Run a short command with no visible window; output discarded. Used only for
// the courtesy `hdc kill` fallback.
int RunCommandHidden(const std::string &cmd)
{
#ifdef _WIN32
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    std::wstring wcmd = Utf8ToW16(cmd);
    if (!CreateProcessW(nullptr, &wcmd[0], nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi)) {
        return -1;
    }
    WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
#else
    return system(cmd.c_str());
#endif
}

// Spawn a detached `hdc -m` (server mode). If a server is already running the
// spawned process exits immediately by itself (hdc program mutex), so this is
// race-safe. The server binds loopback only - exactly what we want; the
// forwarder is what exposes it to the LAN.
void SpawnHdcServer(const std::string &hdcPath, uint16_t hdcPort)
{
    if (hdcPath.empty()) {
        LogWarn("hdc binary not found (use --hdc <path>); cannot spawn hdc server");
        return;
    }
    LogInfo("spawning hdc server on 127.0.0.1:%u: %s -m", (unsigned)hdcPort, hdcPath.c_str());
#ifdef _WIN32
    std::wstring wcmd = L"\"" + Utf8ToW16(hdcPath) + L"\" -m -s 127.0.0.1:" +
                        Utf8ToW16(std::to_string((unsigned)hdcPort));
    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));
    BOOL ok = CreateProcessW(nullptr, &wcmd[0], nullptr, nullptr, FALSE,
                             DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi);
    if (ok) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        LogWarn("CreateProcess failed for hdc server (error %lu)", GetLastError());
    }
#else
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        // detach stdio
        FILE *f1 = freopen("/dev/null", "r", stdin);
        FILE *f2 = freopen("/dev/null", "w", stdout);
        FILE *f3 = freopen("/dev/null", "w", stderr);
        (void)f1;
        (void)f2;
        (void)f3;
        std::string portArg = "127.0.0.1:" + std::to_string((unsigned)hdcPort);
        execl(hdcPath.c_str(), "hdc", "-m", "-s", portArg.c_str(), (char *)nullptr);
        _exit(127);
    }
    if (pid < 0) {
        LogWarn("fork failed for hdc server spawn");
    }
#endif
}

// Ensure local hdc server alive: check, spawn, wait up to waitMs.
std::atomic<bool> g_spawnInFlight(false);
void EnsureHdcServer(const std::string &hdcPath, uint16_t hdcPort, int waitMs)
{
    if (PortListening(hdcPort)) {
        return;
    }
    if (hdcPath.empty()) {
        return;  // cannot spawn; the connection attempt will just fail
    }
    bool expected = false;
    if (!g_spawnInFlight.compare_exchange_strong(expected, true)) {
        // another thread is spawning; just wait for the port
        for (int i = 0; i < waitMs / 200 && !g_stop; i++) {
            if (PortListening(hdcPort)) {
                return;
            }
            SleepMS(200);
        }
        return;
    }
    SpawnHdcServer(hdcPath, hdcPort);
    for (int i = 0; i < waitMs / 200 && !g_stop; i++) {
        if (PortListening(hdcPort)) {
            g_spawnInFlight = false;
            return;
        }
        SleepMS(200);
    }
    g_spawnInFlight = false;
    LogWarn("hdc server did not come up on port %u", (unsigned)hdcPort);
}

bool GetProcessImageName(uint32_t pid, std::string &name)
{
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (h == nullptr) {
        return false;
    }
    wchar_t wname[MAX_PATH] = {0};
    DWORD size = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameW(h, 0, wname, &size);
    CloseHandle(h);
    if (!ok) {
        return false;
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, wname, (int)wcslen(wname), nullptr, 0, nullptr, nullptr);
    name.resize(len);
    WideCharToMultiByte(CP_UTF8, 0, wname, (int)wcslen(wname), &name[0], len, nullptr, nullptr);
    return true;
#else
    char buf[4096] = {0};
    char link[64];
    snprintf(link, sizeof(link), "/proc/%u/exe", pid);
    ssize_t n = readlink(link, buf, sizeof(buf) - 1);
    if (n <= 0) {
        return false;
    }
    buf[n] = '\0';
    name = buf;
    return true;
#endif
}

bool ProcessAlive(uint32_t pid)
{
    if (pid == 0) {
        return false;
    }
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (h == nullptr) {
        return false;
    }
    DWORD code = 0;
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    return code == STILL_ACTIVE;
#else
    if (kill((pid_t)pid, 0) != 0) {
        return false;
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "/proc/%u/stat", pid);
    FILE *f = fopen(buf, "r");
    if (f == nullptr) {
        return false;
    }
    char state = '?';
    if (fscanf(f, "%*d %*s %c", &state) != 1) {
        state = '?';
    }
    fclose(f);
    return state != 'Z' && state != 'X';
#endif
}

bool ImageNameLooksLikeHdc(const std::string &path)
{
    std::string base = path;
    size_t p = base.find_last_of("/\\");
    if (p != std::string::npos) {
        base = base.substr(p + 1);
    }
    std::string lower = base;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower == "hdc" || lower == "hdc.exe" || lower == "hdcd" || lower == "hdcfwd" ||
           lower == "hdcfwd.exe" || lower.rfind("hdc", 0) == 0;
}

bool KillPid(uint32_t pid)
{
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (h == nullptr) {
        return false;
    }
    BOOL ok = TerminateProcess(h, 1);
    CloseHandle(h);
    return ok == TRUE;
#else
    return kill((pid_t)pid, SIGKILL) == 0;
#endif
}

std::string ReadTextFile(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return "";
    }
    std::string out;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
    }
    fclose(f);
    return out;
}

bool WriteTextFileAtomic(const std::string &path, const std::string &content)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    size_t n = fwrite(content.data(), 1, content.size(), f);
    fclose(f);
    return n == content.size();
}

int ParsePidText(const std::string &text)
{
    // hdc writes a bare pid string
    std::string s;
    for (char c : text) {
        if (c >= '0' && c <= '9') {
            s += c;
        } else {
            break;
        }
    }
    if (s.empty()) {
        return -1;
    }
    return atoi(s.c_str());
}

// ---------------------------------------------------------------------------
// HDC-server presence lock (client mode)
//
// hdc clients call Base::ProgramMutex(true) before connecting:
//   Windows : OpenMutex("Global\\HDCServer")
//   POSIX   : fcntl(F_SETLK, F_WRLCK) on <tmp>/.HDCServer.pid
// If it reports "server running", they connect right away without trying to
// auto-spawn a local server. We take over exactly these primitives so the
// tunnel looks like a local hdc server to every local client.
//
// NOTE POSIX: fcntl record locks (what hdc uses) and flock() are independent
// lock spaces on Linux - we MUST use fcntl, not flock.
// ---------------------------------------------------------------------------
class HdcPresenceLock {
public:
    ~HdcPresenceLock()
    {
        Release();
    }

    // returns false with reason when a live hdc server / forwarder holds it
    bool Acquire(std::string &reason)
    {
        pidPath_ = PathJoin(TmpDir(), kPidFileName);
        markerPath_ = PathJoin(TmpDir(), kMarkerFileName);
#ifdef _WIN32
        // Permissive security descriptor, mirroring hdc's ProgramMutex(false)
        SECURITY_DESCRIPTOR sd;
        if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION)) {
            reason = "InitializeSecurityDescriptor failed";
            return false;
        }
        if (!SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE)) {
            reason = "SetSecurityDescriptorDacl failed";
            return false;
        }
        SECURITY_ATTRIBUTES sa;
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = &sd;
        sa.bInheritHandle = FALSE;
        mutex_ = CreateMutexA(&sa, TRUE, kHdcServerMutexName);
        if (mutex_ == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
            if (mutex_ != nullptr) {
                CloseHandle(mutex_);
                mutex_ = nullptr;
            }
            reason = "an HDC server (or another hdcfwd) is running - run 'hdc kill' first";
            return false;
        }
        // write pid "0" so a stray `hdc kill` aborts instead of killing us
        WriteTextFileAtomic(pidPath_, "0");
        WriteTextFileAtomic(markerPath_, std::to_string((uint32_t)
#ifdef _WIN32
            GetCurrentProcessId()
#else
            getpid()
#endif
        ) + "\n");
        return true;
#else
        umask(0);
        fd_ = open(pidPath_.c_str(), O_RDWR | O_CREAT, 0666);
        if (fd_ < 0) {
            reason = std::string("cannot open ") + pidPath_ + ": " + strerror(errno);
            return false;
        }
        struct flock fl;
        memset(&fl, 0, sizeof(fl));
        fl.l_type = F_WRLCK;
        fl.l_start = 0;
        fl.l_whence = SEEK_SET;
        fl.l_len = 0;
        if (fcntl(fd_, F_SETLK, &fl) != 0) {
            close(fd_);
            fd_ = -1;
            reason = "an HDC server (or another hdcfwd) holds " + pidPath_ +
                     " - run 'hdc kill' first";
            return false;
        }
        if (ftruncate(fd_, 0) != 0) {
            // ignore, best effort
        }
        if (write(fd_, "0", 1) != 1) {
            // ignore, best effort
        }
        WriteTextFileAtomic(markerPath_, std::to_string((uint32_t)getpid()) + "\n");
        return true;
#endif
    }

    void Release()
    {
        if (!markerPath_.empty()) {
            remove(markerPath_.c_str());
            markerPath_.clear();
        }
#ifdef _WIN32
        if (mutex_ != nullptr) {
            CloseHandle(mutex_);
            mutex_ = nullptr;
        }
#else
        if (fd_ >= 0) {
            close(fd_);  // releases the fcntl lock; hdc servers can start again
            fd_ = -1;
        }
#endif
    }

private:
    std::string pidPath_;
    std::string markerPath_;
#ifdef _WIN32
    HANDLE mutex_ = nullptr;
#else
    int fd_ = -1;
#endif
};

// ---------------------------------------------------------------------------
// Data plane: bidirectional pump with half-close propagation
// ---------------------------------------------------------------------------
struct Session {
    socket_t a = INVALID_SOCK;
    socket_t b = INVALID_SOCK;
    std::atomic<int> refs{2};
    std::atomic<long long> bytes{0};
    std::string tag;
};

int SendAll(socket_t s, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        int n = send(s, buf + off, (int)(len - off), 0);
        if (n == SOCK_ERROR) {
#ifdef _WIN32
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK) {
                continue;  // blocking socket: should not happen, avoid spin
            }
#else
            if (errno == EINTR) {
                continue;
            }
#endif
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

void PumpThread(Session *s, bool a2b)
{
    socket_t from = a2b ? s->a : s->b;
    socket_t to = a2b ? s->b : s->a;
    std::vector<char> buf(IO_BUF_SIZE);
    for (;;) {
        int n = recv(from, buf.data(), (int)buf.size(), 0);
        if (n == 0) {
            break;  // clean EOF
        }
        if (n == SOCK_ERROR) {
#ifdef _WIN32
            int e = WSAGetLastError();
            if (e == WSAEINTR) {
                continue;
            }
#else
            if (errno == EINTR) {
                continue;
            }
#endif
            break;
        }
        if (SendAll(to, buf.data(), (size_t)n) != 0) {
            break;
        }
        s->bytes += n;
    }
    // propagate close to the other direction (half-close aware)
    shutdown(to, SHUT_WR);
    int left = --s->refs;
    if (left == 0) {
        CLOSE_SOCK(s->a);
        CLOSE_SOCK(s->b);
        LogDebug("session %s closed, %lld bytes relayed", s->tag.c_str(),
                 (long long)s->bytes.load());
        delete s;
    }
}

void StartRelay(socket_t clientSide, socket_t upstreamSide, const std::string &tag)
{
    Session *s = new Session();
    s->a = clientSide;
    s->b = upstreamSide;
    s->tag = tag;
    std::thread t1(PumpThread, s, true);
    std::thread t2(PumpThread, s, false);
    t1.detach();
    t2.detach();
}

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------
struct Options {
    bool serverMode = false;
    std::string remoteHost;
    uint16_t tunnelPort = DEFAULT_TUNNEL_PORT;
    uint16_t hdcPort = DEFAULT_HDC_PORT;
    std::string hdcPathArg;
    std::string hdcPath;  // resolved
    bool expose = false;
    int keepaliveSec = 30;
};

socket_t CreateListener(const std::string &bindIp, uint16_t port)
{
    socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCK) {
        LogError("socket() failed: %s", LastSockError().c_str());
        return INVALID_SOCK;
    }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, bindIp.c_str(), &addr.sin_addr) != 1) {
        LogError("invalid bind ip '%s'", bindIp.c_str());
        CLOSE_SOCK(s);
        return INVALID_SOCK;
    }
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) == SOCK_ERROR) {
        LogError("bind %s:%u failed: %s", bindIp.c_str(), (unsigned)port, LastSockError().c_str());
        CLOSE_SOCK(s);
        return INVALID_SOCK;
    }
    if (listen(s, 64) == SOCK_ERROR) {
        LogError("listen failed: %s", LastSockError().c_str());
        CLOSE_SOCK(s);
        return INVALID_SOCK;
    }
    return s;
}

void PrintUsage()
{
    fprintf(stderr,
        "hdcfwd - transparent LAN port forwarder for OpenHarmony hdc\n"
        "\n"
        "Usage:\n"
        "  hdcfwd server [options]\n"
        "      Run on the machine which has the devices connected. Exposes the local\n"
        "      loopback-only hdc server to the LAN on 0.0.0.0:<tunnel-port>.\n"
        "  hdcfwd client <server-host> [options]\n"
        "      Run on remote LAN machines. Kills the local hdc server, takes over\n"
        "      127.0.0.1:<hdc-port> and forwards it to <server-host>:<tunnel-port>.\n"
        "      Afterwards plain 'hdc shell' / 'hdc list targets' work transparently.\n"
        "\n"
        "Options:\n"
        "  -t, --tunnel-port PORT   tunnel port (default %u)\n"
        "                            server: port to listen on for LAN clients\n"
        "                            client: port on the server host to connect to\n"
        "  -p, --hdc-port PORT      hdc server port (default %u or $%s)\n"
        "  -c, --connect-key KEY    (reserved for future use, ignored)\n"
        "      --hdc PATH           path of the hdc binary (default: next to hdcfwd, then $PATH)\n"
        "      --expose             client: bind 0.0.0.0 instead of loopback (enables chaining,\n"
        "                            use with care - everyone on the LAN can then use the tunnel)\n"
        "      --keepalive SEC      server: hdc server health-check interval (default 30, 0=off)\n"
        "  -v, --verbose            verbose logging\n"
        "  -h, --help               this help\n"
        "\n"
        "Examples:\n"
        "  [win - device host]    hdcfwd server\n"
        "  [linux - lan client]   hdcfwd client 192.168.200.1\n"
        "  [linux]                hdc list targets && hdc shell\n",
        (unsigned)DEFAULT_TUNNEL_PORT, (unsigned)DEFAULT_HDC_PORT, kEnvHdcPort);
}

void ParseArgUint16(const char *text, const char *name, uint16_t &out)
{
    char *end = nullptr;
    unsigned long v = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || v == 0 || v > 65535) {
        fprintf(stderr, "[error] invalid value for %s: %s\n", name, text);
        exit(2);
    }
    out = (uint16_t)v;
}

// ---------------------------------------------------------------------------
// Server mode (on the machine with devices)
// ---------------------------------------------------------------------------
int RunServer(Options &opt)
{
    LogInfo("hdcfwd server starting: 0.0.0.0:%u -> 127.0.0.1:%u", (unsigned)opt.tunnelPort,
            (unsigned)opt.hdcPort);
    if (opt.hdcPath.empty()) {
        LogWarn("hdc binary not found; auto-spawn of the hdc server disabled "
                "(will only forward when the server is already running)");
    } else {
        LogInfo("using hdc binary: %s", opt.hdcPath.c_str());
    }

    // bring the local hdc server up right away (lazy + spawn, like adbportforward)
    EnsureHdcServer(opt.hdcPath, opt.hdcPort, 10000);

    socket_t listener = CreateListener("0.0.0.0", opt.tunnelPort);
    if (listener == INVALID_SOCK) {
        return 1;
    }
    g_listenerForWake = listener;
    LogInfo("hdcfwd server ready on 0.0.0.0:%u (local hdc server port %u)",
            (unsigned)opt.tunnelPort, (unsigned)opt.hdcPort);

    // keepalive thread: like the original tool's periodic `adb start-server`
    std::thread keepalive([&opt]() {
        if (opt.keepaliveSec <= 0) {
            return;
        }
        while (!g_stop) {
            for (int i = 0; i < opt.keepaliveSec * 5 && !g_stop; i++) {
                SleepMS(200);
            }
            if (g_stop) {
                return;
            }
            if (!PortListening(opt.hdcPort)) {
                LogInfo("hdc server is down, restarting");
                EnsureHdcServer(opt.hdcPath, opt.hdcPort, 10000);
            }
        }
    });
    keepalive.detach();

    long long connId = 0;
    while (!g_stop) {
        struct sockaddr_in peer;
        socklen_t peerLen = sizeof(peer);
        socket_t in = accept(listener, (struct sockaddr *)&peer, &peerLen);
        if (in == INVALID_SOCK) {
            if (g_stop) {
                break;
            }
            int e = 0;
#ifdef _WIN32
            e = WSAGetLastError();
#else
            e = errno;
#endif
            (void)e;
            SleepMS(50);
            continue;
        }
        SetNoDelay(in);
        char peerIp[64] = "?";
        inet_ntop(AF_INET, &peer.sin_addr, peerIp, sizeof(peerIp));
        connId++;
        // make sure the hdc server is alive, then connect to it
        EnsureHdcServer(opt.hdcPath, opt.hdcPort, 10000);
        std::string err;
        socket_t up = ConnectHost("127.0.0.1", opt.hdcPort, SHORT_CONNECT_MS, &err);
        if (up == INVALID_SOCK) {
            LogError("connection #%lld from %s dropped: local hdc server unreachable (%s)",
                     connId, peerIp, err.c_str());
            CLOSE_SOCK(in);
            continue;
        }
        LogDebug("session #%lld from %s opened", connId, peerIp);
        StartRelay(in, up, "#" + std::to_string(connId) + "@" + peerIp);
    }
    CLOSE_SOCK(listener);
    LogInfo("hdcfwd server stopped");
    return 0;
}

// ---------------------------------------------------------------------------
// Client mode (on remote LAN machines)
// ---------------------------------------------------------------------------
void FreeLocalHdcPort(const Options &opt)
{
    if (!PortListening(opt.hdcPort)) {
        return;
    }
    std::string pidPath = PathJoin(TmpDir(), kPidFileName);
    std::string pidText = ReadTextFile(pidPath);
    int pid = ParsePidText(pidText);
    LogInfo("local port %u is occupied (pid file says '%s'), trying to free it",
            (unsigned)opt.hdcPort, pidText.c_str());

    if (pid == 0) {
        // pid "0" is our own sentinel written by a previous/running hdcfwd
        std::string marker = ReadTextFile(PathJoin(TmpDir(), kMarkerFileName));
        int mp = ParsePidText(marker);
        if (mp > 0 && ProcessAlive((uint32_t)mp)) {
            fprintf(stderr,
                "[error] another hdcfwd client seems to be running (pid %d).\n"
            "       stop it first, then retry.\n", mp);
            exit(1);
        }
        fprintf(stderr,
            "[error] port %u is occupied by an unknown process (stale hdcfwd marker).\n",
            (unsigned)opt.hdcPort);
        exit(1);
    }

    if (pid > 0 && ProcessAlive((uint32_t)pid)) {
        std::string img;
        bool known = GetProcessImageName((uint32_t)pid, img);
        if (known && ImageNameLooksLikeHdc(img)) {
            LogInfo("killing local hdc server pid %d (%s)", pid, img.c_str());
            KillPid((uint32_t)pid);
        } else if (known) {
            fprintf(stderr,
                "[error] port %u is held by pid %d (%s) which does not look like an hdc process.\n"
                "       refusing to kill it - free the port manually and retry.\n",
                (unsigned)opt.hdcPort, pid, img.c_str());
            exit(1);
        } else if (!opt.hdcPath.empty()) {
            // cannot inspect (maybe different user); try hdc kill as fallback
            LogInfo("cannot inspect pid %d, running '%s kill'", pid, opt.hdcPath.c_str());
            RunCommandHidden("\"" + opt.hdcPath + "\" kill");
        } else {
            fprintf(stderr,
                "[error] port %u is held by pid %d which cannot be inspected. "
                "Run 'hdc kill' manually.\n", (unsigned)opt.hdcPort, pid);
            exit(1);
        }
    } else if (pid <= 0 && !opt.hdcPath.empty()) {
        // pid file missing or stale but something is listening - try hdc kill
        LogInfo("pid file unusable ('%s'), running '%s kill'", pidText.c_str(),
                opt.hdcPath.c_str());
        RunCommandHidden("\"" + opt.hdcPath + "\" kill");
    }

    // wait for the port to actually become free
    for (int i = 0; i < 40; i++) {  // up to 8s
        if (!PortListening(opt.hdcPort)) {
            LogInfo("local port %u is free now", (unsigned)opt.hdcPort);
            return;
        }
        SleepMS(200);
    }
    fprintf(stderr, "[error] port %u still occupied after killing the local hdc server.\n",
            (unsigned)opt.hdcPort);
    exit(1);
}

int RunClient(Options &opt)
{
    LogInfo("hdcfwd client starting: 127.0.0.1:%u -> %s:%u", (unsigned)opt.hdcPort,
            opt.remoteHost.c_str(), (unsigned)opt.tunnelPort);

    FreeLocalHdcPort(opt);

    // take over the "hdc server presence" so local hdc clients connect instead
    // of trying to spawn their own (doomed) local server
    HdcPresenceLock presence;
    std::string reason;
    if (!presence.Acquire(reason)) {
        fprintf(stderr, "[error] cannot take over hdc server presence: %s\n", reason.c_str());
        return 1;
    }
    LogInfo("hdc server presence acquired (mutex/lock + pid file sentinel)");

    std::string bindIp = opt.expose ? "0.0.0.0" : "127.0.0.1";
    if (opt.expose) {
        LogWarn("--expose: tunnel is reachable from the whole LAN on port %u",
                (unsigned)opt.hdcPort);
    }
    socket_t listener = CreateListener(bindIp, opt.hdcPort);
    if (listener == INVALID_SOCK) {
        return 1;
    }
    g_listenerForWake = listener;

    // sanity check the remote side (do not die - server may come later)
    {
        std::string err;
        socket_t t = ConnectHost(opt.remoteHost, opt.tunnelPort, SHORT_CONNECT_MS, &err);
        if (t == INVALID_SOCK) {
            LogWarn("cannot reach %s:%u yet (%s); will keep trying per connection",
                    opt.remoteHost.c_str(), (unsigned)opt.tunnelPort, err.c_str());
        } else {
            LogInfo("remote forwarder %s:%u reachable", opt.remoteHost.c_str(),
                    (unsigned)opt.tunnelPort);
            CLOSE_SOCK(t);
        }
    }

    LogInfo("hdcfwd client ready on %s:%u - plain 'hdc ...' now uses the remote devices",
            bindIp.c_str(), (unsigned)opt.hdcPort);

    long long connId = 0;
    while (!g_stop) {
        struct sockaddr_in peer;
        socklen_t peerLen = sizeof(peer);
        socket_t in = accept(listener, (struct sockaddr *)&peer, &peerLen);
        if (in == INVALID_SOCK) {
            if (g_stop) {
                break;
            }
            SleepMS(50);
            continue;
        }
        SetNoDelay(in);
        connId++;
        char peerIp[64] = "?";
        inet_ntop(AF_INET, &peer.sin_addr, peerIp, sizeof(peerIp));
        std::string err;
        socket_t up = ConnectHost(opt.remoteHost, opt.tunnelPort, CONNECT_TIMEOUT_MS, &err);
        if (up == INVALID_SOCK) {
            LogError("connection #%lld from %s dropped: remote %s:%u unreachable (%s)", connId,
                     peerIp, opt.remoteHost.c_str(), (unsigned)opt.tunnelPort, err.c_str());
            CLOSE_SOCK(in);
            continue;
        }
        LogDebug("session #%lld from %s opened", connId, peerIp);
        StartRelay(in, up, "#" + std::to_string(connId) + "@" + peerIp);
    }
    CLOSE_SOCK(listener);
    presence.Release();
    LogInfo("hdcfwd client stopped");
    return 0;
}

// ---------------------------------------------------------------------------
// Signal handling for graceful stop
// ---------------------------------------------------------------------------
#ifdef _WIN32
BOOL WINAPI ConsoleHandler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT ||
        type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        g_stop = true;
        if (g_opt != nullptr) {
            // connect to the listener to unblock a blocked accept()
            socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s != INVALID_SOCK) {
                struct sockaddr_in a;
                memset(&a, 0, sizeof(a));
                a.sin_family = AF_INET;
                a.sin_port = htons(g_opt->serverMode ? g_opt->tunnelPort : g_opt->hdcPort);
                inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
                int rc = connect(s, (struct sockaddr *)&a, sizeof(a));
                (void)rc;
                closesocket(s);
            }
        }
        return TRUE;
    }
    return FALSE;
}
#else
void OnTermSignal(int)
{
    g_stop = true;
    if (g_listenerForWake != INVALID_SOCK) {
        shutdown(g_listenerForWake, SHUT_RDWR);
    }
}
#endif

}  // namespace

int main(int argc, char **argv)
{
    SocketInit sockInit;
    Options opt;

    // default hdc port honours the same env var as hdc itself
    if (const char *env = getenv(kEnvHdcPort)) {
        char *end = nullptr;
        unsigned long v = strtoul(env, &end, 10);
        if (end != env && *end == '\0' && v > 0 && v <= 65535) {
            opt.hdcPort = (uint16_t)v;
        }
    }

    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char *name) -> std::string {
            if (i + 1 >= argc) {
                fprintf(stderr, "[error] missing value for %s\n", name);
                exit(2);
            }
            return argv[++i];
        };
        if (a == "server" || a == "client") {
            positional.push_back(a);
        } else if (a == "-h" || a == "--help") {
            PrintUsage();
            return 0;
        } else if (a == "-v" || a == "--verbose") {
            g_verbose = true;
        } else if (a == "--expose") {
            opt.expose = true;
        } else if (a == "-t" || a == "--tunnel-port") {
            ParseArgUint16(next("--tunnel-port").c_str(), "--tunnel-port", opt.tunnelPort);
        } else if (a == "-p" || a == "--hdc-port") {
            ParseArgUint16(next("--hdc-port").c_str(), "--hdc-port", opt.hdcPort);
        } else if (a == "--keepalive") {
            opt.keepaliveSec = atoi(next("--keepalive").c_str());
        } else if (a == "--hdc") {
            opt.hdcPathArg = next("--hdc");
        } else if (a == "-c" || a == "--connect-key") {
            (void)next("--connect-key");  // reserved
        } else if (!a.empty() && a[0] == '-') {
            fprintf(stderr, "[error] unknown option: %s\n", a.c_str());
            PrintUsage();
            return 2;
        } else {
            positional.push_back(a);
        }
    }

    if (positional.empty()) {
        PrintUsage();
        return 2;
    }
    if (positional[0] == "server") {
        opt.serverMode = true;
        if (positional.size() > 1) {
            fprintf(stderr, "[error] 'server' mode takes no positional arguments\n");
            return 2;
        }
    } else if (positional[0] == "client") {
        if (positional.size() < 2) {
            fprintf(stderr, "[error] 'client' mode needs the server host, e.g. hdcfwd client 192.168.1.100\n");
            return 2;
        }
        opt.remoteHost = positional[1];
    } else {
        fprintf(stderr, "[error] unknown mode '%s' (use 'server' or 'client')\n", positional[0].c_str());
        return 2;
    }

    opt.hdcPath = ResolveHdcBinary(opt.hdcPathArg);
    g_opt = &opt;

#ifdef _WIN32
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = OnTermSignal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
#endif

    int rc;
    if (opt.serverMode) {
        rc = RunServer(opt);
    } else {
        rc = RunClient(opt);
    }
    return rc;
}
