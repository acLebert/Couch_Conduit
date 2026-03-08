#pragma once
// Couch Conduit — Logging utilities

#include <cstdio>
#include <cstdarg>
#include <chrono>

namespace cc::log {

enum class Level { Trace, Debug, Info, Warn, Error, Fatal };

inline Level g_minLevel = Level::Info;

inline const char* LevelStr(Level l) {
    switch (l) {
        case Level::Trace: return "TRC";
        case Level::Debug: return "DBG";
        case Level::Info:  return "INF";
        case Level::Warn:  return "WRN";
        case Level::Error: return "ERR";
        case Level::Fatal: return "FTL";
    }
    return "???";
}

inline void Log(Level level, const char* file, int line, const char* fmt, ...) {
    if (level < g_minLevel) return;

    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    auto t = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
    localtime_s(&tm_buf, &t);

    fprintf(stderr, "%02d:%02d:%02d.%03d [%s] ",
            tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, (int)ms,
            LevelStr(level));

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    // Strip path to just filename
    const char* fname = file;
    for (const char* p = file; *p; ++p) {
        if (*p == '\\' || *p == '/') fname = p + 1;
    }
    fprintf(stderr, "  (%s:%d)\n", fname, line);
}

}  // namespace cc::log

#define CC_LOG(level, fmt, ...) \
    cc::log::Log(cc::log::Level::level, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define CC_TRACE(fmt, ...) CC_LOG(Trace, fmt, ##__VA_ARGS__)
#define CC_DEBUG(fmt, ...) CC_LOG(Debug, fmt, ##__VA_ARGS__)
#define CC_INFO(fmt, ...)  CC_LOG(Info,  fmt, ##__VA_ARGS__)
#define CC_WARN(fmt, ...)  CC_LOG(Warn,  fmt, ##__VA_ARGS__)
#define CC_ERROR(fmt, ...) CC_LOG(Error, fmt, ##__VA_ARGS__)
#define CC_FATAL(fmt, ...) CC_LOG(Fatal, fmt, ##__VA_ARGS__)
