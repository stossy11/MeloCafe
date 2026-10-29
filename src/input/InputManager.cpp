#include "input/InputManager.h"
#include "config/ActiveSettings.h"
#include "input/ControllerFactory.h"
#include <boost/property_tree/ini_parser.hpp>
#include <pugixml.hpp>
#include <algorithm>
#include "Cafe/GameProfile/GameProfile.h"
#include "util/EventService.h"

#if BOOST_OS_IOS
#include "input/api/Wiimote/NativeWiimoteController.h"
#include "input/api/Wiimote/BLEWiimoteBridge.h"
#include "input/api/iOS/GCController.h"
#include "input/api/iOS/GCControllerProvider.h"
#include "input/emulated/VPADController.h"
#include "input/emulated/ProController.h"
#include "input/emulated/ClassicController.h"
#include "input/emulated/WiimoteController.h"
#endif

InputManager::InputManager()
{
#if BOOST_OS_IOS
    create_provider<GCControllerProvider>();
    create_provider<WiimoteControllerProvider>();
#else
#if HAS_KEYBOARD
    create_provider<KeyboardControllerProvider>();
#endif
#if HAS_SDL
    create_provider<SDLControllerProvider>();
#endif
#if HAS_XINPUT
    create_provider<XInputControllerProvider>();
#endif
#if HAS_DIRECTINPUT
    create_provider<DirectInputControllerProvider>();
#endif
#if HAS_DSU
    create_provider<DSUControllerProvider>();
#endif
#if defined(HAS_GAMECUBE) && HAS_GAMECUBE && defined(HAS_LIBUSB)
    create_provider<GameCubeControllerProvider>();
#endif
#if HAS_WIIMOTE
    create_provider<WiimoteControllerProvider>();
#endif
#endif // BOOST_OS_IOS

    m_update_thread_shutdown.store(false);
    m_update_thread = std::thread(&InputManager::update_thread, this);
}

InputManager::~InputManager()
{
	// destructors will not invoked forever, so we manually release resources in Shutdown().
}

void InputManager::load() noexcept
{
#if BOOST_OS_IOS
    load_gc_controllers();
#else
    for (size_t i = 0; i < kMaxController; ++i)
    {
        try { load(i); }
        catch (const std::exception& ex)
        {
            cemuLog_log(LogType::Force, "can't load controller profile: {}", ex.what());
        }
    }
#endif
}

#if BOOST_OS_IOS

// Keep this entry point in the core's InputManager object so the app does not
// instantiate another inline singleton when linking the static input archive.
extern "C" void WiiRemoteBridge_notifyChanged()
{
    InputManager::instance().on_device_changed();
}

static constexpr int kGCBitA       = 0;
static constexpr int kGCBitB       = 1;
static constexpr int kGCBitX       = 2;
static constexpr int kGCBitY       = 3;

static constexpr int kGCBitLB      = 4;   // L
static constexpr int kGCBitRB      = 5;   // R
static constexpr int kGCBitLTDig   = 6;   // ZL
static constexpr int kGCBitRTDig   = 7;   // ZR

static constexpr int kGCBitOptions = 8;   // Minus / Select
static constexpr int kGCBitMenu    = 9;   // Plus  / Start

static constexpr int kGCBitLS      = 10;
static constexpr int kGCBitRS      = 11;

static constexpr int kGCBitDUp     = kButtonUp;
static constexpr int kGCBitDDown   = kButtonDown;
static constexpr int kGCBitDLeft   = kButtonLeft;
static constexpr int kGCBitDRight  = kButtonRight;

static void apply_default_gc_mappings(EmulatedControllerPtr& emulated,
                                      const std::shared_ptr<ControllerBase>& controller)
{
    using Type = EmulatedController::Type;
    const Type t = emulated->type();
    
    // Left stick
    auto mapLeftStick = [&](uint64 up, uint64 down, uint64 left, uint64 right) {
        emulated->set_mapping(up,    controller, kAxisYN);
        emulated->set_mapping(down,  controller, kAxisYP);
        emulated->set_mapping(left,  controller, kAxisXN);
        emulated->set_mapping(right, controller, kAxisXP);
    };
    
    // Right stick
    auto mapRightStick = [&](uint64 up, uint64 down, uint64 left, uint64 right) {
        emulated->set_mapping(up,    controller, kRotationYN);
        emulated->set_mapping(down,  controller, kRotationYP);
        emulated->set_mapping(left,  controller, kRotationXN);
        emulated->set_mapping(right, controller, kRotationXP);
    };
    
    if (t == Type::VPAD)
    {
        // GamePad
        emulated->set_mapping(VPADController::kButtonId_A,      controller, kGCBitA);
        emulated->set_mapping(VPADController::kButtonId_B,      controller, kGCBitB);
        emulated->set_mapping(VPADController::kButtonId_X,      controller, kGCBitX);
        emulated->set_mapping(VPADController::kButtonId_Y,      controller, kGCBitY);
        emulated->set_mapping(VPADController::kButtonId_L,      controller, kGCBitLB);
        emulated->set_mapping(VPADController::kButtonId_R,      controller, kGCBitRB);
        emulated->set_mapping(VPADController::kButtonId_ZL,     controller, kGCBitLTDig);
        emulated->set_mapping(VPADController::kButtonId_ZR,     controller, kGCBitRTDig);
        emulated->set_mapping(VPADController::kButtonId_Minus,  controller, kGCBitOptions);
        emulated->set_mapping(VPADController::kButtonId_Plus,   controller, kGCBitMenu);
        emulated->set_mapping(VPADController::kButtonId_StickL, controller, kGCBitLS);
        emulated->set_mapping(VPADController::kButtonId_StickR, controller, kGCBitRS);
        emulated->set_mapping(VPADController::kButtonId_Up,     controller, kGCBitDUp);
        emulated->set_mapping(VPADController::kButtonId_Down,   controller, kGCBitDDown);
        emulated->set_mapping(VPADController::kButtonId_Left,   controller, kGCBitDLeft);
        emulated->set_mapping(VPADController::kButtonId_Right,  controller, kGCBitDRight);
        
        mapLeftStick(VPADController::kButtonId_StickL_Up,   VPADController::kButtonId_StickL_Down, VPADController::kButtonId_StickL_Left, VPADController::kButtonId_StickL_Right);
        mapRightStick(VPADController::kButtonId_StickR_Up,   VPADController::kButtonId_StickR_Down, VPADController::kButtonId_StickR_Left, VPADController::kButtonId_StickR_Right);
    }
    else if (t == Type::Pro)
    {
        // Pro Controller
        emulated->set_mapping(ProController::kButtonId_A,      controller, kGCBitA);
        emulated->set_mapping(ProController::kButtonId_B,      controller, kGCBitB);
        emulated->set_mapping(ProController::kButtonId_X,      controller, kGCBitX);
        emulated->set_mapping(ProController::kButtonId_Y,      controller, kGCBitY);
        emulated->set_mapping(ProController::kButtonId_L,      controller, kGCBitLB);
        emulated->set_mapping(ProController::kButtonId_R,      controller, kGCBitRB);
        emulated->set_mapping(ProController::kButtonId_ZL,     controller, kGCBitLTDig);
        emulated->set_mapping(ProController::kButtonId_ZR,     controller, kGCBitRTDig);
        emulated->set_mapping(ProController::kButtonId_Minus,  controller, kGCBitOptions);
        emulated->set_mapping(ProController::kButtonId_Plus,   controller, kGCBitMenu);
        emulated->set_mapping(ProController::kButtonId_StickL, controller, kGCBitLS);
        emulated->set_mapping(ProController::kButtonId_StickR, controller, kGCBitRS);
        emulated->set_mapping(ProController::kButtonId_Up,     controller, kGCBitDUp);
        emulated->set_mapping(ProController::kButtonId_Down,   controller, kGCBitDDown);
        emulated->set_mapping(ProController::kButtonId_Left,   controller, kGCBitDLeft);
        emulated->set_mapping(ProController::kButtonId_Right,  controller, kGCBitDRight);
        
        mapLeftStick(ProController::kButtonId_StickL_Up,   ProController::kButtonId_StickL_Down,
                     ProController::kButtonId_StickL_Left, ProController::kButtonId_StickL_Right);
        mapRightStick(ProController::kButtonId_StickR_Up,   ProController::kButtonId_StickR_Down,
                      ProController::kButtonId_StickR_Left, ProController::kButtonId_StickR_Right);
    }
    else if (t == Type::Classic)
    {
        // Wii Classic Controller
        emulated->set_mapping(ClassicController::kButtonId_A,      controller, kGCBitA);
        emulated->set_mapping(ClassicController::kButtonId_B,      controller, kGCBitB);
        emulated->set_mapping(ClassicController::kButtonId_X,      controller, kGCBitX);
        emulated->set_mapping(ClassicController::kButtonId_Y,      controller, kGCBitY);
        emulated->set_mapping(ClassicController::kButtonId_L,      controller, kGCBitLB);
        emulated->set_mapping(ClassicController::kButtonId_R,      controller, kGCBitRB);
        emulated->set_mapping(ClassicController::kButtonId_ZL,     controller, kGCBitLTDig);
        emulated->set_mapping(ClassicController::kButtonId_ZR,     controller, kGCBitRTDig);
        emulated->set_mapping(ClassicController::kButtonId_Minus,  controller, kGCBitOptions);
        emulated->set_mapping(ClassicController::kButtonId_Plus,   controller, kGCBitMenu);
        emulated->set_mapping(ClassicController::kButtonId_Up,     controller, kGCBitDUp);
        emulated->set_mapping(ClassicController::kButtonId_Down,   controller, kGCBitDDown);
        emulated->set_mapping(ClassicController::kButtonId_Left,   controller, kGCBitDLeft);
        emulated->set_mapping(ClassicController::kButtonId_Right,  controller, kGCBitDRight);
        
        mapLeftStick(ClassicController::kButtonId_StickL_Up,   ClassicController::kButtonId_StickL_Down, ClassicController::kButtonId_StickL_Left, ClassicController::kButtonId_StickL_Right);
        mapRightStick(ClassicController::kButtonId_StickR_Up,   ClassicController::kButtonId_StickR_Down, ClassicController::kButtonId_StickR_Left, ClassicController::kButtonId_StickR_Right);
    }
    else if (t == Type::Wiimote)
    {
        // WiiMote
        emulated->set_mapping(WiimoteController::kButtonId_A,     controller, kGCBitA);
        emulated->set_mapping(WiimoteController::kButtonId_B,     controller, kGCBitB);
        emulated->set_mapping(WiimoteController::kButtonId_1,   controller, kGCBitX);
        emulated->set_mapping(WiimoteController::kButtonId_2,   controller, kGCBitY);
        emulated->set_mapping(WiimoteController::kButtonId_Minus, controller, kGCBitOptions);
        emulated->set_mapping(WiimoteController::kButtonId_Plus,  controller, kGCBitMenu);
        emulated->set_mapping(WiimoteController::kButtonId_Up,    controller, kGCBitDUp);
        emulated->set_mapping(WiimoteController::kButtonId_Down,  controller, kGCBitDDown);
        emulated->set_mapping(WiimoteController::kButtonId_Left,  controller, kGCBitDLeft);
        emulated->set_mapping(WiimoteController::kButtonId_Right, controller, kGCBitDRight);
    }
    else
    {
        cemuLog_log(LogType::Force, "apply_default_gc_mappings: unhandled emulated controller type {}", (int)t);
    }
}

void InputManager::load_gc_controllers()
{
    std::scoped_lock configuration_lock(m_gc_configuration_mutex);
    auto provider = get_api_provider(InputAPI::GCController);
    if (!provider)
    {
        cemuLog_log(LogType::Force, "GCControllerProvider not available");
        return;
    }
    
    const auto controllers = provider->get_controllers();
    decltype(m_vpad) next_vpad{};
    decltype(m_wpad) next_wpad{};
    std::array<bool, kMaxController> assignedPlayers{};
    size_t vpad_count = 0;
    size_t wpad_count = 0;
    const size_t controller_count = std::min(controllers.size(), kMaxController);
    
    for (size_t i = 0; i < controller_count; ++i)
    {
        auto device = std::dynamic_pointer_cast<GCControllerDevice>(controllers[i]);
        if (!device || !device->is_connected()) continue;
        const auto type = device->type();
        if (type >= EmulatedController::Type::MAX) continue;
        if (type == EmulatedController::Type::VPAD ? vpad_count >= kMaxVPADControllers
            : wpad_count >= kMaxWPADControllers)
            continue;
        try
        {
            auto emulated = get_controller(i);
            if (!emulated || emulated->type() != type ||
                emulated->get_controllers().size() != 1 ||
                emulated->get_controllers().front() != device)
            {
                emulated = ControllerFactory::CreateEmulatedController(i, type);
                emulated->add_controller(device);
                apply_default_gc_mappings(emulated, device);
            }
            assignedPlayers[i] = true;
            if (type == EmulatedController::Type::VPAD)
                next_vpad[vpad_count++] = std::static_pointer_cast<VPADController>(emulated);
            else
                next_wpad[wpad_count++] = std::static_pointer_cast<WPADController>(emulated);
        }
        catch (const std::exception& ex)
        {
            cemuLog_log(LogType::Force, "Failed to wire GCController[{}]: {}", i, ex.what());
        }
    }
    // ESP32 remotes enter the native Wiimote provider and occupy Wii WPAD ports.
    // Keep them out of GCController and its configurable GamePad/Pro mappings.
    if (auto wiiProvider = get_api_provider(InputAPI::Wiimote))
    {
        for (const auto& controller : wiiProvider->get_controllers())
        {
            if (vpad_count + wpad_count >= kMaxController || wpad_count >= kMaxWPADControllers) break;
            const auto device = std::dynamic_pointer_cast<NativeWiimoteController>(controller);
            if (!device || !device->is_connected()) continue;
            const auto available = std::find(assignedPlayers.begin(), assignedPlayers.end(), false);
            if (available == assignedPlayers.end()) break;
            const auto playerIndex = static_cast<size_t>(available - assignedPlayers.begin());
            *available = true;
            auto emulated = std::dynamic_pointer_cast<WiimoteController>(get_wpad_controller(wpad_count));
            if (!emulated || emulated->player_index() != playerIndex ||
                emulated->get_controllers().size() != 1 ||
                emulated->get_controllers().front()->api() != InputAPI::Wiimote ||
                emulated->get_controllers().front()->uuid() != device->uuid())
            {
                emulated = std::make_shared<WiimoteController>(playerIndex);
                auto settings = device->get_settings();
                settings.rumble = 1.0f;
                device->set_settings(settings);
                emulated->add_controller(device);
                emulated->set_default_mapping(device);
                device->set_player_index(wpad_count);
            }
            next_wpad[wpad_count++] = std::move(emulated);
        }
    }
    {
        std::scoped_lock lock(m_mutex);
        m_vpad.swap(next_vpad);
        m_wpad.swap(next_wpad);
    }
}

#endif // BOOST_OS_IOS

bool InputManager::load(size_t player_index, std::string_view filename)
{
#if BOOST_OS_IOS
    (void)player_index; (void)filename;
    return false;
#else
    fs::path file_path;
    if (filename.empty())
        file_path = ActiveSettings::GetConfigPath("controllerProfiles/controller{}", player_index);
    else
        file_path = ActiveSettings::GetConfigPath("controllerProfiles/{}", filename);

    auto old_file = file_path;
    old_file.replace_extension(".txt");
    file_path.replace_extension(".xml");

    if (fs::exists(old_file) && !fs::exists(file_path))
        migrate_config(old_file);

    if (!fs::exists(file_path))
        return false;

    try
    {
        auto xmlData = FileStream::LoadIntoMemory(file_path);
        if (!xmlData || xmlData->empty())
            return false;

        pugi::xml_document doc;
        if (!doc.load_buffer(xmlData->data(), xmlData->size()))
            return false;

        const pugi::xml_node root = doc.document_element();

        const auto type_node = root.child("type");
        if (!type_node)
            return false;

        const auto emulate = EmulatedController::type_from_string(type_node.child_value());
        auto emulated_controller = ControllerFactory::CreateEmulatedController(player_index, emulate);

        if (const auto profile_name_node = root.child("profile"))
            emulated_controller->m_profile_name = profile_name_node.child_value();

        emulated_controller->load(root);

        for (const auto controller_node : root.select_nodes("controller"))
        {
            const auto cnode = controller_node.node();
            const auto api_node = cnode.child("api");
            if (!api_node)
                continue;

            const auto uuid_node = cnode.child("uuid");
            if (!uuid_node)
                continue;

            const auto* uuid         = uuid_node.child_value();
            const auto* display_name = cnode.child_value("display_name");

            try
            {
                const auto api = InputAPI::from_string(api_node.child_value());
                auto controller = ControllerFactory::CreateController(api, uuid, display_name);
                emulated_controller->add_controller(controller);

                auto settings = controller->get_settings();
                if (const auto axis_node = cnode.child("axis"))
                {
                    if (const auto value = axis_node.child("deadzone"))
                        settings.axis.deadzone = ConvertString<float>(value.child_value());
                    if (const auto value = axis_node.child("range"))
                        settings.axis.range = ConvertString<float>(value.child_value());
                }
                if (const auto rotation_node = cnode.child("rotation"))
                {
                    if (const auto value = rotation_node.child("deadzone"))
                        settings.rotation.deadzone = ConvertString<float>(value.child_value());
                    if (const auto value = rotation_node.child("range"))
                        settings.rotation.range = ConvertString<float>(value.child_value());
                }
                if (const auto trigger_node = cnode.child("trigger"))
                {
                    if (const auto value = trigger_node.child("deadzone"))
                        settings.trigger.deadzone = ConvertString<float>(value.child_value());
                    if (const auto value = trigger_node.child("range"))
                        settings.trigger.range = ConvertString<float>(value.child_value());
                }
                if (const auto value = cnode.child("rumble"))
                    settings.rumble = ConvertString<float>(value.child_value());
                if (const auto value = cnode.child("motion"))
                    settings.motion = ConvertString<bool>(value.child_value());

                controller->set_settings(settings);
                controller->load(cnode);

                if (const auto mappings_node = cnode.child("mappings"))
                {
                    for (const auto& entry : mappings_node.select_nodes("entry"))
                    {
                        const auto enode = entry.node();
                        const auto mapping_node = enode.child("mapping");
                        if (!mapping_node) continue;
                        const auto button_node = enode.child("button");
                        if (!button_node) continue;

                        const auto mapping = ConvertString<uint64>(mapping_node.child_value());
                        const auto button  = ConvertString<uint64>(button_node.child_value());
                        emulated_controller->set_mapping(mapping, controller, button);
                    }
                }
            }
            catch (const std::exception& ex)
            {
                cemuLog_log(LogType::Force, "can't load controller: {}", ex.what());
            }
        }

        set_controller(emulated_controller);
        return true;
    }
    catch (const std::exception& ex)
    {
        cemuLog_log(LogType::Force, "can't load config file: {}", ex.what());
        return false;
    }
#endif // !BOOST_OS_IOS
}

bool InputManager::migrate_config(const fs::path& file_path)
{
	try
	{
		auto xmlData = FileStream::LoadIntoMemory(file_path);
		if (!xmlData || xmlData->empty())
			return false;

		std::string iniDataStr((const char*)xmlData->data(), xmlData->size());

		std::stringstream iniData(iniDataStr);
		boost::property_tree::ptree m_data;
		read_ini(iniData, m_data);

		const auto emulate_string = m_data.get<std::string>("General.emulate");
		const auto api_string = m_data.get<std::string>("General.api");
		auto uuid_opt = m_data.get_optional<std::string>("General.controller");
		const auto display_name = m_data.get_optional<std::string>("General.display");

		std::string uuid;
		if (api_string == to_string(InputAPI::Keyboard))
			uuid = to_string(InputAPI::Keyboard);
		else
		{
			if (!uuid_opt)
				return false;

			uuid = uuid_opt.value();
			if (api_string == to_string(InputAPI::SDLController))
			{
				uuid += "_0";
			}
		}

		fs::path out_file = file_path;
		out_file.replace_extension(".xml");

		pugi::xml_document doc;
		auto declaration_node = doc.append_child(pugi::node_declaration);
		declaration_node.append_attribute("version") = "1.0";
		declaration_node.append_attribute("encoding") = "UTF-8";

		auto emulated_controller = doc.append_child("emulated_controller");
		emulated_controller.append_child("type").append_child(pugi::node_pcdata).set_value(emulate_string.c_str());

		bool has_keyboard = api_string == to_string(InputAPI::Keyboard);
		if (!has_keyboard) // test if only keyboard configured
		{
			auto controller = emulated_controller.append_child("controller");
			controller.append_child("api").append_child(pugi::node_pcdata).set_value(api_string.c_str());
			controller.append_child("uuid").append_child(pugi::node_pcdata).set_value(uuid.c_str());
			if (display_name.has_value() && !display_name->empty())
				controller.append_child("display_name").append_child(pugi::node_pcdata).set_value(
					display_name.value().c_str());


			controller.append_child("rumble").append_child(pugi::node_pcdata).set_value(
				m_data.get<std::string>("Controller.rumble").c_str());

			auto axis_node = controller.append_child("axis");
			axis_node.append_child("deadzone").append_child(pugi::node_pcdata).set_value(
				m_data.get<std::string>("Controller.leftDeadzone").c_str());
			axis_node.append_child("range").append_child(pugi::node_pcdata).set_value(
				m_data.get<std::string>("Controller.leftRange").c_str());

			auto rotation_node = controller.append_child("rotation");
			rotation_node.append_child("deadzone").append_child(pugi::node_pcdata).set_value(
				m_data.get<std::string>("Controller.rightDeadzone").c_str());
			rotation_node.append_child("range").append_child(pugi::node_pcdata).set_value(
				m_data.get<std::string>("Controller.rightRange").c_str());

			auto mappings_node = controller.append_child("mappings");
			for (int i = 1; i < 28; ++i) // test all possible mappings (max is 27 for vpad controller)
			{
				auto mapping = m_data.get_optional<std::string>(fmt::format("Controller.{}", i));
				if (!mapping || mapping->empty())
					continue;

				if (!boost::starts_with(mapping.value(), "button_"))
				{
					if (boost::starts_with(mapping.value(), "key_"))
						has_keyboard = true;

					continue;
				}

				const auto button = ConvertString<uint64>(mapping.value().substr(7), 16);

				uint64 flag_bit = 0;
				for (auto b = 0; b < 64; ++b)
				{
					if (HAS_BIT(button, b))
					{
						flag_bit = b;
						break;
					}
				}

				// fix old flag layout to new one for all kind of axis stuff
				if (flag_bit >= 24 && flag_bit <= 31)
					flag_bit += 8;
				else if (flag_bit == 32) flag_bit = kTriggerXP;
				else if (flag_bit == 33) flag_bit = kRotationXP;
				else if (flag_bit == 34) flag_bit = kRotationYP;
				else if (flag_bit == 35) flag_bit = kTriggerYP;
				else if (flag_bit == 36) flag_bit = kAxisXN;
				else if (flag_bit == 37) flag_bit = kAxisYN;
				else if (flag_bit == 38) flag_bit = kTriggerXN;
				else if (flag_bit == 39) flag_bit = kRotationXN;
				else if (flag_bit == 40) flag_bit = kRotationYN;
				else if (flag_bit == 41) flag_bit = kTriggerYN;

				// fix old api mappings
				if (api_string == to_string(InputAPI::XInput))
				{
					const std::unordered_map<uint64, uint64> xinput =
					{
						{kButton0, 12}, // XINPUT_GAMEPAD_A
						{kButton1, 13}, // XINPUT_GAMEPAD_B
						{kButton2, 14}, // XINPUT_GAMEPAD_X
						{kButton3, 15}, // XINPUT_GAMEPAD_Y

						{kButton4, 8}, // XINPUT_GAMEPAD_LEFT_SHOULDER
						{kButton5, 9}, // XINPUT_GAMEPAD_LEFT_SHOULDER

						{kButton6, 4}, // XINPUT_GAMEPAD_START
						{kButton7, 5}, // XINPUT_GAMEPAD_BACK

						{kButton8, 6}, // XINPUT_GAMEPAD_LEFT_THUMB
						{kButton9, 7}, // XINPUT_GAMEPAD_RIGHT_THUMB

						{kButton10, 0}, // XINPUT_GAMEPAD_DPAD_UP
						{kButton11, 1}, // XINPUT_GAMEPAD_DPAD_DOWN
						{kButton12, 2}, // XINPUT_GAMEPAD_DPAD_LEFT
						{kButton13, 3}, // XINPUT_GAMEPAD_DPAD_RIGHT
					};

					const auto it = xinput.find(flag_bit);
					if (it != xinput.cend())
						flag_bit = it->second;
				}
				else if (api_string == "DSU")
				{
					const std::unordered_map<uint64, uint64> dsu =
					{
						{7, kButton0}, // ButtonSelect
						{8, kButton1}, // ButtonLStick
						{9, kButton2}, // ButtonRStick
						{6, kButton3}, // ButtonStart

						{4, kButton10}, // ButtonL
						{5, kButton11}, // ButtonR

						{0, kButton14}, // ButtonA
						{1, kButton13}, // ButtonB
						{2, kButton15}, // ButtonX
						{3, kButton12}, // ButtonY
					};

					const auto it = dsu.find(flag_bit);
					if (it != dsu.cend())
						flag_bit = it->second;
				}


				auto entry_node = mappings_node.append_child("entry");
				entry_node.append_child("mapping").append_child(pugi::node_pcdata).set_value(
					fmt::format("{}", i).c_str());
				entry_node.append_child("button").append_child(pugi::node_pcdata).set_value(
					fmt::format("{}", flag_bit).c_str());
			}
		}

		if (has_keyboard)
		{
			auto controller = emulated_controller.append_child("controller");
			controller.append_child("api").append_child(pugi::node_pcdata).set_value("Keyboard");
			controller.append_child("uuid").append_child(pugi::node_pcdata).set_value("Keyboard");

			auto mappings_node = controller.append_child("mappings");
			for (int i = 1; i < 28; ++i) // test all possible mappings (max is 27 for vpad controller)
			{
				auto mapping = m_data.get_optional<std::string>(fmt::format("Controller.{}", i));
				if (!mapping || mapping->empty())
					continue;

				if (!boost::starts_with(mapping.value(), "key_"))
					continue;

				const auto button = ConvertString<uint64>(mapping.value().substr(4));

				auto entry_node = mappings_node.append_child("entry");
				entry_node.append_child("mapping").append_child(pugi::node_pcdata).set_value(
					fmt::format("{}", i).c_str());
				entry_node.append_child("button").append_child(pugi::node_pcdata).set_value(
					fmt::format("{}", button).c_str());
			}
		}

		std::ofstream write_file(fs::resolvePathCI(out_file), std::ios::out | std::ios::trunc);
		if (write_file.is_open())
		{
			doc.save(write_file);
			return true;
		}
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "can't migrate config file {}: {}", file_path.string(), ex.what());
	}

	return false;
}

void InputManager::save() noexcept
{
#if BOOST_OS_IOS
    return; // iOS uses hardwired GCController mapping, nothing to persist
#else
    for (size_t i = 0; i < kMaxController; ++i)
    {
        try { save(i); }
        catch (const std::exception& ex)
        {
            cemuLog_log(LogType::Force, "can't save controller profile: {}", ex.what());
        }
    }
#endif
}

bool InputManager::save(size_t player_index, std::string_view filename)
{
#if BOOST_OS_IOS
    (void)player_index; (void)filename;
    return true;
#else
    if (m_is_gameprofile_set[player_index])
        return true;

    auto emulated_controller = get_controller(player_index);
    if (!emulated_controller)
        return false;

    fs::path file_path = ActiveSettings::GetConfigPath("controllerProfiles");
    fs::create_directories(file_path);

    const auto is_default_file = filename.empty();
    if (is_default_file)
        file_path /= fmt::format("controller{}", player_index);
    else
        file_path /= _utf8ToPath(filename);

    file_path.replace_extension(".xml");

    pugi::xml_document doc;
    auto declaration_node = doc.append_child(pugi::node_declaration);
    declaration_node.append_attribute("version") = "1.0";
    declaration_node.append_attribute("encoding") = "UTF-8";

    auto emulated_controller_node = doc.append_child("emulated_controller");
    emulated_controller_node.append_child("type").append_child(pugi::node_pcdata).set_value(
        std::string{emulated_controller->type_string()}.c_str());

    if (!is_default_file)
        emulated_controller->m_profile_name = std::string{filename};

    if (emulated_controller->has_profile_name())
        emulated_controller_node.append_child("profile").append_child(pugi::node_pcdata).set_value(
            emulated_controller->get_profile_name().c_str());

    emulated_controller->save(emulated_controller_node);

    for (const auto& controller : emulated_controller->get_controllers())
    {
        auto controller_node = emulated_controller_node.append_child("controller");
        controller_node.append_child("api").append_child(pugi::node_pcdata).set_value(
            std::string{controller->api_name()}.c_str());
        controller_node.append_child("uuid").append_child(pugi::node_pcdata).set_value(controller->uuid().c_str());
        controller_node.append_child("display_name").append_child(pugi::node_pcdata).set_value(
            controller->display_name().c_str());

        const auto& settings = controller->get_settings();
        if (controller->has_motion())
            controller_node.append_child("motion").append_child(pugi::node_pcdata).set_value(
                fmt::format("{}", settings.motion).c_str());
        if (controller->has_rumble())
            controller_node.append_child("rumble").append_child(pugi::node_pcdata).set_value(
                fmt::format("{}", settings.rumble).c_str());

        auto axis_node = controller_node.append_child("axis");
        axis_node.append_child("deadzone").append_child(pugi::node_pcdata).set_value(fmt::format("{}", settings.axis.deadzone).c_str());
        axis_node.append_child("range").append_child(pugi::node_pcdata).set_value(fmt::format("{}", settings.axis.range).c_str());

        auto rotation_node = controller_node.append_child("rotation");
        rotation_node.append_child("deadzone").append_child(pugi::node_pcdata).set_value(fmt::format("{}", settings.rotation.deadzone).c_str());
        rotation_node.append_child("range").append_child(pugi::node_pcdata).set_value(fmt::format("{}", settings.rotation.range).c_str());

        auto trigger_node = controller_node.append_child("trigger");
        trigger_node.append_child("deadzone").append_child(pugi::node_pcdata).set_value(fmt::format("{}", settings.trigger.deadzone).c_str());
        trigger_node.append_child("range").append_child(pugi::node_pcdata).set_value(fmt::format("{}", settings.trigger.range).c_str());

        controller->save(controller_node);

        auto mappings_node = controller_node.append_child("mappings");
        for (const auto& mapping : emulated_controller->m_mappings)
        {
            if (!mapping.second.controller.expired() && *controller == *mapping.second.controller.lock())
            {
                auto entry_node = mappings_node.append_child("entry");
                entry_node.append_child("mapping").append_child(pugi::node_pcdata).set_value(fmt::format("{}", mapping.first).c_str());
                entry_node.append_child("button").append_child(pugi::node_pcdata).set_value(fmt::format("{}", mapping.second.button).c_str());
            }
        }
    }

    FileStream* fs = FileStream::createFile2(file_path);
    if (!fs) return false;
    std::stringstream xmlData;
    doc.save(xmlData);
    std::string xmlStr = xmlData.str();
    fs->writeData(xmlStr.data(), xmlStr.size());
    delete fs;
    return true;
#endif // !BOOST_OS_IOS
}

bool InputManager::is_gameprofile_set(size_t player_index) const
{
    return m_is_gameprofile_set[player_index];
}

EmulatedControllerPtr InputManager::set_controller(EmulatedControllerPtr controller)
{
    auto prev_controller = delete_controller(controller->player_index());

    if (prev_controller && controller->get_controllers().empty())
    {
        for (const auto& c : prev_controller->get_controllers())
            controller->add_controller(c);
    }

    for (auto& c : controller->get_controllers())
        c->connect();

    std::scoped_lock lock(m_mutex);
    switch (controller->type())
    {
    case EmulatedController::Type::VPAD:
        for (auto& pad : m_vpad)
        {
            if (!pad) { pad.swap(controller); return prev_controller; }
        }
        break;
    default:
        for (auto& pad : m_wpad)
        {
            if (!pad) { pad.swap(controller); return prev_controller; }
        }
        break;
    }

    cemu_assert_debug(false);
    return prev_controller;
}

EmulatedControllerPtr InputManager::set_controller(size_t player_index, EmulatedController::Type type)
{
    try
    {
        auto emulated_controller = ControllerFactory::CreateEmulatedController(player_index, type);
        set_controller(emulated_controller);
        return emulated_controller;
    }
    catch (const std::exception& ex)
    {
        cemuLog_log(LogType::Force, "Unable to set controller type {} on player index {}: {}", type, player_index, ex.what());
    }
    return {};
}

EmulatedControllerPtr InputManager::set_controller(size_t player_index, EmulatedController::Type type,
                                                   const std::shared_ptr<ControllerBase>& controller)
{
    auto result = set_controller(player_index, type);
    if (result) result->add_controller(controller);
    return result;
}

EmulatedControllerPtr InputManager::get_controller(size_t player_index) const
{
    std::shared_lock lock(m_mutex);
    for (const auto& pad : m_vpad)
        if (pad && pad->player_index() == player_index) return pad;
    for (const auto& pad : m_wpad)
        if (pad && pad->player_index() == player_index) return pad;
    return {};
}

EmulatedControllerPtr InputManager::delete_controller(size_t player_index, bool delete_profile)
{
    std::scoped_lock lock(m_mutex);
    for (auto& controller : m_vpad)
    {
        auto result = controller;
        if (result && result->player_index() == player_index)
        {
            controller = {};
            if (delete_profile)
            {
                std::error_code ec{};
                fs::remove(ActiveSettings::GetConfigPath("controllerProfiles/controller{}.xml", player_index), ec);
                fs::remove(ActiveSettings::GetConfigPath("controllerProfiles/controller{}.txt", player_index), ec);
            }
            return result;
        }
    }
    for (auto& controller : m_wpad)
    {
        auto result = controller;
        if (result && result->player_index() == player_index)
        {
            controller = {};
            if (delete_profile)
            {
                std::error_code ec{};
                fs::remove(ActiveSettings::GetConfigPath("controllerProfiles/controller{}.xml", player_index), ec);
                fs::remove(ActiveSettings::GetConfigPath("controllerProfiles/controller{}.txt", player_index), ec);
            }
            return result;
        }
    }
    return {};
}

std::shared_ptr<VPADController> InputManager::get_vpad_controller(size_t index) const
{
	if (index >= m_vpad.size())
		return {};

	std::shared_lock lock(m_mutex);
	return std::dynamic_pointer_cast<VPADController>(m_vpad[index]);
}

std::shared_ptr<WPADController> InputManager::get_wpad_controller(size_t index) const
{
	if (index >= m_wpad.size())
		return {};

	std::shared_lock lock(m_mutex);
	return std::dynamic_pointer_cast<WPADController>(m_wpad[index]);
}

std::pair<size_t, size_t> InputManager::get_controller_count() const
{
    std::shared_lock lock(m_mutex);
    const size_t vpad = std::count_if(m_vpad.cbegin(), m_vpad.cend(), [](const auto& v) { return v != nullptr; });
    const size_t wpad = std::count_if(m_wpad.cbegin(), m_wpad.cend(), [](const auto& v) { return v != nullptr; });
    return std::make_pair(vpad, wpad);
}

void InputManager::on_device_changed()
{
#if !BOOST_OS_IOS
    {
        std::shared_lock lock(m_mutex);
        for (auto& pad : m_vpad) { if (pad) pad->connect(); }
        for (auto& pad : m_wpad) { if (pad) pad->connect(); }
    }
#else
    load_gc_controllers();
#endif

    EventService::instance().signal<Events::ControllerChanged>();
}

ControllerProviderPtr InputManager::get_api_provider(InputAPI::Type api) const
{
    if (!m_api_available[api].empty())
        return *(m_api_available[api].begin());
    cemu_assert_debug(false);
    return {};
}

ControllerProviderPtr InputManager::get_api_provider(InputAPI::Type api, const ControllerProviderSettings& settings)
{
    for (const auto& p : m_api_available[api])
        if (*p == settings) return p;

    const auto result = ControllerFactory::CreateControllerProvider(api, settings);
    m_api_available[api].emplace_back(result);
    return result;
}

void InputManager::apply_game_profile()
{
#if BOOST_OS_IOS
    return;
#else
    const auto& profiles = g_current_game_profile->GetControllerProfile();
    for (int i = 0; i < kMaxController; ++i)
    {
        if (profiles[i] && !profiles[i]->empty())
        {
            if (load(i, profiles[i].value()))
            {
                m_is_gameprofile_set[i] = true;
                if (const auto controller = get_controller(i))
                    if (!controller->has_profile_name())
                        controller->m_profile_name = profiles[i].value();
            }
        }
    }
#endif
}

std::vector<std::string> InputManager::get_profiles()
{
    const auto path = ActiveSettings::GetConfigPath("controllerProfiles");
    if (!exists(path)) return {};

    std::set<std::string> tmp;
    for (const auto& entry : fs::directory_iterator(path))
    {
        const auto& p = entry.path();
        if (p.has_extension() && (p.extension() == ".xml" || p.extension() == ".txt"))
        {
            auto stem = _pathToUtf8(p.filename().stem());
            if (is_valid_profilename(stem))
                tmp.emplace(stem);
        }
    }

    std::vector<std::string> result;
    result.reserve(tmp.size());
    result.insert(result.end(), tmp.begin(), tmp.end());
    return result;
}

bool InputManager::is_valid_profilename(const std::string& name)
{
    if (!IsValidFilename(name)) return false;
    for (size_t i = 0; i < kMaxController; i++)
        if (name == fmt::format("controller{}", i)) return false;
    return true;
}

glm::ivec2 InputManager::get_mouse_position(bool pad_window) const
{
    if (pad_window) { std::shared_lock lock(m_pad_mouse.m_mutex);  return m_pad_mouse.position; }
    else            { std::shared_lock lock(m_main_mouse.m_mutex); return m_main_mouse.position; }
}

std::optional<glm::ivec2> InputManager::get_left_down_mouse_info(bool* is_pad)
{
    if (is_pad) *is_pad = false;
    { std::shared_lock lock(m_main_mouse.m_mutex); if (std::exchange(m_main_mouse.left_down_toggle, false)) return m_main_mouse.position; if (m_main_mouse.left_down) return m_main_mouse.position; }
    { std::shared_lock lock(m_main_touch.m_mutex); if (std::exchange(m_main_touch.left_down_toggle, false)) return m_main_touch.position; if (m_main_touch.left_down) return m_main_touch.position; }
    if (is_pad) *is_pad = true;
    { std::shared_lock lock(m_pad_mouse.m_mutex); if (std::exchange(m_pad_mouse.left_down_toggle, false)) return m_pad_mouse.position; if (m_pad_mouse.left_down) return m_pad_mouse.position; }
    { std::shared_lock lock(m_pad_touch.m_mutex); if (std::exchange(m_pad_touch.left_down_toggle, false)) return m_pad_touch.position; if (m_pad_touch.left_down) return m_pad_touch.position; }
    return {};
}

std::optional<glm::ivec2> InputManager::get_right_down_mouse_info(bool* is_pad)
{
    if (is_pad) *is_pad = false;
    { std::shared_lock lock(m_main_mouse.m_mutex); if (std::exchange(m_main_mouse.right_down_toggle, false)) return m_main_mouse.position; if (m_main_mouse.right_down) return m_main_mouse.position; }
    { std::shared_lock lock(m_main_touch.m_mutex); if (std::exchange(m_main_touch.right_down_toggle, false)) return m_main_touch.position; if (m_main_touch.right_down) return m_main_touch.position; }
    if (is_pad) *is_pad = true;
    { std::shared_lock lock(m_pad_mouse.m_mutex); if (std::exchange(m_pad_mouse.right_down_toggle, false)) return m_pad_mouse.position; if (m_pad_mouse.right_down) return m_pad_mouse.position; }
    { std::shared_lock lock(m_pad_touch.m_mutex); if (std::exchange(m_pad_touch.right_down_toggle, false)) return m_pad_touch.position; if (m_pad_touch.right_down) return m_pad_touch.position; }
    return {};
}

void InputManager::update_thread()
{
    SetThreadName("Input_update");
    while (!m_update_thread_shutdown.load(std::memory_order::relaxed))
    {
        std::shared_lock lock(m_mutex);
        for (auto& pad : m_vpad) { if (pad) pad->update(); }
        for (auto& pad : m_wpad) { if (pad) pad->update(); }
        lock.unlock();

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::this_thread::yield();
    }
}

void InputManager::Shutdown()
{
    m_update_thread_shutdown = true;

    if (m_update_thread.joinable())
    {
        m_update_thread.join();
    }

    for (auto& pad : m_vpad) pad.reset();
	for (auto& pad : m_wpad) pad.reset();

	for (auto& providers : m_api_available)
	{
		providers.clear();
	}
}
