#pragma once
#include <glm/glm.hpp>
#include <numbers>

namespace WiiMotionCoordinates
{
// Wii sensor axes: X across the remote, Y toward its nose, Z out of its buttons.
// The fusion filter's neutral pose has +Y up. Apply the SAME proper rotation to
// gyro and upward acceleration; reflecting one makes gravity fight real motion.
inline glm::vec3 ToFusion(glm::vec3 sensor) { return {sensor.x, sensor.z, -sensor.y}; }

// Match the derivatives of MotionSample's attitude basis (pitch, yaw, roll).
inline glm::vec3 AngularVelocity(glm::vec3 sensor)
{
    return glm::vec3{sensor.x, sensor.z, -sensor.y} / (2 * std::numbers::pi_v<float>);
}

inline glm::vec2 Down(glm::vec3 acceleration)
{
    const glm::vec2 projected{acceleration.x, -acceleration.z};
    const auto length = glm::length(projected);
    return length > 0.0001f ? projected / length : glm::vec2{};
}
}
