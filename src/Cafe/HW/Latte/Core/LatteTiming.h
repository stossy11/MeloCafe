#pragma once

#include <chrono>

void LatteTiming_setCustomVsyncFrequency(sint32 frequency);
void LatteTiming_disableCustomVsyncFrequency();
bool LatteTiming_getCustomVsyncFrequency(sint32& customFrequency);

void LatteTiming_EnableHostDrivenVSync();

std::chrono::steady_clock::time_point LatteTiming_GetNextVsyncDeadline();
