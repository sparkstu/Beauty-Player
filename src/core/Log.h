#pragma once
#include <chrono>
#include <cstdarg>
#include <cstdio>

// Milliseconds since the process started; keeps log lines orderable even when
// several threads write concurrently.
inline double logElapsedMs() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

inline void logPrint(const char* level, const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::printf("[%9.1f] [%s] %s\n", logElapsedMs(), level, buf);
    std::fflush(stdout);
}

#define LOGI(...) logPrint("I", __VA_ARGS__)
#define LOGW(...) logPrint("W", __VA_ARGS__)
#define LOGE(...) logPrint("E", __VA_ARGS__)
