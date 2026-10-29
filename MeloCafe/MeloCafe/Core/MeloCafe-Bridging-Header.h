//
//  Use this file to import your target's public headers that you would like to expose to Swift.
//



#ifndef MeloCafeHeader
#define MeloCafeHeader
#include <UIKit/UIKit.h>
#include "Controller/CemuController.h"
#include "../../../src/input/api/Wiimote/BLEWiimoteBridge.h"
#include "../../../src/config/CemuConfigWrapper.h"
#include "../../../src/config/GraphicPackWrapper.h"
#include "../../../src/gui/uikit/EmulatedUSBDevices.h"

void CemuUIKit_UpdateMainWindowSize(CGFloat width, CGFloat height, CGFloat scale);

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>

typedef struct GameInfo {
    unsigned long long titleId;
    const char* name;
    const char* path;
    bool isBaseGame;
} GameInfo;

typedef struct GameIconResult {
    uint8_t* data;
    uint32_t size;
} GameIconResult;


void CemuUIKit_SetMainWindow(UIWindow* window);

void CemuUIKit_SetMainView(UIView* view);

void CemuUIKit_SetPadView(UIView* view);

void CemuUIKit_InitializeLayer(bool main);

void CemuUIKit_ShutdownLayer(bool main);

void CemuUIKit_SetDRCPrimary(bool enabled);

void CemuUIKit_SetVisibleOutputs(bool tv, bool pad);

void CemuUIKit_SetGameLoadedCallback(void (*callback)());

void CemuUIKit_SetGameExitCallback(void (*callback)());


void CemuUIKit_UpdatePadWindowSize();

void CemuUIKit_SetPadTouch(CGFloat x, CGFloat y, bool down);

void CemuInitialize(const char* execPath, const char* user_data_path, const char* config_path, const char* cache_path, const char*  data_path);

bool SetInterpreter(bool interpreter);

bool CemuLoadTitle(uint64_t titleId);

unsigned long long CemuLoadFile(const char* launchPath, bool load);

void CemuRun();

void CemuShutdown();

GameInfo* CemuGetAllGames(bool includeUpdates, bool includeDLC, int* outCount);

void CemuFreeGameList(GameInfo* list, int count);

GameIconResult CemuGetGameIcon(unsigned long long titleId);

void Cemu_FreeGameIcon(GameIconResult* icon);

bool CemuInitJIT();

extern size_t g_available_memory;

#ifdef __cplusplus
}
#endif

#endif
