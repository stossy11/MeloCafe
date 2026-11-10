#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

// Source intervals survive BLE batching and the native input queue. The ESP32
// clock wraps every ~72 minutes; host and source clocks must never be subtracted.
class WiiMotionSampleClock
{
public:
    using Clock = std::chrono::steady_clock;
    float elapsed(std::optional<uint32_t> source, Clock::time_point host)
    {
        float seconds = 0;
        if (source && m_source)
        {
            const uint32_t delta = *source - *m_source;
            if (delta > 0x80000000u) return 0; // stale/out-of-order input
            seconds = float(delta) / 1000000;
        }
        else if (!source && !m_source && m_host)
            seconds = std::chrono::duration<float>(host - *m_host).count();
        m_source = source;
        m_host = host;
        // A lost connection or app suspension is not continuous measured motion.
        return seconds >= 0 && seconds <= .1f ? seconds : 0;
    }
private:
    std::optional<uint32_t> m_source;
    std::optional<Clock::time_point> m_host;
};
