#pragma once

#include <fstream>
#include <mutex>
#include <string>

class Logger
{
public:

    enum class Level
    {
        Debug,
        Info,
        Warning,
        Error
    };

    static bool initialize(const std::string& filename);

    static void shutdown();

    static void log(Level level,
                    const std::string& message);

    static void debug(const std::string& message);

    static void info(const std::string& message);

    static void warning(const std::string& message);

    static void error(const std::string& message);

private:

    static std::ofstream logfile_;

    static std::mutex mutex_;

    static std::string timestamp();

    static const char* levelString(Level level);
};
