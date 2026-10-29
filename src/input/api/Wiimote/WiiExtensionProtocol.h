#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// Transport-independent Wii extension initialization and decoding. Times are
// monotonic milliseconds. Only the provider's reader thread mutates this object.
class WiiExtensionProtocol
{
public:
    enum class Extension { None, Nunchuck, Classic };
    struct Nunchuck
    {
        std::array<uint16_t, 3> accelerationRaw{};
        std::array<float, 3> acceleration{}; // g, in the extension's sensor axes
        std::array<uint8_t, 2> stickRaw{128, 128};
        std::array<float, 2> stick{};
        bool c = false, z = false;
        uint64_t sequence = 0;
    };
    struct Classic
    {
        uint16_t buttons = 0; // little-endian physical button bit positions
        uint64_t sequence = 0;
        std::array<float, 2> left{}, right{}, trigger{};
    };
    struct MotionPlus
    {
        bool active = false, factoryCalibrated = false, integrated = false;
        bool extensionConnected = false, lastReportWasGyro = false;
        std::array<uint16_t, 3> raw{}; // yaw, roll, pitch, 14-bit ADC
        std::array<bool, 3> slow{};
        std::array<float, 3> angularVelocity{}; // right-handed pitch, roll, yaw, rad/s
        uint64_t sampleSequence = 0;
        std::array<float, 3> calibratedVelocity{};
        float calibrationProgress = 1; // 1 -> 0; -1 when explicitly stopped
        bool zeroCalibrated = false;
    };

    void StartGyroCalibration();
    void StopGyroCalibration();
    void ObserveAcceleration(std::array<float, 3> acceleration) { m_acceleration = acceleration; }
    void Status(bool extensionConnected, uint64_t now);
    void Tick(uint64_t now);
    bool ReadReply(std::span<const uint8_t> report, uint64_t now);
    void Data(std::span<const uint8_t> bytes, uint64_t now);
    std::vector<std::vector<uint8_t>> TakeCommands();
    Extension GetExtension() const { return m_extension; }
    const Nunchuck& GetNunchuck() const { return m_nunchuck; }
    const Classic& GetClassic() const { return m_classic; }
    const MotionPlus& GetMotionPlus() const { return m_motion; }
    bool NeedsExtensionReports() const { return m_motion.active || m_extension != Extension::None; }
    bool IsReady() const { return m_phase == Phase::Running; }

private:
    enum class Phase { Dormant, ProbeMotionPlus, WaitMotionInit, ReadMotionCalibration,
        WaitExtensionInit, WaitExtensionDecrypt, ReadExtensionID, ReadExtensionCalibration, WaitActivation, VerifyActivation,
        WaitRestart, Running };
    struct Read
    {
        uint32_t address;
        uint16_t size;
        std::array<uint8_t, 32> bytes{};
        std::array<bool, 32> received{};
    };
    Phase m_phase = Phase::Dormant;
    Extension m_extension = Extension::None;
    Nunchuck m_nunchuck;
    Classic m_classic;
    MotionPlus m_motion;
    std::optional<Read> m_read;
    std::vector<std::vector<uint8_t>> m_commands;
    uint64_t m_now = 0, m_deadline = 0, m_nextProbe = 0;
    bool m_statusAttached = false, m_haveMotionPlus = false, m_pollOnly = false;
    bool m_extensionInitialized = false, m_motionInitialized = false;
    uint8_t m_activationMode = 4;
    std::array<uint8_t, 32> m_motionCalibration{};
    std::array<float, 3> m_gyroBias{}, m_gyroMean{};
    std::optional<std::array<float, 3>> m_acceleration;
    std::array<float, 3> m_stationaryAcceleration{};
    uint64_t m_stationaryStart = 0;
    uint64_t m_lastCalibrationSample = 0;
    unsigned m_stationarySamples = 0;
    bool m_calibrating = true;
    std::array<float, 3> m_accZero{512, 512, 512}, m_accOneG{716, 716, 716};
    std::array<float, 2> m_stickMin{35, 35}, m_stickCenter{128, 128}, m_stickMax{225, 225};
    std::array<float, 2> m_classicLeftMin{0, 0}, m_classicLeftCenter{32, 32}, m_classicLeftMax{63, 63};
    std::array<float, 2> m_classicRightMin{0, 0}, m_classicRightCenter{16, 16}, m_classicRightMax{31, 31};

    void Start(bool deactivate);
    void ReadMemory(uint32_t address, uint16_t size, Phase phase);
    void WriteMemory(uint32_t address, uint8_t value);
    void ReadCompleted(const Read& read, bool failed);
    void InitializeExtension();
    void FinishExtension();
    void Run();
    void DecodeNunchuck(std::span<const uint8_t> bytes, bool passthrough);
    void DecodeClassic(std::span<const uint8_t> bytes, bool passthrough);
    void DecodeMotionPlus(std::span<const uint8_t> bytes);
    void SetNunchuckCalibration(std::span<const uint8_t> bytes);
    void SetClassicCalibration(std::span<const uint8_t> bytes);
    void SetMotionCalibration(std::span<const uint8_t> bytes);
};
