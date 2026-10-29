#include "input/api/Wiimote/WiiExtensionProtocol.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

// Protocol references (independent implementation):
// https://wiibrew.org/wiki/Wiimote/Extension_Controllers/Wii_Motion_Plus
// https://wiibrew.org/wiki/Wiimote/Extension_Controllers/Nunchuck
namespace
{
uint16_t Big16(const uint8_t* p) { return uint16_t(p[0]) << 8 | p[1]; }
float Axis(float value, float minimum, float center, float maximum)
{
    const auto span = value < center ? center - minimum : maximum - center;
    return span > 0 ? std::clamp((value - center) / span, -1.0f, 1.0f) : 0.0f;
}
uint32_t CalibrationCRC(std::span<const uint8_t> bytes)
{
    uint32_t crc = ~uint32_t{0};
    for (size_t i = 0; i < 32; ++i)
    {
        if (i == 14 || i == 15 || i == 30 || i == 31) continue;
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
bool IsMotionID(std::span<const uint8_t> id, uint8_t bus)
{
    return id.size() >= 6 && id[2] == bus && id[3] == 0x20 && id[5] == 5;
}
}

void WiiExtensionProtocol::WriteMemory(uint32_t address, uint8_t value)
{
    std::vector<uint8_t> packet(22);
    packet[0] = 0x16; packet[1] = 4;
    packet[2] = address >> 16; packet[3] = address >> 8; packet[4] = address;
    packet[5] = 1; packet[6] = value;
    m_commands.push_back(std::move(packet));
}

void WiiExtensionProtocol::ReadMemory(uint32_t address, uint16_t size, Phase phase)
{
    m_phase = phase;
    m_read = Read{address, size};
    m_deadline = m_now + 2000;
    m_commands.push_back({0x17, 4, uint8_t(address >> 16), uint8_t(address >> 8),
        uint8_t(address), uint8_t(size >> 8), uint8_t(size)});
}

void WiiExtensionProtocol::Start(bool deactivate)
{
    m_read.reset();
    m_extension = Extension::None;
    m_nunchuck = {}; m_classic = {}; m_motion = {};
    m_haveMotionPlus = false;
    m_gyroBias = {}; StartGyroCalibration();
    m_extensionInitialized = false; m_motionInitialized = false;
    m_pollOnly = false;
    // A new extension must never inherit calibration from the previous one.
    m_accZero = {512, 512, 512}; m_accOneG = {716, 716, 716};
    m_stickMin = {35, 35}; m_stickCenter = {128, 128}; m_stickMax = {225, 225};
    m_classicLeftMin = {0, 0}; m_classicLeftCenter = {32, 32}; m_classicLeftMax = {63, 63};
    m_classicRightMin = {0, 0}; m_classicRightCenter = {16, 16}; m_classicRightMax = {31, 31};
    if (deactivate)
    {
        WriteMemory(0xa400f0, 0x55);
        m_phase = Phase::WaitRestart;
        m_deadline = m_now + 400;
    }
    else ReadMemory(0xa600fa, 6, Phase::ProbeMotionPlus);
}

void WiiExtensionProtocol::Status(bool attached, uint64_t now)
{
    m_now = now;
    const bool changed = m_statusAttached != attached;
    m_statusAttached = attached;
    if (m_phase == Phase::Dormant) Start(false);
    else if (m_phase == Phase::Running && changed)
        Start(m_motion.active);
    // Initialization itself generates detach/attach status reports. Their flags
    // are remembered, but they must not interrupt the in-flight register read.
}

void WiiExtensionProtocol::Tick(uint64_t now)
{
    m_now = now;
    if (m_read && now >= m_deadline)
    {
        auto read = *m_read;
        m_read.reset();
        ReadCompleted(read, true);
    }
    else if (!m_read && now >= m_deadline)
    {
        switch (m_phase)
        {
        case Phase::WaitRestart: ReadMemory(0xa600fa, 6, Phase::ProbeMotionPlus); break;
        case Phase::WaitMotionInit: ReadMemory(0xa60020, 32, Phase::ReadMotionCalibration); break;
        case Phase::WaitExtensionInit:
            WriteMemory(0xa400fb, 0);
            m_phase = Phase::WaitExtensionDecrypt;
            m_deadline = m_now + 200;
            break;
        case Phase::WaitExtensionDecrypt: ReadMemory(0xa400fa, 6, Phase::ReadExtensionID); break;
        case Phase::WaitActivation: ReadMemory(0xa400fa, 6, Phase::VerifyActivation); break;
        default: break;
        }
    }
    if (m_phase == Phase::Running && !m_motion.active && now >= m_nextProbe)
    {
        m_pollOnly = true;
        ReadMemory(0xa600fa, 6, Phase::ProbeMotionPlus);
    }
}

bool WiiExtensionProtocol::ReadReply(std::span<const uint8_t> report, uint64_t now)
{
    if (report.size() < 6 || report[0] != 0x21 || !m_read) return false;
    m_now = now;
    const uint16_t address = Big16(report.data() + 4);
    const uint16_t start = m_read->address;
    const auto count = size_t((report[3] >> 4) + 1);
    if (address < start || size_t(address - start) >= m_read->size) return false;
    const bool failed = (report[3] & 15) != 0;
    if (!failed)
    {
        const auto offset = size_t(address - start);
        if (report.size() < 6 + count || offset + count > m_read->size) return false;
        for (size_t i = 0; i < count; ++i)
        {
            m_read->bytes[offset + i] = report[6 + i];
            m_read->received[offset + i] = true;
        }
        if (!std::all_of(m_read->received.begin(), m_read->received.begin() + m_read->size,
            [](bool received) { return received; })) return true;
    }
    auto read = *m_read;
    m_read.reset();
    ReadCompleted(read, failed);
    return true;
}

void WiiExtensionProtocol::InitializeExtension()
{
    m_extensionInitialized = true;
    WriteMemory(0xa400f0, 0x55);
    m_phase = Phase::WaitExtensionInit;
    m_deadline = m_now + 400;
}

void WiiExtensionProtocol::ReadCompleted(const Read& read, bool failed)
{
    const auto bytes = std::span(read.bytes).first(read.size);
    switch (m_phase)
    {
    case Phase::ProbeMotionPlus:
        if (!failed && (IsMotionID(bytes, 0xa6) || IsMotionID(bytes, 0xa4)))
        {
            m_pollOnly = false;
            m_haveMotionPlus = true;
            m_motion.integrated = bytes[0] == 1;
            m_extension = Extension::None;
            m_nunchuck = {}; m_classic = {};
            InitializeExtension();
        }
        else if (m_pollOnly) Run();
        else ReadMemory(0xa400fa, 6, Phase::ReadExtensionID);
        break;
    case Phase::ReadMotionCalibration:
        m_motionInitialized = true;
        if (!failed) SetMotionCalibration(bytes);
        FinishExtension();
        break;
    case Phase::ReadExtensionID:
        if (!failed && IsMotionID(bytes, 0xa4))
        {
            // Reconnecting while the remote is already in active MotionPlus mode.
            Start(true);
        }
        else if (!m_extensionInitialized && m_statusAttached)
            InitializeExtension();
        else if (!failed && bytes[2] == 0xa4 && bytes[3] == 0x20 && bytes[4] == 0 && bytes[5] == 0)
        {
            m_extension = Extension::Nunchuck;
            ReadMemory(0xa40020, 16, Phase::ReadExtensionCalibration);
        }
        else if (!failed && bytes[2] == 0xa4 && bytes[3] == 0x20 && bytes[4] == 1 && bytes[5] == 1)
        {
            m_extension = Extension::Classic;
            ReadMemory(0xa40020, 16, Phase::ReadExtensionCalibration);
        }
        else FinishExtension();
        break;
    case Phase::ReadExtensionCalibration:
        if (!failed)
        {
            if (m_extension == Extension::Nunchuck) SetNunchuckCalibration(bytes);
            if (m_extension == Extension::Classic) SetClassicCalibration(bytes);
        }
        FinishExtension();
        break;
    case Phase::VerifyActivation:
        if (!failed && IsMotionID(bytes, 0xa4) && bytes[4] == m_activationMode)
        {
            m_motion.active = true;
            m_motion.extensionConnected = m_extension != Extension::None;
            Run();
        }
        else
        {
            // Back off after unplug or a failed activation; no tight retry loop.
            m_motion = {}; m_extension = Extension::None;
            m_haveMotionPlus = false; m_motionInitialized = false; m_extensionInitialized = false;
            m_phase = Phase::WaitRestart;
            m_deadline = m_now + 1000;
        }
        break;
    default: break;
    }
}

void WiiExtensionProtocol::FinishExtension()
{
    if (!m_haveMotionPlus) { Run(); return; }
    if (!m_motionInitialized)
    {
        WriteMemory(0xa600f0, 0x55);
        m_phase = Phase::WaitMotionInit;
        m_deadline = m_now + 400;
        return;
    }
    m_activationMode = m_extension == Extension::Nunchuck ? 5 : m_extension == Extension::Classic ? 7 : 4;
    WriteMemory(0xa600fe, m_activationMode);
    m_phase = Phase::WaitActivation;
    m_deadline = m_now + 400;
}

void WiiExtensionProtocol::Run()
{
    m_phase = Phase::Running;
    m_pollOnly = false;
    m_nextProbe = m_now + 8000;
}

std::vector<std::vector<uint8_t>> WiiExtensionProtocol::TakeCommands()
{
    return std::exchange(m_commands, {});
}

void WiiExtensionProtocol::SetMotionCalibration(std::span<const uint8_t> bytes)
{
    const uint32_t storedCRC = uint32_t(Big16(bytes.data() + 14)) << 16 | Big16(bytes.data() + 30);
    if (CalibrationCRC(bytes) != storedCRC) return;
    for (size_t block : {size_t{0}, size_t{16}})
    {
        if (!bytes[block + 12]) return;
        for (size_t axis = 0; axis < 3; ++axis)
            if (std::abs(int(Big16(bytes.data() + block + axis * 2 + 6)) -
                int(Big16(bytes.data() + block + axis * 2))) < 256) return;
    }
    std::copy(bytes.begin(), bytes.end(), m_motionCalibration.begin());
    m_motion.factoryCalibrated = true;
}

void WiiExtensionProtocol::SetNunchuckCalibration(std::span<const uint8_t> b)
{
    for (size_t axis = 0; axis < 3; ++axis)
    {
        const auto shift = 4 - axis * 2;
        const float zero = (uint16_t(b[axis]) << 2) | ((b[3] >> shift) & 3);
        const float oneG = (uint16_t(b[4 + axis]) << 2) | ((b[7] >> shift) & 3);
        if (oneG > zero && oneG - zero >= 16) { m_accZero[axis] = zero; m_accOneG[axis] = oneG; }
    }
    for (size_t axis = 0; axis < 2; ++axis)
    {
        const auto o = 8 + axis * 3;
        if (b[o + 1] < b[o + 2] && b[o + 2] < b[o])
        { m_stickMax[axis] = b[o]; m_stickMin[axis] = b[o + 1]; m_stickCenter[axis] = b[o + 2]; }
    }
}

void WiiExtensionProtocol::SetClassicCalibration(std::span<const uint8_t> b)
{
    for (size_t axis = 0; axis < 2; ++axis)
    {
        auto set = [&](size_t o, float divisor, auto& minimum, auto& center, auto& maximum) {
            if (b[o + 1] < b[o + 2] && b[o + 2] < b[o])
            { maximum[axis] = b[o] / divisor; minimum[axis] = b[o + 1] / divisor; center[axis] = b[o + 2] / divisor; }
        };
        set(axis * 3, 4, m_classicLeftMin, m_classicLeftCenter, m_classicLeftMax);
        set(6 + axis * 3, 8, m_classicRightMin, m_classicRightCenter, m_classicRightMax);
    }
}

void WiiExtensionProtocol::Data(std::span<const uint8_t> bytes, uint64_t now)
{
    if (bytes.size() < 6 || (m_phase != Phase::Running && !m_pollOnly)) return;
    m_now = now;
    if (m_motion.active)
    {
        // Bit zero is reserved in both gyro and passthrough frames. During
        // activation the remote can send unavailable (all-FF/all-zero) data;
        // it must not be interpreted as an extension being plugged/unplugged.
        if ((bytes[5] & 1) || std::all_of(bytes.begin(), bytes.begin() + 6,
            [](uint8_t value) { return value == 0; })) return;
        const bool attached = (bytes[4] & 1) != 0;
        if (attached != m_motion.extensionConnected)
        {
            Start(true);
            return;
        }
        m_motion.lastReportWasGyro = (bytes[5] & 2) != 0;
        if (m_motion.lastReportWasGyro) { DecodeMotionPlus(bytes); return; }
    }
    if (m_extension == Extension::Nunchuck) DecodeNunchuck(bytes, m_motion.active);
    if (m_extension == Extension::Classic) DecodeClassic(bytes, m_motion.active);
}

void WiiExtensionProtocol::DecodeMotionPlus(std::span<const uint8_t> b)
{
    for (size_t i = 0; i < 3; ++i) m_motion.raw[i] = b[i] | uint16_t(b[i + 3] & 0xfc) << 6;
    m_motion.slow = {bool(b[3] & 2), bool(b[4] & 2), bool(b[3] & 1)};
    std::array<float, 3> degrees{}; // yaw, roll, pitch
    for (size_t i = 0; i < 3; ++i)
    {
        float zero = 32768, scale = i == 1 ? 50176 : 15360;
        float speed = m_motion.slow[i] ? 270 : 1200;
        if (m_motion.factoryCalibrated)
        {
            const size_t block = m_motion.slow[i] ? 16 : 0;
            zero = Big16(m_motionCalibration.data() + block + i * 2);
            scale = Big16(m_motionCalibration.data() + block + i * 2 + 6);
            speed = m_motionCalibration[block + 12] * 6.0f;
        }
        degrees[i] = (float(m_motion.raw[i]) * 4 - zero) * speed / (scale - zero);
    }
    constexpr float rad = std::numbers::pi_v<float> / 180;
    // Calibration scales for pitch/yaw have reversed signs on official units.
    m_motion.angularVelocity = {-degrees[2] * rad, degrees[1] * rad, -degrees[0] * rad};
    ++m_motion.sampleSequence;
    if (m_calibrating)
    {
        bool stable = std::all_of(m_motion.slow.begin(), m_motion.slow.end(), [](bool slow) { return slow; });
        for (size_t axis = 0; axis < 3; ++axis)
        {
            // Low variance alone accepts constant rotation as bias. Allow the
            // measured factory residual (~0.28 rad/s on a real Remote Plus),
            // but reject faster movement and verify gravity stays still below.
            const float previousBias = m_motion.zeroCalibrated ? m_gyroBias[axis] : 0;
            if (std::abs(m_motion.angularVelocity[axis] - previousBias) > 0.35f) stable = false;
            if (m_stationarySamples && std::abs(m_motion.angularVelocity[axis] - m_gyroMean[axis]) > 0.08f) stable = false;
        }
        if (m_stationarySamples && m_now - m_lastCalibrationSample > 250) stable = false;
        m_lastCalibrationSample = m_now;
        if (m_acceleration)
        {
            float magnitudeSquared = 0, movementSquared = 0;
            for (size_t axis = 0; axis < 3; ++axis)
            {
                const float value = (*m_acceleration)[axis];
                magnitudeSquared += value * value;
                const float movement = value - m_stationaryAcceleration[axis];
                movementSquared += movement * movement;
            }
            if (!std::isfinite(magnitudeSquared) || magnitudeSquared < .85f * .85f ||
                magnitudeSquared > 1.15f * 1.15f ||
                (m_stationarySamples && movementSquared > .025f * .025f)) stable = false;
        }
        if (!stable) { m_stationarySamples = 0; m_stationaryStart = m_now; m_gyroMean = {}; m_motion.calibrationProgress = 1; }
        else
        {
            if (!m_stationarySamples)
            {
                m_stationaryStart = m_now;
                if (m_acceleration) m_stationaryAcceleration = *m_acceleration;
            }
            ++m_stationarySamples;
            for (size_t axis = 0; axis < 3; ++axis)
                m_gyroMean[axis] += (m_motion.angularVelocity[axis] - m_gyroMean[axis]) / m_stationarySamples;
            m_motion.calibrationProgress = 1 - std::min(1.0f, float(m_now - m_stationaryStart) / 2000);
            if (m_now - m_stationaryStart >= 2000 && m_stationarySamples >= 40)
            {
                m_gyroBias = m_gyroMean;
                m_motion.zeroCalibrated = true;
                m_motion.calibrationProgress = 0;
                m_calibrating = false;
            }
        }
    }
    for (size_t axis = 0; axis < 3; ++axis)
        m_motion.calibratedVelocity[axis] = m_motion.zeroCalibrated ? m_motion.angularVelocity[axis] - m_gyroBias[axis] :
            m_calibrating ? 0 : m_motion.angularVelocity[axis];
}

void WiiExtensionProtocol::StartGyroCalibration()
{
    m_calibrating = true;
    m_stationarySamples = 0;
    m_stationaryStart = m_now;
    m_gyroMean = {};
    m_motion.calibrationProgress = 1;
}

void WiiExtensionProtocol::StopGyroCalibration()
{
    m_calibrating = false;
    // Cancellation must not commit an unvalidated partial window. Keep the last
    // successful bias, or use factory calibration if no stationary window passed.
    m_stationarySamples = 0;
    m_gyroMean = {};
    m_motion.calibrationProgress = -1;
}

void WiiExtensionProtocol::DecodeNunchuck(std::span<const uint8_t> b, bool passthrough)
{
    ++m_nunchuck.sequence;
    m_nunchuck.stickRaw = {b[0], b[1]};
    m_nunchuck.stick = {Axis(b[0], m_stickMin[0], m_stickCenter[0], m_stickMax[0]),
        Axis(b[1], m_stickMin[1], m_stickCenter[1], m_stickMax[1])};
    if (passthrough)
    {
        m_nunchuck.accelerationRaw = {uint16_t(uint16_t(b[2]) << 2 | (b[5] >> 3 & 2)),
            uint16_t(uint16_t(b[3]) << 2 | (b[5] >> 4 & 2)),
            uint16_t(uint16_t(b[4] & 0xfe) << 2 | (b[5] >> 5 & 6))};
        m_nunchuck.c = !(b[5] & 8); m_nunchuck.z = !(b[5] & 4);
    }
    else
    {
        m_nunchuck.accelerationRaw = {uint16_t(uint16_t(b[2]) << 2 | (b[5] >> 2 & 3)),
            uint16_t(uint16_t(b[3]) << 2 | (b[5] >> 4 & 3)),
            uint16_t(uint16_t(b[4]) << 2 | (b[5] >> 6 & 3))};
        m_nunchuck.c = !(b[5] & 2); m_nunchuck.z = !(b[5] & 1);
    }
    for (size_t i = 0; i < 3; ++i)
        m_nunchuck.acceleration[i] = (float(m_nunchuck.accelerationRaw[i]) - m_accZero[i]) / (m_accOneG[i] - m_accZero[i]);
}

void WiiExtensionProtocol::DecodeClassic(std::span<const uint8_t> b, bool passthrough)
{
    ++m_classic.sequence;
    const uint8_t lx = b[0] & (passthrough ? 0x3e : 0x3f), ly = b[1] & (passthrough ? 0x3e : 0x3f);
    const uint8_t rx = (b[0] >> 3 & 0x18) | (b[1] >> 5 & 6) | (b[2] >> 7);
    const uint8_t ry = b[2] & 31;
    m_classic.left = {Axis(lx, m_classicLeftMin[0], m_classicLeftCenter[0], m_classicLeftMax[0]),
        Axis(ly, m_classicLeftMin[1], m_classicLeftCenter[1], m_classicLeftMax[1])};
    m_classic.right = {Axis(rx, m_classicRightMin[0], m_classicRightCenter[0], m_classicRightMax[0]),
        Axis(ry, m_classicRightMin[1], m_classicRightCenter[1], m_classicRightMax[1])};
    m_classic.trigger = {float((b[2] >> 2 & 0x18) | (b[3] >> 5)) / 31, float(b[3] & 31) / 31};
    uint8_t buttonsHigh = b[5];
    if (passthrough) buttonsHigh = (b[5] & 0xfc) | (b[0] & 1) | (b[1] & 1) << 1;
    m_classic.buttons = uint16_t(~(uint16_t(buttonsHigh) << 8 | b[4])) & 0xfffe;
}
