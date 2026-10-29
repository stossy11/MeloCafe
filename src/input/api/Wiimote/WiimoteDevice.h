#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class WiimoteDevice
{
	friend class WiimoteInfo;
public:
	virtual ~WiimoteDevice() = default;

	virtual bool write_data(const std::vector<uint8_t>& data) = 0;
	virtual std::optional<std::vector<uint8_t>> read_data() = 0;
    // Metadata for the last report consumed by the single provider reader.
    virtual std::optional<uint32_t> last_report_timestamp_us() const { return {}; }

	virtual bool is_connected() const { return true; }

	virtual bool operator==(const WiimoteDevice& o) const = 0;
};

using WiimoteDevicePtr = std::shared_ptr<WiimoteDevice>;
