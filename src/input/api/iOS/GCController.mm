#if BOOST_OS_IOS

#include "GCController.h"
#include "GCControllerProvider.h"
#include <cmath>


static constexpr int kBitA       = 0;
static constexpr int kBitB       = 1;
static constexpr int kBitX       = 2;
static constexpr int kBitY       = 3;
static constexpr int kBitLB      = 4;
static constexpr int kBitRB      = 5;
static constexpr int kBitLTDig   = 6;
static constexpr int kBitRTDig   = 7;
static constexpr int kBitOptions = 8;
static constexpr int kBitMenu    = 9;
static constexpr int kBitLS      = 10;
static constexpr int kBitRS      = 11;
static constexpr int kBitDUp     = 16;
static constexpr int kBitDDown   = 17;
static constexpr int kBitDLeft   = 18;
static constexpr int kBitDRight  = 19;

GCControllerDevice::GCControllerDevice(size_t player_index,
                                       std::string_view display_name,
                                       GCBridgeControllerDesc desc)
    : base_type(fmt::format("gccontroller_{}", player_index), display_name)
    , m_player_index(player_index)
    , m_desc(desc)
{
}

GCControllerDevice::~GCControllerDevice()
{
    disconnect();
}

bool GCControllerDevice::is_connected()
{
    return m_connected.load(std::memory_order_relaxed);
}

bool GCControllerDevice::connect()
{
    return is_connected();
}

EmulatedController::Type GCControllerDevice::type() {
    std::scoped_lock lock(m_mutex);
    return (EmulatedController::Type)m_desc.controllerType;
}

bool GCControllerDevice::set_type(EmulatedController::Type type)
{
    std::scoped_lock lock(m_mutex);
    if (m_desc.controllerType == type) return false;
    m_desc.controllerType = static_cast<uint8>(type);
    return true;
}

void GCControllerDevice::disconnect()
{
    GCBridgeReleaseFn release = nullptr;
    void* context = nullptr;
    {
        std::scoped_lock lock(m_mutex);
        if (!m_connected.exchange(false)) return;
        if (m_desc.rumble) m_desc.rumble(m_desc.context, false);
        release = m_desc.release;
        context = m_desc.context;
        m_desc.poll_state = nullptr;
        m_desc.poll_motion = nullptr;
        m_desc.rumble = nullptr;
        m_desc.release = nullptr;
        m_desc.context = nullptr;
    }
    
    if (release) release(context);
}

bool GCControllerDevice::has_motion()
{
    std::scoped_lock lock(m_mutex);
    return m_desc.poll_motion != nullptr;
}

bool GCControllerDevice::has_rumble()
{
    std::scoped_lock lock(m_mutex);
    return m_desc.rumble != nullptr;
}

void GCControllerDevice::start_rumble()
{
    std::scoped_lock lock(m_mutex);
    if (m_desc.rumble) m_desc.rumble(m_desc.context, true);
}

void GCControllerDevice::stop_rumble()
{
    std::scoped_lock lock(m_mutex);
    if (m_desc.rumble) m_desc.rumble(m_desc.context, false);
}

MotionSample GCControllerDevice::get_motion_sample()
{
    std::scoped_lock lock(m_mutex);
    if (!m_desc.poll_motion)
        return {};

    GCBridgeMotionState s = m_desc.poll_motion(m_desc.context);
    
    
    if (!std::isfinite(s.timestamp) || s.timestamp <= m_last_motion_ts)
        return m_last_motion_cache;
    
    float deltaTime = 0.0f;
    if (m_last_motion_ts > 0.0)
    {
        const double elapsed = s.timestamp - m_last_motion_ts;

        if (elapsed < 1.0)
            deltaTime = static_cast<float>(elapsed);
    }
    m_last_motion_ts = s.timestamp;
    
    m_motion_handler.processMotionSample(deltaTime,
        s.gyroscope.x,     s.gyroscope.y,     s.gyroscope.z,
        s.accelerometer.x, s.accelerometer.y, s.accelerometer.z, false);

    m_last_motion_cache = m_motion_handler.getMotionSample().withReversedHorizontalAxes();
    return m_last_motion_cache;
}

ControllerState GCControllerDevice::raw_state()
{
    std::scoped_lock lock(m_mutex);
    ControllerState result{};
    if (!m_connected || !m_desc.poll_state)
        return result;

    GCBridgeControllerState s = m_desc.poll_state(m_desc.context);

    auto btn = [&](int bit, int index) {
        if (s.buttons & (1u << bit))
            result.buttons.SetButtonState(index, true);
    };

    btn(kBitA,       0);
    btn(kBitB,       1);
    btn(kBitX,       2);
    btn(kBitY,       3);
    btn(kBitLB,      4);
    btn(kBitRB,      5);
    btn(kBitLTDig,   6);
    btn(kBitRTDig,   7);
    btn(kBitOptions, 8);
    btn(kBitMenu,    9);
    btn(kBitLS,      10);
    btn(kBitRS,      11);
    btn(kBitDUp,     kButtonUp);
    btn(kBitDDown,   kButtonDown);
    btn(kBitDLeft,   kButtonLeft);
    btn(kBitDRight,  kButtonRight);

    result.axis.x =  s.leftStick.x;
    result.axis.y = -s.leftStick.y;

    result.rotation.x =  s.rightStick.x;
    result.rotation.y = -s.rightStick.y;

    result.trigger.x = s.leftTrigger;
    result.trigger.y = s.rightTrigger;

    return result;
}

std::string GCControllerDevice::get_button_name(uint64 button) const
{
    return base_type::get_button_name(button);
}

#endif // BOOST_OS_IOS
