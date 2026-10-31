#pragma once

#include <functional>

// The stage reference tracks progress inside the initialization callback.
void RunMetalInitialization(const char*& stage, const std::function<void()>& initialize);
void LogMetalInitializationStage(const char*& stage, const char* nextStage);
