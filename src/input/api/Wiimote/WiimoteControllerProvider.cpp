#include "input/api/Wiimote/WiimoteControllerProvider.h"
#include "input/api/Wiimote/NativeWiimoteController.h"
#include "input/api/Wiimote/WiimoteMessages.h"
#include "input/api/Wiimote/WiiIRTracking.h"
#include "input/motion/WiiMotionCoordinates.h"

#ifdef HAS_HIDAPI
#include "input/api/Wiimote/hidapi/HidapiWiimote.h"
#endif
#ifdef HAS_BLUEZ
#include "input/api/Wiimote/l2cap/L2CapWiimote.h"
#endif

#if BOOST_OS_IOS
#include "input/api/Wiimote/BLEWiimoteDevice.h"
#endif

#include <numbers>
#include <queue>

WiimoteControllerProvider::WiimoteControllerProvider()
	: m_running(true)
{
	m_reader_thread = std::thread(&WiimoteControllerProvider::reader_thread, this);
	m_writer_thread = std::thread(&WiimoteControllerProvider::writer_thread, this);
	m_connectionThread = std::thread(&WiimoteControllerProvider::connectionThread, this);
}

WiimoteControllerProvider::~WiimoteControllerProvider()
{
	if (m_running)
	{
		m_running = false;

		{
			std::scoped_lock lock(m_writer_mutex); 
			m_writer_cond.notify_all();
		}

		{
			std::scoped_lock lock(m_connectionMutex);
			m_connectionCond.notify_all();
		}

		if (m_writer_thread.joinable())
		{
			m_writer_thread.join();
		}

		if (m_reader_thread.joinable())
		{
			m_reader_thread.join();
		}

		if (m_connectionThread.joinable())
		{
			m_connectionThread.join();
		}
	}
}

std::vector<std::shared_ptr<ControllerBase>> WiimoteControllerProvider::get_controllers()
{
	m_connectedDeviceMutex.lock();
	auto devices = m_connectedDevices;
	m_connectedDeviceMutex.unlock();
#if BOOST_OS_IOS
	const auto bleDevices = GetBLEWiimoteDevices();
	devices.insert(devices.end(), bleDevices.begin(), bleDevices.end());
#endif

	std::scoped_lock lock(m_device_mutex);

	// Device ownership changes only under the exclusive lock. Reader/writer threads
	// flag disconnects instead of resetting shared_ptrs under a shared lock.
	for (auto& wiimote : m_wiimotes)
	{
		if (wiimote.device && (wiimote.disconnected || !wiimote.device->is_connected()))
			wiimote.device.reset();
	}

	for (auto& device : devices)
	{
		if (!device->is_connected()) continue;

		bool isDuplicate = false;
		ssize_t lowestReplaceableIndex = -1;
		for (ssize_t i = m_wiimotes.size() - 1; i >= 0; --i)
		{
			const auto& wiimoteDevice = m_wiimotes[i].device;
			if (wiimoteDevice)
			{
				if (*wiimoteDevice == *device)
				{
					isDuplicate = true;
					break;
				}
				continue;
			}

			lowestReplaceableIndex = i;
		}
		if (isDuplicate)
			continue;
		if (!device->write_data({kStatusRequest, 0x00})) continue;
		// Do not let commands queued for a previous connection reach its replacement.
		if (lowestReplaceableIndex != -1)
		{
			std::scoped_lock writerLock(m_writer_mutex);
			std::erase_if(m_write_queue, [lowestReplaceableIndex](const auto& packet) {
				return packet.first == static_cast<size_t>(lowestReplaceableIndex);
			});
		}
		if (lowestReplaceableIndex != -1)
			m_wiimotes.replace(lowestReplaceableIndex, std::make_unique<Wiimote>(device));
		else
			m_wiimotes.push_back(std::make_unique<Wiimote>(device));
	}

	std::vector<std::shared_ptr<ControllerBase>> result;
	result.reserve(m_wiimotes.size());
	std::vector<size_t> indices;
	for (size_t i = 0; i < m_wiimotes.size(); ++i)
	{
		if (m_wiimotes[i].device)
			indices.push_back(i);
	}
#if BOOST_OS_IOS
	// Sort the returned controller list, preserving each provider slot and gyro state.
	std::stable_sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
		return std::find(bleDevices.begin(), bleDevices.end(), m_wiimotes[a].device) <
			std::find(bleDevices.begin(), bleDevices.end(), m_wiimotes[b].device);
	});
#endif
	for (const auto index : indices) result.emplace_back(std::make_shared<NativeWiimoteController>(index));

	return result;
}

bool WiimoteControllerProvider::is_connected(size_t index)
{
	std::shared_lock lock(m_device_mutex);
	return index < m_wiimotes.size() && m_wiimotes[index].device &&
		!m_wiimotes[index].disconnected && m_wiimotes[index].device->is_connected();
}

bool WiimoteControllerProvider::is_registered_device(size_t index)
{
	std::shared_lock lock(m_device_mutex);
	return index < m_wiimotes.size();
}

void WiimoteControllerProvider::set_rumble(size_t index, bool state)
{
	std::shared_lock lock(m_device_mutex);
	if (index >= m_wiimotes.size())
		return;
	
	m_wiimotes[index].rumble = state;
	lock.unlock();

	send_packet(index, { kStatusRequest, 0x00 });
}

void WiimoteControllerProvider::calibrate_gyro(size_t index, bool start)
{
    std::shared_lock lock(m_device_mutex);
    if (index < m_wiimotes.size()) m_wiimotes[index].gyro_calibration_request = start ? 1 : 2;
}

void WiimoteControllerProvider::request_status(size_t index)
{
	send_packet(index, {kStatusRequest, 0x00});
}

void WiimoteControllerProvider::set_led(size_t index, size_t player_index)
{
	uint8 mask = 0;
	mask |= 1 << (4 + (player_index % 4));
	if (player_index >= 4)
		mask |= 1 << (4 + ((player_index - 3) % 4));
	send_packet(index, {kLED, mask});
}

uint32 WiimoteControllerProvider::get_packet_delay(size_t index)
{
	std::shared_lock lock(m_device_mutex);
	if (index < m_wiimotes.size())
	{
		return m_wiimotes[index].data_delay;
	}

	return kDefaultPacketDelay;
}

void WiimoteControllerProvider::set_packet_delay(size_t index, uint32 delay)
{
	std::shared_lock lock(m_device_mutex);
	if (index < m_wiimotes.size())
	{
		m_wiimotes[index].data_delay = delay;
	}
}

WiimoteControllerProvider::WiimoteState WiimoteControllerProvider::get_state(size_t index)
{
	std::shared_lock lock(m_device_mutex);
	if (index < m_wiimotes.size())
	{
		std::shared_lock data_lock(m_wiimotes[index].mutex);
		return m_wiimotes[index].state;
	}

	return {};
}

void WiimoteControllerProvider::connectionThread()
{
	SetThreadName("Wiimote-connect");
	while (m_running.load(std::memory_order_relaxed))
	{
		std::vector<WiimoteDevicePtr> devices;
#ifdef HAS_HIDAPI
		const auto& hidDevices = HidapiWiimote::get_devices();
		std::ranges::move(hidDevices, std::back_inserter(devices));
#endif
#ifdef HAS_BLUEZ
		const auto& l2capDevices = L2CapWiimote::get_devices();
		std::ranges::move(l2capDevices, std::back_inserter(devices));
#endif
		{
			std::scoped_lock lock(m_connectedDeviceMutex);
			m_connectedDevices.clear();
			std::ranges::move(devices, std::back_inserter(m_connectedDevices));
		}
		std::unique_lock<std::mutex> lock(m_connectionMutex);
		m_connectionCond.wait_for(lock, std::chrono::seconds(2));
	}
}


void WiimoteControllerProvider::sync_extensions(WiimoteState& state, const WiiExtensionProtocol& extensions)
{
    const auto& mp = extensions.GetMotionPlus();
    if (mp.active)
    {
        if (!state.m_motion_plus) { state.motion_handler = {}; state.m_motion_clock = {}; state.m_gravity_initialized = false; }
        if (!state.m_motion_plus) state.m_motion_plus = MotionPlusData{};
        auto& motion = *state.m_motion_plus;
        motion.angular_velocity = {mp.calibratedVelocity[0], mp.calibratedVelocity[1], mp.calibratedVelocity[2]};
        motion.calibration_progress = mp.calibrationProgress;
        motion.zero_calibrated = mp.zeroCalibrated;
        motion.raw = mp.raw;
        motion.slow = mp.slow;
        motion.extension_connected = mp.extensionConnected;
        motion.factory_calibrated = mp.factoryCalibrated;
        motion.integrated = mp.integrated;
        motion.sequence = mp.sampleSequence;
        motion.last_report_gyro = mp.lastReportWasGyro;
    }
    else
    {
        if (state.m_motion_plus) { state.motion_handler = {}; state.m_motion_clock = {}; state.m_gravity_initialized = false; }
        state.m_motion_plus.reset();
    }

    if (extensions.GetExtension() == WiiExtensionProtocol::Extension::Nunchuck)
    {
        const auto& input = extensions.GetNunchuck();
        const auto previous = std::get_if<NunchuckData>(&state.m_extension);
        if (previous && previous->sequence == input.sequence) return;
        const glm::vec3 previousAcc = previous ? previous->acceleration : glm::vec3{};
        NunchuckData nunchuck;
        nunchuck.sequence = input.sequence;
        nunchuck.c = input.c; nunchuck.z = input.z;
        nunchuck.axis = {input.stick[0], input.stick[1]};
        nunchuck.raw_axis = {input.stickRaw[0], input.stickRaw[1]};
        nunchuck.raw_acceleration = {input.accelerationRaw[0], input.accelerationRaw[1], input.accelerationRaw[2]};
        nunchuck.acceleration = {input.acceleration[0], input.acceleration[1], input.acceleration[2]};
        nunchuck.prev_acceleration = previousAcc;
        float acc[3]{-nunchuck.acceleration.x, -nunchuck.acceleration.z, nunchuck.acceleration.y};
        float zero[3]{}, identity[4]{1, 0, 0, 0};
        nunchuck.motion_sample = MotionSample(acc, glm::length(nunchuck.acceleration - previousAcc), zero, zero, identity);
        state.m_extension = nunchuck;
    }
    else if (extensions.GetExtension() == WiiExtensionProtocol::Extension::Classic)
    {
        const auto& input = extensions.GetClassic();
        ClassicData classic;
        classic.buttons = input.buttons;
        classic.sequence = input.sequence;
        classic.left_axis = {input.left[0], input.left[1]};
        classic.right_axis = {input.right[0], input.right[1]};
        classic.trigger = {input.trigger[0], input.trigger[1]};
        state.m_extension = classic;
    }
    else state.m_extension = {};
}

void WiimoteControllerProvider::reader_thread()
{
    SetThreadName("Wiimote-reader");
    std::chrono::steady_clock::time_point lastCheck{};
    while (m_running.load(std::memory_order_relaxed))
    {
        const auto now = std::chrono::steady_clock::now();
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        if (now - lastCheck >= std::chrono::milliseconds(500))
        {
            get_controllers();
            lastCheck = now;
        }
        bool receivedAnyPacket = false;
        std::shared_lock lock(m_device_mutex);
        for (size_t index = 0; index < m_wiimotes.size(); ++index)
        {
            auto& wiimote = m_wiimotes[index];
            if (!wiimote.device || wiimote.disconnected) continue;
            auto& extensions = wiimote.extensions;
            const auto calibrationRequest = wiimote.gyro_calibration_request.exchange(0);
            if (calibrationRequest == 1) extensions.StartGyroCalibration();
            if (calibrationRequest == 2) extensions.StopGyroCalibration();
            extensions.Tick(milliseconds);
            const auto report = wiimote.device->read_data();
            if (!report) { wiimote.disconnected = true; continue; }
            auto commands = extensions.TakeCommands();
            if (report->empty() && commands.empty()) continue;
            receivedAnyPacket |= !report->empty();

            std::shared_lock readLock(wiimote.mutex);
            WiimoteState state = wiimote.state;
            readLock.unlock();
            const bool previousZero = state.m_motion_plus && state.m_motion_plus->zero_calibrated;
            const bool previousExtended = state.m_extension.index() != 0 || state.m_motion_plus.has_value();
            const auto previousSequence = state.m_motion_plus ? state.m_motion_plus->sequence : 0;
            bool accelerationUpdated = false;
            bool updateReport = false;

            if (!report->empty())
            {
                const auto& b = *report;
                const auto id = b[0];
                const size_t expected = id == 0x20 ? 7 : id == 0x22 ? 5 : id == 0x30 ? 3 :
                    id == 0x31 ? 6 : id == 0x32 ? 11 : id == 0x33 ? 18 : 22;
                if (b.size() < expected) continue;
                if (id != 0x3d) state.buttons = (uint16_t(b[1]) | uint16_t(b[2]) << 8) & ~0x60e0;
                switch (id)
                {
                case kStatus:
                    state.flags = b[3]; state.battery_level = b[6];
                    if (!state.m_calibrated) calibrate(index);
                    extensions.Status((state.flags & kExtensionConnected) != 0, milliseconds);
                    updateReport = true;
                    break;
                case kRead:
                    if (!extensions.ReadReply(b, milliseconds) && (b[3] & 15) == 0 &&
                        b[4] == 0 && b[5] == 0x16 && (b[3] >> 4) + 1 >= 10)
                    {
                        uint8_t checksum = 0x55;
                        for (size_t i = 6; i < 15; ++i) checksum += b[i];
                        if (checksum == b[15])
                        {
                            Calibration calibration;
                            for (int axis = 0; axis < 3; ++axis)
                            {
                                calibration.zero[axis] = uint16_t(b[6 + axis]) << 2 | (b[9] >> (4 - axis * 2) & 3);
                                calibration.gravity[axis] = uint16_t(b[10 + axis]) << 2 | (b[13] >> (4 - axis * 2) & 3);
                            }
                            if (glm::all(glm::greaterThan(calibration.gravity, calibration.zero)))
                            { state.m_calib_acceleration = calibration; state.m_calibrated = true; }
                        }
                    }
                    break;
                case kDataCoreAcc: case kDataCoreAccIR: case kDataCoreAccExt: case kDataCoreAccIRExt:
                {
                    const uint8_t* data = b.data() + 1;
                    parse_acceleration(state, data);
                    accelerationUpdated = true;
                    const auto gravity = glm::vec3(state.m_calib_acceleration.gravity) - glm::vec3(state.m_calib_acceleration.zero);
                    const auto acceleration = state.m_acceleration / gravity;
                    extensions.ObserveAcceleration({acceleration.x, acceleration.y, acceleration.z});
                    if (id == kDataCoreAccIR) parse_ir(state, b.data() + 6, kExtendedIR);
                    if (id == kDataCoreAccIRExt) parse_ir(state, b.data() + 6, kBasicIR);
                    if (id == kDataCoreAccExt) extensions.Data(std::span(b).subspan(6), milliseconds);
                    if (id == kDataCoreAccIRExt) extensions.Data(std::span(b).subspan(16), milliseconds);
                    break;
                }
                case kDataCoreExt8: case kDataCoreExt19:
                    extensions.Data(std::span(b).subspan(3), milliseconds); break;
                case kDataCoreIRExt:
                    parse_ir(state, b.data() + 3, kBasicIR);
                    extensions.Data(std::span(b).subspan(13), milliseconds); break;
                case kDataExt:
                    extensions.Data(std::span(b).subspan(1), milliseconds); break;
                default: break;
                }
            }
            sync_extensions(state, extensions);
            const bool extended = extensions.NeedsExtensionReports();
            updateReport |= previousExtended != extended;
            // Finish extension discovery before flooding the transport with IR.
            // No-extension remotes also need their IR camera enabled when ready.
            updateReport |= extensions.IsReady() && state.ir_camera.mode == kIRDisabled;
            const auto sequence = state.m_motion_plus ? state.m_motion_plus->sequence : 0;
            const bool gyroUpdated = sequence != previousSequence;
            if (gyroUpdated || (accelerationUpdated && !state.m_motion_plus))
            {
                const auto gravity = glm::vec3(state.m_calib_acceleration.gravity) - glm::vec3(state.m_calib_acceleration.zero);
                const auto acceleration = state.m_acceleration / gravity;
                const auto gyro = state.m_motion_plus ? state.m_motion_plus->angular_velocity : glm::vec3{};
                const auto elapsed = state.m_motion_clock.elapsed(wiimote.device->last_report_timestamp_us(), now);
                if (state.m_motion_plus)
                    state.m_motion_plus->angles += WiiMotionCoordinates::AngularVelocity(gyro) * elapsed;
                const auto fusionGyro = WiiMotionCoordinates::ToFusion(gyro);
                const auto fusionAcc = WiiMotionCoordinates::ToFusion(acceleration);
                // Starting sideways must not spend seconds converging from an assumed
                // flat pose. A completed stationary zero window can also restore tilt
                // after movement during startup (when gyro output was suppressed).
                if (!state.m_gravity_initialized || (!previousZero && state.m_motion_plus && state.m_motion_plus->zero_calibrated))
                    state.m_gravity_initialized = state.motion_handler.alignToGravity(fusionAcc.x, fusionAcc.y, fusionAcc.z);
                // WiiExtensionProtocol already removes the stationary gyro bias.
                state.motion_handler.processMotionSample(elapsed,
                    fusionGyro.x, fusionGyro.y, fusionGyro.z,
                    fusionAcc.x, fusionAcc.y, fusionAcc.z, false);
                state.motion_sample = state.motion_handler.getMotionSample().withWiiRemoteAxes();
            }
            auto moreCommands = extensions.TakeCommands();
            commands.insert(commands.end(), std::make_move_iterator(moreCommands.begin()), std::make_move_iterator(moreCommands.end()));
            for (auto& command : commands) send_packet(index, std::move(command));
            {
                std::unique_lock dataLock(wiimote.mutex);
                wiimote.state = std::move(state);
            }
#if BOOST_OS_IOS
            const auto& mp = extensions.GetMotionPlus();
            UpdateBLEWiimoteFeatures(*wiimote.device, (mp.active ? 1 : 0) |
                (extensions.GetExtension() == WiiExtensionProtocol::Extension::Nunchuck ? 2 : 0) |
                (extensions.GetExtension() == WiiExtensionProtocol::Extension::Classic ? 4 : 0) |
                (mp.zeroCalibrated ? 8 : 0));
#endif
            if (updateReport) update_report_type(index);
        }
        lock.unlock();
        if (!receivedAnyPacket) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void WiimoteControllerProvider::parse_acceleration(WiimoteState& wiimote_state, const uint8*& data)
{
	glm::vec<3, uint16> raw_acc;
	// acc encoded in BB BB
	raw_acc.x = (*data >> 5) & 3; // 6|5 -> 1|0
	++data;
	raw_acc.y = (*data >> 4) & 2; // 5 -> 1
	raw_acc.z = (*data >> 5) & 2; // 6 -> 1
	++data;

	raw_acc.x |= (uint16)*data << 2;
	++data;
	raw_acc.y |= (uint16)*data << 2;
	++data;
	raw_acc.z |= (uint16)*data << 2;
	++data;

	wiimote_state.m_raw_acceleration = raw_acc;
	wiimote_state.m_prev_acceleration = wiimote_state.m_acceleration;
	glm::vec3 acceleration = raw_acc;

	const auto& calib = wiimote_state.m_calib_acceleration;
	wiimote_state.m_acceleration = acceleration - glm::vec3(calib.zero);

	glm::vec3 tmp = calib.gravity;
	tmp -= calib.zero;
	acceleration = (wiimote_state.m_acceleration / tmp);

	const float pi_2 = (float)std::numbers::pi / 2.0f;
	wiimote_state.m_roll = std::atan2(acceleration.z, acceleration.x) - pi_2;
}

void WiimoteControllerProvider::rotate_ir(WiimoteState& wiimote_state)
{
    for (auto& dot : wiimote_state.ir_camera.dots)
        if (dot.visible) dot.pos = WiiIRTracking::Rotate(dot.pos, wiimote_state.m_roll);
}

void WiimoteControllerProvider::calculate_ir_position(WiimoteState& wiimote_state)
{
    WiiIRTracking::Track(wiimote_state.ir_camera);
}

sint32 WiimoteControllerProvider::parse_ir(WiimoteState& wiimote_state, const uint8* data, IRMode mode)
{
    ++wiimote_state.ir_camera.sequence;
    wiimote_state.ir_camera.timestamp = std::chrono::steady_clock::now();
	// Reports in flight may use the old mode while IR registers are changing.
	switch (mode)
	{
	case kIRDisabled:
		wiimote_state.ir_camera.dots = {};
		return 0;
	case kBasicIR:
		{
			const auto ir = (BasicIR*)data;
			for (int i = 0; i < 2; ++i)
			{
				auto& dot1 = wiimote_state.ir_camera.dots[i * 2];
				auto& dot2 = wiimote_state.ir_camera.dots[i * 2 + 1];

				dot1.raw.x = ir[i].x1 | (ir[i].bits.x1 << 8); // 9|8
				dot1.raw.y = ir[i].y1 | (ir[i].bits.y1 << 8);
				dot1.size = 0;

				dot2.raw.x = ir[i].x2 | (ir[i].bits.x2 << 8);
				dot2.raw.y = ir[i].y2 | (ir[i].bits.y2 << 8);
				dot2.size = 0;

				dot1.visible = dot1.raw != glm::vec<2, uint16>(0x3ff, 0x3ff);
				if (dot1.visible)
					dot1.pos = glm::vec2(1.0f - dot1.raw.x / 1023.0f, (float)dot1.raw.y / 768.0f);
				else
					dot1.pos = {};

				dot2.visible = dot2.raw != glm::vec<2, uint16>(0x3ff, 0x3ff);
				if (dot2.visible)
					dot2.pos = glm::vec2(1.0f - dot2.raw.x / 1023.0f, (float)dot2.raw.y / 768.0f);
				else
					dot2.pos = {};
			}

			rotate_ir(wiimote_state);
			calculate_ir_position(wiimote_state);
			return sizeof(BasicIR) * 2;
		}
	case kExtendedIR:
		{
			const auto ir = (ExtendedIR*)data;
			for (int i = 0; i < 4; ++i)
			{
				auto& dot = wiimote_state.ir_camera.dots[i];
				dot.raw.x = ir[i].x;
				dot.raw.y = ir[i].y;

				dot.raw.x |= (uint16)ir[i].bits.x << 8; // 9|8
				dot.raw.y |= (uint16)ir[i].bits.y << 8; // 9|8

				dot.size = ir[i].bits.size;

				dot.visible = dot.raw != glm::vec<2, uint16>(0x3ff, 0x3ff);
				if (dot.visible)
					dot.pos = glm::vec2(1.0f - dot.raw.x / 1023.0f, (float)dot.raw.y / 768.0f);
				else
					dot.pos = {};
			}

			rotate_ir(wiimote_state);
			calculate_ir_position(wiimote_state);
			return sizeof(ExtendedIR) * 4;
		}
	default:
		cemu_assert(false);
		break;
	}
	return 0;
}

void WiimoteControllerProvider::writer_thread()
{
	SetThreadName("Wiimote-writer");
	while (m_running.load(std::memory_order_relaxed))
	{
		std::unique_lock writer_lock(m_writer_mutex);
		while (m_write_queue.empty())
		{
			if (m_writer_cond.wait_for(writer_lock, std::chrono::milliseconds(250)) == std::cv_status::timeout)
			{
				if (!m_running.load(std::memory_order_relaxed))
					return;
			}
		}

		auto index = (size_t)-1;
		std::vector<uint8> data;
		writer_lock.unlock();
		std::shared_lock device_lock(m_device_mutex);
		writer_lock.lock();

		// get first packet of device which is ready to be sent
		const auto now = std::chrono::high_resolution_clock::now();
		for (auto it = m_write_queue.begin(); it != m_write_queue.end(); ++it)
		{
			if (it->first >= m_wiimotes.size())
				continue;

			const auto delay = m_wiimotes[it->first].data_delay.load(std::memory_order_relaxed);
			if (now >= m_wiimotes[it->first].data_ts + std::chrono::milliseconds(delay))
			{
				index = it->first;
				data = it->second;
				m_write_queue.erase(it);
				break;
			}
		}
		writer_lock.unlock();

		if (index != (size_t)-1 && !data.empty())
		{
			auto& wiimote = m_wiimotes[index];
			if (!wiimote.device)
				continue;
			if (wiimote.rumble)
				data[1] |= 1;
			if (!wiimote.device->write_data(data))
			{
				wiimote.disconnected = true;
				wiimote.rumble = false;
			}
			else
				wiimote.data_ts = std::chrono::high_resolution_clock::now();
		}
		device_lock.unlock();

		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		std::this_thread::yield();
	}
}

void WiimoteControllerProvider::calibrate(size_t index)
{
	send_read_packet(index, kEEPROMMemory, kRegisterCalibration, 10);
}

void WiimoteControllerProvider::update_report_type(size_t index)
{
	std::shared_lock read_lock(m_wiimotes[index].mutex);
	auto& state = m_wiimotes[index].state;

	const bool extension = state.m_extension.index() != 0 || state.m_motion_plus.has_value();
	const bool ir = state.ir_camera.mode != kIRDisabled;
	const bool motion = true; // UseMotion();

	InputReportId report_type;
	if (extension && ir && motion)
		report_type = kDataCoreAccIRExt;
	else if (extension && ir)
		report_type = kDataCoreIRExt;
	else if (extension && motion)
		report_type = kDataCoreAccExt;
	else if (ir && motion)
		report_type = kDataCoreAccIR;
	else if (extension)
		report_type = kDataCoreExt19;
	else if (ir)
		report_type = kDataCoreAccIR;
	else if (motion)
		report_type = kDataCoreAcc;
	else
		report_type = kDataCore;

    cemuLog_logDebug(LogType::Force,"Setting report type to {}", report_type);
	send_packet(index, {kType, 0x04, report_type});

	read_lock.unlock();
	if (m_wiimotes[index].extensions.IsReady()) set_ir_camera(index, true);
}

IRMode WiimoteControllerProvider::set_ir_camera(size_t index, bool state)
{
	std::unique_lock state_lock(m_wiimotes[index].mutex);
	auto& wiimote_state = m_wiimotes[index].state;

	IRMode mode;
	if (!state)
		mode = kIRDisabled;
	else
	{
		mode = wiimote_state.m_extension.index() == 0 && !wiimote_state.m_motion_plus ? kExtendedIR : kBasicIR;
	}

	if (wiimote_state.ir_camera.mode == mode)
		return mode;

	wiimote_state.ir_camera.mode = mode;
	state_lock.unlock();

	const uint8_t data = state ? 0x04 : 0x00;
	send_packet(index, {kIR, data});
	send_packet(index, {kIR2, data});
	if (state)
	{
		send_write_packet(index, kRegisterMemory, kRegisterIR, {0x08});
		send_write_packet(index, kRegisterMemory, kRegisterIRSensitivity1,
		                  {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0xaa, 0x00, 0x64});
		send_write_packet(index, kRegisterMemory, kRegisterIRSensitivity2, {0x63, 0x03});
		send_write_packet(index, kRegisterMemory, kRegisterIRMode, {(uint8)mode});
		send_write_packet(index, kRegisterMemory, kRegisterIR, {0x08});
	}

	update_report_type(index);
	return mode;
}

void WiimoteControllerProvider::send_packet(size_t index, std::vector<uint8> data)
{
	cemu_assert(data.size() > 1);

	// The writer validates the index under m_device_mutex. The reader also
	// queues commands while holding that mutex, so do not acquire it recursively.
	std::unique_lock lock(m_writer_mutex);
	m_write_queue.emplace_back(index, data);
	m_writer_cond.notify_one();
}

void WiimoteControllerProvider::send_read_packet(size_t index, MemoryType type, RegisterAddress address, uint16 size)
{
	std::vector<uint8> data(7);
	data[0] = kReadMemory;
	data[1] = type;
	*(betype<uint32>*)(data.data() + 2) = (address & 0xFFFFFF) << 8; // only uint24
	*(betype<uint16>*)(data.data() + 2 + 3) = size;

	send_packet(index, std::move(data));
}

void WiimoteControllerProvider::send_write_packet(size_t index, MemoryType type, RegisterAddress address,
                                                  const std::vector<uint8>& data)
{
	cemu_assert(data.size() <= 16);
	std::vector<uint8> packet(6 + 16);
	packet[0] = kWriteMemory;
	packet[1] = type;
	*(betype<uint32>*)(packet.data() + 2) = (address & 0xFFFFFF) << 8; // only uint24
	*(packet.data() + 2 + 3) = (uint8)data.size();
	std::copy(data.begin(), data.end(), packet.data() + 2 + 3 + 1);
	send_packet(index, std::move(packet));
}
