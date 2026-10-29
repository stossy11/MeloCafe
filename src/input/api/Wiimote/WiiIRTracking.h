#pragma once

#include <cmath>
#include <limits>
#include <glm/glm.hpp>

namespace WiiIRTracking
{
// Rotate camera coordinates in pixel space: the sensor is not square. Keep the
// original x/y for both components so a quarter turn cannot collapse an axis.
inline glm::vec2 Rotate(glm::vec2 position, float roll)
{
    const glm::vec2 extent{1023.0f, 768.0f};
    const auto centered = (position - 0.5f) * extent;
    const float sine = std::sin(roll), cosine = std::cos(roll);
    return glm::vec2{centered.x * cosine - centered.y * sine,
                     centered.x * sine + centered.y * cosine} / extent + 0.5f;
}

// Camera slots are detection slots, not permanent identities for the two ends
// of the sensor bar. Acquire a pair from any slots and match it geometrically.
template<class Camera> void Track(Camera& ir)
{
    using Visibility = decltype(ir.m_positionVisibility);
    ir.m_prev_position = ir.position;
    const bool anchored = ir.distance > 0;
    float best = std::numeric_limits<float>::max();
    int first = -1, second = -1;
    for (int i = 0; i < 4; ++i)
    {
        if (!ir.dots[i].visible) continue;
        for (int j = i + 1; j < 4; ++j)
        {
            if (!ir.dots[j].visible) continue;
            const float distance = glm::distance(ir.dots[i].pos, ir.dots[j].pos);
            if (distance <= 0) continue;
            const auto midpoint = (ir.dots[i].pos + ir.dots[j].pos) * 0.5f;
            const float score = anchored ? std::abs(distance - ir.distance) + glm::distance(midpoint, ir.middle) : 0;
            if (score < best) { best = score; first = i; second = j; }
        }
    }
    if (first >= 0)
    {
        ir.indices = {first, second};
        ir.prev_dots[first] = ir.dots[first];
        ir.prev_dots[second] = ir.dots[second];
        ir.middle = ir.position = (ir.dots[first].pos + ir.dots[second].pos) * 0.5f;
        ir.distance = glm::distance(ir.dots[first].pos, ir.dots[second].pos);
        ir.m_positionVisibility = Visibility::FULL;
        if (!anchored) ir.m_prev_position = ir.position;
        return;
    }
    if (anchored)
    {
        // One visible end can maintain a previously acquired midpoint, even if
        // the camera has reassigned its slot. Never invent a midpoint at (0,0).
        best = std::numeric_limits<float>::max();
        glm::vec2 delta{};
        for (const auto& dot : ir.dots)
        {
            if (!dot.visible) continue;
            for (int index : {ir.indices.first, ir.indices.second})
            {
                const auto change = dot.pos - ir.prev_dots[index].pos;
                const float distance = glm::length(change);
                if (distance < best) { best = distance; delta = change; }
            }
        }
        if (best < std::numeric_limits<float>::max())
        {
            ir.position = ir.middle + delta;
            ir.m_positionVisibility = Visibility::PARTIAL;
            return;
        }
    }
    ir.distance = 0;
    ir.m_positionVisibility = Visibility::NONE;
}
}
