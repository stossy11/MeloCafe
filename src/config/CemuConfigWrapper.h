//
//  CemuConfigWrapper.h
//  MeloCafe
//
//  Created by Stossy11 on 5/4/2026.
//

#pragma once
#import <Foundation/Foundation.h>

typedef NS_ENUM(NSInteger, ObjCGraphicAPI) {
    ObjCGraphicAPIOpenGL = 0,
    ObjCGraphicAPIVulkan,
    ObjCGraphicAPIMetal,
};

typedef NS_ENUM(NSInteger, ObjCAudioChannels) {
    ObjCAudioChannelsMono = 0,
    ObjCAudioChannelsStereo,
    ObjCAudioChannelsSurround,
};

typedef NS_ENUM(NSInteger, ObjCUpscalingFilter) {
    ObjCUpscalingFilterLinear,
    ObjCUpscalingFilterBicubic,
    ObjCUpscalingFilterBicubicHermite,
    ObjCUpscalingFilterNearestNeighbor,
};

typedef NS_ENUM(NSInteger, ObjCFullscreenScaling) {
    ObjCFullscreenScalingKeepAspectRatio,
    ObjCFullscreenScalingStretch,
};

typedef NS_ENUM(NSInteger, ObjCScreenPosition) {
    ObjCScreenPositionDisabled = 0,
    ObjCScreenPositionTopLeft,
    ObjCScreenPositionTopCenter,
    ObjCScreenPositionTopRight,
    ObjCScreenPositionBottomLeft,
    ObjCScreenPositionBottomCenter,
    ObjCScreenPositionBottomRight,
};

typedef NS_ENUM(NSInteger, ObjCCPUMode) {
    ObjCCPUModeSinglecoreInterpreter = 0,
    ObjCCPUModeMulticoreInterpreter =  1,
    ObjCCPUModeSinglecoreRecompiler  = 2,
    ObjCCPUModeDualcoreRecompiler    = 3,
    ObjCCPUModeMulticoreRecompiler   = 4,
    ObjCCPUModeAuto                  = 5,
};

typedef NS_ENUM(NSInteger, ObjCCafeConsoleLanguage) {
    ObjCCafeConsoleLanguageJA = 0,
    ObjCCafeConsoleLanguageEN = 1,
    ObjCCafeConsoleLanguageFR = 2,
    ObjCCafeConsoleLanguageDE = 3,
    ObjCCafeConsoleLanguageIT = 4,
    ObjCCafeConsoleLanguageES = 5,
    ObjCCafeConsoleLanguageZH = 6,
    ObjCCafeConsoleLanguageKO = 7,
    ObjCCafeConsoleLanguageNL = 8,
    ObjCCafeConsoleLanguagePT = 9,
    ObjCCafeConsoleLanguageRU = 10,
    ObjCCafeConsoleLanguageTW = 11,
};

typedef NS_ENUM(NSInteger, ObjCNetworkService) {
    ObjCNetworkServiceOffline = 0,
    ObjCNetworkServiceNintendo,
    ObjCNetworkServicePretendo,
    ObjCNetworkServiceCustom,
};

typedef NS_ENUM(NSInteger, ObjCCrashDump) {
    ObjCCrashDumpDisabled,
#if TARGET_OS_OSX || TARGET_OS_IOS
    ObjCCrashDumpEnabled,
#else
    ObjCCrashDumpLite,
    ObjCCrashDumpFull,
#endif
};

typedef NS_ENUM(NSInteger, ObjCPrecompiledShaderOption) {
    ObjCPrecompiledShaderOptionAuto,
    ObjCPrecompiledShaderOptionEnable,
    ObjCPrecompiledShaderOptionDisable,
};

@interface CemuConfigWrapper : NSObject

+ (instancetype _Nonnull )shared;

- (void) loadConfig;
- (void) saveConfig;

@property (nonatomic) uint64_t logFlag;
@property (nonatomic) BOOL advancedPPCLogging;
@property (nonatomic) BOOL permanentStorage;
@property (nonatomic, copy) NSString* _Nonnull mlcPath;
@property (nonatomic, copy) NSString* _Nonnull proxyServer;
@property (nonatomic) BOOL disableScreensaver;
@property (nonatomic) BOOL playBootSound;

@property (nonatomic) ObjCCPUMode cpuMode;

@property (nonatomic) ObjCCafeConsoleLanguage consoleLanguage;

@property (nonatomic) ObjCGraphicAPI graphicAPI;
@property (nonatomic) int vsync;
@property (nonatomic) BOOL gx2DrawDoneSync;
@property (nonatomic) BOOL renderUpsideDown;
@property (nonatomic) BOOL asyncCompile;
@property (nonatomic) ObjCPrecompiledShaderOption precompiledShaders;
@property (nonatomic) BOOL vkAccurateBarriers;
@property (nonatomic) ObjCUpscalingFilter upscaleFilter;
@property (nonatomic) ObjCUpscalingFilter downscaleFilter;
@property (nonatomic) ObjCFullscreenScaling fullscreenScaling;
@property (nonatomic) float textureResolutionScale;
@property (nonatomic) BOOL disableStreamout;

@property (nonatomic) BOOL overrideAppGammaPreference;
@property (nonatomic) float overrideGammaValue;
@property (nonatomic) float userDisplayGamma;

// Metal
@property (nonatomic) BOOL forceMeshShaders;
@property (nonatomic, copy) NSString* _Nonnull gpuCaptureDir;
@property (nonatomic) BOOL framebufferFetch;

@property (nonatomic) int audioAPI;
@property (nonatomic) int audioDelay;
@property (nonatomic) BOOL microphoneEnabled;
@property (nonatomic) BOOL tvAudioEnabled;
@property (nonatomic) BOOL padAudioEnabled;
@property (nonatomic) ObjCAudioChannels tvChannels;
@property (nonatomic) ObjCAudioChannels padChannels;
@property (nonatomic) ObjCAudioChannels inputChannels;
@property (nonatomic) int tvVolume;
@property (nonatomic) int padVolume;
@property (nonatomic) int inputVolume;
@property (nonatomic) int portalVolume;
@property (nonatomic, copy) NSString* _Nonnull tvDevice;
@property (nonatomic, copy) NSString* _Nonnull padDevice;
@property (nonatomic, copy) NSString* _Nonnull inputDevice;
@property (nonatomic, copy) NSString* _Nonnull portalDevice;

@property (nonatomic) uint32_t persistentId;
@property (nonatomic) BOOL legacyOnlineEnabled;
- (ObjCNetworkService)networkServiceForPersistentId:(uint32_t)persistentId;
- (void)setNetworkService:(ObjCNetworkService)service forPersistentId:(uint32_t)persistentId;

@property (nonatomic, copy) NSString* _Nonnull dsuHost;
@property (nonatomic) uint16_t dsuPort;
@property (nonatomic) BOOL disableMotion;

@property (nonatomic) ObjCCrashDump crashDump;
@property (nonatomic) uint16_t gdbPort;

@property (nonatomic) BOOL emulateSkylanderPortal;
@property (nonatomic) BOOL emulateInfinityBase;
@property (nonatomic) BOOL emulateDimensionsToypad;

@property (nonatomic) ObjCScreenPosition overlayPosition;
@property (nonatomic) uint32_t overlayTextColor;
@property (nonatomic) int32_t overlayTextScale;
@property (nonatomic) BOOL overlayFPS;
@property (nonatomic) BOOL overlayCPUMode;
@property (nonatomic) BOOL overlayDrawcalls;
@property (nonatomic) BOOL overlayCPUUsage;
@property (nonatomic) BOOL overlayCPUPerCoreUsage;
@property (nonatomic) BOOL overlayRAMUsage;
@property (nonatomic) BOOL overlayVRAMUsage;
@property (nonatomic) BOOL overlayDebug;
// Diagnostic asset dumping is opt-in for the current app session only.
@property (nonatomic) BOOL dumpGraphicsAssets;

@property (nonatomic) ObjCScreenPosition notificationPosition;
@property (nonatomic) uint32_t notificationTextColor;
@property (nonatomic) int32_t notificationTextScale;
@property (nonatomic) BOOL notificationControllerProfiles;
@property (nonatomic) BOOL notificationControllerBattery;
@property (nonatomic) BOOL notificationShaderCompiling;
@property (nonatomic) BOOL notificationFriends;

@property (nonatomic, readonly) NSArray<NSString*>* _Nonnull gamePaths;
- (void)addGamePath:(NSString* _Nonnull)path;
- (void)removeGamePath:(NSString* _Nonnull)path;

- (BOOL)isGameListFavorite:(uint64_t)titleId;
- (void)setGameListFavorite:(uint64_t)titleId isFavorite:(BOOL)favorite;
- (nullable NSString*)customNameForTitleId:(uint64_t)titleId;
- (void)setCustomName:(NSString* _Nonnull)name forTitleId:(uint64_t)titleId;

- (void)setMLCPath:(NSString* _Nonnull)path save:(BOOL)save;

- (void)save;

+ (int)audioChannelsToNChannels:(ObjCAudioChannels)channels;

- (NSArray<NSDictionary*>* _Nonnull)accounts;
- (void)refreshAccounts;

@property (nonatomic) uint32_t activeAccountPersistentId;
@property (nonatomic, readonly) uint32_t minimumAccountPersistentId;
@property (nonatomic, readonly) uint32_t nextAccountPersistentId;
@property (nonatomic, readonly) BOOL hasFreeAccountSlots;
@property (nonatomic, readonly) BOOL isTitleRunning;
@property (nonatomic, readonly) BOOL canUseCustomNetworkService;
@property (nonatomic, readonly) NSArray<NSDictionary*>* _Nonnull countries;

- (BOOL)accountExistsForPersistentId:(uint32_t)pid;
- (nullable NSString*)createAccountWithPersistentId:(uint32_t)pid miiName:(NSString* _Nonnull)miiName;
- (nullable NSString*)deleteAccountWithPersistentId:(uint32_t)pid;
- (NSString* _Nonnull)onlineStatusForPersistentId:(uint32_t)pid;
- (NSString* _Nonnull)onlineValidationDetailsForPersistentId:(uint32_t)pid;
- (BOOL)isOnlineFullyValidForPersistentId:(uint32_t)pid;

- (BOOL)setMiiName:(NSString* _Nonnull)name forPersistentId:(uint32_t)pid;
- (BOOL)setGender:(int)gender forPersistentId:(uint32_t)pid;
- (BOOL)setEmail:(NSString* _Nonnull)email forPersistentId:(uint32_t)pid;
- (BOOL)setCountry:(int)country forPersistentId:(uint32_t)pid;
- (BOOL)setBirthYear:(uint16_t)year month:(uint8_t)month day:(uint8_t)day forPersistentId:(uint32_t)pid;

@end
