#pragma once

#include <format>
#include <string>

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
    void log(LogLevel level, const std::string& msg);

private:
    Logger();
    ~Logger() = default;

    const char* getLevelColor(LogLevel level) const;
    const char* getLevelString(LogLevel level) const;

    LogLevel currentLevel;
};

} // namespace Zeta

#define ZETA_DEBUG(msg, ...) Zeta::Logger::getInstance().log(Zeta::LogLevel::DEBUG, std::format(msg, ##__VA_ARGS__))
#define ZETA_INFO(msg, ...)  Zeta::Logger::getInstance().log(Zeta::LogLevel::INFO,  std::format(msg, ##__VA_ARGS__))
#define ZETA_WARN(msg, ...)  Zeta::Logger::getInstance().log(Zeta::LogLevel::WARN,  std::format(msg, ##__VA_ARGS__))
#define ZETA_ERROR(msg, ...) Zeta::Logger::getInstance().log(Zeta::LogLevel::ERROR, std::format(msg, ##__VA_ARGS__))
