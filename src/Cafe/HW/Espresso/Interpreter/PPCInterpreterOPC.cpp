#include "../PPCState.h"
#include "PPCInterpreterInternal.h"
#include "PPCInterpreterHelper.h"

#include "Cafe/OS/libs/coreinit/coreinit_CodeGen.h"

#include "../Recompiler/PPCRecompiler.h"

#include <float.h>
#include "Cafe/HW/Latte/Core/LatteBufferCache.h"

void PPCInterpreter_MFMSR(PPCInterpreter_t* hCPU, uint32 opcode)
{
	cemuLog_logDebug(LogType::Force, "Rare instruction: MFMSR");
	if (hCPU->sprExtended.msr & MSR_PR)
		return;
	const uint32 reg = (opcode >> 21) & 31;
	hCPU->gpr[reg] = hCPU->sprExtended.msr;
	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_writeMSR(PPCInterpreter_t* hCPU, uint32 reg)
{
	cemuLog_logDebug(LogType::Force, "Rare instruction: MTMSR");
	if (hCPU->sprExtended.msr & MSR_PR)
		return;
	hCPU->sprExtended.msr = hCPU->gpr[reg];
	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_MTMSR(PPCInterpreter_t* hCPU, uint32 opcode)
{
	PPCInterpreter_writeMSR(hCPU, (opcode >> 21) & 31);
}

void PPCInterpreter_setFPSCRBit(PPCInterpreter_t* hCPU, uint32 bit)
{
	cemuLog_logDebug(LogType::Force, "Rare instruction: MTFSB1X");
	if (bit != 1 && bit != 2)
		hCPU->fpscr |= 1u << (31 - bit);
}

void PPCInterpreter_MTFSB1X(PPCInterpreter_t* hCPU, uint32 opcode)
{
	const uint32 bit = (opcode >> 21) & 31;
	PPCInterpreter_setFPSCRBit(hCPU, bit);
	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_MCRF(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	uint32 crD, crS, b;
	PPC_OPC_TEMPL_X(Opcode, crD, crS, b);
	crD >>= 2;
	crS >>= 2;
	ppc_setCRField(hCPU, crD, ppc_getCRField(hCPU, crS));

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_MFCR(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	// frequently used by GCC compiled code (e.g. SM64 port)
	int rD, rA, rB;
	PPC_OPC_TEMPL_X(Opcode, rD, rA, rB);
	
	hCPU->gpr[rD] = hCPU->cr;
	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_MTCRF(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	// frequently used by GCC compiled code (e.g. SM64 port)
	// tested
	uint32 rS;
	uint32 crfMask;
	PPC_OPC_TEMPL_XFX(Opcode, rS, crfMask);

	const uint32 mask = ppc_MTCRFMaskToPackedMask(crfMask);
	hCPU->cr = (hCPU->cr & ~mask) | (hCPU->gpr[rS] & mask);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_moveXERToCR(PPCInterpreter_t* hCPU, uint32 cr)
{
	uint32 xer = PPCInterpreter_getXER(hCPU);
	uint32 xerBits = (xer >> 28) & 0xF;

	// Preserve the existing bit ordering: XER bit 28 becomes CR field bit 0.
	const uint32 crField = ((xerBits & 1) << 3) | ((xerBits & 2) << 1) |
		((xerBits & 4) >> 1) | ((xerBits & 8) >> 3);
	ppc_setCRField(hCPU, cr, crField);

	// reset copied bits
	PPCInterpreter_setXER(hCPU, xer&~0xF0000000);

}

void PPCInterpreter_MCRXR(PPCInterpreter_t* hCPU, uint32 opcode)
{
	const uint32 cr = ((opcode >> 23) & 7) >> 2;
	PPCInterpreter_moveXERToCR(hCPU, cr);
	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_TLBIE(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	int rS, rA, rB;
	PPC_OPC_TEMPL_X(Opcode, rS, rA, rB);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_TLBSYNC(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	cemu_assert_unimplemented();
	PPCInterpreter_nextInstruction(hCPU);
}

// branch instructions

void PPCInterpreter_BX(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	uint32 li;
	PPC_OPC_TEMPL_I(Opcode, li);
	if ((Opcode & PPC_OPC_AA) == 0)
		li += (unsigned int)hCPU->instructionPointer;
	if (Opcode & PPC_OPC_LK) 
	{
		// update LR and IP
		hCPU->spr.LR = (unsigned int)hCPU->instructionPointer + 4;
		hCPU->instructionPointer = li;
        PPCCore_attemptToEnterAddr(hCPU, li);
		return;
	}
	PPCInterpreter_jumpToInstruction(hCPU, li);
}


void PPCInterpreter_BCX(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	uint32 BO, BI, BD;
	PPC_OPC_TEMPL_B(Opcode, BO, BI, BD);
	if (!(BO & 4))
		hCPU->spr.CTR--;
	bool bo2 = (BO & 2) != 0;
	bool bo8 = (BO & 8) != 0; // branch condition true
	bool cr = ppc_getCRBit(hCPU, BI) != 0;
	if (((BO & 4) || ((hCPU->spr.CTR != 0) ^ bo2)) 
		&& ((BO & 16) || (!(cr ^ bo8)))) 
	{
		if (!(Opcode & PPC_OPC_AA))
		{
			BD += (unsigned int)hCPU->instructionPointer;
		}
		else
		{
			// should never happen
			cemu_assert_unimplemented();
		}
		if (Opcode & PPC_OPC_LK)
			hCPU->spr.LR = ((unsigned int)hCPU->instructionPointer) + 4;
		PPCInterpreter_jumpToInstruction(hCPU, BD);
	}
	else
		PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_BCLRX(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	uint32 BO, BI, BD;
	PPC_OPC_TEMPL_XL(Opcode, BO, BI, BD);
	PPC_ASSERT(BD == 0);
	if (!(BO & 4))
	{
		if (hCPU->spr.CTR == 0)
		{
			PPC_ASSERT(true);
			cemuLog_logDebug(LogType::Force, "Decrementer underflow!");
		}
		hCPU->spr.CTR--;
	}
	bool bo2 = (BO & 2) ? true : false;
	bool bo8 = (BO & 8) ? true : false;
	bool cr = ppc_getCRBit(hCPU, BI) != 0;
	if (((BO & 4) || ((hCPU->spr.CTR != 0) ^ bo2))
		&& ((BO & 16) || (!(cr ^ bo8))))
	{
		BD = hCPU->spr.LR & 0xfffffffc;
		if (Opcode & PPC_OPC_LK)
		{
			hCPU->spr.LR = (unsigned int)hCPU->instructionPointer + 4;
		}
        PPCCore_attemptToEnterAddr(hCPU, BD);
		return;
	}
	else
	{
		BD = (unsigned int)hCPU->instructionPointer + 4;
		PPCInterpreter_nextInstruction(hCPU);
	}
}

void PPCInterpreter_BCCTR(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	uint32 x = (unsigned int)hCPU->instructionPointer;
	uint32 BO, BI, BD;
	PPC_OPC_TEMPL_XL(Opcode, BO, BI, BD);
	PPC_ASSERT(BD == 0);
	PPC_ASSERT(!(BO & 2));
	bool bo8 = (BO & 8) ? true : false;
	bool cr = ppc_getCRBit(hCPU, BI) != 0;
	if ((BO & 16) || (!(cr ^ bo8)))
	{
		if (Opcode & PPC_OPC_LK)
		{
			hCPU->spr.LR = (unsigned int)hCPU->instructionPointer + 4;
			hCPU->instructionPointer = (unsigned int)(hCPU->spr.CTR & 0xfffffffc);
		}
		else
		{
			hCPU->instructionPointer = (unsigned int)(hCPU->spr.CTR & 0xfffffffc);
		}
		PPCRecompiler_attemptEnter(hCPU, hCPU->instructionPointer);
	}
	else
	{
		hCPU->instructionPointer += 4;
	}
}

void PPCInterpreter_DCBT(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rB;
	rA = (Opcode >> (31 - 15)) & 0x1F;
	rB = (Opcode >> (31 - 20)) & 0x1F;
	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_DCBST(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rB;
	rA = (Opcode >> (31 - 15)) & 0x1F;
	rB = (Opcode >> (31 - 20)) & 0x1F;

	uint32 ea = (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB];

	LatteBufferCache_notifyDCFlush(ea, 32);
	
	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_DCBF(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rB;
	rA = (Opcode >> (31 - 15)) & 0x1F;
	rB = (Opcode >> (31 - 20)) & 0x1F;

	uint32 ea = (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB];

	LatteBufferCache_notifyDCFlush(ea, 32);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_DCBZL(PPCInterpreter_t* hCPU, uint32 Opcode) //Undocumented
{
	// no-op
	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_DCBI(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	// no-op
	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_ICBI(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rD, rA, rB;
	PPC_OPC_TEMPL_X(Opcode, rD, rA, rB);
	uint32 ea = (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB];
	// invalidate range
	coreinit::codeGenHandleICBI(ea);
	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_EIEIO(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	// no effect
	// next instruction
	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_SC(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	cemuLog_logDebug(LogType::Force, "SC executed at 0x{:08x}", hCPU->instructionPointer);
	// next instruction
	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_SYNC(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	// no-op
	// next instruction
	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_ISYNC(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	// no-op
	// next instruction
	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_RFI(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	cemuLog_logDebug(LogType::Force, "RFI");
	hCPU->sprExtended.msr &= ~(0x87C0FF73 | 0x00040000);
	hCPU->sprExtended.msr |= hCPU->sprExtended.srr1 & 0x87c0ff73;
	hCPU->sprExtended.msr |= MSR_RI;
	hCPU->instructionPointer = (unsigned int)(hCPU->sprExtended.srr0);
}
