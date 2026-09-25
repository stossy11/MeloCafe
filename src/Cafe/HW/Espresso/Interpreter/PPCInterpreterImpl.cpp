#include "PPCInterpreterInternal.h"
#include <array>
#include <bit>
#include <cstring>
#include <atomic>
#include <vector>
#include <algorithm>
#include <mutex>
#include <thread>
#include "PPCInterpreterHelper.h"
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

struct PPCBlockEntry
{
    PPCBlockHandler fn;
    uint32 opcode;
    uint32 _pad;
};
static_assert(sizeof(PPCBlockEntry) == 16);

using PPCBlockDesc = uint64;

static constexpr uint32 PPCBLOCK_TABLE_BITS = 18;
static constexpr uint32 PPCBLOCK_TABLE_SIZE = 1u << PPCBLOCK_TABLE_BITS;
static constexpr uint32 PPCBLOCK_TABLE_MAX_USED = PPCBLOCK_TABLE_SIZE * 3 / 4;
static constexpr uint32 PPCBLOCK_MAX_LENGTH = 64;
static constexpr uint32 PPCBLOCK_POOL_ENTRIES = 1u << 22;

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
static inline uint32 PPCBlockDesc_firstEntry(PPCBlockDesc d) { return (uint32)(d >> 11) & 0x3FFFFF; }
static inline uint32 PPCBlockDesc_count(PPCBlockDesc d) { return ((uint32)(d >> 5) & 0x3F) + 1; }
static inline PPCBlockTerm PPCBlockDesc_term(PPCBlockDesc d) { return (PPCBlockTerm)((uint32)(d >> 2) & 7); }

struct PPCBlockSlot
{
    std::atomic<PPCBlockDesc> desc;
    std::atomic<uint32> nextSlot;
    uint32 _pad;
};
static_assert(sizeof(PPCBlockSlot) == 16);

struct alignas(64) PPCBlockThreadState
{
    std::atomic<uint32> ack;
    PPCBlockSlot linkScratch;
};

static PPCBlockSlot* s_blockTable = nullptr;
static PPCBlockEntry* s_blockPool = nullptr;
static std::atomic<bool> s_blockCacheReady{false};
static std::atomic<uint32> s_blockTableUsed{0};
static std::atomic<uint32> s_blockPoolHead{0};
static std::atomic<uint64> s_blockDecodes{0};
static std::atomic<uint32> s_blockResets{0};

static std::atomic<uint32> s_blockCacheGeneration{2};

static constexpr uint32 PPCBLOCK_MAX_THREADS = 16;
static PPCBlockThreadState s_blockThreadStates[PPCBLOCK_MAX_THREADS];
static std::atomic<uint32> s_blockThreadCount{0};
static std::mutex s_blockCacheMutex;

static std::atomic<uint32> s_blockCacheCodePages[(1u << 20) / 32]; // 4 GiB / 4 KiB pages, 1 bit each

static inline void PPCBlockCache_markCodePage(uint32 addr)
{
    const uint32 page = addr >> 12;
    const uint32 bit = 1u << (page & 31);
    std::atomic<uint32>& word = s_blockCacheCodePages[page >> 5];
    
    if ((word.load(std::memory_order_relaxed) & bit) == 0)
        word.fetch_or(bit, std::memory_order_relaxed);
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

    s_blockTable = new PPCBlockSlot[PPCBLOCK_TABLE_SIZE]{};
    s_blockPool = new PPCBlockEntry[PPCBLOCK_POOL_ENTRIES];
    s_blockCacheReady.store(true, std::memory_order_release);
}

static thread_local PPCBlockThreadState* t_blockThreadState = nullptr;
static thread_local bool t_blockThreadStateResolved = false;

TLS_WORKAROUND_NOINLINE static PPCBlockThreadState* PPCBlockCache_getThreadState()
{
    if (!t_blockThreadStateResolved)
    {
        t_blockThreadStateResolved = true;
        PPCBlockCache_ensureAllocated();

        const uint32 index = s_blockThreadCount.fetch_add(1, std::memory_order_relaxed);
        if (index < PPCBLOCK_MAX_THREADS)
            t_blockThreadState = &s_blockThreadStates[index];
        else
            cemuLog_log(LogType::Force, "PPC block cache: more than {} interpreter threads, this one falls back to uncached dispatch", PPCBLOCK_MAX_THREADS);
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


static void PPCBlockCache_reset(const char* reason, uint32 expectedGeneration = 0)
{
    if (PPCBlockThreadState* self = t_blockThreadState)
        self->ack.store(0, std::memory_order_release);

    std::lock_guard<std::mutex> lock(s_blockCacheMutex);

    if (!s_blockTable)
        return;
    if (expectedGeneration != 0 && s_blockCacheGeneration.load(std::memory_order_relaxed) != expectedGeneration)
        return;

    const uint32 gen = s_blockCacheGeneration.load(std::memory_order_relaxed);
    const uint32 parked = gen | 1;
    s_blockCacheGeneration.store(parked, std::memory_order_release);

    const uint32 threadCount = std::min<uint32>(s_blockThreadCount.load(std::memory_order_relaxed), PPCBLOCK_MAX_THREADS);
    for (uint32 i = 0; i < threadCount; i++)
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
                cemuLog_log(LogType::Force, "PPC block cache: still waiting for core {} to reach a block boundary", i);
        }
    }

    for (auto& w : s_blockCacheCodePages)
        w.store(0, std::memory_order_relaxed);
    
    for (uint32 i = 0; i < PPCBLOCK_TABLE_SIZE; i++)
    {
        s_blockTable[i].desc.store(0, std::memory_order_relaxed);
        s_blockTable[i].nextSlot.store(0, std::memory_order_relaxed);
    }
    s_blockTableUsed.store(0, std::memory_order_relaxed);
    s_blockPoolHead.store(0, std::memory_order_relaxed);

    const uint32 resets = s_blockResets.fetch_add(1, std::memory_order_relaxed) + 1;
    if (resets <= 16 || (resets & 0xFF) == 0)
        cemuLog_log(LogType::Force, "PPC block cache: reset #{} ({}), {} blocks decoded since start", resets, reason, s_blockDecodes.load(std::memory_order_relaxed));

    s_blockCacheGeneration.store(parked + 1, std::memory_order_release);
}

static inline uint32 PPCBlockCache_hashSlot(uint32 addr)
{
    return ((addr >> 2) * 0x9E3779B1u) >> (32 - PPCBLOCK_TABLE_BITS);
}

static PPCBlockSlot* PPCBlockCache_lookup(uint32 addr)
{
    const uint32 key = PPCBlockDesc_keyForAddr(addr);
    uint32 i = PPCBlockCache_hashSlot(addr);
    for (uint32 probe = 0; probe < PPCBLOCK_TABLE_SIZE; probe++)
    {
        PPCBlockSlot& slot = s_blockTable[i];
        const PPCBlockDesc desc = slot.desc.load(std::memory_order_acquire);
        if (desc == 0)
            return nullptr;
        if (PPCBlockDesc_key(desc) == key)
            return &slot;
        i = (i + 1) & (PPCBLOCK_TABLE_SIZE - 1);
    }
    return nullptr;
}

static PPCBlockSlot* PPCBlockCache_publish(uint32 addr, PPCBlockDesc desc)
{
    const uint32 key = PPCBlockDesc_key(desc);
    uint32 i = PPCBlockCache_hashSlot(addr);
    for (uint32 probe = 0; probe < PPCBLOCK_TABLE_SIZE; probe++)
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
        i = (i + 1) & (PPCBLOCK_TABLE_SIZE - 1);
    }
    return nullptr;
}

void PPCInterpreter_invalidateBlockCache()
{
    PPCBlockCache_reset("code invalidated");
}

void PPCInterpreter_invalidateBlockCacheRange(uint32 addr, uint32 size)
{
    const uint32 firstPage = addr >> 12;
    const uint32 lastPage = (addr + (size ? size - 1 : 0)) >> 12;
    for (uint32 page = firstPage; page <= lastPage; page++)
    {
        if (PPCBlockCache_isCodePage(page << 12))
        {
            PPCInterpreter_invalidateBlockCache();
            return;
        }
        if (page == 0xFFFFF)
            break;
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
    
    static constexpr std::array<PPCBlockHandler, 1024> makeHandlerTable19()
    {
        std::array<PPCBlockHandler, 1024> result{};
        result[0] = PPCInterpreter_MCRF;
        result[16] = PPCInterpreter_BCLRX;
        result[33] = PPCInterpreter_CRNOR;
        result[50] = PPCInterpreter_RFI;
        result[129] = PPCInterpreter_CRANDC;
        result[150] = PPCInterpreter_ISYNC;
        result[193] = PPCInterpreter_CRXOR;
        result[225] = PPCInterpreter_CRNAND;
        result[257] = PPCInterpreter_CRAND;
        result[289] = PPCInterpreter_CREQV;
        result[417] = PPCInterpreter_CRORC;
        result[449] = PPCInterpreter_CROR;
        result[528] = PPCInterpreter_BCCTR;
        return result;
    }
    
    
    static constexpr std::array<PPCBlockHandler, 1024> makeHandlerTable31()
    {
        std::array<PPCBlockHandler, 1024> result{};
        result[0] = PPCInterpreter_CMP;
        result[4] = PPCInterpreter_TW;
        result[8] = PPCInterpreter_SUBFC;
        result[10] = PPCInterpreter_ADDC;
        result[11] = PPCInterpreter_MULHWU_;
        result[19] = PPCInterpreter_MFCR;
        result[20] = PPCInterpreter_LWARX;
        result[23] = PPCInterpreter_LWZX;
        result[24] = PPCInterpreter_SLWX;
        result[26] = PPCInterpreter_CNTLZW;
        result[28] = PPCInterpreter_ANDX;
        result[32] = PPCInterpreter_CMPL;
        result[40] = PPCInterpreter_SUBF;
        result[54] = PPCInterpreter_DCBST;
        result[55] = PPCInterpreter_LWZXU;
        result[60] = PPCInterpreter_ANDCX;
        result[75] = PPCInterpreter_MULHW_;
        result[83] = PPCInterpreter_MFMSR;
        result[86] = PPCInterpreter_DCBF;
        result[87] = PPCInterpreter_LBZX;
        result[104] = PPCInterpreter_NEG;
        result[119] = PPCInterpreter_LBZXU; // Sonic Lost World
        result[124] = PPCInterpreter_NORX;
        result[136] = PPCInterpreter_SUBFE;
        result[138] = PPCInterpreter_ADDE;
        result[144] = PPCInterpreter_MTCRF;
        result[146] = PPCInterpreter_MTMSR;
        result[150] = PPCInterpreter_STWCX;
        result[151] = PPCInterpreter_STWX;
        result[183] = PPCInterpreter_STWUX;
        result[200] = PPCInterpreter_SUBFZE;
        result[202] = PPCInterpreter_ADDZE;
        result[210] = PPCInterpreter_MTSR;
        result[215] = PPCInterpreter_STBX;
        result[232] = PPCInterpreter_SUBFME; // Trine 2
        result[234] = PPCInterpreter_ADDME;
        result[235] = PPCInterpreter_MULLW;
        result[247] = PPCInterpreter_STBUX;
        result[266] = PPCInterpreter_ADD;
        result[278] = PPCInterpreter_DCBT;
        result[279] = PPCInterpreter_LHZX;
        result[284] = PPCInterpreter_EQV;
        result[306] = PPCInterpreter_TLBIE;
        result[311] = PPCInterpreter_LHZUX; // Wii U Menu v177 (US)
        result[316] = PPCInterpreter_XOR;
        result[339] = PPCInterpreter_MFSPR;
        result[343] = PPCInterpreter_LHAX;
        result[371] = PPCInterpreter_MFTB;
        result[375] = PPCInterpreter_LHAUX; // Wii U Menu v177 (US)
        result[407] = PPCInterpreter_STHX;
        result[412] = PPCInterpreter_ORC;
        result[439] = PPCInterpreter_STHUX;
        result[444] = PPCInterpreter_OR;
        result[459] = PPCInterpreter_DIVWU;
        result[467] = PPCInterpreter_MTSPR;
        result[470] = PPCInterpreter_DCBI;
        result[476] = PPCInterpreter_NANDX;
        result[491] = PPCInterpreter_DIVW;
        result[512] = PPCInterpreter_MCRXR;
        result[520] = PPCInterpreter_SUBFCO; // Affordable Space Adventures + other Unity games
        result[522] = PPCInterpreter_ADDCO;
        result[523] = PPCInterpreter_MULHWU_; // 11 | OE; OE is ignored
        result[533] = PPCInterpreter_LSWX;
        result[534] = PPCInterpreter_LWBRX;
        result[535] = PPCInterpreter_LFSX;
        result[536] = PPCInterpreter_SRWX;
        result[552] = PPCInterpreter_SUBFO;
        result[566] = PPCInterpreter_TLBSYNC;
        result[567] = PPCInterpreter_LFSUX;
        result[587] = PPCInterpreter_MULHW_; // 75 | OE; OE is ignored for MULHW
        result[595] = PPCInterpreter_MFSR;
        result[597] = PPCInterpreter_LSWI;
        result[598] = PPCInterpreter_SYNC;
        result[599] = PPCInterpreter_LFDX;
        result[616] = PPCInterpreter_NEGO;
        result[631] = PPCInterpreter_LFDUX;
        result[648] = PPCInterpreter_SUBFEO; // 136 | OE
        result[650] = PPCInterpreter_ADDEO; // 138 | OE
        result[662] = PPCInterpreter_STWBRX;
        result[663] = PPCInterpreter_STFSX;
        result[661] = PPCInterpreter_STSWX;
        result[695] = PPCInterpreter_STFSUX;
        result[712] = PPCInterpreter_SUBFZEO; // 200 | OE
        result[714] = PPCInterpreter_ADDZEO; // 202 | OE
        result[725] = PPCInterpreter_STSWI;
        result[727] = PPCInterpreter_STFDX;
        result[744] = PPCInterpreter_SUBFMEO; // 232 | OE
        result[746] = PPCInterpreter_ADDMEO; // 234 | OE
        result[747] = PPCInterpreter_MULLWO;
        result[759] = PPCInterpreter_STFDUX;
        result[778] = PPCInterpreter_ADDO;
        result[790] = PPCInterpreter_LHBRX;
        result[792] = PPCInterpreter_SRAW;
        result[824] = PPCInterpreter_SRAWI;
        result[854] = PPCInterpreter_EIEIO;
        result[918] = PPCInterpreter_STHBRX;
        result[922] = PPCInterpreter_EXTSH;
        result[954] = PPCInterpreter_EXTSB;
        result[971] = PPCInterpreter_DIVWUO;
        result[982] = PPCInterpreter_ICBI;
        result[983] = PPCInterpreter_STFIWX;
        result[1003] = PPCInterpreter_DIVWO;
        result[1014] = PPCInterpreter_DCBZ;
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
                if (auto handler = handlers[PPC_getBits(opcode, 30, 10)])
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
                if (auto handler = handlers[PPC_getBits(opcode, 30, 10)])
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
    
    static PPCBlockEntry decodeEntry(uint32 opcode, PPCBlockTerm& term)
    {
        static constexpr auto s_table19 = makeHandlerTable19();
        static constexpr auto s_table31 = makeHandlerTable31();
        term = PPCBlockTerm::None;
        PPCBlockHandler fn = nullptr;
        switch (opcode >> 26)
        {
        case 4:
            switch (PPC_getBits(opcode, 30, 5))
            {
            case 0:
                switch (PPC_getBits(opcode, 25, 5))
                {
                case 0: fn = PPCInterpreter_PS_CMPU0; break;
                case 1: fn = PPCInterpreter_PS_CMPO0; break;
                case 2: fn = PPCInterpreter_PS_CMPU1; break;
                default: break;
                }
                break;
            case 6: fn = PPCInterpreter_PSQ_LX; break;
            case 7: fn = PPCInterpreter_PSQ_STX; break;
            case 8:
                switch (PPC_getBits(opcode, 25, 5))
                {
                case 1: fn = PPCInterpreter_PS_NEG; break;
                case 2: fn = PPCInterpreter_PS_MR; break;
                case 4: fn = PPCInterpreter_PS_NABS; break;
                case 8: fn = PPCInterpreter_PS_ABS; break;
                default: break;
                }
                break;
            case 10: fn = PPCInterpreter_PS_SUM0; break;
            case 11: fn = PPCInterpreter_PS_SUM1; break;
            case 12: fn = PPCInterpreter_PS_MULS0; break;
            case 13: fn = PPCInterpreter_PS_MULS1; break;
            case 14: fn = PPCInterpreter_PS_MADDS0; break;
            case 15: fn = PPCInterpreter_PS_MADDS1; break;
            case 16:
                switch (PPC_getBits(opcode, 25, 5))
                {
                case 16: fn = PPCInterpreter_PS_MERGE00; break;
                case 17: fn = PPCInterpreter_PS_MERGE01; break;
                case 18: fn = PPCInterpreter_PS_MERGE10; break;
                case 19: fn = PPCInterpreter_PS_MERGE11; break;
                default: break;
                }
                break;
            case 18: fn = PPCInterpreter_PS_DIV; break;
            case 20: fn = PPCInterpreter_PS_SUB; break;
            case 21: fn = PPCInterpreter_PS_ADD; break;
            case 22: fn = PPCInterpreter_DCBZL; break;
            case 23: fn = PPCInterpreter_PS_SEL; break;
            case 24: fn = PPCInterpreter_PS_RES; break;
            case 25: fn = PPCInterpreter_PS_MUL; break;
            case 26: fn = PPCInterpreter_PS_RSQRTE; break;
            case 28: fn = PPCInterpreter_PS_MSUB; break;
            case 29: fn = PPCInterpreter_PS_MADD; break;
            case 30: fn = PPCInterpreter_PS_NMSUB; break;
            case 31: fn = PPCInterpreter_PS_NMADD; break;
            default: break;
            }
            break;
        case 7: fn = PPCInterpreter_MULLI; break;
        case 8: fn = PPCInterpreter_SUBFIC; break;
        case 10: fn = PPCInterpreter_CMPLI; break;
        case 11: fn = PPCInterpreter_CMPI; break;
        case 12: fn = PPCInterpreter_ADDIC; break;
        case 13: fn = PPCInterpreter_ADDIC_; break;
        case 14: fn = PPCInterpreter_ADDI; break;
        case 15: fn = PPCInterpreter_ADDIS; break;
        case 16: term = PPCBlockTerm::BCX; fn = blockTermBCX; break;
        case 18: term = PPCBlockTerm::BX; fn = blockTermBX; break;
        case 19:
        {
            const uint32 ext = PPC_getBits(opcode, 30, 10);
            if (ext == 16)
            {
                term = PPCBlockTerm::BCLRX;
                fn = blockTermBCLRX;
            }
            else if (ext == 528)
            {
                term = PPCBlockTerm::BCCTR;
                fn = blockTermBCCTR;
            }
            else if (ext == 50 || ext == 150) // rfi, isync > classic path, terminate
                term = PPCBlockTerm::Generic;
            else
                fn = s_table19[ext]; // cr ops, mcrf: straight-line
            break;
        }
        case 20: fn = PPCInterpreter_RLWIMI; break;
        case 21: fn = PPCInterpreter_RLWINM; break;
        case 23: fn = PPCInterpreter_RLWNM; break;
        case 24: fn = PPCInterpreter_ORI; break;
        case 25: fn = PPCInterpreter_ORIS; break;
        case 26: fn = PPCInterpreter_XORI; break;
        case 27: fn = PPCInterpreter_XORIS; break;
        case 28: fn = PPCInterpreter_ANDI_; break;
        case 29: fn = PPCInterpreter_ANDIS_; break;
        case 31:
        {
            const uint32 ext = PPC_getBits(opcode, 30, 10);
            if (ext == 982) // icbi
            {
                fn = blockICBI;
                term = PPCBlockTerm::Generic;
            }
            else if (ext == 4 || ext == 146) // tw, mtmsr -> classic path, terminate
                term = PPCBlockTerm::Generic;
            else
                fn = s_table31[ext];
            break;
        }
        case 32: fn = PPCInterpreter_LWZ; break;
        case 33: fn = PPCInterpreter_LWZU; break;
        case 34: fn = PPCInterpreter_LBZ; break;
        case 35: fn = PPCInterpreter_LBZU; break;
        case 36: fn = PPCInterpreter_STW; break;
        case 37: fn = PPCInterpreter_STWU; break;
        case 38: fn = PPCInterpreter_STB; break;
        case 39: fn = PPCInterpreter_STBU; break;
        case 40: fn = PPCInterpreter_LHZ; break;
        case 41: fn = PPCInterpreter_LHZU; break;
        case 42: fn = PPCInterpreter_LHA; break;
        case 43: fn = PPCInterpreter_LHAU; break;
        case 44: fn = PPCInterpreter_STH; break;
        case 45: fn = PPCInterpreter_STHU; break;
        case 46: fn = PPCInterpreter_LMW; break;
        case 47: fn = PPCInterpreter_STMW; break;
        case 48: fn = PPCInterpreter_LFS; break;
        case 49: fn = PPCInterpreter_LFSU; break;
        case 50: fn = PPCInterpreter_LFD; break;
        case 51: fn = PPCInterpreter_LFDU; break;
        case 52: fn = PPCInterpreter_STFS; break;
        case 53: fn = PPCInterpreter_STFSU; break;
        case 54: fn = PPCInterpreter_STFD; break;
        case 55: fn = PPCInterpreter_STFDU; break;
        case 56: fn = PPCInterpreter_PSQ_L; break;
        case 57: fn = PPCInterpreter_PSQ_LU; break;
        case 59:
            switch (PPC_getBits(opcode, 30, 5))
            {
            case 18: fn = PPCInterpreter_FDIVS; break;
            case 20: fn = PPCInterpreter_FSUBS; break;
            case 21: fn = PPCInterpreter_FADDS; break;
            case 24: fn = PPCInterpreter_FRES; break;
            case 25: fn = PPCInterpreter_FMULS; break;
            case 28: fn = PPCInterpreter_FMSUBS; break;
            case 29: fn = PPCInterpreter_FMADDS; break;
            case 30: fn = PPCInterpreter_FNMSUBS; break;
            case 31: fn = PPCInterpreter_FNMADDS; break;
            default: break;
            }
            break;
        case 60: fn = PPCInterpreter_PSQ_ST; break;
        case 61: fn = PPCInterpreter_PSQ_STU; break;
        case 63:
            switch (PPC_getBits(opcode, 30, 5))
            {
            case 0: fn = PPCInterpreter_FCMPU; break;
            case 12: fn = PPCInterpreter_FRSP; break;
            case 15: fn = PPCInterpreter_FCTIWZ; break;
            case 18: fn = PPCInterpreter_FDIV; break;
            case 20: fn = PPCInterpreter_FSUB; break;
            case 21: fn = PPCInterpreter_FADD; break;
            case 23: fn = PPCInterpreter_FSEL; break;
            case 25: fn = PPCInterpreter_FMUL; break;
            case 26: fn = PPCInterpreter_FRSQRTE; break;
            case 28: fn = PPCInterpreter_FMSUB; break;
            case 29: fn = PPCInterpreter_FMADD; break;
            case 30: fn = PPCInterpreter_FNMSUB; break;
            case 31: fn = PPCInterpreter_FNMADD; break;
            default:
                switch (PPC_getBits(opcode, 30, 10))
                {
                case 14: fn = PPCInterpreter_FCTIW; break;
                case 32: fn = PPCInterpreter_FCMPO; break;
                case 38: fn = PPCInterpreter_MTFSB1X; break;
                case 40: fn = PPCInterpreter_FNEG; break;
                case 72: fn = PPCInterpreter_FMR; break;
                case 136: fn = PPCInterpreter_FNABS; break;
                case 264: fn = PPCInterpreter_FABS; break;
                case 583: fn = PPCInterpreter_MFFS; break;
                case 711: fn = PPCInterpreter_MTFSF; break;
                default: break;
                }
                break;
            }
            break;
        case 1:
            fn = PPCInterpreter_virtualHLE;
            term = PPCBlockTerm::Generic;
            break;
        default:
            break;
        }

        // anything we can't map (or explicitly routed to Generic without a handler) uses the old path -stossy11
        if (term == PPCBlockTerm::None && fn == nullptr)
            term = PPCBlockTerm::Generic;
        if (term == PPCBlockTerm::Generic && fn == nullptr)
            fn = blockFallbackStep;
        return PPCBlockEntry{fn, opcode, 0};
    }
    
    static PPCBlockSlot* decodeBlock(PPCInterpreter_t* hCPU, uint32 startAddr)
    {
        const uint32 generation = s_blockCacheGeneration.load(std::memory_order_relaxed);
        if (s_blockTableUsed.load(std::memory_order_relaxed) >= PPCBLOCK_TABLE_MAX_USED) [[unlikely]]
        {
            PPCBlockCache_reset("table full", generation);
            return nullptr;
        }
        
        PPCBlockEntry scratch[PPCBLOCK_MAX_LENGTH];

        PPCBlockCache_markCodePage(startAddr);

        uint32 addr = startAddr;
        uint32 count = 0;
        PPCBlockTerm term = PPCBlockTerm::None;
        while (count < PPCBLOCK_MAX_LENGTH)
        {
            if ((addr & 0xFFF) == 0 && addr != startAddr)
                PPCBlockCache_markCodePage(addr);
            const uint32 opcode = ppcItpCtrl::memory_readCodeU32(hCPU, addr);
            scratch[count++] = decodeEntry(opcode, term);
            addr += 4;
            if (term != PPCBlockTerm::None)
                break;
            if (addr == 0) // wrapped around the address space
                break;
        }

        const uint32 firstEntry = s_blockPoolHead.fetch_add(count, std::memory_order_relaxed);
        if (firstEntry >= PPCBLOCK_POOL_ENTRIES || count > PPCBLOCK_POOL_ENTRIES - firstEntry) [[unlikely]]
        {
            PPCBlockCache_reset("pool full", generation);
            return nullptr;
        }
        memcpy(s_blockPool + firstEntry, scratch, count * sizeof(PPCBlockEntry));
        s_blockDecodes.fetch_add(1, std::memory_order_relaxed);

        return PPCBlockCache_publish(startAddr, PPCBlockDesc_pack(startAddr, firstEntry, count, term));
    }

    static inline void blockTermBX(PPCInterpreter_t* hCPU, uint32 opcode)
    {
        uint32 li;
        PPC_OPC_TEMPL_I(opcode, li);
        if ((opcode & PPC_OPC_AA) == 0)
            li += (uint32)hCPU->instructionPointer;
        if (opcode & PPC_OPC_LK)
            hCPU->spr.LR = (uint32)hCPU->instructionPointer + 4;
        hCPU->instructionPointer = li;
    }

    static inline void blockTermBCX(PPCInterpreter_t* hCPU, uint32 opcode)
    {
        uint32 BO, BI, BD;
        PPC_OPC_TEMPL_B(opcode, BO, BI, BD);
        if (!(BO & 4))
            hCPU->spr.CTR--;
        const bool bo2 = (BO & 2) != 0;
        const bool bo8 = (BO & 8) != 0;
        const bool cr = ppc_getCRBit(hCPU, BI) != 0;
        if (((BO & 4) || ((hCPU->spr.CTR != 0) ^ bo2))
            && ((BO & 16) || (!(cr ^ bo8))))
        {
            if (!(opcode & PPC_OPC_AA))
                BD += (uint32)hCPU->instructionPointer;
            else
                cemu_assert_unimplemented();
            if (opcode & PPC_OPC_LK)
                hCPU->spr.LR = (uint32)hCPU->instructionPointer + 4;
            hCPU->instructionPointer = BD;
        }
        else
            hCPU->instructionPointer += 4;
    }

    static inline void blockTermBCLRX(PPCInterpreter_t* hCPU, uint32 opcode)
    {
        uint32 BO, BI, BD;
        PPC_OPC_TEMPL_XL(opcode, BO, BI, BD);
        if (!(BO & 4))
            hCPU->spr.CTR--;
        const bool bo2 = (BO & 2) != 0;
        const bool bo8 = (BO & 8) != 0;
        const bool cr = ppc_getCRBit(hCPU, BI) != 0;
        if (((BO & 4) || ((hCPU->spr.CTR != 0) ^ bo2))
            && ((BO & 16) || (!(cr ^ bo8))))
        {
            const uint32 target = hCPU->spr.LR & 0xfffffffc;
            if (opcode & PPC_OPC_LK)
                hCPU->spr.LR = (uint32)hCPU->instructionPointer + 4;
            hCPU->instructionPointer = target;
        }
        else
            hCPU->instructionPointer += 4;
    }

    static inline void blockTermBCCTR(PPCInterpreter_t* hCPU, uint32 opcode)
    {
        uint32 BO, BI, BD;
        PPC_OPC_TEMPL_XL(opcode, BO, BI, BD);
        const bool bo8 = (BO & 8) != 0;
        const bool cr = ppc_getCRBit(hCPU, BI) != 0;
        if ((BO & 16) || (!(cr ^ bo8)))
        {
            if (opcode & PPC_OPC_LK)
                hCPU->spr.LR = (uint32)hCPU->instructionPointer + 4;
            hCPU->instructionPointer = hCPU->spr.CTR & 0xfffffffc;
        }
        else
            hCPU->instructionPointer += 4;
    }
    
    
    static void executeTimesliceCached(PPCInterpreter_t* hCPU)
    {
        PPCBlockThreadState* ts = PPCBlockCache_getThreadState();
        if (!ts) [[unlikely]]
        {
            executeInstruction<true>(hCPU);
            return;
        }

        PPCBlockSlot* const table = s_blockTable;
        const PPCBlockEntry* const pool = s_blockPool;
        PPCBlockSlot* prev = &ts->linkScratch;
        uint32 myGeneration = 0;

        while (hCPU->remainingCycles > 0)
        {
            
            const uint32 gen = s_blockCacheGeneration.load(std::memory_order_relaxed);
            if (gen != myGeneration) [[unlikely]]
            {
                myGeneration = PPCBlockCache_synchronize(*ts);
                prev = &ts->linkScratch;
            }
            
            const uint32 ip = (uint32)hCPU->instructionPointer;
            const uint32 key = PPCBlockDesc_keyForAddr(ip);
            
            
            PPCBlockSlot* slot = table + prev->nextSlot.load(std::memory_order_relaxed);
            PPCBlockDesc desc = slot->desc.load(std::memory_order_acquire);
            if (PPCBlockDesc_key(desc) != key) [[unlikely]]
            {
                slot = PPCBlockCache_lookup(ip);
                if (!slot)
                    slot = decodeBlock(hCPU, ip);
                
                desc = slot ? slot->desc.load(std::memory_order_acquire) : 0;
                
                if (PPCBlockDesc_key(desc) != key) [[unlikely]]
                {
                    executeInstruction<false>(hCPU);
                    hCPU->remainingCycles--;
                    continue;
                }
                
                prev->nextSlot.store((uint32)(slot - table), std::memory_order_relaxed);
            }
            
            
            const uint32 count = PPCBlockDesc_count(desc);
            const PPCBlockTerm term = PPCBlockDesc_term(desc);
            
            hCPU->remainingCycles -= (sint32)count;
            
            const PPCBlockEntry* e = pool + PPCBlockDesc_firstEntry(desc);
            const PPCBlockEntry* const last = e + (count - 1);
            for (; e != last; ++e)
                e->fn(hCPU, e->opcode);

            if (term == PPCBlockTerm::Generic) [[unlikely]]
            {
                ts->ack.store(0, std::memory_order_release);
                e->fn(hCPU, e->opcode);
                myGeneration = 0;
                continue;
            }

            e->fn(hCPU, e->opcode);

#ifdef CEMU_DEBUG_ASSERT
            if (term == PPCBlockTerm::None && hCPU->instructionPointer != ip + count * 4)
                assert_dbg();
#endif
            prev = slot;
        }

        ts->ack.store(0, std::memory_order_release);
    }

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
