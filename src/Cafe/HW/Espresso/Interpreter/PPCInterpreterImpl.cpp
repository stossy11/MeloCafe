#include "PPCInterpreterInternal.h"
#include <array>
#include <bit>
#include <cstring>
#include <atomic>
#include <vector>
#include <algorithm>
#include <mutex>
#include <thread>
#include <chrono>
#include "PPCInterpreterHelper.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/HW/Espresso/Debugger/Debugger.h"
#include "Cafe/HW/Espresso/Debugger/GDBStub.h"

class PPCItpCafeOSUsermode
{
public:
    static const bool allowSupervisorMode = false;
    static const bool allowDSI = false;
    
    inline static uint32 memory_readCodeU32(PPCInterpreter_t* hCPU, uint32 address)
    {
        return _swapEndianU32(*(uint32*)(memory_base + address));
    }
    
    inline static void ppcMem_writeDataDouble(PPCInterpreter_t* hCPU, uint32 address, double vf)
    {
        const uint64 v = CPU_swapEndianU64(std::bit_cast<uint64>(vf));
        
        std::memcpy(memory_getPointerFromVirtualOffset(address), &v, sizeof(v));
    }
    
    inline static void ppcMem_writeDataU64(PPCInterpreter_t* hCPU, uint32 address, uint64 v)
    {
        *(uint64*)(memory_getPointerFromVirtualOffset(address)) = CPU_swapEndianU64(v);
    }
    
    inline static void ppcMem_writeDataU32(PPCInterpreter_t* hCPU, uint32 address, uint32 v)
    {
        *(uint32*)(memory_getPointerFromVirtualOffset(address)) = CPU_swapEndianU32(v);
    }
    
    inline static void ppcMem_writeDataU16(PPCInterpreter_t* hCPU, uint32 address, uint16 v)
    {
        *(uint16*)(memory_getPointerFromVirtualOffset(address)) = CPU_swapEndianU16(v);
    }
    
    inline static void ppcMem_writeDataU8(PPCInterpreter_t* hCPU, uint32 address, uint8 v)
    {
        *(uint8*)(memory_getPointerFromVirtualOffset(address)) = v;
    }
    
    inline static double ppcMem_readDataDouble(PPCInterpreter_t* hCPU, uint32 address)
    {
        uint64 v;
        std::memcpy(&v, memory_getPointerFromVirtualOffset(address), sizeof(v));
        return std::bit_cast<double>(CPU_swapEndianU64(v));
    }
    
    inline static float ppcMem_readDataFloat(PPCInterpreter_t* hCPU, uint32 address)
    {
        uint32 v = *(uint32*)(memory_getPointerFromVirtualOffset(address));
        v = CPU_swapEndianU32(v);
        return *(float*)&v;
    }
    
    inline static uint64 ppcMem_readDataU64(PPCInterpreter_t* hCPU, uint32 address)
    {
        uint64 v = *(uint64*)(memory_getPointerFromVirtualOffset(address));
        return CPU_swapEndianU64(v);
    }
    
    inline static uint32 ppcMem_readDataU32(PPCInterpreter_t* hCPU, uint32 address)
    {
        uint32 v = *(uint32*)(memory_getPointerFromVirtualOffset(address));
        return CPU_swapEndianU32(v);
    }
    
    inline static uint16 ppcMem_readDataU16(PPCInterpreter_t* hCPU, uint32 address)
    {
        uint16 v = *(uint16*)(memory_getPointerFromVirtualOffset(address));
        return CPU_swapEndianU16(v);
    }
    
    inline static uint8 ppcMem_readDataU8(PPCInterpreter_t* hCPU, uint32 address)
    {
        return *(uint8*)(memory_getPointerFromVirtualOffset(address));
    }
    
    inline static uint64 ppcMem_readDataFloatEx(PPCInterpreter_t* hCPU, uint32 addr)
    {
        return ConvertToDoubleNoFTZ(_swapEndianU32(*(uint32*)(memory_base + addr)));
    }
    
    inline static void ppcMem_writeDataFloatEx(PPCInterpreter_t* hCPU, uint32 addr, uint64 value)
    {
        *(uint32*)(memory_base + addr) = _swapEndianU32(ConvertToSingleNoFTZ(value));
    }
    
    inline static uint64 getTB(PPCInterpreter_t* hCPU)
    {
        return PPCInterpreter_getMainCoreCycleCounter();
    }
};

uint32 debug_lastTranslatedHit;

void generateDSIException(PPCInterpreter_t* hCPU, uint32 dataAddress)
{
	// todo - check if we are already inside an interrupt handler (in which case the DSI exception is queued and not executed immediately?)

	// set flag to cancel current instruction
	hCPU->memoryException = true;

	hCPU->sprExtended.srr0 = hCPU->instructionPointer;
	hCPU->sprExtended.srr1 = hCPU->sprExtended.msr & 0x87C0FFFF;

	hCPU->sprExtended.dar = dataAddress;

	hCPU->sprExtended.msr &= ~0x04EF36;

	hCPU->instructionPointer = 0xFFF00300;


	uint32 dsisr = 0;
	dsisr |= (1<<(31-1)); // set if no TLB/BAT match found

	hCPU->sprExtended.dsisr = dsisr;

}

class PPCItpSupervisorWithMMU
{
public:
    static const bool allowSupervisorMode = true;
    static const bool allowDSI = true;

    inline static uint32 ppcMem_translateVirtualDataToPhysicalAddr(PPCInterpreter_t* hCPU, uint32 vAddr)
    {
        // check if address translation is disabled for data accesses
        if (GET_MSR_BIT(MSR_DR) == 0)
        {
            return vAddr;
        }

#ifdef CEMU_DEBUG_ASSERT
        if (hCPU->memoryException)
            assert_dbg(); // should not be set anymore
#endif

        // how to determine if BAT is valid:
        // BAT_entry_valid = (Vs & ~MSR[PR]) | (Vp & MSR[PR]) (The entry has separate enable flags for usermode and supervisor mode)
        for (sint32 i = 0; i < 8; i++)
        {
            // upper
            uint32 batU = hCPU->sprExtended.dbatU[i];
            uint32 BEPI = ((batU >> 17) & 0x7FFF) << 17;
            uint32 Vp = (batU >> 0) & 1;
            uint32 Vs = (batU >> 1) & 1;
            uint32 BL = (((batU >> 2) & 0x7FF) ^ 0x7FF) << 17;
            BL |= 0xF0000000;
            if (Vs == 0)
                continue; // todo - check if in supervisor/usermode
            // lower
            uint32 batL = hCPU->sprExtended.dbatL[i];
            uint32 PP = (batL >> 0) & 3;
            uint32 WIMG = (batL >> 3) & 0xF;
            uint32 BRPN = ((batL >> 17) & 0x7FFF) << 17;

            // check for match
            if ((vAddr&BL) == BEPI)
            {
                // match
                vAddr = (vAddr&~BL) | (BRPN&BL);
                debug_lastTranslatedHit = vAddr;
                return vAddr;
            }
        }

        // no match
        debug_lastTranslatedHit = 0xFFFFFFFF;

        // find segment
        uint32 segmentIndex = (vAddr>>28);
        //uint32 pageIndex = (vAddr >> 12) & 0xFFFF; // for 4KB pages
        // uint32 byteOffset = vAddr & 0xFFF; // for 4KB pages
        uint32 pageIndex = (vAddr >> 17) & 0x7FF; // for 128KB pages
        uint32 byteOffset = vAddr & 0x1FFFF;
        uint32 srValue = hCPU->sprExtended.sr[segmentIndex];
        
        uint8 sr_ks = (srValue >> 30) & 1; // supervisor
        uint8 sr_kp = (srValue >> 29) & 1; // user mode
        uint8 sr_n = (srValue >> 28) & 1; // no-execute
        uint32 sr_vsid = (srValue & 0xFFFFFF);
        //uint32 vpn = pageIndex | (sr_vsid << 16); // 40bit virtual page number


        // look up in page table
        //uint32 lookupHash = (sr_vsid ^ pageIndex) & 0x7FFFF; // not correct for 4KB pages? sr_vsid must be shifted?
        //uint32 lookupHash = (sr_vsid ^ pageIndex) & 0x7FFFF;
        //uint32 lookupHash = ((sr_vsid>>8) ^ pageIndex) & 0x7FFFF;
        uint32 lookupHash = ((sr_vsid >> 0) ^ pageIndex) & 0x7FFFF;

        //lookupHash ^= 0x7FFFF;

        uint32 pageTableAddr = hCPU->sprExtended.sdr1&0xFFFF0000;
        uint32 pageTableMask = hCPU->sprExtended.sdr1&0x1FF;

        for (uint32 ch = 0; ch < 2; ch++)
        {
            uint32 ptegSelectLow = (lookupHash & 0x3FF);
            uint32 maskOR = (lookupHash >> 10) & pageTableMask;

            uint32* pteg = (uint32*)(memory_base + (pageTableAddr | (maskOR << 16)) + ptegSelectLow * 64);
            for (sint32 t = 0; t < 8; t++)
            {
                uint32 w0 = _swapEndianU32(pteg[0]);
                uint32 w1 = _swapEndianU32(pteg[1]);
                pteg += 2;
                if ((w0 & 0x80000000) == 0)
                    continue; // entry not valid

                uint32 abPageIndex = (w0 >> 0) & 0x3F;
                uint8 h = (w0 >> 6) & 1;
                uint32 ptegVSID = (w0 >> 7) & 0xFFFFFF;

                if (abPageIndex == (pageIndex >> 5) && ptegVSID == sr_vsid && h == ch)
                {
                    if (ch == 1)
                        assert_dbg();
                    // match
                    uint32 ptegPhysicalPage = (w1 >> 12) & 0xFFFFF;
                    // replace page (128KB)
                    vAddr = (vAddr & ~0xFFFE0000) | (ptegPhysicalPage << 12);
                    return vAddr;

                }
            }
            // calculate hash 2
            lookupHash = ~lookupHash;
        }

        cemuLog_logDebug(LogType::Force, "DSI exception at 0x{:08x} DataAddress {:08x}", hCPU->instructionPointer, vAddr);

        generateDSIException(hCPU, vAddr);

        // todo: Check hash func 1
        // todo: Check protection bits
        // todo: Check supervisor/usermode bits


        // also use this function in all the mem stuff below

        // note: bat has higher priority than TLB

        // since iterating the bats and page table is too slow, we need to pre-process the data somehow.

        return vAddr;
    }

    inline static uint32 ppcMem_translateVirtualCodeToPhysicalAddr(PPCInterpreter_t* hCPU, uint32 vAddr)
    {
        // check if address translation is disabled for instruction accesses
        if (GET_MSR_BIT(MSR_IR) == 0)
        {
            return vAddr;
        }

        // how to determine if BAT is valid:
        // BAT_entry_valid = (Vs & ~MSR[PR]) | (Vp & MSR[PR]) (The entry has separate enable flags for usermode and supervisor mode)
        for (sint32 i = 0; i < 8; i++)
        {
            // upper
            uint32 batU = hCPU->sprExtended.ibatU[i];
            uint32 BEPI = ((batU >> 17) & 0x7FFF) << 17;
            uint32 Vp = (batU >> 0) & 1;
            uint32 Vs = (batU >> 1) & 1;
            uint32 BL = (((batU >> 2) & 0x7FF) ^ 0x7FF) << 17;
            BL |= 0xF0000000;
            if (Vs == 0)
                continue; // todo - check if in supervisor/usermode
            // lower
            uint32 batL = hCPU->sprExtended.ibatL[i];
            uint32 PP = (batL >> 0) & 3;
            uint32 WIMG = (batL >> 3) & 0xF;
            uint32 BRPN = ((batL >> 17) & 0x7FFF) << 17;

            // check for match
            if ((vAddr&BL) == BEPI)
            {
                // match
                vAddr = (vAddr&~BL) | (BRPN&BL);
                debug_lastTranslatedHit = vAddr;
                return vAddr;
            }
        }
        assert_dbg();

        // no match
        // todo - throw exception if translation is enabled?
        return vAddr;
    }

    static uint32 memory_readCodeU32(PPCInterpreter_t* hCPU, uint32 address)
    {
        return _swapEndianU32(*(uint32*)(memory_base + ppcMem_translateVirtualCodeToPhysicalAddr(hCPU, address)));
    }

    inline static uint8* ppcMem_getDataPtr(PPCInterpreter_t* hCPU, uint32 vAddr)
    {
        return memory_base + ppcMem_translateVirtualDataToPhysicalAddr(hCPU, vAddr);
    }

    inline static void ppcMem_writeDataDouble(PPCInterpreter_t* hCPU, uint32 address, double vf)
    {
        const uint64 v = CPU_swapEndianU64(std::bit_cast<uint64>(vf));
        
        std::memcpy(ppcMem_getDataPtr(hCPU, address), &v, sizeof(v));
    }

    inline static void ppcMem_writeDataU64(PPCInterpreter_t* hCPU, uint32 address, uint64 v)
    {
        *(uint64*)(ppcMem_getDataPtr(hCPU, address)) = CPU_swapEndianU64(v);
    }

    inline static void ppcMem_writeDataU32(PPCInterpreter_t* hCPU, uint32 address, uint32 v)
    {
        uint32 pAddr = ppcMem_translateVirtualDataToPhysicalAddr(hCPU, address);
        if (hCPU->memoryException)
            return;

        if (pAddr >= 0x0c000000 && pAddr < 0x0d100000)
        {
            cemu_assert_unimplemented();
            return;
        }
        *(uint32*)(memory_base + pAddr) = CPU_swapEndianU32(v);
    }

    inline static void ppcMem_writeDataU16(PPCInterpreter_t* hCPU, uint32 address, uint16 v)
    {
        *(uint16*)(ppcMem_getDataPtr(hCPU, address)) = CPU_swapEndianU16(v);
    }

    inline static void ppcMem_writeDataU8(PPCInterpreter_t* hCPU, uint32 address, uint8 v)
    {
        *(uint8*)(ppcMem_getDataPtr(hCPU, address)) = v;
    }

    inline static double ppcMem_readDataDouble(PPCInterpreter_t* hCPU, uint32 address)
    {
        uint64 v;
        std::memcpy(&v, ppcMem_getDataPtr(hCPU, address), sizeof(v));
        return std::bit_cast<double>(CPU_swapEndianU64(v));
    }

    inline static float ppcMem_readDataFloat(PPCInterpreter_t* hCPU, uint32 address)
    {
        uint32 v = *(uint32*)(ppcMem_getDataPtr(hCPU, address));
        v = CPU_swapEndianU32(v);
        return *(float*)&v;
    }

    inline static uint64 ppcMem_readDataU64(PPCInterpreter_t* hCPU, uint32 address)
    {
        uint64 v = *(uint64*)(ppcMem_getDataPtr(hCPU, address));
        return CPU_swapEndianU64(v);
    }

    inline static uint32 ppcMem_readDataU32(PPCInterpreter_t* hCPU, uint32 address)
    {
        uint32 pAddr = ppcMem_translateVirtualDataToPhysicalAddr(hCPU, address);
        if (hCPU->memoryException)
            return 0;
        if (pAddr >= 0x01FFF000 && pAddr < 0x02000000)
        {
            debug_printf("Access u32 boot param block 0x%08x IP %08x LR %08x\n", pAddr, hCPU->instructionPointer, hCPU->spr.LR);
            cemuLog_logDebug(LogType::Force, "Access u32 boot param block 0x{:08x} (org {:08x}) IP {:08x}", pAddr, address, hCPU->instructionPointer);
        }
        if (pAddr >= 0xFFEB73B0 && pAddr < (0xFFEB73B0+0x40C))
        {
            debug_printf("Access cached u32 boot param block 0x%08x IP %08x LR %08x\n", pAddr, hCPU->instructionPointer, hCPU->spr.LR);
            cemuLog_logDebug(LogType::Force, "Access cached u32 boot param block 0x{:08x} (org {:08x}) IP {:08x}", pAddr, address, hCPU->instructionPointer);
        }

        if (pAddr >= 0x0c000000 && pAddr < 0x0d100000)
        {
            cemu_assert_unimplemented();
            return 0;
        }
        uint32 v = *(uint32*)(memory_base + pAddr);
        return CPU_swapEndianU32(v);
    }

    inline static uint16 ppcMem_readDataU16(PPCInterpreter_t* hCPU, uint32 address)
    {
        uint16 v = *(uint16*)(ppcMem_getDataPtr(hCPU, address));
        return CPU_swapEndianU16(v);
    }

    inline static uint8 ppcMem_readDataU8(PPCInterpreter_t* hCPU, uint32 address)
    {
        uint32 pAddr = ppcMem_translateVirtualDataToPhysicalAddr(hCPU, address);
        if (pAddr >= 0x0c000000 && pAddr < 0x0d100000)
        {
            cemu_assert_unimplemented();
            return 0;
        }
        return *(uint8*)(memory_base + pAddr);
    }

    inline static uint64 ppcMem_readDataFloatEx(PPCInterpreter_t* hCPU, uint32 addr)
    {
        return ConvertToDoubleNoFTZ(_swapEndianU32(*(uint32*)(memory_base + addr)));
    }

    inline static void ppcMem_writeDataFloatEx(PPCInterpreter_t* hCPU, uint32 addr, uint64 value)
    {
        *(uint32*)(memory_base + addr) = _swapEndianU32(ConvertToSingleNoFTZ(value));
    }

    inline static uint64 getTB(PPCInterpreter_t* hCPU)
    {
        return hCPU->global->tb / 20ULL;
    }
};


#if !defined(PPC_INTERPRETER_DISABLE_BLOCK_CACHE) && (defined(__aarch64__) || defined(PPC_INTERPRETER_FORCE_BLOCK_CACHE))
#define PPC_INTERPRETER_BLOCK_CACHE 1
#else
#define PPC_INTERPRETER_BLOCK_CACHE 0
#endif

using PPCBlockHandler = void (*)(PPCInterpreter_t*, uint32);

enum class PPCBlockTerm : uint8
{
    None = 0,  // block hit the length cap, falls through to the next address
    BX,
    BCX,
    BCLRX,
    BCCTR,
    Generic,
};

// threaded dispatch
#if !defined(PPC_BLOCK_MUSTTAIL) && defined(__clang__) && defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::musttail)
#define PPC_BLOCK_MUSTTAIL 1
#endif
#endif
#ifndef PPC_BLOCK_MUSTTAIL
#define PPC_BLOCK_MUSTTAIL 0
#endif

#if defined(__clang__) && defined(__has_attribute)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer) || __has_feature(undefined_behavior_sanitizer)
#define PPC_BLOCK_SANITIZED_ABI 1
#else
#define PPC_BLOCK_SANITIZED_ABI 0
#endif
#if __has_attribute(preserve_none) && (defined(__aarch64__) || defined(__x86_64__)) && !PPC_BLOCK_SANITIZED_ABI && !defined(PPC_INTERPRETER_DISABLE_PRESERVE_NONE)
#define PPC_BLOCK_CC __attribute__((preserve_none))
#endif
#endif
#ifndef PPC_BLOCK_CC
#define PPC_BLOCK_CC
#endif

#if PPC_BLOCK_MUSTTAIL && !defined(PPC_INTERPRETER_DISABLE_REGISTER_CACHE)
#define PPC_BLOCK_CACHE_SP 1
#else
#define PPC_BLOCK_CACHE_SP 0
#endif

struct PPCBlockEntry;
struct PPCBlockCtx;
#if PPC_BLOCK_MUSTTAIL
using PPCBlockFn = void (PPC_BLOCK_CC *)(PPCInterpreter_t*, const PPCBlockEntry*, uint8*, PPCBlockCtx*, uint32);
#else
using PPCBlockFn = const PPCBlockEntry* (PPC_BLOCK_CC *)(PPCInterpreter_t*, const PPCBlockEntry*, uint8*, PPCBlockCtx*, uint32);
#endif

struct PPCBlockEntry
{
    PPCBlockFn fn;
    union
    {
        struct
        {
            uint32 imm;
            uint8 d, a, b, x;
        };
        struct { uint32 hash, epoch; };
    };
};
static_assert(sizeof(PPCBlockEntry) == 16);

// a classic handler plus its threaded block handler, which calls it directly so it can be inlined
struct PPCLegacyOp
{
    PPCBlockHandler legacy;
    PPCBlockFn direct;
    PPCBlockFn terminator;
};

using PPCBlockDesc = uint64;

// the table and the pool start small and double when they fill up, the cache is only thrown away once both are at their maximum
static constexpr uint32 PPCBLOCK_TABLE_BITS_MIN = 16;
static constexpr uint32 PPCBLOCK_TABLE_BITS_MAX = 20;
static constexpr uint32 PPCBLOCK_MAX_LENGTH = 64;
static constexpr uint32 PPCBLOCK_POOL_ENTRIES_MIN = 1u << 19;
static constexpr uint32 PPCBLOCK_POOL_ENTRIES_MAX = 1u << 22;

static constexpr uint32 PPCBlock_maxUsedSlots(uint32 tableBits) { return (1u << tableBits) * 3 / 4; }

static constexpr uint32 PPCBLOCK_PREDECODE_POOL_ENTRIES = 1u << 22;
static constexpr uint32 PPCBLOCK_PREDECODE_MAX_RANGES = 64;

static inline PPCBlockDesc PPCBlockDesc_pack(uint32 startAddr, uint32 firstEntry, uint32 count, PPCBlockTerm term)
{
    return (1ull << 63)
        | ((uint64)(startAddr >> 2) << 33)
        | ((uint64)firstEntry << 11)
        | ((uint64)(count - 1) << 5)
        | ((uint64)term << 2);
}

static inline uint32 PPCBlockDesc_keyForAddr(uint32 addr) { return 0x40000000u | (addr >> 2); }
static inline uint32 PPCBlockDesc_key(PPCBlockDesc d) { return (uint32)(d >> 33); }
static inline uint32 PPCBlockDesc_startAddr(PPCBlockDesc d) { return (PPCBlockDesc_key(d) & 0x3FFFFFFF) << 2; }
static inline uint32 PPCBlockDesc_firstEntry(PPCBlockDesc d) { return (uint32)(d >> 11) & 0x3FFFFF; }
static inline uint32 PPCBlockDesc_count(PPCBlockDesc d) { return ((uint32)(d >> 5) & 0x3F) + 1; }
static inline PPCBlockTerm PPCBlockDesc_term(PPCBlockDesc d) { return (PPCBlockTerm)((uint32)(d >> 2) & 7); }

struct PPCBlockSlot
{
    std::atomic<PPCBlockDesc> desc;
    std::atomic<uint32> nextSlot[2];
};
static constexpr uint32 PPCBLOCK_LINK_TAKEN = 0;
static constexpr uint32 PPCBLOCK_LINK_FALLTHROUGH = 1;

static inline void PPCBlockSlot_clearLinks(PPCBlockSlot& slot)
{
    slot.nextSlot[PPCBLOCK_LINK_TAKEN].store(0, std::memory_order_relaxed);
    slot.nextSlot[PPCBLOCK_LINK_FALLTHROUGH].store(0, std::memory_order_relaxed);
}

struct PPCBlockCtx
{
    PPCBlockSlot* table;
    PPCBlockEntry* pool;
    PPCBlockSlot* slot;
    const PPCBlockEntry* firstEntry;
    uint32 blockStart;
    uint32 blockCount;
    uint32 generation;
    uint32 (*targetSlots)[2];
    uint32 idleSpins;
    uint32 idleSkipAddr;
    bool genericExit; // the last block ended on the classic path, which released the cache
};

static constexpr uint8 PPCBLOCK_BRANCH_IDLE = 1;

static constexpr uint32 PPCBLOCK_IDLE_SPINS = 256;
static std::atomic<uint64> s_idleSkips{0};

static void PPCBlockCache_noteIdleSkip(uint32 addr)
{
    const uint64 skips = s_idleSkips.fetch_add(1, std::memory_order_relaxed) + 1;
    if (skips <= 16 || (skips & 0xFFFF) == 0)
        cemuLog_log(LogType::Force, "PPC idle loop at 0x{:08x}: timeslice given up ({} idle skips so far)", addr, skips);
}
static_assert(sizeof(PPCBlockSlot) == 16);

struct alignas(64) PPCBlockThreadState
{
    std::atomic<uint32> inUse;
    std::atomic<uint32> ack;
    PPCBlockSlot linkScratch;
    uint32 targetGeneration = 0;
    uint32 targetSlots[32][2]{};
};

static PPCBlockSlot* s_blockTable = nullptr;
static PPCBlockEntry* s_blockPool = nullptr;
static uint32 s_blockTableBits = PPCBLOCK_TABLE_BITS_MIN;
static uint32 s_blockTableSize = 1u << PPCBLOCK_TABLE_BITS_MIN;
static uint32 s_blockTableMaxUsed = PPCBlock_maxUsedSlots(PPCBLOCK_TABLE_BITS_MIN);
static uint32 s_blockPoolEntries = PPCBLOCK_POOL_ENTRIES_MIN;
static std::atomic<bool> s_blockCacheReady{false};
static std::atomic<uint32> s_blockTableUsed{0};
static std::atomic<uint32> s_blockPoolHead{0};
static std::atomic<uint64> s_blockDecodes{0};
static std::atomic<uint32> s_blockRedecodes{0};
static std::atomic<uint32> s_blockResets{0};

static std::atomic<uint32> s_blockCacheGeneration{2};

static constexpr uint32 PPCBLOCK_THREAD_TABLE_BITS = 6;
static constexpr uint32 PPCBLOCK_THREAD_TABLE_SIZE = 1u << PPCBLOCK_THREAD_TABLE_BITS;
static PPCBlockThreadState s_blockThreadStates[PPCBLOCK_THREAD_TABLE_SIZE];
static std::mutex s_blockCacheMutex;

struct PPCPredecodedRange
{
    uint32 start;
    uint32 end;
    const PPCBlockDesc* descs;
    uint32 count;
};
static std::atomic<PPCBlockEntry*> s_predecodePool{nullptr};
// Cold original opcodes are needed to hash suffixes of old predecoded blocks.
// Runtime execution entries carry no copy of this metadata.
static uint32* s_predecodeOpcodes = nullptr;
static uint32 s_predecodePoolHead = 0; // guarded by s_predecodeMutex
static PPCPredecodedRange s_predecodedRanges[PPCBLOCK_PREDECODE_MAX_RANGES];
static std::atomic<uint32> s_predecodedRangeCount{0};
static std::mutex s_predecodeMutex;
static std::atomic<PPCBlockDesc> s_predecodedTrampolines[MEMORY_CODE_TRAMPOLINE_AREA_SIZE / 4]{};
static std::atomic<uint32> s_blockCacheCodePages[(1u << 20) / 32]; // 4 GiB / 4 KiB pages, 1 bit each

static std::atomic<uint32> s_codeEpoch{1};
static std::atomic<uint32> s_codeEpochFloor{0};
static std::atomic<uint32> s_codePageEpoch[1u << 20];
static std::mutex s_codeEpochMutex;

static inline PPCBlockEntry* PPCBlockDesc_header(PPCBlockEntry* pool, PPCBlockDesc d) { return pool + PPCBlockDesc_firstEntry(d) - 1; }

static const PPCPredecodedRange* PPCPredecode_findRange(uint32 addr)
{
    const uint32 rangeCount = s_predecodedRangeCount.load(std::memory_order_acquire);
    for (uint32 r = rangeCount; r-- > 0;)
    {
        const PPCPredecodedRange& range = s_predecodedRanges[r];
        if (addr >= range.start && addr < range.end)
            return &range;
    }
    return nullptr;
}

static PPCBlockDesc PPCPredecode_lookup(uint32 addr)
{
    if (addr & 3)
        return 0;
    
    auto covers = [addr](PPCBlockDesc d) { return addr - PPCBlockDesc_startAddr(d) < PPCBlockDesc_count(d) * 4; };
    
    const uint32 trampolineOffset = addr - MEMORY_CODE_TRAMPOLINE_AREA_ADDR;
    
    if (trampolineOffset < MEMORY_CODE_TRAMPOLINE_AREA_SIZE)
    {
        const uint32 word = trampolineOffset / 4;
        
        for (uint32 back = 0; back < PPCBLOCK_MAX_LENGTH && back <= word; back++)
        {
            const PPCBlockDesc d = s_predecodedTrampolines[word - back].load(std::memory_order_acquire);
            
            if (d)
                return covers(d) ? d : 0;
        }
        
        return 0;
    }
    
    const PPCPredecodedRange* range = PPCPredecode_findRange(addr);
    if (!range)
        return 0;
    
    const uint32 key = PPCBlockDesc_keyForAddr(addr);
    
    const PPCBlockDesc* it = std::upper_bound(range->descs, range->descs + range->count, key,
        [](uint32 k, PPCBlockDesc d) { return k < PPCBlockDesc_key(d); });
    
    for (uint32 n = 0; n < 4 && it != range->descs; n++)
    {
        --it;
        if (covers(*it))
            return *it;
    }
    
    return 0;
}
static inline stdx::atomic_ref<uint32> PPCBlockHeader_verifiedEpoch(PPCBlockEntry* header) { return stdx::atomic_ref<uint32>(header->epoch); }

static inline uint32 PPCBlockCache_hashStep(uint32 h, uint32 word)
{
    return std::rotl(h ^ (word * 0x9E3779B1u), 13) * 5u + 0xE6546B64u;
}

static inline void PPCBlockCache_markCodePage(uint32 addr)
{
    const uint32 page = addr >> 12;
    const uint32 bit = 1u << (page & 31);
    std::atomic<uint32>& word = s_blockCacheCodePages[page >> 5];

    if ((word.load(std::memory_order_relaxed) & bit) == 0)
        word.fetch_or(bit, std::memory_order_seq_cst);
}

static inline bool PPCBlockCache_isCodePage(uint32 addr)
{
    const uint32 page = addr >> 12;

    return (s_blockCacheCodePages[page >> 5].load(std::memory_order_relaxed) >> (page & 31)) & 1;
}

static void PPCBlockCache_ensureAllocated()
{
    if (s_blockCacheReady.load(std::memory_order_acquire))
        return;

    std::lock_guard<std::mutex> lock(s_blockCacheMutex);
    if (s_blockTable)
        return;

    s_blockTable = new PPCBlockSlot[s_blockTableSize]{};
    s_blockPool = new PPCBlockEntry[s_blockPoolEntries];
    s_blockCacheReady.store(true, std::memory_order_release);
}

static thread_local PPCBlockThreadState* t_blockThreadState = nullptr;
static thread_local bool t_blockThreadStateResolved = false;

// gives the thread's slot back when the thread exits
struct PPCBlockThreadRelease
{
    ~PPCBlockThreadRelease()
    {
        if (PPCBlockThreadState* ts = t_blockThreadState)
        {
            t_blockThreadState = nullptr;
            ts->ack.store(0, std::memory_order_release);
            ts->inUse.store(0, std::memory_order_release);
        }
    }
};

static thread_local PPCBlockThreadRelease t_blockThreadRelease;

TLS_WORKAROUND_NOINLINE static PPCBlockThreadState* PPCBlockCache_getThreadState()
{
    if (!t_blockThreadStateResolved)
    {
        t_blockThreadStateResolved = true;
        PPCBlockCache_ensureAllocated();
        (void)&t_blockThreadRelease;

        const uint64 idHash = std::hash<std::thread::id>{}(std::this_thread::get_id());
        const uint32 home = (uint32)((idHash * 0x9E3779B97F4A7C15ull) >> (64 - PPCBLOCK_THREAD_TABLE_BITS));
        for (uint32 probe = 0; probe < PPCBLOCK_THREAD_TABLE_SIZE; probe++)
        {
            PPCBlockThreadState& ts = s_blockThreadStates[(home + probe) & (PPCBLOCK_THREAD_TABLE_SIZE - 1)];
            uint32 expected = 0;
            if (ts.inUse.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
            {
                PPCBlockSlot_clearLinks(ts.linkScratch);
                t_blockThreadState = &ts;
                break;
            }
        }
        if (!t_blockThreadState)
            cemuLog_log(LogType::Force, "PPC block cache: more than {} live interpreter threads, this one falls back to uncached dispatch", PPCBLOCK_THREAD_TABLE_SIZE);
    }
    return t_blockThreadState;
}

static uint32 PPCBlockCache_synchronize(PPCBlockThreadState& ts)
{
    uint32 gen;
    for (uint64 spin = 0; ; spin++)
    {
        gen = s_blockCacheGeneration.load(std::memory_order_acquire);
        ts.ack.store(gen, std::memory_order_release);
        if ((gen & 1) == 0)
            break;
        if (spin > 64)
            std::this_thread::yield();
    }
    return gen;
}


static inline uint32 PPCBlockCache_hashSlot(uint32 addr);

static inline bool PPCBlockCache_tableFull()
{
    return s_blockTableUsed.load(std::memory_order_relaxed) >= s_blockTableMaxUsed;
}

static inline bool PPCBlockCache_poolFull()
{
    return s_blockPoolHead.load(std::memory_order_relaxed) + PPCBLOCK_MAX_LENGTH + 2 > s_blockPoolEntries;
}

static uint32 PPCBlockCache_parkThreadsLocked()
{
    const uint32 gen = s_blockCacheGeneration.load(std::memory_order_relaxed);
    const uint32 parked = gen | 1;
    s_blockCacheGeneration.store(parked, std::memory_order_release);

    for (uint32 i = 0; i < PPCBLOCK_THREAD_TABLE_SIZE; i++)
    {
        const std::atomic<uint32>& ack = s_blockThreadStates[i].ack;
        for (uint64 spin = 0; ; spin++)
        {
            const uint32 a = ack.load(std::memory_order_acquire);
            if (a == 0 || a == parked)
                break;
            if (spin > 64)
                std::this_thread::yield();
            if (spin == 1000000)
                cemuLog_log(LogType::Force, "PPC block cache: still waiting for thread slot {} to reach a block boundary", i);
        }
    }
    return parked;
}

static void PPCBlockCache_clearLocked(const char* reason)
{
    for (uint32 i = 0; i < s_blockTableSize; i++)
    {
        s_blockTable[i].desc.store(0, std::memory_order_relaxed);
        PPCBlockSlot_clearLinks(s_blockTable[i]);
    }
    s_blockTableUsed.store(0, std::memory_order_relaxed);
    s_blockPoolHead.store(0, std::memory_order_relaxed);

    const uint32 resets = s_blockResets.fetch_add(1, std::memory_order_relaxed) + 1;
    if (resets <= 16 || (resets & 0xFF) == 0)
        cemuLog_log(LogType::Force, "PPC block cache: reset #{} ({}), {} blocks decoded since start, {} redecoded after code changes",
            resets, reason, s_blockDecodes.load(std::memory_order_relaxed), s_blockRedecodes.load(std::memory_order_relaxed));
}

// threads are parked, so nothing holds a pointer into the old table or pool
static void PPCBlockCache_growLocked(bool growTable, bool growPool)
{
    if (growPool)
    {
        const uint32 used = std::min(s_blockPoolHead.load(std::memory_order_relaxed), s_blockPoolEntries);
        PPCBlockEntry* newPool = new PPCBlockEntry[s_blockPoolEntries * 2];
        memcpy(newPool, s_blockPool, used * sizeof(PPCBlockEntry));
        delete[] s_blockPool;
        s_blockPool = newPool;
        s_blockPoolEntries *= 2;
        s_blockPoolHead.store(used, std::memory_order_relaxed);
    }
    
    if (growTable)
    {
        PPCBlockSlot* oldTable = s_blockTable;
        const uint32 oldSize = s_blockTableSize;
        s_blockTableBits++;
        s_blockTableSize = 1u << s_blockTableBits;
        s_blockTableMaxUsed = PPCBlock_maxUsedSlots(s_blockTableBits);
        s_blockTable = new PPCBlockSlot[s_blockTableSize]{};
        
        uint32 used = 0;
        for (uint32 i = 0; i < oldSize; i++)
        {
            const PPCBlockDesc desc = oldTable[i].desc.load(std::memory_order_relaxed);
            if (desc == 0)
                continue;
            uint32 slot = PPCBlockCache_hashSlot(PPCBlockDesc_startAddr(desc));
            while (s_blockTable[slot].desc.load(std::memory_order_relaxed) != 0)
                slot = (slot + 1) & (s_blockTableSize - 1);
            s_blockTable[slot].desc.store(desc, std::memory_order_relaxed);
            used++;
        }
        s_blockTableUsed.store(used, std::memory_order_relaxed);
        delete[] oldTable;
        
        
        for (auto& ts : s_blockThreadStates)
            PPCBlockSlot_clearLinks(ts.linkScratch);
    }
    
    cemuLog_log(LogType::Force, "PPC block cache: grown to {} slots and {} pool entries", s_blockTableSize, s_blockPoolEntries);
}

static void PPCBlockCache_makeRoomLocked(const char* reason)
{
    const bool tableFull = PPCBlockCache_tableFull();
    const bool poolFull = PPCBlockCache_poolFull();
    if (!tableFull && !poolFull)
        return;

    const bool growTable = tableFull && s_blockTableBits < PPCBLOCK_TABLE_BITS_MAX;
    const bool growPool = poolFull && s_blockPoolEntries < PPCBLOCK_POOL_ENTRIES_MAX;
    const uint32 parked = PPCBlockCache_parkThreadsLocked();
    
    if ((tableFull && !growTable) || (poolFull && !growPool))
        PPCBlockCache_clearLocked(reason);
    else
        PPCBlockCache_growLocked(growTable, growPool);
    
    s_blockCacheGeneration.store(parked + 1, std::memory_order_release);
}

static void PPCBlockCache_onFull(const char* reason, uint32 expectedGeneration)
{
    if (PPCBlockThreadState* self = t_blockThreadState)
        self->ack.store(0, std::memory_order_release);

    std::lock_guard<std::mutex> lock(s_blockCacheMutex);

    if (!s_blockTable)
        return;
    if (s_blockCacheGeneration.load(std::memory_order_relaxed) != expectedGeneration)
        return;

    PPCBlockCache_makeRoomLocked(reason);
}

static inline uint32 PPCBlockCache_hashSlot(uint32 addr)
{
    return ((addr >> 2) * 0x9E3779B1u) >> (32 - s_blockTableBits);
}

static PPCBlockSlot* PPCBlockCache_lookup(uint32 addr)
{
    const uint32 key = PPCBlockDesc_keyForAddr(addr);
    uint32 i = PPCBlockCache_hashSlot(addr);
    for (uint32 probe = 0; probe < s_blockTableSize; probe++)
    {
        PPCBlockSlot& slot = s_blockTable[i];
        const PPCBlockDesc desc = slot.desc.load(std::memory_order_acquire);
        if (desc == 0)
            return nullptr;
        if (PPCBlockDesc_key(desc) == key)
            return &slot;
        i = (i + 1) & (s_blockTableSize - 1);
    }
    return nullptr;
}

static FORCE_INLINE PPCBlockSlot* PPCBlockCache_targetLookup(PPCBlockSlot* table, uint32 (*targets)[2], uint32 ip)
{
    const uint32 key = PPCBlockDesc_keyForAddr(ip);
    const uint32 set = ((ip >> 2) ^ (ip >> 9)) & 31;
    for (uint32 way = 0; way < 2; way++)
    {
        PPCBlockSlot* slot = table + targets[set][way];
        if (PPCBlockDesc_key(slot->desc.load(std::memory_order_acquire)) == key)
            return slot;
    }
    return nullptr;
}

static FORCE_INLINE void PPCBlockCache_targetRemember(PPCBlockSlot* table, uint32 (*targets)[2], uint32 ip, PPCBlockSlot* slot)
{
    const uint32 set = ((ip >> 2) ^ (ip >> 9)) & 31;
    const uint32 index = uint32(slot - table);
    if (targets[set][0] != index)
    {
        targets[set][1] = targets[set][0];
        targets[set][0] = index;
    }
}

static PPCBlockSlot* PPCBlockCache_publish(uint32 addr, PPCBlockDesc desc)
{
    const uint32 key = PPCBlockDesc_key(desc);
    uint32 i = PPCBlockCache_hashSlot(addr);
    for (uint32 probe = 0; probe < s_blockTableSize; probe++)
    {
        PPCBlockSlot& slot = s_blockTable[i];
        PPCBlockDesc cur = slot.desc.load(std::memory_order_acquire);
        if (cur == 0)
        {
            if (slot.desc.compare_exchange_strong(cur, desc, std::memory_order_release, std::memory_order_acquire))
            {
                s_blockTableUsed.fetch_add(1, std::memory_order_relaxed);
                return &slot;
            }
        }
        if (PPCBlockDesc_key(cur) == key)
            return &slot;
        i = (i + 1) & (s_blockTableSize - 1);
    }
    return nullptr;
}

void PPCInterpreter_invalidateBlockCache()
{
    std::lock_guard<std::mutex> lock(s_codeEpochMutex);
    const uint32 epoch = s_codeEpoch.load(std::memory_order_relaxed) + 1;
    s_codeEpochFloor.store(epoch, std::memory_order_relaxed);
    s_codeEpoch.store(epoch, std::memory_order_release);
}

void PPCInterpreter_invalidateBlockCacheRange(uint32 addr, uint32 size)
{
    std::atomic_thread_fence(std::memory_order_seq_cst);

    std::unique_lock<std::mutex> lock(s_codeEpochMutex, std::defer_lock);
    uint32 epoch = 0;
    const uint32 firstPage = addr >> 12;
    const uint32 lastPage = (addr + (size ? size - 1 : 0)) >> 12;
    for (uint32 page = firstPage; page <= lastPage; page++)
    {
        if (PPCBlockCache_isCodePage(page << 12))
        {
            if (!lock.owns_lock())
            {
                lock.lock();
                epoch = s_codeEpoch.load(std::memory_order_relaxed) + 1;
            }
            s_codePageEpoch[page].store(epoch, std::memory_order_relaxed);
        }
        if (page == 0xFFFFF)
            break;
    }
    if (lock.owns_lock())
    {
        s_codeEpoch.store(epoch, std::memory_order_release);
        
        if ((epoch & 0xFFF) == 0)
            cemuLog_log(LogType::Force, "PPC block cache: code epoch {}, last bump from a write to 0x{:08x} (size 0x{:x})", epoch, addr, size);
    }
}

#if (defined(__clang__) || defined(__GNUC__)) && !defined(PPC_INTERPRETER_DISABLE_THREADED_DISPATCH) && !defined(__DEBUG_OUTPUT_INSTRUCTION)
#define PPC_PRIMARY_CASE(value) case value: ppc_op_##value:
#define PPC_PRIMARY_DEFAULT default: ppc_unknown:
#define PPC_DISPATCH_NEXT() do { \
    if constexpr (!runTimeslice) return; \
    if (--hCPU->remainingCycles < 0) return; \
    if constexpr (ppcItpCtrl::allowSupervisorMode) ++hCPU->global->tb; \
    opcode = ppcItpCtrl::memory_readCodeU32(hCPU, hCPU->instructionPointer); \
    goto *dispatch[opcode >> 26]; \
} while (false)
#else
#define PPC_PRIMARY_CASE(value) case value:
#define PPC_PRIMARY_DEFAULT default:
#define PPC_DISPATCH_NEXT() break
#endif

template <typename ppcItpCtrl>
class PPCInterpreterContainer
{
public:
#include "PPCInterpreterSPR.hpp"
#include "PPCInterpreterOPC.hpp"
#include "PPCInterpreterLoadStore.hpp"
#include "PPCInterpreterALU.hpp"
    
    static constexpr std::array<PPCLegacyOp, 1024> makeHandlerTable19()
    {
        std::array<PPCLegacyOp, 1024> result{};
        result[0] = legacyOp<PPCInterpreter_MCRF>();
        result[16] = legacyOp<PPCInterpreter_BCLRX>();
        result[33] = legacyOp<PPCInterpreter_CRNOR>();
        result[50] = legacyOp<PPCInterpreter_RFI>();
        result[129] = legacyOp<PPCInterpreter_CRANDC>();
        result[150] = legacyOp<PPCInterpreter_ISYNC>();
        result[193] = legacyOp<PPCInterpreter_CRXOR>();
        result[225] = legacyOp<PPCInterpreter_CRNAND>();
        result[257] = legacyOp<PPCInterpreter_CRAND>();
        result[289] = legacyOp<PPCInterpreter_CREQV>();
        result[417] = legacyOp<PPCInterpreter_CRORC>();
        result[449] = legacyOp<PPCInterpreter_CROR>();
        result[528] = legacyOp<PPCInterpreter_BCCTR>();
        return result;
    }
    
    
    static constexpr std::array<PPCLegacyOp, 1024> makeHandlerTable31()
    {
        std::array<PPCLegacyOp, 1024> result{};
        result[0] = legacyOp<PPCInterpreter_CMP>();
        result[4] = legacyOp<PPCInterpreter_TW>();
        result[8] = legacyOp<PPCInterpreter_SUBFC>();
        result[10] = legacyOp<PPCInterpreter_ADDC>();
        result[11] = legacyOp<PPCInterpreter_MULHWU_>();
        result[19] = legacyOp<PPCInterpreter_MFCR>();
        result[20] = legacyOp<PPCInterpreter_LWARX>();
        result[23] = legacyOp<PPCInterpreter_LWZX>();
        result[24] = legacyOp<PPCInterpreter_SLWX>();
        result[26] = legacyOp<PPCInterpreter_CNTLZW>();
        result[28] = legacyOp<PPCInterpreter_ANDX>();
        result[32] = legacyOp<PPCInterpreter_CMPL>();
        result[40] = legacyOp<PPCInterpreter_SUBF>();
        result[54] = legacyOp<PPCInterpreter_DCBST>();
        result[55] = legacyOp<PPCInterpreter_LWZXU>();
        result[60] = legacyOp<PPCInterpreter_ANDCX>();
        result[75] = legacyOp<PPCInterpreter_MULHW_>();
        result[83] = legacyOp<PPCInterpreter_MFMSR>();
        result[86] = legacyOp<PPCInterpreter_DCBF>();
        result[87] = legacyOp<PPCInterpreter_LBZX>();
        result[104] = legacyOp<PPCInterpreter_NEG>();
        result[119] = legacyOp<PPCInterpreter_LBZXU>(); // Sonic Lost World
        result[124] = legacyOp<PPCInterpreter_NORX>();
        result[136] = legacyOp<PPCInterpreter_SUBFE>();
        result[138] = legacyOp<PPCInterpreter_ADDE>();
        result[144] = legacyOp<PPCInterpreter_MTCRF>();
        result[146] = legacyOp<PPCInterpreter_MTMSR>();
        result[150] = legacyOp<PPCInterpreter_STWCX>();
        result[151] = legacyOp<PPCInterpreter_STWX>();
        result[183] = legacyOp<PPCInterpreter_STWUX>();
        result[200] = legacyOp<PPCInterpreter_SUBFZE>();
        result[202] = legacyOp<PPCInterpreter_ADDZE>();
        result[210] = legacyOp<PPCInterpreter_MTSR>();
        result[215] = legacyOp<PPCInterpreter_STBX>();
        result[232] = legacyOp<PPCInterpreter_SUBFME>(); // Trine 2
        result[234] = legacyOp<PPCInterpreter_ADDME>();
        result[235] = legacyOp<PPCInterpreter_MULLW>();
        result[247] = legacyOp<PPCInterpreter_STBUX>();
        result[266] = legacyOp<PPCInterpreter_ADD>();
        result[278] = legacyOp<PPCInterpreter_DCBT>();
        result[279] = legacyOp<PPCInterpreter_LHZX>();
        result[284] = legacyOp<PPCInterpreter_EQV>();
        result[306] = legacyOp<PPCInterpreter_TLBIE>();
        result[311] = legacyOp<PPCInterpreter_LHZUX>(); // Wii U Menu v177 (US)
        result[316] = legacyOp<PPCInterpreter_XOR>();
        result[339] = legacyOp<PPCInterpreter_MFSPR>();
        result[343] = legacyOp<PPCInterpreter_LHAX>();
        result[371] = legacyOp<PPCInterpreter_MFTB>();
        result[375] = legacyOp<PPCInterpreter_LHAUX>(); // Wii U Menu v177 (US)
        result[407] = legacyOp<PPCInterpreter_STHX>();
        result[412] = legacyOp<PPCInterpreter_ORC>();
        result[439] = legacyOp<PPCInterpreter_STHUX>();
        result[444] = legacyOp<PPCInterpreter_OR>();
        result[459] = legacyOp<PPCInterpreter_DIVWU>();
        result[467] = legacyOp<PPCInterpreter_MTSPR>();
        result[470] = legacyOp<PPCInterpreter_DCBI>();
        result[476] = legacyOp<PPCInterpreter_NANDX>();
        result[491] = legacyOp<PPCInterpreter_DIVW>();
        result[512] = legacyOp<PPCInterpreter_MCRXR>();
        result[520] = legacyOp<PPCInterpreter_SUBFCO>(); // Affordable Space Adventures + other Unity games
        result[522] = legacyOp<PPCInterpreter_ADDCO>();
        result[523] = legacyOp<PPCInterpreter_MULHWU_>(); // 11 | OE; OE is ignored
        result[533] = legacyOp<PPCInterpreter_LSWX>();
        result[534] = legacyOp<PPCInterpreter_LWBRX>();
        result[535] = legacyOp<PPCInterpreter_LFSX>();
        result[536] = legacyOp<PPCInterpreter_SRWX>();
        result[552] = legacyOp<PPCInterpreter_SUBFO>();
        result[566] = legacyOp<PPCInterpreter_TLBSYNC>();
        result[567] = legacyOp<PPCInterpreter_LFSUX>();
        result[587] = legacyOp<PPCInterpreter_MULHW_>(); // 75 | OE; OE is ignored for MULHW
        result[595] = legacyOp<PPCInterpreter_MFSR>();
        result[597] = legacyOp<PPCInterpreter_LSWI>();
        result[598] = legacyOp<PPCInterpreter_SYNC>();
        result[599] = legacyOp<PPCInterpreter_LFDX>();
        result[616] = legacyOp<PPCInterpreter_NEGO>();
        result[631] = legacyOp<PPCInterpreter_LFDUX>();
        result[648] = legacyOp<PPCInterpreter_SUBFEO>(); // 136 | OE
        result[650] = legacyOp<PPCInterpreter_ADDEO>(); // 138 | OE
        result[662] = legacyOp<PPCInterpreter_STWBRX>();
        result[663] = legacyOp<PPCInterpreter_STFSX>();
        result[661] = legacyOp<PPCInterpreter_STSWX>();
        result[695] = legacyOp<PPCInterpreter_STFSUX>();
        result[712] = legacyOp<PPCInterpreter_SUBFZEO>(); // 200 | OE
        result[714] = legacyOp<PPCInterpreter_ADDZEO>(); // 202 | OE
        result[725] = legacyOp<PPCInterpreter_STSWI>();
        result[727] = legacyOp<PPCInterpreter_STFDX>();
        result[744] = legacyOp<PPCInterpreter_SUBFMEO>(); // 232 | OE
        result[746] = legacyOp<PPCInterpreter_ADDMEO>(); // 234 | OE
        result[747] = legacyOp<PPCInterpreter_MULLWO>();
        result[759] = legacyOp<PPCInterpreter_STFDUX>();
        result[778] = legacyOp<PPCInterpreter_ADDO>();
        result[790] = legacyOp<PPCInterpreter_LHBRX>();
        result[792] = legacyOp<PPCInterpreter_SRAW>();
        result[824] = legacyOp<PPCInterpreter_SRAWI>();
        result[854] = legacyOp<PPCInterpreter_EIEIO>();
        result[918] = legacyOp<PPCInterpreter_STHBRX>();
        result[922] = legacyOp<PPCInterpreter_EXTSH>();
        result[954] = legacyOp<PPCInterpreter_EXTSB>();
        result[971] = legacyOp<PPCInterpreter_DIVWUO>();
        result[982] = legacyOp<PPCInterpreter_ICBI>();
        result[983] = legacyOp<PPCInterpreter_STFIWX>();
        result[1003] = legacyOp<PPCInterpreter_DIVWO>();
        result[1014] = legacyOp<PPCInterpreter_DCBZ>();
        return result;
    }

    template<bool runTimeslice = false>
    static void executeInstruction(PPCInterpreter_t* hCPU)
    {
#if (defined(__clang__) || defined(__GNUC__)) && !defined(PPC_INTERPRETER_DISABLE_THREADED_DISPATCH) && !defined(__DEBUG_OUTPUT_INSTRUCTION)
        static void* const dispatch[64] = {
            &&ppc_op_0, &&ppc_op_1, &&ppc_unknown, &&ppc_op_3, &&ppc_op_4, &&ppc_unknown, &&ppc_unknown, &&ppc_op_7,
            &&ppc_op_8, &&ppc_unknown, &&ppc_op_10, &&ppc_op_11, &&ppc_op_12, &&ppc_op_13, &&ppc_op_14, &&ppc_op_15,
            &&ppc_op_16, &&ppc_op_17, &&ppc_op_18, &&ppc_op_19, &&ppc_op_20, &&ppc_op_21, &&ppc_unknown, &&ppc_op_23,
            &&ppc_op_24, &&ppc_op_25, &&ppc_op_26, &&ppc_op_27, &&ppc_op_28, &&ppc_op_29, &&ppc_unknown, &&ppc_op_31,
            &&ppc_op_32, &&ppc_op_33, &&ppc_op_34, &&ppc_op_35, &&ppc_op_36, &&ppc_op_37, &&ppc_op_38, &&ppc_op_39,
            &&ppc_op_40, &&ppc_op_41, &&ppc_op_42, &&ppc_op_43, &&ppc_op_44, &&ppc_op_45, &&ppc_op_46, &&ppc_op_47,
            &&ppc_op_48, &&ppc_op_49, &&ppc_op_50, &&ppc_op_51, &&ppc_op_52, &&ppc_op_53, &&ppc_op_54, &&ppc_op_55,
            &&ppc_op_56, &&ppc_op_57, &&ppc_unknown, &&ppc_op_59, &&ppc_op_60, &&ppc_op_61, &&ppc_unknown, &&ppc_op_63
        };
#endif
        if constexpr (runTimeslice)
        {
            if (--hCPU->remainingCycles < 0)
                return;
        }

    ppc_fetch:
        if constexpr(ppcItpCtrl::allowSupervisorMode)
        {
            hCPU->global->tb++;
        }

#ifdef __DEBUG_OUTPUT_INSTRUCTION
        debug_printf("%08x: ", hCPU->instructionPointer);
#endif

        uint32 opcode = ppcItpCtrl::memory_readCodeU32(hCPU, hCPU->instructionPointer);

#if (defined(__clang__) || defined(__GNUC__)) && !defined(PPC_INTERPRETER_DISABLE_THREADED_DISPATCH) && !defined(__DEBUG_OUTPUT_INSTRUCTION)
        goto *dispatch[opcode >> 26];
#endif
        switch ((opcode >> 26))
        {
        PPC_PRIMARY_CASE(0)
            debug_printf("ZERO[NOP] | 0x%08X\n", (unsigned int)hCPU->instructionPointer);
    #ifdef CEMU_DEBUG_ASSERT
            assert_dbg();
            while (true) std::this_thread::sleep_for(std::chrono::seconds(1));
    #endif
            hCPU->instructionPointer += 4;
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(1) // virtual HLE
            PPCInterpreter_virtualHLE(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(3)
            cemuLog_logDebug(LogType::Force, "Unsupported TWI instruction executed at {:08x}", hCPU->instructionPointer);
            PPCInterpreter_nextInstruction(hCPU);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(4)
            switch (PPC_getBits(opcode, 30, 5))
            {
            case 0: // subcategory compare
                switch (PPC_getBits(opcode, 25, 5))
                {
                case 0: // Sonic All Stars Racing
                    PPCInterpreter_PS_CMPU0(hCPU, opcode);
                    break;
                case 1:
                    PPCInterpreter_PS_CMPO0(hCPU, opcode);
                    break;
                case 2: // Assassin's Creed 3, Sonic All Stars Racing
                    PPCInterpreter_PS_CMPU1(hCPU, opcode);
                    break;
                default:
                    cemuLog_logDebug(LogType::Force, "Unknown execute {:04x} as [4->0] at {:08x}", PPC_getBits(opcode, 25, 5), hCPU->instructionPointer);
                    cemu_assert_unimplemented();
                    hCPU->instructionPointer += 4;
                    break;
                }
                break;
            case 6:
                PPCInterpreter_PSQ_LX(hCPU, opcode);
                break;
            case 7:
                PPCInterpreter_PSQ_STX(hCPU, opcode);
                break;
            case 8:
                switch (PPC_getBits(opcode, 25, 5))
                {
                case 1:
                    PPCInterpreter_PS_NEG(hCPU, opcode);
                    break;
                case 2:
                    PPCInterpreter_PS_MR(hCPU, opcode);
                    break;
                case 4:
                    PPCInterpreter_PS_NABS(hCPU, opcode);
                    break;
                case 8:
                    PPCInterpreter_PS_ABS(hCPU, opcode);
                    break;
                default:
                    cemuLog_logDebug(LogType::Force, "Unknown execute {:04x} as [4->8] at {:08x}", PPC_getBits(opcode, 25, 5), hCPU->instructionPointer);
                    cemu_assert_unimplemented();
                    hCPU->instructionPointer += 4;
                    break;
                }
                break;
            case 10:
                PPCInterpreter_PS_SUM0(hCPU, opcode);
                break;
            case 11:
                PPCInterpreter_PS_SUM1(hCPU, opcode);
                break;
            case 12:
                PPCInterpreter_PS_MULS0(hCPU, opcode);
                break;
            case 13:
                PPCInterpreter_PS_MULS1(hCPU, opcode);
                break;
            case 14:
                PPCInterpreter_PS_MADDS0(hCPU, opcode);
                break;
            case 15:
                PPCInterpreter_PS_MADDS1(hCPU, opcode);
                break;
            case 16: // sub category - merge
                switch (PPC_getBits(opcode, 25, 5))
                {
                case 16:
                    PPCInterpreter_PS_MERGE00(hCPU, opcode);
                    break;
                case 17:
                    PPCInterpreter_PS_MERGE01(hCPU, opcode);
                    break;
                case 18:
                    PPCInterpreter_PS_MERGE10(hCPU, opcode);
                    break;
                case 19:
                    PPCInterpreter_PS_MERGE11(hCPU, opcode);
                    break;
                default:
                    cemuLog_logDebug(LogType::Force, "Unknown execute {:04x} as [4->16] at {:08x}", PPC_getBits(opcode, 25, 5), hCPU->instructionPointer);
                    cemu_assert_unimplemented();
                    hCPU->instructionPointer += 4;
                    break;
                }
                break;
            case 18:
                PPCInterpreter_PS_DIV(hCPU, opcode);
                break;
            case 20:
                PPCInterpreter_PS_SUB(hCPU, opcode);
                break;
            case 21:
                PPCInterpreter_PS_ADD(hCPU, opcode);
                break;
            case 22:
                PPCInterpreter_DCBZL(hCPU, opcode);
                break;
            case 23:
                PPCInterpreter_PS_SEL(hCPU, opcode);
                break;
            case 24:
                PPCInterpreter_PS_RES(hCPU, opcode);
                break;
            case 25:
                PPCInterpreter_PS_MUL(hCPU, opcode);
                break;
            case 26:
                PPCInterpreter_PS_RSQRTE(hCPU, opcode);
                break;
            case 28:
                PPCInterpreter_PS_MSUB(hCPU, opcode);
                break;
            case 29:
                PPCInterpreter_PS_MADD(hCPU, opcode);
                break;
            case 30:
                PPCInterpreter_PS_NMSUB(hCPU, opcode);
                break;
            case 31:
                PPCInterpreter_PS_NMADD(hCPU, opcode);
                break;
            default:
                cemuLog_logDebug(LogType::Force, "Unknown execute {:04x} as [4] at {:08x}", PPC_getBits(opcode, 30, 5), hCPU->instructionPointer);
                cemu_assert_unimplemented();
                hCPU->instructionPointer += 4;
                break;
            }
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(7)
            PPCInterpreter_MULLI(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(8)
            PPCInterpreter_SUBFIC(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(10)
            PPCInterpreter_CMPLI(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(11)
            PPCInterpreter_CMPI(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(12)
            PPCInterpreter_ADDIC(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(13)
            PPCInterpreter_ADDIC_(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(14)
            PPCInterpreter_ADDI(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(15)
            PPCInterpreter_ADDIS(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(16)
            PPCInterpreter_BCX(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(17)
            if (PPC_getBits(opcode, 30, 1) == 1)
            {
                PPCInterpreter_SC(hCPU, opcode);
            }
            else
            {
                cemuLog_logDebug(LogType::Force, "Unsupported Opcode [0x17 > 0x0]");
                cemu_assert_unimplemented();
                hCPU->instructionPointer += 4;
            }
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(18)
            PPCInterpreter_BX(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(19) // opcode category
            {
                static constexpr auto handlers = makeHandlerTable19();
                if (auto handler = handlers[PPC_getBits(opcode, 30, 10)].legacy)
                    handler(hCPU, opcode);
                else
                {
                    cemuLog_logDebug(LogType::Force, "Unknown execute {:04x} as [19] at {:08x}\n", PPC_getBits(opcode, 30, 10), hCPU->instructionPointer);
                    cemu_assert_unimplemented();
                    hCPU->instructionPointer += 4;
                }
            }
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(20)
            PPCInterpreter_RLWIMI(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(21)
            PPCInterpreter_RLWINM(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(23)
            PPCInterpreter_RLWNM(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(24)
            PPCInterpreter_ORI(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(25)
            PPCInterpreter_ORIS(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(26)
            PPCInterpreter_XORI(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(27)
            PPCInterpreter_XORIS(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(28)
            PPCInterpreter_ANDI_(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(29)
            PPCInterpreter_ANDIS_(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(31) // opcode category
            {
                static constexpr auto handlers = makeHandlerTable31();
                if (auto handler = handlers[PPC_getBits(opcode, 30, 10)].legacy)
                    handler(hCPU, opcode);
                else
                {
                    cemuLog_logDebug(LogType::Force, "Unknown execute {:04x} as [31] at {:08x}\n", PPC_getBits(opcode, 30, 10), hCPU->instructionPointer);
                    cemu_assert_unimplemented();
                    hCPU->instructionPointer += 4;
                }
            }
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(32)
            PPCInterpreter_LWZ(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(33)
            PPCInterpreter_LWZU(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(34)
            PPCInterpreter_LBZ(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(35)
            PPCInterpreter_LBZU(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(36)
            PPCInterpreter_STW(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(37)
            PPCInterpreter_STWU(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(38)
            PPCInterpreter_STB(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(39)
            PPCInterpreter_STBU(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(40)
            PPCInterpreter_LHZ(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(41)
            PPCInterpreter_LHZU(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(42)
            PPCInterpreter_LHA(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(43)
            PPCInterpreter_LHAU(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(44)
            PPCInterpreter_STH(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(45)
            PPCInterpreter_STHU(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(46)
            PPCInterpreter_LMW(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(47)
            PPCInterpreter_STMW(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(48)
            PPCInterpreter_LFS(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(49)
            PPCInterpreter_LFSU(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(50)
            PPCInterpreter_LFD(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(51)
            PPCInterpreter_LFDU(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(52)
            PPCInterpreter_STFS(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(53)
            PPCInterpreter_STFSU(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(54)
            PPCInterpreter_STFD(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(55)
            PPCInterpreter_STFDU(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(56)
            PPCInterpreter_PSQ_L(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(57)
            PPCInterpreter_PSQ_LU(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(59) // opcode category
            switch (PPC_getBits(opcode, 30, 5))
            {
            case 18:
                PPCInterpreter_FDIVS(hCPU, opcode);
                break;
            case 20:
                PPCInterpreter_FSUBS(hCPU, opcode);
                break;
            case 21:
                PPCInterpreter_FADDS(hCPU, opcode);
                break;
            case 24:
                PPCInterpreter_FRES(hCPU, opcode);
                break;
            case 25:
                PPCInterpreter_FMULS(hCPU, opcode);
                break;
            case 28:
                PPCInterpreter_FMSUBS(hCPU, opcode);
                break;
            case 29:
                PPCInterpreter_FMADDS(hCPU, opcode);
                break;
            case 30:
                PPCInterpreter_FNMSUBS(hCPU, opcode);
                break;
            case 31:
                PPCInterpreter_FNMADDS(hCPU, opcode);
                break;
            default:
                cemuLog_logDebug(LogType::Force, "Unknown execute {:04x} as [59] at {:08x}\n", PPC_getBits(opcode, 30, 10), hCPU->instructionPointer);
                cemu_assert_unimplemented();
                hCPU->instructionPointer += 4;
                break;
            }
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(60)
            PPCInterpreter_PSQ_ST(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(61)
            PPCInterpreter_PSQ_STU(hCPU, opcode);
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_CASE(63) // opcode category
            switch (PPC_getBits(opcode, 30, 5))
            {
            case 0:
                PPCInterpreter_FCMPU(hCPU, opcode);
                break;
            case 12:
                PPCInterpreter_FRSP(hCPU, opcode);
                break;
            case 15:
                PPCInterpreter_FCTIWZ(hCPU, opcode);
                break;
            case 18:
                PPCInterpreter_FDIV(hCPU, opcode);
                break;
            case 20:
                PPCInterpreter_FSUB(hCPU, opcode);
                break;
            case 21:
                PPCInterpreter_FADD(hCPU, opcode);
                break;
            case 23:
                PPCInterpreter_FSEL(hCPU, opcode);
                break;
            case 25:
                PPCInterpreter_FMUL(hCPU, opcode);
                break;
            case 26:
                PPCInterpreter_FRSQRTE(hCPU, opcode);
                break;
            case 28:
                PPCInterpreter_FMSUB(hCPU, opcode);
                break;
            case 29:
                PPCInterpreter_FMADD(hCPU, opcode);
                break;
            case 30:
                PPCInterpreter_FNMSUB(hCPU, opcode);
                break;
            case 31:
                PPCInterpreter_FNMADD(hCPU, opcode);
                break;
            default:
                switch (PPC_getBits(opcode, 30, 10))
                {
                case 14:
                    PPCInterpreter_FCTIW(hCPU, opcode);
                    break;
                case 32:
                    PPCInterpreter_FCMPO(hCPU, opcode);
                    break;
                case 38:
                    PPCInterpreter_MTFSB1X(hCPU, opcode);
                    break;
                case 40:
                    PPCInterpreter_FNEG(hCPU, opcode);
                    break;
                case 72:
                    PPCInterpreter_FMR(hCPU, opcode);
                    break;
                case 136: // Darksiders 2
                    PPCInterpreter_FNABS(hCPU, opcode);
                    break;
                case 264:
                    PPCInterpreter_FABS(hCPU, opcode);
                    break;
                case 583:
                    PPCInterpreter_MFFS(hCPU, opcode);
                    break;
                case 711:
                    PPCInterpreter_MTFSF(hCPU, opcode);
                    break;
                default:
                    cemuLog_logDebug(LogType::Force, "Unknown execute {:04x} as [63] at {:08x}\n", PPC_getBits(opcode, 30, 10), hCPU->instructionPointer);
                    cemu_assert_unimplemented();
                    PPCInterpreter_nextInstruction(hCPU);
                    break;
                }
            }
            PPC_DISPATCH_NEXT();
        PPC_PRIMARY_DEFAULT
            cemuLog_logDebug(LogType::Force, "Unknown execute {:04x} at {:08x}\n", PPC_getBits(opcode, 5, 6), (unsigned int)hCPU->instructionPointer);
            cemu_assert_unimplemented();
            PPC_DISPATCH_NEXT();
        }
        
        if constexpr (runTimeslice)
        {
            if (--hCPU->remainingCycles >= 0)
                goto ppc_fetch;
        }
    }
    
    
    static void blockFallbackStep(PPCInterpreter_t* hCPU, uint32 /*opcode*/)
    {
        executeInstruction<false>(hCPU);
    }

    static void blockICBI(PPCInterpreter_t* hCPU, uint32 opcode)
    {
        PPCInterpreter_ICBI(hCPU, opcode);
        sint32 rD, rA, rB;
        PPC_OPC_TEMPL_X(opcode, rD, rA, rB);
        const uint32 ea = (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB];
        PPCInterpreter_invalidateBlockCacheRange(ea & ~31u, 32);
    }

    // threaded block handlers :3
#if PPC_BLOCK_MUSTTAIL
#define PPC_BLOCK_RET void
#define PPC_BLOCK_ADVANCE(n) do { const PPCBlockEntry* next_ = e + (n); [[clang::musttail]] return next_->fn(hCPU, next_, mem, ctx, cachedSP); } while (false)
#define PPC_BLOCK_EXIT() do { blkFlushSP(hCPU, cachedSP); return; } while (false)
// ends a block that left instructionPointer at ip, continuing straight into the next block when chainNext allows it
#define PPC_BLOCK_CHAIN(ip, fallthrough) do { \
    if (const PPCBlockEntry* next_ = chainNext(hCPU, ctx, (ip), (fallthrough))) \
        [[clang::musttail]] return next_->fn(hCPU, next_, mem, ctx, cachedSP); \
    blkFlushSP(hCPU, cachedSP); \
    return; \
} while (false)
#else
#define PPC_BLOCK_RET const PPCBlockEntry*
#define PPC_BLOCK_ADVANCE(n) return e + (n)
#define PPC_BLOCK_EXIT() return nullptr
// without guaranteed tail calls chaining would grow the stack, every block goes back to the dispatcher
#define PPC_BLOCK_CHAIN(ip, fallthrough) return nullptr
#endif
#define PPC_BLOCK_NEXT() PPC_BLOCK_ADVANCE(1)
#define PPC_BLOCK_OP(name) static PPC_BLOCK_RET PPC_BLOCK_CC name(PPCInterpreter_t* hCPU, const PPCBlockEntry* e, uint8* mem, [[maybe_unused]] PPCBlockCtx* ctx, [[maybe_unused]] uint32 cachedSP)

    static FORCE_INLINE uint32 blkAddr(const PPCBlockEntry* e, const PPCBlockCtx* ctx)
    {
        return ctx->blockStart + uint32(e - ctx->firstEntry) * 4;
    }

    static FORCE_INLINE uint32 blkGetGpr(PPCInterpreter_t* hCPU, uint32 reg, uint32 cachedSP)
    {
#if PPC_BLOCK_CACHE_SP
        return reg == 1 ? cachedSP : hCPU->gpr[reg];
#else
        return hCPU->gpr[reg];
#endif
    }

    static FORCE_INLINE void blkSetGpr(PPCInterpreter_t* hCPU, uint32 reg, uint32 value, uint32& cachedSP)
    {
#if PPC_BLOCK_CACHE_SP
        if (reg == 1)
            cachedSP = value;
        else
#endif
            hCPU->gpr[reg] = value;
    }

    static FORCE_INLINE void blkFlushSP(PPCInterpreter_t* hCPU, uint32 cachedSP)
    {
#if PPC_BLOCK_CACHE_SP
        hCPU->gpr[1] = cachedSP;
#endif
    }

    static FORCE_INLINE uint32 blkRead32(PPCInterpreter_t* hCPU, uint8* mem, uint32 ea)
    {
        if constexpr (ppcItpCtrl::allowSupervisorMode)
            return ppcItpCtrl::ppcMem_readDataU32(hCPU, ea);
        uint32 v;
        std::memcpy(&v, mem + ea, sizeof(v));
        return CPU_swapEndianU32(v);
    }
    static FORCE_INLINE uint16 blkRead16(PPCInterpreter_t* hCPU, uint8* mem, uint32 ea)
    {
        if constexpr (ppcItpCtrl::allowSupervisorMode)
            return ppcItpCtrl::ppcMem_readDataU16(hCPU, ea);
        uint16 v;
        std::memcpy(&v, mem + ea, sizeof(v));
        return CPU_swapEndianU16(v);
    }
    static FORCE_INLINE uint8 blkRead8(PPCInterpreter_t* hCPU, uint8* mem, uint32 ea)
    {
        if constexpr (ppcItpCtrl::allowSupervisorMode)
            return ppcItpCtrl::ppcMem_readDataU8(hCPU, ea);
        return mem[ea];
    }
    static FORCE_INLINE void blkWrite32(PPCInterpreter_t* hCPU, uint8* mem, uint32 ea, uint32 v)
    {
        if constexpr (ppcItpCtrl::allowSupervisorMode)
            return ppcItpCtrl::ppcMem_writeDataU32(hCPU, ea, v);
        v = CPU_swapEndianU32(v);
        std::memcpy(mem + ea, &v, sizeof(v));
    }
    static FORCE_INLINE void blkWrite16(PPCInterpreter_t* hCPU, uint8* mem, uint32 ea, uint16 v)
    {
        if constexpr (ppcItpCtrl::allowSupervisorMode)
            return ppcItpCtrl::ppcMem_writeDataU16(hCPU, ea, v);
        v = CPU_swapEndianU16(v);
        std::memcpy(mem + ea, &v, sizeof(v));
    }
    static FORCE_INLINE void blkWrite8(PPCInterpreter_t* hCPU, uint8* mem, uint32 ea, uint8 v)
    {
        if constexpr (ppcItpCtrl::allowSupervisorMode)
            return ppcItpCtrl::ppcMem_writeDataU8(hCPU, ea, v);
        mem[ea] = v;
    }
    static FORCE_INLINE void blkCompare(PPCInterpreter_t* hCPU, uint32 field, bool lt, bool gt)
    {
        const uint32 value = hCPU->xer_so | (lt ? 8 : (gt ? 4 : 2));
        ppc_setCRField(hCPU, field, value);
    }
    
    template<PPCBlockHandler H>
    PPC_BLOCK_OP(blkCall)
    {
        hCPU->instructionPointer = blkAddr(e, ctx);
        blkFlushSP(hCPU, cachedSP);
        H(hCPU, e->imm);
        cachedSP = hCPU->gpr[1];
        PPC_BLOCK_NEXT();
    }

    template<PPCBlockHandler H>
    static constexpr PPCLegacyOp legacyOp()
    {
        return { H, blkCall<H>, blkGenericTerm<H> };
    }
    
    template<PPCBlockHandler H>
    PPC_BLOCK_OP(blkGenericTerm)
    {
        hCPU->instructionPointer = blkAddr(e, ctx);
        const uint32 opcode = e->imm;
        blkFlushSP(hCPU, cachedSP);
        const uint32 fallthrough = blkAddr(e, ctx) + 4;
        
        ctx->genericExit = true;
        if (PPCBlockThreadState* ts = t_blockThreadState)
            ts->ack.store(0, std::memory_order_release);
        
        H(hCPU, opcode);
        cachedSP = hCPU->gpr[1];

        if (hCPU->remainingCycles <= 0)
            PPC_BLOCK_EXIT();
        PPCBlockThreadState* ts = t_blockThreadState;
        if (!ts)
            PPC_BLOCK_EXIT();
        {
            // The handler may yield or resize the cache. Pin it again before using any old pointers.
            std::unique_lock<std::mutex> lock(s_blockCacheMutex, std::try_to_lock);
            if (!lock.owns_lock() || s_blockCacheGeneration.load(std::memory_order_relaxed) != ctx->generation)
                PPC_BLOCK_EXIT();
            ts->ack.store(ctx->generation, std::memory_order_release);
        }
        ctx->genericExit = false;
        PPC_BLOCK_CHAIN((uint32)hCPU->instructionPointer, fallthrough);
    }

    static FORCE_INLINE const PPCBlockEntry* chainNext(PPCInterpreter_t* hCPU, PPCBlockCtx* ctx, uint32 ip, uint32 fallthrough)
    {
        if (hCPU->remainingCycles <= 0) [[unlikely]]
            return nullptr;
        if (s_blockCacheGeneration.load(std::memory_order_relaxed) != ctx->generation) [[unlikely]]
            return nullptr;
        // Match the dispatcher's link placement, including branches to their own fallthrough.
        const PPCBlockSlot* cur = ctx->slot;
        const uint32 link = ip == fallthrough ? PPCBLOCK_LINK_FALLTHROUGH : PPCBLOCK_LINK_TAKEN;
        PPCBlockSlot* slot = ctx->table + cur->nextSlot[link].load(std::memory_order_relaxed);
        PPCBlockDesc desc = slot->desc.load(std::memory_order_acquire);
        const uint32 key = PPCBlockDesc_keyForAddr(ip);
        if (PPCBlockDesc_key(desc) != key)
        {
            slot = PPCBlockCache_targetLookup(ctx->table, ctx->targetSlots, ip);
            if (!slot)
                return nullptr;
            desc = slot->desc.load(std::memory_order_acquire);
        }
        PPCBlockEntry* header = PPCBlockDesc_header(ctx->pool, desc);
        if (PPCBlockHeader_verifiedEpoch(header).load(std::memory_order_relaxed) != s_codeEpoch.load(std::memory_order_relaxed)) [[unlikely]]
            return nullptr;
        const uint32 count = PPCBlockDesc_count(desc);
        hCPU->remainingCycles -= (sint32)count;
        ctx->firstEntry = ctx->pool + PPCBlockDesc_firstEntry(desc);
        ctx->slot = slot;
        ctx->blockStart = ip;
        ctx->blockCount = count;
        return ctx->pool + PPCBlockDesc_firstEntry(desc);
    }

    // taken back edge of an idle loop, gives up the rest of the timeslice once the loop has spun for a while.
    static FORCE_INLINE void idleLoopBackEdge(PPCInterpreter_t* hCPU, PPCBlockCtx* ctx, uint32 addr)
    {
        if (++ctx->idleSpins < PPCBLOCK_IDLE_SPINS) [[likely]]
            return;
        ctx->idleSpins = 0;
        ctx->idleSkipAddr = addr;
        // same as PPCInterpreter_relinquishTimeslice
        if (hCPU->remainingCycles >= 0)
        {
            hCPU->skippedCycles = hCPU->remainingCycles + 1;
            hCPU->remainingCycles = -1;
        }
    }

    PPC_BLOCK_OP(blkEnd)
    {
        const uint32 ip = blkAddr(e, ctx);
        hCPU->instructionPointer = ip;
        PPC_BLOCK_CHAIN(ip, ip);
    }

    PPC_BLOCK_OP(blkBX)
    {
        if (e->x)
            hCPU->spr.LR = blkAddr(e, ctx) + 4;
        const uint32 ip = e->imm;
        hCPU->instructionPointer = ip;
        if (e->b & PPCBLOCK_BRANCH_IDLE)
            idleLoopBackEdge(hCPU, ctx, blkAddr(e, ctx));
        PPC_BLOCK_CHAIN(ip, blkAddr(e, ctx) + 4);
    }
    
    static FORCE_INLINE bool blkBranchCondition(PPCInterpreter_t* hCPU, uint32 BO, uint32 BI)
    {
        if (!(BO & 4))
            hCPU->spr.CTR--;
        const bool bo2 = (BO & 2) != 0;
        const bool bo8 = (BO & 8) != 0;
        const bool cr = ppc_getCRBit(hCPU, BI) != 0;
        return ((BO & 4) || ((hCPU->spr.CTR != 0) ^ bo2)) && ((BO & 16) || (!(cr ^ bo8)));
    }
    
    static FORCE_INLINE uint32 blkTakeBranch(PPCInterpreter_t* hCPU, const PPCBlockEntry* e, PPCBlockCtx* ctx, bool taken)
    {
        uint32 ip;
        if (taken)
        {
            if (e->x)
                hCPU->spr.LR = blkAddr(e, ctx) + 4;
            ip = e->imm;
            if (e->b & PPCBLOCK_BRANCH_IDLE)
                idleLoopBackEdge(hCPU, ctx, blkAddr(e, ctx));
        }
        else
        {
            ip = blkAddr(e, ctx) + 4;
            if (e->b & PPCBLOCK_BRANCH_IDLE)
                ctx->idleSpins = 0;
        }
        hCPU->instructionPointer = ip;
        return ip;
    }

    PPC_BLOCK_OP(blkBCX)
    {
        const uint32 ip = blkTakeBranch(hCPU, e, ctx, blkBranchCondition(hCPU, e->d, e->a));
        PPC_BLOCK_CHAIN(ip, blkAddr(e, ctx) + 4);
    }

    template<bool isSigned, bool isImm>
    PPC_BLOCK_OP(blkCmpBC)
    {
        const uint32 a = blkGetGpr(hCPU, e->a, cachedSP);
        const uint32 b = isImm ? e->imm : blkGetGpr(hCPU, e->b, cachedSP);
        const bool lt = isSigned ? (sint32)a < (sint32)b : a < b;
        const bool gt = isSigned ? (sint32)a > (sint32)b : a > b;
        blkCompare(hCPU, e->d, lt, gt);
        const PPCBlockEntry* bc = e + 1;
        const uint32 bit = bc->a & 3; // 0 lt, 1 gt, 2 eq, so (3) is never fused
        const bool crBit = bit == 0 ? lt : (bit == 1 ? gt : (!lt && !gt));
        const uint32 ip = blkTakeBranch(hCPU, bc, ctx, crBit == ((bc->d & 8) != 0));
        PPC_BLOCK_CHAIN(ip, blkAddr(bc, ctx) + 4);
    }

    PPC_BLOCK_OP(blkBCLRX)
    {
        uint32 ip;
        if (blkBranchCondition(hCPU, e->d, e->a))
        {
            ip = hCPU->spr.LR & 0xfffffffc;
            if (e->x)
                hCPU->spr.LR = blkAddr(e, ctx) + 4;
        }
        else
            ip = blkAddr(e, ctx) + 4;
        hCPU->instructionPointer = ip;
        PPC_BLOCK_CHAIN(ip, blkAddr(e, ctx) + 4);
    }
    PPC_BLOCK_OP(blkBCCTR)
    {
        const uint32 BO = e->d;
        const bool bo8 = (BO & 8) != 0;
        const bool cr = ppc_getCRBit(hCPU, e->a) != 0;
        uint32 ip;
        if ((BO & 16) || (!(cr ^ bo8)))
        {
            ip = hCPU->spr.CTR & 0xfffffffc;
            if (e->x)
                hCPU->spr.LR = blkAddr(e, ctx) + 4;
        }
        else
            ip = blkAddr(e, ctx) + 4;
        hCPU->instructionPointer = ip;
        PPC_BLOCK_CHAIN(ip, blkAddr(e, ctx) + 4);
    }

    PPC_BLOCK_OP(blkLI) { blkSetGpr(hCPU, e->d, e->imm, cachedSP); PPC_BLOCK_NEXT(); }
    PPC_BLOCK_OP(blkADDI) { blkSetGpr(hCPU, e->d, blkGetGpr(hCPU, e->a, cachedSP) + e->imm, cachedSP); PPC_BLOCK_NEXT(); }
    PPC_BLOCK_OP(blkADDIC)
    {
        const uint32 a = blkGetGpr(hCPU, e->a, cachedSP);
        const uint32 r = a + e->imm;
        blkSetGpr(hCPU, e->d, r, cachedSP);
        hCPU->xer_ca = r < a;
        if (e->x)
            ppc_update_cr0(hCPU, r);
        PPC_BLOCK_NEXT();
    }
    PPC_BLOCK_OP(blkSUBFIC)
    {
        const uint32 a = blkGetGpr(hCPU, e->a, cachedSP);
        blkSetGpr(hCPU, e->d, ~a + e->imm + 1, cachedSP);
        hCPU->xer_ca = ppc_carry_3(~a, e->imm, 1) ? 1 : 0;
        PPC_BLOCK_NEXT();
    }
    PPC_BLOCK_OP(blkMULLI) { blkSetGpr(hCPU, e->d, blkGetGpr(hCPU, e->a, cachedSP) * e->imm, cachedSP); PPC_BLOCK_NEXT(); }
    PPC_BLOCK_OP(blkCMPI)
    {
        const sint32 a = (sint32)blkGetGpr(hCPU, e->a, cachedSP), b = (sint32)e->imm;
        blkCompare(hCPU, e->d, a < b, a > b);
        PPC_BLOCK_NEXT();
    }
    PPC_BLOCK_OP(blkCMPLI)
    {
        const uint32 a = blkGetGpr(hCPU, e->a, cachedSP), b = e->imm;
        blkCompare(hCPU, e->d, a < b, a > b);
        PPC_BLOCK_NEXT();
    }
    PPC_BLOCK_OP(blkORI) { blkSetGpr(hCPU, e->a, blkGetGpr(hCPU, e->d, cachedSP) | e->imm, cachedSP); PPC_BLOCK_NEXT(); }
    PPC_BLOCK_OP(blkXORI) { blkSetGpr(hCPU, e->a, blkGetGpr(hCPU, e->d, cachedSP) ^ e->imm, cachedSP); PPC_BLOCK_NEXT(); }
    PPC_BLOCK_OP(blkANDI_)
    {
        const uint32 r = blkGetGpr(hCPU, e->d, cachedSP) & e->imm;
        blkSetGpr(hCPU, e->a, r, cachedSP);
        ppc_update_cr0(hCPU, r);
        PPC_BLOCK_NEXT();
    }
    // b: SH, imm: mask
    PPC_BLOCK_OP(blkRLWINM)
    {
        const uint32 r = std::rotl<uint32>(blkGetGpr(hCPU, e->d, cachedSP), e->b) & e->imm;
        blkSetGpr(hCPU, e->a, r, cachedSP);
        if (e->x)
            ppc_update_cr0(hCPU, r);
        PPC_BLOCK_NEXT();
    }
    PPC_BLOCK_OP(blkRLWIMI)
    {
        const uint32 mask = e->imm;
        const uint32 r = (std::rotl<uint32>(blkGetGpr(hCPU, e->d, cachedSP), e->b) & mask) | (blkGetGpr(hCPU, e->a, cachedSP) & ~mask);
        blkSetGpr(hCPU, e->a, r, cachedSP);
        if (e->x)
            ppc_update_cr0(hCPU, r);
        PPC_BLOCK_NEXT();
    }

    PPC_BLOCK_OP(blkRLWNM)
    {
        const uint32 r = std::rotl<uint32>(blkGetGpr(hCPU, e->d, cachedSP), blkGetGpr(hCPU, e->b, cachedSP) & 31) & e->imm;
        blkSetGpr(hCPU, e->a, r, cachedSP);
        if (e->x)
            ppc_update_cr0(hCPU, r);
        PPC_BLOCK_NEXT();
    }
    
    template<bool subtract, bool extended, uint32 operand = 0>
    PPC_BLOCK_OP(blkCarry)
    {
        const uint32 a = subtract ? ~blkGetGpr(hCPU, e->a, cachedSP) : blkGetGpr(hCPU, e->a, cachedSP);
        const uint32 b = operand == 0 ? blkGetGpr(hCPU, e->b, cachedSP) : (operand == 1 ? 0u : 0xFFFFFFFFu);
        const uint32 carry = extended ? hCPU->xer_ca : uint32(subtract);
        const uint64 sum = uint64(a) + b + carry;
        const uint32 r = uint32(sum);
        blkSetGpr(hCPU, e->d, r, cachedSP);
        hCPU->xer_ca = uint8(sum >> 32);
        if (e->x)
            ppc_update_cr0(hCPU, r);
        PPC_BLOCK_NEXT();
    }

#define PPC_BLOCK_ARITH(name, expr) \
    PPC_BLOCK_OP(name) \
    { \
        const uint32 ra = blkGetGpr(hCPU, e->a, cachedSP), rb = blkGetGpr(hCPU, e->b, cachedSP); (void)rb; \
        const uint32 r = (expr); \
        blkSetGpr(hCPU, e->d, r, cachedSP); \
        if (e->x) \
            ppc_update_cr0(hCPU, r); \
        PPC_BLOCK_NEXT(); \
    }
#define PPC_BLOCK_LOGIC(name, expr) \
    PPC_BLOCK_OP(name) \
    { \
        const uint32 rs = blkGetGpr(hCPU, e->d, cachedSP), rb = blkGetGpr(hCPU, e->b, cachedSP); (void)rb; \
        const uint32 r = (expr); \
        blkSetGpr(hCPU, e->a, r, cachedSP); \
        if (e->x) \
            ppc_update_cr0(hCPU, r); \
        PPC_BLOCK_NEXT(); \
    }
    PPC_BLOCK_ARITH(blkADD, ra + rb)
    PPC_BLOCK_ARITH(blkSUBF, ~ra + rb + 1)
    PPC_BLOCK_ARITH(blkNEG, (uint32)-(sint32)ra)
    PPC_BLOCK_ARITH(blkMULLW, (uint32)((sint64)ra * (sint64)rb))
    PPC_BLOCK_ARITH(blkMULHW, (uint32)((uint64)((sint64)(sint32)ra * (sint64)(sint32)rb) >> 32))
    PPC_BLOCK_ARITH(blkMULHWU, (uint32)(((uint64)ra * rb) >> 32))
    PPC_BLOCK_LOGIC(blkOR, rs | rb)
    PPC_BLOCK_LOGIC(blkAND, rs & rb)
    PPC_BLOCK_LOGIC(blkXOR, rs ^ rb)
    PPC_BLOCK_LOGIC(blkANDC, rs & ~rb)
    PPC_BLOCK_LOGIC(blkNOR, ~(rs | rb))
    PPC_BLOCK_LOGIC(blkEXTSB, (uint32)(sint32)(sint8)rs)
    PPC_BLOCK_LOGIC(blkEXTSH, (uint32)(sint32)(sint16)rs)
    PPC_BLOCK_LOGIC(blkCNTLZW, (uint32)std::countl_zero(rs))
    PPC_BLOCK_LOGIC(blkSLW, (rb & 0x3f) > 31 ? 0 : rs << (rb & 0x3f))
    PPC_BLOCK_LOGIC(blkSRW, (rb & 0x3f) > 31 ? 0 : rs >> (rb & 0x3f))
#undef PPC_BLOCK_ARITH
#undef PPC_BLOCK_LOGIC
    // b: SH
    PPC_BLOCK_OP(blkSRAWI)
    {
        const uint32 s = blkGetGpr(hCPU, e->d, cachedSP);
        const uint32 sh = e->b;
        const uint32 droppedBits = s & ~(0xFFFFFFFFu << sh);
        hCPU->xer_ca = (uint8)((s >> 31) & (droppedBits != 0 ? 1u : 0u));
        const uint32 r = (uint32)((sint32)s >> sh);
        blkSetGpr(hCPU, e->a, r, cachedSP);
        if (e->x)
            ppc_update_cr0(hCPU, r);
        PPC_BLOCK_NEXT();
    }
    PPC_BLOCK_OP(blkSRAW)
    {
        const uint32 s = blkGetGpr(hCPU, e->d, cachedSP);
        const uint32 sh = blkGetGpr(hCPU, e->b, cachedSP) & 0x3f;
        uint32 r;
        if (sh > 31)
        {
            hCPU->xer_ca = (s >> 31) & 1;
            r = (uint32)((sint32)s >> 31);
        }
        else
        {
            const uint32 shiftedBits = s & ~(0xFFFFFFFF << sh);
            hCPU->xer_ca = ((s >> 31) & 1) & (shiftedBits != 0 ? 1 : 0);
            r = (uint32)((sint32)s >> sh);
        }
        blkSetGpr(hCPU, e->a, r, cachedSP);
        if (e->x)
            ppc_update_cr0(hCPU, r);
        PPC_BLOCK_NEXT();
    }
    // d: CR field
    PPC_BLOCK_OP(blkCMP)
    {
        const sint32 a = (sint32)blkGetGpr(hCPU, e->a, cachedSP), b = (sint32)blkGetGpr(hCPU, e->b, cachedSP);
        blkCompare(hCPU, e->d, a < b, a > b);
        PPC_BLOCK_NEXT();
    }
    PPC_BLOCK_OP(blkCMPL)
    {
        const uint32 a = blkGetGpr(hCPU, e->a, cachedSP), b = blkGetGpr(hCPU, e->b, cachedSP);
        blkCompare(hCPU, e->d, a < b, a > b);
        PPC_BLOCK_NEXT();
    }
    PPC_BLOCK_OP(blkMFLR) { blkSetGpr(hCPU, e->d, hCPU->spr.LR, cachedSP); PPC_BLOCK_NEXT(); }
    PPC_BLOCK_OP(blkMFCTR) { blkSetGpr(hCPU, e->d, hCPU->spr.CTR, cachedSP); PPC_BLOCK_NEXT(); }
    PPC_BLOCK_OP(blkMTLR) { hCPU->spr.LR = blkGetGpr(hCPU, e->d, cachedSP); PPC_BLOCK_NEXT(); }
    PPC_BLOCK_OP(blkMTCTR) { hCPU->spr.CTR = blkGetGpr(hCPU, e->d, cachedSP); PPC_BLOCK_NEXT(); }

#define PPC_BLOCK_LOAD(name, expr) \
    PPC_BLOCK_OP(name) { const uint32 ea = blkGetGpr(hCPU, e->a, cachedSP) + e->imm; blkSetGpr(hCPU, e->d, (expr), cachedSP); PPC_BLOCK_NEXT(); } \
    PPC_BLOCK_OP(name##A) { const uint32 ea = e->imm; blkSetGpr(hCPU, e->d, (expr), cachedSP); PPC_BLOCK_NEXT(); } \
    PPC_BLOCK_OP(name##X) { const uint32 ea = (e->a ? blkGetGpr(hCPU, e->a, cachedSP) : 0) + blkGetGpr(hCPU, e->b, cachedSP); blkSetGpr(hCPU, e->d, (expr), cachedSP); PPC_BLOCK_NEXT(); }
#define PPC_BLOCK_STORE(name, stmt) \
    PPC_BLOCK_OP(name) { const uint32 ea = blkGetGpr(hCPU, e->a, cachedSP) + e->imm; const uint32 v = blkGetGpr(hCPU, e->d, cachedSP); stmt; PPC_BLOCK_NEXT(); } \
    PPC_BLOCK_OP(name##A) { const uint32 ea = e->imm; const uint32 v = blkGetGpr(hCPU, e->d, cachedSP); stmt; PPC_BLOCK_NEXT(); } \
    PPC_BLOCK_OP(name##X) { const uint32 ea = (e->a ? blkGetGpr(hCPU, e->a, cachedSP) : 0) + blkGetGpr(hCPU, e->b, cachedSP); const uint32 v = blkGetGpr(hCPU, e->d, cachedSP); stmt; PPC_BLOCK_NEXT(); }
    PPC_BLOCK_LOAD(blkLWZ, blkRead32(hCPU, mem, ea))
    PPC_BLOCK_LOAD(blkLBZ, blkRead8(hCPU, mem, ea))
    PPC_BLOCK_LOAD(blkLHZ, blkRead16(hCPU, mem, ea))
    PPC_BLOCK_LOAD(blkLHA, (uint32)(sint32)(sint16)blkRead16(hCPU, mem, ea))
    PPC_BLOCK_STORE(blkSTW, blkWrite32(hCPU, mem, ea, v))
    PPC_BLOCK_STORE(blkSTB, blkWrite8(hCPU, mem, ea, (uint8)v))
    PPC_BLOCK_STORE(blkSTH, blkWrite16(hCPU, mem, ea, (uint16)v))
#undef PPC_BLOCK_LOAD
#undef PPC_BLOCK_STORE
    PPC_BLOCK_OP(blkNOP) { PPC_BLOCK_NEXT(); }
    PPC_BLOCK_OP(blkSTWU)
    {
        const uint32 ea = blkGetGpr(hCPU, e->a, cachedSP) + e->imm;
        blkWrite32(hCPU, mem, ea, blkGetGpr(hCPU, e->d, cachedSP));
        blkSetGpr(hCPU, e->a, ea, cachedSP);
        PPC_BLOCK_NEXT();
    }
    PPC_BLOCK_OP(blkSTBU)
    {
        const uint32 ea = blkGetGpr(hCPU, e->a, cachedSP) + e->imm;
        blkWrite8(hCPU, mem, ea, (uint8)blkGetGpr(hCPU, e->d, cachedSP));
        blkSetGpr(hCPU, e->a, ea, cachedSP);
        PPC_BLOCK_NEXT();
    }
    PPC_BLOCK_OP(blkLWZU)
    {
        const uint32 ea = blkGetGpr(hCPU, e->a, cachedSP) + e->imm;
        blkSetGpr(hCPU, e->a, ea, cachedSP);
        blkSetGpr(hCPU, e->d, blkRead32(hCPU, mem, ea), cachedSP);
        PPC_BLOCK_NEXT();
    }
    PPC_BLOCK_OP(blkLBZU)
    {
        const uint32 ea = blkGetGpr(hCPU, e->a, cachedSP) + e->imm;
        blkSetGpr(hCPU, e->a, ea, cachedSP);
        blkSetGpr(hCPU, e->d, blkRead8(hCPU, mem, ea), cachedSP);
        PPC_BLOCK_NEXT();
    }
    PPC_BLOCK_OP(blkLFS)
    {
        const uint32 ea = (e->a ? blkGetGpr(hCPU, e->a, cachedSP) : 0) + e->imm;
        uint64 val;
        if constexpr (ppcItpCtrl::allowSupervisorMode)
            val = ppcItpCtrl::ppcMem_readDataFloatEx(hCPU, ea);
        else
            val = ConvertToDoubleNoFTZ(blkRead32(hCPU, mem, ea));
        if (PPC_LSQE)
            hCPU->fpr[e->d].fp0int = hCPU->fpr[e->d].fp1int = val;
        else
            hCPU->fpr[e->d].fp0int = val;
        PPC_BLOCK_NEXT();
    }
    PPC_BLOCK_OP(blkLFD)
    {
        const uint32 ea = (e->a ? blkGetGpr(hCPU, e->a, cachedSP) : 0) + e->imm;
        if constexpr (ppcItpCtrl::allowSupervisorMode)
            hCPU->fpr[e->d].fpr = ppcItpCtrl::ppcMem_readDataDouble(hCPU, ea);
        else
        {
            uint64 v;
            std::memcpy(&v, mem + ea, sizeof(v));
            hCPU->fpr[e->d].fp0int = CPU_swapEndianU64(v);
        }
        PPC_BLOCK_NEXT();
    }
    PPC_BLOCK_OP(blkSTFS)
    {
        const uint32 ea = (e->a ? blkGetGpr(hCPU, e->a, cachedSP) : 0) + e->imm;
        if constexpr (ppcItpCtrl::allowSupervisorMode)
            ppcItpCtrl::ppcMem_writeDataFloatEx(hCPU, ea, hCPU->fpr[e->d].fp0int);
        else
            blkWrite32(hCPU, mem, ea, ConvertToSingleNoFTZ(hCPU->fpr[e->d].fp0int));
        PPC_BLOCK_NEXT();
    }
    PPC_BLOCK_OP(blkSTFD)
    {
        const uint32 ea = (e->a ? blkGetGpr(hCPU, e->a, cachedSP) : 0) + e->imm;
        if constexpr (ppcItpCtrl::allowSupervisorMode)
            ppcItpCtrl::ppcMem_writeDataDouble(hCPU, ea, hCPU->fpr[e->d].fpr);
        else
        {
            const uint64 v = CPU_swapEndianU64(hCPU->fpr[e->d].fp0int);
            std::memcpy(mem + ea, &v, sizeof(v));
        }
        PPC_BLOCK_NEXT();
    }

    // Preserve the first instruction's architectural write even when the second
    // uses a different destination. Its value stays local between the two ops.
    template<bool Or>
    PPC_BLOCK_OP(blkLI_Pair)
    {
        const PPCBlockEntry* second = e + 1;
        const uint32 value = e->imm;
        blkSetGpr(hCPU, e->d, value, cachedSP);
        const uint32 result = Or ? value | second->imm : value + second->imm;
        blkSetGpr(hCPU, Or ? second->a : second->d, result, cachedSP);
        PPC_BLOCK_ADVANCE(2);
    }

    template<bool Store>
    PPC_BLOCK_OP(blkADDI_Mem)
    {
        const PPCBlockEntry* second = e + 1;
        const uint32 value = blkGetGpr(hCPU, e->a, cachedSP) + e->imm;
        blkSetGpr(hCPU, e->d, value, cachedSP);
        const uint32 ea = value + second->imm;
        if constexpr (Store)
            blkWrite32(hCPU, mem, ea, blkGetGpr(hCPU, second->d, cachedSP));
        else
            blkSetGpr(hCPU, second->d, blkRead32(hCPU, mem, ea), cachedSP);
        PPC_BLOCK_ADVANCE(2);
    }

    // GQR contents remain dynamic: games may change their type/scale at runtime.
    // Only opcode operands and W/update/indexed choices are specialized.
    template<bool Store, bool Update, bool Indexed, bool Single>
    PPC_BLOCK_OP(blkPSQ)
    {
        const uint32 base = e->a ? blkGetGpr(hCPU, e->a, cachedSP) : 0;
        const uint32 ea = base + (Indexed ? blkGetGpr(hCPU, e->b, cachedSP) : e->imm);
        if constexpr (Update)
            if (e->a)
                blkSetGpr(hCPU, e->a, ea, cachedSP);
        const uint32 gqr = hCPU->spr.UGQR[e->x];
        const uint32 type = Store ? gqr & 7 : (gqr >> 16) & 7;
        const uint8 scale = Store ? (gqr >> 8) & 63 : (gqr >> 24) & 63;
        PPCInterpreter_psqTransfer<Store>(hCPU, ea, e->d, type, scale, Single);
        PPC_BLOCK_NEXT();
    }

    template<bool Store, bool Indexed>
    static PPCBlockFn decodePSQ(uint32 opcode, PPCBlockEntry& en)
    {
        const bool single = (opcode & (Indexed ? 0x400 : 0x8000)) != 0;
        const bool update = (opcode & (Indexed ? 0x40 : 0x04000000)) != 0;
        en.x = (opcode >> (Indexed ? 7 : 12)) & 7;
        en.imm = uint32(sint32(sint16((opcode & 0xFFF) << 4)) >> 4);
        if (update)
            return single ? blkPSQ<Store, true, Indexed, true> : blkPSQ<Store, true, Indexed, false>;
        return single ? blkPSQ<Store, false, Indexed, true> : blkPSQ<Store, false, Indexed, false>;
    }

    // floating point and paired singles, d: frD, a: frA, b: frB, x: frC
    // these are the same as PPCInterpreterFPU.cpp and PPCInterpreterPS.cpp exactly, Rc is ignored there too
    // the register references are taken up front, otherwise every store to an FPR forces the byte sized indices in e to be reloaded
#define PPC_BLOCK_FP_OP(name) \
    PPC_BLOCK_OP(name) \
    { \
        name##_body(hCPU->fpr[e->d], hCPU->fpr[e->a], hCPU->fpr[e->b], hCPU->fpr[e->x]); \
        PPC_BLOCK_NEXT(); \
    } \
    static FORCE_INLINE void name##_body([[maybe_unused]] FPR_t& fD, [[maybe_unused]] const FPR_t& fA, [[maybe_unused]] const FPR_t& fB, [[maybe_unused]] const FPR_t& fC)
    PPC_BLOCK_FP_OP(blkFMR) { fD.fpr = fB.fpr; }
    PPC_BLOCK_FP_OP(blkFNEG) { fD.guint = fB.guint ^ (1ULL << 63); }
    PPC_BLOCK_FP_OP(blkFADD) { fD.fpr = fA.fpr + fB.fpr; }
    PPC_BLOCK_FP_OP(blkFSUB) { fD.fpr = fA.fpr - fB.fpr; }
    PPC_BLOCK_FP_OP(blkFMUL) { fD.fpr = fA.fpr * fC.fpr; }
    PPC_BLOCK_FP_OP(blkFDIV) { fD.fpr = fA.fpr / fB.fpr; }
    PPC_BLOCK_FP_OP(blkFMADD) { fD.fpr = fA.fpr * fC.fpr + fB.fpr; }
    PPC_BLOCK_FP_OP(blkFMSUB) { fD.fpr = (fA.fpr * fC.fpr - fB.fpr); }

#define PPC_BLOCK_FP_SINGLE(name, expr) \
    PPC_BLOCK_FP_OP(name) \
    { \
        fD.fpr = (float)(expr); \
        if (PPC_PSE) \
            fD.fp1 = fD.fp0; \
    }
    PPC_BLOCK_FP_SINGLE(blkFADDS, fA.fpr + fB.fpr)
    PPC_BLOCK_FP_SINGLE(blkFSUBS, fA.fpr - fB.fpr)
    PPC_BLOCK_FP_SINGLE(blkFDIVS, fA.fpr / fB.fpr)
    PPC_BLOCK_FP_SINGLE(blkFMULS, fA.fpr * roundTo25BitAccuracy(fC.fpr))
    PPC_BLOCK_FP_SINGLE(blkFMADDS, fA.fpr * roundTo25BitAccuracy(fC.fpr) + fB.fpr)
    PPC_BLOCK_FP_SINGLE(blkFMSUBS, fA.fp0 * roundTo25BitAccuracy(fC.fp0) - fB.fp0)
#undef PPC_BLOCK_FP_SINGLE

    // both slots are computed before frD is written, frD may alias a source
#define PPC_BLOCK_PS(name, expr0, expr1) \
    PPC_BLOCK_FP_OP(name) \
    { \
        const auto s0 = (expr0); \
        const auto s1 = (expr1); \
        fD.fp0 = s0; \
        fD.fp1 = s1; \
    }
    PPC_BLOCK_PS(blkPS_MR, fB.fp0, fB.fp1)
    PPC_BLOCK_PS(blkPS_NEG, -fB.fp0, -fB.fp1)
    PPC_BLOCK_PS(blkPS_MERGE00, fA.fp0, fB.fp0)
    PPC_BLOCK_PS(blkPS_MERGE01, fA.fp0, fB.fp1)
    PPC_BLOCK_PS(blkPS_MERGE10, fA.fp1, fB.fp0)
    PPC_BLOCK_PS(blkPS_MERGE11, fA.fp1, fB.fp1)
    PPC_BLOCK_PS(blkPS_ADD, (float)(fA.fp0 + fB.fp0), (float)(fA.fp1 + fB.fp1))
    PPC_BLOCK_PS(blkPS_SUB, (float)(fA.fp0 - fB.fp0), (float)(fA.fp1 - fB.fp1))
    PPC_BLOCK_PS(blkPS_MUL,
        flushDenormalToZero((float)(fA.fp0 * roundTo25BitAccuracy(fC.fp0))),
        flushDenormalToZero((float)(fA.fp1 * roundTo25BitAccuracy(fC.fp1))))
    PPC_BLOCK_PS(blkPS_MADD,
        flushDenormalToZero((float)((float)(fA.fp0 * roundTo25BitAccuracy(fC.fp0)) + fB.fp0)),
        flushDenormalToZero((float)((float)(fA.fp1 * roundTo25BitAccuracy(fC.fp1)) + fB.fp1)))
    PPC_BLOCK_PS(blkPS_MSUB,
        (float)(fA.fp0 * roundTo25BitAccuracy(fC.fp0) - fB.fp0),
        (float)(fA.fp1 * roundTo25BitAccuracy(fC.fp1) - fB.fp1))
    PPC_BLOCK_PS(blkPS_MULS0, (float)(fA.fp0 * roundTo25BitAccuracy(fC.fp0)), (float)(fA.fp1 * roundTo25BitAccuracy(fC.fp0)))
    PPC_BLOCK_PS(blkPS_MULS1, (float)(fA.fp0 * roundTo25BitAccuracy(fC.fp1)), (float)(fA.fp1 * roundTo25BitAccuracy(fC.fp1)))
    PPC_BLOCK_PS(blkPS_MADDS0,
        (float)(fA.fp0 * roundTo25BitAccuracy(fC.fp0) + fB.fp0),
        (float)(fA.fp1 * roundTo25BitAccuracy(fC.fp0) + fB.fp1))
    PPC_BLOCK_PS(blkPS_MADDS1,
        (float)(fA.fp0 * roundTo25BitAccuracy(fC.fp1) + fB.fp0),
        (float)(fA.fp1 * roundTo25BitAccuracy(fC.fp1) + fB.fp1))
#undef PPC_BLOCK_PS
#undef PPC_BLOCK_FP_OP
    
    static PPCLegacyOp legacyHandler(uint32 opcode, PPCBlockTerm& term)
    {
        static constexpr auto s_table19 = makeHandlerTable19();
        static constexpr auto s_table31 = makeHandlerTable31();
        term = PPCBlockTerm::None;
        PPCLegacyOp fn{};
        switch (opcode >> 26)
        {
        case 4:
            switch (PPC_getBits(opcode, 30, 5))
            {
            case 0:
                switch (PPC_getBits(opcode, 25, 5))
                {
                case 0: fn = legacyOp<PPCInterpreter_PS_CMPU0>(); break;
                case 1: fn = legacyOp<PPCInterpreter_PS_CMPO0>(); break;
                case 2: fn = legacyOp<PPCInterpreter_PS_CMPU1>(); break;
                default: break;
                }
                break;
            case 6: fn = legacyOp<PPCInterpreter_PSQ_LX>(); break;
            case 7: fn = legacyOp<PPCInterpreter_PSQ_STX>(); break;
            case 8:
                switch (PPC_getBits(opcode, 25, 5))
                {
                case 1: fn = legacyOp<PPCInterpreter_PS_NEG>(); break;
                case 2: fn = legacyOp<PPCInterpreter_PS_MR>(); break;
                case 4: fn = legacyOp<PPCInterpreter_PS_NABS>(); break;
                case 8: fn = legacyOp<PPCInterpreter_PS_ABS>(); break;
                default: break;
                }
                break;
            case 10: fn = legacyOp<PPCInterpreter_PS_SUM0>(); break;
            case 11: fn = legacyOp<PPCInterpreter_PS_SUM1>(); break;
            case 12: fn = legacyOp<PPCInterpreter_PS_MULS0>(); break;
            case 13: fn = legacyOp<PPCInterpreter_PS_MULS1>(); break;
            case 14: fn = legacyOp<PPCInterpreter_PS_MADDS0>(); break;
            case 15: fn = legacyOp<PPCInterpreter_PS_MADDS1>(); break;
            case 16:
                switch (PPC_getBits(opcode, 25, 5))
                {
                case 16: fn = legacyOp<PPCInterpreter_PS_MERGE00>(); break;
                case 17: fn = legacyOp<PPCInterpreter_PS_MERGE01>(); break;
                case 18: fn = legacyOp<PPCInterpreter_PS_MERGE10>(); break;
                case 19: fn = legacyOp<PPCInterpreter_PS_MERGE11>(); break;
                default: break;
                }
                break;
            case 18: fn = legacyOp<PPCInterpreter_PS_DIV>(); break;
            case 20: fn = legacyOp<PPCInterpreter_PS_SUB>(); break;
            case 21: fn = legacyOp<PPCInterpreter_PS_ADD>(); break;
            case 22: fn = legacyOp<PPCInterpreter_DCBZL>(); break;
            case 23: fn = legacyOp<PPCInterpreter_PS_SEL>(); break;
            case 24: fn = legacyOp<PPCInterpreter_PS_RES>(); break;
            case 25: fn = legacyOp<PPCInterpreter_PS_MUL>(); break;
            case 26: fn = legacyOp<PPCInterpreter_PS_RSQRTE>(); break;
            case 28: fn = legacyOp<PPCInterpreter_PS_MSUB>(); break;
            case 29: fn = legacyOp<PPCInterpreter_PS_MADD>(); break;
            case 30: fn = legacyOp<PPCInterpreter_PS_NMSUB>(); break;
            case 31: fn = legacyOp<PPCInterpreter_PS_NMADD>(); break;
            default: break;
            }
            break;
        case 7: fn = legacyOp<PPCInterpreter_MULLI>(); break;
        case 8: fn = legacyOp<PPCInterpreter_SUBFIC>(); break;
        case 10: fn = legacyOp<PPCInterpreter_CMPLI>(); break;
        case 11: fn = legacyOp<PPCInterpreter_CMPI>(); break;
        case 12: fn = legacyOp<PPCInterpreter_ADDIC>(); break;
        case 13: fn = legacyOp<PPCInterpreter_ADDIC_>(); break;
        case 14: fn = legacyOp<PPCInterpreter_ADDI>(); break;
        case 15: fn = legacyOp<PPCInterpreter_ADDIS>(); break;
        case 16: term = PPCBlockTerm::BCX; break;
        case 18: term = PPCBlockTerm::BX; break;
        case 19:
        {
            const uint32 ext = PPC_getBits(opcode, 30, 10);
            if (ext == 16)
                term = PPCBlockTerm::BCLRX;
            else if (ext == 528)
                term = PPCBlockTerm::BCCTR;
            else if (ext == 50 || ext == 150) // rfi, isync > classic path, terminate
                term = PPCBlockTerm::Generic;
            else
                fn = s_table19[ext]; // cr ops, mcrf: straight-line
            break;
        }
        case 20: fn = legacyOp<PPCInterpreter_RLWIMI>(); break;
        case 21: fn = legacyOp<PPCInterpreter_RLWINM>(); break;
        case 23: fn = legacyOp<PPCInterpreter_RLWNM>(); break;
        case 24: fn = legacyOp<PPCInterpreter_ORI>(); break;
        case 25: fn = legacyOp<PPCInterpreter_ORIS>(); break;
        case 26: fn = legacyOp<PPCInterpreter_XORI>(); break;
        case 27: fn = legacyOp<PPCInterpreter_XORIS>(); break;
        case 28: fn = legacyOp<PPCInterpreter_ANDI_>(); break;
        case 29: fn = legacyOp<PPCInterpreter_ANDIS_>(); break;
        case 31:
        {
            const uint32 ext = PPC_getBits(opcode, 30, 10);
            if (ext == 982) // icbi
            {
                fn = legacyOp<blockICBI>();
                term = PPCBlockTerm::Generic;
            }
            else if (ext == 4 || ext == 146) // tw, mtmsr -> classic path, terminate
                term = PPCBlockTerm::Generic;
            else
                fn = s_table31[ext];
            break;
        }
        case 32: fn = legacyOp<PPCInterpreter_LWZ>(); break;
        case 33: fn = legacyOp<PPCInterpreter_LWZU>(); break;
        case 34: fn = legacyOp<PPCInterpreter_LBZ>(); break;
        case 35: fn = legacyOp<PPCInterpreter_LBZU>(); break;
        case 36: fn = legacyOp<PPCInterpreter_STW>(); break;
        case 37: fn = legacyOp<PPCInterpreter_STWU>(); break;
        case 38: fn = legacyOp<PPCInterpreter_STB>(); break;
        case 39: fn = legacyOp<PPCInterpreter_STBU>(); break;
        case 40: fn = legacyOp<PPCInterpreter_LHZ>(); break;
        case 41: fn = legacyOp<PPCInterpreter_LHZU>(); break;
        case 42: fn = legacyOp<PPCInterpreter_LHA>(); break;
        case 43: fn = legacyOp<PPCInterpreter_LHAU>(); break;
        case 44: fn = legacyOp<PPCInterpreter_STH>(); break;
        case 45: fn = legacyOp<PPCInterpreter_STHU>(); break;
        case 46: fn = legacyOp<PPCInterpreter_LMW>(); break;
        case 47: fn = legacyOp<PPCInterpreter_STMW>(); break;
        case 48: fn = legacyOp<PPCInterpreter_LFS>(); break;
        case 49: fn = legacyOp<PPCInterpreter_LFSU>(); break;
        case 50: fn = legacyOp<PPCInterpreter_LFD>(); break;
        case 51: fn = legacyOp<PPCInterpreter_LFDU>(); break;
        case 52: fn = legacyOp<PPCInterpreter_STFS>(); break;
        case 53: fn = legacyOp<PPCInterpreter_STFSU>(); break;
        case 54: fn = legacyOp<PPCInterpreter_STFD>(); break;
        case 55: fn = legacyOp<PPCInterpreter_STFDU>(); break;
        case 56: fn = legacyOp<PPCInterpreter_PSQ_L>(); break;
        case 57: fn = legacyOp<PPCInterpreter_PSQ_LU>(); break;
        case 59:
            switch (PPC_getBits(opcode, 30, 5))
            {
            case 18: fn = legacyOp<PPCInterpreter_FDIVS>(); break;
            case 20: fn = legacyOp<PPCInterpreter_FSUBS>(); break;
            case 21: fn = legacyOp<PPCInterpreter_FADDS>(); break;
            case 24: fn = legacyOp<PPCInterpreter_FRES>(); break;
            case 25: fn = legacyOp<PPCInterpreter_FMULS>(); break;
            case 28: fn = legacyOp<PPCInterpreter_FMSUBS>(); break;
            case 29: fn = legacyOp<PPCInterpreter_FMADDS>(); break;
            case 30: fn = legacyOp<PPCInterpreter_FNMSUBS>(); break;
            case 31: fn = legacyOp<PPCInterpreter_FNMADDS>(); break;
            default: break;
            }
            break;
        case 60: fn = legacyOp<PPCInterpreter_PSQ_ST>(); break;
        case 61: fn = legacyOp<PPCInterpreter_PSQ_STU>(); break;
        case 63:
            switch (PPC_getBits(opcode, 30, 5))
            {
            case 0: fn = legacyOp<PPCInterpreter_FCMPU>(); break;
            case 12: fn = legacyOp<PPCInterpreter_FRSP>(); break;
            case 15: fn = legacyOp<PPCInterpreter_FCTIWZ>(); break;
            case 18: fn = legacyOp<PPCInterpreter_FDIV>(); break;
            case 20: fn = legacyOp<PPCInterpreter_FSUB>(); break;
            case 21: fn = legacyOp<PPCInterpreter_FADD>(); break;
            case 23: fn = legacyOp<PPCInterpreter_FSEL>(); break;
            case 25: fn = legacyOp<PPCInterpreter_FMUL>(); break;
            case 26: fn = legacyOp<PPCInterpreter_FRSQRTE>(); break;
            case 28: fn = legacyOp<PPCInterpreter_FMSUB>(); break;
            case 29: fn = legacyOp<PPCInterpreter_FMADD>(); break;
            case 30: fn = legacyOp<PPCInterpreter_FNMSUB>(); break;
            case 31: fn = legacyOp<PPCInterpreter_FNMADD>(); break;
            default:
                switch (PPC_getBits(opcode, 30, 10))
                {
                case 14: fn = legacyOp<PPCInterpreter_FCTIW>(); break;
                case 32: fn = legacyOp<PPCInterpreter_FCMPO>(); break;
                case 38: fn = legacyOp<PPCInterpreter_MTFSB1X>(); break;
                case 40: fn = legacyOp<PPCInterpreter_FNEG>(); break;
                case 72: fn = legacyOp<PPCInterpreter_FMR>(); break;
                case 136: fn = legacyOp<PPCInterpreter_FNABS>(); break;
                case 264: fn = legacyOp<PPCInterpreter_FABS>(); break;
                case 583: fn = legacyOp<PPCInterpreter_MFFS>(); break;
                case 711: fn = legacyOp<PPCInterpreter_MTFSF>(); break;
                default: break;
                }
                break;
            }
            break;
        case 1:
            fn = legacyOp<PPCInterpreter_virtualHLE>();
            term = PPCBlockTerm::Generic;
            break;
        default:
            break;
        }

        // anything we can't map (or explicitly routed to Generic without a handler) uses the old path -stossy11
        if (term == PPCBlockTerm::None && fn.legacy == nullptr)
            term = PPCBlockTerm::Generic;
        if (term == PPCBlockTerm::Generic && fn.legacy == nullptr)
            fn = legacyOp<blockFallbackStep>();
        return fn;
    }
    
    static PPCBlockFn fastHandler(uint32 opcode, uint32 addr, PPCBlockEntry& en)
    {
        const uint32 rD = (opcode >> 21) & 31, rA = (opcode >> 16) & 31, rB = (opcode >> 11) & 31;
        const uint32 simm = (uint32)(sint32)(sint16)(opcode & 0xFFFF);
        const uint32 uimm = opcode & 0xFFFF;
        const uint8 rc = opcode & 1;
        en.d = (uint8)rD; en.a = (uint8)rA; en.b = (uint8)rB; en.x = rc;
        switch (opcode >> 26)
        {
        case 7: en.imm = simm; return blkMULLI;
        case 8: en.imm = simm; return blkSUBFIC;
        case 10: en.d = (uint8)(rD >> 2); en.imm = uimm; return blkCMPLI;
        case 11: en.d = (uint8)(rD >> 2); en.imm = simm; return blkCMPI;
        case 12: en.imm = simm; en.x = 0; return blkADDIC;
        case 13: en.imm = simm; en.x = 1; return blkADDIC;
        case 14: en.imm = simm; return rA ? blkADDI : blkLI;
        case 15: en.imm = opcode << 16; return rA ? blkADDI : blkLI;
        case 16:
        {
            const uint32 BD = (uint32)(sint32)(sint16)(opcode & 0xFFFC);
            if (opcode & PPC_OPC_AA)
                return nullptr;
            en.imm = BD + addr;
            en.d = (uint8)rD; en.a = (uint8)rA; en.x = (opcode & PPC_OPC_LK) ? 1 : 0;
            en.b = 0; // PPCBLOCK_BRANCH_* flags
            return blkBCX;
        }
        case 18:
        {
            uint32 li;
            PPC_OPC_TEMPL_I(opcode, li);
            if ((opcode & PPC_OPC_AA) == 0)
                li += addr;
            en.imm = li;
            en.x = (opcode & PPC_OPC_LK) ? 1 : 0;
            en.b = 0; // PPCBLOCK_BRANCH_* flags
            return blkBX;
        }
        case 19:
        {
            const uint32 ext = PPC_getBits(opcode, 30, 10);
            en.x = (opcode & PPC_OPC_LK) ? 1 : 0;
            if (ext == 16)
                return blkBCLRX;
            if (ext == 528)
                return blkBCCTR;
            return nullptr;
        }
        case 20:
        {
            const uint32 MB = (opcode >> 6) & 31, ME = (opcode >> 1) & 31;
            en.imm = ppc_mask((int)MB, (int)ME);
            return blkRLWIMI;
        }
        case 21:
        case 23:
        {
            const uint32 MB = (opcode >> 6) & 31, ME = (opcode >> 1) & 31;
            en.imm = ppc_mask((int)MB, (int)ME);
            return (opcode >> 26) == 23 ? blkRLWNM : blkRLWINM;
        }
        case 24: en.imm = uimm; return blkORI;
        case 25: en.imm = uimm << 16; return blkORI;
        case 26: en.imm = uimm; return blkXORI;
        case 27: en.imm = uimm << 16; return blkXORI;
        case 28: en.imm = uimm; return blkANDI_;
        case 29: en.imm = uimm << 16; return blkANDI_;
        case 31:
            switch (PPC_getBits(opcode, 30, 10))
            {
            case 0: en.d = (uint8)(rD >> 2); return blkCMP;
            case 32: en.d = (uint8)(rD >> 2); return blkCMPL;
            case 266: return blkADD;
            // OE variants retain the legacy handlers, including their SO/OV updates.
            case 10: return blkCarry<false, false>; // addc
            case 138: return blkCarry<false, true>; // adde
            case 202: return blkCarry<false, true, 1>; // addze
            case 234: return blkCarry<false, true, 2>; // addme
            case 8: return blkCarry<true, false>; // subfc
            case 136: return blkCarry<true, true>; // subfe
            case 200: return blkCarry<true, true, 1>; // subfze
            case 232: return blkCarry<true, true, 2>; // subfme
            case 40: return blkSUBF;
            case 104: return blkNEG;
            case 235: return blkMULLW;
            case 75: return blkMULHW;
            case 11: return blkMULHWU;
            case 444: return blkOR;
            case 28: return blkAND;
            case 316: return blkXOR;
            case 60: return blkANDC;
            case 124: return blkNOR;
            case 954: return blkEXTSB;
            case 922: return blkEXTSH;
            case 26: return blkCNTLZW;
            case 24: return blkSLW;
            case 536: return blkSRW;
            case 824: return blkSRAWI;
            case 792: return blkSRAW;
            case 339:
            {
                const uint32 spr = rA | (rB << 5);
                if (spr == SPR_LR) return blkMFLR;
                if (spr == SPR_CTR) return blkMFCTR;
                return nullptr;
            }
            case 467:
            {
                const uint32 spr = rA | (rB << 5);
                if (spr == SPR_LR) return blkMTLR;
                if (spr == SPR_CTR) return blkMTCTR;
                return nullptr;
            }
            case 23: return blkLWZX;
            case 87: return blkLBZX;
            case 279: return blkLHZX;
            case 343: return blkLHAX;
            case 151: return blkSTWX;
            case 215: return blkSTBX;
            case 407: return blkSTHX;
            default: return nullptr;
            }
        case 32: en.imm = simm; return rA ? blkLWZ : blkLWZA;
        case 33: en.imm = simm; return blkLWZU;
        case 34: en.imm = simm; return rA ? blkLBZ : blkLBZA;
        case 35: en.imm = simm; return blkLBZU;
        case 36: en.imm = simm; return rA ? blkSTW : blkNOP;
        case 37: en.imm = simm; return blkSTWU;
        case 38: en.imm = simm; return rA ? blkSTB : blkSTBA;
        case 39: en.imm = simm; return blkSTBU;
        case 40: en.imm = simm; return rA ? blkLHZ : blkLHZA;
        case 42: en.imm = simm; return rA ? blkLHA : blkLHAA;
        case 44: en.imm = simm; return rA ? blkSTH : blkSTHA;
        case 56: case 57: return decodePSQ<false, false>(opcode, en);
        case 60: case 61: return decodePSQ<true, false>(opcode, en);
        case 48: en.imm = simm; return blkLFS;
        case 50: en.imm = simm; return blkLFD;
        case 52: en.imm = simm; return blkSTFS;
        case 54: en.imm = simm; return blkSTFD;
        case 4:
            if (((opcode >> 1) & 31) == 6) return decodePSQ<false, true>(opcode, en);
            if (((opcode >> 1) & 31) == 7) return decodePSQ<true, true>(opcode, en);
            en.x = (uint8)((opcode >> 6) & 31);
            switch ((opcode >> 1) & 31)
            {
            case 8:
                switch ((opcode >> 1) & 0x3FF)
                {
                case 40: return blkPS_NEG;
                case 72: return blkPS_MR;
                default: return nullptr;
                }
            case 12: return blkPS_MULS0;
            case 13: return blkPS_MULS1;
            case 14: return blkPS_MADDS0;
            case 15: return blkPS_MADDS1;
            case 16:
                switch ((opcode >> 1) & 0x3FF)
                {
                case 528: return blkPS_MERGE00;
                case 560: return blkPS_MERGE01;
                case 592: return blkPS_MERGE10;
                case 624: return blkPS_MERGE11;
                default: return nullptr;
                }
            case 20: return blkPS_SUB;
            case 21: return blkPS_ADD;
            case 25: return blkPS_MUL;
            case 28: return blkPS_MSUB;
            case 29: return blkPS_MADD;
            default: return nullptr;
            }
        case 59:
            en.x = (uint8)((opcode >> 6) & 31);
            switch ((opcode >> 1) & 31)
            {
            case 18: return blkFDIVS;
            case 20: return blkFSUBS;
            case 21: return blkFADDS;
            case 25: return blkFMULS;
            case 28: return blkFMSUBS;
            case 29: return blkFMADDS;
            default: return nullptr;
            }
        case 63:
            en.x = (uint8)((opcode >> 6) & 31);
            switch ((opcode >> 1) & 31)
            {
            case 18: return blkFDIV;
            case 20: return blkFSUB;
            case 21: return blkFADD;
            case 25: return blkFMUL;
            case 28: return blkFMSUB;
            case 29: return blkFMADD;
            default:
                switch ((opcode >> 1) & 0x3FF)
                {
                case 40: return blkFNEG;
                case 72: return blkFMR;
                default: return nullptr;
                }
            }
        default:
            return nullptr;
        }
    }

    static PPCBlockEntry decodeEntry(uint32 opcode, uint32 addr, PPCBlockTerm& term)
    {
        PPCBlockEntry en{};
        const PPCLegacyOp legacy = legacyHandler(opcode, term);
        if (term != PPCBlockTerm::Generic)
        {
            if (PPCBlockFn fast = fastHandler(opcode, addr, en))
            {
                en.fn = fast;
                return en;
            }
        }
        if (term == PPCBlockTerm::BCX || term == PPCBlockTerm::BX ||
            term == PPCBlockTerm::BCLRX || term == PPCBlockTerm::BCCTR)
        {
            term = PPCBlockTerm::Generic;
            en.imm = opcode;
            en.fn = blkGenericTerm<blockFallbackStep>;
            return en;
        }
        en.imm = opcode;
        en.fn = term == PPCBlockTerm::Generic ? legacy.terminator : legacy.direct;
        return en;
    }

    // registers and CR fields an instruction reads and writes, for isIdleLoop
    struct PPCIdleUses
    {
        uint32 read = 0;
        uint32 write = 0;
        uint8 crRead = 0;
        uint8 crWrite = 0;
    };
    
    
    static bool idleLoopUses(uint32 opcode, PPCIdleUses& uses)
    {
        const uint32 rD = (opcode >> 21) & 31, rA = (opcode >> 16) & 31, rB = (opcode >> 11) & 31;
        const uint8 cr0IfRc = (opcode & 1) ? 1 : 0;
        const uint32 baseA = rA ? (1u << rA) : 0; // rA == 0 means a literal 0 as base
        switch (opcode >> 26)
        {
        case 14: case 15: // addi, addis
            uses = { baseA, 1u << rD, 0, 0 };
            return true;
        case 24: case 25: case 26: case 27: // ori, oris, xori, xoris
            uses = { 1u << rD, 1u << rA, 0, 0 };
            return true;
        case 28: case 29: // andi., andis.
            uses = { 1u << rD, 1u << rA, 0, 1 };
            return true;
        case 21: // rlwinm
            uses = { 1u << rD, 1u << rA, 0, cr0IfRc };
            return true;
        case 10: case 11: // cmpli, cmpi
            uses = { 1u << rA, 0, 0, (uint8)(1u << (rD >> 2)) };
            return true;
        case 32: case 34: case 40: case 42: // lwz, lbz, lhz, lha
            uses = { baseA, 1u << rD, 0, 0 };
            return true;
        case 31:
            switch ((opcode >> 1) & 0x3FF)
            {
            case 0: case 32: // cmp, cmpl
                uses = { (1u << rA) | (1u << rB), 0, 0, (uint8)(1u << (rD >> 2)) };
                return true;
            case 266: case 40: // add, subf
                uses = { (1u << rA) | (1u << rB), 1u << rD, 0, cr0IfRc };
                return true;
            case 28: case 444: case 316: case 60: case 124: case 24: case 536: // and, or, xor, andc, nor, slw, srw
                uses = { (1u << rD) | (1u << rB), 1u << rA, 0, cr0IfRc };
                return true;
            case 954: case 922: case 26: // extsb, extsh, cntlzw
                uses = { 1u << rD, 1u << rA, 0, cr0IfRc };
                return true;
            case 23: case 87: case 279: case 343: // lwzx, lbzx, lhzx, lhax
                uses = { baseA | (1u << rB), 1u << rD, 0, 0 };
                return true;
            case 20: // lwarx, also sets the reservation to (ea, loaded value), which every iteration redoes identically
                // as long as the address registers are loop invariant (checked like any other read). This makes
                // spinlock waits (lwarx / cmpwi / bne back to the lwarx) idle loops.
                uses = { baseA | (1u << rB), 1u << rD, 0, 0 };
                return true;
            default:
                return false;
            }
        default:
            return false;
        }
    }
    
    
    static bool isIdleLoop(const PPCBlockEntry* entries, const uint32* opcodes, uint32 count)
    {
        PPCIdleUses uses[PPCBLOCK_MAX_LENGTH];
        uint32 writtenInLoop = 0;
        uint8 crWrittenInLoop = 0;
        for (uint32 i = 0; i + 1 < count; i++)
        {
            if (!idleLoopUses(opcodes[i], uses[i]))
                return false;
            writtenInLoop |= uses[i].write;
            crWrittenInLoop |= uses[i].crWrite;
        }
        const PPCBlockEntry& branch = entries[count - 1];
        uses[count - 1] = {};
        if (branch.fn == blkBCX && (branch.d & 0x10) == 0)
            uses[count - 1].crRead = (uint8)(1u << (branch.a >> 2));

        uint32 written = 0;
        uint8 crWritten = 0;
        for (uint32 i = 0; i < count; i++)
        {
            if ((uses[i].read & writtenInLoop & ~written) || (uses[i].crRead & crWrittenInLoop & ~crWritten))
                return false; // carried over from the previous iteration
            written |= uses[i].write;
            crWritten |= uses[i].crWrite;
        }
        return true;
    }

    // Decode time rewrites of a block that ends with a fast b / bc, entries are the block's instructions
    static void optimizeBlockEnd(PPCBlockEntry* entries, const uint32* opcodes, uint32 count, uint32 startAddr)
    {
        PPCBlockEntry& branch = entries[count - 1];
        if (branch.fn != blkBCX && branch.fn != blkBX)
            return;

        // bc must not touch CTR (BO & 4) and must test a condition (BO & 16 clear) on lt / gt / eq of the field
        // the compare right before it writes
        if (branch.fn == blkBCX && count >= 2 && (branch.d & 0x14) == 0x04 && (branch.a & 3) != 3)
        {
            PPCBlockEntry& cmp = entries[count - 2];
            if (cmp.d == (branch.a >> 2))
            {
                if (cmp.fn == blkCMPI)
                    cmp.fn = blkCmpBC<true, true>;
                else if (cmp.fn == blkCMPLI)
                    cmp.fn = blkCmpBC<false, true>;
                else if (cmp.fn == blkCMP)
                    cmp.fn = blkCmpBC<true, false>;
                else if (cmp.fn == blkCMPL)
                    cmp.fn = blkCmpBC<false, false>;
            }
        }

        const bool selfLoop = branch.imm == startAddr && !branch.x && (branch.fn == blkBX || (branch.d & 0x04));
        if (selfLoop && isIdleLoop(entries, opcodes, count))
            branch.b |= PPCBLOCK_BRANCH_IDLE;
    }

    static void optimizeBlockPairs(PPCBlockEntry* entries, uint32 count)
    {
#if !defined(PPC_INTERPRETER_DISABLE_FUSION)
        for (uint32 i = 0; i + 1 < count; i++)
        {
            PPCBlockEntry& first = entries[i];
            const PPCBlockEntry& second = entries[i + 1];
            if (first.fn == blkLI && first.d == second.d && second.fn == blkORI)
                first.fn = blkLI_Pair<true>;
            else if (first.fn == blkLI && first.d == second.a && second.fn == blkADDI)
                first.fn = blkLI_Pair<false>;
            else if (first.fn == blkADDI && first.d == second.a && second.fn == blkLWZ)
                first.fn = blkADDI_Mem<false>;
            else if (first.fn == blkADDI && first.d == second.a && second.fn == blkSTW)
                first.fn = blkADDI_Mem<true>;
            else
                continue;
            ++i;
        }
#endif
    }

    static uint32 hashBlockCode(PPCInterpreter_t* hCPU, uint32 startAddr, uint32 count)
    {
        uint32 h = 0;
        for (uint32 i = 0; i < count; i++)
            h = PPCBlockCache_hashStep(h, ppcItpCtrl::memory_readCodeU32(hCPU, startAddr + i * 4));
        return h;
    }

    // decodes the block at startAddr into scratch (header first), returns the number of entries written
    static uint32 decodeBlockEntries(PPCInterpreter_t* hCPU, uint32 startAddr, PPCBlockEntry (&scratch)[PPCBLOCK_MAX_LENGTH + 2], uint32 (&opcodes)[PPCBLOCK_MAX_LENGTH], uint32& countOut, PPCBlockTerm& termOut, uint32 maxInstructions = PPCBLOCK_MAX_LENGTH)
    {
        PPCBlockCache_markCodePage(startAddr);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const uint32 epoch = s_codeEpoch.load(std::memory_order_acquire);

        uint32 addr = startAddr;
        uint32 count = 0;
        uint32 hash = 0;
        PPCBlockTerm term = PPCBlockTerm::None;
        while (count < std::min(maxInstructions, PPCBLOCK_MAX_LENGTH))
        {
            if ((addr & 0xFFF) == 0 && addr != startAddr)
            {
                PPCBlockCache_markCodePage(addr);
                std::atomic_thread_fence(std::memory_order_seq_cst);
            }
            const uint32 opcode = ppcItpCtrl::memory_readCodeU32(hCPU, addr);
            hash = PPCBlockCache_hashStep(hash, opcode);
            opcodes[count] = opcode;
            scratch[1 + count++] = decodeEntry(opcode, addr, term);
            addr += 4;
            if (term != PPCBlockTerm::None)
                break;
            if (addr == 0)
                break;
        }
        if (term == PPCBlockTerm::BCX || term == PPCBlockTerm::BX)
            optimizeBlockEnd(scratch + 1, opcodes, count, startAddr);
        optimizeBlockPairs(scratch + 1, count);
        uint32 stored = 1 + count;
        if (term == PPCBlockTerm::None)
        {
            PPCBlockEntry& end = scratch[stored++];
            end = PPCBlockEntry{};
            end.fn = blkEnd;
        }
        PPCBlockEntry& header = scratch[0];
        header = PPCBlockEntry{};
        header.hash = hash;
        header.epoch = epoch;

        countOut = count;
        termOut = term;
        return stored;
    }
    
    static uint32 buildPredecodedSuffix(const PPCBlockEntry* srcHeader, uint32 srcCount, PPCBlockTerm term, uint32 skip, PPCBlockEntry* dst)
    {
        const uint32 count = srcCount - skip;
        const uint32 stored = 1 + count + (term == PPCBlockTerm::None ? 1 : 0);
        if (skip == 0)
        {
            memcpy(dst, srcHeader, stored * sizeof(PPCBlockEntry));
            return stored;
        }
        memcpy(dst + 1, srcHeader + 1 + skip, (stored - 1) * sizeof(PPCBlockEntry));
        
        const uint32* original = s_predecodeOpcodes
            + (srcHeader - s_predecodePool.load(std::memory_order_relaxed)) + 1 + skip;
        uint32 hash = 0;
        for (uint32 i = 0; i < count; i++)
            hash = PPCBlockCache_hashStep(hash, original[i]);
        dst[0] = PPCBlockEntry{};
        dst[0].hash = hash;
        dst[0].epoch = srcHeader->epoch; // epoch of the pre-decode
        return stored;
    }

    // copies the pre-decoded block covering ip into the runtime pool, starting at ip's instruction
    static PPCBlockDesc importPredecodedBlock(PPCBlockDesc predecoded, uint32 ip)
    {
        const uint32 generation = s_blockCacheGeneration.load(std::memory_order_relaxed);
        const uint32 skip = (ip - PPCBlockDesc_startAddr(predecoded)) / 4;
        const uint32 count = PPCBlockDesc_count(predecoded) - skip;
        const PPCBlockTerm term = PPCBlockDesc_term(predecoded);
        const uint32 stored = 1 + count + (term == PPCBlockTerm::None ? 1 : 0);

        const uint32 headerEntry = s_blockPoolHead.fetch_add(stored, std::memory_order_relaxed);
        if (headerEntry >= s_blockPoolEntries || stored > s_blockPoolEntries - headerEntry) [[unlikely]]
        {
            PPCBlockCache_onFull("pool full", generation);
            return 0;
        }
        const PPCBlockEntry* src = PPCBlockDesc_header(s_predecodePool.load(std::memory_order_relaxed), predecoded);
        buildPredecodedSuffix(src, PPCBlockDesc_count(predecoded), term, skip, s_blockPool + headerEntry);
        return PPCBlockDesc_pack(ip, headerEntry + 1, count, term);
    }

    static PPCBlockDesc decodeBlock(PPCInterpreter_t* hCPU, uint32 startAddr)
    {
        const uint32 generation = s_blockCacheGeneration.load(std::memory_order_relaxed);
        if (PPCBlockCache_tableFull()) [[unlikely]]
        {
            PPCBlockCache_onFull("table full", generation);
            return 0;
        }

        PPCBlockEntry scratch[PPCBLOCK_MAX_LENGTH + 2];
        uint32 opcodes[PPCBLOCK_MAX_LENGTH];
        uint32 count;
        PPCBlockTerm term;
        const uint32 stored = decodeBlockEntries(hCPU, startAddr, scratch, opcodes, count, term);

        const uint32 headerEntry = s_blockPoolHead.fetch_add(stored, std::memory_order_relaxed);
        if (headerEntry >= s_blockPoolEntries || stored > s_blockPoolEntries - headerEntry) [[unlikely]]
        {
            PPCBlockCache_onFull("pool full", generation);
            return 0;
        }
        memcpy(s_blockPool + headerEntry, scratch, stored * sizeof(PPCBlockEntry));
        s_blockDecodes.fetch_add(1, std::memory_order_relaxed);

        return PPCBlockDesc_pack(startAddr, headerEntry + 1, count, term);
    }

    
    static bool verifyBlock(PPCInterpreter_t* hCPU, PPCBlockDesc desc, PPCBlockEntry* header)
    {
        const uint32 epoch = s_codeEpoch.load(std::memory_order_acquire);
        auto verifiedRef = PPCBlockHeader_verifiedEpoch(header);
        const uint32 verified = verifiedRef.load(std::memory_order_relaxed);
        if (verified == epoch)
            return true;

        const uint32 start = PPCBlockDesc_startAddr(desc);
        const uint32 count = PPCBlockDesc_count(desc);
        const uint32 end = start + (count - 1) * 4;
        const bool untouched = verified >= s_codeEpochFloor.load(std::memory_order_relaxed)
            && s_codePageEpoch[start >> 12].load(std::memory_order_relaxed) <= verified
            && s_codePageEpoch[end >> 12].load(std::memory_order_relaxed) <= verified;
        if (!untouched && hashBlockCode(hCPU, start, count) != header->hash)
            return false;

        verifiedRef.store(epoch, std::memory_order_relaxed);
        return true;
    }
    
    static void predecodeGeneratedRange(uint32 start, uint32 size)
    {
        const uint32 offset = start - MEMORY_CODE_TRAMPOLINE_AREA_ADDR;
        if ((start & 3) || (size & 3) || size == 0 ||
            offset >= MEMORY_CODE_TRAMPOLINE_AREA_SIZE || size > MEMORY_CODE_TRAMPOLINE_AREA_SIZE - offset ||
            !memory_isAddressRangeAccessible(start, size))
            return;

        PPCBlockCache_ensureAllocated();
        std::lock_guard<std::mutex> lock(s_predecodeMutex);
        
        for (uint32 i = 0; i < size / 4; i++)
            s_predecodedTrampolines[offset / 4 + i].store(0, std::memory_order_release);
        PPCBlockEntry* pool = s_predecodePool.load(std::memory_order_relaxed);
        if (!pool)
        {
            pool = new PPCBlockEntry[PPCBLOCK_PREDECODE_POOL_ENTRIES];
            s_predecodeOpcodes = new uint32[PPCBLOCK_PREDECODE_POOL_ENTRIES];
            s_predecodePool.store(pool, std::memory_order_release);
        }

        PPCBlockEntry scratch[PPCBLOCK_MAX_LENGTH + 2];
        uint32 opcodes[PPCBLOCK_MAX_LENGTH];
        for (uint32 i = 0; i < size / 4;)
        {
            uint32 count;
            PPCBlockTerm term;
            const uint32 addr = start + i * 4;
            const uint32 stored = decodeBlockEntries(nullptr, addr, scratch, opcodes, count, term, size / 4 - i);
            if (stored > PPCBLOCK_PREDECODE_POOL_ENTRIES - s_predecodePoolHead)
                return; // Runtime decoding remains available when the pool is full.
            const uint32 headerEntry = s_predecodePoolHead;
            memcpy(pool + headerEntry, scratch, stored * sizeof(PPCBlockEntry));
            std::copy_n(opcodes, count, s_predecodeOpcodes + headerEntry + 1);
            s_predecodePoolHead += stored;
            s_predecodedTrampolines[offset / 4 + i].store(
                PPCBlockDesc_pack(addr, headerEntry + 1, count, term), std::memory_order_release);
            i += count;
        }
    }

    static void predecodeRange(uint32 rangeStart, uint32 rangeSize)
    {
        const uint32 start = (rangeStart + 3) & ~3u;
        if (rangeSize <= start - rangeStart)
            return;
        const uint32 numWords = (rangeSize - (start - rangeStart)) / 4;
        if (numWords == 0)
            return;
        if (!memory_isAddressRangeAccessible(start, numWords * 4))
            return;

        PPCBlockCache_ensureAllocated();

        std::vector<bool> leaders(numWords + 1, false);
        leaders[0] = true;
        auto markTarget = [&](uint32 target) {
            const uint32 offset = target - start; // wraps for targets below start
            if ((target & 3) == 0 && offset < numWords * 4)
                leaders[offset / 4] = true;
        };
        for (uint32 i = 0; i < numWords; i++)
        {
            const uint32 addr = start + i * 4;
            const uint32 opcode = ppcItpCtrl::memory_readCodeU32(nullptr, addr);
            switch (opcode >> 26)
            {
            case 16: // bc
            {
                const uint32 bd = (uint32)(sint32)(sint16)(opcode & 0xFFFC);
                markTarget((opcode & 2) ? bd : addr + bd);
                break;
            }
            case 18: // b
            {
                uint32 li = opcode & 0x03FFFFFC;
                if (li & 0x02000000)
                    li |= 0xFC000000;
                markTarget((opcode & 2) ? li : addr + li);
                break;
            }
            default:
                break;
            }
        }

        std::lock_guard<std::mutex> lock(s_predecodeMutex);
        const uint32 rangeIndex = s_predecodedRangeCount.load(std::memory_order_relaxed);
        if (rangeIndex >= PPCBLOCK_PREDECODE_MAX_RANGES)
            return; // the range decodes lazily
        PPCBlockEntry* pool = s_predecodePool.load(std::memory_order_relaxed);
        if (!pool)
        {
            pool = new PPCBlockEntry[PPCBLOCK_PREDECODE_POOL_ENTRIES];
            s_predecodeOpcodes = new uint32[PPCBLOCK_PREDECODE_POOL_ENTRIES];
            s_predecodePool.store(pool, std::memory_order_release);
        }

        std::vector<PPCBlockDesc> descs;
        PPCBlockEntry scratch[PPCBLOCK_MAX_LENGTH + 2];
        uint32 opcodes[PPCBLOCK_MAX_LENGTH];
        for (uint32 i = 0; i < numWords; i++)
        {
            if (!leaders[i])
                continue;
            const uint32 addr = start + i * 4;
            if (ppcItpCtrl::memory_readCodeU32(nullptr, addr) == 0)
            {
                // padding, the next word may be where code resumes
                leaders[i + 1] = true;
                continue;
            }

            uint32 count;
            PPCBlockTerm term;
            // never read past the end of the range, what follows may be data or unmapped
            const uint32 stored = decodeBlockEntries(nullptr, addr, scratch, opcodes, count, term, numWords - i);
            
            if (stored > PPCBLOCK_PREDECODE_POOL_ENTRIES - s_predecodePoolHead)
                break; // pool full, the rest decodes lazily
            
            const uint32 headerEntry = s_predecodePoolHead;
            memcpy(pool + headerEntry, scratch, stored * sizeof(PPCBlockEntry));
            std::copy_n(opcodes, count, s_predecodeOpcodes + headerEntry + 1);
            s_predecodePoolHead += stored;
            descs.push_back(PPCBlockDesc_pack(addr, headerEntry + 1, count, term));

            leaders[i + count] = true; // i + count <= numWords, leaders has room for the end
        }

        if (!descs.empty())
        {
            PPCBlockDesc* published = new PPCBlockDesc[descs.size()];
            std::copy(descs.begin(), descs.end(), published);
            PPCPredecodedRange& range = s_predecodedRanges[rangeIndex];
            range.start = start;
            range.end = start + numWords * 4;
            range.descs = published;
            range.count = (uint32)descs.size();
            s_predecodedRangeCount.store(rangeIndex + 1, std::memory_order_release);
        }
    }

    static void executeTimesliceCached(PPCInterpreter_t* hCPU)
    {
        PPCBlockThreadState* ts = PPCBlockCache_getThreadState();
        if (!ts) [[unlikely]]
        {
            executeInstruction<true>(hCPU);
            return;
        }

        PPCBlockSlot* table = s_blockTable;
        PPCBlockEntry* pool = s_blockPool;
        uint8* const mem = memory_base;
        PPCBlockSlot* prev = &ts->linkScratch;
        uint32 prevFallthrough = 0; // address right after prev, picks which of its links a newly found successor goes into
        uint32 myGeneration = 0;
        PPCBlockCtx ctx{};

        while (hCPU->remainingCycles > 0)
        {

            const uint32 gen = s_blockCacheGeneration.load(std::memory_order_relaxed);
            if (gen != myGeneration) [[unlikely]]
            {
                myGeneration = PPCBlockCache_synchronize(*ts);
                if (ts->targetGeneration != myGeneration)
                {
                    std::memset(ts->targetSlots, 0, sizeof(ts->targetSlots));
                    ts->targetGeneration = myGeneration;
                }
                // the cache may have been resized
                table = s_blockTable;
                pool = s_blockPool;
                prev = &ts->linkScratch;
                prevFallthrough = 0;
            }

            const uint32 ip = (uint32)hCPU->instructionPointer;
            const uint32 key = PPCBlockDesc_keyForAddr(ip);

            const uint32 link = ip == prevFallthrough ? PPCBLOCK_LINK_FALLTHROUGH : PPCBLOCK_LINK_TAKEN;
            PPCBlockSlot* slot = table + prev->nextSlot[link].load(std::memory_order_relaxed);
            PPCBlockDesc desc = slot->desc.load(std::memory_order_acquire);
            if (PPCBlockDesc_key(desc) != key) [[unlikely]]
            {
                slot = PPCBlockCache_targetLookup(table, ts->targetSlots, ip);
                if (!slot)
                    slot = PPCBlockCache_lookup(ip);
                if (!slot)
                {
                    PPCBlockDesc decoded = 0;
                    if (PPCBlockCache_tableFull()) [[unlikely]]
                        PPCBlockCache_onFull("table full", myGeneration);
                    else if (const PPCBlockDesc predecoded = PPCPredecode_lookup(ip))
                        decoded = importPredecodedBlock(predecoded, ip);
                    else
                        decoded = decodeBlock(hCPU, ip);
                    slot = decoded ? PPCBlockCache_publish(ip, decoded) : nullptr;
                }

                desc = slot ? slot->desc.load(std::memory_order_acquire) : 0;

                if (PPCBlockDesc_key(desc) != key) [[unlikely]]
                {
                    executeInstruction<false>(hCPU);
                    hCPU->remainingCycles--;
                    continue;
                }

                prev->nextSlot[link].store((uint32)(slot - table), std::memory_order_relaxed);
                PPCBlockCache_targetRemember(table, ts->targetSlots, ip, slot);
            }

            PPCBlockEntry* header = PPCBlockDesc_header(pool, desc);
            if (PPCBlockHeader_verifiedEpoch(header).load(std::memory_order_relaxed) != s_codeEpoch.load(std::memory_order_relaxed)) [[unlikely]]
            {
                if (!verifyBlock(hCPU, desc, header))
                {
                    const PPCBlockDesc decoded = decodeBlock(hCPU, ip);
                    if (decoded && slot->desc.compare_exchange_strong(desc, decoded, std::memory_order_release, std::memory_order_acquire))
                    {
                        s_blockRedecodes.fetch_add(1, std::memory_order_relaxed);
                        desc = decoded;
                    }
                    else
                    {
                        executeInstruction<false>(hCPU);
                        hCPU->remainingCycles--;
                        continue;
                    }
                }
            }


            const uint32 count = PPCBlockDesc_count(desc);

            hCPU->remainingCycles -= (sint32)count;

            ctx.targetSlots = ts->targetSlots;
            ctx.table = table;
            ctx.pool = pool;
            ctx.firstEntry = pool + PPCBlockDesc_firstEntry(desc);
            ctx.slot = slot;
            ctx.blockStart = ip;
            ctx.blockCount = count;
            ctx.generation = myGeneration;
            ctx.genericExit = false;
            
            const PPCBlockEntry* e = pool + PPCBlockDesc_firstEntry(desc);
#if PPC_BLOCK_MUSTTAIL
            e->fn(hCPU, e, mem, &ctx, hCPU->gpr[1]);
#else
            while (e)
                e = e->fn(hCPU, e, mem, &ctx, hCPU->gpr[1]);
#endif

            if (ctx.idleSkipAddr) [[unlikely]]
            {
                PPCBlockCache_noteIdleSkip(ctx.idleSkipAddr);
                ctx.idleSkipAddr = 0;
            }

            if (ctx.genericExit) [[unlikely]]
            {
                // The terminator left the cache before running the classic path.
                myGeneration = 0;
                continue;
            }

            // carry on from the last block of the chain, a miss there links its successor
            prev = ctx.slot;
            prevFallthrough = ctx.blockStart + ctx.blockCount * 4;
        }

        ts->ack.store(0, std::memory_order_release);
    }

#undef PPC_BLOCK_OP
#undef PPC_BLOCK_RET
#undef PPC_BLOCK_ADVANCE
#undef PPC_BLOCK_NEXT
#undef PPC_BLOCK_EXIT

};

#undef PPC_PRIMARY_CASE
#undef PPC_PRIMARY_DEFAULT
#undef PPC_DISPATCH_NEXT

void PPCInterpreterSlim_executeTimeslice(PPCInterpreter_t* hCPU)
{
#if PPC_INTERPRETER_BLOCK_CACHE
    PPCInterpreterContainer<PPCItpCafeOSUsermode>::executeTimesliceCached(hCPU);
#else
    PPCInterpreterContainer<PPCItpCafeOSUsermode>::executeInstruction<true>(hCPU);
#endif
}

void PPCInterpreter_predecodeRange(uint32 addr, uint32 size)
{
#if PPC_INTERPRETER_BLOCK_CACHE
    PPCInterpreterContainer<PPCItpCafeOSUsermode>::predecodeRange(addr, size);
#endif
}

void PPCInterpreter_predecodeGeneratedRange(uint32 addr, uint32 size)
{
#if PPC_INTERPRETER_BLOCK_CACHE
    PPCInterpreterContainer<PPCItpCafeOSUsermode>::predecodeGeneratedRange(addr, size);
#endif
}

// Slim interpreter, trades some features for extra performance
// Used when emulator runs in CafeOS HLE mode
// Assumes the following:
// - No MMU (linear memory with 1:1 mapping of physical to virtual)
// - No interrupts
// - Always runs in user mode
// - Paired single mode is always enabled
void PPCInterpreterSlim_executeInstruction(PPCInterpreter_t* hCPU)
{
    PPCInterpreterContainer<PPCItpCafeOSUsermode>::executeInstruction(hCPU);
}

// Full interpreter, supports most PowerPC features
// Used when emulator runs in LLE mode
void PPCInterpreterFull_executeInstruction(PPCInterpreter_t* hCPU)
{
    PPCInterpreterContainer<PPCItpSupervisorWithMMU>::executeInstruction(hCPU);
}
