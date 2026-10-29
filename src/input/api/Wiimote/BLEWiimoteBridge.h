#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Handles identify a single connection lifetime. All functions are thread-safe.
uint64_t WiiRemoteBridge_add(void);
void WiiRemoteBridge_remove(uint64_t handle);
bool WiiRemoteBridge_receive(uint64_t handle, const uint8_t* report, size_t length);
bool WiiRemoteBridge_receiveTimed(uint64_t handle, const uint8_t* report, size_t length, uint32_t timestamp_us);
// Peek/consume lets the BLE client retry when its write queue is full.
size_t WiiRemoteBridge_peekOutput(uint64_t handle, uint8_t* report, size_t capacity);
void WiiRemoteBridge_consumeOutput(uint64_t handle);
void WiiRemoteBridge_notifyChanged(void);
// Highest-priority connected handle first. Does not replace transports or reset calibration.
bool WiiRemoteBridge_setPlayerOrder(const uint64_t* handles, size_t count);
// Bits: 0 MotionPlus active, 1 Nunchuck, 2 Classic, 3 gyro zero calibrated.
uint8_t WiiRemoteBridge_features(uint64_t handle);

#ifdef __cplusplus
}
#endif
