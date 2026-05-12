#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <vector>


#define COLOR_RESET   "\033[0m"
#define COLOR_DEBUG   "\033[36m"
#define COLOR_INFO    "\033[32m"
#define COLOR_WARN    "\033[33m"
#define COLOR_ERROR   "\033[31m"

namespace Zeta{

Logger::Logger() : currentLevel(LogLevel::DEBUG) {}

Logger& Logger::getInstance() {
    static Logger instance;
    return instance;
}

void Logger::setLevel(LogLevel level) {
    currentLevel = level;
}

const char* Logger::getLevelColor(LogLevel level) const {
    switch (level) {
        case LogLevel::DEBUG: return COLOR_DEBUG;
        case LogLevel::INFO:  return COLOR_INFO;
        case LogLevel::WARN:  return COLOR_WARN;
        case LogLevel::ERROR: return COLOR_ERROR;
        default:              return COLOR_RESET;
    }
}

const char* Logger::getLevelString(LogLevel level) const {
    switch (level) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::ERROR: return "ERROR";
        default:              return "LOG";
    }
}

void Logger::log(LogLevel level, const char* format, ...) {
    if (level < currentLevel) {
        return;
    }

    fprintf(stderr, "%s[%s]%s ",
            getLevelColor(level),
            getLevelString(level),
            COLOR_RESET);

    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);

    fprintf(stderr, "\n");
    fflush(stderr);
}

} // namespace Zeta