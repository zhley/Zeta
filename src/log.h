#pragma once

namespace Zeta {

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

} // namespace Zeta

#define ZETA_DEBUG(...) Zeta::Logger::getInstance().log(Zeta::LogLevel::DEBUG, __VA_ARGS__)
#define ZETA_INFO(...)  Zeta::Logger::getInstance().log(Zeta::LogLevel::INFO,  __VA_ARGS__)
#define ZETA_WARN(...)  Zeta::Logger::getInstance().log(Zeta::LogLevel::WARN,  __VA_ARGS__)
#define ZETA_ERROR(...) Zeta::Logger::getInstance().log(Zeta::LogLevel::ERROR, __VA_ARGS__)
