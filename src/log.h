#pragma once

enum class LogLevel {
    DEBUG = 0,
    INFO,
    WARN,
    ERROR
};

class Logger {
public:
    static Logger& getInstance();
    void setLevel(LogLevel level);
    void log(LogLevel level, const char* format, ...);

private:
    Logger();
    ~Logger() = default;

    const char* getLevelColor(LogLevel level) const;
    const char* getLevelString(LogLevel level) const;

    LogLevel currentLevel;
};

#define ZETA_DEBUG(...) Logger::getInstance().log(LogLevel::DEBUG, __VA_ARGS__)
#define ZETA_INFO(...)  Logger::getInstance().log(LogLevel::INFO,  __VA_ARGS__)
#define ZETA_WARN(...)  Logger::getInstance().log(LogLevel::WARN,  __VA_ARGS__)
#define ZETA_ERROR(...) Logger::getInstance().log(LogLevel::ERROR, __VA_ARGS__)
