#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// A physical camera heading, not a screen cursor or a new gyro zero. The Wii
// camera's approximate 42-degree horizontal FOV is also used by Dolphin:
// Source/Core/Core/HW/WiimoteEmu/Camera.h (CAMERA_FOV_X).
namespace WiiIRHeading
{
template<class Camera> std::optional<float> Observe(const Camera& camera)
{
    if (camera.m_positionVisibility != decltype(camera.m_positionVisibility)::FULL) return {};
    glm::vec2 points[2];
    int count = 0;
    for (const auto& dot : camera.dots)
    {
        if (!dot.visible) continue;
        if (count == 2 || !std::isfinite(dot.pos.x) || !std::isfinite(dot.pos.y)) return {};
        points[count++] = dot.pos;
    }
    if (count != 2) return {};
    // These positions have already been de-rolled in pixel space. Reject tiny
    // pairs or pairs that do not resemble the horizontal sensor bar.
    const auto difference = (points[1] - points[0]) * glm::vec2{1023, 768};
    if (std::abs(difference.x) < 8 || std::abs(difference.y) > std::abs(difference.x) * .5774f) return {};
    const auto midpoint = (points[0] + points[1]) * .5f;
    if (midpoint.x < 0 || midpoint.x > 1 || midpoint.y < 0 || midpoint.y > 1) return {};
    // Camera x has already been mirrored for screen pointing. Positive physical
    // KPAD yaw turns the remote left, so rightward pointing is negative yaw.
    return -std::atan((midpoint.x * 2 - 1) * std::tan(21.0f * 3.14159265359f / 180));
}

class Correction
{
public:
    bool enabled = true;
    float weight = .05f; // KPAD's documented default DPD correction weight.

    bool setBase(const glm::mat3& base)
    {
        for (int i = 0; i < 3; ++i)
        {
            if (!std::isfinite(glm::dot(base[i],base[i])) || std::abs(glm::dot(base[i],base[i]) - 1) > .01f) return false;
            for (int j = i + 1; j < 3; ++j) if (std::abs(glm::dot(base[i],base[j])) > .01f) return false;
        }
        if (glm::determinant(base) < .99f) return false;
        if (glm::length(base[0]-m_base[0]) + glm::length(base[1]-m_base[1]) + glm::length(base[2]-m_base[2]) < .00001f) return true;
        m_base = base;
        reset();
        return true;
    }

    void reset() { m_yaw = 0; m_lastSequence = 0; m_lastTime = 0; m_acquired = 0; }

    glm::mat3 apply(const glm::mat3& direction, std::optional<float> observed,
                    uint64_t sequence, double sampleTime, double now)
    {
        auto corrected = rotation() * direction;
        if (sequence == m_lastSequence) return corrected;
        const double elapsed = sampleTime - m_lastTime;
        m_lastTime = sampleTime;
        m_lastSequence = sequence;
        if (!enabled || weight <= 0 || !observed || !std::isfinite(*observed) ||
            now - sampleTime < 0 || now - sampleTime > .1 || elapsed < 0 || elapsed > .15)
        {
            m_acquired = 0;
            return corrected;
        }
        m_acquired = std::min(m_acquired + 1, 3u);
        if (m_acquired < 3) return corrected;
        const auto forward = glm::transpose(m_base) * corrected[2];
        if (forward.x * forward.x + forward.z * forward.z < .04f) return corrected;
        const float heading = std::atan2(forward.x, forward.z);
        const float error = std::remainder(*observed - heading, 2 * 3.14159265359f);
        const float dt = float(std::min(elapsed, .05));
        const float alpha = 1 - std::pow(1 - std::clamp(weight, 0.0f, 1.0f), dt * 60);
        // Smooth reacquisition without snapping or turning every newly seen
        // off-center position into the middle of the screen.
        const float limit = dt * 3.14159265359f / 3; // at most 60 degrees/sec
        m_yaw = std::remainder(m_yaw + std::clamp(error * alpha, -limit, limit), 2 * 3.14159265359f);
        return rotation() * direction;
    }

private:
    glm::mat3 rotation() const
    {
        return m_base * glm::mat3(glm::rotate(glm::mat4{1}, m_yaw, glm::vec3{0,1,0})) * glm::transpose(m_base);
    }
    glm::mat3 m_base{1};
    float m_yaw = 0;
    uint64_t m_lastSequence = 0;
    double m_lastTime = 0;
    unsigned m_acquired = 0;
};
}
