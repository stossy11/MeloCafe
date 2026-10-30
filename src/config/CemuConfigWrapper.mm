#import <Foundation/Foundation.h>
#import "CemuConfigWrapper.h"
#include "CemuConfig.h"
#include "ActiveSettings.h"
#include "NetworkSettings.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/GameProfile/GameProfile.h"
#include "Cafe/Account/Account.h"
#include "Cafe/IOSU/legacy/iosu_crypto.h"
#include "Cemu/Logging/CemuLogging.h"
#include "Cemu/ncrypto/ncrypto.h"

#include <algorithm>

static inline NSString* wstr(const std::wstring& s) {
    return [[NSString alloc] initWithBytes:s.data()
                                    length:s.size() * sizeof(wchar_t)
                                  encoding:NSUTF32LittleEndianStringEncoding] ?: @"";
}

static inline std::wstring nsToWstr(NSString* s) {
    if (!s) return L"";
    NSData* data = [s dataUsingEncoding:NSUTF32LittleEndianStringEncoding];
    return std::wstring((wchar_t*)data.bytes, data.length / sizeof(wchar_t));
}

#define autoSave() GetConfigHandle().Save()

static const Account* findAccount(uint32_t pid) {
    for (const auto& account : Account::GetAccounts()) {
        if (account.GetPersistentId() == pid) {
            return &account;
        }
    }
    return nullptr;
}

static const char* onlineFileStateName(OnlineValidator::FileState state) {
    switch (state) {
        case OnlineValidator::FileState::Missing: return "missing";
        case OnlineValidator::FileState::Corrupted: return "corrupted";
        case OnlineValidator::FileState::Ok: return "ok";
    }
    return "unknown";
}

static const char* onlineAccountErrorName(OnlineAccountError error) {
    switch (error) {
        case OnlineAccountError::kNone: return "none";
        case OnlineAccountError::kNoAccountId: return "noAccountId";
        case OnlineAccountError::kNoPasswordCached: return "noPasswordCached";
        case OnlineAccountError::kPasswordCacheEmpty: return "passwordCacheEmpty";
        case OnlineAccountError::kNoPrincipalId: return "noPrincipalId";
    }
    return "unknown";
}

static bool refreshAndLogOnlineDiagnostics(uint32_t pid, const Account* account, const char* context) {
    ActiveSettings::Init();

    if (!account) {
        return false;
    }

    const OnlineValidator validator = account->ValidateOnlineFiles();

    return account->IsValidOnlineAccount() && ActiveSettings::HasRequiredOnlineFiles();
}

static ObjCCPUMode toObjCCPUMode(CPUMode mode) {
    switch (mode) {
        case CPUMode::SinglecoreInterpreter: return ObjCCPUModeSinglecoreInterpreter;
        case CPUMode::SinglecoreRecompiler: return ObjCCPUModeSinglecoreRecompiler;
        case CPUMode::DualcoreRecompiler: return ObjCCPUModeDualcoreRecompiler;
        case CPUMode::MulticoreRecompiler: return ObjCCPUModeMulticoreRecompiler;
        case CPUMode::MulticoreInterpreter: return ObjCCPUModeMulticoreInterpreter;
        case CPUMode::Auto: return ObjCCPUModeAuto;
    }
    return ObjCCPUModeAuto;
}

static CPUMode toCPUMode(ObjCCPUMode mode) {
    switch (mode) {
        case ObjCCPUModeSinglecoreInterpreter: return CPUMode::SinglecoreInterpreter;
        case ObjCCPUModeSinglecoreRecompiler: return CPUMode::SinglecoreRecompiler;
        case ObjCCPUModeDualcoreRecompiler: return CPUMode::DualcoreRecompiler;
        case ObjCCPUModeMulticoreRecompiler: return CPUMode::MulticoreRecompiler;
        case ObjCCPUModeMulticoreInterpreter: return CPUMode::MulticoreInterpreter;
        case ObjCCPUModeAuto: return CPUMode::Auto;
    }
    return CPUMode::Auto;
}

@implementation CemuConfigWrapper

+ (instancetype)shared {
    static CemuConfigWrapper* instance = nil;
    static dispatch_once_t token;
    dispatch_once(&token, ^{ instance = [[self alloc] init]; });
    return instance;
}


- (void)loadConfig                         { GetConfigHandle().Load(); }
- (void)saveConfig                         { GetConfigHandle().Save(); }

- (uint64_t)logFlag                        { return GetConfig().log_flag.GetValue(); }
- (void)setLogFlag:(uint64_t)v             { GetConfig().log_flag = v; cemuLog_setActiveLoggingFlags(v); autoSave(); }

- (BOOL)advancedPPCLogging                 { return GetConfig().advanced_ppc_logging.GetValue(); }
- (void)setAdvancedPPCLogging:(BOOL)v      { GetConfig().advanced_ppc_logging = v; autoSave(); }

- (BOOL)permanentStorage                   { return GetConfig().permanent_storage.GetValue(); }
- (void)setPermanentStorage:(BOOL)v        { GetConfig().permanent_storage = v; autoSave(); }

- (NSString*)mlcPath {
    return [NSString stringWithUTF8String:GetConfig().mlc_path.GetValue().c_str()];
}
- (void)setMlcPath:(NSString*)v {
    GetConfig().mlc_path = v.UTF8String ?: "";
    autoSave();
}

- (NSString*)proxyServer {
    return [NSString stringWithUTF8String:GetConfig().proxy_server.GetValue().c_str()];
}
- (void)setProxyServer:(NSString*)v {
    GetConfig().proxy_server = v.UTF8String ?: "";
    autoSave();
}

- (BOOL)disableScreensaver                 { return GetConfig().disable_screensaver.GetValue(); }
- (void)setDisableScreensaver:(BOOL)v      { GetConfig().disable_screensaver = v; autoSave(); }

- (BOOL)playBootSound                      { return GetConfig().play_boot_sound.GetValue(); }
- (void)setPlayBootSound:(BOOL)v           { GetConfig().play_boot_sound = v; autoSave(); }

- (ObjCCPUMode)cpuMode                     { return toObjCCPUMode(GetConfig().cpu_mode.GetValue()); }
- (void)setCpuMode:(ObjCCPUMode)v          { GetConfig().cpu_mode = toCPUMode(v); autoSave(); }

- (ObjCCafeConsoleLanguage)consoleLanguage { return (ObjCCafeConsoleLanguage)GetConfig().console_language.GetValue(); }
- (void)setConsoleLanguage:(ObjCCafeConsoleLanguage)v { GetConfig().console_language = (CafeConsoleLanguage)v; autoSave(); }

- (ObjCGraphicAPI)graphicAPI               { return (ObjCGraphicAPI)GetConfig().graphic_api.GetValue(); }
- (void)setGraphicAPI:(ObjCGraphicAPI)v    { GetConfig().graphic_api = (GraphicAPI)v; autoSave(); }

- (int)vsync                               { return GetConfig().vsync.GetValue(); }
- (void)setVsync:(int)v                    { GetConfig().vsync = v; autoSave(); }

- (BOOL)gx2DrawDoneSync                    { return GetConfig().gx2drawdone_sync.GetValue(); }
- (void)setGx2DrawDoneSync:(BOOL)v         { GetConfig().gx2drawdone_sync = v; autoSave(); }

- (BOOL)renderUpsideDown                   { return GetConfig().render_upside_down.GetValue(); }
- (void)setRenderUpsideDown:(BOOL)v        { GetConfig().render_upside_down = v; autoSave(); }

- (BOOL)asyncCompile                       { return GetConfig().async_compile.GetValue(); }
- (void)setAsyncCompile:(BOOL)v            { GetConfig().async_compile = v; autoSave(); }

- (ObjCPrecompiledShaderOption)precompiledShaders { return (ObjCPrecompiledShaderOption)GetConfig().precompiled_shaders.GetValue(); }
- (void)setPrecompiledShaders:(ObjCPrecompiledShaderOption)v { GetConfig().precompiled_shaders = (PrecompiledShaderOption)v; autoSave(); }

- (BOOL)vkAccurateBarriers                 { return GetConfig().vk_accurate_barriers.GetValue(); }
- (void)setVkAccurateBarriers:(BOOL)v      { GetConfig().vk_accurate_barriers = v; autoSave(); }

- (ObjCUpscalingFilter)upscaleFilter       { return (ObjCUpscalingFilter)GetConfig().upscale_filter.GetValue(); }
- (void)setUpscaleFilter:(ObjCUpscalingFilter)v { GetConfig().upscale_filter = (sint32)v; autoSave(); }

- (ObjCUpscalingFilter)downscaleFilter     { return (ObjCUpscalingFilter)GetConfig().downscale_filter.GetValue(); }
- (void)setDownscaleFilter:(ObjCUpscalingFilter)v { GetConfig().downscale_filter = (sint32)v; autoSave(); }

- (ObjCFullscreenScaling)fullscreenScaling { return (ObjCFullscreenScaling)GetConfig().fullscreen_scaling.GetValue(); }
- (void)setFullscreenScaling:(ObjCFullscreenScaling)v { GetConfig().fullscreen_scaling = (sint32)v; autoSave(); }

- (float)textureResolutionScale            { return GetConfig().texture_resolution_scale.GetValue(); }
- (void)setTextureResolutionScale:(float)v { GetConfig().texture_resolution_scale = std::clamp(v, 0.5f, 2.0f); autoSave(); }

- (BOOL)disableStreamout                    { return GetConfig().disable_streamout.GetValue(); }
- (void)setDisableStreamout:(BOOL)v         { GetConfig().disable_streamout = v; autoSave(); }

- (BOOL)overrideAppGammaPreference         { return GetConfig().overrideAppGammaPreference.GetValue(); }
- (void)setOverrideAppGammaPreference:(BOOL)v { GetConfig().overrideAppGammaPreference = v; autoSave(); }

- (float)overrideGammaValue                { return GetConfig().overrideGammaValue.GetValue(); }
- (void)setOverrideGammaValue:(float)v     { GetConfig().overrideGammaValue = v; autoSave(); }

- (float)userDisplayGamma                  { return GetConfig().userDisplayGamma.GetValue(); }
- (void)setUserDisplayGamma:(float)v       { GetConfig().userDisplayGamma = v; autoSave(); }

#ifdef ENABLE_METAL
- (BOOL)forceMeshShaders                   { return GetConfig().force_mesh_shaders.GetValue(); }
- (void)setForceMeshShaders:(BOOL)v        { GetConfig().force_mesh_shaders = v; autoSave(); }

- (NSString*)gpuCaptureDir {
    return [NSString stringWithUTF8String:GetConfig().gpu_capture_dir.GetValue().c_str()];
}
- (void)setGpuCaptureDir:(NSString*)v {
    GetConfig().gpu_capture_dir = v.UTF8String ?: "";
    autoSave();
}

- (BOOL)framebufferFetch                   { return GetConfig().framebuffer_fetch.GetValue(); }
- (void)setFramebufferFetch:(BOOL)v        { GetConfig().framebuffer_fetch = v; autoSave(); }

#endif

- (int)audioAPI                            { return GetConfig().audio_api; }
- (void)setAudioAPI:(int)v                 { GetConfig().audio_api = v; autoSave(); }

- (int)audioDelay                          { return GetConfig().audio_delay; }
- (void)setAudioDelay:(int)v               { GetConfig().audio_delay = v; autoSave(); }

- (BOOL)microphoneEnabled                  { return GetConfig().microphone_enabled; }
- (void)setMicrophoneEnabled:(BOOL)v       { GetConfig().microphone_enabled = v; autoSave(); }

- (BOOL)tvAudioEnabled                     { return GetConfig().tv_audio_enabled; }
- (void)setTvAudioEnabled:(BOOL)v          { GetConfig().tv_audio_enabled = v; autoSave(); }

- (BOOL)padAudioEnabled                    { return GetConfig().pad_audio_enabled; }
- (void)setPadAudioEnabled:(BOOL)v         { GetConfig().pad_audio_enabled = v; autoSave(); }

- (ObjCAudioChannels)tvChannels            { return (ObjCAudioChannels)GetConfig().tv_channels; }
- (void)setTvChannels:(ObjCAudioChannels)v { GetConfig().tv_channels = (AudioChannels)v; autoSave(); }

- (ObjCAudioChannels)padChannels           { return (ObjCAudioChannels)GetConfig().pad_channels; }
- (void)setPadChannels:(ObjCAudioChannels)v { GetConfig().pad_channels = (AudioChannels)v; autoSave(); }

- (ObjCAudioChannels)inputChannels         { return (ObjCAudioChannels)GetConfig().input_channels; }
- (void)setInputChannels:(ObjCAudioChannels)v { GetConfig().input_channels = (AudioChannels)v; autoSave(); }

- (int)tvVolume                            { return GetConfig().tv_volume; }
- (void)setTvVolume:(int)v                 { GetConfig().tv_volume = v; autoSave(); }

- (int)padVolume                           { return GetConfig().pad_volume; }
- (void)setPadVolume:(int)v                { GetConfig().pad_volume = v; autoSave(); }

- (int)inputVolume                         { return GetConfig().input_volume; }
- (void)setInputVolume:(int)v              { GetConfig().input_volume = v; autoSave(); }

- (int)portalVolume                        { return GetConfig().portal_volume; }
- (void)setPortalVolume:(int)v             { GetConfig().portal_volume = v; autoSave(); }

- (NSString*)tvDevice                      { return wstr(GetConfig().tv_device); }
- (void)setTvDevice:(NSString*)v           { GetConfig().tv_device = nsToWstr(v); autoSave(); }

- (NSString*)padDevice                     { return wstr(GetConfig().pad_device); }
- (void)setPadDevice:(NSString*)v          { GetConfig().pad_device = nsToWstr(v); autoSave(); }

- (NSString*)inputDevice                   { return wstr(GetConfig().input_device); }
- (void)setInputDevice:(NSString*)v        { GetConfig().input_device = nsToWstr(v); autoSave(); }

- (NSString*)portalDevice                  { return wstr(GetConfig().portal_device); }
- (void)setPortalDevice:(NSString*)v       { GetConfig().portal_device = nsToWstr(v); autoSave(); }

- (uint32_t)persistentId                   { return GetConfig().account.m_persistent_id.GetValue(); }
- (void)setPersistentId:(uint32_t)v        { GetConfig().account.m_persistent_id = v; autoSave(); }

- (uint32_t)activeAccountPersistentId      { return self.persistentId; }
- (void)setActiveAccountPersistentId:(uint32_t)v { self.persistentId = v; }

- (uint32_t)minimumAccountPersistentId     { return Account::kMinPersistendId; }
- (uint32_t)nextAccountPersistentId        { Account::RefreshAccounts(); return Account::GetNextPersistentId(); }
- (BOOL)hasFreeAccountSlots                { Account::RefreshAccounts(); return Account::HasFreeAccountSlots(); }
- (BOOL)isTitleRunning                     { return CafeSystem::IsTitleRunning(); }
- (BOOL)canUseCustomNetworkService         { return NetworkConfig::XMLExists(); }

- (BOOL)legacyOnlineEnabled                { return GetConfig().account.legacy_online_enabled.GetValue(); }
- (void)setLegacyOnlineEnabled:(BOOL)v     { GetConfig().account.legacy_online_enabled = v; autoSave(); }

- (ObjCNetworkService)networkServiceForPersistentId:(uint32_t)persistentId {
    return (ObjCNetworkService)GetConfig().GetAccountNetworkService(persistentId);
}
- (void)setNetworkService:(ObjCNetworkService)service forPersistentId:(uint32_t)persistentId {
    GetConfig().SetAccountSelectedService(persistentId, (NetworkService)service);
    autoSave();
}

- (NSString*)dsuHost {
    return [NSString stringWithUTF8String:GetConfig().dsu_client.host.GetValue().c_str()];
}
- (void)setDsuHost:(NSString*)v {
    GetConfig().dsu_client.host = v.UTF8String ?: "127.0.0.1";
    autoSave();
}

- (uint16_t)dsuPort                        { return GetConfig().dsu_client.port.GetValue(); }
- (void)setDsuPort:(uint16_t)v             { GetConfig().dsu_client.port = v; autoSave(); }

- (BOOL)disableMotion                      { return GetConfig().disable_motion.GetValue(); }
- (void)setDisableMotion:(BOOL)v           { GetConfig().disable_motion = v; autoSave(); }

- (ObjCCrashDump)crashDump                 { return (ObjCCrashDump)GetConfig().crash_dump.GetValue(); }
- (void)setCrashDump:(ObjCCrashDump)v      { GetConfig().crash_dump = (CrashDump)v; autoSave(); }

- (uint16_t)gdbPort                        { return GetConfig().gdb_port.GetValue(); }
- (void)setGdbPort:(uint16_t)v             { GetConfig().gdb_port = v; autoSave(); }

- (BOOL)emulateSkylanderPortal             { return GetConfig().emulated_usb_devices.emulate_skylander_portal.GetValue(); }
- (void)setEmulateSkylanderPortal:(BOOL)v  { GetConfig().emulated_usb_devices.emulate_skylander_portal = v; autoSave(); }

- (BOOL)emulateInfinityBase                { return GetConfig().emulated_usb_devices.emulate_infinity_base.GetValue(); }
- (void)setEmulateInfinityBase:(BOOL)v     { GetConfig().emulated_usb_devices.emulate_infinity_base = v; autoSave(); }

- (BOOL)emulateDimensionsToypad            { return GetConfig().emulated_usb_devices.emulate_dimensions_toypad.GetValue(); }
- (void)setEmulateDimensionsToypad:(BOOL)v { GetConfig().emulated_usb_devices.emulate_dimensions_toypad = v; autoSave(); }

- (ObjCScreenPosition)overlayPosition      { return (ObjCScreenPosition)GetConfig().overlay.position; }
- (void)setOverlayPosition:(ObjCScreenPosition)v { GetConfig().overlay.position = (ScreenPosition)v; autoSave(); }

- (uint32_t)overlayTextColor               { return GetConfig().overlay.text_color; }
- (void)setOverlayTextColor:(uint32_t)v    { GetConfig().overlay.text_color = v; autoSave(); }

- (int32_t)overlayTextScale                { return GetConfig().overlay.text_scale; }
- (void)setOverlayTextScale:(int32_t)v     { GetConfig().overlay.text_scale = v; autoSave(); }

- (BOOL)overlayFPS                         { return GetConfig().overlay.fps; }
- (void)setOverlayFPS:(BOOL)v              { GetConfig().overlay.fps = v; autoSave(); }

- (BOOL)overlayCPUMode                     { return GetConfig().overlay.cpu_mode; }
- (void)setOverlayCPUMode:(BOOL)v          { GetConfig().overlay.cpu_mode = v; autoSave(); }

- (BOOL)overlayDrawcalls                   { return GetConfig().overlay.drawcalls; }
- (void)setOverlayDrawcalls:(BOOL)v        { GetConfig().overlay.drawcalls = v; autoSave(); }

- (BOOL)overlayCPUUsage                    { return GetConfig().overlay.cpu_usage; }
- (void)setOverlayCPUUsage:(BOOL)v         { GetConfig().overlay.cpu_usage = v; autoSave(); }

- (BOOL)overlayCPUPerCoreUsage             { return GetConfig().overlay.cpu_per_core_usage; }
- (void)setOverlayCPUPerCoreUsage:(BOOL)v  { GetConfig().overlay.cpu_per_core_usage = v; autoSave(); }

- (BOOL)overlayRAMUsage                    { return GetConfig().overlay.ram_usage; }
- (void)setOverlayRAMUsage:(BOOL)v         { GetConfig().overlay.ram_usage = v; autoSave(); }

- (BOOL)overlayVRAMUsage                   { return GetConfig().overlay.vram_usage; }
- (void)setOverlayVRAMUsage:(BOOL)v        { GetConfig().overlay.vram_usage = v; autoSave(); }

- (BOOL)overlayDebug                       { return GetConfig().overlay.debug; }
- (void)setOverlayDebug:(BOOL)v            { GetConfig().overlay.debug = v; autoSave(); }

- (BOOL)dumpGraphicsAssets
{
    return ActiveSettings::DumpTexturesEnabled() && ActiveSettings::DumpShadersEnabled();
}

- (void)setDumpGraphicsAssets:(BOOL)enabled
{
    if (enabled)
    {
        for (const char* directory : {"dump/textures", "dump/shaders"})
        {
            std::error_code error;
            fs::create_directories(ActiveSettings::GetUserDataPath(directory), error);
            if (error)
            {
                cemuLog_log(LogType::Force, "Cannot create graphics dump directory {}: {}", directory, error.message());
                return;
            }
        }
    }
    ActiveSettings::EnableDumpTextures(enabled);
    ActiveSettings::EnableDumpShaders(enabled);
}

- (ObjCScreenPosition)notificationPosition { return (ObjCScreenPosition)GetConfig().notification.position; }
- (void)setNotificationPosition:(ObjCScreenPosition)v { GetConfig().notification.position = (ScreenPosition)v; autoSave(); }

- (uint32_t)notificationTextColor          { return GetConfig().notification.text_color; }
- (void)setNotificationTextColor:(uint32_t)v { GetConfig().notification.text_color = v; autoSave(); }

- (int32_t)notificationTextScale           { return GetConfig().notification.text_scale; }
- (void)setNotificationTextScale:(int32_t)v { GetConfig().notification.text_scale = v; autoSave(); }

- (BOOL)notificationControllerProfiles     { return GetConfig().notification.controller_profiles; }
- (void)setNotificationControllerProfiles:(BOOL)v { GetConfig().notification.controller_profiles = v; autoSave(); }

- (BOOL)notificationControllerBattery      { return GetConfig().notification.controller_battery; }
- (void)setNotificationControllerBattery:(BOOL)v { GetConfig().notification.controller_battery = v; autoSave(); }

- (BOOL)notificationShaderCompiling        { return GetConfig().notification.shader_compiling; }
- (void)setNotificationShaderCompiling:(BOOL)v { GetConfig().notification.shader_compiling = v; autoSave(); }

- (BOOL)notificationFriends                { return GetConfig().notification.friends; }
- (void)setNotificationFriends:(BOOL)v     { GetConfig().notification.friends = v; autoSave(); }

- (NSArray<NSDictionary*>*)accounts {
    NSMutableArray* arr = [NSMutableArray new];
    for (const auto& account : Account::GetAccounts()) {
        [arr addObject:@{
            @"persistentId": @(account.GetPersistentId()),
            @"miiName": wstr(std::wstring(account.GetMiiName())),
            @"birthYear":    @(account.GetBirthYear()),
            @"birthMonth":   @(account.GetBirthMonth()),
            @"birthDay":     @(account.GetBirthDay()),
            @"gender":       @(account.GetGender()),
            @"email":        [NSString stringWithUTF8String:std::string(account.GetEmail()).c_str()],
            @"country":      @(account.GetCountry()),
            @"isValidOnline":@(account.IsValidOnlineAccount()),
        }];
    }
    return arr;
}

- (void)refreshAccounts { Account::RefreshAccounts(); }

- (NSArray<NSDictionary*>*)countries {
    NSMutableArray* arr = [NSMutableArray new];
    for (int i = 0; i < NCrypto::GetCountryCount(); ++i) {
        const char* country = NCrypto::GetCountryAsString(i);
        if (country && (i == 0 || std::string(country) != "NN")) {
            [arr addObject:@{
                @"code": @(i),
                @"name": [NSString stringWithUTF8String:country],
            }];
        }
    }
    return arr;
}

- (BOOL)accountExistsForPersistentId:(uint32_t)pid {
    Account::RefreshAccounts();
    return findAccount(pid) != nullptr;
}

- (nullable NSString*)createAccountWithPersistentId:(uint32_t)pid miiName:(NSString*)miiName {
    Account::RefreshAccounts();
    
    if (!Account::HasFreeAccountSlots()) {
        return @"Maximum account limit reached.";
    }
    
    if (pid < Account::kMinPersistendId) {
        return [NSString stringWithFormat:@"The persistent id must be greater than %x!", Account::kMinPersistendId];
    }
    
    if (const Account* existingAccount = findAccount(pid)) {
        return [NSString stringWithFormat:@"The persistent id %x is already in use by account %@!",
                pid,
                wstr(std::wstring{ existingAccount->GetMiiName() })];
    }
    
    std::wstring newName = nsToWstr(miiName);
    if (newName.empty()) {
        return @"Account name may not be empty!";
    }
    
    try {
        Account account(pid, newName);
        const auto error = account.Save();
        if (error) {
            return [NSString stringWithUTF8String:error.message().c_str()];
        }
        
        Account::RefreshAccounts();
        return nil;
    }
    catch (const std::exception& ex) {
        return [NSString stringWithUTF8String:ex.what()];
    }
}

- (nullable NSString*)deleteAccountWithPersistentId:(uint32_t)pid {
    Account::RefreshAccounts();
    
    if (Account::GetAccounts().size() == 1) {
        return @"Can't delete the only account!";
    }
    
    const Account* account = findAccount(pid);
    if (!account) {
        return @"Account not found.";
    }
    
    try {
        const fs::path path = account->GetFileName();
        fs::remove_all(path.parent_path());
        Account::RefreshAccounts();
        return nil;
    }
    catch (const std::exception& ex) {
        return [NSString stringWithUTF8String:ex.what()];
    }
    
}

- (BOOL)isOnlineFullyValidForPersistentId:(uint32_t)pid {
    Account::RefreshAccounts();
    const Account* account = findAccount(pid);
    return refreshAndLogOnlineDiagnostics(pid, account, "isOnlineFullyValid");
}

- (NSString*)onlineStatusForPersistentId:(uint32_t)pid {
    Account::RefreshAccounts();
    const Account* account = findAccount(pid);
    if (!account) {
        refreshAndLogOnlineDiagnostics(pid, account, "onlineStatus");
        return @"No account selected";
    }
    
    const bool onlineFullyValid = refreshAndLogOnlineDiagnostics(pid, account, "onlineStatus");
    if (ActiveSettings::HasRequiredOnlineFiles()) {
        if (account->IsValidOnlineAccount()) {
            return @"Selected account is a valid online account";
        }
        return @"Selected account is not linked to a NNID or PNID";
    }
    
    const OnlineValidator validator = account->ValidateOnlineFiles();
    if (validator.otp != validator.seeprom) {
        return @"OTP.bin or SEEPROM.bin is missing";
    }
    if (validator.otp == OnlineValidator::FileState::Ok && validator.seeprom == OnlineValidator::FileState::Ok) {
        return @"OTP and SEEPROM present but no certificate files were found";
    }
    if (onlineFullyValid) {
        return @"Selected account is a valid online account";
    }
    
    return @"Online play is not set up. Follow the guide below to get started";
}

- (NSString*)onlineValidationDetailsForPersistentId:(uint32_t)pid {
    Account::RefreshAccounts();
    const Account* account = findAccount(pid);
    if (!account) return @"No account selected";
    ActiveSettings::Init();
    const OnlineValidator validator = account->ValidateOnlineFiles();
    if (validator) return @"Selected account is a valid online account";
    
    NSMutableArray<NSString*>* errors = [NSMutableArray array];
    auto addFileError = [&](NSString* name, OnlineValidator::FileState state) {
        if (state == OnlineValidator::FileState::Missing)
            [errors addObject:[name stringByAppendingString:@" missing in Cemu directory"]];
        else if (state == OnlineValidator::FileState::Corrupted)
            [errors addObject:[name stringByAppendingString:@" is invalid"]];
    };
    addFileError(@"otp.bin", validator.otp);
    addFileError(@"seeprom.bin", validator.seeprom);
    if (!validator.missing_files.empty()) {
        [errors addObject:@"Missing certificate and key files:"];
        for (size_t i = 0; i < std::min<size_t>(validator.missing_files.size(), 11); ++i)
            [errors addObject:wstr(validator.missing_files[i])];
        if (validator.missing_files.size() > 11) [errors addObject:@"..."];
    }
    if (!validator.valid_account) {
        [errors addObject:@"The currently selected account is not a valid or dumped online account:"];
        switch (validator.account_error) {
            case OnlineAccountError::kNoAccountId:
                [errors addObject:@"AccountId missing (The account is not connected to a NNID/PNID)"];
                break;
            case OnlineAccountError::kNoPasswordCached:
                [errors addObject:@"IsPasswordCacheEnabled is set to false (The remember password option on your Wii U must be enabled for this account before dumping it)"];
                break;
            case OnlineAccountError::kPasswordCacheEmpty:
                [errors addObject:@"AccountPasswordCache is empty (The remember password option on your Wii U must be enabled for this account before dumping it)"];
                break;
            case OnlineAccountError::kNoPrincipalId:
                [errors addObject:@"PrincipalId missing"];
                break;
            default: break;
        }
    }
    return [@"The following error(s) have been found:\n" stringByAppendingString:[errors componentsJoinedByString:@"\n"]];
}

- (BOOL)setMiiName:(NSString*)name forPersistentId:(uint32_t)pid {
    Account account(Account::GetFileName(pid).wstring());

    if (account.Load()) return NO;

    std::wstring newName = nsToWstr(name);
    if (newName.empty()) {
        newName = L"default";
    }

    account.SetMiiName(newName);

    if (account.Save()) return NO;

    Account::RefreshAccounts();
    return YES;
}

- (BOOL)setGender:(int)gender forPersistentId:(uint32_t)pid {
    Account account(Account::GetFileName(pid).wstring());

    if (account.Load()) return NO;

    account.SetGender((uint8_t)gender);

    if (account.Save()) return NO;

    Account::RefreshAccounts();
    return YES;
}

- (BOOL)setEmail:(NSString*)email forPersistentId:(uint32_t)pid {
    Account account(Account::GetFileName(pid).wstring());

    if (account.Load()) return NO;

    account.SetEmail(email.UTF8String ?: "");

    if (account.Save()) return NO;

    Account::RefreshAccounts();
    return YES;
}

- (BOOL)setCountry:(int)country forPersistentId:(uint32_t)pid {
    Account account(Account::GetFileName(pid).wstring());
    
    if (account.Load()) return NO;
    
    account.SetCountry((uint32_t)country);
    
    if (account.Save()) return NO;
    
    Account::RefreshAccounts();
    return YES;
}

- (BOOL)setBirthYear:(uint16_t)year
               month:(uint8_t)month
                 day:(uint8_t)day
     forPersistentId:(uint32_t)pid {
    Account account(Account::GetFileName(pid).wstring());
    
    if (account.Load()) return NO;
    
    account.SetBirthYear(year);
    account.SetBirthMonth(month);
    account.SetBirthDay(day);
    
    if (account.Save()) return NO;
    
    Account::RefreshAccounts();
    return YES;
}

- (NSArray<NSString*>*)gamePaths {
    NSMutableArray* arr = [NSMutableArray new];
    for (auto& p : GetConfig().game_paths)
        [arr addObject:[NSString stringWithUTF8String:p.c_str()]];
    return arr;
}

- (void)addGamePath:(NSString*)path {
    GetConfig().game_paths.push_back(path.UTF8String);
    autoSave();
}

- (void)removeGamePath:(NSString*)path {
    auto& paths = GetConfig().game_paths;
    std::string target = path.UTF8String;
    paths.erase(std::remove(paths.begin(), paths.end(), target), paths.end());
    autoSave();
}

- (BOOL)isGameListFavorite:(uint64_t)titleId {
    return GetConfig().IsGameListFavorite(titleId);
}

- (void)setGameListFavorite:(uint64_t)titleId isFavorite:(BOOL)favorite {
    GetConfig().SetGameListFavorite(titleId, favorite);
    autoSave();
}

- (nullable NSString*)customNameForTitleId:(uint64_t)titleId {
    std::string name;
    
    if (GetConfig().GetGameListCustomName(titleId, name))
        return [NSString stringWithUTF8String:name.c_str()];
    
    return nil;
}

- (void)setCustomName:(NSString*)name forTitleId:(uint64_t)titleId {
    GetConfig().SetGameListCustomName(titleId, name.UTF8String ?: "");
    autoSave();
}

- (void)setMLCPath:(NSString*)path save:(BOOL)save {
    GetConfig().SetMLCPath(fs::path(path.UTF8String), save);
}

- (void)save {
    GetConfigHandle().Save();
}

+ (int)audioChannelsToNChannels:(ObjCAudioChannels)channels {
    return CemuConfig::AudioChannelsToNChannels((AudioChannels)channels);
}

@end
