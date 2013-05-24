/*
	Copyright (C) 2008-2010 DeSmuME team

	This file is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.

	This file is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with the this software.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "JitCommon.h"
#include "MMU.h"
#include "armcpu.h"
#include "debug.h"
#ifdef _MSC_VER
#include <Windows.h>
#endif

#ifdef HAVE_JIT

#ifdef MAPPED_JIT_FUNCS
static uintptr_t *JIT_MEM[2][32] = {
	//arm9
	{
		/* 0X*/	DUP2(g_JitLut.ARM9_ITCM),
		/* 1X*/	DUP2(g_JitLut.ARM9_ITCM), // mirror
		/* 2X*/	DUP2(g_JitLut.MAIN_MEM),
		/* 3X*/	DUP2(g_JitLut.SWIRAM),
		/* 4X*/	DUP2(NULL),
		/* 5X*/	DUP2(NULL),
		/* 6X*/		 NULL, 
					 g_JitLut.ARM9_LCDC,	// Plain ARM9-CPU Access (LCDC mode) (max 656KB)
		/* 7X*/	DUP2(NULL),
		/* 8X*/	DUP2(NULL),
		/* 9X*/	DUP2(NULL),
		/* AX*/	DUP2(NULL),
		/* BX*/	DUP2(NULL),
		/* CX*/	DUP2(NULL),
		/* DX*/	DUP2(NULL),
		/* EX*/	DUP2(NULL),
		/* FX*/	DUP2(g_JitLut.ARM9_BIOS)
	},
	//arm7
	{
		/* 0X*/	DUP2(g_JitLut.ARM7_BIOS),
		/* 1X*/	DUP2(NULL),
		/* 2X*/	DUP2(g_JitLut.MAIN_MEM),
		/* 3X*/	     g_JitLut.SWIRAM,
		             g_JitLut.ARM7_ERAM,
		/* 4X*/	     NULL,
		             g_JitLut.ARM7_WIRAM,
		/* 5X*/	DUP2(NULL),
		/* 6X*/		 g_JitLut.ARM7_WRAM,		// VRAM allocated as Work RAM to ARM7 (max. 256K)
					 NULL,
		/* 7X*/	DUP2(NULL),
		/* 8X*/	DUP2(NULL),
		/* 9X*/	DUP2(NULL),
		/* AX*/	DUP2(NULL),
		/* BX*/	DUP2(NULL),
		/* CX*/	DUP2(NULL),
		/* DX*/	DUP2(NULL),
		/* EX*/	DUP2(NULL),
		/* FX*/	DUP2(NULL)
	}
};

static u32 JIT_MASK[2][32] = {
	//arm9
	{
		/* 0X*/	DUP2(0x00007FFF),
		/* 1X*/	DUP2(0x00007FFF),
		/* 2X*/	DUP2(0x003FFFFF), // FIXME _MMU_MAIN_MEM_MASK
		/* 3X*/	DUP2(0x00007FFF),
		/* 4X*/	DUP2(0x00000000),
		/* 5X*/	DUP2(0x00000000),
		/* 6X*/		 0x00000000,
					 0x000FFFFF,
		/* 7X*/	DUP2(0x00000000),
		/* 8X*/	DUP2(0x00000000),
		/* 9X*/	DUP2(0x00000000),
		/* AX*/	DUP2(0x00000000),
		/* BX*/	DUP2(0x00000000),
		/* CX*/	DUP2(0x00000000),
		/* DX*/	DUP2(0x00000000),
		/* EX*/	DUP2(0x00000000),
		/* FX*/	DUP2(0x00007FFF)
	},
	//arm7
	{
		/* 0X*/	DUP2(0x00003FFF),
		/* 1X*/	DUP2(0x00000000),
		/* 2X*/	DUP2(0x003FFFFF),
		/* 3X*/	     0x00007FFF,
		             0x0000FFFF,
		/* 4X*/	     0x00000000,
		             0x0000FFFF,
		/* 5X*/	DUP2(0x00000000),
		/* 6X*/		 0x0003FFFF,
					 0x00000000,
		/* 7X*/	DUP2(0x00000000),
		/* 8X*/	DUP2(0x00000000),
		/* 9X*/	DUP2(0x00000000),
		/* AX*/	DUP2(0x00000000),
		/* BX*/	DUP2(0x00000000),
		/* CX*/	DUP2(0x00000000),
		/* DX*/	DUP2(0x00000000),
		/* EX*/	DUP2(0x00000000),
		/* FX*/	DUP2(0x00000000)
	}
};

CACHE_ALIGN JitLut g_JitLut;
#else
DS_ALIGN(4096) uintptr_t g_CompiledFuncs[1<<26] = {0};
#endif

RegisterMap::~RegisterMap()
{
}

bool RegisterMap::Start(void *context, struct armcpu_t *armcpu)
{
	m_SwapData = 0;
	m_StateData = 0;
	m_Context = context;
	m_Cpu = armcpu;

	// check
#ifdef DEVELOPER
	for (u32 i = 0; i < GUESTREG_COUNT; i++)
	{
		if (m_State.GuestRegs[i].state != GuestReg::GRS_MEM)
			PROGINFO("RegisterMap::Start() : GuestRegId[%u] is bad(state)\n", i);

		if (m_State.GuestRegs[i].hostreg != INVALID_REG_ID)
			PROGINFO("RegisterMap::Start() : GuestRegId[%u] is bad(hostreg)\n", i);
	}

	for (u32 i = 0; i < m_HostRegCount; i++)
	{
		if (m_State.HostRegs[i].guestreg != INVALID_REG_ID)
			PROGINFO("RegisterMap::Start() : HostReg[%u] is bad(guestreg)\n", i);

		if (m_State.HostRegs[i].alloced)
			PROGINFO("RegisterMap::Start() : HostReg[%u] is bad(alloced)\n", i);

		if (m_State.HostRegs[i].locked)
			PROGINFO("RegisterMap::Start() : HostReg[%u] is bad(locked)\n", i);

		if (m_State.HostRegs[i].dirty)
			PROGINFO("RegisterMap::Start() : HostReg[%u] is bad(dirty)\n", i);
	}

	if (m_StateMap.size() > 0)
		PROGINFO("RegisterMap::Start() : StateMap is not empty\n");
#endif

	StartBlock();

	return true;
}

void RegisterMap::End(bool cleanup)
{
	UnlockAll();
	FlushAll(true);

	EndBlock();

	if (cleanup)
	{
		DiscardReg(EXECUTECYCLES, true);
		DiscardReg(CPUPTR, true);

		CleanAllStates();

		m_Context = NULL;
		m_Cpu = NULL;
	}
}

bool RegisterMap::IsImm(GuestRegId reg) const
{
	if (reg >= GUESTREG_COUNT)
	{
		PROGINFO("RegisterMap::IsImm() : GuestRegId[%u] invalid\n", (u32)reg);

		return false;
	}

	return m_State.GuestRegs[reg].state == GuestReg::GRS_IMM;
}

void RegisterMap::SetImm8(GuestRegId reg, u8 imm)
{
	if (reg >= GUESTREG_COUNT)
	{
		PROGINFO("RegisterMap::SetImm8() : GuestRegId[%u] invalid\n", (u32)reg);

		return;
	}

	if (m_State.GuestRegs[reg].state == GuestReg::GRS_MAPPED)
	{
		const u32 hostreg = m_State.GuestRegs[reg].hostreg;

		if (hostreg == INVALID_REG_ID || m_State.HostRegs[hostreg].guestreg != reg)
			PROGINFO("RegisterMap::SetImm8() : GuestRegId[%u] out of sync\n", (u32)reg);
		
		m_State.HostRegs[hostreg].guestreg = INVALID_REG_ID;
		m_State.HostRegs[hostreg].alloced = false;
		m_State.HostRegs[hostreg].dirty = false;
		m_State.HostRegs[hostreg].locked = 0;
	}

	m_State.GuestRegs[reg].state = GuestReg::GRS_IMM;
	m_State.GuestRegs[reg].hostreg = INVALID_REG_ID;
	m_State.GuestRegs[reg].immdata.type = ImmData::IMM8;
	m_State.GuestRegs[reg].immdata.imm8 = imm;
}

void RegisterMap::SetImm16(GuestRegId reg, u16 imm)
{
	if (reg >= GUESTREG_COUNT)
	{
		PROGINFO("RegisterMap::SetImm16() : GuestRegId[%u] invalid\n", (u32)reg);

		return;
	}

	if (m_State.GuestRegs[reg].state == GuestReg::GRS_MAPPED)
	{
		const u32 hostreg = m_State.GuestRegs[reg].hostreg;

		if (hostreg == INVALID_REG_ID || m_State.HostRegs[hostreg].guestreg != reg)
			PROGINFO("RegisterMap::SetImm16() : GuestRegId[%u] out of sync\n", (u32)reg);
		
		m_State.HostRegs[hostreg].guestreg = INVALID_REG_ID;
		m_State.HostRegs[hostreg].alloced = false;
		m_State.HostRegs[hostreg].dirty = false;
		m_State.HostRegs[hostreg].locked = 0;
	}

	m_State.GuestRegs[reg].state = GuestReg::GRS_IMM;
	m_State.GuestRegs[reg].hostreg = INVALID_REG_ID;
	m_State.GuestRegs[reg].immdata.type = ImmData::IMM16;
	m_State.GuestRegs[reg].immdata.imm16 = imm;
}

void RegisterMap::SetImm32(GuestRegId reg, u32 imm)
{
	if (reg >= GUESTREG_COUNT)
	{
		PROGINFO("RegisterMap::SetImm32() : GuestRegId[%u] invalid\n", (u32)reg);

		return;
	}

	if (m_State.GuestRegs[reg].state == GuestReg::GRS_MAPPED)
	{
		const u32 hostreg = m_State.GuestRegs[reg].hostreg;

		if (hostreg == INVALID_REG_ID || m_State.HostRegs[hostreg].guestreg != reg)
			PROGINFO("RegisterMap::SetImm32() : GuestRegId[%u] out of sync\n", (u32)reg);
		
		m_State.HostRegs[hostreg].guestreg = INVALID_REG_ID;
		m_State.HostRegs[hostreg].alloced = false;
		m_State.HostRegs[hostreg].dirty = false;
		m_State.HostRegs[hostreg].locked = 0;
	}

	m_State.GuestRegs[reg].state = GuestReg::GRS_IMM;
	m_State.GuestRegs[reg].hostreg = INVALID_REG_ID;
	m_State.GuestRegs[reg].immdata.type = ImmData::IMM32;
	m_State.GuestRegs[reg].immdata.imm32 = imm;
}

void RegisterMap::SetImmPtr(GuestRegId reg, void* imm)
{
	if (reg >= GUESTREG_COUNT)
	{
		PROGINFO("RegisterMap::SetImmPtr() : GuestRegId[%u] invalid\n", (u32)reg);

		return;
	}

	if (m_State.GuestRegs[reg].state == GuestReg::GRS_MAPPED)
	{
		const u32 hostreg = m_State.GuestRegs[reg].hostreg;

		if (hostreg == INVALID_REG_ID || m_State.HostRegs[hostreg].guestreg != reg)
			PROGINFO("RegisterMap::SetImmPtr() : GuestRegId[%u] out of sync\n", (u32)reg);
		
		m_State.HostRegs[hostreg].guestreg = INVALID_REG_ID;
		m_State.HostRegs[hostreg].alloced = false;
		m_State.HostRegs[hostreg].dirty = false;
		m_State.HostRegs[hostreg].locked = 0;
	}

	m_State.GuestRegs[reg].state = GuestReg::GRS_IMM;
	m_State.GuestRegs[reg].hostreg = INVALID_REG_ID;
	m_State.GuestRegs[reg].immdata.type = ImmData::IMMPTR;
	m_State.GuestRegs[reg].immdata.immptr = imm;
}

u8 RegisterMap::GetImm8(GuestRegId reg) const
{
	if (reg >= GUESTREG_COUNT)
	{
		PROGINFO("RegisterMap::GetImm8() : GuestRegId[%u] invalid\n", (u32)reg);

		return 0;
	}

	if (m_State.GuestRegs[reg].state != GuestReg::GRS_IMM)
	{
		PROGINFO("RegisterMap::GetImm8() : GuestRegId[%u] is non-imm register\n", (u32)reg);

		return 0;
	}

	if (m_State.GuestRegs[reg].immdata.type != ImmData::IMM8)
		PROGINFO("RegisterMap::GetImm8() : GuestRegId[%u] is not imm8\n", (u32)reg);

	return m_State.GuestRegs[reg].immdata.imm8;
}

u16 RegisterMap::GetImm16(GuestRegId reg) const
{
	if (reg >= GUESTREG_COUNT)
	{
		PROGINFO("RegisterMap::GetImm16() : GuestRegId[%u] invalid\n", (u32)reg);

		return 0;
	}

	if (m_State.GuestRegs[reg].state != GuestReg::GRS_IMM)
	{
		PROGINFO("RegisterMap::GetImm16() : GuestRegId[%u] is non-imm register\n", (u32)reg);

		return 0;
	}

	if (m_State.GuestRegs[reg].immdata.type != ImmData::IMM16)
		PROGINFO("RegisterMap::GetImm8() : GuestRegId[%u] is not imm16\n", (u32)reg);

	return m_State.GuestRegs[reg].immdata.imm16;
}

u32 RegisterMap::GetImm32(GuestRegId reg) const
{
	if (reg >= GUESTREG_COUNT)
	{
		PROGINFO("RegisterMap::GetImm32() : GuestRegId[%u] invalid\n", (u32)reg);

		return 0;
	}

	if (m_State.GuestRegs[reg].state != GuestReg::GRS_IMM)
	{
		PROGINFO("RegisterMap::GetImm32() : GuestRegId[%u] is non-imm register\n", (u32)reg);

		return 0;
	}

	if (m_State.GuestRegs[reg].immdata.type != ImmData::IMM32)
		PROGINFO("RegisterMap::GetImm32() : GuestRegId[%u] is not imm32\n", (u32)reg);

	return m_State.GuestRegs[reg].immdata.imm32;
}

void* RegisterMap::GetImmPtr(GuestRegId reg) const
{
	if (reg >= GUESTREG_COUNT)
	{
		PROGINFO("RegisterMap::GetImmPtr() : GuestRegId[%u] invalid\n", (u32)reg);

		return 0;
	}

	if (m_State.GuestRegs[reg].state != GuestReg::GRS_IMM)
	{
		PROGINFO("RegisterMap::GetImmPtr() : GuestRegId[%u] is non-imm register\n", (u32)reg);

		return 0;
	}

	if (m_State.GuestRegs[reg].immdata.type != ImmData::IMMPTR)
		PROGINFO("RegisterMap::GetImm32() : GuestRegId[%u] is not ptr\n", (u32)reg);

	return m_State.GuestRegs[reg].immdata.immptr;
}

u32 RegisterMap::MapReg(GuestRegId reg, u32 mapflag)
{
	if (reg >= GUESTREG_COUNT)
	{
		PROGINFO("RegisterMap::MapReg() : GuestRegId[%u] invalid\n", (u32)reg);

		return INVALID_REG_ID;
	}

	if (m_State.GuestRegs[reg].state == GuestReg::GRS_MAPPED)
	{
		const u32 hostreg = m_State.GuestRegs[reg].hostreg;

		if (hostreg == INVALID_REG_ID || m_State.HostRegs[hostreg].guestreg != reg)
			PROGINFO("RegisterMap::MapReg() : GuestRegId[%u] out of sync\n", (u32)reg);

		if (mapflag & MAP_DIRTY)
			m_State.HostRegs[hostreg].dirty = true;

		m_State.HostRegs[hostreg].swapdata = GenSwapData();

		return hostreg;
	}

	const u32 hostreg = AllocHostReg(false);
	if (hostreg == INVALID_REG_ID)
	{
		PROGINFO("RegisterMap::MapReg() : out of host registers\n");

		return INVALID_REG_ID;
	}

	m_State.HostRegs[hostreg].guestreg = reg;
	m_State.HostRegs[hostreg].dirty = (mapflag & MAP_DIRTY);
	m_State.HostRegs[hostreg].swapdata = GenSwapData();
	if (!(mapflag & MAP_NOTINIT))
	{
		if (m_State.GuestRegs[reg].state == GuestReg::GRS_MEM)
		{
			LoadGuestReg(hostreg, reg);
		}
		else if (m_State.GuestRegs[reg].state == GuestReg::GRS_IMM)
		{
			LoadImm(hostreg, m_State.GuestRegs[reg].immdata);

			m_State.HostRegs[hostreg].dirty = true;
		}
	}
	if (mapflag & MAP_DIRTY)
		m_State.HostRegs[hostreg].dirty = true;

	m_State.GuestRegs[reg].state = GuestReg::GRS_MAPPED;
	m_State.GuestRegs[reg].hostreg = hostreg;

	return hostreg;
}

u32 RegisterMap::MappedReg(GuestRegId reg)
{
	if (reg >= GUESTREG_COUNT)
	{
		PROGINFO("RegisterMap::MappedReg() : GuestRegId[%u] invalid\n", (u32)reg);

		return INVALID_REG_ID;
	}

	if (m_State.GuestRegs[reg].state != GuestReg::GRS_MAPPED)
	{
		PROGINFO("RegisterMap::MappedReg() : GuestRegId[%u] is not mapped\n", (u32)reg);

		return INVALID_REG_ID;
	}

	m_State.HostRegs[m_State.GuestRegs[reg].hostreg].swapdata = GenSwapData();

	return m_State.GuestRegs[reg].hostreg;
}

void RegisterMap::DiscardReg(GuestRegId reg, bool force)
{
	if (reg >= GUESTREG_COUNT)
	{
		PROGINFO("RegisterMap::DiscardReg() : GuestRegId[%u] invalid\n", (u32)reg);

		return;
	}

	if (m_State.GuestRegs[reg].state == GuestReg::GRS_MAPPED)
	{
		const u32 hostreg = m_State.GuestRegs[reg].hostreg;

		if (!force && m_State.HostRegs[hostreg].dirty)
		{
			PROGINFO("RegisterMap::DiscardReg() : GuestRegId[%u] is dirty\n", (u32)reg);

			return;
		}

		m_State.HostRegs[hostreg].guestreg = INVALID_REG_ID;
		m_State.HostRegs[hostreg].swapdata = 0;
		m_State.HostRegs[hostreg].alloced = false;
		m_State.HostRegs[hostreg].dirty = false;
		m_State.HostRegs[hostreg].locked = 0;
	}
	else if (m_State.GuestRegs[reg].state == GuestReg::GRS_IMM)
	{
		if (!force && reg != EXECUTECYCLES && reg != CPUPTR)
			PROGINFO("RegisterMap::DiscardReg() : GuestRegId[%u] is immediate\n", (u32)reg);
	}

	m_State.GuestRegs[reg].state = GuestReg::GRS_MEM;
	m_State.GuestRegs[reg].hostreg = INVALID_REG_ID;
}

u32 RegisterMap::AllocTempReg(bool preserved)
{
	const u32 hostreg = AllocHostReg(preserved);
	if (hostreg == INVALID_REG_ID)
	{
		PROGINFO("RegisterMap::AllocTempReg() : out of host registers\n");

		return INVALID_REG_ID;
	}

	Lock(hostreg);

	return hostreg;
}

void RegisterMap::ReleaseTempReg(u32 &reg)
{
	if (reg >= m_HostRegCount)
	{
		PROGINFO("RegisterMap::ReleaseTempReg() : HostReg[%u] invalid\n", (u32)reg);

		return;
	}

	if (!m_State.HostRegs[reg].alloced)
	{
		PROGINFO("RegisterMap::ReleaseTempReg() : HostReg[%u] is not alloced\n", (u32)reg);

		return;
	}

	if (m_State.HostRegs[reg].guestreg != INVALID_REG_ID)
	{
		PROGINFO("RegisterMap::ReleaseTempReg() : HostReg[%u] is not a temp reg\n", (u32)reg);

		return;
	}

	if (m_State.HostRegs[reg].locked > 1)
	{
		PROGINFO("RegisterMap::ReleaseTempReg() : HostReg[%u] is locked\n", (u32)reg);

		return;
	}

	Unlock(reg);

	FlushHostReg(reg);

	reg = INVALID_REG_ID;
}

void RegisterMap::Lock(u32 reg)
{
	if (reg >= m_HostRegCount)
	{
		PROGINFO("RegisterMap::Lock() : HostReg[%u] invalid\n", (u32)reg);

		return;
	}

	if (!m_State.HostRegs[reg].alloced)
	{
		PROGINFO("RegisterMap::Lock() : HostReg[%u] is not alloced\n", (u32)reg);

		return;
	}

	m_State.HostRegs[reg].locked++;
}

void RegisterMap::Unlock(u32 reg)
{
	if (reg >= m_HostRegCount)
	{
		PROGINFO("RegisterMap::Unlock() : HostReg[%u] invalid\n", (u32)reg);

		return;
	}

	if (!m_State.HostRegs[reg].alloced)
	{
		PROGINFO("RegisterMap::Unlock() : HostReg[%u] is not alloced\n", (u32)reg);

		return;
	}

	if (!m_State.HostRegs[reg].locked)
	{
		PROGINFO("RegisterMap::Unlock() : HostReg[%u] is not locked\n", (u32)reg);

		return;
	}

	m_State.HostRegs[reg].locked--;
}

void RegisterMap::UnlockAll()
{
	for (u32 i = 0; i < m_HostRegCount; i++)
	{
		if (m_State.HostRegs[i].alloced)
		{
			for (u32 j = 0; j < m_State.HostRegs[i].locked; j++)
				Unlock(i);
		}
	}
}

void RegisterMap::FlushGuestReg(GuestRegId reg)
{
	if (reg >= GUESTREG_COUNT)
	{
		PROGINFO("RegisterMap::FlushGuestReg() : GuestRegId[%u] invalid\n", (u32)reg);

		return;
	}

	if (m_State.GuestRegs[reg].state == GuestReg::GRS_MAPPED)
	{
		FlushHostReg(m_State.GuestRegs[reg].hostreg);
	}
	else if (m_State.GuestRegs[reg].state == GuestReg::GRS_IMM)
	{
		StoreImm(reg, m_State.GuestRegs[reg].immdata);
	}

	m_State.GuestRegs[reg].state = GuestReg::GRS_MEM;
	m_State.GuestRegs[reg].hostreg = INVALID_REG_ID;
}

void RegisterMap::FlushHostReg(u32 reg)
{
	if (reg >= m_HostRegCount)
	{
		PROGINFO("RegisterMap::FreeHostReg() : HostReg[%u] invalid\n", (u32)reg);

		return;
	}

	if (!m_State.HostRegs[reg].alloced)
	{
		PROGINFO("RegisterMap::FreeHostReg() : HostReg[%u] is not alloced\n", (u32)reg);

		return;
	}

	if (m_State.HostRegs[reg].locked)
	{
		PROGINFO("RegisterMap::FreeHostReg() : HostReg[%u] is locked\n", (u32)reg);

		return;
	}

	if (m_State.HostRegs[reg].guestreg == INVALID_REG_ID)
	{
		//m_State.HostRegs[reg].guestreg = INVALID_REG_ID;
		m_State.HostRegs[reg].swapdata = 0;
		m_State.HostRegs[reg].alloced = false;
		m_State.HostRegs[reg].dirty = false;
		m_State.HostRegs[reg].locked = 0;
	}
	else
	{
		const GuestRegId guestreg = (GuestRegId)m_State.HostRegs[reg].guestreg;

		if (m_State.GuestRegs[guestreg].state != GuestReg::GRS_MAPPED || m_State.GuestRegs[guestreg].hostreg != reg)
			PROGINFO("RegisterMap::FlushHostReg() : HostReg[%u] out of sync\n", (u32)reg);

		if (m_State.HostRegs[reg].dirty)
			StoreGuestReg(reg, guestreg);

		m_State.HostRegs[reg].guestreg = INVALID_REG_ID;
		m_State.HostRegs[reg].swapdata = 0;
		m_State.HostRegs[reg].alloced = false;
		m_State.HostRegs[reg].dirty = false;
		m_State.HostRegs[reg].locked = 0;

		m_State.GuestRegs[guestreg].state = GuestReg::GRS_MEM;
		m_State.GuestRegs[guestreg].hostreg = INVALID_REG_ID;
	}
}

void RegisterMap::FlushAll(bool onlyreal)
{
	if (onlyreal)
	{
		for (u32 i = R0; i <= SPSR; i++)
		{
			FlushGuestReg((GuestRegId)i);
		}
	}
	else
	{
		for (u32 i = 0; i < GUESTREG_COUNT; i++)
		{
			FlushGuestReg((GuestRegId)i);
		}

		for (u32 i = 0; i < m_HostRegCount; i++)
		{
			if (m_State.HostRegs[i].alloced)
				FlushHostReg(i);
		}
	}
}

u32 RegisterMap::StoreState()
{
	State *state = new State;

	state->GuestRegs = new GuestReg[GUESTREG_COUNT];
	memcpy(state->GuestRegs, m_State.GuestRegs, sizeof(GuestReg) * GUESTREG_COUNT);

	state->HostRegs = new HostReg[m_HostRegCount];
	memcpy(state->HostRegs, m_State.HostRegs, sizeof(HostReg) * m_HostRegCount);
	
	u32 new_state_id = GenStateData();
	m_StateMap.insert(std::make_pair(new_state_id, state));

	return new_state_id;
}

void RegisterMap::RestoreState(u32 state_id)
{
	if (state_id == INVALID_STATE_ID)
	{
		PROGINFO("RegisterMap::RestoreState() : state_id is not invalid\n");

		return;
	}

	std::map<u32, State*>::iterator itr = m_StateMap.find(state_id);
	if (itr == m_StateMap.end())
	{
		PROGINFO("RegisterMap::RestoreState() : state_id[%u] is not exist\n", state_id);

		return;
	}

	State *state = itr->second;

	memcpy(m_State.GuestRegs, state->GuestRegs, sizeof(GuestReg) * GUESTREG_COUNT);
	memcpy(m_State.HostRegs, state->HostRegs, sizeof(HostReg) * m_HostRegCount);

	return;
}

void RegisterMap::CleanState(u32 &state_id)
{
	if (state_id == INVALID_STATE_ID)
	{
		PROGINFO("RegisterMap::CleanState() : state_id is not invalid\n");

		return;
	}

	std::map<u32, State*>::iterator itr = m_StateMap.find(state_id);
	if (itr == m_StateMap.end())
	{
		PROGINFO("RegisterMap::RestoreState() : state_id[%u] is not exist\n", state_id);

		return;
	}

	delete itr->second;
	m_StateMap.erase(itr);
	state_id = INVALID_STATE_ID;

	return;
}

void RegisterMap::CleanAllStates()
{
	for (std::map<u32, State*>::iterator itr = m_StateMap.begin(); 
		itr != m_StateMap.end(); itr++)
	{
		delete itr->second;
	}

	m_StateMap.clear();

	return;
}

u32 RegisterMap::CalcStates(u32 state_id, const std::vector<u32> &states)
{
	if (states.size() == 0)
	{
		PROGINFO("RegisterMap::CalcStates() : state_end is empty\n");

		return state_id;
	}

	std::vector<State*> state_datas;

	state_datas.reserve(states.size() + 1);

	{
		std::map<u32, State*>::iterator itr = m_StateMap.find(state_id);
		if (itr != m_StateMap.end())
			state_datas.push_back(itr->second);

		for (size_t i = 0; i < states.size(); i++)
		{
			std::map<u32, State*>::iterator itr = m_StateMap.find(states[i]);
			if (itr != m_StateMap.end())
				state_datas.push_back(itr->second);
		}
	}

	if (state_datas.size() != states.size() + 1)
	{
		PROGINFO("RegisterMap::CalcStates() : some state is not exist\n");

		return state_id;
	}
	
	State *new_state = new State;

	new_state->GuestRegs = new GuestReg[GUESTREG_COUNT];
	memcpy(new_state->GuestRegs, state_datas[0]->GuestRegs, sizeof(GuestReg) * GUESTREG_COUNT);
	new_state->HostRegs = new HostReg[m_HostRegCount];
	memcpy(new_state->HostRegs, state_datas[0]->HostRegs, sizeof(HostReg) * m_HostRegCount);
	
	for (u32 reg = 0; reg < GUESTREG_COUNT; reg++)
	{
		for (u32 s = 1; s < state_datas.size(); s++)
		{
			State *state = state_datas[s];

			switch (new_state->GuestRegs[reg].state)
			{
			case GuestReg::GRS_IMM:
				if (state->GuestRegs[reg].state != GuestReg::GRS_IMM || 
					state->GuestRegs[reg].immdata != new_state->GuestRegs[reg].immdata)
				{
					new_state->GuestRegs[reg].state = GuestReg::GRS_MEM;
				}
				break;

			case GuestReg::GRS_MAPPED:
				if (state->GuestRegs[reg].state != GuestReg::GRS_MAPPED || 
					state->GuestRegs[reg].hostreg != new_state->GuestRegs[reg].hostreg || 
					state->HostRegs[state->GuestRegs[reg].hostreg].dirty != new_state->HostRegs[new_state->GuestRegs[reg].hostreg].dirty)
				{
					const u32 hostreg = new_state->GuestRegs[reg].hostreg;

					new_state->HostRegs[hostreg].guestreg = INVALID_REG_ID;
					new_state->HostRegs[hostreg].swapdata = 0;
					new_state->HostRegs[hostreg].alloced = false;
					new_state->HostRegs[hostreg].dirty = false;
					if (new_state->HostRegs[hostreg].locked > 0)
						PROGINFO("RegisterMap::CalcStates() : HostReg[%u] is locked\n", hostreg);
					new_state->HostRegs[hostreg].locked = 0;

					new_state->GuestRegs[reg].state = GuestReg::GRS_MEM;
					new_state->GuestRegs[reg].hostreg = INVALID_REG_ID;
				}
				else if (state->GuestRegs[reg].state == GuestReg::GRS_MAPPED && 
						state->GuestRegs[reg].hostreg == new_state->GuestRegs[reg].hostreg)
				{
					const u32 hostreg = new_state->GuestRegs[reg].hostreg;

					if (new_state->HostRegs[hostreg].locked != state->HostRegs[hostreg].locked)
						PROGINFO("RegisterMap::CalcStates() : HostReg[%u] lock count is mismatch\n", hostreg);
				}
				break;

			case GuestReg::GRS_MEM:
				break;

			default:
				PROGINFO("RegisterMap::CalcStates() : GuestReg[%u] state unknow\n", reg);
				break;
			}
		}
	}

	for (u32 hostreg = 0; hostreg < m_HostRegCount; hostreg++)
	{
		for (u32 s = 1; s < state_datas.size(); s++)
		{
			State *state = state_datas[s];

			if (new_state->HostRegs[hostreg].alloced && 
				new_state->HostRegs[hostreg].guestreg == INVALID_REG_ID)
			{
				if (state->HostRegs[hostreg].alloced != new_state->HostRegs[hostreg].alloced || 
					state->HostRegs[hostreg].guestreg != new_state->HostRegs[hostreg].guestreg || 
					state->HostRegs[hostreg].locked != new_state->HostRegs[hostreg].locked)
				{
					PROGINFO("RegisterMap::CalcStates() : HostRegs[%u] is mismatch1\n", hostreg);

					continue;
				}
			}

			if (state->HostRegs[hostreg].alloced && 
				state->HostRegs[hostreg].guestreg == INVALID_REG_ID)
			{
				if (state->HostRegs[hostreg].alloced != new_state->HostRegs[hostreg].alloced || 
					state->HostRegs[hostreg].guestreg != new_state->HostRegs[hostreg].guestreg || 
					state->HostRegs[hostreg].locked != new_state->HostRegs[hostreg].locked)
				{
					PROGINFO("RegisterMap::CalcStates() : HostRegs[%u] is mismatch2\n", hostreg);

					continue;
				}
			}
		}
	}
	
	u32 new_state_id = GenStateData();
	m_StateMap.insert(std::make_pair(new_state_id, new_state));

	return new_state_id;
}

void RegisterMap::MergeToStates(u32 state_id)
{
	if (state_id == INVALID_STATE_ID)
	{
		PROGINFO("RegisterMap::MergeToStates() : state_id is not invalid\n");

		return;
	}

	std::map<u32, State*>::iterator itr = m_StateMap.find(state_id);
	if (itr == m_StateMap.end())
	{
		PROGINFO("RegisterMap::MergeToStates() : state_id[%u] is not exist\n", state_id);

		return;
	}

	State *state = itr->second;

	for (u32 reg = 0; reg < GUESTREG_COUNT; reg++)
	{
		switch (state->GuestRegs[reg].state)
		{
		case GuestReg::GRS_IMM:
			if (m_State.GuestRegs[reg].state != GuestReg::GRS_IMM || 
				m_State.GuestRegs[reg].immdata != state->GuestRegs[reg].immdata)
			{
				PROGINFO("RegisterMap::MergeToStates() : GuestReg[%u] state mismatch1\n", reg);
			}
			break;
		case GuestReg::GRS_MAPPED:
			if (m_State.GuestRegs[reg].state != GuestReg::GRS_MAPPED || 
				m_State.GuestRegs[reg].hostreg != state->GuestRegs[reg].hostreg)
			{
				PROGINFO("RegisterMap::MergeToStates() : GuestReg[%u] state mismatch2\n", reg);
			}
			break;
		case GuestReg::GRS_MEM:
			if (m_State.GuestRegs[reg].state != GuestReg::GRS_MEM)
			{
				FlushGuestReg((GuestRegId)reg);
			}
			break;

		default:
			PROGINFO("RegisterMap::MergeToStates() : GuestReg[%u] state unknow\n", reg);
			break;
		}
	}

	for (u32 hostreg = 0; hostreg < m_HostRegCount; hostreg++)
	{
		if (m_State.HostRegs[hostreg].alloced != state->HostRegs[hostreg].alloced)
		{
			PROGINFO("RegisterMap::MergeToStates() : HostRegs[%u] is mismatch(allocate)\n", hostreg);
		}
		else if (m_State.HostRegs[hostreg].alloced && 
				(m_State.HostRegs[hostreg].guestreg != state->HostRegs[hostreg].guestreg || 
				m_State.HostRegs[hostreg].dirty != state->HostRegs[hostreg].dirty || 
				m_State.HostRegs[hostreg].locked != state->HostRegs[hostreg].locked))
		{
			PROGINFO("RegisterMap::MergeToStates() : HostRegs[%u] is mismatch\n", hostreg);
		}
	}
}

RegisterMap::RegisterMap(u32 HostRegCount)
	: m_HostRegCount(HostRegCount)
	, m_SwapData(0)
	, m_StateData(0)
	, m_Context(NULL)
	, m_Cpu(NULL)
{
	m_State.GuestRegs = new GuestReg[GUESTREG_COUNT];
	m_State.HostRegs = new HostReg[HostRegCount];
}

u32 RegisterMap::AllocHostReg(bool preserved)
{
	u32 freereg = INVALID_REG_ID;

	// find a free hostreg
	for (u32 i = 0; i < m_HostRegCount; i++)
	{
		if (!m_State.HostRegs[i].alloced && 
			(!preserved || IsPerdureHostReg(i)))
		{
			freereg = i;

			break;
		}
	}

	if (freereg == INVALID_REG_ID)
	{
		// no free hostreg, try swap
		struct SwapData
		{
			u32 hostreg;
			u32 swapdata;

			static int Compare(const void *p1, const void *p2)
			{
				const SwapData *data1 = (const SwapData *)p1;
				const SwapData *data2 = (const SwapData *)p2;

				if (data1->swapdata > data2->swapdata)
					return 1;
				else if (data1->swapdata < data2->swapdata)
					return -1;

				return 0;
			}
		};

		SwapData *swaplist = (SwapData*)_alloca32(m_HostRegCount * sizeof(SwapData));

		u32 swapcount = 0;
		for (u32 i = 0; i < m_HostRegCount; i++)
		{
			if (m_State.HostRegs[i].alloced && !m_State.HostRegs[i].locked && 
				(!preserved || IsPerdureHostReg(i)))
			{
				swaplist[swapcount].hostreg = i;
				swaplist[swapcount].swapdata = m_State.HostRegs[i].swapdata;
				swapcount++;
			}
		}

		if (swapcount > 0)
		{
			if (swapcount > 1)
				qsort(swaplist, swapcount, sizeof(SwapData), &SwapData::Compare);

			freereg = swaplist[0].hostreg;

			FlushHostReg(freereg);
		}
		else
			return INVALID_REG_ID;
	}

	m_State.HostRegs[freereg].guestreg = INVALID_REG_ID;
	m_State.HostRegs[freereg].swapdata = 0;
	m_State.HostRegs[freereg].alloced = true;
	m_State.HostRegs[freereg].dirty = false;
	m_State.HostRegs[freereg].locked = 0;

	return freereg;
}

u32 RegisterMap::GenSwapData()
{
	return m_SwapData++;
}

u32 RegisterMap::GenStateData()
{
	return m_StateData++;
}

//CACHE_ALIGN u8 g_RecompileCounts[(1<<26)/16];

void JitLutInit()
{
#ifdef MAPPED_JIT_FUNCS
	JIT_MASK[ARMCPU_ARM9][2*2+0] = _MMU_MAIN_MEM_MASK;
	JIT_MASK[ARMCPU_ARM9][2*2+1] = _MMU_MAIN_MEM_MASK;

	for (int proc = 0; proc < JitLut::JIT_MEM_COUNT; proc++)
	{
		for (int i = 0; i < JitLut::JIT_MEM_LEN; i++)
		{
			g_JitLut.JIT_MEM[proc][i] = JIT_MEM[proc][i>>9] + (((i<<14) & JIT_MASK[proc][i>>9]) >> 1);
		}
	}
#endif
}

void JitLutDeInit()
{
}

void JitLutReset()
{
#ifdef MAPPED_JIT_FUNCS
	memset(g_JitLut.MAIN_MEM,  0, sizeof(g_JitLut.MAIN_MEM));
	memset(g_JitLut.SWIRAM,    0, sizeof(g_JitLut.SWIRAM));
	memset(g_JitLut.ARM9_ITCM, 0, sizeof(g_JitLut.ARM9_ITCM));
	memset(g_JitLut.ARM9_LCDC, 0, sizeof(g_JitLut.ARM9_LCDC));
	memset(g_JitLut.ARM9_BIOS, 0, sizeof(g_JitLut.ARM9_BIOS));
	memset(g_JitLut.ARM7_BIOS, 0, sizeof(g_JitLut.ARM7_BIOS));
	memset(g_JitLut.ARM7_ERAM, 0, sizeof(g_JitLut.ARM7_ERAM));
	memset(g_JitLut.ARM7_WIRAM,0, sizeof(g_JitLut.ARM7_WIRAM));
	memset(g_JitLut.ARM7_WRAM, 0, sizeof(g_JitLut.ARM7_WRAM));
#else
	memset(g_CompiledFuncs, 0, sizeof(g_CompiledFuncs));
#endif
	//memset(g_RecompileCounts,0, sizeof(g_RecompileCounts));
}

void FlushIcacheSection(u8 *begin, u8 *end)
{
#ifdef _MSC_VER
	FlushInstructionCache(GetCurrentProcess(), begin, end - begin);
#else
	__builtin___clear_cache(begin, end);
#endif
}

#endif //HAVE_JIT
