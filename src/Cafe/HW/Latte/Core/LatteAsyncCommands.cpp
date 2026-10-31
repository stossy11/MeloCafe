#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteAsyncCommands.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LatteSurfaceCopy.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Cafe/HW/Latte/Core/LatteWorkEvent.h"
#include <atomic>

void LatteThread_Exit();

SlimRWLock swl_gpuAsyncCommands;

typedef struct  
{
	uint32 type;
	union
	{
		struct
		{
			MPTR physAddr;
			MPTR mipAddr;
			uint32 swizzle;
			sint32 format;
			sint32 width;
			sint32 height;
			sint32 depth;
			uint32 pitch;
			uint32 slice;
			sint32 dim;
			Latte::E_HWTILEMODE tilemode;
			sint32 aa;
			sint32 level;
		}forceTextureReadback;

		struct
		{
			uint64 shaderBaseHash; 
			uint64 shaderAuxHash; 
			LatteConst::ShaderType shaderType;
		}deleteShader;

		struct
		{
			LatteSurfaceCopyParam src;
			LatteSurfaceCopyParam dst;
			LatteSurfaceCopyRect rect;
		}textureCopy;
	};
}LatteAsyncCommand_t;

#define ASYNC_CMD_FORCE_TEXTURE_READBACK		1
#define ASYNC_CMD_DELETE_SHADER					2
#define ASYNC_CMD_TEXTURE_COPY					3

std::queue<LatteAsyncCommand_t> LatteAsyncCommandQueue;
static std::atomic<uint32> s_pendingAsyncCommands{0};

void LatteAsyncCommands_queueForceTextureReadback(MPTR physAddr, MPTR mipAddr, uint32 swizzle, sint32 format, sint32 width, sint32 height, sint32 depth, uint32 pitch, uint32 slice, sint32 dim, Latte::E_HWTILEMODE tilemode, sint32 aa, sint32 level)
{
	LatteAsyncCommand_t asyncCommand = {};
	// setup command
	asyncCommand.type = ASYNC_CMD_FORCE_TEXTURE_READBACK;
	
	asyncCommand.forceTextureReadback.physAddr = physAddr;
	asyncCommand.forceTextureReadback.mipAddr = mipAddr;
	asyncCommand.forceTextureReadback.swizzle = swizzle;
	asyncCommand.forceTextureReadback.format = format;
	asyncCommand.forceTextureReadback.width = width;
	asyncCommand.forceTextureReadback.height = height;
	asyncCommand.forceTextureReadback.depth = depth;
	asyncCommand.forceTextureReadback.pitch = pitch;
	asyncCommand.forceTextureReadback.slice = slice;
	asyncCommand.forceTextureReadback.dim = dim;
	asyncCommand.forceTextureReadback.tilemode = tilemode;
	asyncCommand.forceTextureReadback.aa = aa;
	asyncCommand.forceTextureReadback.level = level;
	swl_gpuAsyncCommands.LockWrite();
	LatteAsyncCommandQueue.push(asyncCommand);
	s_pendingAsyncCommands.fetch_add(1, std::memory_order::release);
	swl_gpuAsyncCommands.UnlockWrite();
	Latte_GetWorkEvent().Notify();
}

void LatteAsyncCommands_queueDeleteShader(uint64 shaderBaseHash, uint64 shaderAuxHash, LatteConst::ShaderType shaderType)
{
	LatteAsyncCommand_t asyncCommand = {};
	// setup command
	asyncCommand.type = ASYNC_CMD_DELETE_SHADER;

	asyncCommand.deleteShader.shaderBaseHash = shaderBaseHash;
	asyncCommand.deleteShader.shaderAuxHash = shaderAuxHash;
	asyncCommand.deleteShader.shaderType = shaderType;

	swl_gpuAsyncCommands.LockWrite();
	LatteAsyncCommandQueue.push(asyncCommand);
	s_pendingAsyncCommands.fetch_add(1, std::memory_order::release);
	swl_gpuAsyncCommands.UnlockWrite();
	Latte_GetWorkEvent().Notify();
}

void LatteAsyncCommand_queueTextureCopy(const LatteSurfaceCopyParam& src, const LatteSurfaceCopyParam& dst, const LatteSurfaceCopyRect& rect)
{
	LatteAsyncCommand_t asyncCommand = {};
	// setup command
	asyncCommand.type = ASYNC_CMD_TEXTURE_COPY;
	asyncCommand.textureCopy.src = src;
	asyncCommand.textureCopy.dst = dst;
	asyncCommand.textureCopy.rect = rect;

	swl_gpuAsyncCommands.LockWrite();
	LatteAsyncCommandQueue.push(asyncCommand);
	s_pendingAsyncCommands.fetch_add(1, std::memory_order::release);
	swl_gpuAsyncCommands.UnlockWrite();
	Latte_GetWorkEvent().Notify();
}

void LatteAsyncCommands_waitUntilAllProcessed()
{
	uint32 spinCount = 0;
	while (uint32 pending = s_pendingAsyncCommands.load(std::memory_order::acquire))
	{
		if (spinCount++ < 80)
			_mm_pause();
		else
		{
			s_pendingAsyncCommands.wait(pending, std::memory_order::acquire);
			spinCount = 0;
		}
	}
}

/*
 * Called by the GPU command processor frequently
 */
void LatteAsyncCommands_checkAndExecute()
{
	// inspect the atomic count rather than accessing std::queue without its lock.
	if (Latte_GetStopSignal())
		LatteThread_Exit();
	if (s_pendingAsyncCommands.load(std::memory_order::acquire) == 0)
		return;
	swl_gpuAsyncCommands.LockWrite();
	while (LatteAsyncCommandQueue.empty() == false)
	{
		// get first command in queue
		LatteAsyncCommand_t asyncCommand = LatteAsyncCommandQueue.front();
		swl_gpuAsyncCommands.UnlockWrite();
		if (asyncCommand.type == ASYNC_CMD_FORCE_TEXTURE_READBACK)
		{
			cemu_assert_debug(asyncCommand.forceTextureReadback.level == 0); // implement mip swizzle and verify
			LatteTextureView* textureView = LatteTC_GetTextureSliceViewOrTryCreate(asyncCommand.forceTextureReadback.physAddr, asyncCommand.forceTextureReadback.mipAddr, (Latte::E_GX2SURFFMT)asyncCommand.forceTextureReadback.format, asyncCommand.forceTextureReadback.tilemode, asyncCommand.forceTextureReadback.width, asyncCommand.forceTextureReadback.height, asyncCommand.forceTextureReadback.depth, asyncCommand.forceTextureReadback.pitch, 0, asyncCommand.forceTextureReadback.slice, asyncCommand.forceTextureReadback.level);
			if (textureView != nullptr)
			{
				LatteTexture_UpdateDataToLatest(textureView->baseTexture);
				// start transfer
				LatteTextureReadback_StartTransfer(textureView);
				// wait until finished
				LatteTextureReadback_UpdateFinishedTransfers(true);
			}
			else
			{
				cemuLog_logDebug(LogType::Force, "Texture not found for readback");
			}
		}
		else if (asyncCommand.type == ASYNC_CMD_DELETE_SHADER)
		{
			LatteSHRC_RemoveFromCacheByHash(asyncCommand.deleteShader.shaderBaseHash, asyncCommand.deleteShader.shaderAuxHash, asyncCommand.deleteShader.shaderType);
		}
		else if (asyncCommand.type == ASYNC_CMD_TEXTURE_COPY)
		{
			LatteSurfaceCopy_copySurfaceNew(asyncCommand.textureCopy.src, asyncCommand.textureCopy.dst, asyncCommand.textureCopy.rect);
		}
		else
		{
			cemu_assert_unimplemented();
		}
		swl_gpuAsyncCommands.LockWrite();
		LatteAsyncCommandQueue.pop();
		s_pendingAsyncCommands.fetch_sub(1, std::memory_order::release);
		s_pendingAsyncCommands.notify_all();
	}
	swl_gpuAsyncCommands.UnlockWrite();
}
