#include "logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

std::ofstream Logger::logfile_;
std::mutex Logger::mutex_;

bool Logger::initialize(const std::string& filename)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        logfile_.open(filename, std::ios::out | std::ios::app);
        if (!logfile_.is_open())
        {
            std::cerr << "Failed to open log file: "
                      << filename << std::endl;
            return false;
        }
    } // lock released here, before info() takes mutex_ itself
    info("========== Logger started ==========");
    return true;
}


void Logger::shutdown()
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (logfile_.is_open())
    {
        logfile_ << timestamp()
                 << " [INFO] Logger shutting down"
                 << std::endl;

        logfile_.close();
    }
}

void Logger::log(Level level,
                 const std::string& message)
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::string line =
        timestamp() +
        " [" +
        std::string(levelString(level)) +
        "] " +
        message;

    std::cout << line << std::endl;

    if (logfile_.is_open())
    {
        logfile_ << line << std::endl;
        logfile_.flush();
    }
}

void Logger::debug(const std::string& message)
{
    log(Level::Debug, message);
}

void Logger::info(const std::string& message)
{
    log(Level::Info, message);
}

void Logger::warning(const std::string& message)
{
    log(Level::Warning, message);
}

void Logger::error(const std::string& message)
{
    log(Level::Error, message);
}

std::string Logger::timestamp()
{
    auto now =
        std::chrono::system_clock::now();

    auto time =
        std::chrono::system_clock::to_time_t(now);

    std::tm tm{};

#ifdef _WIN32
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif

    std::ostringstream ss;

    ss << std::put_time(&tm,
                        "%Y-%m-%d %H:%M:%S");

    return ss.str();
}

const char* Logger::levelString(Level level)
{
    switch (level)
    {
        case Level::Debug:
            return "DEBUG";

        case Level::Info:
            return "INFO";

        case Level::Warning:
            return "WARNING";

        case Level::Error:
            return "ERROR";
    }

    return "UNKNOWN";
}
