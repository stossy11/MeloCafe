#include "Cafe/HW/Latte/Renderer/Metal/MetalInitialization.h"
#include "Cemu/Logging/CemuLogging.h"

#import <Foundation/Foundation.h>
#include <exception>

void LogMetalInitializationStage(const char*& stage, const char* nextStage)
{
    stage = nextStage;
    cemuLog_log(LogType::Force, "Metal initialization: {}", stage);
    
    cemuLog_waitForFlush();
}

void RunMetalInitialization(const char*& stage, const std::function<void()>& initialize)
{
    @try
    {
        try
        {
            initialize();
        }
        catch (const std::exception& error)
        {
            cemuLog_log(LogType::Force, "Metal initialization failed during {}: {}", stage, error.what());
            cemuLog_waitForFlush();
            throw;
        }
    }
    @catch (NSException* exception)
    {
        cemuLog_log(LogType::Force, "Metal initialization failed during {}: {}: {}", stage,
                    exception.name.UTF8String ?: "unknown exception",
                    exception.reason.UTF8String ?: "no reason provided");
        cemuLog_log(LogType::Force, "Metal exception call stack: {}",
                    exception.callStackSymbols.description.UTF8String ?: "unavailable");
        cemuLog_waitForFlush();
        @throw;
    }
}
