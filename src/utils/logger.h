#pragma once

#include <string>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <mutex>
#include <sstream>

namespace bachuan {

enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error
};

class Logger {
public:
    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    void set_level(LogLevel level) { level_ = level; }
    LogLevel level() const { return level_; }

    template<typename... Args>
    void debug(const char* fmt, Args&&... args) {
        log(LogLevel::Debug, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void info(const char* fmt, Args&&... args) {
        log(LogLevel::Info, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void warning(const char* fmt, Args&&... args) {
        log(LogLevel::Warning, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void error(const char* fmt, Args&&... args) {
        log(LogLevel::Error, fmt, std::forward<Args>(args)...);
    }

    static std::string bytes_to_hex(const uint8_t* data, size_t len, size_t max_len = 32) {
        std::ostringstream oss;
        size_t display_len = std::min(len, max_len);
        for (size_t i = 0; i < display_len; ++i) {
            oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(data[i]);
            if (i < display_len - 1) oss << " ";
        }
        if (len > max_len) {
            oss << " ... (" << len << " bytes total)";
        }
        return oss.str();
    }

private:
    Logger() : level_(LogLevel::Info) {}

    template<typename... Args>
    void log(LogLevel level, const char* fmt, Args&&... args) {
        if (level < level_) return;

        std::lock_guard<std::mutex> lock(mutex_);

        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        std::cerr << std::put_time(std::localtime(&time), "%H:%M:%S")
                  << "." << std::setfill('0') << std::setw(3) << ms.count()
                  << " [" << level_str(level) << "] ";

        print_formatted(fmt, std::forward<Args>(args)...);
        std::cerr << std::endl;
    }

    static const char* level_str(LogLevel level) {
        switch (level) {
            case LogLevel::Debug:   return "DEBUG";
            case LogLevel::Info:    return "INFO ";
            case LogLevel::Warning: return "WARN ";
            case LogLevel::Error:   return "ERROR";
            default:                return "?????";
        }
    }

    void print_formatted(const char* fmt) {
        std::cerr << fmt;
    }

    template<typename T, typename... Args>
    void print_formatted(const char* fmt, T&& value, Args&&... args) {
        while (*fmt) {
            if (*fmt == '{' && *(fmt + 1) == '}') {
                std::cerr << std::forward<T>(value);
                print_formatted(fmt + 2, std::forward<Args>(args)...);
                return;
            }
            std::cerr << *fmt++;
        }
    }

    LogLevel level_;
    std::mutex mutex_;
};

#define LOG_DEBUG(...) bachuan::Logger::instance().debug(__VA_ARGS__)
#define LOG_INFO(...)  bachuan::Logger::instance().info(__VA_ARGS__)
#define LOG_WARN(...)  bachuan::Logger::instance().warning(__VA_ARGS__)
#define LOG_ERROR(...) bachuan::Logger::instance().error(__VA_ARGS__)

} // namespace bachuan
