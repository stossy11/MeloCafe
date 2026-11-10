#pragma once
#include <cstdint>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

enum class WiimoteWriteResult { Sent, WouldBlock, Disconnected };

class WiimoteDevice
{
	friend class WiimoteInfo;
public:
	virtual ~WiimoteDevice() = default;

	virtual bool write_data(const std::vector<uint8_t>& data) = 0;
    // Queued transports distinguish backpressure from a lost physical device.
    virtual WiimoteWriteResult try_write_data(const std::vector<uint8_t>& data) {
        return write_data(data) ? WiimoteWriteResult::Sent : WiimoteWriteResult::Disconnected;
    }
    // Only transports with guaranteed continuous samples enable this watchdog.
    virtual bool input_is_stale() const { return false; }
    virtual std::optional<size_t> preferred_player_index() const { return {}; }
	virtual std::optional<std::vector<uint8_t>> read_data() = 0;
    // Metadata for the last report consumed by the single provider reader.
    virtual std::optional<uint32_t> last_report_timestamp_us() const { return {}; }

	virtual bool is_connected() const { return true; }

	virtual bool operator==(const WiimoteDevice& o) const = 0;
};

using WiimoteDevicePtr = std::shared_ptr<WiimoteDevice>;
