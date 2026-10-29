#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "Cafe/OS/libs/padscore/padscore.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/OS/libs/coreinit/coreinit_Alarm.h"
#include "Cafe/OS/libs/coreinit/coreinit_SystemInfo.h"
#include "WindowSystem.h"
#include "input/InputManager.h"
#ifdef SUPPORTS_WIIMOTE
#include "input/api/Wiimote/NativeWiimoteController.h"
#endif

// KPAD

enum class KPAD_ERROR : sint32
{
	NONE = 0,
	NO_SAMPLE_DATA = -1,
	NO_CONTROLLER = -2,
	NOT_INITIALIZED = -5,
};

// WPAD (Wii Controller stuff)

enum WPADRumble
{
	kStopRumble = 0,
	kStartRumble = 1,
};

enum class WPADBatteryLevel : uint8
{
	FULL = 4,
};

enum class WPADLed : uint8
{
	CHAN0 = (1 << 0),
	CHAN1 = (1 << 1),
	CHAN2 = (1 << 2),
	CHAN3 = (1 << 3),
};

namespace padscore 
{
	enum WPADState_t
	{
		kWPADStateMaster = 0,
		kWPADStateShutdown,
		kWPADStateInitializing,
		kWPADStateAcquired,
		kWPADStateReleased,
	};
	KPADUnifiedWpadStatus_t* g_kpad_ringbuffer = nullptr;
	uint32 g_kpad_ringbuffer_length = 0;
	bool g_wpad_callback_by_kpad = false;

	WPADState_t g_wpad_state = kWPADStateMaster;

	struct {
		SysAllocator<coreinit::OSAlarm_t> alarm;
		bool kpad_initialized = false;

		struct WPADData
		{
			MEMPTR<void> extension_callback;
			MEMPTR<void> connectCallback;
			MEMPTR<void> sampling_callback;
			MEMPTR<void> dpd_callback;
            MEMPTR<void> mpls_callback;
            int mpls_request = -1;
            bool mpls_started = false;
            std::chrono::steady_clock::time_point mpls_deadline{};
			bool dpd_enabled = true;

			bool disconnectCalled = false;
			bool connectionReported = false;
			WPADDeviceType lastExtensionType = kWAPDevNotFound;

			BtnRepeat btn_repeat{};
		} controller_data[InputManager::kMaxWPADControllers] = {};

		int max_controllers = kWPADMaxControllers; // max bt controllers?
	} g_padscore;
}


#pragma region WPAD

void padscoreExport_WPADGetStatus(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::InputAPI, "WPADGetStatus()");
	uint32 status = 1;
	osLib_returnFromFunction(hCPU, status);
}

void padscoreExport_WPADGetBatteryLevel(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::InputAPI, "WPADGetBatteryLevel()");
	osLib_returnFromFunction(hCPU, (uint32)WPADBatteryLevel::FULL);
}

void padscoreExport_WPADProbe(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamPtr(type, uint32be, 1);

	cemuLog_log(LogType::InputAPI, "WPADProbe({})", channel);

	if(const auto controller = InputManager::instance().get_wpad_controller(channel))
	{
		if(type)
			*type = controller->get_device_type();

		osLib_returnFromFunction(hCPU, WPAD_ERR_NONE);
	}
	else
	{
		if(type)
			*type = 253;

		osLib_returnFromFunction(hCPU, WPAD_ERR_NO_CONTROLLER);
	}
}

typedef struct
{
	uint32be dpd;
	uint32be speaker;
	uint32be attach;
	uint32be lowBat;
	uint32be nearempty;
	betype<WPADBatteryLevel> batteryLevel;
	betype<WPADLed> led;
	uint8 protocol;
	uint8 firmware;
}WPADInfo_t;

static_assert(sizeof(WPADInfo_t) == 0x18); // unsure

// Keep synchronous and asynchronous capability queries consistent with the
// native controller. In particular, an active built-in MotionPlus occupies the
// extension bus even when there is nothing plugged into the external port.
static void FillWPADInfo(WPADInfo_t& info, WPADController& controller, uint32 channel)
{
    info = {};
    info.batteryLevel = WPADBatteryLevel::FULL;
    info.led = static_cast<WPADLed>(1u << channel);
    const auto type = controller.get_device_type();
    info.attach = controller.is_mpls_attached() || type == kWAPDevFreestyle || type == kWAPDevClassic;
#ifdef SUPPORTS_WIIMOTE
    if (const auto native = controller.get_native_wiimote())
    {
        const auto state = native->get_native_state();
        info.attach = (state.flags & kExtensionConnected) != 0 || state.m_motion_plus.has_value() || state.m_extension.index() != 0;
        info.dpd = state.ir_camera.mode != kIRDisabled;
        info.speaker = (state.flags & 4) != 0;
        info.lowBat = (state.flags & 1) != 0;
        info.nearempty = info.lowBat;
        info.led = static_cast<WPADLed>(state.flags >> 4);
    }
#endif
}

void padscoreExport_WPADGetInfoAsync(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamStructPtr(wpadInfo, WPADInfo_t, 1);
	ppcDefineParamMPTR(callbackFunc, 2);
	cemuLog_log(LogType::InputAPI, "WPADGetInfoAsync({}, 0x{:08x}, 0x{:08x})", channel, memory_getVirtualOffsetFromPointer(wpadInfo), callbackFunc);

	if (channel < InputManager::kMaxWPADControllers)
	{
		if (const auto controller = InputManager::instance().get_wpad_controller(channel))
		{
			FillWPADInfo(*wpadInfo, *controller, channel);

			if(callbackFunc != MPTR_NULL)
				coreinitAsyncCallback_add(callbackFunc, 2, channel, (uint32)KPAD_ERROR::NONE);

			osLib_returnFromFunction(hCPU, WPAD_ERR_NONE);
			return;
		}
	}
	else
	{
		debugBreakpoint();
	}

	if (callbackFunc != MPTR_NULL)
		coreinitAsyncCallback_add(callbackFunc, 2, channel, (uint32)KPAD_ERROR::NO_CONTROLLER);

	osLib_returnFromFunction(hCPU, WPAD_ERR_NO_CONTROLLER);
}

void padscoreExport_WPADRead(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamPtr(wpadStatus, WPADStatus_t, 1);
	cemuLog_log(LogType::InputAPI, "WPADRead({}, {:x})", channel, memory_getVirtualOffsetFromPointer(wpadStatus));

	if (channel < InputManager::kMaxWPADControllers)
	{
		if(const auto controller = InputManager::instance().get_wpad_controller(channel) )
		{
			controller->WPADRead(wpadStatus);
		}
	}
	else
	{
		debugBreakpoint();
	}
	osLib_returnFromFunction(hCPU, WPAD_ERR_NO_CONTROLLER);
}

void padscoreExport_WPADSetDataFormat(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamU32(fmt, 1);
	cemuLog_log(LogType::InputAPI, "WPADSetDataFormat({}, {})", channel, fmt);

	if (channel < InputManager::kMaxWPADControllers)
	{
		if (const auto controller = InputManager::instance().get_wpad_controller(channel))
			controller->set_data_format((WPADDataFormat)fmt);
	}

	osLib_returnFromFunction(hCPU, 0);
}

void padscoreExport_WPADGetDataFormat(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	cemuLog_log(LogType::InputAPI, "WPADGetDataFormat({})", channel);

	sint32 dataFormat = kDataFormat_CORE;
	if (channel < InputManager::kMaxWPADControllers)
	{
		if (const auto controller = InputManager::instance().get_wpad_controller(channel))
			dataFormat = controller->get_data_format();
	}
	else
	{
		debugBreakpoint();
	}

	osLib_returnFromFunction(hCPU, dataFormat);
}

void padscoreExport_WPADGetInfo(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamStructPtr(wpadInfo, WPADInfo_t, 1);
	cemuLog_log(LogType::InputAPI, "WPADGetInfo({}, 0x{:08x})", channel, memory_getVirtualOffsetFromPointer(wpadInfo));

	if (channel < InputManager::kMaxWPADControllers)
	{
		if (const auto controller = InputManager::instance().get_wpad_controller(channel))
		{
			FillWPADInfo(*wpadInfo, *controller, channel);

			osLib_returnFromFunction(hCPU, WPAD_ERR_NONE);
			return;
		}
	}
	else
	{
		debugBreakpoint();
	}

	osLib_returnFromFunction(hCPU, WPAD_ERR_NO_CONTROLLER);
}


void padscoreExport_WPADIsMotorEnabled(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::InputAPI, "WPADIsMotorEnabled()");
	osLib_returnFromFunction(hCPU, TRUE);
}

void padscoreExport_WPADControlMotor(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamU32(command, 1);
	cemuLog_log(LogType::InputAPI, "WPADControlMotor({}, {})", channel, command);
	
	if (channel < InputManager::kMaxWPADControllers)
	{
		if (const auto controller = InputManager::instance().get_wpad_controller(channel))
		{
			if( command == kStartRumble )
				controller->start_rumble();
			else
				controller->stop_rumble();
		}
	}
	else
	{
		debugBreakpoint();
	}

	osLib_returnFromFunction(hCPU, 0);
}
#pragma endregion



#pragma region KPAD
void padscoreExport_KPADGetUnifiedWpadStatus(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamPtr(status, KPADUnifiedWpadStatus_t, 1);
	ppcDefineParamU32(count, 2);

	cemuLog_log(LogType::InputAPI, "KPADGetUnifiedWpadStatus({}, 0x{:08x}, 0x{:x})", channel, MEMPTR<void>(status).GetMPTR(), count);

	if (channel < InputManager::kMaxWPADControllers)
	{
		memset(status, 0x00, sizeof(KPADUnifiedWpadStatus_t) * count);

		if (const auto controller = InputManager::instance().get_wpad_controller(channel))
		{
			switch (controller->get_data_format())
			{
			case WPAD_FMT_CORE:
			case WPAD_FMT_CORE_ACC:
			case WPAD_FMT_CORE_ACC_DPD:
			{
				status->fmt = controller->get_data_format();
				controller->WPADRead(&status->u.core);
				break;
			}
			default:
				debugBreakpoint();
			}
		}
		else
		{
			status->u.core.err = WPAD_ERR_NO_CONTROLLER;
		}
	}
	else
	{
		debugBreakpoint();
	}

	osLib_returnFromFunction(hCPU, 0);
}

void padscoreExport_KPADSetBtnRepeat(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	float delaySec = hCPU->fpr[1].fpr;
	float pulseSec = hCPU->fpr[2].fpr;
	cemuLog_log(LogType::InputAPI, "KPADSetBtnRepeat({}, {}, {})", channel, delaySec, pulseSec);

	if (channel < InputManager::kMaxWPADControllers)
	{
		padscore::g_padscore.controller_data[channel].btn_repeat = { (int)delaySec, (int)pulseSec };
	}

	osLib_returnFromFunction(hCPU, 0);
}

void padscoreExport_KPADSetSamplingCallback(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamMPTR(callback, 1);

	cemuLog_log(LogType::InputAPI, "KPADSetSamplingCallback({}, 0x{:x})", channel, callback);

	if (channel >= InputManager::kMaxWPADControllers)
	{
		debugBreakpoint();
		osLib_returnFromFunction(hCPU, MPTR_NULL);
		return;
	}

	const auto old_callback = padscore::g_padscore.controller_data[channel].sampling_callback;
	padscore::g_padscore.controller_data[channel].sampling_callback = callback;
	osLib_returnFromFunction(hCPU, old_callback.GetMPTR());
}

void padscoreExport_WPADControlDpd(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamU32(command, 1);
	ppcDefineParamMPTR(callback, 2);

	cemuLog_log(LogType::InputAPI, "WPADControlDpd({}, {}, 0x{:x})", channel, command, callback);

	if (channel < InputManager::kMaxWPADControllers)
	{
		if (const auto controller = InputManager::instance().get_wpad_controller(channel))
		{
			padscore::g_padscore.controller_data[channel].dpd_callback = callback;
			if (callback)
			{
				coreinitAsyncCallback_add(callback, 2, channel, WPAD_ERR_NONE);
			}

			osLib_returnFromFunction(hCPU, WPAD_ERR_NONE);
			return;
		}
	}

	osLib_returnFromFunction(hCPU, WPAD_ERR_NO_CONTROLLER);
}

void padscoreExport_WPADSetExtensionCallback(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamMPTR(callback, 1);

	cemuLog_log(LogType::InputAPI, "WPADSetExtensionCallback({}, 0x{:x})", channel, callback);

	if (channel >= InputManager::kMaxWPADControllers)
	{
		debugBreakpoint();
		osLib_returnFromFunction(hCPU, MPTR_NULL);
		return;
	}

	const auto old_callback = padscore::g_padscore.controller_data[channel].extension_callback;
	padscore::g_padscore.controller_data[channel].extension_callback = callback;
	padscore::g_padscore.controller_data[channel].lastExtensionType = kWAPDevNotFound;
	osLib_returnFromFunction(hCPU, old_callback.GetMPTR());
}

void padscoreExport_KPADSetConnectCallback(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamMPTR(callback, 1);

	cemuLog_log(LogType::InputAPI, "KPADSetConnectCallback({}, 0x{:x})",channel, callback);

	if (channel >= InputManager::kMaxWPADControllers)
	{
		debugBreakpoint();
		osLib_returnFromFunction(hCPU, MPTR_NULL);
		return;
	}

	const auto old_callback = padscore::g_padscore.controller_data[channel].connectCallback;
	padscore::g_padscore.controller_data[channel].connectCallback = callback;
	padscore::g_padscore.controller_data[channel].disconnectCalled = false;
	padscore::g_padscore.controller_data[channel].connectionReported = false;
	if (const auto controller = InputManager::instance().get_wpad_controller(channel))
		controller->m_status = WPADController::ConnectCallbackStatus::ReportConnect;
	osLib_returnFromFunction(hCPU, old_callback.GetMPTR());
}

uint64 g_kpadLastRead[InputManager::kMaxWPADControllers] = {0};
bool g_kpadIsInited = true;

sint32 _KPADRead(uint32 channel, KPADStatus_t* samplingBufs, uint32 length, betype<KPAD_ERROR>* errResult)
{

	if (channel >= InputManager::kMaxWPADControllers)
	{
		debugBreakpoint();
		return 0;
	}
	
	if (g_kpadIsInited == false)
	{
		if (errResult)
			*errResult = KPAD_ERROR::NOT_INITIALIZED;

		return 0;
	}

	const auto controller = InputManager::instance().get_wpad_controller(channel);
	if (!controller)
	{
		if (errResult)
			*errResult = KPAD_ERROR::NO_CONTROLLER;

		return 0;
	}

	// On console new input samples are only received every few ms and calling KPADRead(Ex) clears the internal queue regardless of length value
	// thus calling KPADRead(Ex) again too soon on the same channel will result in no data being returned
	// Games that depend on this: Affordable Space Adventures
	uint64 currentTime = coreinit::OSGetTime();
	uint64 timeDif = currentTime - g_kpadLastRead[channel];
	if(length == 0 || timeDif < coreinit::EspressoTime::ConvertNsToTimerTicks(1000000))
	{
		if (errResult)
			*errResult = KPAD_ERROR::NO_SAMPLE_DATA;
		return 0;
	}
	g_kpadLastRead[channel] = currentTime;

	memset(samplingBufs, 0x00, sizeof(KPADStatus_t));
	samplingBufs->wpadErr = WPAD_ERR_NONE;
	samplingBufs->data_format = controller->get_data_format();
	samplingBufs->devType = controller->get_device_type();
	if(!WindowSystem::InputConfigWindowHasFocus())
	{
		const auto btn_repeat = padscore::g_padscore.controller_data[channel].btn_repeat;
		controller->KPADRead(*samplingBufs, btn_repeat);
	}

	if (errResult)
		*errResult = KPAD_ERROR::NONE;

	return 1;
}

void padscoreExport_KPADReadEx(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamPtr(kpadStatus, KPADStatus_t, 1);
	ppcDefineParamU32(length, 2);
	ppcDefineParamPtr(errResult, betype<KPAD_ERROR>, 3);
	cemuLog_log(LogType::InputAPI, "KPADReadEx({}, 0x{:x})", channel, length);

	sint32 samplesRead = _KPADRead(channel, kpadStatus, length, errResult);
	osLib_returnFromFunction(hCPU, samplesRead);
}

void padscoreExport_KPADRead(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamPtr(kpadStatus, KPADStatus_t, 1);
	ppcDefineParamU32(length, 2);
	cemuLog_log(LogType::InputAPI, "KPADRead({}, 0x{:x})", channel, length);

	sint32 samplesRead = _KPADRead(channel, kpadStatus, length, nullptr);
	osLib_returnFromFunction(hCPU, samplesRead);
}






#pragma endregion



namespace padscore
{
	void export_KPADEnableDPD(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamS32(channel, 0);
		cemuLog_log(LogType::InputAPI, "KPADEnableDPD({})", channel);
		cemu_assert_debug(0 <= channel && channel < InputManager::kMaxWPADControllers);

		if(const auto controller = InputManager::instance().get_wpad_controller(channel))
		{
			if (controller->get_data_format() != kDataFormat_FREESTYLE && controller->get_data_format() != 0x1F)
				g_padscore.controller_data[channel].dpd_enabled = true;
		}
		

		osLib_returnFromFunction(hCPU, 0);
	}

	void export_KPADGetMplsWorkSize(PPCInterpreter_t* hCPU)
	{
		cemuLog_log(LogType::InputAPI, "KPADGetMplsWorkSize()");
		osLib_returnFromFunction(hCPU, 0x5FE0);
	}

	void KPADInitEx(KPADUnifiedWpadStatus_t ring_buffer[], uint32 length)
	{
		if (g_padscore.kpad_initialized)
			return;

		for (uint32 i = 0; i < InputManager::kMaxWPADControllers; i++)
		{
			g_padscore.controller_data[i].dpd_enabled = true;


		}

		g_kpad_ringbuffer = ring_buffer;
		g_kpad_ringbuffer_length = length;
		g_padscore.kpad_initialized = true;
	}

	void export_KPADInit(PPCInterpreter_t* hCPU)
	{
		cemuLog_log(LogType::InputAPI, "KPADInit()");
		KPADInitEx(nullptr, 0);
		osLib_returnFromFunction(hCPU, 0);
	}

	void export_KPADInitEx(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamMEMPTR(ring_buffer, KPADUnifiedWpadStatus_t, 0);
		ppcDefineParamU32(length, 1);
		cemuLog_log(LogType::InputAPI, "KPADInitEx(0x{:08x}, 0x{:x})", ring_buffer.GetMPTR(), length);
		KPADInitEx(ring_buffer.GetPtr(), length);
		osLib_returnFromFunction(hCPU, 0);
	}

	void WPADSetCallbackByKPAD(bool state)
	{
		g_wpad_callback_by_kpad = state;
	}

	void export_WPADSetCallbackByKPAD(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamU32(state, 0);
		cemuLog_log(LogType::InputAPI, "WPADSetCallbackByKPAD({})", state);
		WPADSetCallbackByKPAD(state);
		osLib_returnFromFunction(hCPU, 0);
	}

	void export_KPADGetMaxControllers(PPCInterpreter_t* hCPU)
	{
		cemuLog_log(LogType::InputAPI, "KPADGetMaxControllers()");
		sint32 max_controllers = g_padscore.max_controllers;
		osLib_returnFromFunction(hCPU, max_controllers);
	}

	bool WPADIsUsedCallbackByKPAD()
	{
		return g_wpad_callback_by_kpad == TRUE;
	}

	void* WPADSetSamplingCallback(sint32 channel, void* callback)
	{
		cemu_assert_debug(0 <= channel && channel < kKPADMaxControllers);

		const auto result = g_padscore.controller_data[channel].sampling_callback;
		g_padscore.controller_data[channel].sampling_callback = callback;
		return result.GetPtr();
	}

	void export_KPADSetMaxControllers(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamU32(new_max_count, 0);
		cemuLog_log(LogType::InputAPI, "KPADSetMaxControllers({})", new_max_count);

		if (new_max_count != kKPADMaxControllers && new_max_count != kWPADMaxControllers)
		{
			debugBreakpoint();
			osLib_returnFromFunction(hCPU, -4);
			return;
		}


		const uint32 max_controllers = g_padscore.max_controllers;
		if (max_controllers == new_max_count)
		{
			osLib_returnFromFunction(hCPU, 0);
			return;
		}

		WPADSetCallbackByKPAD(FALSE);
		for (sint32 i = 0; i < kKPADMaxControllers; i++)
		{
			//WPADSetSamplingCallback(i, 0); TODO
		}

		g_padscore.max_controllers = new_max_count;
		
		WPADSetCallbackByKPAD(true);
		osLib_returnFromFunction(hCPU, new_max_count);
	}

	void export_WPADInit()
	{
		if (g_wpad_state != kWPADStateShutdown)
		{
			if (g_wpad_state == kWPADStateInitializing || g_wpad_state == kWPADStateAcquired)
				return;
		}
		g_wpad_state = kWPADStateInitializing;
	}


	enum class WPADStatus : sint32
	{
		Success = 0,
		NoController = -1,
		Busy = -2,
	};

	WPADStatus WPADIsMplsAttached(sint32 index, uint32be* attached, void* callback)
	{
		if (index < 0 || index >= kKPADMaxControllers || !attached)
			return WPADStatus::NoController;

		const auto controller = InputManager::instance().get_wpad_controller(index);
		*attached = controller && controller->is_mpls_attached();

		if (callback)
			PPCCoreCallback(MEMPTR(callback), index, controller ? WPADStatus::Success : WPADStatus::NoController);

		return WPADStatus::Success;
	}

    // Public homebrew ABI reference: devkitPro/wut include/padscore/kpad.h.
    // HLE owns its motion state, so no guest work buffer is needed.
    void KPADSetMplsWorkarea(void*) {}

    bool ValidChannel(sint32 channel) { return channel >= 0 && channel < InputManager::kMaxWPADControllers; }

    void KPADSetControlMplsCallback(sint32 channel, MEMPTR<void> callback)
    {
        if (ValidChannel(channel)) g_padscore.controller_data[channel].mpls_callback = callback;
    }

    sint32 KPADGetMplsStatus(sint32 channel)
    {
        if (!ValidChannel(channel)) return 0;
        const auto controller = InputManager::instance().get_wpad_controller(channel);
        if (!controller || !controller->m_mpls_enabled || !controller->is_mpls_attached()) return 0;
        const auto type = controller->get_device_type();
        return type == kWAPDevMPLSFreeStyle ? 5 : type == kWAPDevMPLSClassic ? 7 : 4;
    }

    void KPADEnableMpls(sint32 channel, sint32 mode)
    {
        if (!ValidChannel(channel) || (mode != 0 && mode != 4 && mode != 5 && mode != 7)) return;
        if (const auto controller = InputManager::instance().get_wpad_controller(channel))
        {
            controller->m_mpls_enabled = mode != 0;
            controller->set_data_format(mode ? kDataFormat_MPLS : controller->get_default_data_format());
        }
        auto& state = g_padscore.controller_data[channel];
        state.mpls_request = mode;
        state.mpls_started = false;
        state.mpls_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    }

    void KPADDisableMpls(sint32 channel) { KPADEnableMpls(channel, 0); }

    sint32 WPADIsMplsIntegrated(sint32 channel)
    {
        if (!ValidChannel(channel)) return -1;
        const auto controller = InputManager::instance().get_wpad_controller(channel);
        if (!controller) return -1;
#ifdef SUPPORTS_WIIMOTE
        if (const auto native = controller->get_native_wiimote())
        {
            const auto state = native->get_native_state();
            return state.m_motion_plus && state.m_motion_plus->integrated;
        }
#endif
        return 0;
    }

    void KPADStartMplsCalibration(sint32 channel)
    {
        if (!ValidChannel(channel)) return;
#ifdef SUPPORTS_WIIMOTE
        if (const auto controller = InputManager::instance().get_wpad_controller(channel))
            if (const auto native = controller->get_native_wiimote()) native->calibrate_gyro(true);
#endif
    }

    void KPADStopMplsCalibration(sint32 channel)
    {
        if (!ValidChannel(channel)) return;
#ifdef SUPPORTS_WIIMOTE
        if (const auto controller = InputManager::instance().get_wpad_controller(channel))
            if (const auto native = controller->get_native_wiimote()) native->calibrate_gyro(false);
#endif
    }

    float KPADWorkMplsCalibration(sint32 channel)
    {
        if (!ValidChannel(channel)) return -1;
#ifdef SUPPORTS_WIIMOTE
        if (const auto controller = InputManager::instance().get_wpad_controller(channel))
            if (const auto native = controller->get_native_wiimote())
            {
                const auto state = native->get_native_state();
                if (state.m_motion_plus) return state.m_motion_plus->calibration_progress;
            }
#endif
        return -1;
    }

    void KPADSetMplsAngle(sint32 channel, float pitch, float yaw, float roll)
    {
        if (!ValidChannel(channel) || !std::isfinite(pitch) || !std::isfinite(yaw) || !std::isfinite(roll)) return;
        if (const auto controller = InputManager::instance().get_wpad_controller(channel))
            controller->SetMotionPlusAngle({pitch, yaw, roll});
    }

    void KPADSetMplsDirection(sint32 channel, const KPADMPDir_t* direction)
    {
        if (!ValidChannel(channel) || !direction) return;
        const glm::mat3 matrix{
            glm::vec3{direction->X.x, direction->X.y, direction->X.z},
            glm::vec3{direction->Y.x, direction->Y.y, direction->Y.z},
            glm::vec3{direction->Z.x, direction->Z.y, direction->Z.z}};
        for (int column = 0; column < 3; ++column)
            for (int row = 0; row < 3; ++row) if (!std::isfinite(matrix[column][row])) return;
        if (const auto controller = InputManager::instance().get_wpad_controller(channel))
            controller->SetMotionPlusDirection(matrix);
    }

    void KPADSetMplsMagnification(sint32 channel, float pitch, float yaw, float roll)
    {
        if (!ValidChannel(channel) || !std::isfinite(pitch) || !std::isfinite(yaw) || !std::isfinite(roll)) return;
        if (const auto controller = InputManager::instance().get_wpad_controller(channel))
            controller->m_mpls_velocity_scale = {pitch, yaw, roll};
    }

    void KPADSetMplsDirReviseBase(sint32 channel, const KPADMPDir_t* base)
    {
        if (!ValidChannel(channel) || !base) return;
        const glm::mat3 matrix{
            glm::vec3{base->X.x, base->X.y, base->X.z},
            glm::vec3{base->Y.x, base->Y.y, base->Y.z},
            glm::vec3{base->Z.x, base->Z.y, base->Z.z}};
        if (const auto controller = InputManager::instance().get_wpad_controller(channel))
            controller->m_mpls_ir_heading.setBase(matrix);
    }

    void KPADEnableMplsDpdRevise(sint32 channel)
    {
        if (!ValidChannel(channel)) return;
        if (const auto controller = InputManager::instance().get_wpad_controller(channel))
            controller->m_mpls_ir_heading.enabled = true;
    }

    void KPADDisableMplsDpdRevise(sint32 channel)
    {
        if (!ValidChannel(channel)) return;
        if (const auto controller = InputManager::instance().get_wpad_controller(channel))
            controller->m_mpls_ir_heading.enabled = false;
    }

    sint32 KPADIsEnableMplsDpdRevise(sint32 channel)
    {
        if (!ValidChannel(channel)) return 0;
        const auto controller = InputManager::instance().get_wpad_controller(channel);
        return controller && controller->m_mpls_ir_heading.enabled;
    }

    void KPADSetMplsDpdReviseParam(sint32 channel, float weight)
    {
        if (!ValidChannel(channel) || !std::isfinite(weight)) return;
        if (const auto controller = InputManager::instance().get_wpad_controller(channel))
            controller->m_mpls_ir_heading.weight = std::clamp(weight, 0.0f, 1.0f);
    }

    void KPADGetMplsDpdReviseParam(sint32 channel, float32be* weight)
    {
        if (!ValidChannel(channel) || !weight) return;
        if (const auto controller = InputManager::instance().get_wpad_controller(channel))
            *weight = controller->m_mpls_ir_heading.weight;
    }

    void KPADInitMplsDpdReviseParam(sint32 channel) { KPADSetMplsDpdReviseParam(channel, .05f); }

    void KPADResetMpls(sint32 channel)
    {
        if (!ValidChannel(channel)) return;
        if (const auto controller = InputManager::instance().get_wpad_controller(channel))
        {
            controller->SetMotionPlusAngle({});
            controller->SetMotionPlusDirection(glm::mat3{1.0f});
            controller->m_mpls_velocity_scale = glm::vec3{1.0f};
            controller->m_mpls_ir_heading = {};
        }
    }

	struct WPADAcc
	{
		sint32be x;
		sint32be y;
		sint32be z;
	};
	void WPADGetAccGravityUnit(sint32 index, uint32 type, WPADAcc* acc)
	{
		if (index >= kKPADMaxControllers)
			return;

		// default values for wiimote usually are around 99
		acc->x = 99;
		acc->y = 99;
		acc->z = 99;
	}


#pragma endregion

	void TickFunction(PPCInterpreter_t* hCPU)
	{
		auto& instance = InputManager::instance();
        // Complete MotionPlus requests on the emulation thread, including a
        // bounded wait while a newly connected physical remote initializes.
        for (sint32 channel = 0; channel < InputManager::kMaxWPADControllers; ++channel)
        {
            auto& state = g_padscore.controller_data[channel];
            if (state.mpls_request < 0) continue;
            if (!state.mpls_started)
            {
                state.mpls_started = true;
                if (state.mpls_callback) PPCCoreCallback(state.mpls_callback, channel, 0);
                continue;
            }
            const auto controller = instance.get_wpad_controller(channel);
            const bool finished = !controller || state.mpls_request == 0 || controller->is_mpls_attached();
            if (!finished && std::chrono::steady_clock::now() < state.mpls_deadline) continue;
            const int result = finished ? 1 : state.mpls_request == 5 ? -2 : state.mpls_request == 7 ? -3 : -1;
            state.mpls_request = -1;
            if (state.mpls_callback) PPCCoreCallback(state.mpls_callback, channel, result);
        }
		// test for connected/disconnected controllers
		for (auto i = 0; i < InputManager::kMaxWPADControllers; ++i)
		{
			if (g_padscore.controller_data[i].connectCallback) 
			{
				if(!g_padscore.controller_data[i].disconnectCalled)
				{
					g_padscore.controller_data[i].disconnectCalled = true;
					cemuLog_log(LogType::InputAPI, "Calling WPADConnectCallback({}, {})", i, WPAD_ERR_NO_CONTROLLER);
					PPCCoreCallback(g_padscore.controller_data[i].connectCallback, i, WPAD_ERR_NO_CONTROLLER);
					continue;
				}

				if (const auto controller = instance.get_wpad_controller(i))
				{
					if (controller->m_status == WPADController::ConnectCallbackStatus::ReportDisconnect || controller->was_home_button_down()) // fixed!
					{
						controller->m_status = WPADController::ConnectCallbackStatus::ReportConnect;
						g_padscore.controller_data[i].connectionReported = false;

						cemuLog_log(LogType::InputAPI, "Calling WPADConnectCallback({}, {})", i, WPAD_ERR_NO_CONTROLLER);
						PPCCoreCallback(g_padscore.controller_data[i].connectCallback, i, WPAD_ERR_NO_CONTROLLER);
						
					}
					else if (controller->m_status == WPADController::ConnectCallbackStatus::ReportConnect) 
					{
						controller->m_status = WPADController::ConnectCallbackStatus::None;
						g_padscore.controller_data[i].connectionReported = true;
						cemuLog_log(LogType::InputAPI, "Calling WPADConnectCallback({}, {})", i, WPAD_ERR_NONE);
						PPCCoreCallback(g_padscore.controller_data[i].connectCallback, i, WPAD_ERR_NONE);
					}
				}
				else if (g_padscore.controller_data[i].connectionReported)
				{
					// Hot-unplug can remove the WPAD object entirely (including a BLE
					// bridge loss), so there is no object left to carry m_status.
					g_padscore.controller_data[i].connectionReported = false;
					PPCCoreCallback(g_padscore.controller_data[i].connectCallback, i, WPAD_ERR_NO_CONTROLLER);
				}
			}
		}

		// test for connected/disconnected extensions
		for (auto i = 0; i < InputManager::kMaxWPADControllers; ++i)
		{
			if (g_padscore.controller_data[i].extension_callback) 
			{
				if (const auto controller = instance.get_wpad_controller(i))
				{
					const auto deviceType = controller->get_device_type();
					if (controller->m_extension_status == WPADController::ConnectCallbackStatus::ReportConnect ||
						g_padscore.controller_data[i].lastExtensionType != deviceType)
					{
						g_padscore.controller_data[i].lastExtensionType = deviceType;
						controller->m_extension_status = WPADController::ConnectCallbackStatus::None;
						cemuLog_log(LogType::InputAPI, "Calling WPADextensionCallback({})", i);
						PPCCoreCallback(g_padscore.controller_data[i].extension_callback, i, deviceType);
					}
				}
			}
		}

		// call sampling callback
		for (auto i = 0; i < InputManager::kMaxWPADControllers; ++i)
		{
			if (g_padscore.controller_data[i].sampling_callback)
			{
				if (const auto controller = instance.get_wpad_controller(i))
				{
					cemuLog_log(LogType::InputAPI, "Calling WPADsamplingCallback({})", i);
					PPCCoreCallback(g_padscore.controller_data[i].sampling_callback, i);
				}
			}
		}

		osLib_returnFromFunction(hCPU, 0);
	}
	void start()
	{
		OSCreateAlarm(&g_padscore.alarm);
		const uint64 start_tick = coreinit::OSGetTime();
		const uint64 period_tick = coreinit::EspressoTime::GetTimerClock() / 200; // every 5ms
		MPTR handler = PPCInterpreter_makeCallableExportDepr(TickFunction);
		OSSetPeriodicAlarm(&g_padscore.alarm, start_tick, period_tick, handler);
	}

	class : public COSModule
	{
		public:
		std::string_view GetName() override
		{
			return "padscore";
		}

		void RPLMapped() override
		{
			cafeExportRegister("padscore", WPADIsMplsAttached, LogType::InputAPI);
            cafeExportRegister("padscore", KPADSetMplsWorkarea, LogType::InputAPI);
            cafeExportRegister("padscore", KPADSetControlMplsCallback, LogType::InputAPI);
            cafeExportRegister("padscore", KPADGetMplsStatus, LogType::InputAPI);
            cafeExportRegister("padscore", KPADEnableMpls, LogType::InputAPI);
            cafeExportRegister("padscore", KPADDisableMpls, LogType::InputAPI);
            cafeExportRegister("padscore", WPADIsMplsIntegrated, LogType::InputAPI);
            cafeExportRegister("padscore", KPADStartMplsCalibration, LogType::InputAPI);
            cafeExportRegister("padscore", KPADStopMplsCalibration, LogType::InputAPI);
            cafeExportRegister("padscore", KPADWorkMplsCalibration, LogType::InputAPI);
            cafeExportRegister("padscore", KPADSetMplsAngle, LogType::InputAPI);
            cafeExportRegister("padscore", KPADSetMplsDirection, LogType::InputAPI);
            cafeExportRegister("padscore", KPADSetMplsMagnification, LogType::InputAPI);
            cafeExportRegister("padscore", KPADSetMplsDirReviseBase, LogType::InputAPI);
            cafeExportRegister("padscore", KPADEnableMplsDpdRevise, LogType::InputAPI);
            cafeExportRegister("padscore", KPADDisableMplsDpdRevise, LogType::InputAPI);
            cafeExportRegister("padscore", KPADIsEnableMplsDpdRevise, LogType::InputAPI);
            cafeExportRegister("padscore", KPADSetMplsDpdReviseParam, LogType::InputAPI);
            cafeExportRegister("padscore", KPADGetMplsDpdReviseParam, LogType::InputAPI);
            cafeExportRegister("padscore", KPADInitMplsDpdReviseParam, LogType::InputAPI);
            cafeExportRegister("padscore", KPADResetMpls, LogType::InputAPI);

			cafeExportRegister("padscore", WPADGetAccGravityUnit, LogType::InputAPI);

			// wpad
			//osLib_addFunction("padscore", "WPADInit", padscore::export_WPADInit);

			// kpad
			osLib_addFunction("padscore", "KPADSetMaxControllers", padscore::export_KPADSetMaxControllers);
			osLib_addFunction("padscore", "KPADGetMaxControllers", padscore::export_KPADGetMaxControllers);
			osLib_addFunction("padscore", "KPADEnableDPD", padscore::export_KPADEnableDPD);
			osLib_addFunction("padscore", "KPADGetMplsWorkSize", padscore::export_KPADGetMplsWorkSize);
			osLib_addFunction("padscore", "KPADInit", padscore::export_KPADInit);
			osLib_addFunction("padscore", "KPADInitEx", padscore::export_KPADInitEx);

			osLib_addFunction("padscore", "KPADSetConnectCallback", padscoreExport_KPADSetConnectCallback);
			osLib_addFunction("padscore", "KPADReadEx", padscoreExport_KPADReadEx);
			osLib_addFunction("padscore", "KPADRead", padscoreExport_KPADRead);
			osLib_addFunction("padscore", "KPADGetUnifiedWpadStatus", padscoreExport_KPADGetUnifiedWpadStatus);
			osLib_addFunction("padscore", "KPADSetSamplingCallback", padscoreExport_KPADSetSamplingCallback);
			osLib_addFunction("padscore", "KPADSetBtnRepeat", padscoreExport_KPADSetBtnRepeat);

			osLib_addFunction("padscore", "WPADGetBatteryLevel", padscoreExport_WPADGetBatteryLevel);
			osLib_addFunction("padscore", "WPADControlMotor", padscoreExport_WPADControlMotor);
			osLib_addFunction("padscore", "WPADIsMotorEnabled", padscoreExport_WPADIsMotorEnabled);
			osLib_addFunction("padscore", "WPADGetStatus", padscoreExport_WPADGetStatus);
			osLib_addFunction("padscore", "WPADProbe", padscoreExport_WPADProbe);
			osLib_addFunction("padscore", "WPADGetInfoAsync", padscoreExport_WPADGetInfoAsync);
			osLib_addFunction("padscore", "WPADGetInfo", padscoreExport_WPADGetInfo);
			osLib_addFunction("padscore", "WPADSetConnectCallback", padscoreExport_KPADSetConnectCallback);
			osLib_addFunction("padscore", "WPADSetDataFormat", padscoreExport_WPADSetDataFormat);
			osLib_addFunction("padscore", "WPADGetDataFormat", padscoreExport_WPADGetDataFormat);
			osLib_addFunction("padscore", "WPADRead", padscoreExport_WPADRead);
			osLib_addFunction("padscore", "WPADSetExtensionCallback", padscoreExport_WPADSetExtensionCallback);
			osLib_addFunction("padscore", "WPADSetSamplingCallback", padscoreExport_KPADSetSamplingCallback);
			osLib_addFunction("padscore", "WPADControlDpd", padscoreExport_WPADControlDpd);

			osLib_addFunction("padscore", "WPADSetCallbackByKPAD", padscore::export_WPADSetCallbackByKPAD);
		};

	}s_COSCoreinitModule;

	COSModule* GetModule()
	{
		return &s_COSCoreinitModule;
	}

}
