#include "input/api/Wiimote/BLEWiimoteDevice.h"
#include "input/api/Wiimote/BLEWiimoteBridge.h"
#include <algorithm>
#include <deque>
#include <chrono>
#include <map>
#include <mutex>

namespace
{
constexpr size_t kQueueLimit = 128;

size_t InputReportSize(uint8_t id)
{
	switch (id)
	{
	case 0x20: return 7;
	case 0x21: return 22;
	case 0x22: return 5;
	case 0x30: return 3;
	case 0x31: return 6;
	case 0x32: return 11;
	case 0x33: return 18;
	case 0x34: case 0x35: case 0x36: case 0x37: return 22;
	default: return 0;
	}
}

class BLEWiimoteDevice final : public WiimoteDevice
{
public:
    bool write_data(const std::vector<uint8_t>& data) override {
        return try_write_data(data) == WiimoteWriteResult::Sent;
    }
    WiimoteWriteResult try_write_data(const std::vector<uint8_t>& data) override {
        std::scoped_lock lock(m_mutex);
        if (!m_connected || data.size() < 2 || data.size() > 22) return WiimoteWriteResult::Disconnected;
        if (m_output.size() >= kQueueLimit) return WiimoteWriteResult::WouldBlock;
        m_output.push_back(data);
        return WiimoteWriteResult::Sent;
    }
    bool input_is_stale() const override {
        std::scoped_lock lock(m_mutex);
        return !m_connected || (m_inputTimeout.count() > 0 &&
            std::chrono::steady_clock::now() - m_lastSample >= m_inputTimeout);
    }
    std::optional<size_t> preferred_player_index() const override {
        std::scoped_lock lock(m_mutex);
        return m_playerIndex;
    }
    void SetPlayerIndex(size_t player) {
        std::scoped_lock lock(m_mutex);
        m_playerIndex = player;
    }
    void SetInputTimeout(uint32_t milliseconds) {
        std::scoped_lock lock(m_mutex);
        m_inputTimeout = std::chrono::milliseconds(milliseconds);
    }

	std::optional<std::vector<uint8_t>> read_data() override
	{
		std::scoped_lock lock(m_mutex);
		if (!m_connected) return std::nullopt;
        // A stalled main thread must not replay seconds of obsolete button state.
        const auto now = std::chrono::steady_clock::now();
        while (!m_input.empty() && m_input.front().bytes[0] >= 0x30 &&
               now - m_input.front().received > std::chrono::milliseconds(100)) m_input.pop_front();
		if (m_input.empty()) return std::vector<uint8_t>{};
		auto report = std::move(m_input.front());
		m_input.pop_front();
        m_lastTimestamp = report.timestamp;
        if (report.bytes[0] >= 0x30) m_lastSample = report.received;
		return report.bytes;
	}

    std::optional<uint32_t> last_report_timestamp_us() const override
    {
        std::scoped_lock lock(m_mutex);
        return m_lastTimestamp;
    }

	bool is_connected() const override
	{
		std::scoped_lock lock(m_mutex);
		return m_connected;
	}

	bool operator==(const WiimoteDevice& other) const override { return this == &other; }

	void Disconnect()
	{
		std::scoped_lock lock(m_mutex);
		m_connected = false;
		m_input.clear();
		m_output.clear();
	}

	bool Receive(const uint8_t* report, size_t length, std::optional<uint32_t> timestamp = {})
	{
		if (!report || length == 0 || length > 22) return false;
		const auto expected = InputReportSize(report[0]);
		if (!expected || length < expected) return false;
		std::scoped_lock lock(m_mutex);
		if (!m_connected) return false;
		if (m_input.size() >= kQueueLimit)
		{
			// Keep memory replies/status/ACKs, but discard the oldest real-time
			// sample when the consumer briefly stalls (e.g. while loading a game).
			const auto sample = std::find_if(m_input.begin(), m_input.end(),
				[](const auto& packet) { return packet.bytes[0] >= 0x30; });
			if (sample != m_input.end()) m_input.erase(sample);
			else return report[0] >= 0x30;
		}
		m_input.push_back({std::vector<uint8_t>(report, report + length), timestamp, std::chrono::steady_clock::now()});
		return true;
	}

	size_t PeekOutput(uint8_t* report, size_t capacity)
	{
		std::scoped_lock lock(m_mutex);
		if (!m_connected || m_output.empty() || !report || capacity < m_output.front().size()) return 0;
		const auto& packet = m_output.front();
		std::copy(packet.begin(), packet.end(), report);
		return packet.size();
	}

	void ConsumeOutput()
	{
		std::scoped_lock lock(m_mutex);
		if (!m_output.empty()) m_output.pop_front();
	}

public:
    void SetFeatures(uint8_t features) { std::scoped_lock lock(m_mutex); m_features = features; }
    uint8_t GetFeatures() const { std::scoped_lock lock(m_mutex); return m_connected ? m_features : 0; }
private:
    std::optional<size_t> m_playerIndex;
    uint8_t m_features = 0;
	mutable std::mutex m_mutex;
	bool m_connected = true;
    struct Packet {
        std::vector<uint8_t> bytes;
        std::optional<uint32_t> timestamp;
        std::chrono::steady_clock::time_point received;
    };
    std::chrono::milliseconds m_inputTimeout{500};
    std::chrono::steady_clock::time_point m_lastSample{};
    std::optional<uint32_t> m_lastTimestamp;
    std::deque<Packet> m_input;
    std::deque<std::vector<uint8_t>> m_output;
};

std::mutex s_registryMutex;
std::map<uint64_t, std::shared_ptr<BLEWiimoteDevice>> s_devices;
std::vector<uint64_t> s_playerOrder;
uint64_t s_nextHandle = 1;

std::shared_ptr<BLEWiimoteDevice> FindDevice(uint64_t handle)
{
	std::scoped_lock lock(s_registryMutex);
	const auto it = s_devices.find(handle);
	return it == s_devices.end() ? nullptr : it->second;
}
}

std::vector<WiimoteDevicePtr> GetBLEWiimoteDevices()
{
	std::scoped_lock lock(s_registryMutex);
	std::vector<WiimoteDevicePtr> result;
	for (const auto handle : s_playerOrder)
		if (const auto it = s_devices.find(handle); it != s_devices.end()) result.push_back(it->second);
	for (const auto& [handle, device] : s_devices)
		if (std::find(s_playerOrder.begin(), s_playerOrder.end(), handle) == s_playerOrder.end()) result.push_back(device);
	return result;
}

extern "C" bool WiiRemoteBridge_setPlayerOrder(const uint64_t* handles, size_t count)
{
	std::scoped_lock lock(s_registryMutex);
	if (count != s_devices.size() || (count && !handles)) return false;
	std::vector<uint64_t> order;
	for (size_t i = 0; i < count; ++i)
	{
		if (!s_devices.contains(handles[i]) || std::find(order.begin(), order.end(), handles[i]) != order.end()) return false;
		order.push_back(handles[i]);
	}
    for (size_t player = 0; player < order.size(); ++player) s_devices.at(order[player])->SetPlayerIndex(player);
	s_playerOrder = std::move(order);
	return true;
}

extern "C" bool WiiRemoteBridge_setPlayerSlots(const uint64_t* handles, size_t count)
{
    std::scoped_lock lock(s_registryMutex);
    if (count != 4 || !handles) return false;
    std::vector<uint64_t> active;
    for (size_t player = 0; player < count; ++player) {
        if (!handles[player]) continue;
        if (!s_devices.contains(handles[player]) || std::find(active.begin(), active.end(), handles[player]) != active.end()) return false;
        active.push_back(handles[player]);
    }
    if (active.size() != s_devices.size()) return false;
    for (size_t player = 0; player < count; ++player)
        if (handles[player]) s_devices.at(handles[player])->SetPlayerIndex(player);
    s_playerOrder.assign(handles, handles + count);
    return true;
}

extern "C" uint64_t WiiRemoteBridge_add()
{
	std::scoped_lock lock(s_registryMutex);
	if (s_devices.size() >= 4) return 0;
	const auto handle = s_nextHandle++;
	s_devices.emplace(handle, std::make_shared<BLEWiimoteDevice>());
	return handle;
}

extern "C" void WiiRemoteBridge_remove(uint64_t handle)
{
	std::scoped_lock lock(s_registryMutex);
	const auto it = s_devices.find(handle);
	if (it == s_devices.end()) return;
	it->second->Disconnect();
	s_devices.erase(it);
	std::erase(s_playerOrder, handle);
}

extern "C" bool WiiRemoteBridge_receive(uint64_t handle, const uint8_t* report, size_t length)
{
	const auto device = FindDevice(handle);
	return device && device->Receive(report, length);
}

extern "C" bool WiiRemoteBridge_receiveTimed(uint64_t handle, const uint8_t* report, size_t length, uint32_t timestamp_us)
{
    const auto device = FindDevice(handle);
    return device && device->Receive(report, length, timestamp_us);
}

extern "C" size_t WiiRemoteBridge_peekOutput(uint64_t handle, uint8_t* report, size_t capacity)
{
	const auto device = FindDevice(handle);
	return device ? device->PeekOutput(report, capacity) : 0;
}

extern "C" void WiiRemoteBridge_consumeOutput(uint64_t handle)
{
	if (const auto device = FindDevice(handle)) device->ConsumeOutput();
}

void UpdateBLEWiimoteFeatures(WiimoteDevice& device, uint8_t features)
{
    if (auto* ble = dynamic_cast<BLEWiimoteDevice*>(&device)) ble->SetFeatures(features);
}

extern "C" uint8_t WiiRemoteBridge_features(uint64_t handle)
{
    const auto device = FindDevice(handle);
    return device ? device->GetFeatures() : 0;
}

extern "C" void WiiRemoteBridge_setInputTimeout(uint64_t handle, uint32_t milliseconds)
{
    if (const auto device = FindDevice(handle)) device->SetInputTimeout(milliseconds);
}
