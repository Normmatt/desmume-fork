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

#include "JitBase.h"
#include "MMU.h"
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

//CACHE_ALIGN u8 g_RecompileCounts[(1<<26)/16];

// 
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
