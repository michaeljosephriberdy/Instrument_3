#include "engine.h"
#include "logger.h"

#include <iostream>
#include <exception>

int main()
{
    Logger::initialize("instrument.log");

    int result = 0;

    try
    {
        Engine engine;

        if (!engine.initialize())
        {
            std::cerr << "Engine initialization failed.\n";
            result = 1;
        }
        else
        {
            engine.run();
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "Fatal error: " << e.what() << "\n";
        result = 1;
    }
    catch (...)
    {
        std::cerr << "Unknown fatal error.\n";
        result = 1;
    }

    Logger::shutdown();

    return result;
}
