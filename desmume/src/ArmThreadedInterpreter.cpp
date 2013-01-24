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

#include "ArmThreadedInterpreter.h"
#include "ArmAnalyze.h"
#include "instructions.h"
#include "instruction_attributes.h"
#include "Disassembler.h"

#include "armcpu.h"
#include "MMU.h"
#include "MMU_timing.h"
#include "JitBase.h"

typedef u32 (FASTCALL* OpCompiler)(const u32 i, struct MethodCommon* common);
typedef void (FASTCALL* OpMethod)(const struct MethodCommon* common, u8* cycle);

#define GETCPU (&ARMPROC)
#define TEMPLATE template<int PROCNUM> 

static void* AllocCache(u32 size);
static void* AllocCacheAlign32(u32 size);
static u32 GetCacheRemain();

struct MethodCommon
{
	OpMethod func;
	void* data;
	u32 R15;
};

struct Block
{
	MethodCommon *ops;
	u8 cycles;
};

#define DCL_OP_START(name) \
	TEMPLATE struct name \
	{

#define DCL_OP_COMPILER(name) \
	static u32 FASTCALL Compiler(const u32 i, MethodCommon* common) \
	{ \
		name *pData = (name*)AllocCacheAlign32(sizeof(name)); \
		common->func = name<PROCNUM>::Method; \
		common->data = pData; 

#define DCL_OP_METHOD(name) \
	static void FASTCALL Method(const MethodCommon* common, u8* cycle) \
	{ \
		name *pData = (name*)common->data;

#define DONE_COMPILER \
	return 1;

#define GOTO_NEXTOP(num) \
	*cycle += (num); \
	common++; \
	return common->func(common, cycle); 

#define DATA(name) (pData->name)

#define GETCPUREG_R(i)			((i) == 15 ? common->R15 : GETCPU->R[(i)])
#define GETCPUREG_W(i)			(GETCPU->R[(i)])
#define GETCPUREG_RW(i)			(GETCPU->R[(i)])

#define THUMB_REGPOS_R(i, n)	(GETCPUREG_R(((i)>>n)&0x7))
#define THUMB_REGPOS_W(i, n)	(GETCPUREG_W(((i)>>n)&0x7))
#define THUMB_REGPOS_RW(i, n)	(GETCPUREG_RW(((i)>>n)&0x7))

#define ARM_REGPOS_R(i, n)		(GETCPUREG_R(((i)>>n)&0xF))
#define ARM_REGPOS_W(i, n)		(GETCPUREG_W(((i)>>n)&0xF))
#define ARM_REGPOS_RW(i, n)		(GETCPUREG_RW(((i)>>n)&0xF))

//------------------------------------------------------------
//                         THUMB
//------------------------------------------------------------
//-----------------------------------------------------------------------------
//   Undefined instruction
//-----------------------------------------------------------------------------
DCL_OP_START(OP_UND_THUMB)
	armcpu_t *cpu;

	DCL_OP_COMPILER(OP_UND_THUMB)
		DATA(cpu) = GETCPU;

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_UND_THUMB)
		TRAPUNDEF(DATA(cpu));

		GOTO_NEXTOP(1)
	}
};

//-----------------------------------------------------------------------------
//   LSL
//-----------------------------------------------------------------------------
DCL_OP_START(OP_LSL_0)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_3;

	DCL_OP_COMPILER(OP_LSL_0)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(THUMB_REGPOS_W(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LSL_0)
		*DATA(r_0) = *DATA(r_3);
		DATA(cpsr)->bits.N = BIT31(*DATA(r_0));
		DATA(cpsr)->bits.Z = *DATA(r_0) == 0;

		GOTO_NEXTOP(1)
	}
};

DCL_OP_START(OP_LSL)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_3;
	u32 v;

	DCL_OP_COMPILER(OP_LSL)
		DATA(v) = (i>>6) & 0x1F;
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(THUMB_REGPOS_W(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LSL)
		DATA(cpsr)->bits.C = BIT_N(*DATA(r_3), 32 - DATA(v));
		*DATA(r_0) = *DATA(r_3) << DATA(v);
		DATA(cpsr)->bits.N = BIT31(*DATA(r_0));
		DATA(cpsr)->bits.Z = *DATA(r_0) == 0;

		GOTO_NEXTOP(1)
	}
};

DCL_OP_START(OP_LSL_REG)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_3;

	DCL_OP_COMPILER(OP_LSL_REG)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(THUMB_REGPOS_W(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LSL_REG)
		u32 v = *DATA(r_3) & 0xFF;

		if(v==0)
		{
			DATA(cpsr)->bits.N = BIT31(*DATA(r_0));
			DATA(cpsr)->bits.Z = *DATA(r_0) == 0;

			GOTO_NEXTOP(2)
		}
		if(v<32)
		{
			DATA(cpsr)->bits.C = BIT_N(*DATA(r_0), 32-v);
			*DATA(r_0) <<= v;
			DATA(cpsr)->bits.N = BIT31(*DATA(r_0));
			DATA(cpsr)->bits.Z = *DATA(r_0) == 0;

			GOTO_NEXTOP(2)
		}
		if(v==32)
			DATA(cpsr)->bits.C = BIT0(*DATA(r_0));
		else
			DATA(cpsr)->bits.C = 0;

		*DATA(r_0) = 0;
		DATA(cpsr)->bits.N = 0;
		DATA(cpsr)->bits.Z = 1;

		GOTO_NEXTOP(2)
	}
};

//-----------------------------------------------------------------------------
//   LSR
//-----------------------------------------------------------------------------
DCL_OP_START(OP_LSR_0)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_3;

	DCL_OP_COMPILER(OP_LSR_0)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(THUMB_REGPOS_W(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LSR_0)
		DATA(cpsr)->bits.C = BIT31(*DATA(r_3));
		*DATA(r_0) = 0;
		DATA(cpsr)->bits.N = 0;
		DATA(cpsr)->bits.Z = 1;

		GOTO_NEXTOP(1)
	}
};

DCL_OP_START(OP_LSR)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_3;
	u32 v;

	DCL_OP_COMPILER(OP_LSR)
		DATA(v) = (i>>6) & 0x1F;
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(THUMB_REGPOS_W(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LSR)
		DATA(cpsr)->bits.C = BIT_N(*DATA(r_3), DATA(v) - 1);
		*DATA(r_0) = *DATA(r_3) >> DATA(v);
		DATA(cpsr)->bits.N = BIT31(*DATA(r_0));
		DATA(cpsr)->bits.Z = *DATA(r_0) == 0;

		GOTO_NEXTOP(1)
	}
};

DCL_OP_START(OP_LSR_REG)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_3;

	DCL_OP_COMPILER(OP_LSR_REG)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(THUMB_REGPOS_W(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LSR_REG)
		u32 v = *DATA(r_3) & 0xFF;

		if(v == 0)
		{
			DATA(cpsr)->bits.N = BIT31(*DATA(r_0));
			DATA(cpsr)->bits.Z = *DATA(r_0) == 0;

			GOTO_NEXTOP(2)
		}	
		if(v<32)
		{
			DATA(cpsr)->bits.C = BIT_N(*DATA(r_0), v-1);
			*DATA(r_0) >>= v;
			DATA(cpsr)->bits.N = BIT31(*DATA(r_0));
			DATA(cpsr)->bits.Z = *DATA(r_0) == 0;

			GOTO_NEXTOP(2)
		}
		if(v==32)
			DATA(cpsr)->bits.C = BIT31(*DATA(r_0));
		else
			DATA(cpsr)->bits.C = 0;

		*DATA(r_0) = 0;
		DATA(cpsr)->bits.N = 0;
		DATA(cpsr)->bits.Z = 1;

		GOTO_NEXTOP(2)
	}
};

//-----------------------------------------------------------------------------
//   ASR
//-----------------------------------------------------------------------------
DCL_OP_START(OP_ASR_0)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_3;

	DCL_OP_COMPILER(OP_ASR_0)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(THUMB_REGPOS_W(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_ASR_0)
		DATA(cpsr)->bits.C = BIT31(*DATA(r_3));
		*DATA(r_0) = BIT31(*DATA(r_3))*0xFFFFFFFF;
		DATA(cpsr)->bits.N = BIT31(*DATA(r_0));
		DATA(cpsr)->bits.Z = *DATA(r_0) == 0;

		GOTO_NEXTOP(1)
	}
};

DCL_OP_START(OP_ASR)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_3;
	u32 v;

	DCL_OP_COMPILER(OP_ASR)
		DATA(v) = (i>>6) & 0x1F;
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(THUMB_REGPOS_W(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_ASR)
		DATA(cpsr)->bits.C = BIT_N(*DATA(r_3), DATA(v) - 1);
		*DATA(r_0) = (u32)(((s32)*DATA(r_3)) >> DATA(v));
		DATA(cpsr)->bits.N = BIT31(*DATA(r_0));
		DATA(cpsr)->bits.Z = *DATA(r_0) == 0;

		GOTO_NEXTOP(1)
	}
};

DCL_OP_START(OP_ASR_REG)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_3;

	DCL_OP_COMPILER(OP_ASR_REG)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(THUMB_REGPOS_W(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_ASR_REG)
		u32 v = *DATA(r_3) & 0xFF;

		if(v == 0)
		{
			DATA(cpsr)->bits.N = BIT31(*DATA(r_0));
			DATA(cpsr)->bits.Z = *DATA(r_0) == 0;

			GOTO_NEXTOP(2)
		}	
		if(v<32)
		{
			DATA(cpsr)->bits.C = BIT_N(*DATA(r_0), v-1);
			*DATA(r_0) = (u32)(((s32)*DATA(r_0)) >> v);
			DATA(cpsr)->bits.N = BIT31(*DATA(r_0));
			DATA(cpsr)->bits.Z = *DATA(r_0) == 0;

			GOTO_NEXTOP(2)
		}

		DATA(cpsr)->bits.C = BIT31(*DATA(r_0));
		*DATA(r_0) = BIT31(*DATA(r_0))*0xFFFFFFFF;
		DATA(cpsr)->bits.N = BIT31(*DATA(r_0));
		DATA(cpsr)->bits.Z = *DATA(r_0) == 0;

		GOTO_NEXTOP(2)
	}
};

//-----------------------------------------------------------------------------
//   ADD
//-----------------------------------------------------------------------------
DCL_OP_START(OP_ADD_IMM3)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_3;
	u32 imm3;

	DCL_OP_COMPILER(OP_ADD_IMM3)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(THUMB_REGPOS_W(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));
		DATA(imm3) = (i >> 6) & 0x07;

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_ADD_IMM3)
		u32 Rn = *DATA(r_3);

		if (DATA(imm3) == 0)	// mov 2
		{
			*DATA(r_0) = Rn;

			DATA(cpsr)->bits.N = BIT31(*DATA(r_0));
			DATA(cpsr)->bits.Z = *DATA(r_0) == 0;
			DATA(cpsr)->bits.C = 0;
			DATA(cpsr)->bits.V = 0;

			GOTO_NEXTOP(1)
		}

		*DATA(r_0) = Rn + DATA(imm3);
		DATA(cpsr)->bits.N = BIT31(*DATA(r_0));
		DATA(cpsr)->bits.Z = *DATA(r_0) == 0;
		DATA(cpsr)->bits.C = CarryFrom(Rn, DATA(imm3));
		DATA(cpsr)->bits.V = OverflowFromADD(*DATA(r_0), Rn, DATA(imm3));

		GOTO_NEXTOP(1)
	}
};

DCL_OP_START(OP_ADD_IMM8)
	Status_Reg *cpsr;
	u32 *r_8;
	u32 imm8;

	DCL_OP_COMPILER(OP_ADD_IMM8)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_8) = &(THUMB_REGPOS_RW(i, 8));
		DATA(imm8) = (i & 0xFF);

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_ADD_IMM8)
		u32 Rd = *DATA(r_8);

		*DATA(r_8) = Rd + DATA(imm8);
		DATA(cpsr)->bits.N = BIT31(*DATA(r_8));
		DATA(cpsr)->bits.Z = *DATA(r_8) == 0;
		DATA(cpsr)->bits.C = CarryFrom(Rd, DATA(imm8));
		DATA(cpsr)->bits.V = OverflowFromADD(*DATA(r_8), Rd, DATA(imm8));

		GOTO_NEXTOP(1)
	}
};

DCL_OP_START(OP_ADD_REG)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_3;
	u32 *r_6;

	DCL_OP_COMPILER(OP_ADD_REG)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(THUMB_REGPOS_W(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));
		DATA(r_6) = &(THUMB_REGPOS_R(i, 6));

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_ADD_REG)
		u32 Rn = *DATA(r_3);
		u32 Rm = *DATA(r_6);

		*DATA(r_0) = Rn + Rm;
		DATA(cpsr)->bits.N = BIT31(*DATA(r_0));
		DATA(cpsr)->bits.Z = *DATA(r_0) == 0;
		DATA(cpsr)->bits.C = CarryFrom(Rn, Rm);
		DATA(cpsr)->bits.V = OverflowFromADD(*DATA(r_0), Rn, Rm);

		GOTO_NEXTOP(1)
	}
};

DCL_OP_START(OP_ADD_SPE)
	u32 *r_d;
	u32 *r_3;
	bool mod_r15;

	DCL_OP_COMPILER(OP_ADD_SPE)
		u32 Rd = ((i)&0x7) | ((i>>4)&8);
		
		DATA(r_d) = &(GETCPUREG_RW(Rd));
		DATA(r_3) = &(ARM_REGPOS_R(i, 3));
		DATA(mod_r15) = Rd == 15;

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_ADD_SPE)
		*DATA(r_d) += *DATA(r_3);

		if (DATA(mod_r15))
		{
			GOTO_NEXTOP(3)
		}

		GOTO_NEXTOP(1)
	}
};

DCL_OP_START(OP_ADD_2PC)
	u32 *r_8;
	u32 val;

	DCL_OP_COMPILER(OP_ADD_2PC)
		DATA(r_8) = &(THUMB_REGPOS_W(i, 8));
		DATA(val) = (common->R15&0xFFFFFFFC) + ((i&0xFF)<<2);

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_ADD_2PC)
		*DATA(r_8) = DATA(val);

		GOTO_NEXTOP(1)
	}
};

DCL_OP_START(OP_ADD_2SP)
	u32 *r_8;
	u32 *r_13;
	u32 add;

	DCL_OP_COMPILER(OP_ADD_2SP)
		DATA(r_8) = &(THUMB_REGPOS_W(i, 8));
		DATA(r_13) = &(GETCPUREG_R(13));
		DATA(add) = ((i&0xFF)<<2);

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_ADD_2SP)
		*DATA(r_8) = *DATA(r_13) + DATA(add);

		GOTO_NEXTOP(1)
	}
};

//-----------------------------------------------------------------------------
//   SUB
//-----------------------------------------------------------------------------
DCL_OP_START(OP_SUB_IMM3)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_3;
	u32 imm3;

	DCL_OP_COMPILER(OP_SUB_IMM3)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(THUMB_REGPOS_W(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));
		DATA(imm3) = (i>>6) & 0x07;

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_SUB_IMM3)
		u32 Rn = *DATA(r_3);
		u32 tmp = Rn - DATA(imm3);

		*DATA(r_0) = tmp;
		DATA(cpsr)->bits.N = BIT31(tmp);
		DATA(cpsr)->bits.Z = tmp == 0;
		DATA(cpsr)->bits.C = !BorrowFrom(Rn, DATA(imm3));
		DATA(cpsr)->bits.V = OverflowFromSUB(tmp, Rn, DATA(imm3));

		GOTO_NEXTOP(1)
	}
};

DCL_OP_START(OP_SUB_IMM8)
	Status_Reg *cpsr;
	u32 *r_8;
	u32 imm8;

	DCL_OP_COMPILER(OP_SUB_IMM8)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_8) = &(THUMB_REGPOS_RW(i, 8));
		DATA(imm8) = (i & 0xFF);

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_SUB_IMM8)
		u32 Rd = *DATA(r_8);
		u32 tmp = Rd - DATA(imm8);

		*DATA(r_8) = tmp;
		DATA(cpsr)->bits.N = BIT31(tmp);
		DATA(cpsr)->bits.Z = tmp == 0;
		DATA(cpsr)->bits.C = !BorrowFrom(Rd, DATA(imm8));
		DATA(cpsr)->bits.V = OverflowFromSUB(tmp, Rd, DATA(imm8));

		GOTO_NEXTOP(1)
	}
};

DCL_OP_START(OP_SUB_REG)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_3;
	u32 *r_6;

	DCL_OP_COMPILER(OP_SUB_REG)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(THUMB_REGPOS_W(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));
		DATA(r_6) = &(THUMB_REGPOS_R(i, 6));

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_SUB_REG)
		u32 Rn = *DATA(r_3);
		u32 Rm = *DATA(r_6);
		u32 tmp = Rn - Rm;

		*DATA(r_0) = tmp;
		DATA(cpsr)->bits.N = BIT31(tmp);
		DATA(cpsr)->bits.Z = tmp == 0;
		DATA(cpsr)->bits.C = !BorrowFrom(Rn, Rm);
		DATA(cpsr)->bits.V = OverflowFromSUB(tmp, Rn, Rm);

		GOTO_NEXTOP(1)
	}
};

//-----------------------------------------------------------------------------
//   MOV
//-----------------------------------------------------------------------------
DCL_OP_START(OP_MOV_IMM8)
	Status_Reg *cpsr;
	u32 *r_8;
	u32 val;
	bool N;
	bool Z;

	DCL_OP_COMPILER(OP_MOV_IMM8)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_8) = &(THUMB_REGPOS_W(i, 8));
		DATA(val) = (i & 0xFF);
		DATA(N) = BIT31(DATA(val));
		DATA(Z) = DATA(val) == 0;

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_MOV_IMM8)
		*DATA(r_8) = DATA(val);
		DATA(cpsr)->bits.N = DATA(N);
		DATA(cpsr)->bits.Z = DATA(Z);

		GOTO_NEXTOP(1)
	}
};

DCL_OP_START(OP_MOV_SPE)
	u32 *r_d;
	u32 *r_3;
	bool mod_r15;

	DCL_OP_COMPILER(OP_MOV_SPE)
		u32 Rd = ((i)&0x7) | ((i>>4)&8);
		
		DATA(r_d) = &(GETCPUREG_W(Rd));
		DATA(r_3) = &(ARM_REGPOS_R(i, 3));
		DATA(mod_r15) = Rd == 15;

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_MOV_SPE)
		*DATA(r_d) = *DATA(r_3);

		if (DATA(mod_r15))
		{
			GOTO_NEXTOP(3)
		}

		GOTO_NEXTOP(1)
	}
};

//-----------------------------------------------------------------------------
//   CMP
//-----------------------------------------------------------------------------
DCL_OP_START(OP_CMP_IMM8)
	Status_Reg *cpsr;
	u32 *r_8;
	u32 imm8;

	DCL_OP_COMPILER(OP_CMP_IMM8)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_8) = &(THUMB_REGPOS_R(i, 8));
		DATA(imm8) = (i & 0xFF);

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_CMP_IMM8)
		u32 tmp = *DATA(r_8) - DATA(imm8);
		
		DATA(cpsr)->bits.N = BIT31(tmp);
		DATA(cpsr)->bits.Z = tmp == 0;
		DATA(cpsr)->bits.C = !BorrowFrom(*DATA(r_8), DATA(imm8));
		DATA(cpsr)->bits.V = OverflowFromSUB(tmp, *DATA(r_8), DATA(imm8));

		GOTO_NEXTOP(1)
	}
};

DCL_OP_START(OP_CMP)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_3;

	DCL_OP_COMPILER(OP_CMP)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(THUMB_REGPOS_R(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_CMP)
		u32 tmp = *DATA(r_0) - *DATA(r_3);
		
		DATA(cpsr)->bits.N = BIT31(tmp);
		DATA(cpsr)->bits.Z = tmp == 0;
		DATA(cpsr)->bits.C = !BorrowFrom(*DATA(r_0), *DATA(r_3));
		DATA(cpsr)->bits.V = OverflowFromSUB(tmp, *DATA(r_0), *DATA(r_3));

		GOTO_NEXTOP(1)
	}
};

DCL_OP_START(OP_CMP_SPE)
	Status_Reg *cpsr;
	u32 *r_n;
	u32 *r_3;

	DCL_OP_COMPILER(OP_CMP_SPE)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_n) = &(GETCPUREG_R((i&7) | ((i>>4)&8)));
		DATA(r_3) = &(ARM_REGPOS_R(i, 3));

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_CMP_SPE)
		u32 tmp = *DATA(r_n) - *DATA(r_3);
		
		DATA(cpsr)->bits.N = BIT31(tmp);
		DATA(cpsr)->bits.Z = tmp == 0;
		DATA(cpsr)->bits.C = !BorrowFrom(*DATA(r_n), *DATA(r_3));
		DATA(cpsr)->bits.V = OverflowFromSUB(tmp, *DATA(r_n), *DATA(r_3));

		GOTO_NEXTOP(1)
	}
};

//-----------------------------------------------------------------------------
//   AND
//-----------------------------------------------------------------------------
DCL_OP_START(OP_AND)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_3;

	DCL_OP_COMPILER(OP_AND)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(THUMB_REGPOS_RW(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_AND)
		*DATA(r_0) &= *DATA(r_3);
		
		DATA(cpsr)->bits.N = BIT31(*DATA(r_0));
		DATA(cpsr)->bits.Z = *DATA(r_0) == 0;

		GOTO_NEXTOP(1)
	}
};

//-----------------------------------------------------------------------------
//   EOR
//-----------------------------------------------------------------------------
DCL_OP_START(OP_EOR)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_3;

	DCL_OP_COMPILER(OP_EOR)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(THUMB_REGPOS_RW(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_EOR)
		*DATA(r_0) ^= *DATA(r_3);
		
		DATA(cpsr)->bits.N = BIT31(*DATA(r_0));
		DATA(cpsr)->bits.Z = *DATA(r_0) == 0;

		GOTO_NEXTOP(1)
	}
};

//-----------------------------------------------------------------------------
//   ADC
//-----------------------------------------------------------------------------
DCL_OP_START(OP_ADC_REG)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_3;

	DCL_OP_COMPILER(OP_ADC_REG)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(THUMB_REGPOS_RW(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_ADC_REG)
		u32 Rd = *DATA(r_0);
		u32 Rm = *DATA(r_3);

		if (!DATA(cpsr)->bits.C)
		{
			*DATA(r_0) = Rd + Rm;
			DATA(cpsr)->bits.C = *DATA(r_0) < Rm;
		}
		else
		{
			*DATA(r_0) = Rd + Rm + 1;
			DATA(cpsr)->bits.C = *DATA(r_0) <= Rm;
		}
		
		DATA(cpsr)->bits.N = BIT31(*DATA(r_0));
		DATA(cpsr)->bits.Z = *DATA(r_0) == 0;
		DATA(cpsr)->bits.V = BIT31((Rd ^ Rm ^ -1) & (Rd ^ *DATA(r_0)));

		GOTO_NEXTOP(1)
	}
};

//-----------------------------------------------------------------------------
//   SBC
//-----------------------------------------------------------------------------
DCL_OP_START(OP_SBC_REG)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_3;

	DCL_OP_COMPILER(OP_SBC_REG)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(THUMB_REGPOS_RW(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_SBC_REG)
		u32 Rd = *DATA(r_0);
		u32 Rm = *DATA(r_3);

		if (!DATA(cpsr)->bits.C)
		{
			*DATA(r_0) = Rd - Rm - 1;
			DATA(cpsr)->bits.C = Rd > Rm;
		}
		else
		{
			*DATA(r_0) = Rd - Rm;
			DATA(cpsr)->bits.C = Rd >= Rm;
		}
		
		DATA(cpsr)->bits.N = BIT31(*DATA(r_0));
		DATA(cpsr)->bits.Z = *DATA(r_0) == 0;
		DATA(cpsr)->bits.V = BIT31((Rd ^ Rm) & (Rd ^ *DATA(r_0)));

		GOTO_NEXTOP(1)
	}
};

//-----------------------------------------------------------------------------
//   ROR
//-----------------------------------------------------------------------------
DCL_OP_START(OP_ROR_REG)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_3;

	DCL_OP_COMPILER(OP_ROR_REG)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(THUMB_REGPOS_RW(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_ROR_REG)
		u32 v = *DATA(r_3) & 0xFF;

		if(v == 0)
		{
			DATA(cpsr)->bits.N = BIT31(*DATA(r_0));
			DATA(cpsr)->bits.Z = *DATA(r_0) == 0;

			GOTO_NEXTOP(2)
		}

		v &= 0x1F;
		if(v == 0)
		{
			DATA(cpsr)->bits.C = BIT31(*DATA(r_0));
			DATA(cpsr)->bits.N = BIT31(*DATA(r_0));
			DATA(cpsr)->bits.Z = *DATA(r_0) == 0;

			GOTO_NEXTOP(2)
		}
		
		DATA(cpsr)->bits.C = BIT_N(*DATA(r_0), v-1);
		*DATA(r_0) = ROR(*DATA(r_0), v);
		DATA(cpsr)->bits.N = BIT31(*DATA(r_0));
		DATA(cpsr)->bits.Z = *DATA(r_0) == 0;

		GOTO_NEXTOP(2)
	}
};

//-----------------------------------------------------------------------------
//   TST
//-----------------------------------------------------------------------------
DCL_OP_START(OP_TST)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_3;

	DCL_OP_COMPILER(OP_TST)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(THUMB_REGPOS_R(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_TST)
		u32 tmp = *DATA(r_0) & *DATA(r_3);

		DATA(cpsr)->bits.N = BIT31(tmp);
		DATA(cpsr)->bits.Z = tmp == 0;

		GOTO_NEXTOP(1)
	}
};

//-----------------------------------------------------------------------------
//   NEG
//-----------------------------------------------------------------------------
DCL_OP_START(OP_NEG)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_3;

	DCL_OP_COMPILER(OP_NEG)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(THUMB_REGPOS_W(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_NEG)
		u32 Rm = *DATA(r_3);

		*DATA(r_0) = (u32)((s32)0 - (s32)Rm);

		DATA(cpsr)->bits.N = BIT31(*DATA(r_0));
		DATA(cpsr)->bits.Z = *DATA(r_0) == 0;
		DATA(cpsr)->bits.C = !BorrowFrom(0, Rm);
		DATA(cpsr)->bits.V = OverflowFromSUB(*DATA(r_0), 0, Rm);

		GOTO_NEXTOP(1)
	}
};

//-----------------------------------------------------------------------------
//   CMN
//-----------------------------------------------------------------------------
DCL_OP_START(OP_CMN)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_3;

	DCL_OP_COMPILER(OP_CMN)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(THUMB_REGPOS_R(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_CMN)
		u32 tmp = *DATA(r_0) + *DATA(r_3);

		DATA(cpsr)->bits.N = BIT31(tmp);
		DATA(cpsr)->bits.Z = tmp == 0;
		DATA(cpsr)->bits.C = CarryFrom(*DATA(r_0), *DATA(r_3));
		DATA(cpsr)->bits.V = OverflowFromADD(tmp, *DATA(r_0), *DATA(r_3));

		GOTO_NEXTOP(1)
	}
};

//-----------------------------------------------------------------------------
//   ORR
//-----------------------------------------------------------------------------
DCL_OP_START(OP_ORR)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_3;

	DCL_OP_COMPILER(OP_ORR)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(THUMB_REGPOS_RW(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_ORR)
		*DATA(r_0) |= *DATA(r_3);

		DATA(cpsr)->bits.N = BIT31(*DATA(r_0));
		DATA(cpsr)->bits.Z = *DATA(r_0) == 0;

		GOTO_NEXTOP(1)
	}
};

//-----------------------------------------------------------------------------
//   BIC
//-----------------------------------------------------------------------------
DCL_OP_START(OP_BIC)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_3;

	DCL_OP_COMPILER(OP_BIC)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(THUMB_REGPOS_RW(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_BIC)
		*DATA(r_0) &= (~(*DATA(r_3)));

		DATA(cpsr)->bits.N = BIT31(*DATA(r_0));
		DATA(cpsr)->bits.Z = *DATA(r_0) == 0;

		GOTO_NEXTOP(1)
	}
};

//-----------------------------------------------------------------------------
//   MVN
//-----------------------------------------------------------------------------
DCL_OP_START(OP_MVN)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_3;

	DCL_OP_COMPILER(OP_MVN)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(THUMB_REGPOS_W(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_MVN)
		*DATA(r_0) = (~(*DATA(r_3)));

		DATA(cpsr)->bits.N = BIT31(*DATA(r_0));
		DATA(cpsr)->bits.Z = *DATA(r_0) == 0;

		GOTO_NEXTOP(1)
	}
};

//-----------------------------------------------------------------------------
//   MUL
//-----------------------------------------------------------------------------
#define MUL_Mxx_END_THUMB(c) \
	v >>= 8; \
	if((v==0)||(v==0xFFFFFF)) \
	{ \
		GOTO_NEXTOP(c+1); \
	} \
	v >>= 8; \
	if((v==0)||(v==0xFFFF)) \
	{ \
		GOTO_NEXTOP(c+2); \
	} \
	v >>= 8; \
	if((v==0)||(v==0xFF)) \
	{ \
		GOTO_NEXTOP(c+3); \
	} \
	GOTO_NEXTOP(c+4);

DCL_OP_START(OP_MUL_REG)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_3;

	DCL_OP_COMPILER(OP_MUL_REG)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(THUMB_REGPOS_RW(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_MUL_REG)
		u32 v = *DATA(r_3);

		*DATA(r_0) *= v;

		DATA(cpsr)->bits.N = BIT31(*DATA(r_0));
		DATA(cpsr)->bits.Z = *DATA(r_0) == 0;

		if (PROCNUM == 1)	// ARM4T 1S + mI, m = 3
		{
			GOTO_NEXTOP(4)
		}

		MUL_Mxx_END_THUMB(1)
	}
};

#undef MUL_Mxx_END_THUMB

//-----------------------------------------------------------------------------
//   STRB / LDRB
//-----------------------------------------------------------------------------
DCL_OP_START(OP_STRB_IMM_OFF)
	armcpu_t *cpu;
	u32 *r_0;
	u32 *r_3;
	u32 val;

	DCL_OP_COMPILER(OP_STRB_IMM_OFF)
		DATA(cpu) = GETCPU;
		DATA(r_0) = &(THUMB_REGPOS_R(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));
		DATA(val) = ((i>>6)&0x1F);

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_STRB_IMM_OFF)
		u32 adr = *DATA(r_3) + DATA(val);
		
		WRITE8(DATA(cpu)->mem_if->data, adr, (u8)*DATA(r_0));

		u8 c = MMU_aluMemAccessCycles<PROCNUM,8,MMU_AD_WRITE>(2, adr);
		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_LDRB_IMM_OFF)
	armcpu_t *cpu;
	u32 *r_0;
	u32 *r_3;
	u32 val;

	DCL_OP_COMPILER(OP_LDRB_IMM_OFF)
		DATA(cpu) = GETCPU;
		DATA(r_0) = &(THUMB_REGPOS_W(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));
		DATA(val) = ((i>>6)&0x1F);

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LDRB_IMM_OFF)
		u32 adr = *DATA(r_3) + DATA(val);
		
		*DATA(r_0) = (u32)READ8(DATA(cpu)->mem_if->data, adr);

		u8 c = MMU_aluMemAccessCycles<PROCNUM,8,MMU_AD_READ>(3, adr);
		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_STRB_REG_OFF)
	armcpu_t *cpu;
	u32 *r_0;
	u32 *r_3;
	u32 *r_6;

	DCL_OP_COMPILER(OP_STRB_REG_OFF)
		DATA(cpu) = GETCPU;
		DATA(r_0) = &(THUMB_REGPOS_R(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));
		DATA(r_6) = &(THUMB_REGPOS_R(i, 6));

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_STRB_REG_OFF)
		u32 adr = *DATA(r_3) + *DATA(r_6);
		
		WRITE8(DATA(cpu)->mem_if->data, adr, (u8)*DATA(r_0));

		u8 c = MMU_aluMemAccessCycles<PROCNUM,8,MMU_AD_WRITE>(2, adr);
		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_LDRB_REG_OFF)
	armcpu_t *cpu;
	u32 *r_0;
	u32 *r_3;
	u32 *r_6;

	DCL_OP_COMPILER(OP_LDRB_REG_OFF)
		DATA(cpu) = GETCPU;
		DATA(r_0) = &(THUMB_REGPOS_W(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));
		DATA(r_6) = &(THUMB_REGPOS_R(i, 6));

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LDRB_REG_OFF)
		u32 adr = *DATA(r_3) + *DATA(r_6);
		
		*DATA(r_0) = (u32)READ8(DATA(cpu)->mem_if->data, adr);

		u8 c = MMU_aluMemAccessCycles<PROCNUM,8,MMU_AD_READ>(3, adr);
		GOTO_NEXTOP(c)
	}
};

//-----------------------------------------------------------------------------
//   LDRSB
//-----------------------------------------------------------------------------
DCL_OP_START(OP_LDRSB_REG_OFF)
	armcpu_t *cpu;
	u32 *r_0;
	u32 *r_3;
	u32 *r_6;

	DCL_OP_COMPILER(OP_LDRSB_REG_OFF)
		DATA(cpu) = GETCPU;
		DATA(r_0) = &(THUMB_REGPOS_W(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));
		DATA(r_6) = &(THUMB_REGPOS_R(i, 6));

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LDRSB_REG_OFF)
		u32 adr = *DATA(r_3) + *DATA(r_6);
		
		*DATA(r_0) = (u32)((s8)READ8(DATA(cpu)->mem_if->data, adr));

		u8 c = MMU_aluMemAccessCycles<PROCNUM,8,MMU_AD_READ>(3, adr);
		GOTO_NEXTOP(c)
	}
};

//-----------------------------------------------------------------------------
//   STRH / LDRH
//-----------------------------------------------------------------------------
DCL_OP_START(OP_STRH_IMM_OFF)
	armcpu_t *cpu;
	u32 *r_0;
	u32 *r_3;
	u32 val;

	DCL_OP_COMPILER(OP_STRH_IMM_OFF)
		DATA(cpu) = GETCPU;
		DATA(r_0) = &(THUMB_REGPOS_R(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));
		DATA(val) = ((i>>5)&0x3E);

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_STRH_IMM_OFF)
		u32 adr = *DATA(r_3) + DATA(val);
		
		WRITE16(DATA(cpu)->mem_if->data, adr, (u16)*DATA(r_0));

		u8 c = MMU_aluMemAccessCycles<PROCNUM,16,MMU_AD_WRITE>(2, adr);
		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_LDRH_IMM_OFF)
	armcpu_t *cpu;
	u32 *r_0;
	u32 *r_3;
	u32 val;

	DCL_OP_COMPILER(OP_LDRH_IMM_OFF)
		DATA(cpu) = GETCPU;
		DATA(r_0) = &(THUMB_REGPOS_W(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));
		DATA(val) = ((i>>5)&0x3E);

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LDRH_IMM_OFF)
		u32 adr = *DATA(r_3) + DATA(val);
		
		*DATA(r_0) = (u32)READ16(DATA(cpu)->mem_if->data, adr);

		u8 c = MMU_aluMemAccessCycles<PROCNUM,16,MMU_AD_READ>(3, adr);
		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_STRH_REG_OFF)
	armcpu_t *cpu;
	u32 *r_0;
	u32 *r_3;
	u32 *r_6;

	DCL_OP_COMPILER(OP_STRH_REG_OFF)
		DATA(cpu) = GETCPU;
		DATA(r_0) = &(THUMB_REGPOS_R(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));
		DATA(r_6) = &(THUMB_REGPOS_R(i, 6));

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_STRH_REG_OFF)
		u32 adr = *DATA(r_3) + *DATA(r_6);
		
		WRITE16(DATA(cpu)->mem_if->data, adr, (u16)*DATA(r_0));

		u8 c = MMU_aluMemAccessCycles<PROCNUM,16,MMU_AD_WRITE>(2, adr);
		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_LDRH_REG_OFF)
	armcpu_t *cpu;
	u32 *r_0;
	u32 *r_3;
	u32 *r_6;

	DCL_OP_COMPILER(OP_LDRH_REG_OFF)
		DATA(cpu) = GETCPU;
		DATA(r_0) = &(THUMB_REGPOS_W(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));
		DATA(r_6) = &(THUMB_REGPOS_R(i, 6));

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LDRH_REG_OFF)
		u32 adr = *DATA(r_3) + *DATA(r_6);
		
		*DATA(r_0) = (u32)READ16(DATA(cpu)->mem_if->data, adr);

		u8 c = MMU_aluMemAccessCycles<PROCNUM,16,MMU_AD_READ>(3, adr);
		GOTO_NEXTOP(c)
	}
};

//-----------------------------------------------------------------------------
//   LDRSH
//-----------------------------------------------------------------------------
DCL_OP_START(OP_LDRSH_REG_OFF)
	armcpu_t *cpu;
	u32 *r_0;
	u32 *r_3;
	u32 *r_6;

	DCL_OP_COMPILER(OP_LDRSH_REG_OFF)
		DATA(cpu) = GETCPU;
		DATA(r_0) = &(THUMB_REGPOS_W(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));
		DATA(r_6) = &(THUMB_REGPOS_R(i, 6));

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LDRSH_REG_OFF)
		u32 adr = *DATA(r_3) + *DATA(r_6);
		
		*DATA(r_0) = (u32)((s16)READ16(DATA(cpu)->mem_if->data, adr));

		u8 c = MMU_aluMemAccessCycles<PROCNUM,16,MMU_AD_READ>(3, adr);
		GOTO_NEXTOP(c)
	}
};

//-----------------------------------------------------------------------------
//   STR / LDR
//-----------------------------------------------------------------------------
DCL_OP_START(OP_STR_IMM_OFF)
	armcpu_t *cpu;
	u32 *r_0;
	u32 *r_3;
	u32 val;

	DCL_OP_COMPILER(OP_STR_IMM_OFF)
		DATA(cpu) = GETCPU;
		DATA(r_0) = &(THUMB_REGPOS_R(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));
		DATA(val) = ((i>>4)&0x7C);

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_STR_IMM_OFF)
		u32 adr = *DATA(r_3) + DATA(val);
		
		WRITE32(DATA(cpu)->mem_if->data, adr, *DATA(r_0));

		u8 c = MMU_aluMemAccessCycles<PROCNUM,32,MMU_AD_WRITE>(2, adr);
		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_LDR_IMM_OFF)
	armcpu_t *cpu;
	u32 *r_0;
	u32 *r_3;
	u32 val;

	DCL_OP_COMPILER(OP_LDR_IMM_OFF)
		DATA(cpu) = GETCPU;
		DATA(r_0) = &(THUMB_REGPOS_W(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));
		DATA(val) = ((i>>4)&0x7C);

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LDR_IMM_OFF)
		u32 adr = *DATA(r_3) + DATA(val);
		u32 tempValue = READ32(DATA(cpu)->mem_if->data, adr);
		adr = (adr&3)*8;
		tempValue = (tempValue>>adr) | (tempValue<<(32-adr));
		*DATA(r_0) = tempValue;

		u8 c = MMU_aluMemAccessCycles<PROCNUM,32,MMU_AD_READ>(3, adr);
		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_STR_REG_OFF)
	armcpu_t *cpu;
	u32 *r_0;
	u32 *r_3;
	u32 *r_6;

	DCL_OP_COMPILER(OP_STR_REG_OFF)
		DATA(cpu) = GETCPU;
		DATA(r_0) = &(THUMB_REGPOS_R(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));
		DATA(r_6) = &(THUMB_REGPOS_R(i, 6));

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_STR_REG_OFF)
		u32 adr = *DATA(r_3) + *DATA(r_6);
		
		WRITE32(DATA(cpu)->mem_if->data, adr, *DATA(r_0));

		u8 c = MMU_aluMemAccessCycles<PROCNUM,32,MMU_AD_WRITE>(2, adr);
		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_LDR_REG_OFF)
	armcpu_t *cpu;
	u32 *r_0;
	u32 *r_3;
	u32 *r_6;

	DCL_OP_COMPILER(OP_LDR_REG_OFF)
		DATA(cpu) = GETCPU;
		DATA(r_0) = &(THUMB_REGPOS_W(i, 0));
		DATA(r_3) = &(THUMB_REGPOS_R(i, 3));
		DATA(r_6) = &(THUMB_REGPOS_R(i, 6));

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LDR_REG_OFF)
		u32 adr = *DATA(r_3) + *DATA(r_6);
		u32 tempValue = READ32(DATA(cpu)->mem_if->data, adr);
		adr = (adr&3)*8;
		tempValue = (tempValue>>adr) | (tempValue<<(32-adr));
		*DATA(r_0) = tempValue;

		u8 c = MMU_aluMemAccessCycles<PROCNUM,32,MMU_AD_READ>(3, adr);
		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_STR_SPREL)
	armcpu_t *cpu;
	u32 *r_8;
	u32 *r_13;
	u32 val;

	DCL_OP_COMPILER(OP_STR_SPREL)
		DATA(cpu) = GETCPU;
		DATA(r_8) = &(THUMB_REGPOS_R(i, 8));
		DATA(r_13) = &(GETCPUREG_R(13));
		DATA(val) = ((i&0xFF)<<2);

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_STR_SPREL)
		u32 adr = *DATA(r_13) + DATA(val);
		
		WRITE32(DATA(cpu)->mem_if->data, adr, *DATA(r_8));

		u8 c = MMU_aluMemAccessCycles<PROCNUM,32,MMU_AD_WRITE>(2, adr);
		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_LDR_SPREL)
	armcpu_t *cpu;
	u32 *r_8;
	u32 *r_13;
	u32 val;

	DCL_OP_COMPILER(OP_LDR_SPREL)
		DATA(cpu) = GETCPU;
		DATA(r_8) = &(THUMB_REGPOS_W(i, 8));
		DATA(r_13) = &(GETCPUREG_R(13));
		DATA(val) = ((i&0xFF)<<2);

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LDR_SPREL)
		u32 adr = *DATA(r_13) + DATA(val);
		
		*DATA(r_8) = READ32(DATA(cpu)->mem_if->data, adr);

		u8 c = MMU_aluMemAccessCycles<PROCNUM,32,MMU_AD_READ>(3, adr);
		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_LDR_PCREL)
	armcpu_t *cpu;
	u32 *r_8;
	u32 adr;

	DCL_OP_COMPILER(OP_LDR_PCREL)
		DATA(cpu) = GETCPU;
		DATA(r_8) = &(THUMB_REGPOS_W(i, 8));
		DATA(adr) = (common->R15&0xFFFFFFFC) + ((i&0xFF)<<2);

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LDR_PCREL)
		*DATA(r_8) = READ32(DATA(cpu)->mem_if->data, DATA(adr));

		u8 c = MMU_aluMemAccessCycles<PROCNUM,32,MMU_AD_READ>(3, DATA(adr));
		GOTO_NEXTOP(c)
	}
};

//-----------------------------------------------------------------------------
//   Adjust SP
//-----------------------------------------------------------------------------
DCL_OP_START(OP_ADJUST_P_SP)
	u32 *r_13;
	u32 val;

	DCL_OP_COMPILER(OP_ADJUST_P_SP)
		DATA(r_13) = &(GETCPUREG_RW(13));
		DATA(val) = ((i&0x7F)<<2);

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_ADJUST_P_SP)
		*DATA(r_13) += DATA(val);
		
		GOTO_NEXTOP(1)
	}
};

DCL_OP_START(OP_ADJUST_M_SP)
	u32 *r_13;
	u32 val;

	DCL_OP_COMPILER(OP_ADJUST_M_SP)
		DATA(r_13) = &(GETCPUREG_RW(13));
		DATA(val) = ((i&0x7F)<<2);

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_ADJUST_M_SP)
		*DATA(r_13) -= DATA(val);
		
		GOTO_NEXTOP(1)
	}
};

//-----------------------------------------------------------------------------
//   PUSH / POP
//-----------------------------------------------------------------------------
DCL_OP_START(OP_PUSH)
	armcpu_t *cpu;
	u32 *r_13;
	u32 *r[8+1];

	DCL_OP_COMPILER(OP_PUSH)
		DATA(cpu) = GETCPU;
		DATA(r_13) = &(GETCPUREG_RW(13));

		u32 count = 0;
		for(u32 j = 0; j<8; j++)
		{
			if(BIT_N(i, 7-j))
			{
				DATA(r[count++]) = &(GETCPUREG_R(7-j));
			}
		}
		DATA(r[count]) = NULL;

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_PUSH)
		u32 adr = *DATA(r_13) - 4;
		u8 c = 0;

		u32 count = 0;
		while (DATA(r[count]))
		{
			WRITE32(DATA(cpu)->mem_if->data, adr, *DATA(r[count++]));
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr);
			adr -= 4;
		}

		*DATA(r_13) = adr + 4;

		c = MMU_aluMemCycles<PROCNUM>(3, c);
		
		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_PUSH_LR)
	armcpu_t *cpu;
	u32 *r_13;
	u32 *r_14;
	u32 *r[8+1];

	DCL_OP_COMPILER(OP_PUSH_LR)
		DATA(cpu) = GETCPU;
		DATA(r_13) = &(GETCPUREG_RW(13));
		DATA(r_14) = &(GETCPUREG_R(14));

		u32 count = 0;
		for(u32 j = 0; j<8; j++)
		{
			if(BIT_N(i, 7-j))
			{
				DATA(r[count++]) = &(GETCPUREG_R(7-j));
			}
		}
		DATA(r[count]) = NULL;

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_PUSH_LR)
		u32 adr = *DATA(r_13) - 4;

		WRITE32(DATA(cpu)->mem_if->data, adr, *DATA(r_14));
		u8 c = MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr);
		adr -= 4;

		u32 count = 0;
		while (DATA(r[count]))
		{
			WRITE32(DATA(cpu)->mem_if->data, adr, *DATA(r[count++]));
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr);
			adr -= 4;
		}

		*DATA(r_13) = adr + 4;

		c = MMU_aluMemCycles<PROCNUM>(4, c);
		
		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_POP)
	armcpu_t *cpu;
	u32 *r_13;
	u32 *r[8+1];

	DCL_OP_COMPILER(OP_POP)
		DATA(cpu) = GETCPU;
		DATA(r_13) = &(GETCPUREG_RW(13));

		u32 count = 0;
		for(u32 j = 0; j<8; j++)
		{
			if(BIT_N(i, j))
			{
				DATA(r[count++]) = &(GETCPUREG_W(j));
			}
		}
		DATA(r[count]) = NULL;

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_POP)
		u32 adr = *DATA(r_13);
		u8 c = 0;

		u32 count = 0;
		while (DATA(r[count]))
		{
			*DATA(r[count++]) = READ32(DATA(cpu)->mem_if->data, adr);
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
			adr += 4;
		}

		*DATA(r_13) = adr;

		c = MMU_aluMemCycles<PROCNUM>(2, c);
		
		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_POP_PC)
	armcpu_t *cpu;
	Status_Reg *cpsr;
	u32 *r_13;
	u32 *r_15;
	u32 *r[8+1];

	DCL_OP_COMPILER(OP_POP_PC)
		DATA(cpu) = GETCPU;
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_13) = &(GETCPUREG_RW(13));
		DATA(r_15) = &(GETCPUREG_W(15));

		u32 count = 0;
		for(u32 j = 0; j<8; j++)
		{
			if(BIT_N(i, j))
			{
				DATA(r[count++]) = &(GETCPUREG_W(j));
			}
		}
		DATA(r[count]) = NULL;

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_POP_PC)
		u32 adr = *DATA(r_13);
		u8 c = 0;

		u32 count = 0;
		while (DATA(r[count]))
		{
			*DATA(r[count++]) = READ32(DATA(cpu)->mem_if->data, adr);
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
			adr += 4;
		}

		u32 v = READ32(DATA(cpu)->mem_if->data, adr);
		c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);

		if(PROCNUM==0)
			DATA(cpsr->bits.T) = BIT0(v);
		*DATA(r_15) = v & 0xFFFFFFFE;

		*DATA(r_13) = adr + 4;

		c = MMU_aluMemCycles<PROCNUM>(5, c);
		
		GOTO_NEXTOP(c)
	}
};

//-----------------------------------------------------------------------------
//   STMIA / LDMIA
//-----------------------------------------------------------------------------
DCL_OP_START(OP_STMIA_THUMB)
	armcpu_t *cpu;
	u32 *r_8;
	u32 *r[8+1];

	DCL_OP_COMPILER(OP_STMIA_THUMB)
		DATA(cpu) = GETCPU;
		DATA(r_8) = &(THUMB_REGPOS_RW(i, 8));

		u32 erList = 1; //Empty Register List

		if (BIT_N(i, (((i)>>8)&0x7)))
			printf("STMIA with Rb in Rlist\n");

		u32 count = 0;
		for(u32 j = 0; j<8; j++)
		{
			if(BIT_N(i, j))
			{
				DATA(r[count++]) = &(GETCPUREG_R(j));

				erList = 0; //Register List isnt empty
			}
		}
		DATA(r[count]) = NULL;

		if (erList)
			 printf("STMIA with Empty Rlist\n");

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_STMIA_THUMB)
		u32 adr = *DATA(r_8);
		u8 c = 0;

		u32 count = 0;
		while (DATA(r[count]))
		{
			WRITE32(DATA(cpu)->mem_if->data, adr, *DATA(r[count++]));
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr);
			adr += 4;
		}

		*DATA(r_8) = adr;

		c = MMU_aluMemCycles<PROCNUM>(2, c);
		
		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_LDMIA_THUMB)
	armcpu_t *cpu;
	u32 *r_8;
	u32 *r[8+1];
	bool write_back;

	DCL_OP_COMPILER(OP_LDMIA_THUMB)
		DATA(cpu) = GETCPU;

		u32 regIndex = (((i)>>8)&0x7);
		u32 erList = 1; //Empty Register List

		DATA(r_8) = &(GETCPUREG_RW(regIndex));
		DATA(write_back) = !BIT_N(i, regIndex);

		//if (BIT_N(i, regIndex))
		//	 printf("LDMIA with Rb in Rlist at %08X\n",cpu->instruct_adr);

		u32 count = 0;
		for(u32 j = 0; j<8; j++)
		{
			if(BIT_N(i, j))
			{
				DATA(r[count++]) = &(GETCPUREG_W(j));

				erList = 0; //Register List isnt empty
			}
		}
		DATA(r[count]) = NULL;

		if (erList)
			 printf("LDMIA with Empty Rlist\n");

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LDMIA_THUMB)
		u32 adr = *DATA(r_8);
		u8 c = 0;

		u32 count = 0;
		while (DATA(r[count]))
		{
			*DATA(r[count++]) = READ32(DATA(cpu)->mem_if->data, adr);
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
			adr += 4;
		}

		if (DATA(write_back))
			*DATA(r_8) = adr;

		c = MMU_aluMemCycles<PROCNUM>(3, c);
		
		GOTO_NEXTOP(c)
	}
};

//-----------------------------------------------------------------------------
//   BKPT
//-----------------------------------------------------------------------------
DCL_OP_START(OP_BKPT_THUMB)

	DCL_OP_COMPILER(OP_BKPT_THUMB)

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_BKPT_THUMB)
		printf("THUMB%c: Unimplemented opcode BKPT\n", PROCNUM?'7':'9');
		
		GOTO_NEXTOP(1)
	}
};

//-----------------------------------------------------------------------------
//   SWI
//-----------------------------------------------------------------------------
DCL_OP_START(OP_SWI_THUMB)
	armcpu_t *cpu;
	u32 swinum;

	DCL_OP_COMPILER(OP_SWI_THUMB)
		DATA(cpu) = GETCPU;
		DATA(swinum) = i & 0xFF;

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_SWI_THUMB)
		bool bypassBuiltinSWI = (DATA(cpu)->intVector == 0x00000000 && PROCNUM==0) 
								|| (DATA(cpu)->intVector == 0xFFFF0000 && PROCNUM==1);

		if(DATA(cpu)->swi_tab && !bypassBuiltinSWI)
		{
			u8 c = DATA(cpu)->swi_tab[DATA(swinum) & 0x1F]() + 3;

			GOTO_NEXTOP(c)
		}
		else
		{
			Status_Reg tmp = DATA(cpu)->CPSR;
			armcpu_switchMode(DATA(cpu), SVC);
			DATA(cpu)->R[14] = common->R15 - 2;
			DATA(cpu)->SPSR = tmp;
			DATA(cpu)->CPSR.bits.T = 0;
			DATA(cpu)->CPSR.bits.I = 1;
			DATA(cpu)->changeCPSR();
			DATA(cpu)->R[15] = DATA(cpu)->intVector + 0x08;

			GOTO_NEXTOP(3)
		}
	}
};

//-----------------------------------------------------------------------------
//   Branch
//-----------------------------------------------------------------------------
#define SIGNEEXT_IMM11(i)	(((i)&0x7FF) | (BIT10(i) * 0xFFFFF800))
#define SIGNEXTEND_11(i)	(((s32)i<<21)>>21)

DCL_OP_START(OP_B_COND)
	Status_Reg *cpsr;
	u32 *r_15;
	//u32 cond;
	u32 val;

	DCL_OP_COMPILER(OP_B_COND)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_15) = &(GETCPUREG_RW(15));
		//DATA(cond) = (i>>8)&0xF;
		DATA(val) = (u32)((s8)(i&0xFF))<<1;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_B_COND)
		//if(!TEST_COND(DATA(cond), 0, (*DATA(cpsr))))
		//{
		//	GOTO_NEXTOP(1)
		//}

		*DATA(r_15) += DATA(val);

		GOTO_NEXTOP(3)
	}
};

DCL_OP_START(OP_B_UNCOND)
	u32 *r_15;
	u32 val;

	DCL_OP_COMPILER(OP_B_UNCOND)
		DATA(r_15) = &(GETCPUREG_RW(15));
		DATA(val) = (SIGNEEXT_IMM11(i)<<1);
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_B_UNCOND)
		*DATA(r_15) += DATA(val);

		GOTO_NEXTOP(1)
	}
};

DCL_OP_START(OP_BLX)
	Status_Reg *cpsr;
	u32 *r_14;
	u32 *r_15;
	u32 val;

	DCL_OP_COMPILER(OP_BLX)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_14) = &(GETCPUREG_RW(14));
		DATA(r_15) = &(GETCPUREG_W(15));
		DATA(val) = ((i&0x7FF)<<1);
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_BLX)
		*DATA(r_15) = (*DATA(r_14) + DATA(val))&0xFFFFFFFC;
		*DATA(r_14) = (common->R15 - 2) | 1;
		DATA(cpsr)->bits.T = 0;

		GOTO_NEXTOP(3)
	}
};

DCL_OP_START(OP_BL_10)
	u32 *r_14;
	u32 *r_15;
	u32 val;

	DCL_OP_COMPILER(OP_BL_10)
		DATA(r_14) = &(GETCPUREG_W(14));
		DATA(r_15) = &(GETCPUREG_R(15));
		DATA(val) = (SIGNEXTEND_11(i)<<12);
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_BL_10)
		*DATA(r_14) = *DATA(r_15) + DATA(val);

		GOTO_NEXTOP(1)
	}
};

DCL_OP_START(OP_BL_11)
	u32 *r_14;
	u32 *r_15;
	u32 val;

	DCL_OP_COMPILER(OP_BL_11)
		DATA(r_14) = &(GETCPUREG_RW(14));
		DATA(r_15) = &(GETCPUREG_W(15));
		DATA(val) = ((i&0x7FF)<<1);
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_BL_11)
		*DATA(r_15) = *DATA(r_14) + DATA(val);
		*DATA(r_14) = ((common->R15 - 2)) | 1;

		GOTO_NEXTOP(4)
	}
};

DCL_OP_START(OP_BX_THUMB)
	Status_Reg *cpsr;
	u32 *r_3;
	u32 *r_15;

	DCL_OP_COMPILER(OP_BX_THUMB)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_3) = &(ARM_REGPOS_R(i, 3));
		DATA(r_15) = &(GETCPUREG_W(15));
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_BX_THUMB)
		u32 Rm = *DATA(r_3);
		
		DATA(cpsr)->bits.T = BIT0(Rm);
		*DATA(r_15) = (Rm & (0xFFFFFFFC|(1<<DATA(cpsr)->bits.T)));

		GOTO_NEXTOP(3)
	}
};

DCL_OP_START(OP_BLX_THUMB)
	Status_Reg *cpsr;
	u32 *r_3;
	u32 *r_14;
	u32 *r_15;

	DCL_OP_COMPILER(OP_BLX_THUMB)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_3) = &(ARM_REGPOS_R(i, 3));
		DATA(r_14) = &(GETCPUREG_W(14));
		DATA(r_15) = &(GETCPUREG_W(15));
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_BLX_THUMB)
		u32 Rm = *DATA(r_3);
		
		DATA(cpsr)->bits.T = BIT0(Rm);
		*DATA(r_15) = Rm & 0xFFFFFFFE;
		*DATA(r_14) = ((common->R15 - 2)) | 1;

		GOTO_NEXTOP(4)
	}
};

//------------------------------------------------------------
//                         ARM
//------------------------------------------------------------
#define DCL_OP2_ARG1(name, op1, op2, arg) \
	DCL_OP_START(name) \
		op1##_DATA \
		op2##_DATA \
		DCL_OP_COMPILER(name) \
			op1##_COMPILER \
			op2##_COMPILER \
			DONE_COMPILER \
		} \
		DCL_OP_METHOD(name) \
			op1; \
			op2(arg); \
		} \
	}; 

#define DCL_OP2_ARG2(name, op1, op2, arg1, arg2) \
	DCL_OP_START(name) \
		op1##_DATA \
		op2##_DATA \
		DCL_OP_COMPILER(name) \
			op1##_COMPILER \
			op2##_COMPILER \
			DONE_COMPILER \
		} \
		DCL_OP_METHOD(name) \
			op1; \
			op2(arg1, arg2); \
		} \
	}; 

#define DCL_OP2EX_ARG2(name, op1, op2, opex, arg1, arg2) \
	DCL_OP_START(name) \
		op1##_DATA \
		op2##_DATA \
		DCL_OP_COMPILER(name) \
			op1##_COMPILER \
			op2##_COMPILER \
			DONE_COMPILER \
		} \
		DCL_OP_METHOD(name) \
			op1; \
			opex; \
			op2(arg1, arg2); \
		} \
	}; 

//-----------------------------------------------------------------------------
//   Shifting macros
//-----------------------------------------------------------------------------
#define LSL_IMM_DATA \
	u32 *r_0; \
	u32 shift_op;
#define LSL_IMM_COMPILER \
	DATA(r_0) = &(ARM_REGPOS_R(i,0)); \
	DATA(shift_op) = ((i>>7)&0x1F);
#define LSL_IMM \
	u32 shift_op = (*DATA(r_0))<<(DATA(shift_op));

#define S_LSL_IMM_DATA \
	Status_Reg *cpsr; \
	u32 *r_0; \
	u32 shift_op;
#define S_LSL_IMM_COMPILER \
	DATA(cpsr) = &(GETCPU->CPSR); \
	DATA(r_0) = &(ARM_REGPOS_R(i,0)); \
	DATA(shift_op) = ((i>>7)&0x1F);
#define S_LSL_IMM \
	u32 shift_op = DATA(shift_op); \
	u32 c = DATA(cpsr)->bits.C; \
	if(shift_op==0) \
		shift_op=(*DATA(r_0)); \
	else \
	{ \
		c = BIT_N((*DATA(r_0)), 32-shift_op); \
		shift_op = (*DATA(r_0))<<shift_op; \
	}

#define LSL_REG_DATA \
	u32 *r_0; \
	u32 *r_8;
#define LSL_REG_COMPILER \
	DATA(r_0) = &(ARM_REGPOS_R(i,0)); \
	DATA(r_8) = &(ARM_REGPOS_R(i,8)); 
#define LSL_REG \
	u32 shift_op = (*DATA(r_8))&0xFF; \
	if(shift_op>=32) \
		shift_op=0; \
	else \
		shift_op=(*DATA(r_0))<<shift_op;

#define S_LSL_REG_DATA \
	Status_Reg *cpsr; \
	u32 *r_0; \
	u32 *r_8;
#define S_LSL_REG_COMPILER \
	DATA(cpsr) = &(GETCPU->CPSR); \
	DATA(r_0) = &(ARM_REGPOS_R(i,0)); \
	DATA(r_8) = &(ARM_REGPOS_R(i,8));
#define S_LSL_REG \
	u32 shift_op = (*DATA(r_8))&0xFF; \
	u32 c = DATA(cpsr)->bits.C; \
	if(shift_op==0) \
		shift_op=(*DATA(r_0)); \
	else \
	if(shift_op<32) \
	{ \
		c = BIT_N((*DATA(r_0)), 32-shift_op); \
		shift_op = (*DATA(r_0))<<shift_op; \
	} \
	else \
	if(shift_op==32) \
	{ \
		shift_op = 0; \
		c = BIT0((*DATA(r_0))); \
	} \
	else \
	{ \
		shift_op = 0; \
		c = 0; \
	}

#define LSR_IMM_DATA \
	u32 *r_0; \
	u32 shift_op;
#define LSR_IMM_COMPILER \
	DATA(r_0) = &(ARM_REGPOS_R(i,0)); \
	DATA(shift_op) = ((i>>7)&0x1F);
#define LSR_IMM \
	u32 shift_op = DATA(shift_op); \
	if(shift_op!=0) \
		shift_op = (*DATA(r_0))>>shift_op;

#define S_LSR_IMM_DATA \
	Status_Reg *cpsr; \
	u32 *r_0; \
	u32 shift_op;
#define S_LSR_IMM_COMPILER \
	DATA(cpsr) = &(GETCPU->CPSR); \
	DATA(r_0) = &(ARM_REGPOS_R(i,0)); \
	DATA(shift_op) = ((i>>7)&0x1F);
#define S_LSR_IMM \
	u32 shift_op = DATA(shift_op); \
	u32 c = DATA(cpsr)->bits.C; \
	if(shift_op==0) \
	{ \
		c = BIT31(*DATA(r_0)); \
	} \
	else \
	{ \
		c = BIT_N(*DATA(r_0), shift_op-1); \
		shift_op = *DATA(r_0)>>shift_op; \
	}

#define LSR_REG_DATA \
	u32 *r_0; \
	u32 *r_8;
#define LSR_REG_COMPILER \
	DATA(r_0) = &(ARM_REGPOS_R(i,0)); \
	DATA(r_8) = &(ARM_REGPOS_R(i,8)); 
#define LSR_REG \
	u32 shift_op = (*DATA(r_8))&0xFF; \
	if(shift_op>=32) \
		shift_op = 0; \
	else \
		shift_op = *DATA(r_0)>>shift_op;

#define S_LSR_REG_DATA \
	Status_Reg *cpsr; \
	u32 *r_0; \
	u32 *r_8;
#define S_LSR_REG_COMPILER \
	DATA(cpsr) = &(GETCPU->CPSR); \
	DATA(r_0) = &(ARM_REGPOS_R(i,0)); \
	DATA(r_8) = &(ARM_REGPOS_R(i,8));
#define S_LSR_REG \
	u32 shift_op = (*DATA(r_8))&0xFF; \
	u32 c = DATA(cpsr)->bits.C; \
	if(shift_op==0) \
	{ \
		shift_op = *DATA(r_0); \
	} \
	else \
	if(shift_op<32) \
	{ \
		c = BIT_N(*DATA(r_0), shift_op-1); \
		shift_op = *DATA(r_0)>>shift_op; \
	} \
	else \
	if(shift_op==32) \
	{ \
		c = BIT31(*DATA(r_0)); \
		shift_op = 0; \
	} \
	else \
	{ \
		c = 0; \
		shift_op = 0; \
	}

#define ASR_IMM_DATA \
	u32 *r_0; \
	u32 shift_op;
#define ASR_IMM_COMPILER \
	DATA(r_0) = &(ARM_REGPOS_R(i,0)); \
	DATA(shift_op) = ((i>>7)&0x1F);
#define ASR_IMM \
	u32 shift_op = DATA(shift_op); \
	if(shift_op==0) \
		shift_op=BIT31(*DATA(r_0))*0xFFFFFFFF; \
	else \
		shift_op = (u32)((s32)*DATA(r_0)>>shift_op);

#define S_ASR_IMM_DATA \
	Status_Reg *cpsr; \
	u32 *r_0; \
	u32 shift_op;
#define S_ASR_IMM_COMPILER \
	DATA(cpsr) = &(GETCPU->CPSR); \
	DATA(r_0) = &(ARM_REGPOS_R(i,0)); \
	DATA(shift_op) = ((i>>7)&0x1F);
#define S_ASR_IMM \
	u32 shift_op = DATA(shift_op); \
	u32 c = DATA(cpsr)->bits.C; \
	if(shift_op==0) \
	{ \
		shift_op=BIT31(*DATA(r_0))*0xFFFFFFFF; \
		c = BIT31(*DATA(r_0)); \
	} \
	else \
	{ \
		c = BIT_N(*DATA(r_0), shift_op-1); \
		shift_op = (u32)((s32)*DATA(r_0)>>shift_op); \
	}

#define ASR_REG_DATA \
	u32 *r_0; \
	u32 *r_8;
#define ASR_REG_COMPILER \
	DATA(r_0) = &(ARM_REGPOS_R(i,0)); \
	DATA(r_8) = &(ARM_REGPOS_R(i,8)); 
#define ASR_REG \
	u32 shift_op = (*DATA(r_8))&0xFF; \
	if(shift_op==0) \
		shift_op=*DATA(r_0); \
	else \
	if(shift_op<32) \
		shift_op = (u32)((s32)*DATA(r_0)>>shift_op); \
	else \
		shift_op=BIT31(*DATA(r_0))*0xFFFFFFFF;

#define S_ASR_REG_DATA \
	Status_Reg *cpsr; \
	u32 *r_0; \
	u32 *r_8;
#define S_ASR_REG_COMPILER \
	DATA(cpsr) = &(GETCPU->CPSR); \
	DATA(r_0) = &(ARM_REGPOS_R(i,0)); \
	DATA(r_8) = &(ARM_REGPOS_R(i,8));
#define S_ASR_REG \
	u32 shift_op = (*DATA(r_8))&0xFF; \
	u32 c = DATA(cpsr)->bits.C; \
	if(shift_op==0) \
		shift_op=*DATA(r_0); \
	else \
	if(shift_op<32) \
	{ \
		c = BIT_N(*DATA(r_0), shift_op-1); \
		shift_op = (u32)((s32)*DATA(r_0)>>shift_op); \
	} \
	else \
	{ \
		c = BIT31(*DATA(r_0)); \
		shift_op=BIT31(*DATA(r_0))*0xFFFFFFFF; \
	}

#define ROR_IMM_DATA \
	Status_Reg *cpsr; \
	u32 *r_0; \
	u32 shift_op;
#define ROR_IMM_COMPILER \
	DATA(cpsr) = &(GETCPU->CPSR); \
	DATA(r_0) = &(ARM_REGPOS_R(i,0)); \
	DATA(shift_op) = ((i>>7)&0x1F);
#define ROR_IMM \
	u32 shift_op = DATA(shift_op); \
	if(shift_op==0) \
	{ \
		shift_op = ((u32)DATA(cpsr)->bits.C<<31)|(*DATA(r_0)>>1); \
	} \
	else \
		shift_op = ROR(*DATA(r_0),shift_op);

#define ROR_IMM2_DATA \
	u32 *r_0; \
	u32 shift_op;
#define ROR_IMM2_COMPILER \
	DATA(r_0) = &(ARM_REGPOS_R(i,0)); \
	DATA(shift_op) = ((i>>7)&0x1F);
#define ROR_IMM2 \
	u32 shift_op = DATA(shift_op); \
	if(shift_op==0) \
	{ \
		shift_op = ((u32)DATA(cpsr)->bits.C<<31)|(*DATA(r_0)>>1); \
	} \
	else \
		shift_op = ROR(*DATA(r_0),shift_op);

#define S_ROR_IMM_DATA \
	Status_Reg *cpsr; \
	u32 *r_0; \
	u32 shift_op;
#define S_ROR_IMM_COMPILER \
	DATA(cpsr) = &(GETCPU->CPSR); \
	DATA(r_0) = &(ARM_REGPOS_R(i,0)); \
	DATA(shift_op) = ((i>>7)&0x1F);
#define S_ROR_IMM \
	u32 shift_op = DATA(shift_op); \
	u32 c = DATA(cpsr)->bits.C; \
	if(shift_op==0) \
	{ \
		shift_op = ((u32)DATA(cpsr)->bits.C<<31)|(*DATA(r_0)>>1); \
		c = BIT0(*DATA(r_0)); \
	} \
	else \
	{ \
		c = BIT_N(*DATA(r_0), shift_op-1); \
		shift_op = ROR(*DATA(r_0),shift_op); \
	}

#define ROR_REG_DATA \
	u32 *r_0; \
	u32 *r_8;
#define ROR_REG_COMPILER \
	DATA(r_0) = &(ARM_REGPOS_R(i,0)); \
	DATA(r_8) = &(ARM_REGPOS_R(i,8)); 
#define ROR_REG \
	u32 shift_op = (*DATA(r_8))&0xFF; \
	if((shift_op==0)||((shift_op&0x1F)==0)) \
		shift_op=*DATA(r_0); \
	else \
		shift_op = ROR(*DATA(r_0),(shift_op&0x1F));

#define S_ROR_REG_DATA \
	Status_Reg *cpsr; \
	u32 *r_0; \
	u32 *r_8;
#define S_ROR_REG_COMPILER \
	DATA(cpsr) = &(GETCPU->CPSR); \
	DATA(r_0) = &(ARM_REGPOS_R(i,0)); \
	DATA(r_8) = &(ARM_REGPOS_R(i,8));
#define S_ROR_REG \
	u32 shift_op = (*DATA(r_8))&0xFF; \
	u32 c = DATA(cpsr)->bits.C; \
	if(shift_op==0) \
		shift_op=*DATA(r_0); \
	else \
	{ \
		shift_op&=0x1F; \
		if(shift_op==0) \
		{ \
			shift_op=*DATA(r_0); \
			c = BIT31(*DATA(r_0)); \
		} \
		else \
		{ \
			c = BIT_N(*DATA(r_0), shift_op-1); \
			shift_op = ROR(*DATA(r_0),shift_op); \
		} \
	}

#define IMM_VALUE_DATA \
	u32 shift_op; 
#define IMM_VALUE_COMPILER \
	DATA(shift_op) = ROR((i&0xFF), (i>>7)&0x1E);
#define IMM_VALUE \
	u32 shift_op = DATA(shift_op);

#define S_IMM_VALUE_DATA \
	Status_Reg *cpsr; \
	u32 shift_op; \
	u32 val; 
#define S_IMM_VALUE_COMPILER \
	DATA(cpsr) = &(GETCPU->CPSR); \
	DATA(shift_op) = ROR((i&0xFF), (i>>7)&0x1E); \
	DATA(val) = (i>>8)&0xF;
#define S_IMM_VALUE \
	u32 shift_op = DATA(shift_op); \
	u32 c = DATA(cpsr)->bits.C; \
	if(DATA(val)) \
		c = BIT31(shift_op);

//-----------------------------------------------------------------------------
//   Undefined instruction
//-----------------------------------------------------------------------------
DCL_OP_START(OP_UND)
	armcpu_t *cpu;

	DCL_OP_COMPILER(OP_UND)
		DATA(cpu) = GETCPU;

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_UND)
		TRAPUNDEF(DATA(cpu));

		GOTO_NEXTOP(1)
	}
};

//-----------------------------------------------------------------------------
//   AND / ANDS
//   Timing: OK
//-----------------------------------------------------------------------------
#define OP_AND_DATA \
	u32 *r_12; \
	u32 *r_16; \
	bool mod_r15; 
#define OP_AND_COMPILER \
	DATA(r_12) = &(ARM_REGPOS_W(i,12)); \
	DATA(r_16) = &(ARM_REGPOS_R(i,16)); \
	DATA(mod_r15) = REG_POS(i,12) == 15;
#define OP_AND(a, b) \
	*DATA(r_12) = *DATA(r_16) & shift_op; \
	if(DATA(mod_r15)) \
	{ \
		GOTO_NEXTOP(b); \
	} \
	GOTO_NEXTOP(a);

#define OP_ANDS_DATA \
	armcpu_t *cpu; \
	u32 *r_12; \
	u32 *r_16; \
	bool mod_r15; 
#define OP_ANDS_COMPILER \
	DATA(cpu) = GETCPU; \
	DATA(r_12) = &(ARM_REGPOS_W(i,12)); \
	DATA(r_16) = &(ARM_REGPOS_R(i,16)); \
	DATA(mod_r15) = REG_POS(i,12) == 15;
#define OP_ANDS(a, b) \
	*DATA(r_12) = *DATA(r_16) & shift_op; \
	if(DATA(mod_r15)) \
	{ \
		Status_Reg SPSR = DATA(cpu)->SPSR; \
		armcpu_switchMode(DATA(cpu), SPSR.bits.mode); \
		DATA(cpu)->CPSR=SPSR; \
		DATA(cpu)->changeCPSR(); \
		DATA(cpu)->R[15] &= (0xFFFFFFFC|(((u32)DATA(cpsr)->bits.T)<<1)); \
		GOTO_NEXTOP(b); \
	} \
	DATA(cpsr)->bits.C = c; \
	DATA(cpsr)->bits.N = BIT31(*DATA(r_12)); \
	DATA(cpsr)->bits.Z = (*DATA(r_12)==0); \
	GOTO_NEXTOP(a);

DCL_OP2_ARG2(OP_AND_LSL_IMM, LSL_IMM, OP_AND, 1, 3)
DCL_OP2_ARG2(OP_AND_LSL_REG, LSL_REG, OP_AND, 2, 4)
DCL_OP2_ARG2(OP_AND_LSR_IMM, LSR_IMM, OP_AND, 1, 3)
DCL_OP2_ARG2(OP_AND_LSR_REG, LSR_REG, OP_AND, 2, 4)
DCL_OP2_ARG2(OP_AND_ASR_IMM, ASR_IMM, OP_AND, 1, 3)
DCL_OP2_ARG2(OP_AND_ASR_REG, ASR_REG, OP_AND, 2, 4)
DCL_OP2_ARG2(OP_AND_ROR_IMM, ROR_IMM, OP_AND, 1, 3)
DCL_OP2_ARG2(OP_AND_ROR_REG, ROR_REG, OP_AND, 2, 4)
DCL_OP2_ARG2(OP_AND_IMM_VAL, IMM_VALUE, OP_AND, 1, 3)

DCL_OP2_ARG2(OP_AND_S_LSL_IMM, S_LSL_IMM, OP_ANDS, 1, 3)
DCL_OP2_ARG2(OP_AND_S_LSL_REG, S_LSL_REG, OP_ANDS, 2, 4)
DCL_OP2_ARG2(OP_AND_S_LSR_IMM, S_LSR_IMM, OP_ANDS, 1, 3)
DCL_OP2_ARG2(OP_AND_S_LSR_REG, S_LSR_REG, OP_ANDS, 2, 4)
DCL_OP2_ARG2(OP_AND_S_ASR_IMM, S_ASR_IMM, OP_ANDS, 1, 3)
DCL_OP2_ARG2(OP_AND_S_ASR_REG, S_ASR_REG, OP_ANDS, 2, 4)
DCL_OP2_ARG2(OP_AND_S_ROR_IMM, S_ROR_IMM, OP_ANDS, 1, 3)
DCL_OP2_ARG2(OP_AND_S_ROR_REG, S_ROR_REG, OP_ANDS, 2, 4)
DCL_OP2_ARG2(OP_AND_S_IMM_VAL, S_IMM_VALUE, OP_ANDS, 1, 3)

//-----------------------------------------------------------------------------
//   EOR / EORS
//-----------------------------------------------------------------------------
#define OP_EOR_DATA \
	u32 *r_12; \
	u32 *r_16; \
	bool mod_r15; 
#define OP_EOR_COMPILER \
	DATA(r_12) = &(ARM_REGPOS_W(i,12)); \
	DATA(r_16) = &(ARM_REGPOS_R(i,16)); \
	DATA(mod_r15) = REG_POS(i,12) == 15;
#define OP_EOR(a, b) \
	*DATA(r_12) = *DATA(r_16) ^ shift_op; \
	if(DATA(mod_r15)) \
	{ \
		GOTO_NEXTOP(b); \
	} \
	GOTO_NEXTOP(a); 

#define OP_EORS_DATA \
	armcpu_t *cpu; \
	u32 *r_12; \
	u32 *r_16; \
	bool mod_r15; 
#define OP_EORS_COMPILER \
	DATA(cpu) = GETCPU; \
	DATA(r_12) = &(ARM_REGPOS_W(i,12)); \
	DATA(r_16) = &(ARM_REGPOS_R(i,16)); \
	DATA(mod_r15) = REG_POS(i,12) == 15;
#define OP_EORS(a, b) \
	*DATA(r_12) = *DATA(r_16) ^ shift_op; \
	if(DATA(mod_r15)) \
	{ \
		Status_Reg SPSR = DATA(cpu)->SPSR; \
		armcpu_switchMode(DATA(cpu), SPSR.bits.mode); \
		DATA(cpu)->CPSR=SPSR; \
		DATA(cpu)->changeCPSR(); \
		DATA(cpu)->R[15] &= (0xFFFFFFFC|(((u32)DATA(cpsr)->bits.T)<<1)); \
		GOTO_NEXTOP(b); \
	} \
	DATA(cpsr)->bits.C = c; \
	DATA(cpsr)->bits.N = BIT31(*DATA(r_12)); \
	DATA(cpsr)->bits.Z = (*DATA(r_12)==0); \
	GOTO_NEXTOP(a);

DCL_OP2_ARG2(OP_EOR_LSL_IMM, LSL_IMM, OP_EOR, 1, 3)
DCL_OP2_ARG2(OP_EOR_LSL_REG, LSL_REG, OP_EOR, 2, 4)
DCL_OP2_ARG2(OP_EOR_LSR_IMM, LSR_IMM, OP_EOR, 1, 3)
DCL_OP2_ARG2(OP_EOR_LSR_REG, LSR_REG, OP_EOR, 2, 4)
DCL_OP2_ARG2(OP_EOR_ASR_IMM, ASR_IMM, OP_EOR, 1, 3)
DCL_OP2_ARG2(OP_EOR_ASR_REG, ASR_REG, OP_EOR, 2, 4)
DCL_OP2_ARG2(OP_EOR_ROR_IMM, ROR_IMM, OP_EOR, 1, 3)
DCL_OP2_ARG2(OP_EOR_ROR_REG, ROR_REG, OP_EOR, 2, 4)
DCL_OP2_ARG2(OP_EOR_IMM_VAL, IMM_VALUE, OP_EOR, 1, 3)

DCL_OP2_ARG2(OP_EOR_S_LSL_IMM, S_LSL_IMM, OP_EORS, 1, 3)
DCL_OP2_ARG2(OP_EOR_S_LSL_REG, S_LSL_REG, OP_EORS, 2, 4)
DCL_OP2_ARG2(OP_EOR_S_LSR_IMM, S_LSR_IMM, OP_EORS, 1, 3)
DCL_OP2_ARG2(OP_EOR_S_LSR_REG, S_LSR_REG, OP_EORS, 2, 4)
DCL_OP2_ARG2(OP_EOR_S_ASR_IMM, S_ASR_IMM, OP_EORS, 1, 3)
DCL_OP2_ARG2(OP_EOR_S_ASR_REG, S_ASR_REG, OP_EORS, 2, 4)
DCL_OP2_ARG2(OP_EOR_S_ROR_IMM, S_ROR_IMM, OP_EORS, 1, 3)
DCL_OP2_ARG2(OP_EOR_S_ROR_REG, S_ROR_REG, OP_EORS, 2, 4)
DCL_OP2_ARG2(OP_EOR_S_IMM_VAL, S_IMM_VALUE, OP_EORS, 1, 3)

//-----------------------------------------------------------------------------
//   SUB / SUBS
//-----------------------------------------------------------------------------
#define OP_SUB_DATA \
	u32 *r_12; \
	u32 *r_16; \
	bool mod_r15; 
#define OP_SUB_COMPILER \
	DATA(r_12) = &(ARM_REGPOS_W(i,12)); \
	DATA(r_16) = &(ARM_REGPOS_R(i,16)); \
	DATA(mod_r15) = REG_POS(i,12) == 15;
#define OP_SUB(a, b) \
	*DATA(r_12) = *DATA(r_16) - shift_op; \
	if(DATA(mod_r15)) \
	{ \
		GOTO_NEXTOP(b); \
	} \
	GOTO_NEXTOP(a);

#define OP_SUBS_DATA \
	armcpu_t *cpu; \
	Status_Reg *cpsr; \
	u32 *r_12; \
	u32 *r_16; \
	bool mod_r15; 
#define OP_SUBS_COMPILER \
	DATA(cpu) = GETCPU; \
	DATA(cpsr) = &(GETCPU->CPSR); \
	DATA(r_12) = &(ARM_REGPOS_W(i,12)); \
	DATA(r_16) = &(ARM_REGPOS_R(i,16)); \
	DATA(mod_r15) = REG_POS(i,12) == 15;
#define OP_SUBS(a, b) \
	u32 v = *DATA(r_16); \
	*DATA(r_12) = v - shift_op; \
	if(DATA(mod_r15)) \
	{ \
		Status_Reg SPSR = DATA(cpu)->SPSR; \
		armcpu_switchMode(DATA(cpu), SPSR.bits.mode); \
		DATA(cpu)->CPSR=SPSR; \
		DATA(cpu)->changeCPSR(); \
		DATA(cpu)->R[15] &= (0xFFFFFFFC|(((u32)DATA(cpsr)->bits.T)<<1)); \
		GOTO_NEXTOP(b); \
	} \
	DATA(cpsr)->bits.N = BIT31(*DATA(r_12)); \
	DATA(cpsr)->bits.Z = (*DATA(r_12)==0); \
	DATA(cpsr)->bits.C = !BorrowFrom(v, shift_op); \
	DATA(cpsr)->bits.V = OverflowFromSUB(*DATA(r_12), v, shift_op); \
	GOTO_NEXTOP(a);

DCL_OP2_ARG2(OP_SUB_LSL_IMM, LSL_IMM, OP_SUB, 1, 3)
DCL_OP2_ARG2(OP_SUB_LSL_REG, LSL_REG, OP_SUB, 2, 4)
DCL_OP2_ARG2(OP_SUB_LSR_IMM, LSR_IMM, OP_SUB, 1, 3)
DCL_OP2_ARG2(OP_SUB_LSR_REG, LSR_REG, OP_SUB, 2, 4)
DCL_OP2_ARG2(OP_SUB_ASR_IMM, ASR_IMM, OP_SUB, 1, 3)
DCL_OP2_ARG2(OP_SUB_ASR_REG, ASR_REG, OP_SUB, 2, 4)
DCL_OP2_ARG2(OP_SUB_ROR_IMM, ROR_IMM, OP_SUB, 1, 3)
DCL_OP2_ARG2(OP_SUB_ROR_REG, ROR_REG, OP_SUB, 2, 4)
DCL_OP2_ARG2(OP_SUB_IMM_VAL, IMM_VALUE, OP_SUB, 1, 3)

DCL_OP2_ARG2(OP_SUB_S_LSL_IMM, LSL_IMM, OP_SUBS, 1, 3)
DCL_OP2_ARG2(OP_SUB_S_LSL_REG, LSL_REG, OP_SUBS, 2, 4)
DCL_OP2_ARG2(OP_SUB_S_LSR_IMM, LSR_IMM, OP_SUBS, 1, 3)
DCL_OP2_ARG2(OP_SUB_S_LSR_REG, LSR_REG, OP_SUBS, 2, 4)
DCL_OP2_ARG2(OP_SUB_S_ASR_IMM, ASR_IMM, OP_SUBS, 1, 3)
DCL_OP2_ARG2(OP_SUB_S_ASR_REG, ASR_REG, OP_SUBS, 2, 4)
DCL_OP2_ARG2(OP_SUB_S_ROR_IMM, ROR_IMM2, OP_SUBS, 1, 3)
DCL_OP2_ARG2(OP_SUB_S_ROR_REG, ROR_REG, OP_SUBS, 2, 4)
DCL_OP2_ARG2(OP_SUB_S_IMM_VAL, IMM_VALUE, OP_SUBS, 1, 3)

//-----------------------------------------------------------------------------
//   RSB / RSBS
//-----------------------------------------------------------------------------
#define OP_RSB_DATA \
	u32 *r_12; \
	u32 *r_16; \
	bool mod_r15; 
#define OP_RSB_COMPILER \
	DATA(r_12) = &(ARM_REGPOS_W(i,12)); \
	DATA(r_16) = &(ARM_REGPOS_R(i,16)); \
	DATA(mod_r15) = REG_POS(i,12) == 15;
#define OP_RSB(a, b) \
	*DATA(r_12) = shift_op - *DATA(r_16); \
	if(DATA(mod_r15)) \
	{ \
		GOTO_NEXTOP(b); \
	} \
	GOTO_NEXTOP(a);

#define OP_RSBS_DATA \
	armcpu_t *cpu; \
	Status_Reg *cpsr; \
	u32 *r_12; \
	u32 *r_16; \
	bool mod_r15; 
#define OP_RSBS_COMPILER \
	DATA(cpu) = GETCPU; \
	DATA(cpsr) = &(GETCPU->CPSR); \
	DATA(r_12) = &(ARM_REGPOS_W(i,12)); \
	DATA(r_16) = &(ARM_REGPOS_R(i,16)); \
	DATA(mod_r15) = REG_POS(i,12) == 15;
#define OP_RSBS(a, b) \
	u32 v = *DATA(r_16); \
	*DATA(r_12) = shift_op - v; \
	if(DATA(mod_r15)) \
	{ \
		Status_Reg SPSR = DATA(cpu)->SPSR; \
		armcpu_switchMode(DATA(cpu), SPSR.bits.mode); \
		DATA(cpu)->CPSR=SPSR; \
		DATA(cpu)->changeCPSR(); \
		DATA(cpu)->R[15] &= (0xFFFFFFFC|(((u32)DATA(cpsr)->bits.T)<<1)); \
		GOTO_NEXTOP(b); \
	} \
	DATA(cpsr)->bits.N = BIT31(*DATA(r_12)); \
	DATA(cpsr)->bits.Z = (*DATA(r_12)==0); \
	DATA(cpsr)->bits.C = !BorrowFrom(shift_op, v); \
	DATA(cpsr)->bits.V = OverflowFromSUB(*DATA(r_12), shift_op, v); \
	GOTO_NEXTOP(a);

DCL_OP2_ARG2(OP_RSB_LSL_IMM, LSL_IMM, OP_RSB, 1, 3)
DCL_OP2_ARG2(OP_RSB_LSL_REG, LSL_REG, OP_RSB, 2, 4)
DCL_OP2_ARG2(OP_RSB_LSR_IMM, LSR_IMM, OP_RSB, 1, 3)
DCL_OP2_ARG2(OP_RSB_LSR_REG, LSR_REG, OP_RSB, 2, 4)
DCL_OP2_ARG2(OP_RSB_ASR_IMM, ASR_IMM, OP_RSB, 1, 3)
DCL_OP2_ARG2(OP_RSB_ASR_REG, ASR_REG, OP_RSB, 2, 4)
DCL_OP2_ARG2(OP_RSB_ROR_IMM, ROR_IMM, OP_RSB, 1, 3)
DCL_OP2_ARG2(OP_RSB_ROR_REG, ROR_REG, OP_RSB, 2, 4)
DCL_OP2_ARG2(OP_RSB_IMM_VAL, IMM_VALUE, OP_RSB, 1, 3)

DCL_OP2_ARG2(OP_RSB_S_LSL_IMM, LSL_IMM, OP_RSBS, 1, 3)
DCL_OP2_ARG2(OP_RSB_S_LSL_REG, LSL_REG, OP_RSBS, 2, 4)
DCL_OP2_ARG2(OP_RSB_S_LSR_IMM, LSR_IMM, OP_RSBS, 1, 3)
DCL_OP2_ARG2(OP_RSB_S_LSR_REG, LSR_REG, OP_RSBS, 2, 4)
DCL_OP2_ARG2(OP_RSB_S_ASR_IMM, ASR_IMM, OP_RSBS, 1, 3)
DCL_OP2_ARG2(OP_RSB_S_ASR_REG, ASR_REG, OP_RSBS, 2, 4)
DCL_OP2_ARG2(OP_RSB_S_ROR_IMM, ROR_IMM2, OP_RSBS, 1, 3)
DCL_OP2_ARG2(OP_RSB_S_ROR_REG, ROR_REG, OP_RSBS, 2, 4)
DCL_OP2_ARG2(OP_RSB_S_IMM_VAL, IMM_VALUE, OP_RSBS, 1, 3)

//-----------------------------------------------------------------------------
//   ADD / ADDS
//-----------------------------------------------------------------------------
#define OP_ADD_DATA \
	u32 *r_12; \
	u32 *r_16; \
	bool mod_r15; 
#define OP_ADD_COMPILER \
	DATA(r_12) = &(ARM_REGPOS_W(i,12)); \
	DATA(r_16) = &(ARM_REGPOS_R(i,16)); \
	DATA(mod_r15) = REG_POS(i,12) == 15;
#define OP_ADD(a, b) \
	*DATA(r_12) = *DATA(r_16) + shift_op; \
	if(DATA(mod_r15)) \
	{ \
		GOTO_NEXTOP(b); \
	} \
	GOTO_NEXTOP(a);

#define OP_ADDS_DATA \
	armcpu_t *cpu; \
	Status_Reg *cpsr; \
	u32 *r_12; \
	u32 *r_16; \
	bool mod_r15; 
#define OP_ADDS_COMPILER \
	DATA(cpu) = GETCPU; \
	DATA(cpsr) = &(GETCPU->CPSR); \
	DATA(r_12) = &(ARM_REGPOS_W(i,12)); \
	DATA(r_16) = &(ARM_REGPOS_R(i,16)); \
	DATA(mod_r15) = REG_POS(i,12) == 15;
#define OP_ADDS(a, b) \
	u32 v = *DATA(r_16); \
	*DATA(r_12) = v + shift_op; \
	if(DATA(mod_r15)) \
	{ \
		Status_Reg SPSR = DATA(cpu)->SPSR; \
		armcpu_switchMode(DATA(cpu), SPSR.bits.mode); \
		DATA(cpu)->CPSR=SPSR; \
		DATA(cpu)->changeCPSR(); \
		DATA(cpu)->R[15] &= (0xFFFFFFFC|(((u32)DATA(cpsr)->bits.T)<<1)); \
		GOTO_NEXTOP(b); \
	} \
	DATA(cpsr)->bits.N = BIT31(*DATA(r_12)); \
	DATA(cpsr)->bits.Z = (*DATA(r_12)==0); \
	DATA(cpsr)->bits.C = CarryFrom(v, shift_op); \
	DATA(cpsr)->bits.V = OverflowFromADD(*DATA(r_12), v, shift_op); \
	GOTO_NEXTOP(a);

DCL_OP2_ARG2(OP_ADD_LSL_IMM, LSL_IMM, OP_ADD, 1, 3)
DCL_OP2_ARG2(OP_ADD_LSL_REG, LSL_REG, OP_ADD, 2, 4)
DCL_OP2_ARG2(OP_ADD_LSR_IMM, LSR_IMM, OP_ADD, 1, 3)
DCL_OP2_ARG2(OP_ADD_LSR_REG, LSR_REG, OP_ADD, 2, 4)
DCL_OP2_ARG2(OP_ADD_ASR_IMM, ASR_IMM, OP_ADD, 1, 3)
DCL_OP2_ARG2(OP_ADD_ASR_REG, ASR_REG, OP_ADD, 2, 4)
DCL_OP2_ARG2(OP_ADD_ROR_IMM, ROR_IMM, OP_ADD, 1, 3)
DCL_OP2_ARG2(OP_ADD_ROR_REG, ROR_REG, OP_ADD, 2, 4)
DCL_OP2_ARG2(OP_ADD_IMM_VAL, IMM_VALUE, OP_ADD, 1, 3)

DCL_OP2_ARG2(OP_ADD_S_LSL_IMM, LSL_IMM, OP_ADDS, 1, 3)
DCL_OP2_ARG2(OP_ADD_S_LSL_REG, LSL_REG, OP_ADDS, 2, 4)
DCL_OP2_ARG2(OP_ADD_S_LSR_IMM, LSR_IMM, OP_ADDS, 1, 3)
DCL_OP2_ARG2(OP_ADD_S_LSR_REG, LSR_REG, OP_ADDS, 2, 4)
DCL_OP2_ARG2(OP_ADD_S_ASR_IMM, ASR_IMM, OP_ADDS, 1, 3)
DCL_OP2_ARG2(OP_ADD_S_ASR_REG, ASR_REG, OP_ADDS, 2, 4)
DCL_OP2_ARG2(OP_ADD_S_ROR_IMM, ROR_IMM2, OP_ADDS, 1, 3)
DCL_OP2_ARG2(OP_ADD_S_ROR_REG, ROR_REG, OP_ADDS, 2, 4)
DCL_OP2_ARG2(OP_ADD_S_IMM_VAL, IMM_VALUE, OP_ADDS, 1, 3)

//-----------------------------------------------------------------------------
//   ADC / ADCS
//-----------------------------------------------------------------------------
#define OP_ADC_DATA \
	Status_Reg *cpsr; \
	u32 *r_12; \
	u32 *r_16; \
	bool mod_r15; 
#define OP_ADC_COMPILER \
	DATA(cpsr) = &(GETCPU->CPSR); \
	DATA(r_12) = &(ARM_REGPOS_W(i,12)); \
	DATA(r_16) = &(ARM_REGPOS_R(i,16)); \
	DATA(mod_r15) = REG_POS(i,12) == 15;
#define OP_ADC(a, b) \
	*DATA(r_12) = *DATA(r_16) + shift_op + DATA(cpsr)->bits.C; \
	if(DATA(mod_r15)) \
	{ \
		GOTO_NEXTOP(b); \
	} \
	GOTO_NEXTOP(a);

#define OP_ADCS_DATA \
	armcpu_t *cpu; \
	Status_Reg *cpsr; \
	u32 *r_12; \
	u32 *r_16; \
	bool mod_r15; 
#define OP_ADCS_COMPILER \
	DATA(cpu) = GETCPU; \
	DATA(cpsr) = &(GETCPU->CPSR); \
	DATA(r_12) = &(ARM_REGPOS_W(i,12)); \
	DATA(r_16) = &(ARM_REGPOS_R(i,16)); \
	DATA(mod_r15) = REG_POS(i,12) == 15;
#define OP_ADCS(a, b) \
	u32 v = *DATA(r_16); \
	if(DATA(mod_r15)) \
	{ \
		*DATA(r_12) = v + shift_op + DATA(cpsr)->bits.C; \
		Status_Reg SPSR = DATA(cpu)->SPSR; \
		armcpu_switchMode(DATA(cpu), SPSR.bits.mode); \
		DATA(cpu)->CPSR=SPSR; \
		DATA(cpu)->changeCPSR(); \
		DATA(cpu)->R[15] &= (0xFFFFFFFC|(((u32)DATA(cpsr)->bits.T)<<1)); \
		GOTO_NEXTOP(b); \
	} \
	if (!DATA(cpsr)->bits.C) \
	{ \
		*DATA(r_12) = v + shift_op; \
		DATA(cpsr)->bits.C = *DATA(r_12) < v; \
	} \
	else \
	{ \
		*DATA(r_12) = v + shift_op + 1; \
		DATA(cpsr)->bits.C = *DATA(r_12) <= v; \
	} \
	DATA(cpsr)->bits.N = BIT31(*DATA(r_12)); \
	DATA(cpsr)->bits.Z = (*DATA(r_12)==0); \
	DATA(cpsr)->bits.V = BIT31((v ^ shift_op ^ -1) & (v ^ *DATA(r_12)));\
	GOTO_NEXTOP(a); 

DCL_OP2_ARG2(OP_ADC_LSL_IMM, LSL_IMM, OP_ADC, 1, 3)
DCL_OP2_ARG2(OP_ADC_LSL_REG, LSL_REG, OP_ADC, 2, 4)
DCL_OP2_ARG2(OP_ADC_LSR_IMM, LSR_IMM, OP_ADC, 1, 3)
DCL_OP2_ARG2(OP_ADC_LSR_REG, LSR_REG, OP_ADC, 2, 4)
DCL_OP2_ARG2(OP_ADC_ASR_IMM, ASR_IMM, OP_ADC, 1, 3)
DCL_OP2_ARG2(OP_ADC_ASR_REG, ASR_REG, OP_ADC, 2, 4)
DCL_OP2_ARG2(OP_ADC_ROR_IMM, ROR_IMM2, OP_ADC, 1, 3)
DCL_OP2_ARG2(OP_ADC_ROR_REG, ROR_REG, OP_ADC, 2, 4)
DCL_OP2_ARG2(OP_ADC_IMM_VAL, IMM_VALUE, OP_ADC, 1, 3)

DCL_OP2_ARG2(OP_ADC_S_LSL_IMM, LSL_IMM, OP_ADCS, 1, 3)
DCL_OP2_ARG2(OP_ADC_S_LSL_REG, LSL_REG, OP_ADCS, 2, 4)
DCL_OP2_ARG2(OP_ADC_S_LSR_IMM, LSR_IMM, OP_ADCS, 1, 3)
DCL_OP2_ARG2(OP_ADC_S_LSR_REG, LSR_REG, OP_ADCS, 2, 4)
DCL_OP2_ARG2(OP_ADC_S_ASR_IMM, ASR_IMM, OP_ADCS, 1, 3)
DCL_OP2_ARG2(OP_ADC_S_ASR_REG, ASR_REG, OP_ADCS, 2, 4)
DCL_OP2_ARG2(OP_ADC_S_ROR_IMM, ROR_IMM2, OP_ADCS, 1, 3)
DCL_OP2_ARG2(OP_ADC_S_ROR_REG, ROR_REG, OP_ADCS, 2, 4)
DCL_OP2_ARG2(OP_ADC_S_IMM_VAL, IMM_VALUE, OP_ADCS, 1, 3)

//-----------------------------------------------------------------------------
//   SBC / SBCS
//-----------------------------------------------------------------------------
#define OP_SBC_DATA \
	Status_Reg *cpsr; \
	u32 *r_12; \
	u32 *r_16; \
	bool mod_r15; 
#define OP_SBC_COMPILER \
	DATA(cpsr) = &(GETCPU->CPSR); \
	DATA(r_12) = &(ARM_REGPOS_W(i,12)); \
	DATA(r_16) = &(ARM_REGPOS_R(i,16)); \
	DATA(mod_r15) = REG_POS(i,12) == 15;
#define OP_SBC(a, b) \
	*DATA(r_12) = *DATA(r_16) - shift_op - !DATA(cpsr)->bits.C; \
	if(DATA(mod_r15)) \
	{ \
		GOTO_NEXTOP(b); \
	} \
	GOTO_NEXTOP(a);

#define OP_SBCS_DATA \
	armcpu_t *cpu; \
	Status_Reg *cpsr; \
	u32 *r_12; \
	u32 *r_16; \
	bool mod_r15; 
#define OP_SBCS_COMPILER \
	DATA(cpu) = GETCPU; \
	DATA(cpsr) = &(GETCPU->CPSR); \
	DATA(r_12) = &(ARM_REGPOS_W(i,12)); \
	DATA(r_16) = &(ARM_REGPOS_R(i,16)); \
	DATA(mod_r15) = REG_POS(i,12) == 15;
#define OP_SBCS(a, b) \
	u32 v = *DATA(r_16); \
	if(DATA(mod_r15)) \
	{ \
		*DATA(r_12) = v - shift_op - !DATA(cpsr)->bits.C; \
		Status_Reg SPSR = DATA(cpu)->SPSR; \
		armcpu_switchMode(DATA(cpu), SPSR.bits.mode); \
		DATA(cpu)->CPSR=SPSR; \
		DATA(cpu)->changeCPSR(); \
		DATA(cpu)->R[15] &= (0xFFFFFFFC|(((u32)DATA(cpsr)->bits.T)<<1)); \
		GOTO_NEXTOP(b); \
	} \
	if (!DATA(cpsr)->bits.C) \
	{ \
		*DATA(r_12) = v - shift_op - 1; \
		DATA(cpsr)->bits.C = v > shift_op; \
	} \
	else \
	{ \
		*DATA(r_12) = v - shift_op; \
		DATA(cpsr)->bits.C = v >= shift_op; \
	} \
	DATA(cpsr)->bits.N = BIT31(*DATA(r_12)); \
	DATA(cpsr)->bits.Z = (*DATA(r_12)==0); \
	DATA(cpsr)->bits.V = BIT31((v ^ shift_op) & (v ^ *DATA(r_12))); \
	GOTO_NEXTOP(a);

DCL_OP2_ARG2(OP_SBC_LSL_IMM, LSL_IMM, OP_SBC, 1, 3)
DCL_OP2_ARG2(OP_SBC_LSL_REG, LSL_REG, OP_SBC, 2, 4)
DCL_OP2_ARG2(OP_SBC_LSR_IMM, LSR_IMM, OP_SBC, 1, 3)
DCL_OP2_ARG2(OP_SBC_LSR_REG, LSR_REG, OP_SBC, 2, 4)
DCL_OP2_ARG2(OP_SBC_ASR_IMM, ASR_IMM, OP_SBC, 1, 3)
DCL_OP2_ARG2(OP_SBC_ASR_REG, ASR_REG, OP_SBC, 2, 4)
DCL_OP2_ARG2(OP_SBC_ROR_IMM, ROR_IMM2, OP_SBC, 1, 3)
DCL_OP2_ARG2(OP_SBC_ROR_REG, ROR_REG, OP_SBC, 2, 4)
DCL_OP2_ARG2(OP_SBC_IMM_VAL, IMM_VALUE, OP_SBC, 1, 3)

DCL_OP2_ARG2(OP_SBC_S_LSL_IMM, LSL_IMM, OP_SBCS, 1, 3)
DCL_OP2_ARG2(OP_SBC_S_LSL_REG, LSL_REG, OP_SBCS, 2, 4)
DCL_OP2_ARG2(OP_SBC_S_LSR_IMM, LSR_IMM, OP_SBCS, 1, 3)
DCL_OP2_ARG2(OP_SBC_S_LSR_REG, LSR_REG, OP_SBCS, 2, 4)
DCL_OP2_ARG2(OP_SBC_S_ASR_IMM, ASR_IMM, OP_SBCS, 1, 3)
DCL_OP2_ARG2(OP_SBC_S_ASR_REG, ASR_REG, OP_SBCS, 2, 4)
DCL_OP2_ARG2(OP_SBC_S_ROR_IMM, ROR_IMM2, OP_SBCS, 1, 3)
DCL_OP2_ARG2(OP_SBC_S_ROR_REG, ROR_REG, OP_SBCS, 2, 4)
DCL_OP2_ARG2(OP_SBC_S_IMM_VAL, IMM_VALUE, OP_SBCS, 1, 3)

//-----------------------------------------------------------------------------
//   RSC / RSCS
//-----------------------------------------------------------------------------
#define OP_RSC_DATA \
	Status_Reg *cpsr; \
	u32 *r_12; \
	u32 *r_16; \
	bool mod_r15; 
#define OP_RSC_COMPILER \
	DATA(cpsr) = &(GETCPU->CPSR); \
	DATA(r_12) = &(ARM_REGPOS_W(i,12)); \
	DATA(r_16) = &(ARM_REGPOS_R(i,16)); \
	DATA(mod_r15) = REG_POS(i,12) == 15;
#define OP_RSC(a, b) \
	*DATA(r_12) =  shift_op - *DATA(r_16) + DATA(cpsr)->bits.C - 1; \
	if(DATA(mod_r15)) \
	{ \
		GOTO_NEXTOP(b); \
	} \
	GOTO_NEXTOP(a);

#define OP_RSCS_DATA \
	armcpu_t *cpu; \
	Status_Reg *cpsr; \
	u32 *r_12; \
	u32 *r_16; \
	bool mod_r15; 
#define OP_RSCS_COMPILER \
	DATA(cpu) = GETCPU; \
	DATA(cpsr) = &(GETCPU->CPSR); \
	DATA(r_12) = &(ARM_REGPOS_W(i,12)); \
	DATA(r_16) = &(ARM_REGPOS_R(i,16)); \
	DATA(mod_r15) = REG_POS(i,12) == 15;
#define OP_RSCS(a, b) \
	u32 v = *DATA(r_16); \
	if(DATA(mod_r15)) \
	{ \
		*DATA(r_12) = shift_op - v - !DATA(cpsr)->bits.C; \
		Status_Reg SPSR = DATA(cpu)->SPSR; \
		armcpu_switchMode(DATA(cpu), SPSR.bits.mode); \
		DATA(cpu)->CPSR=SPSR; \
		DATA(cpu)->changeCPSR(); \
		DATA(cpu)->R[15] &= (0xFFFFFFFC|(((u32)DATA(cpsr)->bits.T)<<1)); \
		GOTO_NEXTOP(b); \
	} \
	if (!DATA(cpsr)->bits.C) \
	{ \
		*DATA(r_12) = shift_op - v - 1; \
		DATA(cpsr)->bits.C = shift_op > v; \
	} \
	else \
	{ \
		*DATA(r_12) = shift_op - v; \
		DATA(cpsr)->bits.C = shift_op >= v; \
	} \
	DATA(cpsr)->bits.N = BIT31(*DATA(r_12)); \
	DATA(cpsr)->bits.Z = (*DATA(r_12)==0); \
	DATA(cpsr)->bits.V = BIT31((shift_op ^ v) & (shift_op ^ *DATA(r_12))); \
	GOTO_NEXTOP(a); 

DCL_OP2_ARG2(OP_RSC_LSL_IMM, LSL_IMM, OP_RSC, 1, 3)
DCL_OP2_ARG2(OP_RSC_LSL_REG, LSL_REG, OP_RSC, 2, 4)
DCL_OP2_ARG2(OP_RSC_LSR_IMM, LSR_IMM, OP_RSC, 1, 3)
DCL_OP2_ARG2(OP_RSC_LSR_REG, LSR_REG, OP_RSC, 2, 4)
DCL_OP2_ARG2(OP_RSC_ASR_IMM, ASR_IMM, OP_RSC, 1, 3)
DCL_OP2_ARG2(OP_RSC_ASR_REG, ASR_REG, OP_RSC, 2, 4)
DCL_OP2_ARG2(OP_RSC_ROR_IMM, ROR_IMM2, OP_RSC, 1, 3)
DCL_OP2_ARG2(OP_RSC_ROR_REG, ROR_REG, OP_RSC, 2, 4)
DCL_OP2_ARG2(OP_RSC_IMM_VAL, IMM_VALUE, OP_RSC, 1, 3)

DCL_OP2_ARG2(OP_RSC_S_LSL_IMM, LSL_IMM, OP_RSCS, 1, 3)
DCL_OP2_ARG2(OP_RSC_S_LSL_REG, LSL_REG, OP_RSCS, 2, 4)
DCL_OP2_ARG2(OP_RSC_S_LSR_IMM, LSR_IMM, OP_RSCS, 1, 3)
DCL_OP2_ARG2(OP_RSC_S_LSR_REG, LSR_REG, OP_RSCS, 2, 4)
DCL_OP2_ARG2(OP_RSC_S_ASR_IMM, ASR_IMM, OP_RSCS, 1, 3)
DCL_OP2_ARG2(OP_RSC_S_ASR_REG, ASR_REG, OP_RSCS, 2, 4)
DCL_OP2_ARG2(OP_RSC_S_ROR_IMM, ROR_IMM2, OP_RSCS, 1, 3)
DCL_OP2_ARG2(OP_RSC_S_ROR_REG, ROR_REG, OP_RSCS, 2, 4)
DCL_OP2_ARG2(OP_RSC_S_IMM_VAL, IMM_VALUE, OP_RSCS, 1, 3)

//-----------------------------------------------------------------------------
//   TST
//-----------------------------------------------------------------------------
#define OP_TST_DATA \
	u32 *r_16;
#define OP_TST_COMPILER \
	DATA(r_16) = &(ARM_REGPOS_R(i,16));
#define OP_TST(a) \
	{ \
	u32 tmp = *DATA(r_16) & shift_op; \
	DATA(cpsr)->bits.C = c; \
	DATA(cpsr)->bits.N = BIT31(tmp); \
	DATA(cpsr)->bits.Z = (tmp==0); \
	GOTO_NEXTOP(a); \
	}

DCL_OP2_ARG1(OP_TST_LSL_IMM, S_LSL_IMM, OP_TST, 1)
DCL_OP2_ARG1(OP_TST_LSL_REG, S_LSL_REG, OP_TST, 2)
DCL_OP2_ARG1(OP_TST_LSR_IMM, S_LSR_IMM, OP_TST, 1)
DCL_OP2_ARG1(OP_TST_LSR_REG, S_LSR_REG, OP_TST, 2)
DCL_OP2_ARG1(OP_TST_ASR_IMM, S_ASR_IMM, OP_TST, 1)
DCL_OP2_ARG1(OP_TST_ASR_REG, S_ASR_REG, OP_TST, 2)
DCL_OP2_ARG1(OP_TST_ROR_IMM, S_ROR_IMM, OP_TST, 1)
DCL_OP2_ARG1(OP_TST_ROR_REG, S_ROR_REG, OP_TST, 2)
DCL_OP2_ARG1(OP_TST_IMM_VAL, S_IMM_VALUE, OP_TST, 1)

//-----------------------------------------------------------------------------
//   TEQ
//-----------------------------------------------------------------------------
#define OP_TEQ_DATA \
	u32 *r_16;
#define OP_TEQ_COMPILER \
	DATA(r_16) = &(ARM_REGPOS_R(i,16));
#define OP_TEQ(a) \
	{ \
	u32 tmp = *DATA(r_16) ^ shift_op; \
	DATA(cpsr)->bits.C = c; \
	DATA(cpsr)->bits.N = BIT31(tmp); \
	DATA(cpsr)->bits.Z = (tmp==0); \
	GOTO_NEXTOP(a); \
	}

DCL_OP2_ARG1(OP_TEQ_LSL_IMM, S_LSL_IMM, OP_TEQ, 1)
DCL_OP2_ARG1(OP_TEQ_LSL_REG, S_LSL_REG, OP_TEQ, 2)
DCL_OP2_ARG1(OP_TEQ_LSR_IMM, S_LSR_IMM, OP_TEQ, 1)
DCL_OP2_ARG1(OP_TEQ_LSR_REG, S_LSR_REG, OP_TEQ, 2)
DCL_OP2_ARG1(OP_TEQ_ASR_IMM, S_ASR_IMM, OP_TEQ, 1)
DCL_OP2_ARG1(OP_TEQ_ASR_REG, S_ASR_REG, OP_TEQ, 2)
DCL_OP2_ARG1(OP_TEQ_ROR_IMM, S_ROR_IMM, OP_TEQ, 1)
DCL_OP2_ARG1(OP_TEQ_ROR_REG, S_ROR_REG, OP_TEQ, 2)
DCL_OP2_ARG1(OP_TEQ_IMM_VAL, S_IMM_VALUE, OP_TEQ, 1)

//-----------------------------------------------------------------------------
//   CMP
//-----------------------------------------------------------------------------
#define OP_CMP_DATA \
	Status_Reg *cpsr; \
	u32 *r_16;
#define OP_CMP_COMPILER \
	DATA(cpsr) = &(GETCPU->CPSR); \
	DATA(r_16) = &(ARM_REGPOS_R(i,16));
#define OP_CMP(a) \
	{ \
	u32 tmp = *DATA(r_16) - shift_op; \
	DATA(cpsr)->bits.N = BIT31(tmp); \
	DATA(cpsr)->bits.Z = (tmp==0); \
	DATA(cpsr)->bits.C = !BorrowFrom(*DATA(r_16), shift_op); \
	DATA(cpsr)->bits.V = OverflowFromSUB(tmp, *DATA(r_16), shift_op); \
	GOTO_NEXTOP(a); \
	}

DCL_OP2_ARG1(OP_CMP_LSL_IMM, LSL_IMM, OP_CMP, 1)
DCL_OP2_ARG1(OP_CMP_LSL_REG, LSL_REG, OP_CMP, 2)
DCL_OP2_ARG1(OP_CMP_LSR_IMM, LSR_IMM, OP_CMP, 1)
DCL_OP2_ARG1(OP_CMP_LSR_REG, LSR_REG, OP_CMP, 2)
DCL_OP2_ARG1(OP_CMP_ASR_IMM, ASR_IMM, OP_CMP, 1)
DCL_OP2_ARG1(OP_CMP_ASR_REG, ASR_REG, OP_CMP, 2)
DCL_OP2_ARG1(OP_CMP_ROR_IMM, ROR_IMM2, OP_CMP, 1)
DCL_OP2_ARG1(OP_CMP_ROR_REG, ROR_REG, OP_CMP, 2)
DCL_OP2_ARG1(OP_CMP_IMM_VAL, IMM_VALUE, OP_CMP, 1)

//-----------------------------------------------------------------------------
//   CMN
//-----------------------------------------------------------------------------
#define OP_CMN_DATA \
	Status_Reg *cpsr; \
	u32 *r_16;
#define OP_CMN_COMPILER \
	DATA(cpsr) = &(GETCPU->CPSR); \
	DATA(r_16) = &(ARM_REGPOS_R(i,16));
#define OP_CMN(a) \
	{ \
	u32 tmp = *DATA(r_16) + shift_op; \
	DATA(cpsr)->bits.N = BIT31(tmp); \
	DATA(cpsr)->bits.Z = (tmp==0); \
	DATA(cpsr)->bits.C = CarryFrom(*DATA(r_16), shift_op); \
	DATA(cpsr)->bits.V = OverflowFromADD(tmp, *DATA(r_16), shift_op); \
	GOTO_NEXTOP(a); \
	}

DCL_OP2_ARG1(OP_CMN_LSL_IMM, LSL_IMM, OP_CMN, 1)
DCL_OP2_ARG1(OP_CMN_LSL_REG, LSL_REG, OP_CMN, 2)
DCL_OP2_ARG1(OP_CMN_LSR_IMM, LSR_IMM, OP_CMN, 1)
DCL_OP2_ARG1(OP_CMN_LSR_REG, LSR_REG, OP_CMN, 2)
DCL_OP2_ARG1(OP_CMN_ASR_IMM, ASR_IMM, OP_CMN, 1)
DCL_OP2_ARG1(OP_CMN_ASR_REG, ASR_REG, OP_CMN, 2)
DCL_OP2_ARG1(OP_CMN_ROR_IMM, ROR_IMM2, OP_CMN, 1)
DCL_OP2_ARG1(OP_CMN_ROR_REG, ROR_REG, OP_CMN, 2)
DCL_OP2_ARG1(OP_CMN_IMM_VAL, IMM_VALUE, OP_CMN, 1)

//-----------------------------------------------------------------------------
//   ORR / ORRS
//-----------------------------------------------------------------------------
#define OP_ORR_DATA \
	u32 *r_12; \
	u32 *r_16; \
	bool mod_r15; 
#define OP_ORR_COMPILER \
	DATA(r_12) = &(ARM_REGPOS_W(i,12)); \
	DATA(r_16) = &(ARM_REGPOS_R(i,16)); \
	DATA(mod_r15) = REG_POS(i,12) == 15;
#define OP_ORR(a, b) \
	*DATA(r_12) = *DATA(r_16) | shift_op; \
	if(DATA(mod_r15)) \
	{ \
		GOTO_NEXTOP(b); \
	} \
	GOTO_NEXTOP(a);

#define OP_ORRS_DATA \
	armcpu_t *cpu; \
	u32 *r_12; \
	u32 *r_16; \
	bool mod_r15; 
#define OP_ORRS_COMPILER \
	DATA(cpu) = GETCPU; \
	DATA(r_12) = &(ARM_REGPOS_W(i,12)); \
	DATA(r_16) = &(ARM_REGPOS_R(i,16)); \
	DATA(mod_r15) = REG_POS(i,12) == 15;
#define OP_ORRS(a,b) \
	{ \
	*DATA(r_12) = *DATA(r_16) | shift_op; \
	if(DATA(mod_r15)) \
	{ \
		Status_Reg SPSR = DATA(cpu)->SPSR; \
		armcpu_switchMode(DATA(cpu), SPSR.bits.mode); \
		DATA(cpu)->CPSR=SPSR; \
		DATA(cpu)->changeCPSR(); \
		DATA(cpu)->R[15] &= (0xFFFFFFFC|(((u32)DATA(cpsr)->bits.T)<<1)); \
		GOTO_NEXTOP(b); \
	} \
	DATA(cpsr)->bits.C = c; \
	DATA(cpsr)->bits.N = BIT31(*DATA(r_12)); \
	DATA(cpsr)->bits.Z = (*DATA(r_12)==0); \
	GOTO_NEXTOP(a); \
	}

DCL_OP2_ARG2(OP_ORR_LSL_IMM, LSL_IMM, OP_ORR, 1, 3)
DCL_OP2_ARG2(OP_ORR_LSL_REG, LSL_REG, OP_ORR, 2, 4)
DCL_OP2_ARG2(OP_ORR_LSR_IMM, LSR_IMM, OP_ORR, 1, 3)
DCL_OP2_ARG2(OP_ORR_LSR_REG, LSR_REG, OP_ORR, 2, 4)
DCL_OP2_ARG2(OP_ORR_ASR_IMM, ASR_IMM, OP_ORR, 1, 3)
DCL_OP2_ARG2(OP_ORR_ASR_REG, ASR_REG, OP_ORR, 2, 4)
DCL_OP2_ARG2(OP_ORR_ROR_IMM, ROR_IMM, OP_ORR, 1, 3)
DCL_OP2_ARG2(OP_ORR_ROR_REG, ROR_REG, OP_ORR, 2, 4)
DCL_OP2_ARG2(OP_ORR_IMM_VAL, IMM_VALUE, OP_ORR, 1, 3)

DCL_OP2_ARG2(OP_ORR_S_LSL_IMM, S_LSL_IMM, OP_ORRS, 1, 3)
DCL_OP2_ARG2(OP_ORR_S_LSL_REG, S_LSL_REG, OP_ORRS, 2, 4)
DCL_OP2_ARG2(OP_ORR_S_LSR_IMM, S_LSR_IMM, OP_ORRS, 1, 3)
DCL_OP2_ARG2(OP_ORR_S_LSR_REG, S_LSR_REG, OP_ORRS, 2, 4)
DCL_OP2_ARG2(OP_ORR_S_ASR_IMM, S_ASR_IMM, OP_ORRS, 1, 3)
DCL_OP2_ARG2(OP_ORR_S_ASR_REG, S_ASR_REG, OP_ORRS, 2, 4)
DCL_OP2_ARG2(OP_ORR_S_ROR_IMM, S_ROR_IMM, OP_ORRS, 1, 3)
DCL_OP2_ARG2(OP_ORR_S_ROR_REG, S_ROR_REG, OP_ORRS, 2, 4)
DCL_OP2_ARG2(OP_ORR_S_IMM_VAL, S_IMM_VALUE, OP_ORRS, 1, 3)

//-----------------------------------------------------------------------------
//   MOV / MOVS
//-----------------------------------------------------------------------------
#define OP_MOV_DATA \
	u32 *r_12; \
	bool mod_r15; \
	bool p0_r15; 
#define OP_MOV_COMPILER \
	DATA(r_12) = &(ARM_REGPOS_W(i,12)); \
	DATA(mod_r15) = REG_POS(i,12) == 15; \
	DATA(p0_r15) = REG_POS(i,0) == 15;
#define OP_MOV(a, b) \
	*DATA(r_12) = shift_op; \
	if(DATA(mod_r15)) \
	{ \
		GOTO_NEXTOP(b); \
	} \
	GOTO_NEXTOP(a);

#define OP_MOVS_DATA \
	armcpu_t *cpu; \
	u32 *r_12; \
	bool mod_r15; \
	bool p0_r15; 
#define OP_MOVS_COMPILER \
	DATA(cpu) = GETCPU; \
	DATA(r_12) = &(ARM_REGPOS_W(i,12)); \
	DATA(mod_r15) = REG_POS(i,12) == 15; \
	DATA(p0_r15) = REG_POS(i,0) == 15;
#define OP_MOVS(a, b) \
	*DATA(r_12) = shift_op; \
	if(DATA(mod_r15)) \
	{ \
		Status_Reg SPSR = DATA(cpu)->SPSR; \
		armcpu_switchMode(DATA(cpu), SPSR.bits.mode); \
		DATA(cpu)->CPSR=SPSR; \
		DATA(cpu)->changeCPSR(); \
		DATA(cpu)->R[15] &= (0xFFFFFFFC|(((u32)DATA(cpsr)->bits.T)<<1)); \
		GOTO_NEXTOP(b); \
	} \
	DATA(cpsr)->bits.C = c; \
	DATA(cpsr)->bits.N = BIT31(*DATA(r_12)); \
	DATA(cpsr)->bits.Z = (*DATA(r_12)==0); \
	GOTO_NEXTOP(a);

#define OPEX_MOV \
	if (DATA(p0_r15)) shift_op += 4;

DCL_OP2_ARG2(OP_MOV_LSL_IMM, LSL_IMM, OP_MOV, 1, 3)
DCL_OP2EX_ARG2(OP_MOV_LSL_REG, LSL_REG, OP_MOV, OPEX_MOV, 2, 4)
DCL_OP2_ARG2(OP_MOV_LSR_IMM, LSR_IMM, OP_MOV, 1, 3)
DCL_OP2EX_ARG2(OP_MOV_LSR_REG, LSR_REG, OP_MOV, OPEX_MOV, 2, 4)
DCL_OP2_ARG2(OP_MOV_ASR_IMM, ASR_IMM, OP_MOV, 1, 3)
DCL_OP2_ARG2(OP_MOV_ASR_REG, ASR_REG, OP_MOV, 2, 4)
DCL_OP2_ARG2(OP_MOV_ROR_IMM, ROR_IMM, OP_MOV, 1, 3)
DCL_OP2_ARG2(OP_MOV_ROR_REG, ROR_REG, OP_MOV, 2, 4)
DCL_OP2_ARG2(OP_MOV_IMM_VAL, IMM_VALUE, OP_MOV, 1, 3)

DCL_OP2_ARG2(OP_MOV_S_LSL_IMM, S_LSL_IMM, OP_MOVS, 1, 3)
DCL_OP2EX_ARG2(OP_MOV_S_LSL_REG, S_LSL_REG, OP_MOVS, OPEX_MOV, 2, 4)
DCL_OP2_ARG2(OP_MOV_S_LSR_IMM, S_LSR_IMM, OP_MOVS, 1, 3)
DCL_OP2EX_ARG2(OP_MOV_S_LSR_REG, S_LSR_REG, OP_MOVS, OPEX_MOV, 2, 4)
DCL_OP2_ARG2(OP_MOV_S_ASR_IMM, S_ASR_IMM, OP_MOVS, 1, 3)
DCL_OP2_ARG2(OP_MOV_S_ASR_REG, S_ASR_REG, OP_MOVS, 2, 4)
DCL_OP2_ARG2(OP_MOV_S_ROR_IMM, S_ROR_IMM, OP_MOVS, 1, 3)
DCL_OP2_ARG2(OP_MOV_S_ROR_REG, S_ROR_REG, OP_MOVS, 2, 4)
DCL_OP2_ARG2(OP_MOV_S_IMM_VAL, S_IMM_VALUE, OP_MOVS, 1, 3)

//-----------------------------------------------------------------------------
//   BIC / BICS
//-----------------------------------------------------------------------------
#define OP_BIC_DATA \
	u32 *r_12; \
	u32 *r_16; \
	bool mod_r15; 
#define OP_BIC_COMPILER \
	DATA(r_12) = &(ARM_REGPOS_W(i,12)); \
	DATA(r_16) = &(ARM_REGPOS_R(i,16)); \
	DATA(mod_r15) = REG_POS(i,12) == 15; 
#define OP_BIC(a, b) \
	*DATA(r_12) = *DATA(r_16) & (~shift_op); \
	if(DATA(mod_r15)) \
	{ \
		GOTO_NEXTOP(b); \
	} \
	GOTO_NEXTOP(a);

#define OP_BICS_DATA \
	armcpu_t *cpu; \
	u32 *r_12; \
	u32 *r_16; \
	bool mod_r15; 
#define OP_BICS_COMPILER \
	DATA(cpu) = GETCPU; \
	DATA(r_12) = &(ARM_REGPOS_W(i,12)); \
	DATA(r_16) = &(ARM_REGPOS_R(i,16)); \
	DATA(mod_r15) = REG_POS(i,12) == 15; 
#define OP_BICS(a, b) \
	*DATA(r_12) = *DATA(r_16) & (~shift_op); \
	if(DATA(mod_r15)) \
	{ \
		Status_Reg SPSR = DATA(cpu)->SPSR; \
		armcpu_switchMode(DATA(cpu), SPSR.bits.mode); \
		DATA(cpu)->CPSR=SPSR; \
		DATA(cpu)->changeCPSR(); \
		DATA(cpu)->R[15] &= (0xFFFFFFFC|(((u32)DATA(cpsr)->bits.T)<<1)); \
		GOTO_NEXTOP(b); \
	} \
	DATA(cpsr)->bits.C = c; \
	DATA(cpsr)->bits.N = BIT31(*DATA(r_12)); \
	DATA(cpsr)->bits.Z = (*DATA(r_12)==0); \
	GOTO_NEXTOP(a);

DCL_OP2_ARG2(OP_BIC_LSL_IMM, LSL_IMM, OP_BIC, 1, 3)
DCL_OP2_ARG2(OP_BIC_LSL_REG, LSL_REG, OP_BIC, 2, 4)
DCL_OP2_ARG2(OP_BIC_LSR_IMM, LSR_IMM, OP_BIC, 1, 3)
DCL_OP2_ARG2(OP_BIC_LSR_REG, LSR_REG, OP_BIC, 2, 4)
DCL_OP2_ARG2(OP_BIC_ASR_IMM, ASR_IMM, OP_BIC, 1, 3)
DCL_OP2_ARG2(OP_BIC_ASR_REG, ASR_REG, OP_BIC, 2, 4)
DCL_OP2_ARG2(OP_BIC_ROR_IMM, ROR_IMM, OP_BIC, 1, 3)
DCL_OP2_ARG2(OP_BIC_ROR_REG, ROR_REG, OP_BIC, 2, 4)
DCL_OP2_ARG2(OP_BIC_IMM_VAL, IMM_VALUE, OP_BIC, 1, 3)

DCL_OP2_ARG2(OP_BIC_S_LSL_IMM, S_LSL_IMM, OP_BICS, 1, 3)
DCL_OP2_ARG2(OP_BIC_S_LSL_REG, S_LSL_REG, OP_BICS, 2, 4)
DCL_OP2_ARG2(OP_BIC_S_LSR_IMM, S_LSR_IMM, OP_BICS, 1, 3)
DCL_OP2_ARG2(OP_BIC_S_LSR_REG, S_LSR_REG, OP_BICS, 2, 4)
DCL_OP2_ARG2(OP_BIC_S_ASR_IMM, S_ASR_IMM, OP_BICS, 1, 3)
DCL_OP2_ARG2(OP_BIC_S_ASR_REG, S_ASR_REG, OP_BICS, 2, 4)
DCL_OP2_ARG2(OP_BIC_S_ROR_IMM, S_ROR_IMM, OP_BICS, 1, 3)
DCL_OP2_ARG2(OP_BIC_S_ROR_REG, S_ROR_REG, OP_BICS, 2, 4)
DCL_OP2_ARG2(OP_BIC_S_IMM_VAL, S_IMM_VALUE, OP_BICS, 1, 3)

//-----------------------------------------------------------------------------
//   MVN / MVNS
//-----------------------------------------------------------------------------
#define OP_MVN_DATA \
	u32 *r_12; \
	u32 *r_16; \
	bool mod_r15; 
#define OP_MVN_COMPILER \
	DATA(r_12) = &(ARM_REGPOS_W(i,12)); \
	DATA(r_16) = &(ARM_REGPOS_R(i,16)); \
	DATA(mod_r15) = REG_POS(i,12) == 15; 
#define OP_MVN(a, b) \
	*DATA(r_12) = ~shift_op; \
	if(DATA(mod_r15)) \
	{ \
		GOTO_NEXTOP(b); \
	} \
	GOTO_NEXTOP(a);

#define OP_MVNS_DATA \
	armcpu_t *cpu; \
	u32 *r_12; \
	u32 *r_16; \
	bool mod_r15; 
#define OP_MVNS_COMPILER \
	DATA(cpu) = GETCPU; \
	DATA(r_12) = &(ARM_REGPOS_W(i,12)); \
	DATA(r_16) = &(ARM_REGPOS_R(i,16)); \
	DATA(mod_r15) = REG_POS(i,12) == 15; 
#define OP_MVNS(a, b) \
	*DATA(r_12) = ~shift_op; \
	if(DATA(mod_r15)) \
	{ \
		Status_Reg SPSR = DATA(cpu)->SPSR; \
		armcpu_switchMode(DATA(cpu), SPSR.bits.mode); \
		DATA(cpu)->CPSR=SPSR; \
		DATA(cpu)->changeCPSR(); \
		DATA(cpu)->R[15] &= (0xFFFFFFFC|(((u32)DATA(cpsr)->bits.T)<<1)); \
		GOTO_NEXTOP(b); \
	} \
	DATA(cpsr)->bits.C = c; \
	DATA(cpsr)->bits.N = BIT31(*DATA(r_12)); \
	DATA(cpsr)->bits.Z = (*DATA(r_12)==0); \
	GOTO_NEXTOP(a);

DCL_OP2_ARG2(OP_MVN_LSL_IMM, LSL_IMM, OP_MVN, 1, 3)
DCL_OP2_ARG2(OP_MVN_LSL_REG, LSL_REG, OP_MVN, 2, 4)
DCL_OP2_ARG2(OP_MVN_LSR_IMM, LSR_IMM, OP_MVN, 1, 3)
DCL_OP2_ARG2(OP_MVN_LSR_REG, LSR_REG, OP_MVN, 2, 4)
DCL_OP2_ARG2(OP_MVN_ASR_IMM, ASR_IMM, OP_MVN, 1, 3)
DCL_OP2_ARG2(OP_MVN_ASR_REG, ASR_REG, OP_MVN, 2, 4)
DCL_OP2_ARG2(OP_MVN_ROR_IMM, ROR_IMM, OP_MVN, 1, 3)
DCL_OP2_ARG2(OP_MVN_ROR_REG, ROR_REG, OP_MVN, 2, 4)
DCL_OP2_ARG2(OP_MVN_IMM_VAL, IMM_VALUE, OP_MVN, 1, 3)

DCL_OP2_ARG2(OP_MVN_S_LSL_IMM, S_LSL_IMM, OP_MVNS, 1, 3)
DCL_OP2_ARG2(OP_MVN_S_LSL_REG, S_LSL_REG, OP_MVNS, 2, 4)
DCL_OP2_ARG2(OP_MVN_S_LSR_IMM, S_LSR_IMM, OP_MVNS, 1, 3)
DCL_OP2_ARG2(OP_MVN_S_LSR_REG, S_LSR_REG, OP_MVNS, 2, 4)
DCL_OP2_ARG2(OP_MVN_S_ASR_IMM, S_ASR_IMM, OP_MVNS, 1, 3)
DCL_OP2_ARG2(OP_MVN_S_ASR_REG, S_ASR_REG, OP_MVNS, 2, 4)
DCL_OP2_ARG2(OP_MVN_S_ROR_IMM, S_ROR_IMM, OP_MVNS, 1, 3)
DCL_OP2_ARG2(OP_MVN_S_ROR_REG, S_ROR_REG, OP_MVNS, 2, 4)
DCL_OP2_ARG2(OP_MVN_S_IMM_VAL, S_IMM_VALUE, OP_MVNS, 1, 3)

//-----------------------------------------------------------------------------
//   MUL / MULS / MLA / MLAS
//-----------------------------------------------------------------------------
#define MUL_Mxx_END(c) \
	v >>= 8; \
	if((v==0)||(v==0xFFFFFF)) \
	{ \
		GOTO_NEXTOP(c+1); \
	} \
	v >>= 8; \
	if((v==0)||(v==0xFFFF)) \
	{ \
		GOTO_NEXTOP(c+2); \
	} \
	v >>= 8; \
	if((v==0)||(v==0xFF)) \
	{ \
		GOTO_NEXTOP(c+3); \
	} \
	GOTO_NEXTOP(c+4);

DCL_OP_START(OP_MUL)
	u32 *r_0;
	u32 *r_8;
	u32 *r_16; 

	DCL_OP_COMPILER(OP_MUL)
		DATA(r_0) = &(ARM_REGPOS_R(i,0));
		DATA(r_8) = &(ARM_REGPOS_R(i,8));
		DATA(r_16) = &(ARM_REGPOS_W(i,16)); 

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_MUL)
		u32 v = *DATA(r_8);
		*DATA(r_16) = *DATA(r_0) * v;

		MUL_Mxx_END(1)
	}
};

DCL_OP_START(OP_MLA)
	u32 *r_0;
	u32 *r_8;
	u32 *r_12;
	u32 *r_16; 

	DCL_OP_COMPILER(OP_MLA)
		DATA(r_0) = &(ARM_REGPOS_R(i,0));
		DATA(r_8) = &(ARM_REGPOS_R(i,8));
		DATA(r_12) = &(ARM_REGPOS_R(i,12));
		DATA(r_16) = &(ARM_REGPOS_W(i,16)); 

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_MLA)
		u32 v = *DATA(r_8);
		*DATA(r_16) = *DATA(r_0) * v + *DATA(r_12);

		MUL_Mxx_END(2)
	}
};

DCL_OP_START(OP_MUL_S)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_8;
	u32 *r_16; 

	DCL_OP_COMPILER(OP_MUL_S)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(ARM_REGPOS_R(i,0));
		DATA(r_8) = &(ARM_REGPOS_R(i,8));
		DATA(r_16) = &(ARM_REGPOS_W(i,16)); 

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_MUL_S)
		u32 v = *DATA(r_8);
		*DATA(r_16) = *DATA(r_0) * v;

		DATA(cpsr)->bits.N = BIT31(*DATA(r_16));
		DATA(cpsr)->bits.Z = (*DATA(r_16)==0);

		MUL_Mxx_END(1)
	}
};

DCL_OP_START(OP_MLA_S)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_8;
	u32 *r_12;
	u32 *r_16; 

	DCL_OP_COMPILER(OP_MLA_S)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(ARM_REGPOS_R(i,0));
		DATA(r_8) = &(ARM_REGPOS_R(i,8));
		DATA(r_12) = &(ARM_REGPOS_R(i,12));
		DATA(r_16) = &(ARM_REGPOS_W(i,16)); 

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_MLA_S)
		u32 v = *DATA(r_8);
		*DATA(r_16) = *DATA(r_0) * v + *DATA(r_12);

		DATA(cpsr)->bits.N = BIT31(*DATA(r_16));
		DATA(cpsr)->bits.Z = (*DATA(r_16)==0);

		MUL_Mxx_END(2)
	}
};

#undef MUL_Mxx_END

//-----------------------------------------------------------------------------
//   UMULL / UMULLS / UMLAL / UMLALS
//-----------------------------------------------------------------------------
#define MUL_UMxxL_END(c) \
	v >>= 8; \
	if(v==0) \
	{ \
		GOTO_NEXTOP(c+1); \
	} \
	v >>= 8; \
	if(v==0) \
	{ \
		GOTO_NEXTOP(c+2); \
	} \
	v >>= 8; \
	if(v==0) \
	{ \
		GOTO_NEXTOP(c+3); \
	} \
	GOTO_NEXTOP(c+4); 

DCL_OP_START(OP_UMULL)
	u32 *r_0;
	u32 *r_8;
	u32 *r_12;
	u32 *r_16; 

	DCL_OP_COMPILER(OP_UMULL)
		DATA(r_0) = &(ARM_REGPOS_R(i,0));
		DATA(r_8) = &(ARM_REGPOS_R(i,8));
		DATA(r_12) = &(ARM_REGPOS_W(i,12));
		DATA(r_16) = &(ARM_REGPOS_W(i,16)); 

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_UMULL)
		u32 v = *DATA(r_8);
		u64 res = (u64)*DATA(r_0) * (u64)v;

		*DATA(r_12) = (u32)res;
		*DATA(r_16) = (u32)(res>>32);

		MUL_UMxxL_END(2)
	}
};

DCL_OP_START(OP_UMLAL)
	u32 *r_0;
	u32 *r_8;
	u32 *r_12;
	u32 *r_16; 

	DCL_OP_COMPILER(OP_UMLAL)
		DATA(r_0) = &(ARM_REGPOS_R(i,0));
		DATA(r_8) = &(ARM_REGPOS_R(i,8));
		DATA(r_12) = &(ARM_REGPOS_RW(i,12));
		DATA(r_16) = &(ARM_REGPOS_W(i,16)); 

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_UMLAL)
		u32 v = *DATA(r_8);
		u64 res = (u64)*DATA(r_0) * (u64)v;

		u32 tmp = (u32)res;
		*DATA(r_16) = (u32)(res>>32) + *DATA(r_16) + CarryFrom(tmp, *DATA(r_12));
		*DATA(r_12) += tmp; 

		MUL_UMxxL_END(3)
	}
};

DCL_OP_START(OP_UMULL_S)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_8;
	u32 *r_12;
	u32 *r_16; 

	DCL_OP_COMPILER(OP_UMULL_S)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(ARM_REGPOS_R(i,0));
		DATA(r_8) = &(ARM_REGPOS_R(i,8));
		DATA(r_12) = &(ARM_REGPOS_W(i,12));
		DATA(r_16) = &(ARM_REGPOS_W(i,16)); 

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_UMULL_S)
		u32 v = *DATA(r_8);
		u64 res = (u64)*DATA(r_0) * (u64)v;

		*DATA(r_12) = (u32)res;
		*DATA(r_16) = (u32)(res>>32);

		DATA(cpsr)->bits.N = BIT31(*DATA(r_16));
		DATA(cpsr)->bits.Z = (*DATA(r_16)==0) && (*DATA(r_12)==0);

		MUL_UMxxL_END(2)
	}
};

DCL_OP_START(OP_UMLAL_S)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_8;
	u32 *r_12;
	u32 *r_16; 

	DCL_OP_COMPILER(OP_UMLAL_S)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(ARM_REGPOS_R(i,0));
		DATA(r_8) = &(ARM_REGPOS_R(i,8));
		DATA(r_12) = &(ARM_REGPOS_RW(i,12));
		DATA(r_16) = &(ARM_REGPOS_W(i,16)); 

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_UMLAL_S)
		u32 v = *DATA(r_8);
		u64 res = (u64)*DATA(r_0) * (u64)v;

		u32 tmp = (u32)res;
		*DATA(r_16) = (u32)(res>>32) + *DATA(r_16) + CarryFrom(tmp, *DATA(r_12));
		*DATA(r_12) += tmp; 

		DATA(cpsr)->bits.N = BIT31(*DATA(r_16));
		DATA(cpsr)->bits.Z = (*DATA(r_16)==0) && (*DATA(r_12)==0);

		MUL_UMxxL_END(3)
	}
};

#undef MUL_UMxxL_END

//-----------------------------------------------------------------------------
//   SMULL / SMULLS / SMLAL / SMLALS
//-----------------------------------------------------------------------------
#define MUL_SMxxL_END(c) \
	v &= 0xFFFFFFFF; \
	v >>= 8; \
	if((v==0)||(v==0xFFFFFF)) \
	{ \
		GOTO_NEXTOP(c+1); \
	} \
	v >>= 8; \
	if((v==0)||(v==0xFFFF)) \
	{ \
		GOTO_NEXTOP(c+2); \
	} \
	v >>= 8; \
	if((v==0)||(v==0xFF)) \
	{ \
		GOTO_NEXTOP(c+3); \
	} \
	GOTO_NEXTOP(c+4); 

DCL_OP_START(OP_SMULL)
	u32 *r_0;
	u32 *r_8;
	u32 *r_12;
	u32 *r_16; 

	DCL_OP_COMPILER(OP_SMULL)
		DATA(r_0) = &(ARM_REGPOS_R(i,0));
		DATA(r_8) = &(ARM_REGPOS_R(i,8));
		DATA(r_12) = &(ARM_REGPOS_W(i,12));
		DATA(r_16) = &(ARM_REGPOS_W(i,16)); 

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_SMULL)
		s64 v = (s32)*DATA(r_8);
		s64 res = v * (s64)(s32)*DATA(r_0);

		*DATA(r_12) = (u32)res;
		*DATA(r_16) = (u32)(res>>32);

		MUL_SMxxL_END(2)
	}
};

DCL_OP_START(OP_SMLAL)
	u32 *r_0;
	u32 *r_8;
	u32 *r_12;
	u32 *r_16; 

	DCL_OP_COMPILER(OP_SMLAL)
		DATA(r_0) = &(ARM_REGPOS_R(i,0));
		DATA(r_8) = &(ARM_REGPOS_R(i,8));
		DATA(r_12) = &(ARM_REGPOS_RW(i,12));
		DATA(r_16) = &(ARM_REGPOS_W(i,16)); 

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_SMLAL)
		s64 v = (s32)*DATA(r_8);
		s64 res = v * (s64)(s32)*DATA(r_0);

		u32 tmp = (u32)res;

		*DATA(r_16) = (u32)(res>>32) + *DATA(r_16) + CarryFrom(tmp, *DATA(r_12));
		*DATA(r_12) += tmp;

		MUL_SMxxL_END(3)
	}
};

DCL_OP_START(OP_SMULL_S)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_8;
	u32 *r_12;
	u32 *r_16; 

	DCL_OP_COMPILER(OP_SMULL_S)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(ARM_REGPOS_R(i,0));
		DATA(r_8) = &(ARM_REGPOS_R(i,8));
		DATA(r_12) = &(ARM_REGPOS_W(i,12));
		DATA(r_16) = &(ARM_REGPOS_W(i,16)); 

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_SMULL_S)
		s64 v = (s32)*DATA(r_8);
		s64 res = v * (s64)(s32)*DATA(r_0);

		*DATA(r_12) = (u32)res;
		*DATA(r_16) = (u32)(res>>32);

		DATA(cpsr)->bits.N = BIT31(*DATA(r_16));
		DATA(cpsr)->bits.Z = (*DATA(r_16)==0) && (*DATA(r_12)==0);

		MUL_SMxxL_END(2)
	}
};

DCL_OP_START(OP_SMLAL_S)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_8;
	u32 *r_12;
	u32 *r_16; 

	DCL_OP_COMPILER(OP_SMLAL_S)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(ARM_REGPOS_R(i,0));
		DATA(r_8) = &(ARM_REGPOS_R(i,8));
		DATA(r_12) = &(ARM_REGPOS_RW(i,12));
		DATA(r_16) = &(ARM_REGPOS_W(i,16)); 

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_SMLAL_S)
		s64 v = (s32)*DATA(r_8);
		s64 res = v * (s64)(s32)*DATA(r_0);

		u32 tmp = (u32)res;

		*DATA(r_16) = (u32)(res>>32) + *DATA(r_16) + CarryFrom(tmp, *DATA(r_12));
		*DATA(r_12) += tmp;

		DATA(cpsr)->bits.N = BIT31(*DATA(r_16));
		DATA(cpsr)->bits.Z = (*DATA(r_16)==0) && (*DATA(r_12)==0);

		MUL_SMxxL_END(3)
	}
};

#undef MUL_SMxxL_END

//-----------------------------------------------------------------------------
//   SWP / SWPB
//-----------------------------------------------------------------------------
DCL_OP_START(OP_SWP)
	armcpu_t *cpu;
	u32 *r_0;
	u32 *r_12;
	u32 *r_16; 

	DCL_OP_COMPILER(OP_SWP)
		DATA(cpu) = GETCPU;
		DATA(r_0) = &(ARM_REGPOS_R(i,0));
		DATA(r_12) = &(ARM_REGPOS_W(i,12));
		DATA(r_16) = &(ARM_REGPOS_R(i,16)); 

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_SWP)
		u32 adr = *DATA(r_16);
		u32 tmp = ROR(READ32(DATA(cpu)->mem_if->data, adr), (adr & 3)<<3);

		WRITE32(DATA(cpu)->mem_if->data, adr, *DATA(r_0));
		*DATA(r_12) = tmp;

		u32 c = MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
		c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr);

		c = MMU_aluMemCycles<PROCNUM>(4, c);

		GOTO_NEXTOP(c);
	}
};

DCL_OP_START(OP_SWPB)
	armcpu_t *cpu;
	u32 *r_0;
	u32 *r_12;
	u32 *r_16; 

	DCL_OP_COMPILER(OP_SWPB)
		DATA(cpu) = GETCPU;
		DATA(r_0) = &(ARM_REGPOS_R(i,0));
		DATA(r_12) = &(ARM_REGPOS_W(i,12));
		DATA(r_16) = &(ARM_REGPOS_R(i,16)); 

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_SWPB)
		u32 adr = *DATA(r_16);
		u32 tmp = READ8(DATA(cpu)->mem_if->data, adr);

		WRITE8(DATA(cpu)->mem_if->data, adr, (u8)(*DATA(r_0)&0xFF));
		*DATA(r_12) = tmp;

		u32 c = MMU_memAccessCycles<PROCNUM,8,MMU_AD_READ>(adr);
		c += MMU_memAccessCycles<PROCNUM,8,MMU_AD_WRITE>(adr);

		c = MMU_aluMemCycles<PROCNUM>(4, c);

		GOTO_NEXTOP(c);
	}
};

//-----------------------------------------------------------------------------
//   LDR/STR macros
//-----------------------------------------------------------------------------
#define OP_LDR_PRE_(Op, Adr, MemCycles) \
	DCL_OP_START(OP_##Op##_##Adr) \
		Op##_DATA \
		Adr##_DATA \
		DCL_OP_COMPILER(OP_##Op##_##Adr) \
			Op##_COMPILER \
			Adr##_COMPILER \
			DONE_COMPILER \
		} \
		DCL_OP_METHOD(OP_##Op##_##Adr) \
			PRE_INDE_##Adr \
			Op \
			u32 c = (MemCycles); \
			GOTO_NEXTOP(c) \
		} \
	};

#define OP_LDR_PRE_WB_(Op, Adr, MemCycles) \
	DCL_OP_START(OP_##Op##_PRE_INDE_##Adr) \
		Op##_DATA \
		Adr##_DATA \
		DCL_OP_COMPILER(OP_##Op##_PRE_INDE_##Adr) \
			Op##_COMPILER \
			Adr##_COMPILER \
			DONE_COMPILER \
		} \
		DCL_OP_METHOD(OP_##Op##_PRE_INDE_##Adr) \
			PRE_INDE_##Adr \
			PRE_INDE_ADR_WB \
			Op \
			u32 c = (MemCycles); \
			GOTO_NEXTOP(c) \
		} \
	};

#define OP_LDR_POS_(Op, Adr, MemCycles) \
	DCL_OP_START(OP_##Op##_POS_INDE_##Adr) \
		Op##_DATA \
		Adr##_DATA \
		DCL_OP_COMPILER(OP_##Op##_POS_INDE_##Adr) \
			Op##_COMPILER \
			Adr##_COMPILER \
			DONE_COMPILER \
		} \
		DCL_OP_METHOD(OP_##Op##_POS_INDE_##Adr) \
			POS_INDE_ADR_CALC \
			POS_INDE_##Adr \
			Op \
			u32 c = (MemCycles); \
			GOTO_NEXTOP(c) \
		} \
	};

#define OP_STR_PRE_(Op, Adr, MemCycles) \
	DCL_OP_START(OP_##Op##_##Adr) \
		Op##_DATA \
		Adr##_DATA \
		DCL_OP_COMPILER(OP_##Op##_##Adr) \
			Op##_COMPILER \
			Adr##_COMPILER \
			DONE_COMPILER \
		} \
		DCL_OP_METHOD(OP_##Op##_##Adr) \
			PRE_INDE_##Adr \
			Op \
			u32 c = (MemCycles); \
			GOTO_NEXTOP(c) \
		} \
	};

#define OP_STR_PRE_WB_(Op, Adr, MemCycles) \
	DCL_OP_START(OP_##Op##_PRE_INDE_##Adr) \
		Op##_DATA \
		Adr##_DATA \
		DCL_OP_COMPILER(OP_##Op##_PRE_INDE_##Adr) \
			Op##_COMPILER \
			Adr##_COMPILER \
			DONE_COMPILER \
		} \
		DCL_OP_METHOD(OP_##Op##_PRE_INDE_##Adr) \
			PRE_INDE_##Adr \
			PRE_INDE_ADR_WB \
			Op \
			u32 c = (MemCycles); \
			GOTO_NEXTOP(c) \
		} \
	};

#define OP_STR_POS_(Op, Adr, MemCycles) \
	DCL_OP_START(OP_##Op##_POS_INDE_##Adr) \
		Op##_DATA \
		Adr##_DATA \
		DCL_OP_COMPILER(OP_##Op##_POS_INDE_##Adr) \
			Op##_COMPILER \
			Adr##_COMPILER \
			DONE_COMPILER \
		} \
		DCL_OP_METHOD(OP_##Op##_POS_INDE_##Adr) \
			POS_INDE_ADR_CALC \
			Op \
			POS_INDE_##Adr \
			u32 c = (MemCycles); \
			GOTO_NEXTOP(c) \
		} \
	};

#define IMM_OFF (((i>>4)&0xF0)+(i&0xF))

#define P_IMM_OFF_DATA \
	u32 *r_16; \
	u32 value;
#define P_IMM_OFF_COMPILER \
	DATA(r_16) = &(ARM_REGPOS_RW(i,16)); \
	DATA(value) = IMM_OFF;

#define M_IMM_OFF_DATA \
	u32 *r_16; \
	u32 value;
#define M_IMM_OFF_COMPILER \
	DATA(r_16) = &(ARM_REGPOS_RW(i,16)); \
	DATA(value) = IMM_OFF;

#define P_REG_OFF_DATA \
	u32 *r_0; \
	u32 *r_16;
#define P_REG_OFF_COMPILER \
	DATA(r_0) = &(ARM_REGPOS_R(i,0)); \
	DATA(r_16) = &(ARM_REGPOS_RW(i,16)); 

#define M_REG_OFF_DATA \
	u32 *r_0; \
	u32 *r_16;
#define M_REG_OFF_COMPILER \
	DATA(r_0) = &(ARM_REGPOS_R(i,0)); \
	DATA(r_16) = &(ARM_REGPOS_RW(i,16)); 

#define PRE_INDE_P_IMM_OFF \
	u32 adr = *DATA(r_16) + DATA(value); 
#define PRE_INDE_M_IMM_OFF \
	u32 adr = *DATA(r_16) - DATA(value); 
#define PRE_INDE_P_REG_OFF \
	u32 adr = *DATA(r_16) + *DATA(r_0); 
#define PRE_INDE_M_REG_OFF \
	u32 adr = *DATA(r_16) - *DATA(r_0); 
#define PRE_INDE_ADR_WB \
	*DATA(r_16) = adr; 

#define POS_INDE_ADR_CALC \
	u32 adr = *DATA(r_16); 
#define POS_INDE_P_IMM_OFF \
	*DATA(r_16) += DATA(value);
#define POS_INDE_M_IMM_OFF \
	*DATA(r_16) -= DATA(value);
#define POS_INDE_P_REG_OFF \
	*DATA(r_16) += *DATA(r_0); 
#define POS_INDE_M_REG_OFF \
	*DATA(r_16) -= *DATA(r_0); 

//-----------------------------------------------------------------------------
//   LDRH
//-----------------------------------------------------------------------------
#define LDRH_DATA \
	u32 *r_12; 
#define LDRH_COMPILER \
	DATA(r_12) = &(ARM_REGPOS_W(i,12)); 
#define LDRH \
	*DATA(r_12) = (u32)READ16(DATA(cpu)->mem_if->data, adr);

#define LDRH_MEMCYCLES \
	MMU_aluMemAccessCycles<PROCNUM,16,MMU_AD_READ>(3,adr)

OP_LDR_PRE_(LDRH, P_IMM_OFF, LDRH_MEMCYCLES)
OP_LDR_PRE_(LDRH, M_IMM_OFF, LDRH_MEMCYCLES)
OP_LDR_PRE_(LDRH, P_REG_OFF, LDRH_MEMCYCLES)
OP_LDR_PRE_(LDRH, M_REG_OFF, LDRH_MEMCYCLES)
OP_LDR_PRE_WB_(LDRH, P_IMM_OFF, LDRH_MEMCYCLES)
OP_LDR_PRE_WB_(LDRH, M_IMM_OFF, LDRH_MEMCYCLES)
OP_LDR_PRE_WB_(LDRH, P_REG_OFF, LDRH_MEMCYCLES)
OP_LDR_PRE_WB_(LDRH, M_REG_OFF, LDRH_MEMCYCLES)
OP_LDR_POS_(LDRH, P_IMM_OFF, LDRH_MEMCYCLES)
OP_LDR_POS_(LDRH, M_IMM_OFF, LDRH_MEMCYCLES)
OP_LDR_POS_(LDRH, P_REG_OFF, LDRH_MEMCYCLES)
OP_LDR_POS_(LDRH, M_REG_OFF, LDRH_MEMCYCLES)

#undef LDRH_DATA
#undef LDRH_COMPILER
#undef LDRH
#undef LDRH_MEMCYCLES

//-----------------------------------------------------------------------------
//   STRH
//-----------------------------------------------------------------------------
#define STRH_DATA \
	u32 *r_12; 
#define STRH_COMPILER \
	DATA(r_12) = &(ARM_REGPOS_R(i,12)); 
#define STRH \
	WRITE16(cpu->mem_if->data, adr, (u16)*DATA(r_12));

#define STRH_MEMCYCLES \
	MMU_aluMemAccessCycles<PROCNUM,16,MMU_AD_WRITE>(2,adr)

OP_STR_PRE_(STRH, P_IMM_OFF, STRH_MEMCYCLES)
OP_STR_PRE_(STRH, M_IMM_OFF, STRH_MEMCYCLES)
OP_STR_PRE_(STRH, P_REG_OFF, STRH_MEMCYCLES)
OP_STR_PRE_(STRH, M_REG_OFF, STRH_MEMCYCLES)
OP_STR_PRE_WB_(STRH, P_IMM_OFF, STRH_MEMCYCLES)
OP_STR_PRE_WB_(STRH, M_IMM_OFF, STRH_MEMCYCLES)
OP_STR_PRE_WB_(STRH, P_REG_OFF, STRH_MEMCYCLES)
OP_STR_PRE_WB_(STRH, M_REG_OFF, STRH_MEMCYCLES)
OP_STR_POS_(STRH, P_IMM_OFF, STRH_MEMCYCLES)
OP_STR_POS_(STRH, M_IMM_OFF, STRH_MEMCYCLES)
OP_STR_POS_(STRH, P_REG_OFF, STRH_MEMCYCLES)
OP_STR_POS_(STRH, M_REG_OFF, STRH_MEMCYCLES)

#undef STRH_DATA
#undef STRH_COMPILER
#undef STRH
#undef STRH_MEMCYCLES

//-----------------------------------------------------------------------------
//   LDRSH
//-----------------------------------------------------------------------------
#define LDRSH_DATA \
	u32 *r_12; 
#define LDRSH_COMPILER \
	DATA(r_12) = &(ARM_REGPOS_W(i,12)); 
#define LDRSH \
	*DATA(r_12) = (s32)((s16)READ16(cpu->mem_if->data, adr));

#define LDRSH_MEMCYCLES \
	MMU_aluMemAccessCycles<PROCNUM,16,MMU_AD_READ>(3,adr)

OP_LDR_PRE_(LDRSH, P_IMM_OFF, LDRSH_MEMCYCLES)
OP_LDR_PRE_(LDRSH, M_IMM_OFF, LDRSH_MEMCYCLES)
OP_LDR_PRE_(LDRSH, P_REG_OFF, LDRSH_MEMCYCLES)
OP_LDR_PRE_(LDRSH, M_REG_OFF, LDRSH_MEMCYCLES)
OP_LDR_PRE_WB_(LDRSH, P_IMM_OFF, LDRSH_MEMCYCLES)
OP_LDR_PRE_WB_(LDRSH, M_IMM_OFF, LDRSH_MEMCYCLES)
OP_LDR_PRE_WB_(LDRSH, P_REG_OFF, LDRSH_MEMCYCLES)
OP_LDR_PRE_WB_(LDRSH, M_REG_OFF, LDRSH_MEMCYCLES)
OP_LDR_POS_(LDRSH, P_IMM_OFF, LDRSH_MEMCYCLES)
OP_LDR_POS_(LDRSH, M_IMM_OFF, LDRSH_MEMCYCLES)
OP_LDR_POS_(LDRSH, P_REG_OFF, LDRSH_MEMCYCLES)
OP_LDR_POS_(LDRSH, M_REG_OFF, LDRSH_MEMCYCLES)

#undef LDRSH_DATA
#undef LDRSH_COMPILER
#undef LDRSH
#undef LDRSH_MEMCYCLES

//-----------------------------------------------------------------------------
//   LDRSB
//-----------------------------------------------------------------------------
#define LDRSB_DATA \
	u32 *r_12; 
#define LDRSB_COMPILER \
	DATA(r_12) = &(ARM_REGPOS_W(i,12)); 
#define LDRSB \
	*DATA(r_12) = (s32)((s8)READ8(cpu->mem_if->data, adr));

#define LDRSB_MEMCYCLES \
	MMU_aluMemAccessCycles<PROCNUM,8,MMU_AD_READ>(3,adr)

OP_LDR_PRE_(LDRSB, P_IMM_OFF, LDRSB_MEMCYCLES)
OP_LDR_PRE_(LDRSB, M_IMM_OFF, LDRSB_MEMCYCLES)
OP_LDR_PRE_(LDRSB, P_REG_OFF, LDRSB_MEMCYCLES)
OP_LDR_PRE_(LDRSB, M_REG_OFF, LDRSB_MEMCYCLES)
OP_LDR_PRE_WB_(LDRSB, P_IMM_OFF, LDRSB_MEMCYCLES)
OP_LDR_PRE_WB_(LDRSB, M_IMM_OFF, LDRSB_MEMCYCLES)
OP_LDR_PRE_WB_(LDRSB, P_REG_OFF, LDRSB_MEMCYCLES)
OP_LDR_PRE_WB_(LDRSB, M_REG_OFF, LDRSB_MEMCYCLES)
OP_LDR_POS_(LDRSB, P_IMM_OFF, LDRSB_MEMCYCLES)
OP_LDR_POS_(LDRSB, M_IMM_OFF, LDRSB_MEMCYCLES)
OP_LDR_POS_(LDRSB, P_REG_OFF, LDRSB_MEMCYCLES)
OP_LDR_POS_(LDRSB, M_REG_OFF, LDRSB_MEMCYCLES)

#undef LDRSB_DATA
#undef LDRSB_COMPILER
#undef LDRSB
#undef LDRSB_MEMCYCLES

//-----------------------------------------------------------------------------
//   MRS / MSR
//-----------------------------------------------------------------------------
DCL_OP_START(OP_MRS_CPSR)
	Status_Reg *cpsr;
	u32 *r_12; 

	DCL_OP_COMPILER(OP_MRS_CPSR)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_12) = &(ARM_REGPOS_W(i,12)); 

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_MRS_CPSR)
		*DATA(r_12) = DATA(cpsr)->val;
		
		GOTO_NEXTOP(1)
	}
};

DCL_OP_START(OP_MRS_SPSR)
	Status_Reg *spsr;
	u32 *r_12; 

	DCL_OP_COMPILER(OP_MRS_SPSR)
		DATA(spsr) = &(GETCPU->SPSR);
		DATA(r_12) = &(ARM_REGPOS_W(i,12)); 

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_MRS_SPSR)
		*DATA(r_12) = DATA(spsr)->val;
		
		GOTO_NEXTOP(1)
	}
};

#define OPERAND_DATA \
	u32 *r_0; 
#define OPERAND_COMPILER \
	DATA(r_0) = &(ARM_REGPOS_R(i,0)); 
#define OPERAND \
	u32 operand = *DATA(r_0);

#define MSR_CPSR_DATA \
	armcpu_t *cpu; \
	u32 byte_mask_USR; \
	u32 byte_mask_OTH; \
	bool flag;
#define MSR_CPSR_COMPILER \
	DATA(cpu) = GETCPU; \
	DATA(byte_mask_USR) = (BIT19(i)?0xFF000000:0x00000000); \
	DATA(byte_mask_OTH) = (BIT16(i)?0x000000FF:0x00000000) | \
							(BIT17(i)?0x0000FF00:0x00000000) | \
							(BIT18(i)?0x00FF0000:0x00000000) | \
							(BIT19(i)?0xFF000000:0x00000000); \
	DATA(flag) = BIT16(i); 
#define MSR_CPSR(operand, c) \
	u32 byte_mask = (DATA(cpu)->CPSR.bits.mode == USR)?DATA(byte_mask_USR):DATA(byte_mask_OTH); \
	if(DATA(cpu)->CPSR.bits.mode != USR && DATA(flag)) \
		{ armcpu_switchMode(DATA(cpu), operand & 0x1F); } \
	DATA(cpu)->CPSR.val = (DATA(cpu)->CPSR.val & ~byte_mask) | (operand & byte_mask); \
	DATA(cpu)->changeCPSR(); \
	GOTO_NEXTOP(c)

#define MSR_SPSR_DATA \
	armcpu_t *cpu; \
	u32 byte_mask; 
#define MSR_SPSR_COMPILER \
	DATA(cpu) = GETCPU; \
	DATA(byte_mask) = (BIT16(i)?0x000000FF:0x00000000) | \
						(BIT17(i)?0x0000FF00:0x00000000) | \
						(BIT18(i)?0x00FF0000:0x00000000) | \
						(BIT19(i)?0xFF000000:0x00000000); 
#define MSR_SPSR(operand, c) \
	if(DATA(cpu)->CPSR.bits.mode == USR || DATA(cpu)->CPSR.bits.mode == SYS) \
		{ GOTO_NEXTOP(1) }\
	DATA(cpu)->SPSR.val = (DATA(cpu)->SPSR.val & ~DATA(byte_mask)) | (operand & DATA(byte_mask)); \
	DATA(cpu)->changeCPSR(); \
	GOTO_NEXTOP(c)

DCL_OP2_ARG2(OP_MSR_CPSR, OPERAND, MSR_CPSR, operand, 1)
DCL_OP2_ARG2(OP_MSR_SPSR, OPERAND, MSR_SPSR, operand, 1)
DCL_OP2_ARG2(OP_MSR_CPSR_IMM_VAL, IMM_VALUE, MSR_CPSR, shift_op, 1)
DCL_OP2_ARG2(OP_MSR_SPSR_IMM_VAL, IMM_VALUE, MSR_SPSR, shift_op, 1)

#undef OPERAND_DATA
#undef OPERAND_COMPILER
#undef OPERAND
#undef MSR_CPSR_DATA
#undef MSR_CPSR_COMPILER
#undef MSR_CPSR
#undef MSR_SPSR_DATA
#undef MSR_SPSR_COMPILER
#undef MSR_SPSR

//-----------------------------------------------------------------------------
//   Branch
//-----------------------------------------------------------------------------
DCL_OP_START(OP_BX)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_15;

	DCL_OP_COMPILER(OP_BX)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(ARM_REGPOS_R(i, 0));
		DATA(r_15) = &(GETCPUREG_W(15));
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_BX)
		u32 tmp = *DATA(r_0);
		
		DATA(cpsr)->bits.T = BIT0(tmp);
		*DATA(r_15) = (tmp & (0xFFFFFFFC|(DATA(cpsr)->bits.T)<<1));

		GOTO_NEXTOP(3)
	}
};

DCL_OP_START(OP_BLX_REG)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_14;
	u32 *r_15;

	DCL_OP_COMPILER(OP_BLX_REG)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(ARM_REGPOS_R(i, 0));
		DATA(r_14) = &(GETCPUREG_W(14));
		DATA(r_15) = &(GETCPUREG_W(15));
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_BLX_REG)
		u32 tmp = *DATA(r_0);
		
		*DATA(r_14) = common->R15 - 4;
		DATA(cpsr)->bits.T = BIT0(tmp);
		*DATA(r_15) = (tmp & (0xFFFFFFFC|(DATA(cpsr)->bits.T)<<1));

		GOTO_NEXTOP(3)
	}
};

#define SIGNEXTEND_24(i) (((s32)i<<8)>>8)

DCL_OP_START(OP_B)
	Status_Reg *cpsr;
	u32 *r_14;
	u32 *r_15;
	u32 off;
	u32 cond;

	DCL_OP_COMPILER(OP_B)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_14) = &(GETCPUREG_W(14));
		DATA(r_15) = &(GETCPUREG_RW(15));
		DATA(off) = SIGNEXTEND_24(i);
		DATA(cond) = CONDITION(i);
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_B)
		if (DATA(cond) == 0xF)
		{
			*DATA(r_14) = common->R15 - 4;
			DATA(cpsr)->bits.T = 1;
		}
		*DATA(r_15) += (DATA(off)<<2);
		*DATA(r_15) &= (0xFFFFFFFC|(DATA(cpsr)->bits.T<<1));

		GOTO_NEXTOP(3)
	}
};

DCL_OP_START(OP_BL)
	Status_Reg *cpsr;
	u32 *r_14;
	u32 *r_15;
	u32 off;
	u32 cond;

	DCL_OP_COMPILER(OP_BL)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_14) = &(GETCPUREG_W(14));
		DATA(r_15) = &(GETCPUREG_RW(15));
		DATA(off) = SIGNEXTEND_24(i);
		DATA(cond) = CONDITION(i);
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_BL)
		if (DATA(cond) == 0xF)
		{
			DATA(cpsr)->bits.T = 1;
			*DATA(r_15) += 2;
		}
		*DATA(r_14) = common->R15 - 4;
		*DATA(r_15) += (DATA(off)<<2);
		*DATA(r_15) &= (0xFFFFFFFC|(DATA(cpsr)->bits.T<<1));

		GOTO_NEXTOP(3)
	}
};

#undef SIGNEXTEND_24

//-----------------------------------------------------------------------------
//   CLZ
//-----------------------------------------------------------------------------
const u8 CLZ_TAB[16]=
{
	0,							// 0000
	1,							// 0001
	2, 2,						// 001X
	3, 3, 3, 3,					// 01XX
	4, 4, 4, 4, 4, 4, 4, 4		// 1XXX
};

DCL_OP_START(OP_CLZ)
	u32 *r_0;
	u32 *r_12;

	DCL_OP_COMPILER(OP_CLZ)
		DATA(r_0) = &(ARM_REGPOS_R(i, 0));
		DATA(r_12) = &(ARM_REGPOS_W(i, 12));
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_CLZ)
		u32 Rm = *DATA(r_0);

		if(Rm==0)
		{
			*DATA(r_12) = 32;
			GOTO_NEXTOP(2)
		}

		Rm |= (Rm >>1);
		Rm |= (Rm >>2);
		Rm |= (Rm >>4);
		Rm |= (Rm >>8);
		Rm |= (Rm >>16);
	
		u32 pos =	 
			CLZ_TAB[Rm&0xF] +
			CLZ_TAB[(Rm>>4)&0xF] +
			CLZ_TAB[(Rm>>8)&0xF] +
			CLZ_TAB[(Rm>>12)&0xF] +
			CLZ_TAB[(Rm>>16)&0xF] +
			CLZ_TAB[(Rm>>20)&0xF] +
			CLZ_TAB[(Rm>>24)&0xF] +
			CLZ_TAB[(Rm>>28)&0xF];

		*DATA(r_12) = 32 - pos;
		
		GOTO_NEXTOP(2)
	}
};

//-----------------------------------------------------------------------------
//   QADD / QDADD / QSUB / QDSUB
//-----------------------------------------------------------------------------
DCL_OP_START(OP_QADD)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_12;
	u32 *r_16;
	bool mod_r15;

	DCL_OP_COMPILER(OP_QADD)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(ARM_REGPOS_R(i, 0));
		DATA(r_12) = &(ARM_REGPOS_W(i, 12));
		DATA(r_16) = &(ARM_REGPOS_R(i, 16));
		DATA(mod_r15) = REG_POS(i,12)==15;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_QADD)
		u32 res = *DATA(r_16) + *DATA(r_0);

		if (SIGNED_OVERFLOW(*DATA(r_16), *DATA(r_0), res))
		{
			DATA(cpsr)->bits.Q=1;
			*DATA(r_12)=0x80000000-BIT31(res);

			GOTO_NEXTOP(2)
		}

		*DATA(r_12)=res;

		if (DATA(mod_r15))
		{
			*DATA(r_12) &= 0xFFFFFFFC;

			GOTO_NEXTOP(3)
		}

		GOTO_NEXTOP(2)
	}
};

DCL_OP_START(OP_QSUB)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_12;
	u32 *r_16;
	bool mod_r15;

	DCL_OP_COMPILER(OP_QSUB)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(ARM_REGPOS_R(i, 0));
		DATA(r_12) = &(ARM_REGPOS_W(i, 12));
		DATA(r_16) = &(ARM_REGPOS_R(i, 16));
		DATA(mod_r15) = REG_POS(i,12)==15;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_QSUB)
		u32 res = *DATA(r_0) - *DATA(r_16);

		if (SIGNED_UNDERFLOW(*DATA(r_0), *DATA(r_16), res))
		{
			DATA(cpsr)->bits.Q=1;
			*DATA(r_12)=0x80000000-BIT31(res);

			GOTO_NEXTOP(2)
		}

		*DATA(r_12)=res;

		if (DATA(mod_r15))
		{
			*DATA(r_12) &= 0xFFFFFFFC;

			GOTO_NEXTOP(3)
		}

		GOTO_NEXTOP(2)
	}
};

DCL_OP_START(OP_QDADD)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_12;
	u32 *r_16;
	bool mod_r15;

	DCL_OP_COMPILER(OP_QDADD)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(ARM_REGPOS_R(i, 0));
		DATA(r_12) = &(ARM_REGPOS_W(i, 12));
		DATA(r_16) = &(ARM_REGPOS_R(i, 16));
		DATA(mod_r15) = REG_POS(i,12)==15;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_QDADD)
		u32 mul = *DATA(r_16)<<1;
		
		if(BIT31(*DATA(r_16))!=BIT31(mul))
		{
			DATA(cpsr)->bits.Q=1;
			mul = 0x80000000-BIT31(mul);
		}
		
		u32 res = mul + *DATA(r_0);

		if (SIGNED_OVERFLOW(*DATA(r_0), mul, res))
		{
			DATA(cpsr)->bits.Q=1;
			*DATA(r_12)=0x80000000-BIT31(res);

			GOTO_NEXTOP(2)
		}

		*DATA(r_12)=res;

		if (DATA(mod_r15))
		{
			*DATA(r_12) &= 0xFFFFFFFC;

			GOTO_NEXTOP(3)
		}

		GOTO_NEXTOP(2)
	}
};

DCL_OP_START(OP_QDSUB)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_12;
	u32 *r_16;
	bool mod_r15;

	DCL_OP_COMPILER(OP_QDSUB)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(ARM_REGPOS_R(i, 0));
		DATA(r_12) = &(ARM_REGPOS_W(i, 12));
		DATA(r_16) = &(ARM_REGPOS_R(i, 16));
		DATA(mod_r15) = REG_POS(i,12)==15;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_QDSUB)
		u32 mul = *DATA(r_16)<<1;
		
		if(BIT31(*DATA(r_16))!=BIT31(mul))
		{
			DATA(cpsr)->bits.Q=1;
			mul = 0x80000000-BIT31(mul);
		}
		
		u32 res = *DATA(r_0) - mul;

		if (SIGNED_UNDERFLOW(*DATA(r_0), mul, res))
		{
			DATA(cpsr)->bits.Q=1;
			*DATA(r_12)=0x80000000-BIT31(res);

			GOTO_NEXTOP(2)
		}

		*DATA(r_12)=res;

		if (DATA(mod_r15))
		{
			*DATA(r_12) &= 0xFFFFFFFC;

			GOTO_NEXTOP(3)
		}

		GOTO_NEXTOP(2)
	}
};

//-----------------------------------------------------------------------------
//   SMUL
//-----------------------------------------------------------------------------
#define HWORD(i)   ((s32)(((s32)(i))>>16))
#define LWORD(i)   (s32)(((s32)((i)<<16))>>16)

DCL_OP_START(OP_SMUL_B_B)
	u32 *r_0;
	u32 *r_8;
	u32 *r_16;

	DCL_OP_COMPILER(OP_SMUL_B_B)
		DATA(r_0) = &(ARM_REGPOS_R(i, 0));
		DATA(r_8) = &(ARM_REGPOS_R(i, 8));
		DATA(r_16) = &(ARM_REGPOS_W(i, 16));
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_SMUL_B_B)
		*DATA(r_16) = (u32)(LWORD(*DATA(r_0)) * LWORD(*DATA(r_8)));

		GOTO_NEXTOP(2)
	}
};

DCL_OP_START(OP_SMUL_B_T)
	u32 *r_0;
	u32 *r_8;
	u32 *r_16;

	DCL_OP_COMPILER(OP_SMUL_B_T)
		DATA(r_0) = &(ARM_REGPOS_R(i, 0));
		DATA(r_8) = &(ARM_REGPOS_R(i, 8));
		DATA(r_16) = &(ARM_REGPOS_W(i, 16));
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_SMUL_B_T)
		*DATA(r_16) = (u32)(LWORD(*DATA(r_0)) * HWORD(*DATA(r_8)));

		GOTO_NEXTOP(2)
	}
};

DCL_OP_START(OP_SMUL_T_B)
	u32 *r_0;
	u32 *r_8;
	u32 *r_16;

	DCL_OP_COMPILER(OP_SMUL_T_B)
		DATA(r_0) = &(ARM_REGPOS_R(i, 0));
		DATA(r_8) = &(ARM_REGPOS_R(i, 8));
		DATA(r_16) = &(ARM_REGPOS_W(i, 16));
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_SMUL_T_B)
		*DATA(r_16) = (u32)(HWORD(*DATA(r_0)) * LWORD(*DATA(r_8)));

		GOTO_NEXTOP(2)
	}
};

DCL_OP_START(OP_SMUL_T_T)
	u32 *r_0;
	u32 *r_8;
	u32 *r_16;

	DCL_OP_COMPILER(OP_SMUL_T_T)
		DATA(r_0) = &(ARM_REGPOS_R(i, 0));
		DATA(r_8) = &(ARM_REGPOS_R(i, 8));
		DATA(r_16) = &(ARM_REGPOS_W(i, 16));
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_SMUL_T_T)
		*DATA(r_16) = (u32)(HWORD(*DATA(r_0)) * HWORD(*DATA(r_8)));

		GOTO_NEXTOP(2)
	}
};

//-----------------------------------------------------------------------------
//   SMLA
//-----------------------------------------------------------------------------
DCL_OP_START(OP_SMLA_B_B)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_8;
	u32 *r_12;
	u32 *r_16;

	DCL_OP_COMPILER(OP_SMLA_B_B)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(ARM_REGPOS_R(i, 0));
		DATA(r_8) = &(ARM_REGPOS_R(i, 8));
		DATA(r_12) = &(ARM_REGPOS_R(i, 12));
		DATA(r_16) = &(ARM_REGPOS_W(i, 16));
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_SMLA_B_B)
		u32 tmp = (u32)((s16)(*DATA(r_0)) * (s16)(*DATA(r_8)));
		
		*DATA(r_16) = tmp + *DATA(r_12);

		if (OverflowFromADD(*DATA(r_16), tmp, *DATA(r_12)))
			DATA(cpsr)->bits.Q=1;

		GOTO_NEXTOP(2)
	}
};

DCL_OP_START(OP_SMLA_B_T)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_8;
	u32 *r_12;
	u32 *r_16;

	DCL_OP_COMPILER(OP_SMLA_B_T)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(ARM_REGPOS_R(i, 0));
		DATA(r_8) = &(ARM_REGPOS_R(i, 8));
		DATA(r_12) = &(ARM_REGPOS_R(i, 12));
		DATA(r_16) = &(ARM_REGPOS_W(i, 16));
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_SMLA_B_T)
		u32 tmp = (u32)(LWORD(*DATA(r_0)) * HWORD(*DATA(r_8)));
		u32 a = *DATA(r_12);
		
		*DATA(r_16) = tmp + a;

		if (SIGNED_OVERFLOW(tmp, a, *DATA(r_16)))
			DATA(cpsr)->bits.Q=1;

		GOTO_NEXTOP(2)
	}
};

DCL_OP_START(OP_SMLA_T_B)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_8;
	u32 *r_12;
	u32 *r_16;

	DCL_OP_COMPILER(OP_SMLA_T_B)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(ARM_REGPOS_R(i, 0));
		DATA(r_8) = &(ARM_REGPOS_R(i, 8));
		DATA(r_12) = &(ARM_REGPOS_R(i, 12));
		DATA(r_16) = &(ARM_REGPOS_W(i, 16));
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_SMLA_T_B)
		u32 tmp = (u32)(HWORD(*DATA(r_0)) * LWORD(*DATA(r_8)));
		u32 a = *DATA(r_12);
		
		*DATA(r_16) = tmp + a;

		if (SIGNED_OVERFLOW(tmp, a, *DATA(r_16)))
			DATA(cpsr)->bits.Q=1;

		GOTO_NEXTOP(2)
	}
};

DCL_OP_START(OP_SMLA_T_T)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_8;
	u32 *r_12;
	u32 *r_16;

	DCL_OP_COMPILER(OP_SMLA_T_T)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(ARM_REGPOS_R(i, 0));
		DATA(r_8) = &(ARM_REGPOS_R(i, 8));
		DATA(r_12) = &(ARM_REGPOS_R(i, 12));
		DATA(r_16) = &(ARM_REGPOS_W(i, 16));
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_SMLA_T_T)
		u32 tmp = (u32)(HWORD(*DATA(r_0)) * HWORD(*DATA(r_8)));
		u32 a = *DATA(r_12);
		
		*DATA(r_16) = tmp + a;

		if (SIGNED_OVERFLOW(tmp, a, *DATA(r_16)))
			DATA(cpsr)->bits.Q=1;

		GOTO_NEXTOP(2)
	}
};

//-----------------------------------------------------------------------------
//   SMLAL
//-----------------------------------------------------------------------------
DCL_OP_START(OP_SMLAL_B_B)
	u32 *r_0;
	u32 *r_8;
	u32 *r_12;
	u32 *r_16;

	DCL_OP_COMPILER(OP_SMLAL_B_B)
		DATA(r_0) = &(ARM_REGPOS_R(i, 0));
		DATA(r_8) = &(ARM_REGPOS_R(i, 8));
		DATA(r_12) = &(ARM_REGPOS_RW(i, 12));
		DATA(r_16) = &(ARM_REGPOS_RW(i, 16));
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_SMLAL_B_B)
		s64 tmp = (s64)(LWORD(*DATA(r_0)) * LWORD(*DATA(r_8)));
		u64 res = (u64)tmp + *DATA(r_12);

		*DATA(r_12) = (u32) res;
		*DATA(r_16) += (res + ((tmp<0)*0xFFFFFFFF));

		GOTO_NEXTOP(2)
	}
};

DCL_OP_START(OP_SMLAL_B_T)
	u32 *r_0;
	u32 *r_8;
	u32 *r_12;
	u32 *r_16;

	DCL_OP_COMPILER(OP_SMLAL_B_T)
		DATA(r_0) = &(ARM_REGPOS_R(i, 0));
		DATA(r_8) = &(ARM_REGPOS_R(i, 8));
		DATA(r_12) = &(ARM_REGPOS_RW(i, 12));
		DATA(r_16) = &(ARM_REGPOS_RW(i, 16));
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_SMLAL_B_T)
		s64 tmp = (s64)(LWORD(*DATA(r_0)) * HWORD(*DATA(r_8)));
		u64 res = (u64)tmp + *DATA(r_12);

		*DATA(r_12) = (u32) res;
		*DATA(r_16) += (res + ((tmp<0)*0xFFFFFFFF));

		GOTO_NEXTOP(2)
	}
};

DCL_OP_START(OP_SMLAL_T_B)
	u32 *r_0;
	u32 *r_8;
	u32 *r_12;
	u32 *r_16;

	DCL_OP_COMPILER(OP_SMLAL_T_B)
		DATA(r_0) = &(ARM_REGPOS_R(i, 0));
		DATA(r_8) = &(ARM_REGPOS_R(i, 8));
		DATA(r_12) = &(ARM_REGPOS_RW(i, 12));
		DATA(r_16) = &(ARM_REGPOS_RW(i, 16));
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_SMLAL_T_B)
		s64 tmp = (s64)(HWORD(*DATA(r_0)) * (s64)LWORD(*DATA(r_8)));
		u64 res = (u64)tmp + *DATA(r_12);

		*DATA(r_12) = (u32) res;
		*DATA(r_16) += (res + ((tmp<0)*0xFFFFFFFF));

		GOTO_NEXTOP(2)
	}
};

DCL_OP_START(OP_SMLAL_T_T)
	u32 *r_0;
	u32 *r_8;
	u32 *r_12;
	u32 *r_16;

	DCL_OP_COMPILER(OP_SMLAL_T_T)
		DATA(r_0) = &(ARM_REGPOS_R(i, 0));
		DATA(r_8) = &(ARM_REGPOS_R(i, 8));
		DATA(r_12) = &(ARM_REGPOS_RW(i, 12));
		DATA(r_16) = &(ARM_REGPOS_RW(i, 16));
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_SMLAL_T_T)
		s64 tmp = (s64)(HWORD(*DATA(r_0)) * HWORD(*DATA(r_8)));
		u64 res = (u64)tmp + *DATA(r_12);

		*DATA(r_12) = (u32) res;
		*DATA(r_16) += (res + ((tmp<0)*0xFFFFFFFF));

		GOTO_NEXTOP(2)
	}
};

//-----------------------------------------------------------------------------
//   SMULW
//-----------------------------------------------------------------------------
DCL_OP_START(OP_SMULW_B)
	u32 *r_0;
	u32 *r_8;
	u32 *r_16;

	DCL_OP_COMPILER(OP_SMULW_B)
		DATA(r_0) = &(ARM_REGPOS_R(i, 0));
		DATA(r_8) = &(ARM_REGPOS_R(i, 8));
		DATA(r_16) = &(ARM_REGPOS_W(i, 16));
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_SMULW_B)
		s64 tmp = (s64)LWORD(*DATA(r_8)) * (s64)(s32)(*DATA(r_0));

		*DATA(r_16) = ((tmp>>16)&0xFFFFFFFF);

		GOTO_NEXTOP(2)
	}
};

DCL_OP_START(OP_SMULW_T)
	u32 *r_0;
	u32 *r_8;
	u32 *r_16;

	DCL_OP_COMPILER(OP_SMULW_T)
		DATA(r_0) = &(ARM_REGPOS_R(i, 0));
		DATA(r_8) = &(ARM_REGPOS_R(i, 8));
		DATA(r_16) = &(ARM_REGPOS_W(i, 16));
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_SMULW_T)
		s64 tmp = (s64)HWORD(*DATA(r_8)) * (s64)(s32)(*DATA(r_0));

		*DATA(r_16) = ((tmp>>16)&0xFFFFFFFF);

		GOTO_NEXTOP(2)
	}
};

//-----------------------------------------------------------------------------
//   SMLAW
//-----------------------------------------------------------------------------
DCL_OP_START(OP_SMLAW_B)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_8;
	u32 *r_12;
	u32 *r_16;

	DCL_OP_COMPILER(OP_SMLAW_B)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(ARM_REGPOS_R(i, 0));
		DATA(r_8) = &(ARM_REGPOS_R(i, 8));
		DATA(r_12) = &(ARM_REGPOS_R(i, 12));
		DATA(r_16) = &(ARM_REGPOS_W(i, 16));
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_SMLAW_B)
		s64 tmp = (s64)LWORD(*DATA(r_8)) * (s64)(s32)(*DATA(r_0));
		u32 a = *DATA(r_12);

		tmp = (tmp>>16);
		
		*DATA(r_16) = tmp + a;

		if (SIGNED_OVERFLOW((u32)tmp, a, *DATA(r_16)))
			DATA(cpsr)->bits.Q=1;

		GOTO_NEXTOP(2)
	}
};

DCL_OP_START(OP_SMLAW_T)
	Status_Reg *cpsr;
	u32 *r_0;
	u32 *r_8;
	u32 *r_12;
	u32 *r_16;

	DCL_OP_COMPILER(OP_SMLAW_T)
		DATA(cpsr) = &(GETCPU->CPSR);
		DATA(r_0) = &(ARM_REGPOS_R(i, 0));
		DATA(r_8) = &(ARM_REGPOS_R(i, 8));
		DATA(r_12) = &(ARM_REGPOS_R(i, 12));
		DATA(r_16) = &(ARM_REGPOS_W(i, 16));
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_SMLAW_T)
		s64 tmp = (s64)HWORD(*DATA(r_8)) * (s64)(s32)(*DATA(r_0));
		u32 a = *DATA(r_12);

		tmp = ((tmp>>16)&0xFFFFFFFF);
		
		*DATA(r_16) = tmp + a;

		if (SIGNED_OVERFLOW((u32)tmp, a, *DATA(r_16)))
			DATA(cpsr)->bits.Q=1;

		GOTO_NEXTOP(2)
	}
};

//-----------------------------------------------------------------------------
//   LDR/STR macros
//-----------------------------------------------------------------------------
#define IMM_OFF_12_DATA \
	u32 offset;
#define IMM_OFF_12_COMPILER \
	DATA(offset) = ((i)&0xFFF);
#define IMM_OFF_12 

#define IMM_OFF_12

//-----------------------------------------------------------------------------
//   LDR
//-----------------------------------------------------------------------------
#define OP_LDR_PRE_DATA \
	armcpu_t *cpu; \
	Status_Reg *cpsr; \
	u32 *r_12; \
	u32 *r_16; \
	bool mod_r15;
#define OP_LDR_PRE_COMPILER \
	DATA(cpu) = GETCPU; \
	DATA(cpsr) = &(GETCPU->CPSR); \
	DATA(r_12) = &(ARM_REGPOS_W(i, 12)); \
	DATA(r_16) = &(ARM_REGPOS_R(i, 16)); \
	DATA(mod_r15) = REG_POS(i, 12) == 15;
#define OP_LDR_PRE(a, b) \
	*DATA(r_12) = ROR(READ32(DATA(cpu)->mem_if->data, adr), 8*(adr&3)); \
	if (DATA(mod_r15)) \
	{ \
		if (PROCNUM == 0) \
		{ \
			DATA(cpsr)->bits.T = BIT0(*DATA(r_12)); \
			*DATA(r_12) &= 0xFFFFFFFE; \
		} \
		else \
		{ \
			*DATA(r_12) &= 0xFFFFFFFC; \
		} \
		u32 c = MMU_aluMemAccessCycles<PROCNUM,32,MMU_AD_READ>(b,adr); \
		GOTO_NEXTOP(c) \
	} \
	u32 c = MMU_aluMemAccessCycles<PROCNUM,32,MMU_AD_READ>(a,adr); \
	GOTO_NEXTOP(c) 

#define OP_LDR_PRE_WB_DATA \
	armcpu_t *cpu; \
	Status_Reg *cpsr; \
	u32 *r_12; \
	u32 *r_16; \
	bool mod_r15;
#define OP_LDR_PRE_WB_COMPILER \
	DATA(cpu) = GETCPU; \
	DATA(cpsr) = &(GETCPU->CPSR); \
	DATA(r_12) = &(ARM_REGPOS_W(i, 12)); \
	DATA(r_16) = &(ARM_REGPOS_RW(i, 16)); \
	DATA(mod_r15) = REG_POS(i, 12) == 15;
#define OP_LDR_PRE_WB(a, b) \
	*DATA(r_16) = adr; \
	*DATA(r_12) = ROR(READ32(DATA(cpu)->mem_if->data, adr), 8*(adr&3)); \
	if (DATA(mod_r15)) \
	{ \
		if (PROCNUM == 0) \
		{ \
			DATA(cpsr)->bits.T = BIT0(*DATA(r_12)); \
			*DATA(r_12) &= 0xFFFFFFFE; \
		} \
		else \
		{ \
			*DATA(r_12) &= 0xFFFFFFFC; \
		} \
		u32 c = MMU_aluMemAccessCycles<PROCNUM,32,MMU_AD_READ>(b,adr); \
		GOTO_NEXTOP(c) \
	} \
	u32 c = MMU_aluMemAccessCycles<PROCNUM,32,MMU_AD_READ>(a,adr); \
	GOTO_NEXTOP(c) 

#define OP_LDR_POS_DATA \
	armcpu_t *cpu; \
	Status_Reg *cpsr; \
	u32 *r_12; \
	u32 *r_16; \
	bool mod_r15;
#define OP_LDR_POS_COMPILER \
	DATA(cpu) = GETCPU; \
	DATA(cpsr) = &(GETCPU->CPSR); \
	DATA(r_12) = &(ARM_REGPOS_W(i, 12)); \
	DATA(r_16) = &(ARM_REGPOS_RW(i, 16)); \
	DATA(mod_r15) = REG_POS(i, 12) == 15;
#define OP_LDR_POS(a, b) \
	u32 adr = *DATA(r_16); \
	*DATA(r_16) = adr + offset; \
	*DATA(r_12) = ROR(READ32(DATA(cpu)->mem_if->data, adr), 8*(adr&3)); \
	if (DATA(mod_r15)) \
	{ \
		if (PROCNUM == 0) \
		{ \
			DATA(cpsr)->bits.T = BIT0(*DATA(r_12)); \
			*DATA(r_12) &= 0xFFFFFFFE; \
		} \
		else \
		{ \
			*DATA(r_12) &= 0xFFFFFFFC; \
		} \
		u32 c = MMU_aluMemAccessCycles<PROCNUM,32,MMU_AD_READ>(b,adr); \
		GOTO_NEXTOP(c) \
	} \
	u32 c = MMU_aluMemAccessCycles<PROCNUM,32,MMU_AD_READ>(a,adr); \
	GOTO_NEXTOP(c) 

DCL_OP2EX_ARG2(OP_LDR_P_IMM_OFF, IMM_OFF_12, OP_LDR_PRE, u32 adr=*DATA(r_16)+DATA(offset), 3, 5)
DCL_OP2EX_ARG2(OP_LDR_M_IMM_OFF, IMM_OFF_12, OP_LDR_PRE, u32 adr=*DATA(r_16)-DATA(offset), 3, 5)
DCL_OP2EX_ARG2(OP_LDR_P_LSL_IMM_OFF, LSL_IMM, OP_LDR_PRE, u32 adr=*DATA(r_16)+shift_op, 3, 5)
DCL_OP2EX_ARG2(OP_LDR_M_LSL_IMM_OFF, LSL_IMM, OP_LDR_PRE, u32 adr=*DATA(r_16)-shift_op, 3, 5)
DCL_OP2EX_ARG2(OP_LDR_P_LSR_IMM_OFF, LSR_IMM, OP_LDR_PRE, u32 adr=*DATA(r_16)+shift_op, 3, 5)
DCL_OP2EX_ARG2(OP_LDR_M_LSR_IMM_OFF, LSR_IMM, OP_LDR_PRE, u32 adr=*DATA(r_16)-shift_op, 3, 5)
DCL_OP2EX_ARG2(OP_LDR_P_ASR_IMM_OFF, ASR_IMM, OP_LDR_PRE, u32 adr=*DATA(r_16)+shift_op, 3, 5)
DCL_OP2EX_ARG2(OP_LDR_M_ASR_IMM_OFF, ASR_IMM, OP_LDR_PRE, u32 adr=*DATA(r_16)-shift_op, 3, 5)
DCL_OP2EX_ARG2(OP_LDR_P_ROR_IMM_OFF, ROR_IMM2, OP_LDR_PRE, u32 adr=*DATA(r_16)+shift_op, 3, 5)
DCL_OP2EX_ARG2(OP_LDR_M_ROR_IMM_OFF, ROR_IMM2, OP_LDR_PRE, u32 adr=*DATA(r_16)-shift_op, 3, 5)

DCL_OP2EX_ARG2(OP_LDR_P_IMM_OFF_PREIND, IMM_OFF_12, OP_LDR_PRE_WB, u32 adr=*DATA(r_16)+DATA(offset), 3, 5)
DCL_OP2EX_ARG2(OP_LDR_M_IMM_OFF_PREIND, IMM_OFF_12, OP_LDR_PRE_WB, u32 adr=*DATA(r_16)-DATA(offset), 3, 5)
DCL_OP2EX_ARG2(OP_LDR_P_LSL_IMM_OFF_PREIND, LSL_IMM, OP_LDR_PRE_WB, u32 adr=*DATA(r_16)+shift_op, 3, 5)
DCL_OP2EX_ARG2(OP_LDR_M_LSL_IMM_OFF_PREIND, LSL_IMM, OP_LDR_PRE_WB, u32 adr=*DATA(r_16)-shift_op, 3, 5)
DCL_OP2EX_ARG2(OP_LDR_P_LSR_IMM_OFF_PREIND, LSR_IMM, OP_LDR_PRE_WB, u32 adr=*DATA(r_16)+shift_op, 3, 5)
DCL_OP2EX_ARG2(OP_LDR_M_LSR_IMM_OFF_PREIND, LSR_IMM, OP_LDR_PRE_WB, u32 adr=*DATA(r_16)-shift_op, 3, 5)
DCL_OP2EX_ARG2(OP_LDR_P_ASR_IMM_OFF_PREIND, ASR_IMM, OP_LDR_PRE_WB, u32 adr=*DATA(r_16)+shift_op, 3, 5)
DCL_OP2EX_ARG2(OP_LDR_M_ASR_IMM_OFF_PREIND, ASR_IMM, OP_LDR_PRE_WB, u32 adr=*DATA(r_16)-shift_op, 3, 5)
DCL_OP2EX_ARG2(OP_LDR_P_ROR_IMM_OFF_PREIND, ROR_IMM2, OP_LDR_PRE_WB, u32 adr=*DATA(r_16)+shift_op, 3, 5)
DCL_OP2EX_ARG2(OP_LDR_M_ROR_IMM_OFF_PREIND, ROR_IMM2, OP_LDR_PRE_WB, u32 adr=*DATA(r_16)-shift_op, 3, 5)

DCL_OP2EX_ARG2(OP_LDR_P_IMM_OFF_POSTIND, IMM_OFF_12, OP_LDR_POS, u32 offset=DATA(offset), 3, 5)
DCL_OP2EX_ARG2(OP_LDR_M_IMM_OFF_POSTIND, IMM_OFF_12, OP_LDR_POS, u32 offset=-DATA(offset), 3, 5)
DCL_OP2EX_ARG2(OP_LDR_P_LSL_IMM_OFF_POSTIND, LSL_IMM, OP_LDR_POS, u32 offset=shift_op, 3, 5)
DCL_OP2EX_ARG2(OP_LDR_M_LSL_IMM_OFF_POSTIND, LSL_IMM, OP_LDR_POS, u32 offset=-shift_op, 3, 5)
DCL_OP2EX_ARG2(OP_LDR_P_LSR_IMM_OFF_POSTIND, LSR_IMM, OP_LDR_POS, u32 offset=shift_op, 3, 5)
DCL_OP2EX_ARG2(OP_LDR_M_LSR_IMM_OFF_POSTIND, LSR_IMM, OP_LDR_POS, u32 offset=-shift_op, 3, 5)
DCL_OP2EX_ARG2(OP_LDR_P_ASR_IMM_OFF_POSTIND, ASR_IMM, OP_LDR_POS, u32 offset=shift_op, 3, 5)
DCL_OP2EX_ARG2(OP_LDR_M_ASR_IMM_OFF_POSTIND, ASR_IMM, OP_LDR_POS, u32 offset=-shift_op, 3, 5)
DCL_OP2EX_ARG2(OP_LDR_P_ROR_IMM_OFF_POSTIND, ROR_IMM2, OP_LDR_POS, u32 offset=shift_op, 3, 5)
DCL_OP2EX_ARG2(OP_LDR_M_ROR_IMM_OFF_POSTIND, ROR_IMM2, OP_LDR_POS, u32 offset=-shift_op, 3, 5)

//-----------------------------------------------------------------------------
//   LDREX
//-----------------------------------------------------------------------------
DCL_OP_START(OP_LDREX)
	armcpu_t *cpu;
	u32 *r_12;
	u32 *r_16;

	DCL_OP_COMPILER(OP_LDREX)
		DATA(cpu) = GETCPU;
		DATA(r_12) = &(ARM_REGPOS_W(i, 12));
		DATA(r_16) = &(ARM_REGPOS_R(i, 16));
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LDREX)
		u32 adr = *DATA(r_16);
		*DATA(r_12) = ROR(READ32(DATA(cpu)->mem_if->data, adr), 8*(adr&3));

		u32 c = MMU_aluMemAccessCycles<PROCNUM,32,MMU_AD_READ>(3,adr);

		GOTO_NEXTOP(c)
	}
};

//-----------------------------------------------------------------------------
//   LDRB
//-----------------------------------------------------------------------------
#define OP_LDRB_PRE_DATA \
	armcpu_t *cpu; \
	u32 *r_12; \
	u32 *r_16; 
#define OP_LDRB_PRE_COMPILER \
	DATA(cpu) = GETCPU; \
	DATA(r_12) = &(ARM_REGPOS_W(i, 12)); \
	DATA(r_16) = &(ARM_REGPOS_R(i, 16)); 
#define OP_LDRB_PRE(a, b) \
	*DATA(r_12) = (u32)READ8(DATA(cpu)->mem_if->data, adr); \
	u32 c = MMU_aluMemAccessCycles<PROCNUM,8,MMU_AD_READ>(3,adr); \
	GOTO_NEXTOP(c) 

#define OP_LDRB_PRE_WB_DATA \
	armcpu_t *cpu; \
	u32 *r_12; \
	u32 *r_16; 
#define OP_LDRB_PRE_WB_COMPILER \
	DATA(cpu) = GETCPU; \
	DATA(r_12) = &(ARM_REGPOS_W(i, 12)); \
	DATA(r_16) = &(ARM_REGPOS_RW(i, 16)); 
#define OP_LDRB_PRE_WB(a, b) \
	*DATA(r_16) = adr; \
	*DATA(r_12) = (u32)READ8(DATA(cpu)->mem_if->data, adr); \
	u32 c = MMU_aluMemAccessCycles<PROCNUM,8,MMU_AD_READ>(3,adr); \
	GOTO_NEXTOP(c) 

#define OP_LDRB_POS_DATA \
	armcpu_t *cpu; \
	u32 *r_12; \
	u32 *r_16; 
#define OP_LDRB_POS_COMPILER \
	DATA(cpu) = GETCPU; \
	DATA(r_12) = &(ARM_REGPOS_W(i, 12)); \
	DATA(r_16) = &(ARM_REGPOS_RW(i, 16)); 
#define OP_LDRB_POS(a, b) \
	u32 adr = *DATA(r_16); \
	*DATA(r_16) = adr + offset; \
	*DATA(r_12) = (u32)READ8(DATA(cpu)->mem_if->data, adr); \
	u32 c = MMU_aluMemAccessCycles<PROCNUM,8,MMU_AD_READ>(3,adr); \
	GOTO_NEXTOP(c) 

DCL_OP2EX_ARG2(OP_LDRB_P_IMM_OFF, IMM_OFF_12, OP_LDRB_PRE, u32 adr=*DATA(r_16)+DATA(offset), 3, 3)
DCL_OP2EX_ARG2(OP_LDRB_M_IMM_OFF, IMM_OFF_12, OP_LDRB_PRE, u32 adr=*DATA(r_16)-DATA(offset), 3, 3)
DCL_OP2EX_ARG2(OP_LDRB_P_LSL_IMM_OFF, LSL_IMM, OP_LDRB_PRE, u32 adr=*DATA(r_16)+shift_op, 3, 3)
DCL_OP2EX_ARG2(OP_LDRB_M_LSL_IMM_OFF, LSL_IMM, OP_LDRB_PRE, u32 adr=*DATA(r_16)-shift_op, 3, 3)
DCL_OP2EX_ARG2(OP_LDRB_P_LSR_IMM_OFF, LSR_IMM, OP_LDRB_PRE, u32 adr=*DATA(r_16)+shift_op, 3, 3)
DCL_OP2EX_ARG2(OP_LDRB_M_LSR_IMM_OFF, LSR_IMM, OP_LDRB_PRE, u32 adr=*DATA(r_16)-shift_op, 3, 3)
DCL_OP2EX_ARG2(OP_LDRB_P_ASR_IMM_OFF, ASR_IMM, OP_LDRB_PRE, u32 adr=*DATA(r_16)+shift_op, 3, 3)
DCL_OP2EX_ARG2(OP_LDRB_M_ASR_IMM_OFF, ASR_IMM, OP_LDRB_PRE, u32 adr=*DATA(r_16)-shift_op, 3, 3)
DCL_OP2EX_ARG2(OP_LDRB_P_ROR_IMM_OFF, ROR_IMM, OP_LDRB_PRE, u32 adr=*DATA(r_16)+shift_op, 3, 3)
DCL_OP2EX_ARG2(OP_LDRB_M_ROR_IMM_OFF, ROR_IMM, OP_LDRB_PRE, u32 adr=*DATA(r_16)-shift_op, 3, 3)

DCL_OP2EX_ARG2(OP_LDRB_P_IMM_OFF_PREIND, IMM_OFF_12, OP_LDRB_PRE_WB, u32 adr=*DATA(r_16)+DATA(offset), 3, 3)
DCL_OP2EX_ARG2(OP_LDRB_M_IMM_OFF_PREIND, IMM_OFF_12, OP_LDRB_PRE_WB, u32 adr=*DATA(r_16)-DATA(offset), 3, 3)
DCL_OP2EX_ARG2(OP_LDRB_P_LSL_IMM_OFF_PREIND, LSL_IMM, OP_LDRB_PRE_WB, u32 adr=*DATA(r_16)+shift_op, 3, 3)
DCL_OP2EX_ARG2(OP_LDRB_M_LSL_IMM_OFF_PREIND, LSL_IMM, OP_LDRB_PRE_WB, u32 adr=*DATA(r_16)-shift_op, 3, 3)
DCL_OP2EX_ARG2(OP_LDRB_P_LSR_IMM_OFF_PREIND, LSR_IMM, OP_LDRB_PRE_WB, u32 adr=*DATA(r_16)+shift_op, 3, 3)
DCL_OP2EX_ARG2(OP_LDRB_M_LSR_IMM_OFF_PREIND, LSR_IMM, OP_LDRB_PRE_WB, u32 adr=*DATA(r_16)-shift_op, 3, 3)
DCL_OP2EX_ARG2(OP_LDRB_P_ASR_IMM_OFF_PREIND, ASR_IMM, OP_LDRB_PRE_WB, u32 adr=*DATA(r_16)+shift_op, 3, 3)
DCL_OP2EX_ARG2(OP_LDRB_M_ASR_IMM_OFF_PREIND, ASR_IMM, OP_LDRB_PRE_WB, u32 adr=*DATA(r_16)-shift_op, 3, 3)
DCL_OP2EX_ARG2(OP_LDRB_P_ROR_IMM_OFF_PREIND, ROR_IMM, OP_LDRB_PRE_WB, u32 adr=*DATA(r_16)+shift_op, 3, 3)
DCL_OP2EX_ARG2(OP_LDRB_M_ROR_IMM_OFF_PREIND, ROR_IMM, OP_LDRB_PRE_WB, u32 adr=*DATA(r_16)-shift_op, 3, 3)

DCL_OP2EX_ARG2(OP_LDRB_P_IMM_OFF_POSTIND, IMM_OFF_12, OP_LDRB_POS, u32 offset=DATA(offset), 3, 3)
DCL_OP2EX_ARG2(OP_LDRB_M_IMM_OFF_POSTIND, IMM_OFF_12, OP_LDRB_POS, u32 offset=-DATA(offset), 3, 3)
DCL_OP2EX_ARG2(OP_LDRB_P_LSL_IMM_OFF_POSTIND, LSL_IMM, OP_LDRB_POS, u32 offset=shift_op, 3, 3)
DCL_OP2EX_ARG2(OP_LDRB_M_LSL_IMM_OFF_POSTIND, LSL_IMM, OP_LDRB_POS, u32 offset=-shift_op, 3, 3)
DCL_OP2EX_ARG2(OP_LDRB_P_LSR_IMM_OFF_POSTIND, LSR_IMM, OP_LDRB_POS, u32 offset=shift_op, 3, 3)
DCL_OP2EX_ARG2(OP_LDRB_M_LSR_IMM_OFF_POSTIND, LSR_IMM, OP_LDRB_POS, u32 offset=-shift_op, 3, 3)
DCL_OP2EX_ARG2(OP_LDRB_P_ASR_IMM_OFF_POSTIND, ASR_IMM, OP_LDRB_POS, u32 offset=shift_op, 3, 3)
DCL_OP2EX_ARG2(OP_LDRB_M_ASR_IMM_OFF_POSTIND, ASR_IMM, OP_LDRB_POS, u32 offset=-shift_op, 3, 3)
DCL_OP2EX_ARG2(OP_LDRB_P_ROR_IMM_OFF_POSTIND, ROR_IMM, OP_LDRB_POS, u32 offset=shift_op, 3, 3)
DCL_OP2EX_ARG2(OP_LDRB_M_ROR_IMM_OFF_POSTIND, ROR_IMM, OP_LDRB_POS, u32 offset=-shift_op, 3, 3)

//-----------------------------------------------------------------------------
//   STR
//-----------------------------------------------------------------------------
#define OP_STR_PRE_DATA \
	armcpu_t *cpu; \
	u32 *r_12; \
	u32 *r_16; 
#define OP_STR_PRE_COMPILER \
	DATA(cpu) = GETCPU; \
	DATA(r_12) = &(ARM_REGPOS_R(i, 12)); \
	DATA(r_16) = &(ARM_REGPOS_R(i, 16)); 
#define OP_STR_PRE(a, b) \
	WRITE32(DATA(cpu)->mem_if->data, adr, *DATA(r_12)); \
	u32 c = MMU_aluMemAccessCycles<PROCNUM,32,MMU_AD_WRITE>(2,adr); \
	GOTO_NEXTOP(c) 

#define OP_STR_PRE_WB_DATA \
	armcpu_t *cpu; \
	u32 *r_12; \
	u32 *r_16; 
#define OP_STR_PRE_WB_COMPILER \
	DATA(cpu) = GETCPU; \
	DATA(r_12) = &(ARM_REGPOS_R(i, 12)); \
	DATA(r_16) = &(ARM_REGPOS_RW(i, 16)); 
#define OP_STR_PRE_WB(a, b) \
	*DATA(r_16) = adr; \
	WRITE32(DATA(cpu)->mem_if->data, adr, *DATA(r_12)); \
	u32 c = MMU_aluMemAccessCycles<PROCNUM,32,MMU_AD_WRITE>(2,adr); \
	GOTO_NEXTOP(c) 

#define OP_STR_POS_DATA \
	armcpu_t *cpu; \
	u32 *r_12; \
	u32 *r_16; 
#define OP_STR_POS_COMPILER \
	DATA(cpu) = GETCPU; \
	DATA(r_12) = &(ARM_REGPOS_R(i, 12)); \
	DATA(r_16) = &(ARM_REGPOS_RW(i, 16)); 
#define OP_STR_POS(a, b) \
	u32 adr = *DATA(r_16); \
	WRITE32(DATA(cpu)->mem_if->data, adr, *DATA(r_12)); \
	*DATA(r_16) = adr + offset; \
	u32 c = MMU_aluMemAccessCycles<PROCNUM,32,MMU_AD_WRITE>(2,adr); \
	GOTO_NEXTOP(c) 

DCL_OP2EX_ARG2(OP_STR_P_IMM_OFF, IMM_OFF_12, OP_STR_PRE, u32 adr=*DATA(r_16)+DATA(offset), 2, 2)
DCL_OP2EX_ARG2(OP_STR_M_IMM_OFF, IMM_OFF_12, OP_STR_PRE, u32 adr=*DATA(r_16)-DATA(offset), 2, 2)
DCL_OP2EX_ARG2(OP_STR_P_LSL_IMM_OFF, LSL_IMM, OP_STR_PRE, u32 adr=*DATA(r_16)+shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STR_M_LSL_IMM_OFF, LSL_IMM, OP_STR_PRE, u32 adr=*DATA(r_16)-shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STR_P_LSR_IMM_OFF, LSR_IMM, OP_STR_PRE, u32 adr=*DATA(r_16)+shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STR_M_LSR_IMM_OFF, LSR_IMM, OP_STR_PRE, u32 adr=*DATA(r_16)-shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STR_P_ASR_IMM_OFF, ASR_IMM, OP_STR_PRE, u32 adr=*DATA(r_16)+shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STR_M_ASR_IMM_OFF, ASR_IMM, OP_STR_PRE, u32 adr=*DATA(r_16)-shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STR_P_ROR_IMM_OFF, ROR_IMM, OP_STR_PRE, u32 adr=*DATA(r_16)+shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STR_M_ROR_IMM_OFF, ROR_IMM, OP_STR_PRE, u32 adr=*DATA(r_16)-shift_op, 2, 2)

DCL_OP2EX_ARG2(OP_STR_P_IMM_OFF_PREIND, IMM_OFF_12, OP_STR_PRE_WB, u32 adr=*DATA(r_16)+DATA(offset), 2, 2)
DCL_OP2EX_ARG2(OP_STR_M_IMM_OFF_PREIND, IMM_OFF_12, OP_STR_PRE_WB, u32 adr=*DATA(r_16)-DATA(offset), 2, 2)
DCL_OP2EX_ARG2(OP_STR_P_LSL_IMM_OFF_PREIND, LSL_IMM, OP_STR_PRE_WB, u32 adr=*DATA(r_16)+shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STR_M_LSL_IMM_OFF_PREIND, LSL_IMM, OP_STR_PRE_WB, u32 adr=*DATA(r_16)-shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STR_P_LSR_IMM_OFF_PREIND, LSR_IMM, OP_STR_PRE_WB, u32 adr=*DATA(r_16)+shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STR_M_LSR_IMM_OFF_PREIND, LSR_IMM, OP_STR_PRE_WB, u32 adr=*DATA(r_16)-shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STR_P_ASR_IMM_OFF_PREIND, ASR_IMM, OP_STR_PRE_WB, u32 adr=*DATA(r_16)+shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STR_M_ASR_IMM_OFF_PREIND, ASR_IMM, OP_STR_PRE_WB, u32 adr=*DATA(r_16)-shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STR_P_ROR_IMM_OFF_PREIND, ROR_IMM, OP_STR_PRE_WB, u32 adr=*DATA(r_16)+shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STR_M_ROR_IMM_OFF_PREIND, ROR_IMM, OP_STR_PRE_WB, u32 adr=*DATA(r_16)-shift_op, 2, 2)

DCL_OP2EX_ARG2(OP_STR_P_IMM_OFF_POSTIND, IMM_OFF_12, OP_STR_POS, u32 offset=DATA(offset), 2, 2)
DCL_OP2EX_ARG2(OP_STR_M_IMM_OFF_POSTIND, IMM_OFF_12, OP_STR_POS, u32 offset=-DATA(offset), 2, 2)
DCL_OP2EX_ARG2(OP_STR_P_LSL_IMM_OFF_POSTIND, LSL_IMM, OP_STR_POS, u32 offset=shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STR_M_LSL_IMM_OFF_POSTIND, LSL_IMM, OP_STR_POS, u32 offset=-shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STR_P_LSR_IMM_OFF_POSTIND, LSR_IMM, OP_STR_POS, u32 offset=shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STR_M_LSR_IMM_OFF_POSTIND, LSR_IMM, OP_STR_POS, u32 offset=-shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STR_P_ASR_IMM_OFF_POSTIND, ASR_IMM, OP_STR_POS, u32 offset=shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STR_M_ASR_IMM_OFF_POSTIND, ASR_IMM, OP_STR_POS, u32 offset=-shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STR_P_ROR_IMM_OFF_POSTIND, ROR_IMM, OP_STR_POS, u32 offset=shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STR_M_ROR_IMM_OFF_POSTIND, ROR_IMM, OP_STR_POS, u32 offset=-shift_op, 2, 2)

//-----------------------------------------------------------------------------
//   STREX
//-----------------------------------------------------------------------------
DCL_OP_START(OP_STREX)
	armcpu_t *cpu;
	u32 *r_0;
	u32 *r_12;
	u32 *r_16;

	DCL_OP_COMPILER(OP_STREX)
		DATA(cpu) = GETCPU;
		DATA(r_0) = &(ARM_REGPOS_R(i, 0));
		DATA(r_12) = &(ARM_REGPOS_W(i, 12));
		DATA(r_16) = &(ARM_REGPOS_R(i, 16));
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_STREX)
		u32 adr = *DATA(r_16);
		WRITE32(DATA(cpu)->mem_if->data, adr, *DATA(r_0));
		*DATA(r_12) = 0;

		u32 c = MMU_aluMemAccessCycles<PROCNUM,32,MMU_AD_WRITE>(2,adr);

		GOTO_NEXTOP(c)
	}
};

//-----------------------------------------------------------------------------
//   STRB
//-----------------------------------------------------------------------------
#define OP_STRB_PRE_DATA \
	armcpu_t *cpu; \
	u32 *r_12; \
	u32 *r_16; 
#define OP_STRB_PRE_COMPILER \
	DATA(cpu) = GETCPU; \
	DATA(r_12) = &(ARM_REGPOS_R(i, 12)); \
	DATA(r_16) = &(ARM_REGPOS_R(i, 16)); 
#define OP_STRB_PRE(a, b) \
	WRITE8(DATA(cpu)->mem_if->data, adr, (u8)*DATA(r_12)); \
	u32 c = MMU_aluMemAccessCycles<PROCNUM,8,MMU_AD_WRITE>(2,adr); \
	GOTO_NEXTOP(c) 

#define OP_STRB_PRE_WB_DATA \
	armcpu_t *cpu; \
	u32 *r_12; \
	u32 *r_16; 
#define OP_STRB_PRE_WB_COMPILER \
	DATA(cpu) = GETCPU; \
	DATA(r_12) = &(ARM_REGPOS_R(i, 12)); \
	DATA(r_16) = &(ARM_REGPOS_RW(i, 16)); 
#define OP_STRB_PRE_WB(a, b) \
	*DATA(r_16) = adr; \
	WRITE8(DATA(cpu)->mem_if->data, adr, (u8)*DATA(r_12)); \
	u32 c = MMU_aluMemAccessCycles<PROCNUM,8,MMU_AD_WRITE>(2,adr); \
	GOTO_NEXTOP(c) 

#define OP_STRB_POS_DATA \
	armcpu_t *cpu; \
	u32 *r_12; \
	u32 *r_16; 
#define OP_STRB_POS_COMPILER \
	DATA(cpu) = GETCPU; \
	DATA(r_12) = &(ARM_REGPOS_R(i, 12)); \
	DATA(r_16) = &(ARM_REGPOS_RW(i, 16)); 
#define OP_STRB_POS(a, b) \
	u32 adr = *DATA(r_16); \
	WRITE8(DATA(cpu)->mem_if->data, adr, (u8)*DATA(r_12)); \
	*DATA(r_16) = adr + offset; \
	u32 c = MMU_aluMemAccessCycles<PROCNUM,8,MMU_AD_WRITE>(2,adr); \
	GOTO_NEXTOP(c) 

DCL_OP2EX_ARG2(OP_STRB_P_IMM_OFF, IMM_OFF_12, OP_STRB_PRE, u32 adr=*DATA(r_16)+DATA(offset), 2, 2)
DCL_OP2EX_ARG2(OP_STRB_M_IMM_OFF, IMM_OFF_12, OP_STRB_PRE, u32 adr=*DATA(r_16)-DATA(offset), 2, 2)
DCL_OP2EX_ARG2(OP_STRB_P_LSL_IMM_OFF, LSL_IMM, OP_STRB_PRE, u32 adr=*DATA(r_16)+shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STRB_M_LSL_IMM_OFF, LSL_IMM, OP_STRB_PRE, u32 adr=*DATA(r_16)-shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STRB_P_LSR_IMM_OFF, LSR_IMM, OP_STRB_PRE, u32 adr=*DATA(r_16)+shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STRB_M_LSR_IMM_OFF, LSR_IMM, OP_STRB_PRE, u32 adr=*DATA(r_16)-shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STRB_P_ASR_IMM_OFF, ASR_IMM, OP_STRB_PRE, u32 adr=*DATA(r_16)+shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STRB_M_ASR_IMM_OFF, ASR_IMM, OP_STRB_PRE, u32 adr=*DATA(r_16)-shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STRB_P_ROR_IMM_OFF, ROR_IMM, OP_STRB_PRE, u32 adr=*DATA(r_16)+shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STRB_M_ROR_IMM_OFF, ROR_IMM, OP_STRB_PRE, u32 adr=*DATA(r_16)-shift_op, 2, 2)

DCL_OP2EX_ARG2(OP_STRB_P_IMM_OFF_PREIND, IMM_OFF_12, OP_STRB_PRE_WB, u32 adr=*DATA(r_16)+DATA(offset), 2, 2)
DCL_OP2EX_ARG2(OP_STRB_M_IMM_OFF_PREIND, IMM_OFF_12, OP_STRB_PRE_WB, u32 adr=*DATA(r_16)-DATA(offset), 2, 2)
DCL_OP2EX_ARG2(OP_STRB_P_LSL_IMM_OFF_PREIND, LSL_IMM, OP_STRB_PRE_WB, u32 adr=*DATA(r_16)+shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STRB_M_LSL_IMM_OFF_PREIND, LSL_IMM, OP_STRB_PRE_WB, u32 adr=*DATA(r_16)-shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STRB_P_LSR_IMM_OFF_PREIND, LSR_IMM, OP_STRB_PRE_WB, u32 adr=*DATA(r_16)+shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STRB_M_LSR_IMM_OFF_PREIND, LSR_IMM, OP_STRB_PRE_WB, u32 adr=*DATA(r_16)-shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STRB_P_ASR_IMM_OFF_PREIND, ASR_IMM, OP_STRB_PRE_WB, u32 adr=*DATA(r_16)+shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STRB_M_ASR_IMM_OFF_PREIND, ASR_IMM, OP_STRB_PRE_WB, u32 adr=*DATA(r_16)-shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STRB_P_ROR_IMM_OFF_PREIND, ROR_IMM, OP_STRB_PRE_WB, u32 adr=*DATA(r_16)+shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STRB_M_ROR_IMM_OFF_PREIND, ROR_IMM, OP_STRB_PRE_WB, u32 adr=*DATA(r_16)-shift_op, 2, 2)

DCL_OP2EX_ARG2(OP_STRB_P_IMM_OFF_POSTIND, IMM_OFF_12, OP_STRB_POS, u32 offset=DATA(offset), 2, 2)
DCL_OP2EX_ARG2(OP_STRB_M_IMM_OFF_POSTIND, IMM_OFF_12, OP_STRB_POS, u32 offset=-DATA(offset), 2, 2)
DCL_OP2EX_ARG2(OP_STRB_P_LSL_IMM_OFF_POSTIND, LSL_IMM, OP_STRB_POS, u32 offset=shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STRB_M_LSL_IMM_OFF_POSTIND, LSL_IMM, OP_STRB_POS, u32 offset=-shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STRB_P_LSR_IMM_OFF_POSTIND, LSR_IMM, OP_STRB_POS, u32 offset=shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STRB_M_LSR_IMM_OFF_POSTIND, LSR_IMM, OP_STRB_POS, u32 offset=-shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STRB_P_ASR_IMM_OFF_POSTIND, ASR_IMM, OP_STRB_POS, u32 offset=shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STRB_M_ASR_IMM_OFF_POSTIND, ASR_IMM, OP_STRB_POS, u32 offset=-shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STRB_P_ROR_IMM_OFF_POSTIND, ROR_IMM, OP_STRB_POS, u32 offset=shift_op, 2, 2)
DCL_OP2EX_ARG2(OP_STRB_M_ROR_IMM_OFF_POSTIND, ROR_IMM, OP_STRB_POS, u32 offset=-shift_op, 2, 2)

//-----------------------------------------------------------------------------
//   LDMIA / LDMIB / LDMDA / LDMDB
//-----------------------------------------------------------------------------
DCL_OP_START(OP_LDMIA)
	armcpu_t *cpu;
	Status_Reg *cpsr; 
	u32 *r_16;
	u32 *r[15+1];
	u32 *r_15;

	DCL_OP_COMPILER(OP_LDMIA)
		DATA(cpu) = GETCPU;
		DATA(cpsr) = &(GETCPU->CPSR); 
		DATA(r_16) = &(ARM_REGPOS_R(i, 16));
		if (BIT15(i))
			DATA(r_15) = &(GETCPUREG_W(15));
		else
			DATA(r_15) = NULL;

		u32 count = 0;
		for (s32 j = 0; j <= 14; j++)
		{
			if (BIT_N(i,j))
			{
				DATA(r[count++]) = &(GETCPUREG_R(j));
			}
		}
		DATA(r[count]) = NULL;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LDMIA)
		u32 adr = *DATA(r_16);
		u8 c = 0;
		
		u32 count = 0;
		while (DATA(r[count]))
		{
			*DATA(r[count++]) = READ32(DATA(cpu)->mem_if->data, adr);
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
			adr += 4;
		}

		if (DATA(r_15))
		{
			u32 tmp = READ32(DATA(cpu)->mem_if->data, adr);

			if (PROCNUM == 0)
			{
				DATA(cpsr)->bits.T = BIT0(tmp);
				*DATA(r_15) = tmp & 0xFFFFFFFE;
			}
			else
			{
				*DATA(r_15) = tmp & 0xFFFFFFFC;
			}
			//adr += 4;
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
		}

		c = MMU_aluMemCycles<PROCNUM>(2, c);

		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_LDMIB)
	armcpu_t *cpu;
	Status_Reg *cpsr; 
	u32 *r_16;
	u32 *r[15+1];
	u32 *r_15;

	DCL_OP_COMPILER(OP_LDMIB)
		DATA(cpu) = GETCPU;
		DATA(cpsr) = &(GETCPU->CPSR); 
		DATA(r_16) = &(ARM_REGPOS_R(i, 16));
		if (BIT15(i))
			DATA(r_15) = &(GETCPUREG_W(15));
		else
			DATA(r_15) = NULL;

		u32 count = 0;
		for (s32 j = 0; j <= 14; j++)
		{
			if (BIT_N(i,j))
			{
				DATA(r[count++]) = &(GETCPUREG_W(j));
			}
		}
		DATA(r[count]) = NULL;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LDMIB)
		u32 adr = *DATA(r_16);
		u8 c = 0;
		
		u32 count = 0;
		while (DATA(r[count]))
		{
			adr += 4;
			*DATA(r[count++]) = READ32(DATA(cpu)->mem_if->data, adr);
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
		}

		if (DATA(r_15))
		{
			adr += 4;
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
			u32 tmp = READ32(DATA(cpu)->mem_if->data, adr);

			if (PROCNUM == 0)
			{
				DATA(cpsr)->bits.T = BIT0(tmp);
				*DATA(r_15) = tmp & 0xFFFFFFFE;
			}
			else
			{
				*DATA(r_15) = tmp & 0xFFFFFFFC;
			}
			
			c = MMU_aluMemCycles<PROCNUM>(4, c);

			GOTO_NEXTOP(c)
		}

		c = MMU_aluMemCycles<PROCNUM>(2, c);

		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_LDMDA)
	armcpu_t *cpu;
	Status_Reg *cpsr; 
	u32 *r_16;
	u32 *r[15+1];
	u32 *r_15;

	DCL_OP_COMPILER(OP_LDMDA)
		DATA(cpu) = GETCPU;
		DATA(cpsr) = &(GETCPU->CPSR); 
		DATA(r_16) = &(ARM_REGPOS_R(i, 16));
		if (BIT15(i))
			DATA(r_15) = &(GETCPUREG_W(15));
		else
			DATA(r_15) = NULL;

		u32 count = 0;
		for (s32 j = 14; j >= 0; j--)
		{
			if (BIT_N(i,j))
			{
				DATA(r[count++]) = &(GETCPUREG_W(j));
			}
		}
		DATA(r[count]) = NULL;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LDMDA)
		u32 adr = *DATA(r_16);
		u8 c = 0;

		if (DATA(r_15))
		{
			u32 tmp = READ32(DATA(cpu)->mem_if->data, adr);

			if (PROCNUM == 0)
			{
				DATA(cpsr)->bits.T = BIT0(tmp);
				*DATA(r_15) = tmp & 0xFFFFFFFE;
			}
			else
			{
				*DATA(r_15) = tmp & 0xFFFFFFFC;
			}
			
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);

			adr -= 4;
		}
		
		u32 count = 0;
		while (DATA(r[count]))
		{
			*DATA(r[count++]) = READ32(DATA(cpu)->mem_if->data, adr);
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
			adr -= 4;
		}

		c = MMU_aluMemCycles<PROCNUM>(2, c);

		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_LDMDB)
	armcpu_t *cpu;
	Status_Reg *cpsr; 
	u32 *r_16;
	u32 *r[15+1];
	u32 *r_15;

	DCL_OP_COMPILER(OP_LDMDB)
		DATA(cpu) = GETCPU;
		DATA(cpsr) = &(GETCPU->CPSR); 
		DATA(r_16) = &(ARM_REGPOS_R(i, 16));
		if (BIT15(i))
			DATA(r_15) = &(GETCPUREG_W(15));
		else
			DATA(r_15) = NULL;

		u32 count = 0;
		for (s32 j = 14; j >= 0; j--)
		{
			if (BIT_N(i,j))
			{
				DATA(r[count++]) = &(GETCPUREG_W(j));
			}
		}
		DATA(r[count]) = NULL;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LDMDB)
		u32 adr = *DATA(r_16);
		u8 c = 0;

		if (DATA(r_15))
		{
			adr -= 4;
			u32 tmp = READ32(DATA(cpu)->mem_if->data, adr);

			if (PROCNUM == 0)
			{
				DATA(cpsr)->bits.T = BIT0(tmp);
				*DATA(r_15) = tmp & 0xFFFFFFFE;
			}
			else
			{
				*DATA(r_15) = tmp & 0xFFFFFFFC;
			}
			
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
		}
		
		u32 count = 0;
		while (DATA(r[count]))
		{
			adr -= 4;
			*DATA(r[count++]) = READ32(DATA(cpu)->mem_if->data, adr);
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
		}

		c = MMU_aluMemCycles<PROCNUM>(2, c);

		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_LDMIA_W)
	armcpu_t *cpu;
	Status_Reg *cpsr; 
	u32 *r_16;
	u32 *r[15+1];
	u32 *r_15;
	bool wb_flg1;
	bool wb_flg2;

	DCL_OP_COMPILER(OP_LDMIA_W)
		DATA(cpu) = GETCPU;
		DATA(cpsr) = &(GETCPU->CPSR); 
		DATA(r_16) = &(ARM_REGPOS_RW(i, 16));
		if (BIT15(i))
			DATA(r_15) = &(GETCPUREG_W(15));
		else
			DATA(r_15) = NULL;
		DATA(wb_flg1) = (i & (1 << REG_POS(i,16)));
		DATA(wb_flg2) = (i & ((~((2 << REG_POS(i,16))-1)) & 0xFFFF));

		u32 count = 0;
		for (s32 j = 0; j <= 14; j++)
		{
			if (BIT_N(i,j))
			{
				DATA(r[count++]) = &(GETCPUREG_W(j));
			}
		}
		DATA(r[count]) = NULL;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LDMIA_W)
		u32 adr = *DATA(r_16);
		u8 c = 0;
		u8 alu_c = 2;
		
		u32 count = 0;
		while (DATA(r[count]))
		{
			*DATA(r[count++]) = READ32(DATA(cpu)->mem_if->data, adr);
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
			adr += 4;
		}

		if (DATA(r_15))
		{
			alu_c = 4;

			u32 tmp = READ32(DATA(cpu)->mem_if->data, adr);

			if (PROCNUM == 0)
			{
				DATA(cpsr)->bits.T = BIT0(tmp);
				*DATA(r_15) = tmp & 0xFFFFFFFE;
			}
			else
			{
				*DATA(r_15) = tmp & 0xFFFFFFFC;
			}
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
			adr += 4;
		}

		if (DATA(wb_flg1))
		{
			if (DATA(wb_flg2))
				*DATA(r_16) = adr;
		}
		else
		{
			*DATA(r_16) = adr;
		}

		c = MMU_aluMemCycles<PROCNUM>(alu_c, c);

		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_LDMIB_W)
	armcpu_t *cpu;
	Status_Reg *cpsr; 
	u32 *r_16;
	u32 *r[15+1];
	u32 *r_15;
	bool wb_flg1;
	bool wb_flg2;

	DCL_OP_COMPILER(OP_LDMIB_W)
		DATA(cpu) = GETCPU;
		DATA(cpsr) = &(GETCPU->CPSR); 
		DATA(r_16) = &(ARM_REGPOS_RW(i, 16));
		if (BIT15(i))
			DATA(r_15) = &(GETCPUREG_W(15));
		else
			DATA(r_15) = NULL;
		DATA(wb_flg1) = (i & (1 << REG_POS(i,16)));
		DATA(wb_flg2) = (i & ((~((2 << REG_POS(i,16))-1)) & 0xFFFF));

		u32 count = 0;
		for (s32 j = 0; j <= 14; j++)
		{
			if (BIT_N(i,j))
			{
				DATA(r[count++]) = &(GETCPUREG_W(j));
			}
		}
		DATA(r[count]) = NULL;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LDMIB_W)
		u32 adr = *DATA(r_16);
		u8 c = 0;
		u8 alu_c = 2;
		
		u32 count = 0;
		while (DATA(r[count]))
		{
			adr += 4;
			*DATA(r[count++]) = READ32(DATA(cpu)->mem_if->data, adr);
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
		}

		if (DATA(r_15))
		{
			alu_c = 4;

			adr += 4;
			u32 tmp = READ32(DATA(cpu)->mem_if->data, adr);

			if (PROCNUM == 0)
			{
				DATA(cpsr)->bits.T = BIT0(tmp);
				*DATA(r_15) = tmp & 0xFFFFFFFE;
			}
			else
			{
				*DATA(r_15) = tmp & 0xFFFFFFFC;
			}
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
		}

		if (DATA(wb_flg1))
		{
			if (DATA(wb_flg2))
				*DATA(r_16) = adr;
		}
		else
		{
			*DATA(r_16) = adr;
		}

		c = MMU_aluMemCycles<PROCNUM>(alu_c, c);

		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_LDMDA_W)
	armcpu_t *cpu;
	Status_Reg *cpsr; 
	u32 *r_16;
	u32 *r[15+1];
	u32 *r_15;
	bool wb_flg1;
	bool wb_flg2;

	DCL_OP_COMPILER(OP_LDMDA_W)
		DATA(cpu) = GETCPU;
		DATA(cpsr) = &(GETCPU->CPSR); 
		DATA(r_16) = &(ARM_REGPOS_RW(i, 16));
		if (BIT15(i))
			DATA(r_15) = &(GETCPUREG_W(15));
		else
			DATA(r_15) = NULL;
		DATA(wb_flg1) = (i & (1 << REG_POS(i,16)));
		DATA(wb_flg2) = (i & ((~((2 << REG_POS(i,16))-1)) & 0xFFFF));

		u32 count = 0;
		for (s32 j = 14; j >= 0; j--)
		{
			if (BIT_N(i,j))
			{
				DATA(r[count++]) = &(GETCPUREG_W(j));
			}
		}
		DATA(r[count]) = NULL;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LDMDA_W)
		u32 adr = *DATA(r_16);
		u8 c = 0;

		if (DATA(r_15))
		{
			u32 tmp = READ32(DATA(cpu)->mem_if->data, adr);

			if (PROCNUM == 0)
			{
				DATA(cpsr)->bits.T = BIT0(tmp);
				*DATA(r_15) = tmp & 0xFFFFFFFE;
			}
			else
			{
				*DATA(r_15) = tmp & 0xFFFFFFFC;
			}
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
			adr -= 4;
		}
		
		u32 count = 0;
		while (DATA(r[count]))
		{
			*DATA(r[count++]) = READ32(DATA(cpu)->mem_if->data, adr);
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
			adr -= 4;
		}

		if (DATA(wb_flg1))
		{
			if (DATA(wb_flg2))
				*DATA(r_16) = adr;
		}
		else
		{
			*DATA(r_16) = adr;
		}

		c = MMU_aluMemCycles<PROCNUM>(2, c);

		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_LDMDB_W)
	armcpu_t *cpu;
	Status_Reg *cpsr; 
	u32 *r_16;
	u32 *r[15+1];
	u32 *r_15;
	bool wb_flg1;
	bool wb_flg2;

	DCL_OP_COMPILER(OP_LDMDB_W)
		DATA(cpu) = GETCPU;
		DATA(cpsr) = &(GETCPU->CPSR); 
		DATA(r_16) = &(ARM_REGPOS_RW(i, 16));
		if (BIT15(i))
			DATA(r_15) = &(GETCPUREG_W(15));
		else
			DATA(r_15) = NULL;
		DATA(wb_flg1) = (i & (1 << REG_POS(i,16)));
		DATA(wb_flg2) = (i & ((~((2 << REG_POS(i,16))-1)) & 0xFFFF));

		u32 count = 0;
		for (s32 j = 14; j >= 0; j--)
		{
			if (BIT_N(i,j))
			{
				DATA(r[count++]) = &(GETCPUREG_W(j));
			}
		}
		DATA(r[count]) = NULL;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LDMDB_W)
		u32 adr = *DATA(r_16);
		u8 c = 0;

		if (DATA(r_15))
		{
			adr -= 4;

			u32 tmp = READ32(DATA(cpu)->mem_if->data, adr);

			if (PROCNUM == 0)
			{
				DATA(cpsr)->bits.T = BIT0(tmp);
				*DATA(r_15) = tmp & 0xFFFFFFFE;
			}
			else
			{
				*DATA(r_15) = tmp & 0xFFFFFFFC;
			}
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
		}
		
		u32 count = 0;
		while (DATA(r[count]))
		{
			adr -= 4;
			*DATA(r[count++]) = READ32(DATA(cpu)->mem_if->data, adr);
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
		}

		if (DATA(wb_flg1))
		{
			if (DATA(wb_flg2))
				*DATA(r_16) = adr;
		}
		else
		{
			*DATA(r_16) = adr;
		}

		c = MMU_aluMemCycles<PROCNUM>(2, c);

		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_LDMIA2)
	armcpu_t *cpu;
	Status_Reg *cpsr; 
	u32 *r_16;
	u32 *r[15+1];
	u32 *r_15;

	DCL_OP_COMPILER(OP_LDMIA2)
		DATA(cpu) = GETCPU;
		DATA(cpsr) = &(GETCPU->CPSR); 
		DATA(r_16) = &(ARM_REGPOS_R(i, 16));
		if (BIT15(i))
			DATA(r_15) = &(GETCPUREG_W(15));
		else
			DATA(r_15) = NULL;

		u32 count = 0;
		for (s32 j = 0; j <= 14; j++)
		{
			if (BIT_N(i,j))
			{
				DATA(r[count++]) = &(GETCPUREG_W(j));
			}
		}
		DATA(r[count]) = NULL;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LDMIA2)
		u32 adr = *DATA(r_16);
		u32 oldmode = 0;
		u8 c = 0;

		if (DATA(r_15) == NULL)
		{
			if((DATA(cpu)->CPSR.bits.mode==USR)||(DATA(cpu)->CPSR.bits.mode==SYS))
			{
				printf("ERROR1\n");
				GOTO_NEXTOP(1)
			}
			oldmode = armcpu_switchMode(DATA(cpu), SYS);
		}
		
		u32 count = 0;
		while (DATA(r[count]))
		{
			*DATA(r[count++]) = READ32(DATA(cpu)->mem_if->data, adr);
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
			adr += 4;
		}

		if (DATA(r_15) == NULL)
		{
			armcpu_switchMode(DATA(cpu), oldmode);
		}
		else
		{
			u32 tmp = READ32(DATA(cpu)->mem_if->data, adr);
			Status_Reg SPSR;
			*DATA(r_15) = tmp & (0XFFFFFFFC | (BIT0(tmp)<<1));
			SPSR = DATA(cpu)->SPSR;
			armcpu_switchMode(DATA(cpu), SPSR.bits.mode);
			DATA(cpu)->CPSR=SPSR;
			DATA(cpu)->changeCPSR();
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
			//adr += 4;
		}

		c = MMU_aluMemCycles<PROCNUM>(2, c);

		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_LDMIB2)
	armcpu_t *cpu;
	Status_Reg *cpsr; 
	u32 *r_16;
	u32 *r[15+1];
	u32 *r_15;

	DCL_OP_COMPILER(OP_LDMIB2)
		DATA(cpu) = GETCPU;
		DATA(cpsr) = &(GETCPU->CPSR); 
		DATA(r_16) = &(ARM_REGPOS_R(i, 16));
		if (BIT15(i))
			DATA(r_15) = &(GETCPUREG_W(15));
		else
			DATA(r_15) = NULL;

		u32 count = 0;
		for (s32 j = 0; j <= 14; j++)
		{
			if (BIT_N(i,j))
			{
				DATA(r[count++]) = &(GETCPUREG_W(j));
			}
		}
		DATA(r[count]) = NULL;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LDMIB2)
		u32 adr = *DATA(r_16);
		u32 oldmode = 0;
		u8 c = 0;

		if (DATA(r_15) == NULL)
		{
			if((DATA(cpu)->CPSR.bits.mode==USR)||(DATA(cpu)->CPSR.bits.mode==SYS))
			{
				printf("ERROR1\n");
				GOTO_NEXTOP(1)
			}
			oldmode = armcpu_switchMode(DATA(cpu), SYS);
		}
		
		u32 count = 0;
		while (DATA(r[count]))
		{
			adr += 4;
			*DATA(r[count++]) = READ32(DATA(cpu)->mem_if->data, adr);
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
		}

		if (DATA(r_15) == NULL)
		{
			armcpu_switchMode(DATA(cpu), oldmode);
		}
		else
		{
			adr += 4;
			u32 tmp = READ32(DATA(cpu)->mem_if->data, adr);
			Status_Reg SPSR;
			*DATA(r_15) = tmp & (0XFFFFFFFC | (BIT0(tmp)<<1));
			SPSR = DATA(cpu)->SPSR;
			armcpu_switchMode(DATA(cpu), SPSR.bits.mode);
			DATA(cpu)->CPSR=SPSR;
			DATA(cpu)->changeCPSR();
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
		}

		c = MMU_aluMemCycles<PROCNUM>(2, c);

		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_LDMDA2)
	armcpu_t *cpu;
	Status_Reg *cpsr; 
	u32 *r_16;
	u32 *r[15+1];
	u32 *r_15;

	DCL_OP_COMPILER(OP_LDMDA2)
		DATA(cpu) = GETCPU;
		DATA(cpsr) = &(GETCPU->CPSR); 
		DATA(r_16) = &(ARM_REGPOS_R(i, 16));
		if (BIT15(i))
			DATA(r_15) = &(GETCPUREG_W(15));
		else
			DATA(r_15) = NULL;

		u32 count = 0;
		for (s32 j = 14; j >= 0; j--)
		{
			if (BIT_N(i,j))
			{
				DATA(r[count++]) = &(GETCPUREG_W(j));
			}
		}
		DATA(r[count]) = NULL;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LDMDA2)
		u32 adr = *DATA(r_16);
		u32 oldmode = 0;
		u8 c = 0;

		if (DATA(r_15) == NULL)
		{
			if((DATA(cpu)->CPSR.bits.mode==USR)||(DATA(cpu)->CPSR.bits.mode==SYS))
			{
				printf("ERROR1\n");
				GOTO_NEXTOP(1)
			}
			oldmode = armcpu_switchMode(DATA(cpu), SYS);
		}
		else
		{
			u32 tmp = READ32(DATA(cpu)->mem_if->data, adr);
			*DATA(r_15) = tmp & (0XFFFFFFFC | (BIT0(tmp)<<1));
			DATA(cpu)->CPSR = DATA(cpu)->SPSR;
			DATA(cpu)->changeCPSR();
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
			adr -= 4;
		}
		
		u32 count = 0;
		while (DATA(r[count]))
		{
			*DATA(r[count++]) = READ32(DATA(cpu)->mem_if->data, adr);
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
			adr -= 4;
		}

		if (DATA(r_15) == NULL)
		{
			armcpu_switchMode(DATA(cpu), oldmode);
		}
		else
		{
			Status_Reg SPSR = DATA(cpu)->SPSR;
			armcpu_switchMode(DATA(cpu), SPSR.bits.mode);
			DATA(cpu)->CPSR=SPSR;
			DATA(cpu)->changeCPSR();
		}

		c = MMU_aluMemCycles<PROCNUM>(2, c);

		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_LDMDB2)
	armcpu_t *cpu;
	Status_Reg *cpsr; 
	u32 *r_16;
	u32 *r[15+1];
	u32 *r_15;

	DCL_OP_COMPILER(OP_LDMDB2)
		DATA(cpu) = GETCPU;
		DATA(cpsr) = &(GETCPU->CPSR); 
		DATA(r_16) = &(ARM_REGPOS_R(i, 16));
		if (BIT15(i))
			DATA(r_15) = &(GETCPUREG_W(15));
		else
			DATA(r_15) = NULL;

		u32 count = 0;
		for (s32 j = 14; j >= 0; j--)
		{
			if (BIT_N(i,j))
			{
				DATA(r[count++]) = &(GETCPUREG_W(j));
			}
		}
		DATA(r[count]) = NULL;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LDMDB2)
		u32 adr = *DATA(r_16);
		u32 oldmode = 0;
		u8 c = 0;

		if (DATA(r_15) == NULL)
		{
			if((DATA(cpu)->CPSR.bits.mode==USR)||(DATA(cpu)->CPSR.bits.mode==SYS))
			{
				printf("ERROR1\n");
				GOTO_NEXTOP(1)
			}
			oldmode = armcpu_switchMode(DATA(cpu), SYS);
		}
		else
		{
			adr -= 4;
			u32 tmp = READ32(DATA(cpu)->mem_if->data, adr);
			*DATA(r_15) = tmp & (0XFFFFFFFC | (BIT0(tmp)<<1));
			DATA(cpu)->CPSR = DATA(cpu)->SPSR;
			DATA(cpu)->changeCPSR();
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
		}
		
		u32 count = 0;
		while (DATA(r[count]))
		{
			adr -= 4;
			*DATA(r[count++]) = READ32(DATA(cpu)->mem_if->data, adr);
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
		}

		if (DATA(r_15) == NULL)
		{
			armcpu_switchMode(DATA(cpu), oldmode);
		}
		else
		{
			Status_Reg SPSR = DATA(cpu)->SPSR;
			armcpu_switchMode(DATA(cpu), SPSR.bits.mode);
			DATA(cpu)->CPSR=SPSR;
			DATA(cpu)->changeCPSR();
		}

		c = MMU_aluMemCycles<PROCNUM>(2, c);

		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_LDMIA2_W)
	armcpu_t *cpu;
	Status_Reg *cpsr; 
	u32 *r_16;
	u32 *r[15+1];
	u32 *r_15;
	bool wb_flg;

	DCL_OP_COMPILER(OP_LDMIA2_W)
		DATA(cpu) = GETCPU;
		DATA(cpsr) = &(GETCPU->CPSR); 
		DATA(r_16) = &(ARM_REGPOS_RW(i, 16));
		if (BIT15(i))
			DATA(r_15) = &(GETCPUREG_W(15));
		else
			DATA(r_15) = NULL;
		DATA(wb_flg) = !BIT_N(i, REG_POS(i,16));

		u32 count = 0;
		for (s32 j = 0; j <= 14; j++)
		{
			if (BIT_N(i,j))
			{
				DATA(r[count++]) = &(GETCPUREG_W(j));
			}
		}
		DATA(r[count]) = NULL;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LDMIA2_W)
		u32 adr = *DATA(r_16);
		u32 oldmode = 0;
		u8 c = 0;

		if (DATA(r_15) == NULL)
		{
			if((DATA(cpu)->CPSR.bits.mode==USR)||(DATA(cpu)->CPSR.bits.mode==SYS))
			{
				printf("ERROR1\n");
				GOTO_NEXTOP(1)
			}
			oldmode = armcpu_switchMode(DATA(cpu), SYS);
		}
		
		u32 count = 0;
		while (DATA(r[count]))
		{
			*DATA(r[count++]) = READ32(DATA(cpu)->mem_if->data, adr);
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
			adr += 4;
		}

		if (DATA(r_15) == NULL)
		{
			if (DATA(wb_flg))
				*DATA(r_16) = adr;
			armcpu_switchMode(DATA(cpu), oldmode);
		}
		else
		{
			if (DATA(wb_flg))
				*DATA(r_16) = adr + 4;
			u32 tmp = READ32(DATA(cpu)->mem_if->data, adr);
			Status_Reg SPSR;
			*DATA(r_15) = tmp & (0XFFFFFFFC | (BIT0(tmp)<<1));
			SPSR = DATA(cpu)->SPSR;
			armcpu_switchMode(DATA(cpu), SPSR.bits.mode);
			DATA(cpu)->CPSR=SPSR;
			DATA(cpu)->changeCPSR();
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
			//adr += 4;
		}

		c = MMU_aluMemCycles<PROCNUM>(2, c);

		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_LDMIB2_W)
	armcpu_t *cpu;
	Status_Reg *cpsr; 
	u32 *r_16;
	u32 *r[15+1];
	u32 *r_15;
	bool wb_flg;

	DCL_OP_COMPILER(OP_LDMIB2_W)
		DATA(cpu) = GETCPU;
		DATA(cpsr) = &(GETCPU->CPSR); 
		DATA(r_16) = &(ARM_REGPOS_RW(i, 16));
		if (BIT15(i))
			DATA(r_15) = &(GETCPUREG_W(15));
		else
			DATA(r_15) = NULL;
		DATA(wb_flg) = !BIT_N(i, REG_POS(i,16));

		u32 count = 0;
		for (s32 j = 0; j <= 14; j++)
		{
			if (BIT_N(i,j))
			{
				DATA(r[count++]) = &(GETCPUREG_W(j));
			}
		}
		DATA(r[count]) = NULL;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LDMIB2_W)
		u32 adr = *DATA(r_16);
		u32 oldmode = 0;
		u8 c = 0;

		if (DATA(r_15) == NULL)
		{
			if((DATA(cpu)->CPSR.bits.mode==USR)||(DATA(cpu)->CPSR.bits.mode==SYS))
			{
				printf("ERROR1\n");
				GOTO_NEXTOP(1)
			}
			oldmode = armcpu_switchMode(DATA(cpu), SYS);
		}
		
		u32 count = 0;
		while (DATA(r[count]))
		{
			adr += 4;
			*DATA(r[count++]) = READ32(DATA(cpu)->mem_if->data, adr);
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
		}

		if (DATA(r_15) == NULL)
		{
			if (DATA(wb_flg))
				*DATA(r_16) = adr;
			armcpu_switchMode(DATA(cpu), oldmode);
		}
		else
		{
			adr += 4;
			if (DATA(wb_flg))
				*DATA(r_16) = adr;
			u32 tmp = READ32(DATA(cpu)->mem_if->data, adr);
			*DATA(r_15) = tmp & (0XFFFFFFFC | (BIT0(tmp)<<1));
			DATA(cpu)->CPSR = DATA(cpu)->SPSR;
			DATA(cpu)->changeCPSR();
			Status_Reg SPSR;
			SPSR = DATA(cpu)->SPSR;
			armcpu_switchMode(DATA(cpu), SPSR.bits.mode);
			DATA(cpu)->CPSR=SPSR;
			DATA(cpu)->changeCPSR();
			
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
		}

		c = MMU_aluMemCycles<PROCNUM>(2, c);

		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_LDMDA2_W)
	armcpu_t *cpu;
	Status_Reg *cpsr; 
	u32 *r_16;
	u32 *r[15+1];
	u32 *r_15;
	bool wb_flg;

	DCL_OP_COMPILER(OP_LDMDA2_W)
		DATA(cpu) = GETCPU;
		DATA(cpsr) = &(GETCPU->CPSR); 
		DATA(r_16) = &(ARM_REGPOS_RW(i, 16));
		if (BIT15(i))
			DATA(r_15) = &(GETCPUREG_W(15));
		else
			DATA(r_15) = NULL;
		DATA(wb_flg) = !BIT_N(i, REG_POS(i,16));

		u32 count = 0;
		for (s32 j = 14; j >= 0; j--)
		{
			if (BIT_N(i,j))
			{
				DATA(r[count++]) = &(GETCPUREG_W(j));
			}
		}
		DATA(r[count]) = NULL;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LDMDA2_W)
		u32 adr = *DATA(r_16);
		u32 oldmode = 0;
		u8 c = 0;

		if (DATA(r_15) == NULL)
		{
			if((DATA(cpu)->CPSR.bits.mode==USR)||(DATA(cpu)->CPSR.bits.mode==SYS))
			{
				printf("ERROR1\n");
				GOTO_NEXTOP(1)
			}
			oldmode = armcpu_switchMode(DATA(cpu), SYS);
		}
		else
		{
			if (!DATA(wb_flg))
				printf("error1_1\n");
			u32 tmp = READ32(DATA(cpu)->mem_if->data, adr);
			*DATA(r_15) = tmp & (0XFFFFFFFC | (BIT0(tmp)<<1));
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
			adr -= 4;
		}
		
		u32 count = 0;
		while (DATA(r[count]))
		{
			*DATA(r[count++]) = READ32(DATA(cpu)->mem_if->data, adr);
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
			adr -= 4;
		}

		if (DATA(wb_flg))
			*DATA(r_16) = adr;

		if (DATA(r_15) == NULL)
		{
			armcpu_switchMode(DATA(cpu), oldmode);
		}
		else
		{
			Status_Reg SPSR = DATA(cpu)->SPSR;
			armcpu_switchMode(DATA(cpu), SPSR.bits.mode);
			DATA(cpu)->CPSR=SPSR;
			DATA(cpu)->changeCPSR();
		}

		c = MMU_aluMemCycles<PROCNUM>(2, c);

		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_LDMDB2_W)
	armcpu_t *cpu;
	Status_Reg *cpsr; 
	u32 *r_16;
	u32 *r[15+1];
	u32 *r_15;
	bool wb_flg;

	DCL_OP_COMPILER(OP_LDMDB2_W)
		DATA(cpu) = GETCPU;
		DATA(cpsr) = &(GETCPU->CPSR); 
		DATA(r_16) = &(ARM_REGPOS_RW(i, 16));
		if (BIT15(i))
			DATA(r_15) = &(GETCPUREG_W(15));
		else
			DATA(r_15) = NULL;
		DATA(wb_flg) = !BIT_N(i, REG_POS(i,16));

		u32 count = 0;
		for (s32 j = 14; j >= 0; j--)
		{
			if (BIT_N(i,j))
			{
				DATA(r[count++]) = &(GETCPUREG_W(j));
			}
		}
		DATA(r[count]) = NULL;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LDMDB2_W)
		u32 adr = *DATA(r_16);
		u32 oldmode = 0;
		u8 c = 0;

		if (DATA(r_15) == NULL)
		{
			if((DATA(cpu)->CPSR.bits.mode==USR)||(DATA(cpu)->CPSR.bits.mode==SYS))
			{
				printf("ERROR1\n");
				GOTO_NEXTOP(1)
			}
			oldmode = armcpu_switchMode(DATA(cpu), SYS);
		}
		else
		{
			if (!DATA(wb_flg))
				printf("error1_2\n");
			adr -= 4;
			u32 tmp = READ32(DATA(cpu)->mem_if->data, adr);
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
			*DATA(r_15) = tmp & (0XFFFFFFFC | (BIT0(tmp)<<1));
			DATA(cpu)->CPSR = DATA(cpu)->SPSR;
			DATA(cpu)->changeCPSR();
		}
		
		u32 count = 0;
		while (DATA(r[count]))
		{
			adr -= 4;
			*DATA(r[count++]) = READ32(DATA(cpu)->mem_if->data, adr);
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
		}

		if (DATA(wb_flg))
			*DATA(r_16) = adr;

		if (DATA(r_15) == NULL)
		{
			armcpu_switchMode(DATA(cpu), oldmode);
		}
		else
		{
			Status_Reg SPSR = DATA(cpu)->SPSR;
			armcpu_switchMode(DATA(cpu), SPSR.bits.mode);
			DATA(cpu)->CPSR=SPSR;
			DATA(cpu)->changeCPSR();
		}

		c = MMU_aluMemCycles<PROCNUM>(2, c);

		GOTO_NEXTOP(c)
	}
};

//-----------------------------------------------------------------------------
//   STMIA / STMIB / STMDA / STMDB
//-----------------------------------------------------------------------------
DCL_OP_START(OP_STMIA)
	armcpu_t *cpu;
	u32 *r_16;
	u32 *r[16+1];

	DCL_OP_COMPILER(OP_STMIA)
		DATA(cpu) = GETCPU;
		DATA(r_16) = &(ARM_REGPOS_R(i, 16));

		u32 count = 0;
		for (u32 j = 0; j < 16; j++)
		{
			if (BIT_N(i,j))
			{
				DATA(r[count++]) = &(GETCPUREG_R(j));
			}
		}
		DATA(r[count]) = NULL;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_STMIA)
		u32 adr = *DATA(r_16);
		u8 c = 0;
		
		u32 count = 0;
		while (DATA(r[count]))
		{
			WRITE32(DATA(cpu)->mem_if->data, adr, *DATA(r[count++]));
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr);
			adr += 4;
		}

		c = MMU_aluMemCycles<PROCNUM>(1, c);

		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_STMIB)
	armcpu_t *cpu;
	u32 *r_16;
	u32 *r[16+1];

	DCL_OP_COMPILER(OP_STMIB)
		DATA(cpu) = GETCPU;
		DATA(r_16) = &(ARM_REGPOS_R(i, 16));

		u32 count = 0;
		for (u32 j = 0; j < 16; j++)
		{
			if (BIT_N(i,j))
			{
				DATA(r[count++]) = &(GETCPUREG_R(j));
			}
		}
		DATA(r[count]) = NULL;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_STMIB)
		u32 adr = *DATA(r_16);
		u8 c = 0;
		
		u32 count = 0;
		while (DATA(r[count]))
		{
			adr += 4;
			WRITE32(DATA(cpu)->mem_if->data, adr, *DATA(r[count++]));
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr);
		}

		c = MMU_aluMemCycles<PROCNUM>(1, c);

		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_STMDA)
	armcpu_t *cpu;
	u32 *r_16;
	u32 *r[16+1];

	DCL_OP_COMPILER(OP_STMDA)
		DATA(cpu) = GETCPU;
		DATA(r_16) = &(ARM_REGPOS_R(i, 16));

		u32 count = 0;
		for (s32 j = 15; j >= 0; j--)
		{
			if (BIT_N(i,j))
			{
				DATA(r[count++]) = &(GETCPUREG_R(j));
			}
		}
		DATA(r[count]) = NULL;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_STMDA)
		u32 adr = *DATA(r_16);
		u8 c = 0;
		
		u32 count = 0;
		while (DATA(r[count]))
		{
			WRITE32(DATA(cpu)->mem_if->data, adr, *DATA(r[count++]));
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr);
			adr -= 4;
		}

		c = MMU_aluMemCycles<PROCNUM>(1, c);

		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_STMDB)
	armcpu_t *cpu;
	u32 *r_16;
	u32 *r[16+1];

	DCL_OP_COMPILER(OP_STMDB)
		DATA(cpu) = GETCPU;
		DATA(r_16) = &(ARM_REGPOS_R(i, 16));

		u32 count = 0;
		for (s32 j = 15; j >= 0; j--)
		{
			if (BIT_N(i,j))
			{
				DATA(r[count++]) = &(GETCPUREG_R(j));
			}
		}
		DATA(r[count]) = NULL;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_STMDB)
		u32 adr = *DATA(r_16);
		u8 c = 0;
		
		u32 count = 0;
		while (DATA(r[count]))
		{
			adr -= 4;
			WRITE32(DATA(cpu)->mem_if->data, adr, *DATA(r[count++]));
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr);
		}

		c = MMU_aluMemCycles<PROCNUM>(1, c);

		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_STMIA_W)
	armcpu_t *cpu;
	u32 *r_16;
	u32 *r[16+1];

	DCL_OP_COMPILER(OP_STMIA_W)
		DATA(cpu) = GETCPU;
		DATA(r_16) = &(ARM_REGPOS_RW(i, 16));

		u32 count = 0;
		for (u32 j = 0; j < 16; j++)
		{
			if (BIT_N(i,j))
			{
				DATA(r[count++]) = &(GETCPUREG_R(j));
			}
		}
		DATA(r[count]) = NULL;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_STMIA_W)
		u32 adr = *DATA(r_16);
		u8 c = 0;
		
		u32 count = 0;
		while (DATA(r[count]))
		{
			WRITE32(DATA(cpu)->mem_if->data, adr, *DATA(r[count++]));
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr);
			adr += 4;
		}

		*DATA(r_16) = adr;

		c = MMU_aluMemCycles<PROCNUM>(1, c);

		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_STMIB_W)
	armcpu_t *cpu;
	u32 *r_16;
	u32 *r[16+1];

	DCL_OP_COMPILER(OP_STMIB_W)
		DATA(cpu) = GETCPU;
		DATA(r_16) = &(ARM_REGPOS_RW(i, 16));

		u32 count = 0;
		for (u32 j = 0; j < 16; j++)
		{
			if (BIT_N(i,j))
			{
				DATA(r[count++]) = &(GETCPUREG_R(j));
			}
		}
		DATA(r[count]) = NULL;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_STMIB_W)
		u32 adr = *DATA(r_16);
		u8 c = 0;
		
		u32 count = 0;
		while (DATA(r[count]))
		{
			adr += 4;
			WRITE32(DATA(cpu)->mem_if->data, adr, *DATA(r[count++]));
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr);
		}

		*DATA(r_16) = adr;

		c = MMU_aluMemCycles<PROCNUM>(1, c);

		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_STMDA_W)
	armcpu_t *cpu;
	u32 *r_16;
	u32 *r[16+1];

	DCL_OP_COMPILER(OP_STMDA_W)
		DATA(cpu) = GETCPU;
		DATA(r_16) = &(ARM_REGPOS_RW(i, 16));

		u32 count = 0;
		for (s32 j = 15; j >= 0; j--)
		{
			if (BIT_N(i,j))
			{
				DATA(r[count++]) = &(GETCPUREG_R(j));
			}
		}
		DATA(r[count]) = NULL;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_STMDA_W)
		u32 adr = *DATA(r_16);
		u8 c = 0;
		
		u32 count = 0;
		while (DATA(r[count]))
		{
			WRITE32(DATA(cpu)->mem_if->data, adr, *DATA(r[count++]));
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr);
			adr -= 4;
		}

		*DATA(r_16) = adr;

		c = MMU_aluMemCycles<PROCNUM>(1, c);

		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_STMDB_W)
	armcpu_t *cpu;
	u32 *r_16;
	u32 *r[16+1];

	DCL_OP_COMPILER(OP_STMDB_W)
		DATA(cpu) = GETCPU;
		DATA(r_16) = &(ARM_REGPOS_RW(i, 16));

		u32 count = 0;
		for (s32 j = 15; j >= 0; j--)
		{
			if (BIT_N(i,j))
			{
				DATA(r[count++]) = &(GETCPUREG_R(j));
			}
		}
		DATA(r[count]) = NULL;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_STMDB_W)
		u32 adr = *DATA(r_16);
		u8 c = 0;
		
		u32 count = 0;
		while (DATA(r[count]))
		{
			adr -= 4;
			WRITE32(DATA(cpu)->mem_if->data, adr, *DATA(r[count++]));
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr);
		}

		*DATA(r_16) = adr;

		c = MMU_aluMemCycles<PROCNUM>(1, c);

		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_STMIA2)
	armcpu_t *cpu;
	Status_Reg *cpsr; 
	u32 *r_16;
	u32 *r[16+1];

	DCL_OP_COMPILER(OP_STMIA2)
		DATA(cpu) = GETCPU;
		DATA(cpsr) = &(GETCPU->CPSR); 
		DATA(r_16) = &(ARM_REGPOS_R(i, 16));

		u32 count = 0;
		for (u32 j = 0; j < 16; j++)
		{
			if (BIT_N(i,j))
			{
				DATA(r[count++]) = &(GETCPUREG_R(j));
			}
		}
		DATA(r[count]) = NULL;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_STMIA2)
		u32 adr = *DATA(r_16);
		u8 c = 0;

		if(DATA(cpsr)->bits.mode==USR)
		{
			GOTO_NEXTOP(2)
		}

		u32 oldmode = armcpu_switchMode(DATA(cpu), SYS);
		
		u32 count = 0;
		while (DATA(r[count]))
		{
			WRITE32(DATA(cpu)->mem_if->data, adr, *DATA(r[count++]));
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr);
			adr += 4;
		}

		armcpu_switchMode(DATA(cpu), oldmode);

		c = MMU_aluMemCycles<PROCNUM>(1, c);

		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_STMIB2)
	armcpu_t *cpu;
	Status_Reg *cpsr; 
	u32 *r_16;
	u32 *r[16+1];

	DCL_OP_COMPILER(OP_STMIB2)
		DATA(cpu) = GETCPU;
		DATA(cpsr) = &(GETCPU->CPSR); 
		DATA(r_16) = &(ARM_REGPOS_R(i, 16));

		u32 count = 0;
		for (u32 j = 0; j < 16; j++)
		{
			if (BIT_N(i,j))
			{
				DATA(r[count++]) = &(GETCPUREG_R(j));
			}
		}
		DATA(r[count]) = NULL;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_STMIB2)
		u32 adr = *DATA(r_16);
		u8 c = 0;

		if(DATA(cpsr)->bits.mode==USR)
		{
			GOTO_NEXTOP(2)
		}

		u32 oldmode = armcpu_switchMode(DATA(cpu), SYS);
		
		u32 count = 0;
		while (DATA(r[count]))
		{
			adr += 4;
			WRITE32(DATA(cpu)->mem_if->data, adr, *DATA(r[count++]));
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr);
		}

		armcpu_switchMode(DATA(cpu), oldmode);

		c = MMU_aluMemCycles<PROCNUM>(1, c);

		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_STMDA2)
	armcpu_t *cpu;
	Status_Reg *cpsr; 
	u32 *r_16;
	u32 *r[16+1];

	DCL_OP_COMPILER(OP_STMDA2)
		DATA(cpu) = GETCPU;
		DATA(cpsr) = &(GETCPU->CPSR); 
		DATA(r_16) = &(ARM_REGPOS_R(i, 16));

		u32 count = 0;
		for (s32 j = 15; j >= 0; j--)
		{
			if (BIT_N(i,j))
			{
				DATA(r[count++]) = &(GETCPUREG_R(j));
			}
		}
		DATA(r[count]) = NULL;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_STMDA2)
		u32 adr = *DATA(r_16);
		u8 c = 0;

		if(DATA(cpsr)->bits.mode==USR)
		{
			GOTO_NEXTOP(2)
		}

		u32 oldmode = armcpu_switchMode(DATA(cpu), SYS);
		
		u32 count = 0;
		while (DATA(r[count]))
		{
			WRITE32(DATA(cpu)->mem_if->data, adr, *DATA(r[count++]));
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr);
			adr -= 4;
		}

		armcpu_switchMode(DATA(cpu), oldmode);

		c = MMU_aluMemCycles<PROCNUM>(1, c);

		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_STMDB2)
	armcpu_t *cpu;
	Status_Reg *cpsr; 
	u32 *r_16;
	u32 *r[16+1];

	DCL_OP_COMPILER(OP_STMDB2)
		DATA(cpu) = GETCPU;
		DATA(cpsr) = &(GETCPU->CPSR); 
		DATA(r_16) = &(ARM_REGPOS_R(i, 16));

		u32 count = 0;
		for (s32 j = 15; j >= 0; j--)
		{
			if (BIT_N(i,j))
			{
				DATA(r[count++]) = &(GETCPUREG_R(j));
			}
		}
		DATA(r[count]) = NULL;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_STMDB2)
		u32 adr = *DATA(r_16);
		u8 c = 0;

		if(DATA(cpsr)->bits.mode==USR)
		{
			GOTO_NEXTOP(2)
		}

		u32 oldmode = armcpu_switchMode(DATA(cpu), SYS);
		
		u32 count = 0;
		while (DATA(r[count]))
		{
			adr -= 4;
			WRITE32(DATA(cpu)->mem_if->data, adr, *DATA(r[count++]));
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr);
		}

		armcpu_switchMode(DATA(cpu), oldmode);

		c = MMU_aluMemCycles<PROCNUM>(1, c);

		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_STMIA2_W)
	armcpu_t *cpu;
	Status_Reg *cpsr; 
	u32 *r_16;
	u32 *r[16+1];

	DCL_OP_COMPILER(OP_STMIA2_W)
		DATA(cpu) = GETCPU;
		DATA(cpsr) = &(GETCPU->CPSR); 
		DATA(r_16) = &(ARM_REGPOS_RW(i, 16));

		u32 count = 0;
		for (u32 j = 0; j < 16; j++)
		{
			if (BIT_N(i,j))
			{
				DATA(r[count++]) = &(GETCPUREG_R(j));
			}
		}
		DATA(r[count]) = NULL;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_STMIA2_W)
		u32 adr = *DATA(r_16);
		u8 c = 0;

		if(DATA(cpsr)->bits.mode==USR)
		{
			GOTO_NEXTOP(2)
		}

		u32 oldmode = armcpu_switchMode(DATA(cpu), SYS);
		
		u32 count = 0;
		while (DATA(r[count]))
		{
			WRITE32(DATA(cpu)->mem_if->data, adr, *DATA(r[count++]));
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr);
			adr += 4;
		}

		*DATA(r_16) = adr;

		armcpu_switchMode(DATA(cpu), oldmode);

		c = MMU_aluMemCycles<PROCNUM>(1, c);

		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_STMIB2_W)
	armcpu_t *cpu;
	Status_Reg *cpsr; 
	u32 *r_16;
	u32 *r[16+1];

	DCL_OP_COMPILER(OP_STMIB2_W)
		DATA(cpu) = GETCPU;
		DATA(cpsr) = &(GETCPU->CPSR); 
		DATA(r_16) = &(ARM_REGPOS_RW(i, 16));

		u32 count = 0;
		for (u32 j = 0; j < 16; j++)
		{
			if (BIT_N(i,j))
			{
				DATA(r[count++]) = &(GETCPUREG_R(j));
			}
		}
		DATA(r[count]) = NULL;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_STMIB2_W)
		u32 adr = *DATA(r_16);
		u8 c = 0;

		if(DATA(cpsr)->bits.mode==USR)
		{
			GOTO_NEXTOP(2)
		}

		u32 oldmode = armcpu_switchMode(DATA(cpu), SYS);
		
		u32 count = 0;
		while (DATA(r[count]))
		{
			adr += 4;
			WRITE32(DATA(cpu)->mem_if->data, adr, *DATA(r[count++]));
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr);
		}

		armcpu_switchMode(DATA(cpu), oldmode);

		*DATA(r_16) = adr;

		c = MMU_aluMemCycles<PROCNUM>(1, c);

		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_STMDA2_W)
	armcpu_t *cpu;
	Status_Reg *cpsr; 
	u32 *r_16;
	u32 *r[16+1];

	DCL_OP_COMPILER(OP_STMDA2_W)
		DATA(cpu) = GETCPU;
		DATA(cpsr) = &(GETCPU->CPSR); 
		DATA(r_16) = &(ARM_REGPOS_RW(i, 16));

		u32 count = 0;
		for (s32 j = 15; j >= 0; j--)
		{
			if (BIT_N(i,j))
			{
				DATA(r[count++]) = &(GETCPUREG_R(j));
			}
		}
		DATA(r[count]) = NULL;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_STMDA2_W)
		u32 adr = *DATA(r_16);
		u8 c = 0;

		if(DATA(cpsr)->bits.mode==USR)
		{
			GOTO_NEXTOP(2)
		}

		u32 oldmode = armcpu_switchMode(DATA(cpu), SYS);
		
		u32 count = 0;
		while (DATA(r[count]))
		{
			WRITE32(DATA(cpu)->mem_if->data, adr, *DATA(r[count++]));
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr);
			adr -= 4;
		}

		*DATA(r_16) = adr;

		armcpu_switchMode(DATA(cpu), oldmode);

		c = MMU_aluMemCycles<PROCNUM>(1, c);

		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_STMDB2_W)
	armcpu_t *cpu;
	Status_Reg *cpsr; 
	u32 *r_16;
	u32 *r[16+1];

	DCL_OP_COMPILER(OP_STMDB2_W)
		DATA(cpu) = GETCPU;
		DATA(cpsr) = &(GETCPU->CPSR); 
		DATA(r_16) = &(ARM_REGPOS_RW(i, 16));

		u32 count = 0;
		for (s32 j = 15; j >= 0; j--)
		{
			if (BIT_N(i,j))
			{
				DATA(r[count++]) = &(GETCPUREG_R(j));
			}
		}
		DATA(r[count]) = NULL;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_STMDB2_W)
		u32 adr = *DATA(r_16);
		u8 c = 0;

		if(DATA(cpsr)->bits.mode==USR)
		{
			GOTO_NEXTOP(2)
		}

		u32 oldmode = armcpu_switchMode(DATA(cpu), SYS);
		
		u32 count = 0;
		while (DATA(r[count]))
		{
			adr -= 4;
			WRITE32(DATA(cpu)->mem_if->data, adr, *DATA(r[count++]));
			c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr);
		}

		*DATA(r_16) = adr;

		armcpu_switchMode(DATA(cpu), oldmode);

		c = MMU_aluMemCycles<PROCNUM>(1, c);

		GOTO_NEXTOP(c)
	}
};

//-----------------------------------------------------------------------------
//   LDRD / STRD
//-----------------------------------------------------------------------------
DCL_OP_START(OP_LDRD_STRD_POST_INDEX)
	armcpu_t *cpu;
	u32 *r_addr;
	u32 *r_off;
	u32 i_off;
	u8 Rd_num;
	bool i_bit;
	bool u_bit;
	bool s_bit;
	bool flg1;

	DCL_OP_COMPILER(OP_LDRD_STRD_POST_INDEX)
		DATA(cpu) = GETCPU;
		DATA(r_addr) = &(ARM_REGPOS_RW(i, 16));
		DATA(r_off) = &(ARM_REGPOS_R(i, 0));
		DATA(i_off) = IMM_OFF;
		DATA(Rd_num) = REG_POS(i, 12);
		DATA(i_bit) = BIT22(i);
		DATA(u_bit) = BIT23(i);
		DATA(s_bit) = BIT5(i);
		DATA(flg1) = !(DATA(Rd_num) & 0x1);
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LDRD_STRD_POST_INDEX)
		u32 addr = *DATA(r_addr);
		u32 index;

		if (DATA(i_bit))
			index = DATA(i_off);
		else
			index = *DATA(r_off);

		if (DATA(u_bit))
			*DATA(r_addr) += index;
		else
			*DATA(r_addr) -= index;

		u8 c = 0;
		u8 Rd_num = DATA(Rd_num);
		if (DATA(flg1))
		{
			if (DATA(s_bit))
			{
				WRITE32(DATA(cpu)->mem_if->data, addr, DATA(cpu)->R[Rd_num]);
				WRITE32(DATA(cpu)->mem_if->data, addr + 4, DATA(cpu)->R[Rd_num + 1]);
				c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(addr);
				c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(addr + 4);
			}
			else
			{
				DATA(cpu)->R[Rd_num] = READ32(DATA(cpu)->mem_if->data, addr);
				DATA(cpu)->R[Rd_num + 1] = READ32(DATA(cpu)->mem_if->data, addr + 4);
				c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(addr);
				c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(addr + 4);
			}
		}

		c = MMU_aluMemCycles<PROCNUM>(3, c);

		GOTO_NEXTOP(c)
	}
};

DCL_OP_START(OP_LDRD_STRD_OFFSET_PRE_INDEX)
	armcpu_t *cpu;
	u32 *r_addr;
	u32 *r_off;
	u32 i_off;
	u8 Rd_num;
	bool i_bit;
	bool u_bit;
	bool s_bit;
	bool w_bit;
	bool flg1;

	DCL_OP_COMPILER(OP_LDRD_STRD_OFFSET_PRE_INDEX)
		DATA(cpu) = GETCPU;
		DATA(r_addr) = &(ARM_REGPOS_RW(i, 16));
		DATA(r_off) = &(ARM_REGPOS_R(i, 0));
		DATA(i_off) = IMM_OFF;
		DATA(Rd_num) = REG_POS(i, 12);
		DATA(i_bit) = BIT22(i);
		DATA(u_bit) = BIT23(i);
		DATA(s_bit) = BIT5(i);
		DATA(w_bit) = BIT21(i);
		DATA(flg1) = !(DATA(Rd_num) & 0x1);
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_LDRD_STRD_OFFSET_PRE_INDEX)
		u32 addr = *DATA(r_addr);
		u32 index;

		if (DATA(i_bit))
			index = DATA(i_off);
		else
			index = *DATA(r_off);

		if (DATA(u_bit))
			addr += index;
		else
			addr -= index;

		u8 c = 0;
		u8 Rd_num = DATA(Rd_num);
		if (DATA(flg1))
		{
			if (DATA(s_bit))
			{
				WRITE32(DATA(cpu)->mem_if->data, addr, DATA(cpu)->R[Rd_num]);
				WRITE32(DATA(cpu)->mem_if->data, addr + 4, DATA(cpu)->R[Rd_num + 1]);
				c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(addr);
				c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(addr + 4);
				if (DATA(w_bit))
					*DATA(r_addr) = addr;
			}
			else
			{
				if (DATA(w_bit))
					*DATA(r_addr) = addr;
				DATA(cpu)->R[Rd_num] = READ32(DATA(cpu)->mem_if->data, addr);
				DATA(cpu)->R[Rd_num + 1] = READ32(DATA(cpu)->mem_if->data, addr + 4);
				c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(addr);
				c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(addr + 4);
			}
		}

		c = MMU_aluMemCycles<PROCNUM>(3, c);

		GOTO_NEXTOP(c)
	}
};

//-----------------------------------------------------------------------------
//   STC
//   the NDS has no coproc that responses to a STC, no feedback is given to the arm
//-----------------------------------------------------------------------------
#define DCL_UNDEF_OP(name) \
	DCL_OP_START(name) \
		armcpu_t *cpu; \
		DCL_OP_COMPILER(name) \
			DATA(cpu) = GETCPU; \
			DONE_COMPILER \
		} \
		DCL_OP_METHOD(name) \
			TRAPUNDEF(DATA(cpu)); \
			GOTO_NEXTOP(1) \
		} \
	};

DCL_UNDEF_OP(OP_STC_P_IMM_OFF)
DCL_UNDEF_OP(OP_STC_M_IMM_OFF)
DCL_UNDEF_OP(OP_STC_P_PREIND)
DCL_UNDEF_OP(OP_STC_M_PREIND)
DCL_UNDEF_OP(OP_STC_P_POSTIND)
DCL_UNDEF_OP(OP_STC_M_POSTIND)
DCL_UNDEF_OP(OP_STC_OPTION)

//-----------------------------------------------------------------------------
//   LDC
//   the NDS has no coproc that responses to a LDC, no feedback is given to the arm
//-----------------------------------------------------------------------------
DCL_UNDEF_OP(OP_LDC_P_IMM_OFF)
DCL_UNDEF_OP(OP_LDC_M_IMM_OFF)
DCL_UNDEF_OP(OP_LDC_P_PREIND)
DCL_UNDEF_OP(OP_LDC_M_PREIND)
DCL_UNDEF_OP(OP_LDC_P_POSTIND)
DCL_UNDEF_OP(OP_LDC_M_POSTIND)
DCL_UNDEF_OP(OP_LDC_OPTION)

//-----------------------------------------------------------------------------
//   MCR / MRC
//-----------------------------------------------------------------------------
DCL_OP_START(OP_MCR)
	u32 *r_12;
	u8 cpnum;
	u8 crn;
	u8 crm;
	u8 op1;
	u8 op2;

	DCL_OP_COMPILER(OP_MCR)
		DATA(r_12) = &(ARM_REGPOS_R(i, 12));
		DATA(cpnum) = REG_POS(i, 8);
		DATA(crn) = REG_POS(i, 16);
		DATA(crm) = REG_POS(i, 0);
		DATA(op1) = (i>>21)&0x7;
		DATA(op2) = (i>>5)&0x7;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_MCR)
		if(DATA(cpnum) != 15)
		{
			GOTO_NEXTOP(2)
		}

		cp15.moveARM2CP(*DATA(r_12), DATA(crn), DATA(crm), DATA(op1), DATA(op2));

		GOTO_NEXTOP(2)
	}
};

DCL_OP_START(OP_MRC)
	Status_Reg *cpsr; 
	u32 *r_12;
	bool mod_r15;
	u8 cpnum;
	u8 crn;
	u8 crm;
	u8 op1;
	u8 op2;

	DCL_OP_COMPILER(OP_MRC)
		DATA(cpsr) = &(GETCPU->CPSR); 
		DATA(r_12) = &(ARM_REGPOS_RW(i, 12));
		DATA(mod_r15) = REG_POS(i, 12)==15;
		DATA(cpnum) = REG_POS(i, 8);
		DATA(crn) = REG_POS(i, 16);
		DATA(crm) = REG_POS(i, 0);
		DATA(op1) = (i>>21)&0x7;
		DATA(op2) = (i>>5)&0x7;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_MRC)
		if(DATA(cpnum) != 15)
		{
			GOTO_NEXTOP(2)
		}

		u32 data = 0;
		cp15.moveCP2ARM(&data, DATA(crn), DATA(crm), DATA(op1), DATA(op2));
		if (DATA(mod_r15))
		{
			DATA(cpsr)->bits.N = BIT31(data);
			DATA(cpsr)->bits.Z = BIT30(data);
			DATA(cpsr)->bits.C = BIT29(data);
			DATA(cpsr)->bits.V = BIT28(data);
		}
		else
		{
			*DATA(r_12) = data;
		}

		GOTO_NEXTOP(4)
	}
};

//-----------------------------------------------------------------------------
//   SWI
//-----------------------------------------------------------------------------
DCL_OP_START(OP_SWI)
	armcpu_t *cpu;
	u32 swinum;

	DCL_OP_COMPILER(OP_SWI)
		DATA(cpu) = GETCPU;
		DATA(swinum) = (i>>16)&0xFF;

		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_SWI)
		bool bypassBuiltinSWI = (DATA(cpu)->intVector == 0x00000000 && PROCNUM==0) 
								|| (DATA(cpu)->intVector == 0xFFFF0000 && PROCNUM==1);

		if(DATA(cpu)->swi_tab && !bypassBuiltinSWI)
		{
			u8 c = DATA(cpu)->swi_tab[DATA(swinum) & 0x1F]() + 3;

			GOTO_NEXTOP(c)
		}
		else
		{
			Status_Reg tmp = DATA(cpu)->CPSR;
			armcpu_switchMode(DATA(cpu), SVC);
			DATA(cpu)->R[14] = common->R15 - 4;
			DATA(cpu)->SPSR = tmp;
			DATA(cpu)->CPSR.bits.T = 0;
			DATA(cpu)->CPSR.bits.I = 1;
			DATA(cpu)->changeCPSR();
			DATA(cpu)->R[15] = DATA(cpu)->intVector + 0x08;

			GOTO_NEXTOP(3)
		}
	}
};

//-----------------------------------------------------------------------------
//   BKPT
//-----------------------------------------------------------------------------
DCL_OP_START(OP_BKPT)
	armcpu_t *cpu;
	
	DCL_OP_COMPILER(OP_BKPT)
		DATA(cpu) = GETCPU;
		
		DONE_COMPILER
	}

	DCL_OP_METHOD(OP_BKPT)
		printf("THUMB%c: Unimplemented opcode BKPT\n", PROCNUM?'7':'9');
		
		GOTO_NEXTOP(4)
	}
};

//-----------------------------------------------------------------------------
//   CDP
//-----------------------------------------------------------------------------
DCL_UNDEF_OP(OP_CDP);

//-----------------------------------------------------------------------------
//   Dispatch table
//-----------------------------------------------------------------------------
static const OpCompiler thumb_compiler_set[2][1024] = {{
#define TABDECL(x) x<0>::Compiler
#include "thumb_tabdef.inc"
#undef TABDECL
},{
#define TABDECL(x) x<1>::Compiler
#include "thumb_tabdef.inc"
#undef TABDECL
}};

static const OpCompiler arm_compiler_set[2][4096] = {{
#define TABDECL(x) x<0>::Compiler
#include "instruction_tabdef.inc"
#undef TABDECL
},{
#define TABDECL(x) x<1>::Compiler
#include "instruction_tabdef.inc"
#undef TABDECL
}};

//-----------------------------------------------------------------------------
//   Generic instruction wrapper
//-----------------------------------------------------------------------------
template<int PROCNUM, int thumb>
static void FASTCALL Method_OPDECODE(const MethodCommon* common, u8* cycle)
{
	u32 c;
	u32 adr = ARMPROC.instruct_adr;

	if(thumb)
	{
		ARMPROC.next_instruction = adr + 2;
		ARMPROC.R[15] = adr + 4;
		u32 opcode = _MMU_read16<PROCNUM, MMU_AT_CODE>(adr);
		c = thumb_instructions_set[PROCNUM][opcode>>6](opcode);
	}
	else
	{
		ARMPROC.next_instruction = adr + 4;
		ARMPROC.R[15] = adr + 8;
		u32 opcode = _MMU_read32<PROCNUM, MMU_AT_CODE>(adr);
		if(CONDITION(opcode) == 0xE || TEST_COND(CONDITION(opcode), CODE(opcode), ARMPROC.CPSR))
			c = arm_instructions_set[PROCNUM][INSTRUCTION_INDEX(opcode)](opcode);
		else
			c = 1;
	}

	ARMPROC.instruct_adr = ARMPROC.next_instruction;
	*cycle += c;

	return;
}

static MethodCommon s_OpDecode[2][2] =	{
											{{Method_OPDECODE<0,0>, NULL, 0},{Method_OPDECODE<0,1>, NULL, 0},},
											{{Method_OPDECODE<1,0>, NULL, 0},{Method_OPDECODE<1,1>, NULL, 0},},
										};

static Block s_OpDecodeBlock[2][2] =	{
											{{&s_OpDecode[0][0], 0},{&s_OpDecode[0][1], 0},},
											{{&s_OpDecode[1][0], 0},{&s_OpDecode[1][1], 0},},
										};

struct OP_WRAPPER
{
	u32 adr;
	u32 opcode;
	OpFunc fun;

	TEMPLATE static void FASTCALL Compiler(const Decoded &i, MethodCommon* common)
	{
		OP_WRAPPER *pData = (OP_WRAPPER*)AllocCacheAlign32(sizeof(OP_WRAPPER));
		common->data = pData;

		DATA(adr) = i.Address;
		if (i.ThumbFlag)
		{
			DATA(opcode) = i.Instruction.ThumbOp;
			DATA(fun) = thumb_instructions_set[PROCNUM][i.Instruction.ThumbOp>>6];
			common->func = OP_WRAPPER::MethodThumb<PROCNUM>;
		}
		else
		{
			DATA(opcode) = i.Instruction.ArmOp;
			DATA(fun) = arm_instructions_set[PROCNUM][INSTRUCTION_INDEX(i.Instruction.ArmOp)];
			common->func = OP_WRAPPER::MethodArm<PROCNUM>;
		}
	}

	TEMPLATE static void FASTCALL MethodArm(const MethodCommon* common, u8* cycle)
	{
		OP_WRAPPER *pData = (OP_WRAPPER*)common->data;

		u32 c;
		u32 opcode = DATA(opcode);

		ARMPROC.next_instruction = DATA(adr) + 4;
		ARMPROC.R[15] = common->R15;

		//if(CONDITION(opcode) == 0xE || TEST_COND(CONDITION(opcode), CODE(opcode), ARMPROC.CPSR))
			c = DATA(fun)(opcode);
		//else
		//	c = 1;

		//ARMPROC.instruct_adr = ARMPROC.next_instruction;

		GOTO_NEXTOP(c)
	}

	TEMPLATE static void FASTCALL MethodThumb(const MethodCommon* common, u8* cycle)
	{
		OP_WRAPPER *pData = (OP_WRAPPER*)common->data;

		u32 c;
		u32 opcode = DATA(opcode);

		ARMPROC.next_instruction = DATA(adr) + 2;
		ARMPROC.R[15] = common->R15;

		c = DATA(fun)(opcode);

		//ARMPROC.instruct_adr = ARMPROC.next_instruction;

		GOTO_NEXTOP(c)
	}
};

//-----------------------------------------------------------------------------
//   Block method
//-----------------------------------------------------------------------------
struct OP_SyncR15AfterSWI
{
	armcpu_t *cpu;

	TEMPLATE static void FASTCALL Compiler(const u32 i, MethodCommon* common)
	{
		OP_SyncR15AfterSWI *pData = (OP_SyncR15AfterSWI*)AllocCacheAlign32(sizeof(OP_SyncR15AfterSWI));
		common->func = OP_SyncR15AfterSWI::Method<PROCNUM>;
		common->data = pData;

		DATA(cpu) = GETCPU;
	}

	TEMPLATE static void FASTCALL Method(const MethodCommon* common, u8* cycle)
	{
		OP_SyncR15AfterSWI *pData = (OP_SyncR15AfterSWI*)common->data;

		bool bypassBuiltinSWI = 
				(DATA(cpu)->intVector == 0x00000000 && PROCNUM==0) || 
				(DATA(cpu)->intVector == 0xFFFF0000 && PROCNUM==1);

		if (!DATA(cpu)->swi_tab || bypassBuiltinSWI)
		{
			DATA(cpu)->instruct_adr = DATA(cpu)->R[15];

			return;
		}

		GOTO_NEXTOP(0)
	}
};

struct OP_SyncR15Before
{
	armcpu_t *cpu;

	TEMPLATE static void FASTCALL Compiler(const u32 i, MethodCommon* common)
	{
		OP_SyncR15Before *pData = (OP_SyncR15Before*)AllocCacheAlign32(sizeof(OP_SyncR15Before));
		common->func = OP_SyncR15Before::Method;
		common->data = pData;

		DATA(cpu) = GETCPU;
	}

	static void FASTCALL Method(const MethodCommon* common, u8* cycle)
	{
		OP_SyncR15Before *pData = (OP_SyncR15Before*)common->data;

		common++;
		DATA(cpu)->R[15] = common->R15;

		return common->func(common, cycle);
	}
};

struct OP_SyncR15After
{
	armcpu_t *cpu;

	TEMPLATE static void FASTCALL Compiler(const u32 i, MethodCommon* common)
	{
		OP_SyncR15After *pData = (OP_SyncR15After*)AllocCacheAlign32(sizeof(OP_SyncR15After));
		common->func = OP_SyncR15After::Method;
		common->data = pData;

		DATA(cpu) = GETCPU;
	}

	static void FASTCALL Method(const MethodCommon* common, u8* cycle)
	{
		OP_SyncR15After *pData = (OP_SyncR15After*)common->data;

		DATA(cpu)->instruct_adr = DATA(cpu)->R[15];

		return;
	}
};

struct OP_StopExecute
{
	armcpu_t *cpu;
	u32 nextinsadr;

	TEMPLATE static void FASTCALL Compiler(const u32 i, MethodCommon* common)
	{
		OP_StopExecute *pData = (OP_StopExecute*)AllocCacheAlign32(sizeof(OP_StopExecute));
		common->func = OP_StopExecute::Method;
		common->data = pData;

		DATA(cpu) = GETCPU;
		DATA(nextinsadr) = i;
	}

	static void FASTCALL Method(const MethodCommon* common, u8* cycle)
	{
		OP_StopExecute *pData = (OP_StopExecute*)common->data;

		DATA(cpu)->instruct_adr = DATA(nextinsadr);

		return;
	}
};

struct Cond_SubBlockStart
{
	u32 cond;
	armcpu_t *cpu;
	MethodCommon* target;
	u8 instructions;

	TEMPLATE static Cond_SubBlockStart* FASTCALL Compiler(const u32 cond, MethodCommon* common)
	{
		Cond_SubBlockStart *pData = (Cond_SubBlockStart*)AllocCacheAlign32(sizeof(Cond_SubBlockStart));
		common->func = Cond_SubBlockStart::Method;
		common->data = pData;

		DATA(cond) = cond;
		DATA(cpu) = GETCPU;

		return pData;
	}

	static void FASTCALL Method(const MethodCommon* common, u8* cycle)
	{
		Cond_SubBlockStart *pData = (Cond_SubBlockStart*)common->data;

		if(TEST_COND(DATA(cond), 0, DATA(cpu)->CPSR))
			common++;
		else
		{
			common = DATA(target);
			*cycle += DATA(instructions);
		}

		return common->func(common, cycle);
	}
};

////////////////////////////////////////////////////////////////////
static u32 s_CacheReserve = 16 * 1024 * 1024;
static u32 s_ReserveBufferUsed = 0;
static u8* s_ReserveBuffer = NULL;

static void ReleaseCache()
{
	if (s_ReserveBuffer)
	{
		delete [] s_ReserveBuffer;
		s_ReserveBuffer = NULL;
	}
	s_ReserveBufferUsed = 0;
}

static void InitializeCache()
{
	ReleaseCache();

	s_ReserveBuffer = new u8[s_CacheReserve];
	memset(s_ReserveBuffer, 0xFD, s_CacheReserve * sizeof(u8));
	s_ReserveBufferUsed = 0;
}

static void ResetCache()
{
	memset(s_ReserveBuffer, 0xFD, s_CacheReserve * sizeof(u8));
	s_ReserveBufferUsed = 0;
}

static void* AllocCache(u32 size)
{
	if (s_ReserveBufferUsed + size >= s_CacheReserve)
		return NULL;

	void *ptr = &s_ReserveBuffer[s_ReserveBufferUsed];
	s_ReserveBufferUsed += size;

	return ptr;
}

static void* AllocCacheAlign32(u32 size)
{
	static const u32 align = 32 - 1;

	u32 size_new = size + align;

	uintptr_t ptr = (uintptr_t)AllocCache(size_new);
	if (ptr == 0)
		return NULL;

	uintptr_t retptr = (ptr + align) & ~align;

	return (uintptr_t*)retptr;
}

static u32 GetCacheRemain()
{
	return s_CacheReserve - s_ReserveBufferUsed;
}

////////////////////////////////////////////////////////////////////
static ArmAnalyze *s_pArmAnalyze = NULL;

TEMPLATE static Block* armcpu_compile()
{
#define DO_FB_BLOCK \
	Block *block = &s_OpDecodeBlock[PROCNUM][ARMPROC.CPSR.bits.T];\
	JITLUT_HANDLE(adr, PROCNUM) = (uintptr_t)block;\
	return block;

#define ALLOC_METHOD(name) \
	name = &block->ops[n++];\
	name->R15 = CalcR15;

	u32 adr = ARMPROC.instruct_adr;

	if (!JITLUT_MAPPED(adr & 0x0FFFFFFF, PROCNUM))
	{
		INFO("JIT: use unmapped memory address %08X\n", adr);
		execute = false;
		return NULL;
	}

	if (!JitBlockModify(adr))
	{
		INFO("hot modify %x %d !!!.\n", adr, PROCNUM);

		DO_FB_BLOCK
	}

	if (GetCacheRemain() < 1 * 128 * 1024)
	{
		INFO("cache full, reset cpu[%d].\n", PROCNUM);

		arm_threadedinterpreter.Reset();
	}

	const s32 MaxInstructionsNum = 100;
	Decoded Instructions[MaxInstructionsNum];

	s32 InstructionsNum = s_pArmAnalyze->Decode(GETCPU, Instructions, MaxInstructionsNum);
	if (InstructionsNum <= 0)
	{
		DO_FB_BLOCK
	}
	InstructionsNum = s_pArmAnalyze->Optimize(Instructions, InstructionsNum);

	u32 R15Num = s_pArmAnalyze->OptimizeFlag(Instructions, InstructionsNum);
	s32 SubBlocks = s_pArmAnalyze->CreateSubBlocks(Instructions, InstructionsNum);

	Block *block = (Block*)AllocCacheAlign32(sizeof(Block));
	JITLUT_HANDLE(adr, PROCNUM) = (uintptr_t)block;

	u32 MethodCount = InstructionsNum + R15Num * 2 + SubBlocks + 1/* StopExecute */;
	block->ops = (MethodCommon*)AllocCacheAlign32(sizeof(MethodCommon) * MethodCount);

	Cond_SubBlockStart *pSubBlockStart = NULL;
	u32 CurSubBlock = INVALID_SUBBLOCK;
	u32 CurInstructions = 0;

	u32 n = 0;
	for (s32 i = 0; i < InstructionsNum; i++)
	{
		Decoded &Inst = Instructions[i];

		u32 CalcR15 = Inst.CalcR15(Inst);
		bool link = false;

		if (Inst.IROp == IR_NOP)
			continue;

		MethodCommon *pCond_SubBlockStart = NULL;
		if (CurSubBlock != Inst.SubBlock)
		{
			if (pSubBlockStart)
				pSubBlockStart->instructions = CurInstructions;

			CurInstructions = 0;

			if (Inst.Cond != 0xE && Inst.Cond != 0xF)
			{
				ALLOC_METHOD(pCond_SubBlockStart)

				if (pSubBlockStart)
					pSubBlockStart->target = pCond_SubBlockStart;

				pSubBlockStart = Cond_SubBlockStart::Compiler<PROCNUM>(Inst.Cond, pCond_SubBlockStart);
			}
			else
				link = true;

			CurSubBlock = Inst.SubBlock;
		}

		MethodCommon *pSynR15Before = NULL;
		if (Inst.R15Modified && Inst.R15Used)
		{
			ALLOC_METHOD(pSynR15Before)

			OP_SyncR15Before::Compiler<PROCNUM>(Inst.ThumbFlag ? Inst.Instruction.ThumbOp : Inst.Instruction.ArmOp, pSynR15Before);
		}

		MethodCommon *pMethod;
		ALLOC_METHOD(pMethod)

		if (Inst.ThumbFlag)
		{
			//if ((Inst.IROp >= IR_NOP && Inst.IROp <= IR_NOP))
			//	OP_WRAPPER::Compiler<PROCNUM>(Inst, pMethod);
			//else
				thumb_compiler_set[Inst.ProcessID][Inst.Instruction.ThumbOp>>6](Inst.Instruction.ThumbOp, pMethod);
		}
		else
		{
			//if ((Inst.IROp >= IR_NOP && Inst.IROp <= IR_NOP))
			//	OP_WRAPPER::Compiler<PROCNUM>(Inst, pMethod);
			//else
				arm_compiler_set[Inst.ProcessID][INSTRUCTION_INDEX(Inst.Instruction.ArmOp)](Inst.Instruction.ArmOp, pMethod);
		}

		CurInstructions++;

		if (Inst.R15Modified)
		{
			MethodCommon *pSynR15After;
			ALLOC_METHOD(pSynR15After)

			if (Inst.IROp == IR_SWI)
				OP_SyncR15AfterSWI::Compiler<PROCNUM>(Inst.ThumbFlag ? Inst.Instruction.ThumbOp : Inst.Instruction.ArmOp, pSynR15After);
			else
				OP_SyncR15After::Compiler<PROCNUM>(Inst.ThumbFlag ? Inst.Instruction.ThumbOp : Inst.Instruction.ArmOp, pSynR15After);
		}

		if (link && pSubBlockStart)
		{
			pSubBlockStart->target = pSynR15Before ? pSynR15Before : pMethod;
			pSubBlockStart = NULL;
		}
	}

	Decoded &LastIns = Instructions[InstructionsNum - 1];
	u32 CalcR15 = LastIns.CalcR15(LastIns);

	MethodCommon *pEndBlock;
	ALLOC_METHOD(pEndBlock)

	OP_StopExecute::Compiler<PROCNUM>(LastIns.Address + (LastIns.ThumbFlag ? 2 : 4), pEndBlock);
	if (pSubBlockStart)
	{
		pSubBlockStart->target = pEndBlock;
		pSubBlockStart->instructions = CurInstructions;
		pSubBlockStart = NULL;
	}

	if (n > MethodCount)
		INFO("method over !!!.\n");

	return block;

#undef ALLOC_METHOD
#undef DO_FB_BLOCK
}

////////////////////////////////////////////////////////////////////
static void cpuReserve()
{
	InitializeCache();

	s_pArmAnalyze = new ArmAnalyze();
	s_pArmAnalyze->Initialize();

	s_pArmAnalyze->m_MergeSubBlocks = true;
}

static void cpuShutdown()
{
	ReleaseCache();

	JitLutReset();

	delete s_pArmAnalyze;
	s_pArmAnalyze = NULL;
}

static void cpuReset()
{
	ResetCache();

	JitLutReset();
}

static void cpuSync()
{
	armcpu_sync();
}

TEMPLATE static void cpuClear(u32 Addr, u32 Size)
{
	JITLUT_HANDLE(Addr, PROCNUM) = (uintptr_t)NULL;
}

TEMPLATE static u32 cpuExecute()
{
	Block *block = (Block*)JITLUT_HANDLE(ARMPROC.instruct_adr, PROCNUM);
	if (!block)
		block = armcpu_compile<PROCNUM>();

	block->cycles = 0;
	block->ops->func(block->ops, &block->cycles);

	return block->cycles;
}

static u32 cpuGetCacheReserve()
{
	return s_CacheReserve / 1024 /1024;
}

static void cpuSetCacheReserve(u32 reserveInMegs)
{
	s_CacheReserve = reserveInMegs * 1024 * 1024;
}

static const char* cpuDescription()
{
	return "Arm Threaded Interpreter";
}

CpuBase arm_threadedinterpreter =
{
	cpuReserve,

	cpuShutdown,

	cpuReset,

	cpuSync,

	cpuClear<0>, cpuClear<1>,

	cpuExecute<0>, cpuExecute<1>,

	cpuGetCacheReserve,
	cpuSetCacheReserve,

	cpuDescription
};
