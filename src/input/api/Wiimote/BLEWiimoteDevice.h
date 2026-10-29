#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
#include "input/api/Wiimote/WiimoteDevice.h"

// A snapshot keeps disconnected devices alive until the provider releases them.
std::vector<WiimoteDevicePtr> GetBLEWiimoteDevices();

void UpdateBLEWiimoteFeatures(WiimoteDevice& device, uint8_t features);
