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
#include "ArmCJit.h"
#include "ArmAnalyze.h"
#include "instructions.h"
#include "Disassembler.h"

#include "armcpu.h"
#include "MMU.h"
#include "MMU_timing.h"
#include "JitBase.h"
#include "utils/MemBuffer.h"
#include "utils/tinycc/libtcc.h"

#ifdef HAVE_JIT

//#define USE_INTERPRETER_FIRST

#define GETCPUPTR (&ARMPROC)
#define GETCPU (ARMPROC)

#define REG_R(i)	(i)==15?"_C":"",(i)==15?(u32*)(d.CalcR15(d)&d.ReadPCMask):&(GETCPU.R[(i)])
#define REG_W(i)	(&(GETCPU.R[(i)]))
#define REG(i)		(&(GETCPU.R[(i)]))
#define REGPTR(i)	(&(GETCPU.R[(i)]))

#define TEMPLATE template<u32 PROCNUM> 
#define OPCDECODER_DECL(name) void FASTCALL name##_CDecoder(const Decoded &d, char *&szCodeBuffer)
#define WRITE_CODE(...) szCodeBuffer += sprintf(szCodeBuffer, __VA_ARGS__)

typedef void (FASTCALL* IROpCDecoder)(const Decoded &d, char *&szCodeBuffer);

typedef u32 (FASTCALL* Interpreter)(const Decoded &d);

typedef u32 (FASTCALL* MemOp1)(u32, u32*);
typedef u32 (FASTCALL* MemOp2)(u32, u32);
typedef u32 (* MemOp3)(u32, u32, u32*);
typedef u32 (* MemOp4)(u32, u32*, u32);

// (*(u32*)0x11)
// ((u32 (FASTCALL *)(u32,u32))0x11)(1,1);

// #define REG_R(p)		(*(u32*)p)
// #define REG_SR(p)	(*(s32*)p)
// #define REG_R_C(p)	((u32)p)
// #define REG_SR_C(p)	((s32)p)
// #define REG_W(p)		(*(u32*)p)
// #define REG(p)		(*(u32*)p)
// #define REGPTR(p)	((u32*)p)

namespace ArmCJit
{
//------------------------------------------------------------
//                         Memory type
//------------------------------------------------------------
	enum {
		MEMTYPE_GENERIC = 0,	// no assumptions
		MEMTYPE_MAIN = 1,		// arm9:r/w arm7:r/w
		MEMTYPE_DTCM_ARM9 = 2,	// arm9:r/w
		MEMTYPE_ERAM_ARM7 = 3,	// arm7:r/w
		MEMTYPE_SWIRAM = 4,		// arm9:r/w arm7:r/w

		MEMTYPE_COUNT,
	};

	u32 GuessAddressArea(u32 PROCNUM, u32 adr)
	{
		if(PROCNUM==ARMCPU_ARM9 && (adr & ~0x3FFF) == MMU.DTCMRegion)
			return MEMTYPE_DTCM_ARM9;
		else if((adr & 0x0F000000) == 0x02000000)
			return MEMTYPE_MAIN;
		else if(PROCNUM==ARMCPU_ARM7 && (adr & 0xFF800000) == 0x03800000)
			return MEMTYPE_ERAM_ARM7;
		else if((adr & 0xFF800000) == 0x03000000)
			return MEMTYPE_SWIRAM;
		else
			return MEMTYPE_GENERIC;
	}

	u32 GuessAddressArea(u32 PROCNUM, u32 adr_s, u32 adr_e)
	{
		u32 mt_s = GuessAddressArea(PROCNUM, adr_s);
		u32 mt_e = GuessAddressArea(PROCNUM, adr_e);

		if (mt_s != mt_e)
			return MEMTYPE_GENERIC;

		return mt_s;
	}

//------------------------------------------------------------
//                         Help function
//------------------------------------------------------------
	void FASTCALL IRShiftOpGenerate(const Decoded &d, char *&szCodeBuffer, bool clacCarry)
	{
		u32 PROCNUM = d.ProcessID;

		switch (d.Typ)
		{
		case IRSHIFT_LSL:
			if (!d.R)
			{
				if (clacCarry)
				{
					if (d.Immediate == 0)
						WRITE_CODE("u32 c = ((Status_Reg*)0x%p)->bits.C;\n", &(GETCPU.CPSR));
					else
						WRITE_CODE("u32 c = BIT_N(REG_R%s(0x%p), %u);\n", REG_R(d.Rm), 32-d.Immediate);
				}

				if (d.Immediate == 0)
					WRITE_CODE("u32 shift_op = REG_R%s(0x%p);\n", REG_R(d.Rm));
				else
					WRITE_CODE("u32 shift_op = REG_R%s(0x%p)<<%u;\n", REG_R(d.Rm), d.Immediate);
			}
			else
			{
				if (clacCarry)
				{
					WRITE_CODE("u32 c;\n");
					WRITE_CODE("u32 shift_op = REG_R%s(0x%p)&0xFF;\n", REG_R(d.Rs));
					WRITE_CODE("if (shift_op == 0){\n");
					WRITE_CODE("c=((Status_Reg*)0x%p)->bits.C;\n", &(GETCPU.CPSR));
					WRITE_CODE("shift_op=REG_R%s(0x%p);\n", REG_R(d.Rm));
					WRITE_CODE("}else if (shift_op < 32){\n");
					WRITE_CODE("c = BIT_N(REG_R%s(0x%p), 32-shift_op);\n", REG_R(d.Rm));
					WRITE_CODE("shift_op = REG_R%s(0x%p)<<shift_op;\n", REG_R(d.Rm));
					WRITE_CODE("}else if (shift_op == 32){\n");
					WRITE_CODE("c = BIT0(REG_R%s(0x%p));\n", REG_R(d.Rm));
					WRITE_CODE("shift_op=0;\n");
					WRITE_CODE("}else{\n");
					WRITE_CODE("shift_op=c=0;}\n");
				}
				else
				{
					WRITE_CODE("u32 shift_op = REG_R%s(0x%p)&0xFF;\n", REG_R(d.Rs));
					WRITE_CODE("if (shift_op >= 32)\n");
					WRITE_CODE("shift_op=0;\n");
					WRITE_CODE("else\n");
					WRITE_CODE("shift_op=REG_R%s(0x%p)<<shift_op;\n", REG_R(d.Rm));
				}
			}
			break;
		case IRSHIFT_LSR:
			if (!d.R)
			{
				if (clacCarry)
				{
					if (d.Immediate == 0)
						WRITE_CODE("u32 c = BIT31(REG_R%s(0x%p));\n", REG_R(d.Rm));
					else
						WRITE_CODE("u32 c = BIT_N(REG_R%s(0x%p), %u);\n", REG_R(d.Rm), d.Immediate-1);
				}

				if (d.Immediate == 0)
					WRITE_CODE("u32 shift_op = 0;\n");
				else
					WRITE_CODE("u32 shift_op = REG_R%s(0x%p)>>%u;\n", REG_R(d.Rm), d.Immediate);
			}
			else
			{
				if (clacCarry)
				{
					WRITE_CODE("u32 c;\n");
					WRITE_CODE("u32 shift_op = REG_R%s(0x%p)&0xFF;\n", REG_R(d.Rs));
					WRITE_CODE("if (shift_op == 0){\n");
					WRITE_CODE("c=((Status_Reg*)0x%p)->bits.C;\n", &(GETCPU.CPSR));
					WRITE_CODE("shift_op=REG_R%s(0x%p);\n", REG_R(d.Rm));
					WRITE_CODE("}else if (shift_op < 32){\n");
					WRITE_CODE("c = BIT_N(REG_R%s(0x%p), shift_op-1);\n", REG_R(d.Rm));
					WRITE_CODE("shift_op = REG_R%s(0x%p)>>shift_op;\n", REG_R(d.Rm));
					WRITE_CODE("}else if (shift_op == 32){\n");
					WRITE_CODE("c = BIT31(REG_R%s(0x%p));\n", REG_R(d.Rm));
					WRITE_CODE("shift_op=0;\n");
					WRITE_CODE("}else{\n");
					WRITE_CODE("shift_op=c=0;}\n");
				}
				else
				{
					WRITE_CODE("u32 shift_op = REG_R%s(0x%p)&0xFF;\n",REG_R(d.Rs));
					WRITE_CODE("if (shift_op >= 32)\n");
					WRITE_CODE("shift_op=0;\n");
					WRITE_CODE("else\n");
					WRITE_CODE("shift_op=REG_R%s(0x%p)>>shift_op;\n", REG_R(d.Rm));
				}
			}
			break;
		case IRSHIFT_ASR:
			if (!d.R)
			{
				if (clacCarry)
				{
					if (d.Immediate == 0)
						WRITE_CODE("u32 c = BIT31(REG_R%s(0x%p));\n", REG_R(d.Rm));
					else
						WRITE_CODE("u32 c = BIT_N(REG_R%s(0x%p), %u);\n", REG_R(d.Rm), d.Immediate-1);
				}

				if (d.Immediate == 0)
					WRITE_CODE("u32 shift_op = BIT31(REG_R%s(0x%p))*0xFFFFFFFF;\n", REG_R(d.Rm));
				else
					WRITE_CODE("u32 shift_op = (u32)(REG_SR%s(0x%p)>>%u);\n", REG_R(d.Rm), d.Immediate);
			}
			else
			{
				if (clacCarry)
				{
					WRITE_CODE("u32 c;\n");
					WRITE_CODE("u32 shift_op = REG_R%s(0x%p)&0xFF;\n", REG_R(d.Rs));
					WRITE_CODE("if (shift_op == 0){\n");
					WRITE_CODE("c=((Status_Reg*)0x%p)->bits.C;\n", &(GETCPU.CPSR));
					WRITE_CODE("shift_op = REG_R%s(0x%p);\n", REG_R(d.Rm));
					WRITE_CODE("}else if (shift_op < 32){\n");
					WRITE_CODE("c = BIT_N(REG_R%s(0x%p), shift_op-1);\n", REG_R(d.Rm));
					WRITE_CODE("shift_op = (u32)(REG_SR%s(0x%p)>>shift_op);\n", REG_R(d.Rm));
					WRITE_CODE("}else{\n");
					WRITE_CODE("c = BIT31(REG_R%s(0x%p));\n", REG_R(d.Rm));
					WRITE_CODE("shift_op = BIT31(REG_R%s(0x%p))*0xFFFFFFFF;}\n", REG_R(d.Rm));
				}
				else
				{
					WRITE_CODE("u32 shift_op = REG_R%s(0x%p)&0xFF;\n", REG_R(d.Rs));
					WRITE_CODE("if (shift_op == 0)\n");
					WRITE_CODE("shift_op = REG_R%s(0x%p);\n", REG_R(d.Rm));
					WRITE_CODE("else if (shift_op < 32)\n");
					WRITE_CODE("shift_op = (u32)(REG_SR%s(0x%p)>>shift_op);\n", REG_R(d.Rm));
					WRITE_CODE("else\n");
					WRITE_CODE("shift_op = BIT31(REG_R%s(0x%p))*0xFFFFFFFF;\n", REG_R(d.Rm));
				}
			}
			break;
		case IRSHIFT_ROR:
			if (!d.R)
			{
				if (clacCarry)
				{
					if (d.Immediate == 0)
						WRITE_CODE("u32 c = BIT0(REG_R%s(0x%p));\n", REG_R(d.Rm));
					else
						WRITE_CODE("u32 c = BIT_N(REG_R%s(0x%p), %u);\n", REG_R(d.Rm), d.Immediate-1);
				}

				if (d.Immediate == 0)
					WRITE_CODE("u32 shift_op = (((u32)((Status_Reg*)0x%p)->bits.C)<<31)|(REG_R%s(0x%p)>>1);\n", &(GETCPU.CPSR), REG_R(d.Rm));
				else
					WRITE_CODE("u32 shift_op = ROR(REG_R%s(0x%p), %u);\n", REG_R(d.Rm), d.Immediate);
			}
			else
			{
				if (clacCarry)
				{
					WRITE_CODE("u32 c;\n");
					WRITE_CODE("u32 shift_op = REG_R%s(0x%p)&0xFF;\n", REG_R(d.Rs));
					WRITE_CODE("if (shift_op == 0){\n");
					WRITE_CODE("c=((Status_Reg*)0x%p)->bits.C;\n", &(GETCPU.CPSR));
					WRITE_CODE("shift_op = REG_R%s(0x%p);\n", REG_R(d.Rm));
					WRITE_CODE("}else{\n");
					WRITE_CODE("shift_op &= 0x1F;\n");
					WRITE_CODE("if (shift_op != 0){\n");
					WRITE_CODE("c = BIT_N(REG_R%s(0x%p), shift_op-1);\n", REG_R(d.Rm));
					WRITE_CODE("shift_op = ROR(REG_R%s(0x%p), shift_op);\n", REG_R(d.Rm));
					WRITE_CODE("}else{\n");
					WRITE_CODE("c = BIT31(REG_R%s(0x%p));\n", REG_R(d.Rm));
					WRITE_CODE("shift_op = REG_R%s(0x%p);}}\n", REG_R(d.Rm));
				
				}
				else
				{
					WRITE_CODE("u32 shift_op = REG_R%s(0x%p)&0x1F;\n", REG_R(d.Rs));
					WRITE_CODE("if (shift_op == 0)\n");
					WRITE_CODE("shift_op = REG_R%s(0x%p);\n", REG_R(d.Rm));
					WRITE_CODE("else\n");
					WRITE_CODE("shift_op = ROR(REG_R%s(0x%p), shift_op);\n", REG_R(d.Rm));
				}
			}
			break;
		default:
			INFO("Unknow Shift Op : %u.\n", d.Typ);
			if (clacCarry)
				WRITE_CODE("u32 c = 0;\n");
			WRITE_CODE("u32 shift_op = 0;\n");
			break;
		}
	}

	void FASTCALL DataProcessLoadCPSRGenerate(const Decoded &d, char *&szCodeBuffer)
	{
		u32 PROCNUM = d.ProcessID;

		WRITE_CODE("{\n");

		WRITE_CODE("Status_Reg SPSR;\n");
		WRITE_CODE("SPSR.val = ((Status_Reg*)0x%p)->val;\n", &(GETCPU.SPSR));
		WRITE_CODE("((u32 (*)(void*,u8))0x%p)((void*)0x%p,SPSR.bits.mode);\n", armcpu_switchMode, GETCPUPTR);
		WRITE_CODE("((Status_Reg*)0x%p)->val = SPSR.val;\n", &(GETCPU.CPSR));
		WRITE_CODE("((void (*)(void*))0x%p)((void*)0x%p);\n", armcpu_changeCPSR, GETCPUPTR);
		WRITE_CODE("REG_W(0x%p)&=(0xFFFFFFFC|((((Status_Reg*)0x%p)->bits.T)<<1));\n", REG_W(15), &(GETCPU.CPSR));

		WRITE_CODE("}\n");
	}

	void FASTCALL LDM_S_LoadCPSRGenerate(const Decoded &d, char *&szCodeBuffer)
	{
		u32 PROCNUM = d.ProcessID;

		DataProcessLoadCPSRGenerate(d, szCodeBuffer);
	}

	void FASTCALL R15ModifiedGenerate(const Decoded &d, char *&szCodeBuffer)
	{
		u32 PROCNUM = d.ProcessID;

		WRITE_CODE("(*(u32*)0x%p) = REG(0x%p);\n", &(GETCPU.instruct_adr), REG(15));
		WRITE_CODE("return ExecuteCycles;\n");
	}

//------------------------------------------------------------
//                         IROp decoder
//------------------------------------------------------------
	OPCDECODER_DECL(IR_UND)
	{
		u32 PROCNUM = d.ProcessID;

		INFO("IR_UND\n");

		WRITE_CODE("(*(u32*)0x%p) = %u;\n", &(GETCPU.instruction), d.ThumbFlag?d.Instruction.ThumbOp:d.Instruction.ArmOp);
		WRITE_CODE("(*(u32*)0x%p) = %u;\n", &(GETCPU.instruct_adr), d.Address);
		WRITE_CODE("((u32 (*)(void*))0x%p)((void*)0x%p);\n", TRAPUNDEF, GETCPUPTR);
		WRITE_CODE("return ExecuteCycles;\n");
	}

	OPCDECODER_DECL(IR_NOP)
	{
	}

	OPCDECODER_DECL(IR_DUMMY)
	{
	}

	OPCDECODER_DECL(IR_T32P1)
	{
		u32 PROCNUM = d.ProcessID;

		INFO("IR_T32P1\n");

		WRITE_CODE("(*(u32*)0x%p) = %u;\n", &(GETCPU.instruction), d.ThumbFlag?d.Instruction.ThumbOp:d.Instruction.ArmOp);
		WRITE_CODE("(*(u32*)0x%p) = %u;\n", &(GETCPU.instruct_adr), d.Address);
		WRITE_CODE("((u32 (*)(void*))0x%p)((void*)0x%p);\n", TRAPUNDEF, GETCPUPTR);
		WRITE_CODE("return ExecuteCycles;\n");
	}

	OPCDECODER_DECL(IR_T32P2)
	{
		u32 PROCNUM = d.ProcessID;

		INFO("IR_T32P2\n");

		WRITE_CODE("(*(u32*)0x%p) = %u;\n", &(GETCPU.instruction), d.ThumbFlag?d.Instruction.ThumbOp:d.Instruction.ArmOp);
		WRITE_CODE("(*(u32*)0x%p) = %u;\n", &(GETCPU.instruct_adr), d.Address);
		WRITE_CODE("((u32 (*)(void*))0x%p)((void*)0x%p);\n", TRAPUNDEF, GETCPUPTR);
		WRITE_CODE("return ExecuteCycles;\n");
	}

	OPCDECODER_DECL(IR_MOV)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.I)
		{
			WRITE_CODE("REG_W(0x%p)=%u;\n",REG_W(d.Rd), d.Immediate);
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=%u;\n", &(GETCPU.CPSR), BIT31(d.Immediate));
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.N=%u;\n", &(GETCPU.CPSR), BIT31(d.Immediate));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=%u;\n", &(GETCPU.CPSR), d.Immediate==0 ? 1 : 0);
			}
		}
		else
		{
			const bool clacCarry = d.S && !d.R15Modified && (d.FlagsSet & FLAG_C);
			IRShiftOpGenerate(d, szCodeBuffer, clacCarry);

			WRITE_CODE("REG_W(0x%p)=shift_op;\n", REG_W(d.Rd));
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=c;\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(shift_op);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(shift_op==0);\n", &(GETCPU.CPSR));
			}
		}

		if (d.R15Modified)
		{
			if (d.S)
			{
				DataProcessLoadCPSRGenerate(d, szCodeBuffer);
			}

			R15ModifiedGenerate(d, szCodeBuffer);
		}
	}

	OPCDECODER_DECL(IR_MVN)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.I)
		{
			WRITE_CODE("REG_W(0x%p)=%u;\n", REG_W(d.Rd), ~d.Immediate);
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=%u;\n", &(GETCPU.CPSR), BIT31(d.Immediate));
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.N=%u;\n", &(GETCPU.CPSR), BIT31(~d.Immediate));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=%u;\n", &(GETCPU.CPSR), (~d.Immediate)==0 ? 1 : 0);
			}
		}
		else
		{
			const bool clacCarry = d.S && !d.R15Modified && (d.FlagsSet & FLAG_C);
			IRShiftOpGenerate(d, szCodeBuffer, clacCarry);

			WRITE_CODE("shift_op=REG_W(0x%p)=~shift_op;\n", REG_W(d.Rd));
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=c;\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(shift_op);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(shift_op==0);\n", &(GETCPU.CPSR));
			}
		}

		if (d.R15Modified)
		{
			if (d.S)
			{
				DataProcessLoadCPSRGenerate(d, szCodeBuffer);
			}

			R15ModifiedGenerate(d, szCodeBuffer);
		}
	}

	OPCDECODER_DECL(IR_AND)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.I)
		{
			WRITE_CODE("u32 shift_op=REG_W(0x%p)=REG_R%s(0x%p)&%u;\n", REG_W(d.Rd), REG_R(d.Rn), d.Immediate);
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=%u;\n", &(GETCPU.CPSR), BIT31(d.Immediate));
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(shift_op);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(shift_op==0);\n", &(GETCPU.CPSR));
			}
		}
		else
		{
			const bool clacCarry = d.S && !d.R15Modified && (d.FlagsSet & FLAG_C);
			IRShiftOpGenerate(d, szCodeBuffer, clacCarry);

			WRITE_CODE("shift_op=REG_W(0x%p)=REG_R%s(0x%p)&shift_op;\n", REG_W(d.Rd), REG_R(d.Rn));
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=c;\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(shift_op);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(shift_op==0);\n", &(GETCPU.CPSR));
			}
		}

		if (d.R15Modified)
		{
			if (d.S)
			{
				DataProcessLoadCPSRGenerate(d, szCodeBuffer);
			}

			R15ModifiedGenerate(d, szCodeBuffer);
		}
	}

	OPCDECODER_DECL(IR_TST)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.I)
		{
			WRITE_CODE("u32 shift_op=REG_R%s(0x%p)&%u;\n", REG_R(d.Rn), d.Immediate);

			{
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=%u;\n", &(GETCPU.CPSR), BIT31(d.Immediate));
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(shift_op);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(shift_op==0);\n", &(GETCPU.CPSR));
			}
		}
		else
		{
			const bool clacCarry = (d.FlagsSet & FLAG_C);
			IRShiftOpGenerate(d, szCodeBuffer, clacCarry);

			WRITE_CODE("shift_op=REG_R%s(0x%p)&shift_op;\n", REG_R(d.Rn));
			
			{
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=c;\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(shift_op);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(shift_op==0);\n", &(GETCPU.CPSR));
			}
		}
	}

	OPCDECODER_DECL(IR_EOR)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.I)
		{
			WRITE_CODE("u32 shift_op=REG_W(0x%p)=REG_R%s(0x%p)^%u;\n", REG_W(d.Rd), REG_R(d.Rn), d.Immediate);
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=%u;\n", &(GETCPU.CPSR), BIT31(d.Immediate));
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(shift_op);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(shift_op==0);\n", &(GETCPU.CPSR));
			}
		}
		else
		{
			const bool clacCarry = d.S && !d.R15Modified && (d.FlagsSet & FLAG_C);
			IRShiftOpGenerate(d, szCodeBuffer, clacCarry);

			WRITE_CODE("shift_op=REG_W(0x%p)=REG_R%s(0x%p)^shift_op;\n", REG_W(d.Rd), REG_R(d.Rn));
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=c;\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(shift_op);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(shift_op==0);\n", &(GETCPU.CPSR));
			}
		}

		if (d.R15Modified)
		{
			if (d.S)
			{
				DataProcessLoadCPSRGenerate(d, szCodeBuffer);
			}

			R15ModifiedGenerate(d, szCodeBuffer);
		}
	}

	OPCDECODER_DECL(IR_TEQ)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.I)
		{
			WRITE_CODE("u32 shift_op=REG_R%s(0x%p)^%u;\n", REG_R(d.Rn), d.Immediate);

			{
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=%u;\n", &(GETCPU.CPSR), BIT31(d.Immediate));
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(shift_op);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(shift_op==0);\n", &(GETCPU.CPSR));
			}
		}
		else
		{
			const bool clacCarry = (d.FlagsSet & FLAG_C);
			IRShiftOpGenerate(d, szCodeBuffer, clacCarry);

			WRITE_CODE("shift_op=REG_R%s(0x%p)^shift_op;\n", REG_R(d.Rn));
			
			{
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=c;\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(shift_op);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(shift_op==0);\n", &(GETCPU.CPSR));
			}
		}
	}

	OPCDECODER_DECL(IR_ORR)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.I)
		{
			WRITE_CODE("u32 shift_op=REG_W(0x%p)=REG_R%s(0x%p)|%u;\n", REG_W(d.Rd), REG_R(d.Rn), d.Immediate);
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=%u;\n", &(GETCPU.CPSR), BIT31(d.Immediate));
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(shift_op);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(shift_op==0);\n", &(GETCPU.CPSR));
			}
		}
		else
		{
			const bool clacCarry = d.S && !d.R15Modified && (d.FlagsSet & FLAG_C);
			IRShiftOpGenerate(d, szCodeBuffer, clacCarry);

			WRITE_CODE("shift_op=REG_W(0x%p)=REG_R%s(0x%p)|shift_op;\n", REG_W(d.Rd), REG_R(d.Rn));
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=c;\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(shift_op);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(shift_op==0);\n", &(GETCPU.CPSR));
			}
		}

		if (d.R15Modified)
		{
			if (d.S)
			{
				DataProcessLoadCPSRGenerate(d, szCodeBuffer);
			}

			R15ModifiedGenerate(d, szCodeBuffer);
		}
	}

	OPCDECODER_DECL(IR_BIC)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.I)
		{
			WRITE_CODE("u32 shift_op=REG_W(0x%p)=REG_R%s(0x%p)&%u;\n", REG_W(d.Rd), REG_R(d.Rn), ~d.Immediate);
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=%u;\n", &(GETCPU.CPSR), BIT31(d.Immediate));
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(shift_op);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(shift_op==0);\n", &(GETCPU.CPSR));
			}
		}
		else
		{
			const bool clacCarry = d.S && !d.R15Modified && (d.FlagsSet & FLAG_C);
			IRShiftOpGenerate(d, szCodeBuffer, clacCarry);

			WRITE_CODE("shift_op=REG_W(0x%p)=REG_R%s(0x%p)&(~shift_op);\n", REG_W(d.Rd), REG_R(d.Rn));
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=c;\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(shift_op);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(shift_op==0);\n", &(GETCPU.CPSR));
			}
		}

		if (d.R15Modified)
		{
			if (d.S)
			{
				DataProcessLoadCPSRGenerate(d, szCodeBuffer);
			}

			R15ModifiedGenerate(d, szCodeBuffer);
		}
	}

	OPCDECODER_DECL(IR_ADD)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.I)
		{
			if (d.S && !d.R15Modified && ((d.FlagsSet & FLAG_C) || (d.FlagsSet & FLAG_V)))
				WRITE_CODE("u32 v=REG_R%s(0x%p);\n", REG_R(d.Rn));
			WRITE_CODE("REG_W(0x%p)=REG_R%s(0x%p)+%u;\n", REG_W(d.Rd), REG_R(d.Rn), d.Immediate);
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(REG(0x%p));\n", &(GETCPU.CPSR), REG(d.Rd));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(REG(0x%p)==0);\n", &(GETCPU.CPSR), REG(d.Rd));
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=CarryFrom(v, %u);\n", &(GETCPU.CPSR), d.Immediate);
				if (d.FlagsSet & FLAG_V)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.V=OverflowFromADD(REG(0x%p), v, %u);\n", &(GETCPU.CPSR), REG(d.Rd), d.Immediate);
			}
		}
		else
		{
			IRShiftOpGenerate(d, szCodeBuffer, false);

			if (d.S && !d.R15Modified && ((d.FlagsSet & FLAG_C) || (d.FlagsSet & FLAG_V)))
				WRITE_CODE("u32 v=REG_R%s(0x%p);\n", REG_R(d.Rn));
			WRITE_CODE("REG_W(0x%p)=REG_R%s(0x%p)+shift_op;\n", REG_W(d.Rd), REG_R(d.Rn));
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(REG(0x%p));\n", &(GETCPU.CPSR), REG(d.Rd));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(REG(0x%p)==0);\n", &(GETCPU.CPSR), REG(d.Rd));
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=CarryFrom(v, shift_op);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_V)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.V=OverflowFromADD(REG(0x%p), v, shift_op);\n", &(GETCPU.CPSR), REG(d.Rd));
			}
		}

		if (d.R15Modified)
		{
			if (d.S)
			{
				DataProcessLoadCPSRGenerate(d, szCodeBuffer);
			}

			R15ModifiedGenerate(d, szCodeBuffer);
		}
	}

	OPCDECODER_DECL(IR_ADC)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.I)
		{
			if (d.S && !d.R15Modified && ((d.FlagsSet & FLAG_C) || (d.FlagsSet & FLAG_V)))
				WRITE_CODE("u32 v=REG_R%s(0x%p);\n", REG_R(d.Rn));
			WRITE_CODE("REG_W(0x%p)=REG_R%s(0x%p)+%u+((Status_Reg*)0x%p)->bits.C;\n", REG_W(d.Rd), REG_R(d.Rn), d.Immediate, &(GETCPU.CPSR));
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(REG(0x%p));\n", &(GETCPU.CPSR), REG(d.Rd));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(REG(0x%p)==0);\n", &(GETCPU.CPSR), REG(d.Rd));
				if (d.FlagsSet & FLAG_V)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.V=BIT31((v^%u^-1) & (v^REG(0x%p)));\n", &(GETCPU.CPSR), d.Immediate, REG(d.Rd));
				if (d.FlagsSet & FLAG_C)
				{
					WRITE_CODE("if(((Status_Reg*)0x%p)->bits.C)\n", &(GETCPU.CPSR));
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=REG(0x%p)<=v;\n", &(GETCPU.CPSR), REG(d.Rd));
					WRITE_CODE("else\n");
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=REG(0x%p)<v;\n", &(GETCPU.CPSR), REG(d.Rd));
				}
			}
		}
		else
		{
			IRShiftOpGenerate(d, szCodeBuffer, false);

			if (d.S && !d.R15Modified && ((d.FlagsSet & FLAG_C) || (d.FlagsSet & FLAG_V)))
				WRITE_CODE("u32 v=REG_R%s(0x%p);\n", REG_R(d.Rn));
			WRITE_CODE("REG_W(0x%p)=REG_R%s(0x%p)+shift_op+((Status_Reg*)0x%p)->bits.C;\n", REG_W(d.Rd), REG_R(d.Rn), &(GETCPU.CPSR));
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(REG(0x%p));\n", &(GETCPU.CPSR), REG(d.Rd));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(REG(0x%p)==0);\n", &(GETCPU.CPSR), REG(d.Rd));
				if (d.FlagsSet & FLAG_V)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.V=BIT31((v^shift_op^-1) & (v^REG(0x%p)));\n", &(GETCPU.CPSR), REG(d.Rd));
				if (d.FlagsSet & FLAG_C)
				{
					WRITE_CODE("if(((Status_Reg*)0x%p)->bits.C)\n", &(GETCPU.CPSR));
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=REG(0x%p)<=v;\n", &(GETCPU.CPSR), REG(d.Rd));
					WRITE_CODE("else\n");
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=REG(0x%p)<v;\n", &(GETCPU.CPSR), REG(d.Rd));
				}
			}
		}

		if (d.R15Modified)
		{
			if (d.S)
			{
				DataProcessLoadCPSRGenerate(d, szCodeBuffer);
			}

			R15ModifiedGenerate(d, szCodeBuffer);
		}
	}

	OPCDECODER_DECL(IR_SUB)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.I)
		{
			if (d.S && !d.R15Modified && ((d.FlagsSet & FLAG_C) || (d.FlagsSet & FLAG_V)))
				WRITE_CODE("u32 v=REG_R%s(0x%p);\n", REG_R(d.Rn));
			WRITE_CODE("REG_W(0x%p)=REG_R%s(0x%p)-%u;\n", REG_W(d.Rd), REG_R(d.Rn), d.Immediate);
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(REG(0x%p));\n", &(GETCPU.CPSR), REG(d.Rd));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(REG(0x%p)==0);\n", &(GETCPU.CPSR), REG(d.Rd));
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=!BorrowFrom(v, %u);\n", &(GETCPU.CPSR), d.Immediate);
				if (d.FlagsSet & FLAG_V)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.V=OverflowFromSUB(REG(0x%p), v, %u);\n", &(GETCPU.CPSR), REG(d.Rd), d.Immediate);
			}
		}
		else
		{
			IRShiftOpGenerate(d, szCodeBuffer, false);

			if (d.S && !d.R15Modified && ((d.FlagsSet & FLAG_C) || (d.FlagsSet & FLAG_V)))
				WRITE_CODE("u32 v=REG_R%s(0x%p);\n", REG_R(d.Rn));
			WRITE_CODE("REG_W(0x%p)=REG_R%s(0x%p)-shift_op;\n", REG_W(d.Rd), REG_R(d.Rn));
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(REG(0x%p));\n", &(GETCPU.CPSR), REG(d.Rd));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(REG(0x%p)==0);\n", &(GETCPU.CPSR), REG(d.Rd));
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=!BorrowFrom(v, shift_op);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_V)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.V=OverflowFromSUB(REG(0x%p), v, shift_op);\n", &(GETCPU.CPSR), REG(d.Rd));
			}
		}

		if (d.R15Modified)
		{
			if (d.S)
			{
				DataProcessLoadCPSRGenerate(d, szCodeBuffer);
			}

			R15ModifiedGenerate(d, szCodeBuffer);
		}
	}

	OPCDECODER_DECL(IR_SBC)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.I)
		{
			if (d.S && !d.R15Modified && ((d.FlagsSet & FLAG_C) || (d.FlagsSet & FLAG_V)))
				WRITE_CODE("u32 v=REG_R%s(0x%p);\n", REG_R(d.Rn));
			WRITE_CODE("REG_W(0x%p)=REG_R%s(0x%p)-%u-!((Status_Reg*)0x%p)->bits.C;\n", REG_W(d.Rd), REG_R(d.Rn), d.Immediate, &(GETCPU.CPSR));
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(REG(0x%p));\n", &(GETCPU.CPSR), REG(d.Rd));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(REG(0x%p)==0);\n", &(GETCPU.CPSR), REG(d.Rd));
				if (d.FlagsSet & FLAG_V)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.V=BIT31((v^%u) & (v^REG(0x%p)));\n", &(GETCPU.CPSR), d.Immediate, REG(d.Rd));
				if (d.FlagsSet & FLAG_C)
				{
					WRITE_CODE("if(((Status_Reg*)0x%p)->bits.C)\n", &(GETCPU.CPSR));
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=v>=%u;\n", &(GETCPU.CPSR), d.Immediate);
					WRITE_CODE("else\n");
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=v>%u;\n", &(GETCPU.CPSR), d.Immediate);
				}
			}
		}
		else
		{
			IRShiftOpGenerate(d, szCodeBuffer, false);

			if (d.S && !d.R15Modified && ((d.FlagsSet & FLAG_C) || (d.FlagsSet & FLAG_V)))
				WRITE_CODE("u32 v=REG_R%s(0x%p);\n", REG_R(d.Rn));
			WRITE_CODE("REG_W(0x%p)=REG_R%s(0x%p)-shift_op-!((Status_Reg*)0x%p)->bits.C;\n", REG_W(d.Rd), REG_R(d.Rn), &(GETCPU.CPSR));
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(REG(0x%p));\n", &(GETCPU.CPSR), REG(d.Rd));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(REG(0x%p)==0);\n", &(GETCPU.CPSR), REG(d.Rd));
				if (d.FlagsSet & FLAG_V)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.V=BIT31((v^shift_op) & (v^REG(0x%p)));\n", &(GETCPU.CPSR), REG(d.Rd));
				if (d.FlagsSet & FLAG_C)
				{
					WRITE_CODE("if(((Status_Reg*)0x%p)->bits.C)\n", &(GETCPU.CPSR));
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=v>=shift_op;\n", &(GETCPU.CPSR));
					WRITE_CODE("else\n");
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=v>shift_op;\n", &(GETCPU.CPSR));
				}
			}
		}

		if (d.R15Modified)
		{
			if (d.S)
			{
				DataProcessLoadCPSRGenerate(d, szCodeBuffer);
			}

			R15ModifiedGenerate(d, szCodeBuffer);
		}
	}

	OPCDECODER_DECL(IR_RSB)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.I)
		{
			if (d.S && !d.R15Modified && ((d.FlagsSet & FLAG_C) || (d.FlagsSet & FLAG_V)))
				WRITE_CODE("u32 v=REG_R%s(0x%p);\n", REG_R(d.Rn));
			WRITE_CODE("REG_W(0x%p)=%u-REG_R%s(0x%p);\n", REG_W(d.Rd), d.Immediate, REG_R(d.Rn));
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(REG(0x%p));\n", &(GETCPU.CPSR), REG(d.Rd));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(REG(0x%p)==0);\n", &(GETCPU.CPSR), REG(d.Rd));
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=!BorrowFrom(%u, v);\n", &(GETCPU.CPSR), d.Immediate);
				if (d.FlagsSet & FLAG_V)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.V=OverflowFromSUB(REG(0x%p), %u, v);\n", &(GETCPU.CPSR), REG(d.Rd), d.Immediate);
			}
		}
		else
		{
			IRShiftOpGenerate(d, szCodeBuffer, false);

			if (d.S && !d.R15Modified && ((d.FlagsSet & FLAG_C) || (d.FlagsSet & FLAG_V)))
				WRITE_CODE("u32 v=REG_R%s(0x%p);\n", REG_R(d.Rn));
			WRITE_CODE("REG_W(0x%p)=shift_op-REG_R%s(0x%p);\n", REG_W(d.Rd), REG_R(d.Rn));
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(REG(0x%p));\n", &(GETCPU.CPSR), REG(d.Rd));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(REG(0x%p)==0);\n", &(GETCPU.CPSR), REG(d.Rd));
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=!BorrowFrom(shift_op, v);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_V)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.V=OverflowFromSUB(REG(0x%p), shift_op, v);\n", &(GETCPU.CPSR), REG(d.Rd));
			}
		}

		if (d.R15Modified)
		{
			if (d.S)
			{
				DataProcessLoadCPSRGenerate(d, szCodeBuffer);
			}

			R15ModifiedGenerate(d, szCodeBuffer);
		}
	}

	OPCDECODER_DECL(IR_RSC)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.I)
		{
			if (d.S && !d.R15Modified && ((d.FlagsSet & FLAG_C) || (d.FlagsSet & FLAG_V)))
				WRITE_CODE("u32 v=REG_R%s(0x%p);\n", REG_R(d.Rn));
			WRITE_CODE("REG_W(0x%p)=%u-REG_R%s(0x%p)-!((Status_Reg*)0x%p)->bits.C;\n", REG_W(d.Rd), d.Immediate, REG_R(d.Rn), &(GETCPU.CPSR));
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(REG(0x%p));\n", &(GETCPU.CPSR), REG(d.Rd));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(REG(0x%p)==0);\n", &(GETCPU.CPSR), REG(d.Rd));
				if (d.FlagsSet & FLAG_V)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.V=BIT31((%u^v) & (%u^REG(0x%p)));\n", &(GETCPU.CPSR), d.Immediate, d.Immediate, REG(d.Rd));
				if (d.FlagsSet & FLAG_C)
				{
					WRITE_CODE("if(((Status_Reg*)0x%p)->bits.C)\n", &(GETCPU.CPSR));
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=%u>=v;\n", &(GETCPU.CPSR), d.Immediate);
					WRITE_CODE("else\n");
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=%u>v;\n", &(GETCPU.CPSR), d.Immediate);
				}
			}
		}
		else
		{
			IRShiftOpGenerate(d, szCodeBuffer, false);

			if (d.S && !d.R15Modified && ((d.FlagsSet & FLAG_C) || (d.FlagsSet & FLAG_V)))
				WRITE_CODE("u32 v=REG_R%s(0x%p);\n", REG_R(d.Rn));
			WRITE_CODE("REG_W(0x%p)=shift_op-REG_R%s(0x%p)-!((Status_Reg*)0x%p)->bits.C;\n", REG_W(d.Rd), REG_R(d.Rn), &(GETCPU.CPSR));
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(REG(0x%p));\n", &(GETCPU.CPSR), REG(d.Rd));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(REG(0x%p)==0);\n", &(GETCPU.CPSR), REG(d.Rd));
				if (d.FlagsSet & FLAG_V)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.V=BIT31((v^shift_op) & (shift_op^REG(0x%p)));\n", &(GETCPU.CPSR), REG(d.Rd));
				if (d.FlagsSet & FLAG_C)
				{
					WRITE_CODE("if(((Status_Reg*)0x%p)->bits.C)\n", &(GETCPU.CPSR));
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=shift_op>=v;\n", &(GETCPU.CPSR));
					WRITE_CODE("else\n");
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=shift_op>v;\n", &(GETCPU.CPSR));
				}
			}
		}

		if (d.R15Modified)
		{
			if (d.S)
			{
				DataProcessLoadCPSRGenerate(d, szCodeBuffer);
			}

			R15ModifiedGenerate(d, szCodeBuffer);
		}
	}

	OPCDECODER_DECL(IR_CMP)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.I)
		{
			WRITE_CODE("u32 tmp=REG_R%s(0x%p)-%u;\n", REG_R(d.Rn), d.Immediate);

			{
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(tmp);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(tmp==0);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=!BorrowFrom(REG_R%s(0x%p), %u);\n", &(GETCPU.CPSR), REG_R(d.Rn), d.Immediate);
				if (d.FlagsSet & FLAG_V)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.V=OverflowFromSUB(tmp, REG_R%s(0x%p), %u);\n", &(GETCPU.CPSR), REG_R(d.Rn), d.Immediate);
			}
		}
		else
		{
			IRShiftOpGenerate(d, szCodeBuffer, false);

			WRITE_CODE("u32 tmp=REG_R%s(0x%p)-shift_op;\n", REG_R(d.Rn));

			{
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(tmp);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(tmp==0);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=!BorrowFrom(REG_R%s(0x%p), shift_op);\n", &(GETCPU.CPSR), REG_R(d.Rn));
				if (d.FlagsSet & FLAG_V)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.V=OverflowFromSUB(tmp, REG_R%s(0x%p), shift_op);\n", &(GETCPU.CPSR), REG_R(d.Rn));
			}
		}
	}

	OPCDECODER_DECL(IR_CMN)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.I)
		{
			WRITE_CODE("u32 tmp=REG_R%s(0x%p)+%u;\n", REG_R(d.Rn), d.Immediate);

			{
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(tmp);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(tmp==0);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=CarryFrom(REG_R%s(0x%p), %u);\n", &(GETCPU.CPSR), REG_R(d.Rn), d.Immediate);
				if (d.FlagsSet & FLAG_V)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.V=OverflowFromADD(tmp, REG_R%s(0x%p), %u);\n", &(GETCPU.CPSR), REG_R(d.Rn), d.Immediate);
			}
		}
		else
		{
			IRShiftOpGenerate(d, szCodeBuffer, false);

			WRITE_CODE("u32 tmp=REG_R%s(0x%p)+shift_op;\n", REG_R(d.Rn));

			{
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(tmp);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(tmp==0);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=CarryFrom(REG_R%s(0x%p), shift_op);\n", &(GETCPU.CPSR), REG_R(d.Rn));
				if (d.FlagsSet & FLAG_V)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.V=OverflowFromADD(tmp, REG_R%s(0x%p), shift_op);\n", &(GETCPU.CPSR), REG_R(d.Rn));
			}
		}
	}

	OPCDECODER_DECL(IR_MUL)
	{
		u32 PROCNUM = d.ProcessID;

		WRITE_CODE("u32 v=REG_R%s(0x%p);\n", REG_R(d.Rs));
		WRITE_CODE("REG_W(0x%p)=REG_R%s(0x%p)*v;\n", REG_W(d.Rd), REG_R(d.Rm));
		if (d.S)
		{
			if (d.FlagsSet & FLAG_N)
				WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(REG(0x%p));\n", &(GETCPU.CPSR), REG(d.Rd));
			if (d.FlagsSet & FLAG_Z)
				WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(REG(0x%p)==0);\n", &(GETCPU.CPSR), REG(d.Rd));
		}

		WRITE_CODE("v >>= 8;\n");
		WRITE_CODE("if((v==0)||(v==0xFFFFFF)){\n");
		WRITE_CODE("ExecuteCycles+=1+1;\n");
		WRITE_CODE("}else{\n");
		WRITE_CODE("v >>= 8;\n");
		WRITE_CODE("if((v==0)||(v==0xFFFF)){\n");
		WRITE_CODE("ExecuteCycles+=1+2;\n");
		WRITE_CODE("}else{\n");
		WRITE_CODE("v >>= 8;\n");
		WRITE_CODE("if((v==0)||(v==0xFF)){\n");
		WRITE_CODE("ExecuteCycles+=1+3;\n");
		WRITE_CODE("}else{\n");
		WRITE_CODE("ExecuteCycles+=1+4;\n");
		WRITE_CODE("}}}\n");
	}

	OPCDECODER_DECL(IR_MLA)
	{
		u32 PROCNUM = d.ProcessID;

		WRITE_CODE("u32 v=REG_R%s(0x%p);\n", REG_R(d.Rs));
		WRITE_CODE("REG_W(0x%p)=REG_R%s(0x%p)*v+REG_R%s(0x%p);\n", REG_W(d.Rd), REG_R(d.Rm), REG_R(d.Rn));
		if (d.S)
		{
			if (d.FlagsSet & FLAG_N)
				WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(REG(0x%p));\n", &(GETCPU.CPSR), REG(d.Rd));
			if (d.FlagsSet & FLAG_Z)
				WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(REG(0x%p)==0);\n", &(GETCPU.CPSR), REG(d.Rd));
		}

		WRITE_CODE("v >>= 8;\n");
		WRITE_CODE("if((v==0)||(v==0xFFFFFF)){\n");
		WRITE_CODE("ExecuteCycles+=2+1;\n");
		WRITE_CODE("}else{\n");
		WRITE_CODE("v >>= 8;\n");
		WRITE_CODE("if((v==0)||(v==0xFFFF)){\n");
		WRITE_CODE("ExecuteCycles+=2+2;\n");
		WRITE_CODE("}else{\n");
		WRITE_CODE("v >>= 8;\n");
		WRITE_CODE("if((v==0)||(v==0xFF)){\n");
		WRITE_CODE("ExecuteCycles+=2+3;\n");
		WRITE_CODE("}else{\n");
		WRITE_CODE("ExecuteCycles+=2+4;\n");
		WRITE_CODE("}}}\n");
	}

	OPCDECODER_DECL(IR_UMULL)
	{
		u32 PROCNUM = d.ProcessID;

		WRITE_CODE("u32 v=REG_R%s(0x%p);\n", REG_R(d.Rs));
		WRITE_CODE("u64 res=(u64)REG_R%s(0x%p)*v;\n", REG_R(d.Rm));
		WRITE_CODE("REG_W(0x%p)=(u32)res;\n", REG_W(d.Rn));
		WRITE_CODE("REG_W(0x%p)=(u32)(res>>32);\n", REG_W(d.Rd));
		if (d.S)
		{
			if (d.FlagsSet & FLAG_N)
				WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(REG(0x%p));\n", &(GETCPU.CPSR), REG(d.Rd));
			if (d.FlagsSet & FLAG_Z)
				WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(REG(0x%p)==0)&&(REG(0x%p)==0);\n", &(GETCPU.CPSR), REG(d.Rd), REG(d.Rn));
		}

		WRITE_CODE("v >>= 8;\n");
		WRITE_CODE("if(v==0){\n");
		WRITE_CODE("ExecuteCycles+=2+1;\n");
		WRITE_CODE("}else{\n");
		WRITE_CODE("v >>= 8;\n");
		WRITE_CODE("if(v==0){\n");
		WRITE_CODE("ExecuteCycles+=2+2;\n");
		WRITE_CODE("}else{\n");
		WRITE_CODE("v >>= 8;\n");
		WRITE_CODE("if(v==0){\n");
		WRITE_CODE("ExecuteCycles+=2+3;\n");
		WRITE_CODE("}else{\n");
		WRITE_CODE("ExecuteCycles+=2+4;\n");
		WRITE_CODE("}}}\n");
	}

	OPCDECODER_DECL(IR_UMLAL)
	{
		u32 PROCNUM = d.ProcessID;

		WRITE_CODE("u32 v=REG_R%s(0x%p);\n", REG_R(d.Rs));
		WRITE_CODE("u64 res=(u64)REG_R%s(0x%p)*v;\n", REG_R(d.Rm));
		WRITE_CODE("u32 tmp=(u32)res;\n");
		WRITE_CODE("REG_W(0x%p)=(u32)(res>>32)+REG_R%s(0x%p)+CarryFrom(tmp,REG_R%s(0x%p));\n", REG_W(d.Rd), REG_R(d.Rd), REG_R(d.Rn));
		WRITE_CODE("REG_W(0x%p)=REG_R%s(0x%p)+tmp;\n", REG_W(d.Rn), REG_R(d.Rn));
		if (d.S)
		{
			if (d.FlagsSet & FLAG_N)
				WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(REG(0x%p));\n", &(GETCPU.CPSR), REG(d.Rd));
			if (d.FlagsSet & FLAG_Z)
				WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(REG(0x%p)==0)&&(REG(0x%p)==0);\n", &(GETCPU.CPSR), REG(d.Rd), REG(d.Rn));
		}

		WRITE_CODE("v >>= 8;\n");
		WRITE_CODE("if(v==0){\n");
		WRITE_CODE("ExecuteCycles+=3+1;\n");
		WRITE_CODE("}else{\n");
		WRITE_CODE("v >>= 8;\n");
		WRITE_CODE("if(v==0){\n");
		WRITE_CODE("ExecuteCycles+=3+2;\n");
		WRITE_CODE("}else{\n");
		WRITE_CODE("v >>= 8;\n");
		WRITE_CODE("if(v==0){\n");
		WRITE_CODE("ExecuteCycles+=3+3;\n");
		WRITE_CODE("}else{\n");
		WRITE_CODE("ExecuteCycles+=3+4;\n");
		WRITE_CODE("}}}\n");
	}

	OPCDECODER_DECL(IR_SMULL)
	{
		u32 PROCNUM = d.ProcessID;

		WRITE_CODE("s64 v=REG_SR%s(0x%p);\n", REG_R(d.Rs));
		WRITE_CODE("s64 res=(s64)REG_SR%s(0x%p)*v;\n", REG_R(d.Rm));
		WRITE_CODE("REG_W(0x%p)=(u32)res;\n", REG_W(d.Rn));
		WRITE_CODE("REG_W(0x%p)=(u32)(res>>32);\n", REG_W(d.Rd));
		if (d.S)
		{
			if (d.FlagsSet & FLAG_N)
				WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(REG(0x%p));\n", &(GETCPU.CPSR), REG(d.Rd));
			if (d.FlagsSet & FLAG_Z)
				WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(REG(0x%p)==0)&&(REG(0x%p)==0);\n", &(GETCPU.CPSR), REG(d.Rd), REG(d.Rn));
		}

		WRITE_CODE("u32 v2 = v&0xFFFFFFFF;\n");
		WRITE_CODE("v2 >>= 8;\n");
		WRITE_CODE("if((v2==0)||(v2==0xFFFFFF)){\n");
		WRITE_CODE("ExecuteCycles+=2+1;\n");
		WRITE_CODE("}else{\n");
		WRITE_CODE("v2 >>= 8;\n");
		WRITE_CODE("if((v2==0)||(v2==0xFFFF)){\n");
		WRITE_CODE("ExecuteCycles+=2+2;\n");
		WRITE_CODE("}else{\n");
		WRITE_CODE("v2 >>= 8;\n");
		WRITE_CODE("if((v2==0)||(v2==0xFF)){\n");
		WRITE_CODE("ExecuteCycles+=2+3;\n");
		WRITE_CODE("}else{\n");
		WRITE_CODE("ExecuteCycles+=2+4;\n");
		WRITE_CODE("}}}\n");
	}

	OPCDECODER_DECL(IR_SMLAL)
	{
		u32 PROCNUM = d.ProcessID;

		WRITE_CODE("s64 v=REG_SR%s(0x%p);\n", REG_R(d.Rs));
		WRITE_CODE("s64 res=(s64)REG_SR%s(0x%p)*v;\n", REG_R(d.Rm));
		WRITE_CODE("u32 tmp=(u32)res;\n");
		WRITE_CODE("REG_W(0x%p)=(u32)(res>>32)+REG_R%s(0x%p)+CarryFrom(tmp,REG_R%s(0x%p));\n", REG_W(d.Rd), REG_R(d.Rd), REG_R(d.Rn));
		WRITE_CODE("REG_W(0x%p)=REG_R%s(0x%p)+tmp;\n", REG_W(d.Rn), REG_R(d.Rn));
		if (d.S)
		{
			if (d.FlagsSet & FLAG_N)
				WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(REG(0x%p));\n", &(GETCPU.CPSR), REG(d.Rd));
			if (d.FlagsSet & FLAG_Z)
				WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(REG(0x%p)==0)&&(REG(0x%p)==0);\n", &(GETCPU.CPSR), REG(d.Rd), REG(d.Rn));
		}

		WRITE_CODE("u32 v2 = v&0xFFFFFFFF;\n");
		WRITE_CODE("v2 >>= 8;\n");
		WRITE_CODE("if((v2==0)||(v2==0xFFFFFF)){\n");
		WRITE_CODE("ExecuteCycles+=3+1;\n");
		WRITE_CODE("}else{\n");
		WRITE_CODE("v2 >>= 8;\n");
		WRITE_CODE("if((v2==0)||(v2==0xFFFF)){\n");
		WRITE_CODE("ExecuteCycles+=3+2;\n");
		WRITE_CODE("}else{\n");
		WRITE_CODE("v2 >>= 8;\n");
		WRITE_CODE("if((v2==0)||(v2==0xFF)){\n");
		WRITE_CODE("ExecuteCycles+=3+3;\n");
		WRITE_CODE("}else{\n");
		WRITE_CODE("ExecuteCycles+=3+4;\n");
		WRITE_CODE("}}}\n");
	}

	OPCDECODER_DECL(IR_SMULxy)
	{
		u32 PROCNUM = d.ProcessID;

		WRITE_CODE("REG_W(0x%p)=(u32)(", REG_W(d.Rd));
		if (d.X)
			WRITE_CODE("HWORD(");
		else
			WRITE_CODE("LWORD(");
		WRITE_CODE("REG_R%s(0x%p))*", REG_R(d.Rm));
		if (d.Y)
			WRITE_CODE("HWORD(");
		else
			WRITE_CODE("LWORD(");
		WRITE_CODE("REG_R%s(0x%p)));\n", REG_R(d.Rs));
	}

	OPCDECODER_DECL(IR_SMLAxy)
	{
		u32 PROCNUM = d.ProcessID;

		if (!d.X && !d.Y)
		{
			WRITE_CODE("u32 tmp=(u32)((s16)REG_R%s(0x%p) * (s16)REG_R%s(0x%p));\n", REG_R(d.Rm), REG_R(d.Rs));
			WRITE_CODE("REG_W(0x%p) = tmp + REG_R%s(0x%p);\n", REG_W(d.Rd), REG_R(d.Rn));
			WRITE_CODE("if (OverflowFromADD(REG(0x%p), tmp, REG_R%s(0x%p)))\n", REG(d.Rd), REG_R(d.Rn));
			WRITE_CODE("((Status_Reg*)0x%p)->bits.Q=1;\n", &(GETCPU.CPSR));
		}
		else
		{
			WRITE_CODE("u32 tmp=(u32)(");
			if (d.X)
				WRITE_CODE("HWORD(");
			else
				WRITE_CODE("LWORD(");
			WRITE_CODE("REG_R%s(0x%p))*", REG_R(d.Rm));
			if (d.Y)
				WRITE_CODE("HWORD(");
			else
				WRITE_CODE("LWORD(");
			WRITE_CODE("REG_R%s(0x%p)));\n", REG_R(d.Rs));
			WRITE_CODE("u32 a = REG_R%s(0x%p);\n", REG_R(d.Rn));
			WRITE_CODE("REG_W(0x%p) = tmp + a;\n", REG_W(d.Rd));
			WRITE_CODE("if (SIGNED_OVERFLOW(tmp, a, REG(0x%p)))\n", REG(d.Rd));
			WRITE_CODE("((Status_Reg*)0x%p)->bits.Q=1;\n", &(GETCPU.CPSR));
		}
	}

	OPCDECODER_DECL(IR_SMULWy)
	{
		u32 PROCNUM = d.ProcessID;

		WRITE_CODE("s64 tmp = (s64)");
		if (d.Y)
			WRITE_CODE("HWORD(");
		else
			WRITE_CODE("LWORD(");
		WRITE_CODE("REG_R%s(0x%p)) * (s64)((s32)REG_R%s(0x%p));\n", REG_R(d.Rs), REG_R(d.Rm));
		WRITE_CODE("REG_W(0x%p) = ((tmp>>16)&0xFFFFFFFF);\n", REG_W(d.Rd));
	}

	OPCDECODER_DECL(IR_SMLAWy)
	{
		u32 PROCNUM = d.ProcessID;

		WRITE_CODE("s64 tmp = (s64)");
		if (d.Y)
			WRITE_CODE("HWORD(");
		else
			WRITE_CODE("LWORD(");
		WRITE_CODE("REG_R%s(0x%p)) * (s64)((s32)REG_R%s(0x%p));\n", REG_R(d.Rs), REG_R(d.Rm));
		WRITE_CODE("u32 a = REG_R%s(0x%p);\n", REG_R(d.Rn));
		WRITE_CODE("tmp = ((tmp>>16)&0xFFFFFFFF);\n");
		WRITE_CODE("REG_W(0x%p) = tmp + a;\n", REG_W(d.Rd));
		WRITE_CODE("if (SIGNED_OVERFLOW((u32)tmp, a, REG(0x%p)))\n", REG(d.Rd));
		WRITE_CODE("((Status_Reg*)0x%p)->bits.Q=1;\n", &(GETCPU.CPSR));
	}

	OPCDECODER_DECL(IR_SMLALxy)
	{
		u32 PROCNUM = d.ProcessID;

		WRITE_CODE("s64 tmp=(s64)(");
		if (d.X)
			WRITE_CODE("HWORD(");
		else
			WRITE_CODE("LWORD(");
		WRITE_CODE("REG_R%s(0x%p))*", REG_R(d.Rm));
		if (d.Y)
			WRITE_CODE("HWORD(");
		else
			WRITE_CODE("LWORD(");
		WRITE_CODE("REG_R%s(0x%p)));\n", REG_R(d.Rs));
		WRITE_CODE("u64 res = (u64)tmp + REG_R%s(0x%p);\n", REG_R(d.Rn));
		WRITE_CODE("REG_W(0x%p) = (u32)res;\n", REG_W(d.Rn));
		WRITE_CODE("REG_W(0x%p) = REG_R%s(0x%p) + (res + ((tmp<0)*0xFFFFFFFF));\n", REG_W(d.Rd), REG_R(d.Rd));
	}

	template<u32 PROCNUM, u32 memtype, u32 cycle>
	static u32 FASTCALL MEMOP_LDR(u32 adr, u32 *dstreg)
	{
		u32 data = READ32(GETCPU.mem_if->data, adr);
		if(adr&3)
			data = ROR(data, 8*(adr&3));
		*dstreg = data;
		return MMU_aluMemAccessCycles<PROCNUM,32,MMU_AD_READ>(cycle,adr);
	}

	template<u32 PROCNUM, u32 memtype, u32 cycle>
	static u32 FASTCALL MEMOP_LDRB(u32 adr, u32 *dstreg)
	{
		*dstreg = READ8(GETCPU.mem_if->data, adr);
		return MMU_aluMemAccessCycles<PROCNUM,8,MMU_AD_READ>(cycle,adr);
	}

	static const MemOp1 LDR_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_LDR<0,MEMTYPE_GENERIC,3>,
			MEMOP_LDR<0,MEMTYPE_MAIN,3>,
			MEMOP_LDR<0,MEMTYPE_DTCM_ARM9,3>,
			MEMOP_LDR<0,MEMTYPE_GENERIC,3>,//MEMOP_LDR<0,MEMTYPE_ERAM_ARM7,3>,
			MEMOP_LDR<0,MEMTYPE_SWIRAM,3>,
		},
		{
			MEMOP_LDR<1,MEMTYPE_GENERIC,3>,
			MEMOP_LDR<1,MEMTYPE_MAIN,3>,
			MEMOP_LDR<1,MEMTYPE_GENERIC,3>,//MEMOP_LDR<1,MEMTYPE_DTCM_ARM9,3>,
			MEMOP_LDR<1,MEMTYPE_ERAM_ARM7,3>,
			MEMOP_LDR<1,MEMTYPE_SWIRAM,3>,
		}
	};

	static const MemOp1 LDR_R15_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_LDR<0,MEMTYPE_GENERIC,5>,
			MEMOP_LDR<0,MEMTYPE_MAIN,5>,
			MEMOP_LDR<0,MEMTYPE_DTCM_ARM9,5>,
			MEMOP_LDR<0,MEMTYPE_GENERIC,5>,//MEMOP_LDR<0,MEMTYPE_ERAM_ARM7,5>,
			MEMOP_LDR<0,MEMTYPE_SWIRAM,5>,
		},
		{
			MEMOP_LDR<1,MEMTYPE_GENERIC,5>,
			MEMOP_LDR<1,MEMTYPE_MAIN,5>,
			MEMOP_LDR<1,MEMTYPE_GENERIC,5>,//MEMOP_LDR<1,MEMTYPE_DTCM_ARM9,5>,
			MEMOP_LDR<1,MEMTYPE_ERAM_ARM7,5>,
			MEMOP_LDR<1,MEMTYPE_SWIRAM,5>,
		}
	};

	static const MemOp1 LDRB_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_LDRB<0,MEMTYPE_GENERIC,3>,
			MEMOP_LDRB<0,MEMTYPE_MAIN,3>,
			MEMOP_LDRB<0,MEMTYPE_DTCM_ARM9,3>,
			MEMOP_LDRB<0,MEMTYPE_GENERIC,3>,//MEMOP_LDRB<0,MEMTYPE_ERAM_ARM7,3>,
			MEMOP_LDRB<0,MEMTYPE_SWIRAM,3>,
		},
		{
			MEMOP_LDRB<1,MEMTYPE_GENERIC,3>,
			MEMOP_LDRB<1,MEMTYPE_MAIN,3>,
			MEMOP_LDRB<1,MEMTYPE_GENERIC,3>,//MEMOP_LDRB<1,MEMTYPE_DTCM_ARM9,3>,
			MEMOP_LDRB<1,MEMTYPE_ERAM_ARM7,3>,
			MEMOP_LDRB<1,MEMTYPE_SWIRAM,3>,
		}
	};

	OPCDECODER_DECL(IR_LDR)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.P)
		{
			if (d.I)
				WRITE_CODE("u32 adr = REG_R%s(0x%p) %c %u;\n", REG_R(d.Rn), d.U?'+':'-', d.Immediate);
			else
			{
				IRShiftOpGenerate(d, szCodeBuffer, false);

				WRITE_CODE("u32 adr = REG_R%s(0x%p) %c shift_op;\n", REG_R(d.Rn), d.U?'+':'-');
			}

			if (d.W)
				WRITE_CODE("REG_W(0x%p) = adr;\n", REG_W(d.Rn));
		}
		else
		{
			WRITE_CODE("u32 adr = REG_R%s(0x%p);\n", REG_R(d.Rn));
			if (d.I)
				WRITE_CODE("REG_W(0x%p) = adr %c %u;\n", REG_W(d.Rn), d.U?'+':'-', d.Immediate);
			else
			{
				IRShiftOpGenerate(d, szCodeBuffer, false);

				WRITE_CODE("REG_W(0x%p) = adr %c shift_op;\n", REG_W(d.Rn), d.U?'+':'-');
			}
		}

		if (d.B)
			WRITE_CODE("ExecuteCycles+=((u32 (FASTCALL *)(u32, u32*))0x%p)(adr,REGPTR(0x%p));\n", LDRB_Tab[PROCNUM][0], REGPTR(d.Rd));
		else
		{
			if (d.R15Modified)
			{
				WRITE_CODE("ExecuteCycles+=((u32 (FASTCALL *)(u32, u32*))0x%p)(adr,REGPTR(0x%p));\n", LDR_R15_Tab[PROCNUM][0], REGPTR(d.Rd));

				if (PROCNUM == 0)
				{
					WRITE_CODE("((Status_Reg*)0x%p)->bits.T=BIT0(REG(0x%p));\n", &(GETCPU.CPSR), REG(15));
					WRITE_CODE("REG(0x%p) &= 0xFFFFFFFE;\n", REG(15));
				}
				else
					WRITE_CODE("REG(0x%p) &= 0xFFFFFFFC;\n", REG(15));

				R15ModifiedGenerate(d, szCodeBuffer);
			}
			else
				WRITE_CODE("ExecuteCycles+=((u32 (FASTCALL *)(u32, u32*))0x%p)(adr,REGPTR(0x%p));\n", LDR_Tab[PROCNUM][0], REGPTR(d.Rd));
		}
	}

	template<u32 PROCNUM, u32 memtype, u32 cycle>
	static u32 FASTCALL MEMOP_STR(u32 adr, u32 data)
	{
		WRITE32(GETCPU.mem_if->data, adr, data);
		return MMU_aluMemAccessCycles<PROCNUM,32,MMU_AD_WRITE>(cycle,adr);
	}

	template<u32 PROCNUM, u32 memtype, u32 cycle>
	static u32 FASTCALL MEMOP_STRB(u32 adr, u32 data)
	{
		WRITE8(GETCPU.mem_if->data, adr, data);
		return MMU_aluMemAccessCycles<PROCNUM,8,MMU_AD_WRITE>(cycle,adr);
	}

	static const MemOp2 STR_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_STR<0,MEMTYPE_GENERIC,2>,
			MEMOP_STR<0,MEMTYPE_MAIN,2>,
			MEMOP_STR<0,MEMTYPE_DTCM_ARM9,2>,
			MEMOP_STR<0,MEMTYPE_GENERIC,2>,//MEMOP_STR<0,MEMTYPE_ERAM_ARM7,2>,
			MEMOP_STR<0,MEMTYPE_SWIRAM,2>,
		},
		{
			MEMOP_STR<1,MEMTYPE_GENERIC,2>,
			MEMOP_STR<1,MEMTYPE_MAIN,2>,
			MEMOP_STR<1,MEMTYPE_GENERIC,2>,//MEMOP_STR<1,MEMTYPE_DTCM_ARM9,2>,
			MEMOP_STR<1,MEMTYPE_ERAM_ARM7,2>,
			MEMOP_STR<1,MEMTYPE_SWIRAM,2>,
		}
	};

	static const MemOp2 STRB_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_STRB<0,MEMTYPE_GENERIC,2>,
			MEMOP_STRB<0,MEMTYPE_MAIN,2>,
			MEMOP_STRB<0,MEMTYPE_DTCM_ARM9,2>,
			MEMOP_STRB<0,MEMTYPE_GENERIC,2>,//MEMOP_STRB<0,MEMTYPE_ERAM_ARM7,2>,
			MEMOP_STRB<0,MEMTYPE_SWIRAM,2>,
		},
		{
			MEMOP_STRB<1,MEMTYPE_GENERIC,2>,
			MEMOP_STRB<1,MEMTYPE_MAIN,2>,
			MEMOP_STRB<1,MEMTYPE_GENERIC,2>,//MEMOP_STRB<1,MEMTYPE_DTCM_ARM9,2>,
			MEMOP_STRB<1,MEMTYPE_ERAM_ARM7,2>,
			MEMOP_STRB<1,MEMTYPE_SWIRAM,2>,
		}
	};

	OPCDECODER_DECL(IR_STR)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.P)
		{
			if (d.I)
				WRITE_CODE("u32 adr = REG_R%s(0x%p) %c %u;\n", REG_R(d.Rn), d.U?'+':'-', d.Immediate);
			else
			{
				IRShiftOpGenerate(d, szCodeBuffer, false);

				WRITE_CODE("u32 adr = REG_R%s(0x%p) %c shift_op;\n", REG_R(d.Rn), d.U?'+':'-');
			}

			if (d.W)
				WRITE_CODE("REG_W(0x%p) = adr;\n", REG_W(d.Rn));
		}
		else
			WRITE_CODE("u32 adr = REG_R%s(0x%p);\n", REG_R(d.Rn));

		if (d.B)
			WRITE_CODE("ExecuteCycles+=((u32 (FASTCALL *)(u32, u32))0x%p)(adr,REG_R%s(0x%p));\n", STRB_Tab[PROCNUM][0], REG_R(d.Rd));
		else
			WRITE_CODE("ExecuteCycles+=((u32 (FASTCALL *)(u32, u32))0x%p)(adr,REG_R%s(0x%p));\n", STR_Tab[PROCNUM][0], REG_R(d.Rd));

		if (!d.P)
		{
			if (d.I)
				WRITE_CODE("REG_W(0x%p) = adr %c %u;\n", REG_W(d.Rn), d.U?'+':'-', d.Immediate);
			else
			{
				IRShiftOpGenerate(d, szCodeBuffer, false);

				WRITE_CODE("REG_W(0x%p) = adr %c shift_op;\n", REG_W(d.Rn), d.U?'+':'-');
			}
		}
	}

	template<u32 PROCNUM, u32 memtype, u32 cycle>
	static u32 FASTCALL MEMOP_LDRH(u32 adr, u32 *dstreg)
	{
		*dstreg = READ16(GETCPU.mem_if->data, adr);
		return MMU_aluMemAccessCycles<PROCNUM,16,MMU_AD_READ>(cycle,adr);
	}

	template<u32 PROCNUM, u32 memtype, u32 cycle>
	static u32 FASTCALL MEMOP_LDRSH(u32 adr, u32 *dstreg)
	{
		*dstreg = (s16)READ16(GETCPU.mem_if->data, adr);
		return MMU_aluMemAccessCycles<PROCNUM,16,MMU_AD_READ>(cycle,adr);
	}

	template<u32 PROCNUM, u32 memtype, u32 cycle>
	static u32 FASTCALL MEMOP_LDRSB(u32 adr, u32 *dstreg)
	{
		*dstreg = (s8)READ8(GETCPU.mem_if->data, adr);
		return MMU_aluMemAccessCycles<PROCNUM,8,MMU_AD_READ>(cycle,adr);
	}

	static const MemOp1 LDRH_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_LDRH<0,MEMTYPE_GENERIC,3>,
			MEMOP_LDRH<0,MEMTYPE_MAIN,3>,
			MEMOP_LDRH<0,MEMTYPE_DTCM_ARM9,3>,
			MEMOP_LDRH<0,MEMTYPE_GENERIC,3>,//MEMOP_LDRH<0,MEMTYPE_ERAM_ARM7,3>,
			MEMOP_LDRH<0,MEMTYPE_SWIRAM,3>,
		},
		{
			MEMOP_LDRH<1,MEMTYPE_GENERIC,3>,
			MEMOP_LDRH<1,MEMTYPE_MAIN,3>,
			MEMOP_LDRH<1,MEMTYPE_GENERIC,3>,//MEMOP_LDRH<1,MEMTYPE_DTCM_ARM9,3>,
			MEMOP_LDRH<1,MEMTYPE_ERAM_ARM7,3>,
			MEMOP_LDRH<1,MEMTYPE_SWIRAM,3>,
		}
	};

	static const MemOp1 LDRSH_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_LDRSH<0,MEMTYPE_GENERIC,3>,
			MEMOP_LDRSH<0,MEMTYPE_MAIN,3>,
			MEMOP_LDRSH<0,MEMTYPE_DTCM_ARM9,3>,
			MEMOP_LDRSH<0,MEMTYPE_GENERIC,3>,//MEMOP_LDRSH<0,MEMTYPE_ERAM_ARM7,3>,
			MEMOP_LDRSH<0,MEMTYPE_SWIRAM,3>,
		},
		{
			MEMOP_LDRSH<1,MEMTYPE_GENERIC,3>,
			MEMOP_LDRSH<1,MEMTYPE_MAIN,3>,
			MEMOP_LDRSH<1,MEMTYPE_GENERIC,3>,//MEMOP_LDRSH<1,MEMTYPE_DTCM_ARM9,3>,
			MEMOP_LDRSH<1,MEMTYPE_ERAM_ARM7,3>,
			MEMOP_LDRSH<1,MEMTYPE_SWIRAM,3>,
		}
	};

	static const MemOp1 LDRSB_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_LDRSB<0,MEMTYPE_GENERIC,3>,
			MEMOP_LDRSB<0,MEMTYPE_MAIN,3>,
			MEMOP_LDRSB<0,MEMTYPE_DTCM_ARM9,3>,
			MEMOP_LDRSB<0,MEMTYPE_GENERIC,3>,//MEMOP_LDRSB<0,MEMTYPE_ERAM_ARM7,3>,
			MEMOP_LDRSB<0,MEMTYPE_SWIRAM,3>,
		},
		{
			MEMOP_LDRSB<1,MEMTYPE_GENERIC,3>,
			MEMOP_LDRSB<1,MEMTYPE_MAIN,3>,
			MEMOP_LDRSB<1,MEMTYPE_GENERIC,3>,//MEMOP_LDRSB<1,MEMTYPE_DTCM_ARM9,3>,
			MEMOP_LDRSB<1,MEMTYPE_ERAM_ARM7,3>,
			MEMOP_LDRSB<1,MEMTYPE_SWIRAM,3>,
		}
	};

	OPCDECODER_DECL(IR_LDRx)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.P)
		{
			if (d.I)
				WRITE_CODE("u32 adr = REG_R%s(0x%p) %c %u;\n", REG_R(d.Rn), d.U?'+':'-', d.Immediate);
			else
				WRITE_CODE("u32 adr = REG_R%s(0x%p) %c REG_R%s(0x%p);\n", REG_R(d.Rn), d.U?'+':'-', REG_R(d.Rm));

			if (d.W)
				WRITE_CODE("REG_W(0x%p) = adr;\n", REG_W(d.Rn));
		}
		else
		{
			WRITE_CODE("u32 adr = REG_R%s(0x%p);\n", REG_R(d.Rn));

			if (d.I)
				WRITE_CODE("REG_W(0x%p) = adr %c %u;\n", REG_W(d.Rn), d.U?'+':'-', d.Immediate);
			else
				WRITE_CODE("REG_W(0x%p) = adr %c REG_R%s(0x%p);\n", REG_W(d.Rn), d.U?'+':'-', REG_R(d.Rm));
		}

		if (d.H)
		{
			if (d.S)
				WRITE_CODE("ExecuteCycles+=((u32 (FASTCALL *)(u32, u32*))0x%p)(adr,REGPTR(0x%p));\n", LDRSH_Tab[PROCNUM][0], REGPTR(d.Rd));
			else
				WRITE_CODE("ExecuteCycles+=((u32 (FASTCALL *)(u32, u32*))0x%p)(adr,REGPTR(0x%p));\n", LDRH_Tab[PROCNUM][0], REGPTR(d.Rd));
		}
		else
			WRITE_CODE("ExecuteCycles+=((u32 (FASTCALL *)(u32, u32*))0x%p)(adr,REGPTR(0x%p));\n", LDRSB_Tab[PROCNUM][0], REGPTR(d.Rd));
	}

	template<u32 PROCNUM, u32 memtype, u32 cycle>
	static u32 FASTCALL MEMOP_STRH(u32 adr, u32 data)
	{
		WRITE16(GETCPU.mem_if->data, adr, data);
		return MMU_aluMemAccessCycles<PROCNUM,16,MMU_AD_WRITE>(cycle,adr);
	}

	static const MemOp2 STRH_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_STRH<0,MEMTYPE_GENERIC,2>,
			MEMOP_STRH<0,MEMTYPE_MAIN,2>,
			MEMOP_STRH<0,MEMTYPE_DTCM_ARM9,2>,
			MEMOP_STRH<0,MEMTYPE_GENERIC,2>,//MEMOP_STRH<0,MEMTYPE_ERAM_ARM7,2>,
			MEMOP_STRH<0,MEMTYPE_SWIRAM,2>,
		},
		{
			MEMOP_STRH<1,MEMTYPE_GENERIC,2>,
			MEMOP_STRH<1,MEMTYPE_MAIN,2>,
			MEMOP_STRH<1,MEMTYPE_GENERIC,2>,//MEMOP_STRH<1,MEMTYPE_DTCM_ARM9,2>,
			MEMOP_STRH<1,MEMTYPE_ERAM_ARM7,2>,
			MEMOP_STRH<1,MEMTYPE_SWIRAM,2>,
		}
	};

	OPCDECODER_DECL(IR_STRx)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.P)
		{
			if (d.I)
				WRITE_CODE("u32 adr = REG_R%s(0x%p) %c %u;\n", REG_R(d.Rn), d.U?'+':'-', d.Immediate);
			else
				WRITE_CODE("u32 adr = REG_R%s(0x%p) %c REG_R%s(0x%p);\n", REG_R(d.Rn), d.U?'+':'-', REG_R(d.Rm));

			if (d.W)
				WRITE_CODE("REG_W(0x%p) = adr;\n", REG_W(d.Rn));
		}
		else
			WRITE_CODE("u32 adr = REG_R%s(0x%p);\n", REG_R(d.Rn));

		WRITE_CODE("ExecuteCycles+=((u32 (FASTCALL *)(u32, u32))0x%p)(adr,REG_R%s(0x%p));\n", STRH_Tab[PROCNUM][0], REG_R(d.Rd));

		if (!d.P)
		{
			if (d.I)
				WRITE_CODE("REG_W(0x%p) = adr %c %u;\n", REG_W(d.Rn), d.U?'+':'-', d.Immediate);
			else
				WRITE_CODE("REG_W(0x%p) = adr %c REG_R%s(0x%p);\n", REG_W(d.Rn), d.U?'+':'-', REG_R(d.Rm));
		}
	}

	template<u32 PROCNUM, u32 memtype, u32 cycle>
	static u32 FASTCALL MEMOP_LDRD(u32 adr, u32 *dstreg)
	{
		*dstreg = READ32(GETCPU.mem_if->data, adr);
		*(dstreg+1) = READ32(GETCPU.mem_if->data, adr+4);
		return MMU_aluMemCycles<PROCNUM>(cycle, MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr) + MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr+4));
	}

	static const MemOp1 LDRD_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_LDRD<0,MEMTYPE_GENERIC,3>,
			MEMOP_LDRD<0,MEMTYPE_MAIN,3>,
			MEMOP_LDRD<0,MEMTYPE_DTCM_ARM9,3>,
			MEMOP_LDRD<0,MEMTYPE_GENERIC,3>,//MEMOP_LDRD<0,MEMTYPE_ERAM_ARM7,3>,
			MEMOP_LDRD<0,MEMTYPE_SWIRAM,3>,
		},
		{
			MEMOP_LDRD<1,MEMTYPE_GENERIC,3>,
			MEMOP_LDRD<1,MEMTYPE_MAIN,3>,
			MEMOP_LDRD<1,MEMTYPE_GENERIC,3>,//MEMOP_LDRD<1,MEMTYPE_DTCM_ARM9,3>,
			MEMOP_LDRD<1,MEMTYPE_ERAM_ARM7,3>,
			MEMOP_LDRD<1,MEMTYPE_SWIRAM,3>,
		}
	};

	OPCDECODER_DECL(IR_LDRD)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.P)
		{
			if (d.I)
				WRITE_CODE("u32 adr = REG_R%s(0x%p) %c %u;\n", REG_R(d.Rn), d.U?'+':'-', d.Immediate);
			else
				WRITE_CODE("u32 adr = REG_R%s(0x%p) %c REG_R%s(0x%p);\n", REG_R(d.Rn), d.U?'+':'-', REG_R(d.Rm));

			if (d.W)
				WRITE_CODE("REG_W(0x%p) = adr;\n", REG_W(d.Rn));
		}
		else
		{
			WRITE_CODE("u32 adr = REG_R%s(0x%p);\n", REG_R(d.Rn));

			if (d.I)
				WRITE_CODE("REG_W(0x%p) = adr %c %u;\n", REG_W(d.Rn), d.Immediate, d.U?'+':'-');
			else
				WRITE_CODE("REG_W(0x%p) = adr %c REG_R%s(0x%p);\n", REG_W(d.Rn), d.U?'+':'-', REG_R(d.Rm));
		}

		WRITE_CODE("ExecuteCycles+=((u32 (FASTCALL *)(u32, u32*))0x%p)(adr,REGPTR(0x%p));\n", LDRD_Tab[PROCNUM][0], REGPTR(d.Rd));
	}

	template<u32 PROCNUM, u32 memtype, u32 cycle>
	static u32 FASTCALL MEMOP_STRD(u32 adr, u32 *srcreg)
	{
		WRITE32(GETCPU.mem_if->data, adr, *srcreg);
		WRITE32(GETCPU.mem_if->data, adr+4, *(srcreg+1));
		return MMU_aluMemCycles<PROCNUM>(cycle, MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr) + MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr+4));
	}

	static const MemOp1 STRD_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_STRD<0,MEMTYPE_GENERIC,3>,
			MEMOP_STRD<0,MEMTYPE_MAIN,3>,
			MEMOP_STRD<0,MEMTYPE_DTCM_ARM9,3>,
			MEMOP_STRD<0,MEMTYPE_GENERIC,3>,//MEMOP_STRD<0,MEMTYPE_ERAM_ARM7,3>,
			MEMOP_STRD<0,MEMTYPE_SWIRAM,3>,
		},
		{
			MEMOP_STRD<1,MEMTYPE_GENERIC,3>,
			MEMOP_STRD<1,MEMTYPE_MAIN,3>,
			MEMOP_STRD<1,MEMTYPE_GENERIC,3>,//MEMOP_STRD<1,MEMTYPE_DTCM_ARM9,3>,
			MEMOP_STRD<1,MEMTYPE_ERAM_ARM7,3>,
			MEMOP_STRD<1,MEMTYPE_SWIRAM,3>,
		}
	};

	OPCDECODER_DECL(IR_STRD)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.P)
		{
			if (d.I)
				WRITE_CODE("u32 adr = REG_R%s(0x%p) %c %u;\n", REG_R(d.Rn), d.U?'+':'-', d.Immediate);
			else
				WRITE_CODE("u32 adr = REG_R%s(0x%p) %c REG_R%s(0x%p);\n", REG_R(d.Rn), d.U?'+':'-', REG_R(d.Rm));

			if (d.W)
				WRITE_CODE("REG_W(0x%p) = adr;\n", REG_W(d.Rn));
		}
		else
		{
			WRITE_CODE("u32 adr = REG_R%s(0x%p);\n", REG_R(d.Rn));

			if (d.I)
				WRITE_CODE("REG_W(0x%p) = adr %c %u;\n", REG_W(d.Rn), d.Immediate, d.U?'+':'-');
			else
				WRITE_CODE("REG_W(0x%p) = adr %c REG_R%s(0x%p);\n", REG_W(d.Rn), d.U?'+':'-', REG_R(d.Rm));
		}

		if (d.Rd == 14)
			WRITE_CODE("REG_W(0x%p) = %u;\n", REG_W(15), d.CalcR15(d));

		WRITE_CODE("ExecuteCycles+=((u32 (FASTCALL *)(u32, u32*))0x%p)(adr,REGPTR(0x%p));\n", STRD_Tab[PROCNUM][0], REGPTR(d.Rd));
	}

	OPCDECODER_DECL(IR_LDREX)
	{
		u32 PROCNUM = d.ProcessID;

		WRITE_CODE("u32 adr = REG_R%s(0x%p);\n", REG_R(d.Rn));

		WRITE_CODE("ExecuteCycles+=((u32 (FASTCALL *)(u32, u32*))0x%p)(adr,REGPTR(0x%p));\n", LDR_Tab[PROCNUM][0], REGPTR(d.Rd));
	}

	OPCDECODER_DECL(IR_STREX)
	{
		u32 PROCNUM = d.ProcessID;

		WRITE_CODE("u32 adr = REG_R%s(0x%p);\n", REG_R(d.Rn));

		WRITE_CODE("ExecuteCycles+=((u32 (FASTCALL *)(u32, u32))0x%p)(adr,REG_R%s(0x%p));\n", STR_Tab[PROCNUM][0], REG_R(d.Rd));

		WRITE_CODE("REG_W(0x%p) = 0;\n", REG_W(d.Rm));
	}

	template<u32 PROCNUM, u32 memtype, u32 cycle, bool up>
	static u32 MEMOP_LDM_SEQUENCE(u32 adr, u32 count, u32 *regs)
	{
		u32 c = 0;
		u8 *ptr = _MMU_read_getrawptr32<PROCNUM, MMU_AT_DATA>(adr, adr+(count-1)*4);
		if (ptr)
		{
#ifdef WORDS_BIGENDIAN
			if (up)
			{
				for (u32 i = 0; i < count; i++)
				{
					regs[i] = T1ReadLong_guaranteedAligned(ptr, i * sizeof(u32));
					c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
					adr += 4;
				}
			}
			else
			{
				adr = adr+(count-1)*4;
				for (s32 i = (s32)count - 1; i >= 0; i--)
				{
					regs[i] = T1ReadLong_guaranteedAligned(ptr, i * sizeof(u32));
					c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
					adr -= 4;
				}
			}
#else
			memcpy(regs, ptr, sizeof(u32) * count);
			if (up)
			{
				for (u32 i = 0; i < count; i++)
				{
					c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
					adr += 4;
				}
			}
			else
			{
				adr = adr+(count-1)*4;
				for (s32 i = (s32)count - 1; i >= 0; i--)
				{
					c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
					adr -= 4;
				}
			}
#endif
		}
		else
		{
			if (up)
			{
				for (u32 i = 0; i < count; i++)
				{
					regs[i] = READ32(GETCPU.mem_if->data, adr);
					c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
					adr += 4;
				}
			}
			else
			{
				adr = adr+(count-1)*4;
				for (s32 i = (s32)count - 1; i >= 0; i--)
				{
					regs[i] = READ32(GETCPU.mem_if->data, adr);
					c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
					adr -= 4;
				}
			}
		}

		return MMU_aluMemCycles<PROCNUM>(cycle, c);
	}

	template<u32 PROCNUM, u32 memtype, u32 cycle, bool up>
	static u32 MEMOP_LDM(u32 adr, u32 count, u32 *regs_ptr)
	{
		u32 c = 0;
		u32 **regs = (u32 **)regs_ptr;
		u8 *ptr = _MMU_read_getrawptr32<PROCNUM, MMU_AT_DATA>(adr, adr+(count-1)*4);
		if (ptr)
		{
			if (up)
			{
				for (u32 i = 0; i < count; i++)
				{
					*(regs[i]) = T1ReadLong_guaranteedAligned(ptr, i * sizeof(u32));
					c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
					adr += 4;
				}
			}
			else
			{
				adr = adr+(count-1)*4;
				for (s32 i = (s32)count - 1; i >= 0; i--)
				{
					*(regs[i]) = T1ReadLong_guaranteedAligned(ptr, i * sizeof(u32));
					c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
					adr -= 4;
				}
			}
		}
		else
		{
			if (up)
			{
				for (u32 i = 0; i < count; i++)
				{
					*(regs[i]) = READ32(GETCPU.mem_if->data, adr);
					c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
					adr += 4;
				}
			}
			else
			{
				adr = adr+(count-1)*4;
				for (s32 i = (s32)count - 1; i >= 0; i--)
				{
					*(regs[i]) = READ32(GETCPU.mem_if->data, adr);
					c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr);
					adr -= 4;
				}
			}
		}

		return MMU_aluMemCycles<PROCNUM>(cycle, c);
	}

	static const MemOp3 LDM_SEQUENCE_Up_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_GENERIC,2,true>,
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_MAIN,2,true>,
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_DTCM_ARM9,2,true>,
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_GENERIC,2,true>,//MEMOP_LDM_SEQUENCE<0,MEMTYPE_ERAM_ARM7,2,true>,
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_SWIRAM,2,true>,
		},
		{
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_GENERIC,2,true>,
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_MAIN,2,true>,
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_GENERIC,2,true>,//MEMOP_LDM_SEQUENCE<1,MEMTYPE_DTCM_ARM9,2,true>,
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_ERAM_ARM7,2,true>,
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_SWIRAM,2,true>,
		}
	};

	static const MemOp3 LDM_Up_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_LDM<0,MEMTYPE_GENERIC,2,true>,
			MEMOP_LDM<0,MEMTYPE_MAIN,2,true>,
			MEMOP_LDM<0,MEMTYPE_DTCM_ARM9,2,true>,
			MEMOP_LDM<0,MEMTYPE_GENERIC,2,true>,//MEMOP_LDM<0,MEMTYPE_ERAM_ARM7,2,true>,
			MEMOP_LDM<0,MEMTYPE_SWIRAM,2,true>,
		},
		{
			MEMOP_LDM<1,MEMTYPE_GENERIC,2,true>,
			MEMOP_LDM<1,MEMTYPE_MAIN,2,true>,
			MEMOP_LDM<1,MEMTYPE_GENERIC,2,true>,//MEMOP_LDM<1,MEMTYPE_DTCM_ARM9,2,true>,
			MEMOP_LDM<1,MEMTYPE_ERAM_ARM7,2,true>,
			MEMOP_LDM<1,MEMTYPE_SWIRAM,2,true>,
		}
	};

	static const MemOp3 LDM_SEQUENCE_Up_R15_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_GENERIC,4,true>,
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_MAIN,4,true>,
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_DTCM_ARM9,4,true>,
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_GENERIC,4,true>,//MEMOP_LDM_SEQUENCE<0,MEMTYPE_ERAM_ARM7,4,true>,
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_SWIRAM,4,true>,
		},
		{
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_GENERIC,4,true>,
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_MAIN,4,true>,
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_GENERIC,4,true>,//MEMOP_LDM_SEQUENCE<1,MEMTYPE_DTCM_ARM9,4,true>,
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_ERAM_ARM7,4,true>,
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_SWIRAM,4,true>,
		}
	};

	static const MemOp3 LDM_Up_R15_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_LDM<0,MEMTYPE_GENERIC,4,true>,
			MEMOP_LDM<0,MEMTYPE_MAIN,4,true>,
			MEMOP_LDM<0,MEMTYPE_DTCM_ARM9,4,true>,
			MEMOP_LDM<0,MEMTYPE_GENERIC,4,true>,//MEMOP_LDM<0,MEMTYPE_ERAM_ARM7,4,true>,
			MEMOP_LDM<0,MEMTYPE_SWIRAM,4,true>,
		},
		{
			MEMOP_LDM<1,MEMTYPE_GENERIC,4,true>,
			MEMOP_LDM<1,MEMTYPE_MAIN,4,true>,
			MEMOP_LDM<1,MEMTYPE_GENERIC,4,true>,//MEMOP_LDM<1,MEMTYPE_DTCM_ARM9,4,true>,
			MEMOP_LDM<1,MEMTYPE_ERAM_ARM7,4,true>,
			MEMOP_LDM<1,MEMTYPE_SWIRAM,4,true>,
		}
	};

	static const MemOp3 LDM_SEQUENCE_Down_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_GENERIC,2,false>,
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_MAIN,2,false>,
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_DTCM_ARM9,2,false>,
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_GENERIC,2,false>,//MEMOP_LDM_SEQUENCE<0,MEMTYPE_ERAM_ARM7,2,false>,
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_SWIRAM,2,false>,
		},
		{
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_GENERIC,2,false>,
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_MAIN,2,false>,
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_GENERIC,2,false>,//MEMOP_LDM_SEQUENCE<1,MEMTYPE_DTCM_ARM9,2,false>,
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_ERAM_ARM7,2,false>,
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_SWIRAM,2,false>,
		}
	};

	static const MemOp3 LDM_Down_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_LDM<0,MEMTYPE_GENERIC,2,false>,
			MEMOP_LDM<0,MEMTYPE_MAIN,2,false>,
			MEMOP_LDM<0,MEMTYPE_DTCM_ARM9,2,false>,
			MEMOP_LDM<0,MEMTYPE_GENERIC,2,false>,//MEMOP_LDM<0,MEMTYPE_ERAM_ARM7,2,false>,
			MEMOP_LDM<0,MEMTYPE_SWIRAM,2,false>,
		},
		{
			MEMOP_LDM<1,MEMTYPE_GENERIC,2,false>,
			MEMOP_LDM<1,MEMTYPE_MAIN,2,false>,
			MEMOP_LDM<1,MEMTYPE_GENERIC,2,false>,//MEMOP_LDM<1,MEMTYPE_DTCM_ARM9,2,false>,
			MEMOP_LDM<1,MEMTYPE_ERAM_ARM7,2,false>,
			MEMOP_LDM<1,MEMTYPE_SWIRAM,2,false>,
		}
	};

	static const MemOp3 LDM_SEQUENCE_Down_R15_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_GENERIC,4,false>,
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_MAIN,4,false>,
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_DTCM_ARM9,4,false>,
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_GENERIC,4,false>,//MEMOP_LDM_SEQUENCE<0,MEMTYPE_ERAM_ARM7,4,false>,
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_SWIRAM,4,false>,
		},
		{
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_GENERIC,4,false>,
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_MAIN,4,false>,
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_GENERIC,4,false>,//MEMOP_LDM_SEQUENCE<1,MEMTYPE_DTCM_ARM9,4,false>,
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_ERAM_ARM7,4,false>,
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_SWIRAM,4,false>,
		}
	};

	static const MemOp3 LDM_Down_R15_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_LDM<0,MEMTYPE_GENERIC,4,false>,
			MEMOP_LDM<0,MEMTYPE_MAIN,4,false>,
			MEMOP_LDM<0,MEMTYPE_DTCM_ARM9,4,false>,
			MEMOP_LDM<0,MEMTYPE_GENERIC,4,false>,//MEMOP_LDM<0,MEMTYPE_ERAM_ARM7,4,false>,
			MEMOP_LDM<0,MEMTYPE_SWIRAM,4,false>,
		},
		{
			MEMOP_LDM<1,MEMTYPE_GENERIC,4,false>,
			MEMOP_LDM<1,MEMTYPE_MAIN,4,false>,
			MEMOP_LDM<1,MEMTYPE_GENERIC,4,false>,//MEMOP_LDM<1,MEMTYPE_DTCM_ARM9,4,false>,
			MEMOP_LDM<1,MEMTYPE_ERAM_ARM7,4,false>,
			MEMOP_LDM<1,MEMTYPE_SWIRAM,4,false>,
		}
	};

	OPCDECODER_DECL(IR_LDM)
	{
		u32 PROCNUM = d.ProcessID;

		u32 SequenceFlag = 0;//0:no sequence start,1:one sequence start,2:one sequence end,3:more than one sequence start
		u32 Count = 0;
		u32* Regs[16];
		for(u32 RegisterList = d.RegisterList, n = 0; RegisterList; RegisterList >>= 1, n++)
		{
			if (RegisterList & 0x1)
			{
				Regs[Count] = &GETCPU.R[n];
				Count++;

				if (SequenceFlag == 0)
					SequenceFlag = 1;
				else if (SequenceFlag == 2)
					SequenceFlag = 3;
			}
			else
			{
				if (SequenceFlag == 1)
					SequenceFlag = 2;
			}
		}

		bool NeedWriteBack = false;
		if (d.W)
		{
			if (d.RegisterList & (1 << d.Rn))
			{
				u32 bitList = (~((2 << d.Rn)-1)) & 0xFFFF;
				if (/*!d.S && */(d.RegisterList & bitList))
					NeedWriteBack = true;
			}
			else
				NeedWriteBack = true;
		}

		bool IsOneSequence = (SequenceFlag == 1 || SequenceFlag == 2);

		if (NeedWriteBack)
			WRITE_CODE("u32 adr_old = REG_R%s(0x%p);\n", REG_R(d.Rn));

		if (d.P)
			WRITE_CODE("u32 adr = (REG_R%s(0x%p) %c 4) & 0xFFFFFFFC;\n", REG_R(d.Rn), d.U?'+':'-');
		else
			WRITE_CODE("u32 adr = REG_R%s(0x%p) & 0xFFFFFFFC;\n", REG_R(d.Rn));

		if (d.S)
		{
			if (d.R15Modified)
			{
				//WRITE_CODE("((Status_Reg*)0x%p)->val=((Status_Reg*)0x%p)->val;\n", &(GETCPU.CPSR), &(GETCPU.SPSR));
			}
			else
				WRITE_CODE("u32 oldmode = ((u32 (*)(void*,u8))0x%p)((void*)0x%p,%u);\n", armcpu_switchMode, GETCPUPTR, SYS);
		}

		if (IsOneSequence)
		{
			if (d.U)
			{
				if (d.R15Modified)
					WRITE_CODE("ExecuteCycles+=((u32 (*)(u32, u32, u32*))0x%p)(adr, %u,(u32*)0x%p);\n", LDM_SEQUENCE_Up_R15_Tab[PROCNUM][0], Count, Regs[0]);
				else
					WRITE_CODE("ExecuteCycles+=((u32 (*)(u32, u32, u32*))0x%p)(adr, %u,(u32*)0x%p);\n", LDM_SEQUENCE_Up_Tab[PROCNUM][0], Count, Regs[0]);
			}
			else
			{
				if (d.R15Modified)
					WRITE_CODE("ExecuteCycles+=((u32 (*)(u32, u32, u32*))0x%p)(adr, %u,(u32*)0x%p);\n", LDM_SEQUENCE_Down_R15_Tab[PROCNUM][0], Count, Regs[0]);
				else
					WRITE_CODE("ExecuteCycles+=((u32 (*)(u32, u32, u32*))0x%p)(adr, %u,(u32*)0x%p);\n", LDM_SEQUENCE_Down_Tab[PROCNUM][0], Count, Regs[0]);
			}
		}
		else
		{
			WRITE_CODE("static const u32* Regs[]={");
			for (u32 i = 0; i < Count; i++)
			{
				WRITE_CODE("(u32*)0x%p", Regs[i]);
				if (i != Count - 1)
					WRITE_CODE(",");
			}
			WRITE_CODE("};\n");

			if (d.U)
			{
				if (d.R15Modified)
					WRITE_CODE("ExecuteCycles+=((u32 (*)(u32, u32, u32*))0x%p)(adr, %u,(u32*)&Regs[0]);\n", LDM_Up_R15_Tab[PROCNUM][0], Count);
				else
					WRITE_CODE("ExecuteCycles+=((u32 (*)(u32, u32, u32*))0x%p)(adr, %u,(u32*)&Regs[0]);\n", LDM_Up_Tab[PROCNUM][0], Count);
			}
			else
			{
				if (d.R15Modified)
					WRITE_CODE("ExecuteCycles+=((u32 (*)(u32, u32, u32*))0x%p)(adr, %u,(u32*)&Regs[0]);\n", LDM_Down_R15_Tab[PROCNUM][0], Count);
				else
					WRITE_CODE("ExecuteCycles+=((u32 (*)(u32, u32, u32*))0x%p)(adr, %u,(u32*)&Regs[0]);\n", LDM_Down_Tab[PROCNUM][0], Count);
			}
		}

		if (d.S)
		{
			if (NeedWriteBack)
				WRITE_CODE("REG_W(0x%p)=adr_old %c %u;\n", REG_W(d.Rn), d.U?'+':'-', Count*4);

			if (d.R15Modified)
			{
				LDM_S_LoadCPSRGenerate(d, szCodeBuffer);

				R15ModifiedGenerate(d, szCodeBuffer);
			}
			else
				WRITE_CODE("((u32 (*)(void*,u8))0x%p)((void*)0x%p,oldmode);\n", armcpu_switchMode, GETCPUPTR);
		}
		else
		{
			if (d.R15Modified)
			{
				if (PROCNUM == 0)
				{
					WRITE_CODE("((Status_Reg*)0x%p)->bits.T=BIT0(REG(0x%p));\n", &(GETCPU.CPSR), REG(15));
					WRITE_CODE("REG(0x%p)&=0xFFFFFFFE;\n", REG(15));
				}
				else
					WRITE_CODE("REG(0x%p)&=0xFFFFFFFC;\n", REG(15));
			}

			if (NeedWriteBack)
				WRITE_CODE("REG_W(0x%p)=adr_old %c %u;\n", REG_W(d.Rn), d.U?'+':'-', Count*4);

			if (d.R15Modified)
				R15ModifiedGenerate(d, szCodeBuffer);
		}
	}

	template<u32 PROCNUM, u32 memtype, u32 cycle, bool up>
	static u32 MEMOP_STM_SEQUENCE(u32 adr, u32 count, u32 *regs)
	{
		u32 c = 0;
		u8 *ptr = _MMU_write_getrawptr32<PROCNUM, MMU_AT_DATA>(adr, adr+(count-1)*4);
		if (ptr)
		{
#ifdef WORDS_BIGENDIAN
			if (up)
			{
				for (u32 i = 0; i < count; i++)
				{
					T1WriteLong(ptr, i * sizeof(u32), regs[i]);
					c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr);
					adr += 4;
				}
			}
			else
			{
				adr = adr+(count-1)*4;
				for (s32 i = (s32)count - 1; i >= 0; i--)
				{
					T1WriteLong(ptr, i * sizeof(u32), regs[i]);
					c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr);
					adr -= 4;
				}
			}
#else
			memcpy(ptr, regs, sizeof(u32) * count);
			if (up)
			{
				for (u32 i = 0; i < count; i++)
				{
					c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr);
					adr += 4;
				}
			}
			else
			{
				adr = adr+(count-1)*4;
				for (s32 i = (s32)count - 1; i >= 0; i--)
				{
					c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr);
					adr -= 4;
				}
			}
#endif
		}
		else
		{
			if (up)
			{
				for (u32 i = 0; i < count; i++)
				{
					WRITE32(GETCPU.mem_if->data, adr, regs[i]);
					c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr);
					adr += 4;
				}
			}
			else
			{
				adr = adr+(count-1)*4;
				for (s32 i = (s32)count - 1; i >= 0; i--)
				{
					WRITE32(GETCPU.mem_if->data, adr, regs[i]);
					c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr);
					adr -= 4;
				}
			}
		}

		return MMU_aluMemCycles<PROCNUM>(cycle, c);
	}

	template<u32 PROCNUM, u32 memtype, u32 cycle, bool up>
	static u32 MEMOP_STM(u32 adr, u32 count, u32 *regs_ptr)
	{
		u32 c = 0;
		u32 **regs = (u32 **)regs_ptr;
		u8 *ptr = _MMU_write_getrawptr32<PROCNUM, MMU_AT_DATA>(adr, adr+(count-1)*4);
		if (ptr)
		{
			if (up)
			{
				for (u32 i = 0; i < count; i++)
				{
					T1WriteLong(ptr, i * sizeof(u32), *(regs[i]));
					c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr);
					adr += 4;
				}
			}
			else
			{
				adr = adr+(count-1)*4;
				for (s32 i = (s32)count - 1; i >= 0; i--)
				{
					T1WriteLong(ptr, i * sizeof(u32), *(regs[i]));
					c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr);
					adr -= 4;
				}
			}
		}
		else
		{
			if (up)
			{
				for (u32 i = 0; i < count; i++)
				{
					WRITE32(GETCPU.mem_if->data, adr, *(regs[i]));
					c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr);
					adr += 4;
				}
			}
			else
			{
				adr = adr+(count-1)*4;
				for (s32 i = (s32)count - 1; i >= 0; i--)
				{
					WRITE32(GETCPU.mem_if->data, adr, *(regs[i]));
					c += MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr);
					adr -= 4;
				}
			}
		}

		return MMU_aluMemCycles<PROCNUM>(cycle, c);
	}

	static const MemOp3 STM_SEQUENCE_Up_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_STM_SEQUENCE<0,MEMTYPE_GENERIC,1,true>,
			MEMOP_STM_SEQUENCE<0,MEMTYPE_MAIN,1,true>,
			MEMOP_STM_SEQUENCE<0,MEMTYPE_DTCM_ARM9,1,true>,
			MEMOP_STM_SEQUENCE<0,MEMTYPE_GENERIC,1,true>,//MEMOP_STM_SEQUENCE<0,MEMTYPE_ERAM_ARM7,1,true>,
			MEMOP_STM_SEQUENCE<0,MEMTYPE_SWIRAM,1,true>,
		},
		{
			MEMOP_STM_SEQUENCE<1,MEMTYPE_GENERIC,1,true>,
			MEMOP_STM_SEQUENCE<1,MEMTYPE_MAIN,1,true>,
			MEMOP_STM_SEQUENCE<1,MEMTYPE_GENERIC,1,true>,//MEMOP_STM_SEQUENCE<1,MEMTYPE_DTCM_ARM9,1,true>,
			MEMOP_STM_SEQUENCE<1,MEMTYPE_ERAM_ARM7,1,true>,
			MEMOP_STM_SEQUENCE<1,MEMTYPE_SWIRAM,1,true>,
		}
	};

	static const MemOp3 STM_Up_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_STM<0,MEMTYPE_GENERIC,1,true>,
			MEMOP_STM<0,MEMTYPE_MAIN,1,true>,
			MEMOP_STM<0,MEMTYPE_DTCM_ARM9,1,true>,
			MEMOP_STM<0,MEMTYPE_GENERIC,1,true>,//MEMOP_STM<0,MEMTYPE_ERAM_ARM7,1,true>,
			MEMOP_STM<0,MEMTYPE_SWIRAM,1,true>,
		},
		{
			MEMOP_STM<1,MEMTYPE_GENERIC,1,true>,
			MEMOP_STM<1,MEMTYPE_MAIN,1,true>,
			MEMOP_STM<1,MEMTYPE_GENERIC,1,true>,//MEMOP_STM<1,MEMTYPE_DTCM_ARM9,1,true>,
			MEMOP_STM<1,MEMTYPE_ERAM_ARM7,1,true>,
			MEMOP_STM<1,MEMTYPE_SWIRAM,1,true>,
		}
	};

	static const MemOp3 STM_SEQUENCE_Down_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_STM_SEQUENCE<0,MEMTYPE_GENERIC,1,false>,
			MEMOP_STM_SEQUENCE<0,MEMTYPE_MAIN,1,false>,
			MEMOP_STM_SEQUENCE<0,MEMTYPE_DTCM_ARM9,1,false>,
			MEMOP_STM_SEQUENCE<0,MEMTYPE_GENERIC,1,false>,//MEMOP_STM_SEQUENCE<0,MEMTYPE_ERAM_ARM7,1,false>,
			MEMOP_STM_SEQUENCE<0,MEMTYPE_SWIRAM,1,false>,
		},
		{
			MEMOP_STM_SEQUENCE<1,MEMTYPE_GENERIC,1,false>,
			MEMOP_STM_SEQUENCE<1,MEMTYPE_MAIN,1,false>,
			MEMOP_STM_SEQUENCE<1,MEMTYPE_GENERIC,1,false>,//MEMOP_STM_SEQUENCE<1,MEMTYPE_DTCM_ARM9,1,false>,
			MEMOP_STM_SEQUENCE<1,MEMTYPE_ERAM_ARM7,1,false>,
			MEMOP_STM_SEQUENCE<1,MEMTYPE_SWIRAM,1,false>,
		}
	};

	static const MemOp3 STM_Down_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_STM<0,MEMTYPE_GENERIC,1,false>,
			MEMOP_STM<0,MEMTYPE_MAIN,1,false>,
			MEMOP_STM<0,MEMTYPE_DTCM_ARM9,1,false>,
			MEMOP_STM<0,MEMTYPE_GENERIC,1,false>,//MEMOP_STM<0,MEMTYPE_ERAM_ARM7,1,false>,
			MEMOP_STM<0,MEMTYPE_SWIRAM,1,false>,
		},
		{
			MEMOP_STM<1,MEMTYPE_GENERIC,1,false>,
			MEMOP_STM<1,MEMTYPE_MAIN,1,false>,
			MEMOP_STM<1,MEMTYPE_GENERIC,1,false>,//MEMOP_STM<1,MEMTYPE_DTCM_ARM9,1,false>,
			MEMOP_STM<1,MEMTYPE_ERAM_ARM7,1,false>,
			MEMOP_STM<1,MEMTYPE_SWIRAM,1,false>,
		}
	};

	OPCDECODER_DECL(IR_STM)
	{
		u32 PROCNUM = d.ProcessID;

		bool StoreR15 = false;
		u32 SequenceFlag = 0;//0:no sequence start,1:one sequence start,2:one sequence end,3:more than one sequence start
		u32 Count = 0;
		u32* Regs[16];
		for(u32 RegisterList = d.RegisterList, n = 0; RegisterList; RegisterList >>= 1, n++)
		{
			if (RegisterList & 0x1)
			{
				Regs[Count] = &GETCPU.R[n];
				Count++;

				if (n == 15)
					StoreR15 = true;

				if (SequenceFlag == 0)
					SequenceFlag = 1;
				else if (SequenceFlag == 2)
					SequenceFlag = 3;
			}
			else
			{
				if (SequenceFlag == 1)
					SequenceFlag = 2;
			}
		}

		if (d.S)
			WRITE_CODE("if (((Status_Reg*)0x%p)->bits.mode!=%u){\n", &(GETCPU.CPSR), USR);

		if (StoreR15)
			WRITE_CODE("REG_W(0x%p) = %u;\n", REG_W(15), d.CalcR15(d));

		bool IsOneSequence = (SequenceFlag == 1 || SequenceFlag == 2);

		if (d.W)
			WRITE_CODE("u32 adr_old = REG_R%s(0x%p);\n", REG_R(d.Rn));

		if (d.P)
			WRITE_CODE("u32 adr = (REG_R%s(0x%p) %c 4) & 0xFFFFFFFC;\n", REG_R(d.Rn), d.U?'+':'-');
		else
			WRITE_CODE("u32 adr = REG_R%s(0x%p) & 0xFFFFFFFC;\n", REG_R(d.Rn));

		if (d.S)
			WRITE_CODE("u32 oldmode = ((u32 (*)(void*,u8))0x%p)((void*)0x%p,%u);\n", armcpu_switchMode, GETCPUPTR, SYS);

		if (IsOneSequence)
		{
			if (d.U)
				WRITE_CODE("ExecuteCycles+=((u32 (*)(u32, u32, u32*))0x%p)(adr, %u,(u32*)0x%p);\n", STM_SEQUENCE_Up_Tab[PROCNUM][0], Count, Regs[0]);
			else
				WRITE_CODE("ExecuteCycles+=((u32 (*)(u32, u32, u32*))0x%p)(adr, %u,(u32*)0x%p);\n", STM_SEQUENCE_Down_Tab[PROCNUM][0], Count, Regs[0]);
		}
		else
		{
			WRITE_CODE("static const u32* Regs[]={");
			for (u32 i = 0; i < Count; i++)
			{
				WRITE_CODE("(u32*)0x%p", Regs[i]);
				if (i != Count - 1)
					WRITE_CODE(",");
			}
			WRITE_CODE("};\n");

			if (d.U)
				WRITE_CODE("ExecuteCycles+=((u32 (*)(u32, u32, u32*))0x%p)(adr, %u,(u32*)&Regs[0]);\n", STM_Up_Tab[PROCNUM][0], Count);
			else
				WRITE_CODE("ExecuteCycles+=((u32 (*)(u32, u32, u32*))0x%p)(adr, %u,(u32*)&Regs[0]);\n", STM_Down_Tab[PROCNUM][0], Count);
		}

		if (d.S)
		{
			if (d.W)
				WRITE_CODE("REG_W(0x%p)=adr_old %c %u;\n", REG_W(d.Rn), d.U?'+':'-', Count*4);

			WRITE_CODE("((u32 (*)(void*,u8))0x%p)((void*)0x%p,oldmode);\n", armcpu_switchMode, GETCPUPTR);

			WRITE_CODE("}else ExecuteCycles+=2;\n");
		}
		else
		{
			if (d.W)
				WRITE_CODE("REG_W(0x%p)=adr_old %c %u;\n", REG_W(d.Rn), d.U?'+':'-', Count*4);
		}
	}

	template<u32 PROCNUM, u32 memtype, u32 cycle>
	static u32 MEMOP_SWP(u32 adr, u32 *Rd, u32 Rm)
	{
		u32 tmp = ROR(READ32(GETCPU.mem_if->data, adr), (adr & 3)<<3);
		WRITE32(GETCPU.mem_if->data, adr, Rm);
		*Rd = tmp;

		return MMU_aluMemCycles<PROCNUM>(cycle, MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr) + MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr));
	}

	template<u32 PROCNUM, u32 memtype, u32 cycle>
	static u32 MEMOP_SWPB(u32 adr, u32 *Rd, u32 Rm)
	{
		u32 tmp = READ8(GETCPU.mem_if->data, adr);
		WRITE8(GETCPU.mem_if->data, adr, Rm);
		*Rd = tmp;

		return MMU_aluMemCycles<PROCNUM>(cycle, MMU_memAccessCycles<PROCNUM,8,MMU_AD_READ>(adr) + MMU_memAccessCycles<PROCNUM,8,MMU_AD_WRITE>(adr));
	}

	static const MemOp4 SWP_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_SWP<0,MEMTYPE_GENERIC,4>,
			MEMOP_SWP<0,MEMTYPE_MAIN,4>,
			MEMOP_SWP<0,MEMTYPE_DTCM_ARM9,4>,
			MEMOP_SWP<0,MEMTYPE_GENERIC,4>,//MEMOP_SWP<0,MEMTYPE_ERAM_ARM7,4>,
			MEMOP_SWP<0,MEMTYPE_SWIRAM,4>,
		},
		{
			MEMOP_SWP<1,MEMTYPE_GENERIC,4>,
			MEMOP_SWP<1,MEMTYPE_MAIN,4>,
			MEMOP_SWP<1,MEMTYPE_GENERIC,4>,//MEMOP_SWP<1,MEMTYPE_DTCM_ARM9,4>,
			MEMOP_SWP<1,MEMTYPE_ERAM_ARM7,4>,
			MEMOP_SWP<1,MEMTYPE_SWIRAM,4>,
		}
	};

	static const MemOp4 SWPB_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_SWPB<0,MEMTYPE_GENERIC,4>,
			MEMOP_SWPB<0,MEMTYPE_MAIN,4>,
			MEMOP_SWPB<0,MEMTYPE_DTCM_ARM9,4>,
			MEMOP_SWPB<0,MEMTYPE_GENERIC,4>,//MEMOP_SWPB<0,MEMTYPE_ERAM_ARM7,4>,
			MEMOP_SWPB<0,MEMTYPE_SWIRAM,4>,
		},
		{
			MEMOP_SWPB<1,MEMTYPE_GENERIC,4>,
			MEMOP_SWPB<1,MEMTYPE_MAIN,4>,
			MEMOP_SWPB<1,MEMTYPE_GENERIC,4>,//MEMOP_SWPB<1,MEMTYPE_DTCM_ARM9,4>,
			MEMOP_SWPB<1,MEMTYPE_ERAM_ARM7,4>,
			MEMOP_SWPB<1,MEMTYPE_SWIRAM,4>,
		}
	};

	OPCDECODER_DECL(IR_SWP)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.B)
			WRITE_CODE("ExecuteCycles+=((u32 (*)(u32, u32*, u32))0x%p)(REG_R%s(0x%p),REGPTR(0x%p),REG_R%s(0x%p));\n", SWPB_Tab[PROCNUM][0], REG_R(d.Rn), REGPTR(d.Rd), REG_R(d.Rm));
		else
			WRITE_CODE("ExecuteCycles+=((u32 (*)(u32, u32*, u32))0x%p)(REG_R%s(0x%p),REGPTR(0x%p),REG_R%s(0x%p));\n", SWP_Tab[PROCNUM][0], REG_R(d.Rn), REGPTR(d.Rd), REG_R(d.Rm));
	}

	OPCDECODER_DECL(IR_B)
	{
		u32 PROCNUM = d.ProcessID;

		WRITE_CODE("REG_W(0x%p)=%u;\n", REG_W(15), d.Immediate);

		R15ModifiedGenerate(d, szCodeBuffer);
	}

	OPCDECODER_DECL(IR_BL)
	{
		u32 PROCNUM = d.ProcessID;

		WRITE_CODE("REG_W(0x%p)=%u;\n", REG_W(14), d.CalcNextInstruction(d) | d.ThumbFlag);
		WRITE_CODE("REG_W(0x%p)=%u;\n", REG_W(15), d.Immediate);

		R15ModifiedGenerate(d, szCodeBuffer);
	}

	OPCDECODER_DECL(IR_BX)
	{
		u32 PROCNUM = d.ProcessID;

		WRITE_CODE("u32 tmp = REG_R%s(0x%p);\n", REG_R(d.Rn));

		WRITE_CODE("((Status_Reg*)0x%p)->bits.T=BIT0(tmp);\n", &(GETCPU.CPSR));
		WRITE_CODE("REG_W(0x%p)=tmp & (0xFFFFFFFC|(BIT0(tmp)<<1));\n", REG_W(15));

		R15ModifiedGenerate(d, szCodeBuffer);
	}

	OPCDECODER_DECL(IR_BLX)
	{
		u32 PROCNUM = d.ProcessID;

		WRITE_CODE("u32 tmp = REG_R%s(0x%p);\n", REG_R(d.Rn));

		WRITE_CODE("REG_W(0x%p)=%u;\n", REG_W(14), d.CalcNextInstruction(d) | d.ThumbFlag);
		WRITE_CODE("((Status_Reg*)0x%p)->bits.T=BIT0(tmp);\n", &(GETCPU.CPSR));
		WRITE_CODE("REG_W(0x%p)=tmp & (0xFFFFFFFC|(BIT0(tmp)<<1));\n", REG_W(15));

		R15ModifiedGenerate(d, szCodeBuffer);
	}

	OPCDECODER_DECL(IR_SWI)
	{
		u32 PROCNUM = d.ProcessID;

		if (GETCPU.swi_tab)
		{
			if (PROCNUM == 0)
				WRITE_CODE("if ((*(u32*)0x%p) != 0x00000000){\n", &(GETCPU.intVector));
			else
				WRITE_CODE("if ((*(u32*)0x%p) != 0xFFFF0000){\n", &(GETCPU.intVector));

			if (d.MayHalt)
			{
				if (d.ThumbFlag)
				{
					WRITE_CODE("(*(u32*)0x%p) = %u;\n", &(GETCPU.instruct_adr), d.Address);
					//WRITE_CODE("(*(u32*)0x%p) = %u;\n", &(GETCPU.next_instruction), d.CalcNextInstruction(d));
					// alway set r15 to next_instruction
					WRITE_CODE("REG_W(0x%p) = %u;\n", REG_W(15), d.CalcNextInstruction(d));
				}
				else
				{
					WRITE_CODE("(*(u32*)0x%p) = %u;\n", &(GETCPU.instruct_adr), d.Address);
					//WRITE_CODE("(*(u32*)0x%p) = %u;\n", &(GETCPU.next_instruction), d.CalcNextInstruction(d));
					// alway set r15 to next_instruction
					WRITE_CODE("REG_W(0x%p) = %u;\n", REG_W(15), d.CalcNextInstruction(d));
				}
			}

			WRITE_CODE("ExecuteCycles+=((u32 (*)())0x%p)()+3;\n", GETCPU.swi_tab[d.Immediate]);

			if (d.MayHalt)
				R15ModifiedGenerate(d, szCodeBuffer);

			WRITE_CODE("}else{\n");
		}
		
		{
			WRITE_CODE("Status_Reg tmp;\n");
			WRITE_CODE("tmp.val = ((Status_Reg*)0x%p)->val;\n", &(GETCPU.CPSR));
			WRITE_CODE("((u32 (*)(void*,u8))0x%p)((void*)0x%p,%u);\n", armcpu_switchMode, GETCPUPTR, SVC);
			WRITE_CODE("REG_W(0x%p)=%u;\n", REG_W(14), d.CalcNextInstruction(d));
			WRITE_CODE("((Status_Reg*)0x%p)->val = tmp.val;\n", &(GETCPU.SPSR));
			WRITE_CODE("((Status_Reg*)0x%p)->bits.T=0;\n", &(GETCPU.CPSR));
			WRITE_CODE("((Status_Reg*)0x%p)->bits.I=1;\n", &(GETCPU.CPSR));
			WRITE_CODE("((void (*)(void*))0x%p)((void*)0x%p);\n", armcpu_changeCPSR, GETCPUPTR);
			WRITE_CODE("REG_W(0x%p)= (*(u32*)0x%p) + 0x08;\n", REG_W(15), &(GETCPU.intVector));

			WRITE_CODE("ExecuteCycles+=3;\n");

			R15ModifiedGenerate(d, szCodeBuffer);
		}

		if (GETCPU.swi_tab)
			WRITE_CODE("}\n");
	}

	OPCDECODER_DECL(IR_MSR)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.P)
		{
			u32 byte_mask = (BIT0(d.OpData)?0x000000FF:0x00000000) |
							(BIT1(d.OpData)?0x0000FF00:0x00000000) |
							(BIT2(d.OpData)?0x00FF0000:0x00000000) |
							(BIT3(d.OpData)?0xFF000000:0x00000000);

			WRITE_CODE("if(((Status_Reg*)0x%p)->bits.mode!=%u&&((Status_Reg*)0x%p)->bits.mode!=%u){\n",
					&(GETCPU.CPSR), USR, &(GETCPU.CPSR), SYS);
			if (d.I)
				WRITE_CODE("(*(u32*)0x%p) = ((*(u32*)0x%p) & %u) | %u);\n", 
						&(GETCPU.SPSR.val), &(GETCPU.SPSR.val), ~byte_mask, byte_mask & d.Immediate);
			else
				WRITE_CODE("(*(u32*)0x%p) = ((*(u32*)0x%p) & %u) | (REG_R%s(0x%p) & %u);\n", 
						&(GETCPU.SPSR.val), &(GETCPU.SPSR.val), ~byte_mask, REG_R(d.Rm), byte_mask);
			WRITE_CODE("((void (*)(void*))0x%p)((void*)0x%p);\n", armcpu_changeCPSR, GETCPUPTR);
			WRITE_CODE("}\n");
		}
		else
		{
			u32 byte_mask_usr = (BIT3(d.OpData)?0xFF000000:0x00000000);
			u32 byte_mask_other = (BIT0(d.OpData)?0x000000FF:0x00000000) |
								(BIT1(d.OpData)?0x0000FF00:0x00000000) |
								(BIT2(d.OpData)?0x00FF0000:0x00000000) |
								(BIT3(d.OpData)?0xFF000000:0x00000000);

			WRITE_CODE("u32 byte_mask=(((Status_Reg*)0x%p)->bits.mode==%u)?%u:%u;\n", 
					&(GETCPU.CPSR), USR, byte_mask_usr, byte_mask_other);

			if (BIT0(d.OpData))
			{
				WRITE_CODE("if(((Status_Reg*)0x%p)->bits.mode!=%u){\n", &(GETCPU.CPSR), USR);
				if (d.I)
					WRITE_CODE("((u32 (*)(void*,u8))0x%p)((void*)0x%p,%u);\n", armcpu_switchMode, GETCPUPTR, d.Immediate & 0x1F);
				else
					WRITE_CODE("((u32 (*)(void*,u8))0x%p)((void*)0x%p,REG_R%s(0x%p)&0x1F);\n", armcpu_switchMode, GETCPUPTR, REG_R(d.Rm));
				WRITE_CODE("}\n");
			}

			if (d.I)
				WRITE_CODE("(*(u32*)0x%p) = ((*(u32*)0x%p) & ~byte_mask) | (%u & byte_mask);\n", 
						&(GETCPU.CPSR.val), &(GETCPU.CPSR.val), d.Immediate);
			else
				WRITE_CODE("(*(u32*)0x%p)=((*(u32*)0x%p)&~byte_mask)|(REG_R%s(0x%p)&byte_mask);\n", 
						&(GETCPU.CPSR.val), &(GETCPU.CPSR.val), REG_R(d.Rm));
			WRITE_CODE("((void (*)(void*))0x%p)((void*)0x%p);\n", armcpu_changeCPSR, GETCPUPTR);
		}
	}

	OPCDECODER_DECL(IR_MRS)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.P)
			WRITE_CODE("REG_W(0x%p)= (*(u32*)0x%p);\n", REG_W(d.Rd), &(GETCPU.SPSR.val));
		else
			WRITE_CODE("REG_W(0x%p)= (*(u32*)0x%p);\n", REG_W(d.Rd), &(GETCPU.CPSR.val));
	}

	OPCDECODER_DECL(IR_MCR)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.CPNum == 15)
		{
			WRITE_CODE("((BOOL (*)(u32,u8,u8,u8,u8))0x%p)(REG_R%s(0x%p),%u,%u,%u,%u);\n", 
					armcp15_moveARM2CP, REG_R(d.Rd), d.CRn, d.CRm, d.CPOpc, d.CP);
		}
		else
		{
			INFO("ARM%c: MCR P%i, 0, R%i, C%i, C%i, %i, %i (don't allocated coprocessor)\n", 
				PROCNUM?'7':'9', d.CPNum, d.Rd, d.CRn, d.CRm, d.CPOpc, d.CP);
		}
	}

	OPCDECODER_DECL(IR_MRC)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.CPNum == 15)
		{
			if (d.Rd == 15)
			{
				WRITE_CODE("u32 data = 0;\n");
				WRITE_CODE("((BOOL (*)(u32*,u8,u8,u8,u8))0x%p)(&data,%u,%u,%u,%u);\n", 
						armcp15_moveCP2ARM, d.CRn, d.CRm, d.CPOpc, d.CP);
				WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(data);\n", &(GETCPU.CPSR));
				WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=BIT30(data);\n", &(GETCPU.CPSR));
				WRITE_CODE("((Status_Reg*)0x%p)->bits.C=BIT29(data);\n", &(GETCPU.CPSR));
				WRITE_CODE("((Status_Reg*)0x%p)->bits.V=BIT28(data);\n", &(GETCPU.CPSR));
			}
			else
			{
				WRITE_CODE("((BOOL (*)(u32*,u8,u8,u8,u8))0x%p)(REGPTR(0x%p),%u,%u,%u,%u);\n", 
						armcp15_moveCP2ARM, REGPTR(d.Rd), d.CRn, d.CRm, d.CPOpc, d.CP);
			}
		}
		else
		{
			INFO("ARM%c: MRC P%i, 0, R%i, C%i, C%i, %i, %i (don't allocated coprocessor)\n", 
				PROCNUM?'7':'9', d.CPNum, d.Rd, d.CRn, d.CRm, d.CPOpc, d.CP);
		}
	}

	static const u8 CLZ_TAB[16]=
	{
		0,							// 0000
		1,							// 0001
		2, 2,						// 001X
		3, 3, 3, 3,					// 01XX
		4, 4, 4, 4, 4, 4, 4, 4		// 1XXX
	};

	OPCDECODER_DECL(IR_CLZ)
	{
		u32 PROCNUM = d.ProcessID;

		WRITE_CODE("u32 Rm = REG_R%s(0x%p);\n", REG_R(d.Rm));
		WRITE_CODE("if(Rm==0){\n");
		WRITE_CODE("REG_W(0x%p)=32;\n", REG_W(d.Rd));
		WRITE_CODE("}else{\n");
		WRITE_CODE("Rm |= (Rm >>1);\n");
		WRITE_CODE("Rm |= (Rm >>2);\n");
		WRITE_CODE("Rm |= (Rm >>4);\n");
		WRITE_CODE("Rm |= (Rm >>8);\n");
		WRITE_CODE("Rm |= (Rm >>16);\n");
		WRITE_CODE("static const u8* CLZ_TAB = (u8*)0x%p;\n", CLZ_TAB);
		WRITE_CODE("u32 pos = CLZ_TAB[Rm&0xF] + \n");
		WRITE_CODE("			CLZ_TAB[(Rm>>4)&0xF] + \n");
		WRITE_CODE("			CLZ_TAB[(Rm>>8)&0xF] + \n");
		WRITE_CODE("			CLZ_TAB[(Rm>>12)&0xF] + \n");
		WRITE_CODE("			CLZ_TAB[(Rm>>16)&0xF] + \n");
		WRITE_CODE("			CLZ_TAB[(Rm>>20)&0xF] + \n");
		WRITE_CODE("			CLZ_TAB[(Rm>>24)&0xF] + \n");
		WRITE_CODE("			CLZ_TAB[(Rm>>28)&0xF];\n");
		WRITE_CODE("REG_W(0x%p)=32-pos;}\n", REG_W(d.Rd));
	}

	OPCDECODER_DECL(IR_QADD)
	{
		u32 PROCNUM = d.ProcessID;

		WRITE_CODE("u32 res = REG_R%s(0x%p) + REG_R%s(0x%p);\n", REG_R(d.Rn), REG_R(d.Rm));
		WRITE_CODE("if(SIGNED_OVERFLOW(REG_R%s(0x%p),REG_R%s(0x%p),res)){\n", 
				REG_R(d.Rn), REG_R(d.Rm));
		WRITE_CODE("((Status_Reg*)0x%p)->bits.Q=1;\n", &(GETCPU.CPSR));
		WRITE_CODE("REG_W(0x%p)=0x80000000-BIT31(res);\n", REG_W(d.Rd));
		WRITE_CODE("}else{\n");
		if (d.R15Modified)
		{
			WRITE_CODE("REG_W(0x%p)=res & 0xFFFFFFFC;\n", REG_W(d.Rd));
			R15ModifiedGenerate(d, szCodeBuffer);
		}
		else
			WRITE_CODE("REG_W(0x%p)=res;\n", REG_W(d.Rd));
		WRITE_CODE("}\n");
	}

	OPCDECODER_DECL(IR_QSUB)
	{
		u32 PROCNUM = d.ProcessID;

		WRITE_CODE("u32 res = REG_R%s(0x%p) - REG_R%s(0x%p);\n", REG_R(d.Rm), REG_R(d.Rn));
		WRITE_CODE("if(SIGNED_UNDERFLOW(REG_R%s(0x%p),REG_R%s(0x%p),res)){\n", 
				REG_R(d.Rm), REG_R(d.Rn));
		WRITE_CODE("((Status_Reg*)0x%p)->bits.Q=1;\n", &(GETCPU.CPSR));
		WRITE_CODE("REG_W(0x%p)=0x80000000-BIT31(res);\n", REG_W(d.Rd));
		WRITE_CODE("}else{\n");
		if (d.R15Modified)
		{
			WRITE_CODE("REG_W(0x%p)=res & 0xFFFFFFFC;\n", REG_W(d.Rd));
			R15ModifiedGenerate(d, szCodeBuffer);
		}
		else
			WRITE_CODE("REG_W(0x%p)=res;\n", REG_W(d.Rd));
		WRITE_CODE("}\n");
	}

	OPCDECODER_DECL(IR_QDADD)
	{
		u32 PROCNUM = d.ProcessID;

		WRITE_CODE("u32 mul = REG_R%s(0x%p)<<1;\n", REG_R(d.Rn));
		WRITE_CODE("if(BIT31(REG_R%s(0x%p))!=BIT31(mul)){\n", REG_R(d.Rn));
		WRITE_CODE("((Status_Reg*)0x%p)->bits.Q=1;\n", &(GETCPU.CPSR));
		WRITE_CODE("REG_W(0x%p)=0x80000000-BIT31(res);\n", REG_W(d.Rd));
		WRITE_CODE("}\n");
		WRITE_CODE("u32 res = mul + REG_R%s(0x%p);\n", REG_R(d.Rm));
		WRITE_CODE("if(SIGNED_OVERFLOW(REG_R%s(0x%p),mul, res)){\n", REG_R(d.Rm));
		WRITE_CODE("((Status_Reg*)0x%p)->bits.Q=1;\n", &(GETCPU.CPSR));
		WRITE_CODE("REG_W(0x%p)=0x80000000-BIT31(res);\n", REG_W(d.Rd));
		WRITE_CODE("}else{\n");
		if (d.R15Modified)
		{
			WRITE_CODE("REG_W(0x%p)=res & 0xFFFFFFFC;\n", REG_W(d.Rd));
			R15ModifiedGenerate(d, szCodeBuffer);
		}
		else
			WRITE_CODE("REG_W(0x%p)=res;\n", REG_W(d.Rd));
		WRITE_CODE("}\n");
	}

	OPCDECODER_DECL(IR_QDSUB)
	{
		u32 PROCNUM = d.ProcessID;

		WRITE_CODE("u32 mul = REG_R%s(0x%p)<<1;\n", REG_R(d.Rn));
		WRITE_CODE("if(BIT31(REG_R%s(0x%p))!=BIT31(mul)){\n", REG_R(d.Rn));
		WRITE_CODE("((Status_Reg*)0x%p)->bits.Q=1;\n", &(GETCPU.CPSR));
		WRITE_CODE("REG_W(0x%p)=0x80000000-BIT31(res);\n", REG_W(d.Rd));
		WRITE_CODE("}\n");
		WRITE_CODE("u32 res = REG_R%s(0x%p) - mul;\n", REG_R(d.Rm));
		WRITE_CODE("if(SIGNED_UNDERFLOW(REG_R%s(0x%p),mul, res)){\n", REG_R(d.Rm));
		WRITE_CODE("((Status_Reg*)0x%p)->bits.Q=1;\n", &(GETCPU.CPSR));
		WRITE_CODE("REG_W(0x%p)=0x80000000-BIT31(res);\n", REG_W(d.Rd));
		WRITE_CODE("}else{\n");
		if (d.R15Modified)
		{
			WRITE_CODE("REG_W(0x%p)=res & 0xFFFFFFFC;\n", REG_W(d.Rd));
			R15ModifiedGenerate(d, szCodeBuffer);
		}
		else
			WRITE_CODE("REG_W(0x%p)=res;\n", REG_W(d.Rd));
		WRITE_CODE("}\n");
	}

	OPCDECODER_DECL(IR_BLX_IMM)
	{
		u32 PROCNUM = d.ProcessID;

		if(d.ThumbFlag)
			WRITE_CODE("((Status_Reg*)0x%p)->bits.T=0;\n", &(GETCPU.CPSR));
		else
			WRITE_CODE("((Status_Reg*)0x%p)->bits.T=1;\n", &(GETCPU.CPSR));

		WRITE_CODE("REG_W(0x%p)=%u;\n", REG_W(14), d.CalcNextInstruction(d) | d.ThumbFlag);
		WRITE_CODE("REG_W(0x%p)=%u;\n", REG_W(15), d.Immediate);

		R15ModifiedGenerate(d, szCodeBuffer);
	}

	OPCDECODER_DECL(IR_BKPT)
	{
		u32 PROCNUM = d.ProcessID;

		INFO("ARM%c: Unimplemented opcode BKPT\n", PROCNUM?'7':'9');
	}
};

static const IROpCDecoder iropcdecoder_set[IR_MAXNUM] = {
#define TABDECL(x) ArmCJit::x##_CDecoder
#include "ArmAnalyze_tabdef.inc"
#undef TABDECL
};

////////////////////////////////////////////////////////////////////
template<u32 PROCNUM, bool thumb>
static u32 FASTCALL RunInterpreter(const Decoded &d)
{
	u32 cycles;

	GETCPU.next_instruction = d.CalcNextInstruction(d);
	GETCPU.R[15] = d.CalcR15(d);
	if (thumb)
	{
		u32 opcode = d.Instruction.ThumbOp;
		cycles = thumb_instructions_set[PROCNUM][opcode>>6](opcode);
	}
	else
	{
		u32 opcode = d.Instruction.ArmOp;
		if(CONDITION(opcode) == 0xE || TEST_COND(CONDITION(opcode), CODE(opcode), GETCPU.CPSR))
			cycles = arm_instructions_set[PROCNUM][INSTRUCTION_INDEX(opcode)](opcode);
		else
			cycles = 1;
	}
	GETCPU.instruct_adr = GETCPU.next_instruction;

	return cycles;
}

static const Interpreter s_OpDecode[2][2] = {RunInterpreter<0,false>, RunInterpreter<0,true>, RunInterpreter<1,false>, RunInterpreter<1,true>};

////////////////////////////////////////////////////////////////////
static void FASTCALL InterpreterFallback(const Decoded &d, char *&szCodeBuffer)
{
	u32 PROCNUM = d.ProcessID;

	WRITE_CODE("(*(u32*)0x%p) = %u;\n", &(GETCPU.next_instruction), d.CalcNextInstruction(d));
	WRITE_CODE("REG_W(0x%p) = %u;\n", REG_W(15), d.CalcR15(d));
	if (d.ThumbFlag)
		WRITE_CODE("ExecuteCycles+=((u32 (FASTCALL *)(u32))0x%p)(%u);\n", thumb_instructions_set[PROCNUM][d.Instruction.ThumbOp>>6], d.Instruction.ThumbOp);
	else
		WRITE_CODE("ExecuteCycles+=((u32 (FASTCALL *)(u32))0x%p)(%u);\n", arm_instructions_set[PROCNUM][INSTRUCTION_INDEX(d.Instruction.ArmOp)], d.Instruction.ArmOp);
	WRITE_CODE("(*(u32*)0x%p) = (*(u32*)0x%p);\n", &(GETCPU.instruct_adr), &(GETCPU.next_instruction));

	if (d.R15Modified)
		WRITE_CODE("return ExecuteCycles;\n");
}
////////////////////////////////////////////////////////////////////
static const u32 s_CacheReserveMin = 4 * 1024 * 1024;
static u32 s_CacheReserve = 16 * 1024 * 1024;
static MemBuffer* s_CodeBuffer = NULL;

static void ReleaseCodeBuffer()
{
	delete s_CodeBuffer;
	s_CodeBuffer = NULL;
}

static void InitializeCodeBuffer()
{
	ReleaseCodeBuffer();

	s_CodeBuffer = new MemBuffer(MemBuffer::kRead|MemBuffer::kWrite|MemBuffer::kExec,s_CacheReserveMin);
	s_CodeBuffer->Reserve(s_CacheReserve);
	s_CacheReserve = s_CodeBuffer->GetReservedSize();
}

static void ResetCodeBuffer()
{
	uintptr_t base = (uintptr_t)s_CodeBuffer->GetBasePtr();
	u32 size = s_CodeBuffer->GetUsedSize();

	PROGINFO("CodeBuffer : %u\n", size);

#ifdef __GNUC__
	__builtin___clear_cache(base, base + size);
#endif

	s_CodeBuffer->Reset();
}

static void* AllocCodeBuffer(size_t size)
{
	static const u32 align = 4 - 1;

	u32 size_new = size + align;

	uintptr_t ptr = (uintptr_t)s_CodeBuffer->Alloc(size_new);
	if (ptr == 0)
		return NULL;

	uintptr_t retptr = (ptr + align) & ~align;

	return (void*)retptr;
}

////////////////////////////////////////////////////////////////////
static const u32 s_TccCount = 1;
static TCCState* s_TccList[s_TccCount] = {NULL};
static u32 s_NextTcc = 0;

static void ReleaseTcc()
{
	for (u32 i = 0; i < s_TccCount; i++)
	{
		if (s_TccList[i])
		{
			tcc_delete(s_TccList[i]);
			s_TccList[i] = NULL;
		}
	}

	s_NextTcc = 0;
}

static void InitializeTcc()
{
	ReleaseTcc();

	s_NextTcc = 0;
	for (u32 i = 0; i < s_TccCount; i++)
	{
		TCCState* s = tcc_new();
		//tcc_set_output_type(s, TCC_OUTPUT_MEMORY);
		//tcc_set_options(s, "-Werror");
		tcc_set_options(s, "-nostdlib");

		s_TccList[i] = s;
	}
}

static TCCState* AllocTcc()
{
	if (s_NextTcc < s_TccCount)
		return s_TccList[s_NextTcc++];

	for (u32 i = 0; i < s_TccCount; i++)
	{
		tcc_delete(s_TccList[i]);

		TCCState* s = tcc_new();
		//tcc_set_output_type(s, TCC_OUTPUT_MEMORY);
		//tcc_set_options(s, "-Werror");
		tcc_set_options(s, "-nostdlib");

		s_TccList[i] = s;
	}

	s_NextTcc = 1;
	return s_TccList[0];
}

////////////////////////////////////////////////////////////////////
static MemBuffer* s_CMemBuffer = NULL;
static char* s_CBufferBase = NULL;
static char* s_CBuffer = NULL;
static char* s_CBufferCur = NULL;

static void ReleaseCBuffer()
{
	delete s_CMemBuffer;

	s_CMemBuffer = NULL;
	s_CBufferBase = NULL;
	s_CBuffer = NULL;
	s_CBufferCur = NULL;
}

static void InitializeCBuffer()
{
	ReleaseCBuffer();

	static const int Size = 1 * 256 * 1024;

	s_CMemBuffer = new MemBuffer(MemBuffer::kRead|MemBuffer::kWrite,Size);
	s_CMemBuffer->Reserve();
	s_CBufferBase = (char*)s_CMemBuffer->Alloc(Size);

	{
		char* szCodeBuffer = s_CBufferBase;

		WRITE_CODE("typedef unsigned char u8;\n");
		WRITE_CODE("typedef unsigned short u16;\n");
		WRITE_CODE("typedef unsigned int u32;\n");
		WRITE_CODE("typedef unsigned long long u64;\n");
		WRITE_CODE("typedef signed char s8;\n");
		WRITE_CODE("typedef signed short s16;\n");
		WRITE_CODE("typedef signed int s32;\n");
		WRITE_CODE("typedef signed long long s64;\n");
		WRITE_CODE("typedef int BOOL;\n");

#ifdef __MINGW32__ 
		WRITE_CODE("#define FASTCALL __attribute__((fastcall))\n");
#elif defined (__i386__) && !defined(__clang__)
		WRITE_CODE("#define FASTCALL __attribute__((regparm(3)))\n");
#elif defined(_MSC_VER) || defined(__INTEL_COMPILER)
		//WRITE_CODE("#define FASTCALL __fastcall\n");
		WRITE_CODE("#define FASTCALL __attribute__((fastcall))\n");
#else
		WRITE_CODE("#define FASTCALL\n");
#endif

		WRITE_CODE("#define BIT(n)  (1<<(n))\n");
		WRITE_CODE("#define BIT_N(i,n)  (((i)>>(n))&1)\n");
		WRITE_CODE("#define BIT0(i)     ((i)&1)\n");
		//WRITE_CODE("#define BIT1(i)     BIT_N((i),1)\n");
		//WRITE_CODE("#define BIT2(i)     BIT_N((i),2)\n");
		//WRITE_CODE("#define BIT3(i)     BIT_N((i),3)\n");
		//WRITE_CODE("#define BIT4(i)     BIT_N((i),4)\n");
		//WRITE_CODE("#define BIT5(i)     BIT_N((i),5)\n");
		//WRITE_CODE("#define BIT6(i)     BIT_N((i),6)\n");
		//WRITE_CODE("#define BIT7(i)     BIT_N((i),7)\n");
		//WRITE_CODE("#define BIT8(i)     BIT_N((i),8)\n");
		//WRITE_CODE("#define BIT9(i)     BIT_N((i),9)\n");
		//WRITE_CODE("#define BIT10(i)     BIT_N((i),10)\n");
		//WRITE_CODE("#define BIT11(i)     BIT_N((i),11)\n");
		//WRITE_CODE("#define BIT12(i)     BIT_N((i),12)\n");
		//WRITE_CODE("#define BIT13(i)     BIT_N((i),13)\n");
		//WRITE_CODE("#define BIT14(i)     BIT_N((i),14)\n");
		//WRITE_CODE("#define BIT15(i)     BIT_N((i),15)\n");
		//WRITE_CODE("#define BIT16(i)     BIT_N((i),16)\n");
		//WRITE_CODE("#define BIT17(i)     BIT_N((i),17)\n");
		//WRITE_CODE("#define BIT18(i)     BIT_N((i),18)\n");
		//WRITE_CODE("#define BIT19(i)     BIT_N((i),19)\n");
		//WRITE_CODE("#define BIT20(i)     BIT_N((i),20)\n");
		//WRITE_CODE("#define BIT21(i)     BIT_N((i),21)\n");
		//WRITE_CODE("#define BIT22(i)     BIT_N((i),22)\n");
		//WRITE_CODE("#define BIT23(i)     BIT_N((i),23)\n");
		//WRITE_CODE("#define BIT24(i)     BIT_N((i),24)\n");
		//WRITE_CODE("#define BIT25(i)     BIT_N((i),25)\n");
		//WRITE_CODE("#define BIT26(i)     BIT_N((i),26)\n");
		//WRITE_CODE("#define BIT27(i)     BIT_N((i),27)\n");
		WRITE_CODE("#define BIT28(i)     BIT_N((i),28)\n");
		WRITE_CODE("#define BIT29(i)     BIT_N((i),29)\n");
		WRITE_CODE("#define BIT30(i)     BIT_N((i),30)\n");
		WRITE_CODE("#define BIT31(i)    ((i)>>31)\n");

		WRITE_CODE("#define HWORD(i)   ((s32)(((s32)(i))>>16))\n");
		WRITE_CODE("#define LWORD(i)   (s32)(((s32)((i)<<16))>>16)\n");

#ifdef WORDS_BIGENDIAN
		WRITE_CODE("typedef union{\n");
		WRITE_CODE("	struct{\n");
		WRITE_CODE("		u32 N : 1,\n");
		WRITE_CODE("		Z : 1,\n");
		WRITE_CODE("		C : 1,\n");
		WRITE_CODE("		V : 1,\n");
		WRITE_CODE("		Q : 1,\n");
		WRITE_CODE("		RAZ : 19,\n");
		WRITE_CODE("		I : 1,\n");
		WRITE_CODE("		F : 1,\n");
		WRITE_CODE("		T : 1,\n");
		WRITE_CODE("		mode : 5;\n");
		WRITE_CODE("	} bits;\n");
		WRITE_CODE("	u32 val;\n");
		WRITE_CODE("} Status_Reg;\n");
#else
		WRITE_CODE("typedef union{\n");
		WRITE_CODE("	struct{\n");
		WRITE_CODE("		u32 mode : 5,\n");
		WRITE_CODE("		T : 1,\n");
		WRITE_CODE("		F : 1,\n");
		WRITE_CODE("		I : 1,\n");
		WRITE_CODE("		RAZ : 19,\n");
		WRITE_CODE("		Q : 1,\n");
		WRITE_CODE("		V : 1,\n");
		WRITE_CODE("		C : 1,\n");
		WRITE_CODE("		Z : 1,\n");
		WRITE_CODE("		N : 1;\n");
		WRITE_CODE("	} bits;\n");
		WRITE_CODE("	u32 val;\n");
		WRITE_CODE("} Status_Reg;\n");
#endif

		WRITE_CODE("#define REG_R(p)	(*(u32*)(p))\n");
		WRITE_CODE("#define REG_SR(p)	(*(s32*)(p))\n");
		WRITE_CODE("#define REG_R_C(p)	((u32)(p))\n");
		WRITE_CODE("#define REG_SR_C(p)	((s32)(p))\n");
		WRITE_CODE("#define REG_W(p)	(*(u32*)(p))\n");
		WRITE_CODE("#define REG(p)		(*(u32*)(p))\n");
		WRITE_CODE("#define REGPTR(p)	((u32*)(p))\n");

		WRITE_CODE("static const u8* arm_cond_table = (const u8*)0x%p;\n", arm_cond_table);

		WRITE_CODE("#define TEST_COND(cond,inst,CPSR) ((arm_cond_table[((CPSR>>24)&0xf0)|(cond)]) & (1<<(inst)))\n");

		WRITE_CODE("inline u32 ROR(u32 i, u32 j)\n");
		WRITE_CODE("{return ((((u32)(i))>>(j)) | (((u32)(i))<<(32-(j))));}\n");

		WRITE_CODE("inline u32 UNSIGNED_OVERFLOW(u32 a,u32 b,u32 c)\n");
		WRITE_CODE("{return BIT31(((a)&(b)) | (((a)|(b))&(~c)));}\n");

		WRITE_CODE("inline u32 UNSIGNED_UNDERFLOW(u32 a,u32 b,u32 c)\n");
		WRITE_CODE("{return BIT31(((~a)&(b)) | (((~a)|(b))&(c)));}\n");

		WRITE_CODE("inline u32 SIGNED_OVERFLOW(u32 a,u32 b,u32 c)\n");
		WRITE_CODE("{return BIT31(((a)&(b)&(~c)) | ((~a)&(~(b))&(c)));}\n");

		WRITE_CODE("inline u32 SIGNED_UNDERFLOW(u32 a,u32 b,u32 c)\n");
		WRITE_CODE("{return BIT31(((a)&(~(b))&(~c)) | ((~a)&(b)&(c)));}\n");

		WRITE_CODE("inline BOOL CarryFrom(s32 left, s32 right)\n");
		WRITE_CODE("{u32 res  = (0xFFFFFFFFU - (u32)left);\n");
		WRITE_CODE("return ((u32)right > res);}\n");

		WRITE_CODE("inline BOOL BorrowFrom(s32 left, s32 right)\n");
		WRITE_CODE("{return ((u32)right > (u32)left);}\n");

		WRITE_CODE("inline BOOL OverflowFromADD(s32 alu_out, s32 left, s32 right)\n");
		WRITE_CODE("{return ((left >= 0 && right >= 0) || (left < 0 && right < 0))\n");
		WRITE_CODE("&& ((left < 0 && alu_out >= 0) || (left >= 0 && alu_out < 0));}\n");

		WRITE_CODE("inline BOOL OverflowFromSUB(s32 alu_out, s32 left, s32 right)\n");
		WRITE_CODE("{return ((left < 0 && right >= 0) || (left >= 0 && right < 0))\n");
		WRITE_CODE("&& ((left < 0 && alu_out >= 0) || (left >= 0 && alu_out < 0));}\n");
	}

	s_CBuffer = s_CBufferBase + strlen(s_CBufferBase);
	s_CBufferCur = s_CBuffer;
}

static char* ResetCBuffer()
{
	s_CBufferCur = s_CBuffer;

	return s_CBufferCur;
}

static ArmAnalyze *s_pArmAnalyze = NULL;

typedef u32 (* ArmOpCompiled)();

TEMPLATE static u32 armcpu_compile()
{
	u32 adr = ARMPROC.instruct_adr;
	u32 Cycles = 0;

	if (!JITLUT_MAPPED(adr & 0x0FFFFFFF, PROCNUM))
	{
		INFO("JIT: use unmapped memory address %08X\n", adr);
		execute = false;
		return 1;
	}

	char szFunName[64] = {0};
	const s32 MaxInstructionsNum = 100;
	Decoded Instructions[MaxInstructionsNum];

	s32 InstructionsNum = s_pArmAnalyze->Decode(GETCPUPTR, Instructions, MaxInstructionsNum);
	if (InstructionsNum <= 0)
	{
		INFO("JIT: unknow error cpu[%d].\n", PROCNUM);
		return 1;
	}
	u32 R15Num = s_pArmAnalyze->OptimizeFlag(Instructions, InstructionsNum);
	s32 SubBlocks = s_pArmAnalyze->CreateSubBlocks(Instructions, InstructionsNum);
	InstructionsNum = s_pArmAnalyze->Optimize(Instructions, InstructionsNum);

	char* szCodeBuffer = ResetCBuffer();

	sprintf(szFunName, "ArmOp_%u", Instructions[0].Address);
	WRITE_CODE("u32 %s(){\n", szFunName);
	WRITE_CODE("u32 ExecuteCycles=0;\n");
	
	u32 CurSubBlock = INVALID_SUBBLOCK;
	u32 CurInstructions = 0;
	bool IsSubBlockStart = false;

	for (s32 i = 0; i < InstructionsNum; i++)
	{
		Decoded &Inst = Instructions[i];

		if (CurSubBlock != Inst.SubBlock)
		{
			if (IsSubBlockStart)
			{
				WRITE_CODE("}\n");
				WRITE_CODE("else ExecuteCycles+=%u;\n", CurInstructions);
				IsSubBlockStart = false;
			}

			if (Inst.Cond != 0xE && Inst.Cond != 0xF)
			{
				WRITE_CODE("if(TEST_COND(%u,0,(*(u32*)0x%p))){\n", Inst.Cond, &GETCPU.CPSR.val);
				IsSubBlockStart = true;
			}

			CurInstructions = 0;

			CurSubBlock = Inst.SubBlock;
		}

		CurInstructions++;

		WRITE_CODE("{\n");
		if (Inst.ThumbFlag)
		{
			if ((Inst.IROp >= IR_LDM && Inst.IROp <= IR_STM))
				InterpreterFallback(Inst, szCodeBuffer);
			else
			{
				if (!Inst.VariableCycles)
					WRITE_CODE("ExecuteCycles+=%u;\n", Inst.ExecuteCycles);
				iropcdecoder_set[Inst.IROp](Inst, szCodeBuffer);
			}
		}
		else
		{
			if ((Inst.IROp >= IR_LDM && Inst.IROp <= IR_STM))
				InterpreterFallback(Inst, szCodeBuffer);
			else
			{
				if (!Inst.VariableCycles)
					WRITE_CODE("ExecuteCycles+=%u;\n", Inst.ExecuteCycles);
				iropcdecoder_set[Inst.IROp](Inst, szCodeBuffer);
			}
		}
		WRITE_CODE("}\n");

#ifdef USE_INTERPRETER_FIRST
		Cycles += s_OpDecode[PROCNUM][Inst.ThumbFlag](Inst);
#endif
	}

	Decoded &LastIns = Instructions[InstructionsNum - 1];
	if (IsSubBlockStart)
	{
		WRITE_CODE("}\n");
		IsSubBlockStart = false;
	}
	WRITE_CODE("(*(u32*)0x%p) = %u;\n", &(GETCPU.instruct_adr), LastIns.Address + (LastIns.ThumbFlag ? 2 : 4));
	WRITE_CODE("return ExecuteCycles;}\n");

	{
		TCCState* s = AllocTcc();

		if (tcc_compile_string(s, s_CBufferBase) == -1)
		{
			fprintf(stderr, "%s\n", s_CBufferBase);
			return 1;
		}

		int size = tcc_relocate(s, NULL);
		if (size == -1)
		{
			fprintf(stderr, "%s\n", s_CBufferBase);
			return 1;
		}
		void* ptr = AllocCodeBuffer(size);
		if (!ptr)
		{
			INFO("JIT: cache full, reset cpu[%d].\n", PROCNUM);

			arm_cjit.Reset();

			ptr = AllocCodeBuffer(size);
		}

		size = tcc_relocate(s, ptr);
		if (size == -1)
		{
			fprintf(stderr, "%s\n", s_CBufferBase);
			return 1;
		}

		ArmOpCompiled opfun = (ArmOpCompiled)tcc_get_symbol(s, szFunName);

		JITLUT_HANDLE(adr, PROCNUM) = (uintptr_t)opfun;

#ifndef USE_INTERPRETER_FIRST
		Cycles = opfun();
#endif
	}

	return Cycles;
}

static void cpuReserve()
{
	InitializeCBuffer();
	InitializeTcc();
	InitializeCodeBuffer();

	s_pArmAnalyze = new ArmAnalyze();
	s_pArmAnalyze->Initialize();

	s_pArmAnalyze->m_MergeSubBlocks = true;
	//s_pArmAnalyze->m_OptimizeFlag = true;

#ifdef USE_INTERPRETER_FIRST
	INFO("Use Interpreter First\n");
#endif
}

static void cpuShutdown()
{
	ReleaseCBuffer();
	ReleaseTcc();
	ReleaseCodeBuffer();

	JitLutReset();

	delete s_pArmAnalyze;
	s_pArmAnalyze = NULL;
}

static void cpuReset()
{
	ResetCodeBuffer();

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
	ArmOpCompiled opfun = (ArmOpCompiled)JITLUT_HANDLE(ARMPROC.instruct_adr, PROCNUM);
	if (opfun)
		return opfun();

	return armcpu_compile<PROCNUM>();
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
	return "Arm CJit";
}

CpuBase arm_cjit =
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

#endif
