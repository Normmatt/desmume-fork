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
#include "utils/tinycc/libtcc.h"

#ifdef HAVE_JIT

#define GETCPUPTR (&ARMPROC)
#define GETCPU (ARMPROC)

#define REG_R(i)	(i)==15?"_C":"",(i)==15?(u32*)d.CalcR15(d):&(GETCPU.R[(i)])
#define REG_W(i)	(&(GETCPU.R[(i)]))
#define REG(i)		(&(GETCPU.R[(i)]))
#define REGPTR(i)	(&(GETCPU.R[(i)]))

#define TEMPLATE template<int PROCNUM> 
#define OPCDECODER_DECL(name) void FASTCALL name##_CDecoder(const Decoded &d, char *&szCodeBuffer)
#define WRITE_CODE(...) szCodeBuffer += sprintf(szCodeBuffer, __VA_ARGS__)

typedef void (FASTCALL* IROpCDecoder)(const Decoded &d, char *&szCodeBuffer);

typedef u32 (FASTCALL* MemOp1)(u32, u32*);
typedef u32 (FASTCALL* MemOp2)(u32, u32);
typedef u32 (FASTCALL* MemOp3)(u32, u32, u32*);
typedef u32 (FASTCALL* MemOp4)(u32, u32*, u32);

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
	enum {
		MEMTYPE_GENERIC = 0, // no assumptions
		MEMTYPE_MAIN = 1,
		MEMTYPE_DTCM = 2,
		MEMTYPE_ERAM = 3,
		MEMTYPE_SWIRAM = 4,

		MEMTYPE_COUNT,
	};

	static u32 classify_adr(u32 PROCNUM, u32 adr, bool store)
	{
		if(PROCNUM==ARMCPU_ARM9 && (adr & ~0x3FFF) == MMU.DTCMRegion)
			return MEMTYPE_DTCM;
		else if((adr & 0x0F000000) == 0x02000000)
			return MEMTYPE_MAIN;
		else if(PROCNUM==ARMCPU_ARM7 && !store && (adr & 0xFF800000) == 0x03800000)
			return MEMTYPE_ERAM;
		else if(PROCNUM==ARMCPU_ARM7 && !store && (adr & 0xFF800000) == 0x03000000)
			return MEMTYPE_SWIRAM;
		else
			return MEMTYPE_GENERIC;
	}

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
	}

	void FASTCALL LDM_S_LoadCPSRGenerate(const Decoded &d, char *&szCodeBuffer)
	{
	}

	void FASTCALL R15ModifiedGenerate(const Decoded &d, char *&szCodeBuffer)
	{
	}

	OPCDECODER_DECL(IR_UND)
	{
		u32 PROCNUM = d.ProcessID;

		WRITE_CODE("TRAPUNDEF((void*)0x%p);\n", GETCPUPTR);
	}

	OPCDECODER_DECL(IR_NOP)
	{
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
			const bool clacCarry = d.S && !d.R15Modified && (d.FlagsSet & FLAG_C);
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
			const bool clacCarry = d.S && !d.R15Modified && (d.FlagsSet & FLAG_C);
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
			WRITE_CODE("shift_op=REG_W(0x%p)=REG_R%s(0x%p)+shift_op;\n", REG_W(d.Rd), REG_R(d.Rn));
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.N=BIT31(REG(0x%p));\n", &(GETCPU.CPSR), REG(d.Rn));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.Z=(REG(0x%p)==0);\n", &(GETCPU.CPSR), REG(d.Rn));
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=CarryFrom(v, shift_op);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_V)
					WRITE_CODE("((Status_Reg*)0x%p)->bits.V=OverflowFromADD(REG(0x%p), v, shift_op);\n", &(GETCPU.CPSR), REG(d.Rn));
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
			WRITE_CODE("shift_op=REG_W(0x%p)=REG_R%s(0x%p)+shift_op+((Status_Reg*)0x%p)->bits.C;\n", REG_W(d.Rd), REG_R(d.Rn), &(GETCPU.CPSR));
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
			WRITE_CODE("shift_op=REG_W(0x%p)=REG_R%s(0x%p)-shift_op;\n", REG_W(d.Rd), REG_R(d.Rn));
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
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=REG(0x%p)>=v;\n", &(GETCPU.CPSR), REG(d.Rd));
					WRITE_CODE("else\n");
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=REG(0x%p)>v;\n", &(GETCPU.CPSR), REG(d.Rd));
				}
			}
		}
		else
		{
			IRShiftOpGenerate(d, szCodeBuffer, false);

			if (d.S && !d.R15Modified && ((d.FlagsSet & FLAG_C) || (d.FlagsSet & FLAG_V)))
				WRITE_CODE("u32 v=REG_R%s(0x%p);\n", REG_R(d.Rn));
			WRITE_CODE("shift_op=REG_W(0x%p)=REG_R%s(0x%p)-shift_op-!((Status_Reg*)0x%p)->bits.C;\n", REG_W(d.Rd), REG_R(d.Rn), &(GETCPU.CPSR));
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
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=REG(0x%p)>=v;\n", &(GETCPU.CPSR), REG(d.Rd));
					WRITE_CODE("else\n");
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=REG(0x%p)>v;\n", &(GETCPU.CPSR), REG(d.Rd));
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
			WRITE_CODE("shift_op=REG_W(0x%p)=shift_op-REG_R%s(0x%p);\n", REG_W(d.Rd), REG_R(d.Rn));
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
					WRITE_CODE("((Status_Reg*)0x%p)->bits.V=BIT31((%u^v) & (REG(0x%p)^v));\n", &(GETCPU.CPSR), d.Immediate, REG(d.Rd));
				if (d.FlagsSet & FLAG_C)
				{
					WRITE_CODE("if(((Status_Reg*)0x%p)->bits.C)\n", &(GETCPU.CPSR));
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=REG(0x%p)>=v;\n", &(GETCPU.CPSR), REG(d.Rd));
					WRITE_CODE("else\n");
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=REG(0x%p)>v;\n", &(GETCPU.CPSR), REG(d.Rd));
				}
			}
		}
		else
		{
			IRShiftOpGenerate(d, szCodeBuffer, false);

			if (d.S && !d.R15Modified && ((d.FlagsSet & FLAG_C) || (d.FlagsSet & FLAG_V)))
				WRITE_CODE("u32 v=REG_R%s(0x%p);\n", REG_R(d.Rn));
			WRITE_CODE("shift_op=REG_W(0x%p)=shift_op-REG_R%s(0x%p)-!((Status_Reg*)0x%p)->bits.C;\n", REG_W(d.Rd), REG_R(d.Rn), &(GETCPU.CPSR));
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
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=REG(0x%p)>=v;\n", &(GETCPU.CPSR), REG(d.Rd));
					WRITE_CODE("else\n");
					WRITE_CODE("((Status_Reg*)0x%p)->bits.C=REG(0x%p)>v;\n", &(GETCPU.CPSR), REG(d.Rd));
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
			MEMOP_LDR<0,MEMTYPE_DTCM,3>,
			MEMOP_LDR<0,MEMTYPE_ERAM,3>,
			MEMOP_LDR<0,MEMTYPE_SWIRAM,3>,
		},
		{
			MEMOP_LDR<1,MEMTYPE_GENERIC,3>,
			MEMOP_LDR<1,MEMTYPE_MAIN,3>,
			MEMOP_LDR<1,MEMTYPE_DTCM,3>,
			MEMOP_LDR<1,MEMTYPE_ERAM,3>,
			MEMOP_LDR<1,MEMTYPE_SWIRAM,3>,
		}
	};

	static const MemOp1 LDR_R15_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_LDR<0,MEMTYPE_GENERIC,5>,
			MEMOP_LDR<0,MEMTYPE_MAIN,5>,
			MEMOP_LDR<0,MEMTYPE_DTCM,5>,
			MEMOP_LDR<0,MEMTYPE_ERAM,5>,
			MEMOP_LDR<0,MEMTYPE_SWIRAM,5>,
		},
		{
			MEMOP_LDR<1,MEMTYPE_GENERIC,5>,
			MEMOP_LDR<1,MEMTYPE_MAIN,5>,
			MEMOP_LDR<1,MEMTYPE_DTCM,5>,
			MEMOP_LDR<1,MEMTYPE_ERAM,5>,
			MEMOP_LDR<1,MEMTYPE_SWIRAM,5>,
		}
	};

	static const MemOp1 LDRB_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_LDRB<0,MEMTYPE_GENERIC,3>,
			MEMOP_LDRB<0,MEMTYPE_MAIN,3>,
			MEMOP_LDRB<0,MEMTYPE_DTCM,3>,
			MEMOP_LDRB<0,MEMTYPE_ERAM,3>,
			MEMOP_LDRB<0,MEMTYPE_SWIRAM,3>,
		},
		{
			MEMOP_LDRB<1,MEMTYPE_GENERIC,3>,
			MEMOP_LDRB<1,MEMTYPE_MAIN,3>,
			MEMOP_LDRB<1,MEMTYPE_DTCM,3>,
			MEMOP_LDRB<1,MEMTYPE_ERAM,3>,
			MEMOP_LDRB<1,MEMTYPE_SWIRAM,3>,
		}
	};

	OPCDECODER_DECL(IR_LDR)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.P)
		{
			if (d.I)
				WRITE_CODE("u32 adr = REG_R%s(0x%p) %c %u;\n", REG_R(d.Rn), d.U ? '+' : '-', d.Immediate);
			else
			{
				IRShiftOpGenerate(d, szCodeBuffer, false);

				WRITE_CODE("u32 adr = REG_R%s(0x%p) %c shift_op;\n", REG_R(d.Rn), d.U ? '+' : '-');
			}

			if (d.W)
				WRITE_CODE("REG_W(0x%p) = adr;\n", REG_W(d.Rn));
		}
		else
		{
			WRITE_CODE("u32 adr = REG_R%s(0x%p);\n", REG_R(d.Rn));
			if (d.I)
				WRITE_CODE("REG_W(0x%p) = adr %c %u;\n", REG_W(d.Rn), d.Immediate, d.U ? '+' : '-');
			else
			{
				IRShiftOpGenerate(d, szCodeBuffer, false);

				WRITE_CODE("REG_W(0x%p) = adr %c shift_op;\n", REG_W(d.Rn), d.U ? '+' : '-');
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
					WRITE_CODE("REG(0x%p) &= 0xFFFFFFFE", REG(15));
				}
				else
					WRITE_CODE("REG(0x%p) &= 0xFFFFFFFC", REG(15));

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
			MEMOP_STR<0,MEMTYPE_DTCM,2>,
			MEMOP_STR<0,MEMTYPE_ERAM,2>,
			MEMOP_STR<0,MEMTYPE_SWIRAM,2>,
		},
		{
			MEMOP_STR<1,MEMTYPE_GENERIC,2>,
			MEMOP_STR<1,MEMTYPE_MAIN,2>,
			MEMOP_STR<1,MEMTYPE_DTCM,2>,
			MEMOP_STR<1,MEMTYPE_ERAM,2>,
			MEMOP_STR<1,MEMTYPE_SWIRAM,2>,
		}
	};

	static const MemOp2 STRB_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_STRB<0,MEMTYPE_GENERIC,2>,
			MEMOP_STRB<0,MEMTYPE_MAIN,2>,
			MEMOP_STRB<0,MEMTYPE_DTCM,2>,
			MEMOP_STRB<0,MEMTYPE_ERAM,2>,
			MEMOP_STRB<0,MEMTYPE_SWIRAM,2>,
		},
		{
			MEMOP_STRB<1,MEMTYPE_GENERIC,2>,
			MEMOP_STRB<1,MEMTYPE_MAIN,2>,
			MEMOP_STRB<1,MEMTYPE_DTCM,2>,
			MEMOP_STRB<1,MEMTYPE_ERAM,2>,
			MEMOP_STRB<1,MEMTYPE_SWIRAM,2>,
		}
	};

	OPCDECODER_DECL(IR_STR)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.P)
		{
			if (d.I)
				WRITE_CODE("u32 adr = REG_R%s(0x%p) %c %u;\n", REG_R(d.Rn), d.U ? '+' : '-', d.Immediate);
			else
			{
				IRShiftOpGenerate(d, szCodeBuffer, false);

				WRITE_CODE("u32 adr = REG_R%s(0x%p) %c shift_op;\n", REG_R(d.Rn), d.U ? '+' : '-');
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
				WRITE_CODE("REG_W(0x%p) = adr %c %u;\n", REG_W(d.Rn), d.Immediate, d.U ? '+' : '-');
			else
			{
				IRShiftOpGenerate(d, szCodeBuffer, false);

				WRITE_CODE("REG_W(0x%p) = adr %c shift_op;\n", REG_W(d.Rn), d.U ? '+' : '-');
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
			MEMOP_LDRH<0,MEMTYPE_DTCM,3>,
			MEMOP_LDRH<0,MEMTYPE_ERAM,3>,
			MEMOP_LDRH<0,MEMTYPE_SWIRAM,3>,
		},
		{
			MEMOP_LDRH<1,MEMTYPE_GENERIC,3>,
			MEMOP_LDRH<1,MEMTYPE_MAIN,3>,
			MEMOP_LDRH<1,MEMTYPE_DTCM,3>,
			MEMOP_LDRH<1,MEMTYPE_ERAM,3>,
			MEMOP_LDRH<1,MEMTYPE_SWIRAM,3>,
		}
	};

	static const MemOp1 LDRSH_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_LDRSH<0,MEMTYPE_GENERIC,3>,
			MEMOP_LDRSH<0,MEMTYPE_MAIN,3>,
			MEMOP_LDRSH<0,MEMTYPE_DTCM,3>,
			MEMOP_LDRSH<0,MEMTYPE_ERAM,3>,
			MEMOP_LDRSH<0,MEMTYPE_SWIRAM,3>,
		},
		{
			MEMOP_LDRSH<1,MEMTYPE_GENERIC,3>,
			MEMOP_LDRSH<1,MEMTYPE_MAIN,3>,
			MEMOP_LDRSH<1,MEMTYPE_DTCM,3>,
			MEMOP_LDRSH<1,MEMTYPE_ERAM,3>,
			MEMOP_LDRSH<1,MEMTYPE_SWIRAM,3>,
		}
	};

	static const MemOp1 LDRSB_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_LDRSB<0,MEMTYPE_GENERIC,3>,
			MEMOP_LDRSB<0,MEMTYPE_MAIN,3>,
			MEMOP_LDRSB<0,MEMTYPE_DTCM,3>,
			MEMOP_LDRSB<0,MEMTYPE_ERAM,3>,
			MEMOP_LDRSB<0,MEMTYPE_SWIRAM,3>,
		},
		{
			MEMOP_LDRSB<1,MEMTYPE_GENERIC,3>,
			MEMOP_LDRSB<1,MEMTYPE_MAIN,3>,
			MEMOP_LDRSB<1,MEMTYPE_DTCM,3>,
			MEMOP_LDRSB<1,MEMTYPE_ERAM,3>,
			MEMOP_LDRSB<1,MEMTYPE_SWIRAM,3>,
		}
	};

	OPCDECODER_DECL(IR_LDRx)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.P)
		{
			if (d.I)
				WRITE_CODE("u32 adr = REG_R%s(0x%p) %c %u;\n", REG_R(d.Rn), d.U ? '+' : '-', d.Immediate);
			else
				WRITE_CODE("u32 adr = REG_R%s(0x%p) %c REG_R%s(0x%p);\n", REG_R(d.Rn), d.U ? '+' : '-', REG_R(d.Rm));

			if (d.W)
				WRITE_CODE("REG_W(0x%p) = adr;\n", REG_W(d.Rn));
		}
		else
		{
			WRITE_CODE("u32 adr = REG_R%s(0x%p);\n", REG_R(d.Rn));

			if (d.I)
				WRITE_CODE("REG_W(0x%p) = adr %c %u;\n", REG_W(d.Rn), d.Immediate, d.U ? '+' : '-');
			else
				WRITE_CODE("REG_W(0x%p) = adr %c REG_R%s(0x%p);\n", REG_W(d.Rn), d.U ? '+' : '-', REG_R(d.Rm));
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
			MEMOP_STRH<0,MEMTYPE_DTCM,2>,
			MEMOP_STRH<0,MEMTYPE_ERAM,2>,
			MEMOP_STRH<0,MEMTYPE_SWIRAM,2>,
		},
		{
			MEMOP_STRH<1,MEMTYPE_GENERIC,2>,
			MEMOP_STRH<1,MEMTYPE_MAIN,2>,
			MEMOP_STRH<1,MEMTYPE_DTCM,2>,
			MEMOP_STRH<1,MEMTYPE_ERAM,2>,
			MEMOP_STRH<1,MEMTYPE_SWIRAM,2>,
		}
	};

	OPCDECODER_DECL(IR_STRx)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.P)
		{
			if (d.I)
				WRITE_CODE("u32 adr = REG_R%s(0x%p) %c %u;\n", REG_R(d.Rn), d.U ? '+' : '-', d.Immediate);
			else
				WRITE_CODE("u32 adr = REG_R%s(0x%p) %c REG_R%s(0x%p);\n", REG_R(d.Rn), d.U ? '+' : '-', REG_R(d.Rm));

			if (d.W)
				WRITE_CODE("REG_W(0x%p) = adr;\n", REG_W(d.Rn));
		}
		else
			WRITE_CODE("u32 adr = REG_R%s(0x%p);\n", REG_R(d.Rn));

		WRITE_CODE("ExecuteCycles+=((u32 (FASTCALL *)(u32, u32))0x%p)(adr,REG_R%s(0x%p));\n", STRH_Tab[PROCNUM][0], REG_R(d.Rd));

		if (!d.P)
		{
			if (d.I)
				WRITE_CODE("REG_W(0x%p) = adr %c %u;\n", REG_W(d.Rn), d.Immediate, d.U ? '+' : '-');
			else
				WRITE_CODE("REG_W(0x%p) = adr %c REG_R%s(0x%p);\n", REG_W(d.Rn), d.U ? '+' : '-', REG_R(d.Rm));
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
			MEMOP_LDRD<0,MEMTYPE_DTCM,3>,
			MEMOP_LDRD<0,MEMTYPE_ERAM,3>,
			MEMOP_LDRD<0,MEMTYPE_SWIRAM,3>,
		},
		{
			MEMOP_LDRD<1,MEMTYPE_GENERIC,3>,
			MEMOP_LDRD<1,MEMTYPE_MAIN,3>,
			MEMOP_LDRD<1,MEMTYPE_DTCM,3>,
			MEMOP_LDRD<1,MEMTYPE_ERAM,3>,
			MEMOP_LDRD<1,MEMTYPE_SWIRAM,3>,
		}
	};

	OPCDECODER_DECL(IR_LDRD)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.P)
		{
			if (d.I)
				WRITE_CODE("u32 adr = REG_R%s(0x%p) %c %u;\n", REG_R(d.Rn), d.U ? '+' : '-', d.Immediate);
			else
				WRITE_CODE("u32 adr = REG_R%s(0x%p) %c REG_R%s(0x%p);\n", REG_R(d.Rn), d.U ? '+' : '-', REG_R(d.Rm));

			if (d.W)
				WRITE_CODE("REG_W(0x%p) = adr;\n", REG_W(d.Rn));
		}
		else
		{
			WRITE_CODE("u32 adr = REG_R%s(0x%p);\n", REG_R(d.Rn));

			if (d.I)
				WRITE_CODE("REG_W(0x%p) = adr %c %u;\n", REG_W(d.Rn), d.Immediate, d.U ? '+' : '-');
			else
				WRITE_CODE("REG_W(0x%p) = adr %c REG_R%s(0x%p);\n", REG_W(d.Rn), d.U ? '+' : '-', REG_R(d.Rm));
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
			MEMOP_STRD<0,MEMTYPE_DTCM,3>,
			MEMOP_STRD<0,MEMTYPE_ERAM,3>,
			MEMOP_STRD<0,MEMTYPE_SWIRAM,3>,
		},
		{
			MEMOP_STRD<1,MEMTYPE_GENERIC,3>,
			MEMOP_STRD<1,MEMTYPE_MAIN,3>,
			MEMOP_STRD<1,MEMTYPE_DTCM,3>,
			MEMOP_STRD<1,MEMTYPE_ERAM,3>,
			MEMOP_STRD<1,MEMTYPE_SWIRAM,3>,
		}
	};

	OPCDECODER_DECL(IR_STRD)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.P)
		{
			if (d.I)
				WRITE_CODE("u32 adr = REG_R%s(0x%p) %c %u;\n", REG_R(d.Rn), d.U ? '+' : '-', d.Immediate);
			else
				WRITE_CODE("u32 adr = REG_R%s(0x%p) %c REG_R%s(0x%p);\n", REG_R(d.Rn), d.U ? '+' : '-', REG_R(d.Rm));

			if (d.W)
				WRITE_CODE("REG_W(0x%p) = adr;\n", REG_W(d.Rn));
		}
		else
		{
			WRITE_CODE("u32 adr = REG_R%s(0x%p);\n", REG_R(d.Rn));

			if (d.I)
				WRITE_CODE("REG_W(0x%p) = adr %c %u;\n", REG_W(d.Rn), d.Immediate, d.U ? '+' : '-');
			else
				WRITE_CODE("REG_W(0x%p) = adr %c REG_R%s(0x%p);\n", REG_W(d.Rn), d.U ? '+' : '-', REG_R(d.Rm));
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
	static u32 FASTCALL MEMOP_LDM_SEQUENCE(u32 adr, u32 count, u32 *regs)
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
				for (s32 i = (s32)count; i > 0; i--)
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
				for (s32 i = (s32)count; i > 0; i--)
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
				for (s32 i = (s32)count; i > 0; i--)
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
	static u32 FASTCALL MEMOP_LDM(u32 adr, u32 count, u32 *regs_ptr)
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
				for (s32 i = (s32)count; i > 0; i--)
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
				for (s32 i = (s32)count; i > 0; i--)
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
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_DTCM,2,true>,
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_ERAM,2,true>,
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_SWIRAM,2,true>,
		},
		{
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_GENERIC,2,true>,
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_MAIN,2,true>,
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_DTCM,2,true>,
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_ERAM,2,true>,
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_SWIRAM,2,true>,
		}
	};

	static const MemOp3 LDM_Up_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_LDM<0,MEMTYPE_GENERIC,2,true>,
			MEMOP_LDM<0,MEMTYPE_MAIN,2,true>,
			MEMOP_LDM<0,MEMTYPE_DTCM,2,true>,
			MEMOP_LDM<0,MEMTYPE_ERAM,2,true>,
			MEMOP_LDM<0,MEMTYPE_SWIRAM,2,true>,
		},
		{
			MEMOP_LDM<1,MEMTYPE_GENERIC,2,true>,
			MEMOP_LDM<1,MEMTYPE_MAIN,2,true>,
			MEMOP_LDM<1,MEMTYPE_DTCM,2,true>,
			MEMOP_LDM<1,MEMTYPE_ERAM,2,true>,
			MEMOP_LDM<1,MEMTYPE_SWIRAM,2,true>,
		}
	};

	static const MemOp3 LDM_SEQUENCE_Up_R15_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_GENERIC,4,true>,
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_MAIN,4,true>,
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_DTCM,4,true>,
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_ERAM,4,true>,
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_SWIRAM,4,true>,
		},
		{
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_GENERIC,4,true>,
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_MAIN,4,true>,
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_DTCM,4,true>,
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_ERAM,4,true>,
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_SWIRAM,4,true>,
		}
	};

	static const MemOp3 LDM_Up_R15_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_LDM<0,MEMTYPE_GENERIC,4,true>,
			MEMOP_LDM<0,MEMTYPE_MAIN,4,true>,
			MEMOP_LDM<0,MEMTYPE_DTCM,4,true>,
			MEMOP_LDM<0,MEMTYPE_ERAM,4,true>,
			MEMOP_LDM<0,MEMTYPE_SWIRAM,4,true>,
		},
		{
			MEMOP_LDM<1,MEMTYPE_GENERIC,4,true>,
			MEMOP_LDM<1,MEMTYPE_MAIN,4,true>,
			MEMOP_LDM<1,MEMTYPE_DTCM,4,true>,
			MEMOP_LDM<1,MEMTYPE_ERAM,4,true>,
			MEMOP_LDM<1,MEMTYPE_SWIRAM,4,true>,
		}
	};

	static const MemOp3 LDM_SEQUENCE_Down_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_GENERIC,2,false>,
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_MAIN,2,false>,
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_DTCM,2,false>,
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_ERAM,2,false>,
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_SWIRAM,2,false>,
		},
		{
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_GENERIC,2,false>,
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_MAIN,2,false>,
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_DTCM,2,false>,
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_ERAM,2,false>,
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_SWIRAM,2,false>,
		}
	};

	static const MemOp3 LDM_Down_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_LDM<0,MEMTYPE_GENERIC,2,false>,
			MEMOP_LDM<0,MEMTYPE_MAIN,2,false>,
			MEMOP_LDM<0,MEMTYPE_DTCM,2,false>,
			MEMOP_LDM<0,MEMTYPE_ERAM,2,false>,
			MEMOP_LDM<0,MEMTYPE_SWIRAM,2,false>,
		},
		{
			MEMOP_LDM<1,MEMTYPE_GENERIC,2,false>,
			MEMOP_LDM<1,MEMTYPE_MAIN,2,false>,
			MEMOP_LDM<1,MEMTYPE_DTCM,2,false>,
			MEMOP_LDM<1,MEMTYPE_ERAM,2,false>,
			MEMOP_LDM<1,MEMTYPE_SWIRAM,2,false>,
		}
	};

	static const MemOp3 LDM_SEQUENCE_Down_R15_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_GENERIC,4,false>,
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_MAIN,4,false>,
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_DTCM,4,false>,
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_ERAM,4,false>,
			MEMOP_LDM_SEQUENCE<0,MEMTYPE_SWIRAM,4,false>,
		},
		{
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_GENERIC,4,false>,
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_MAIN,4,false>,
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_DTCM,4,false>,
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_ERAM,4,false>,
			MEMOP_LDM_SEQUENCE<1,MEMTYPE_SWIRAM,4,false>,
		}
	};

	static const MemOp3 LDM_Down_R15_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_LDM<0,MEMTYPE_GENERIC,4,false>,
			MEMOP_LDM<0,MEMTYPE_MAIN,4,false>,
			MEMOP_LDM<0,MEMTYPE_DTCM,4,false>,
			MEMOP_LDM<0,MEMTYPE_ERAM,4,false>,
			MEMOP_LDM<0,MEMTYPE_SWIRAM,4,false>,
		},
		{
			MEMOP_LDM<1,MEMTYPE_GENERIC,4,false>,
			MEMOP_LDM<1,MEMTYPE_MAIN,4,false>,
			MEMOP_LDM<1,MEMTYPE_DTCM,4,false>,
			MEMOP_LDM<1,MEMTYPE_ERAM,4,false>,
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
			WRITE_CODE("u32 adr = (REG_R%s(0x%p) %c 4) & 0xFFFFFFFC;\n", REG_R(d.Rn), d.U ? '+' : '-');
		else
			WRITE_CODE("u32 adr = REG_R%s(0x%p) & 0xFFFFFFFC;\n", REG_R(d.Rn));

		if (d.S)
		{
			if (d.R15Modified)
			{
				//WRITE_CODE("((Status_Reg*)0x%p)->val=((Status_Reg*)0x%p)->val;\n", &(GETCPU.CPSR), &(GETCPU.SPSR));
			}
			else
				WRITE_CODE("u32 oldmode = ((u32 (*)(void*,u8))0x%p)(0x%p,%u);\n", armcpu_switchMode, GETCPUPTR, SYS);
		}

		if (IsOneSequence)
		{
			if (d.U)
			{
				if (d.R15Modified)
					WRITE_CODE("ExecuteCycles+=((u32 (FASTCALL *)(u32, u32, u32*))0x%p)(adr, %u,(u32*)0x%p);\n", LDM_SEQUENCE_Up_R15_Tab[PROCNUM][0], Count, Regs[0]);
				else
					WRITE_CODE("ExecuteCycles+=((u32 (FASTCALL *)(u32, u32, u32*))0x%p)(adr, %u,(u32*)0x%p);\n", LDM_SEQUENCE_Up_Tab[PROCNUM][0], Count, Regs[0]);
			}
			else
			{
				if (d.R15Modified)
					WRITE_CODE("ExecuteCycles+=((u32 (FASTCALL *)(u32, u32, u32*))0x%p)(adr, %u,(u32*)0x%p);\n", LDM_SEQUENCE_Down_R15_Tab[PROCNUM][0], Count, Regs[0]);
				else
					WRITE_CODE("ExecuteCycles+=((u32 (FASTCALL *)(u32, u32, u32*))0x%p)(adr, %u,(u32*)0x%p);\n", LDM_SEQUENCE_Down_Tab[PROCNUM][0], Count, Regs[0]);
			}
		}
		else
		{
			WRITE_CODE("static const u32* Regs[]={");
			for (u32 i = 0; i < Count; i++)
			{
				WRITE_CODE("0x%p", Regs[i]);
				if (i != Count - 1)
					WRITE_CODE(",");
			}
			WRITE_CODE("};\n");

			if (d.U)
			{
				if (d.R15Modified)
					WRITE_CODE("ExecuteCycles+=((u32 (FASTCALL *)(u32, u32, u32*))0x%p)(adr, %u,(u32*)&Regs[0]);\n", LDM_Up_R15_Tab[PROCNUM][0], Count);
				else
					WRITE_CODE("ExecuteCycles+=((u32 (FASTCALL *)(u32, u32, u32*))0x%p)(adr, %u,(u32*)&Regs[0]);\n", LDM_Up_Tab[PROCNUM][0], Count);
			}
			else
			{
				if (d.R15Modified)
					WRITE_CODE("ExecuteCycles+=((u32 (FASTCALL *)(u32, u32, u32*))0x%p)(adr, %u,(u32*)&Regs[0]);\n", LDM_Down_R15_Tab[PROCNUM][0], Count);
				else
					WRITE_CODE("ExecuteCycles+=((u32 (FASTCALL *)(u32, u32, u32*))0x%p)(adr, %u,(u32*)&Regs[0]);\n", LDM_Down_Tab[PROCNUM][0], Count);
			}
		}

		if (d.S)
		{
			if (NeedWriteBack)
				WRITE_CODE("REG_W(0x%p)=adr_old %c %u;\n", REG_W(d.Rn), d.U ? '+' : '-', Count*4);

			if (d.R15Modified)
			{
				LDM_S_LoadCPSRGenerate(d, szCodeBuffer);

				R15ModifiedGenerate(d, szCodeBuffer);
			}
			else
				WRITE_CODE("((u32 (*)(void*,u8))0x%p)(0x%p,oldmode);\n", armcpu_switchMode, GETCPUPTR);
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
				WRITE_CODE("REG_W(0x%p)=adr_old %c %u;\n", REG_W(d.Rn), d.U ? '+' : '-', Count*4);

			if (d.R15Modified)
				R15ModifiedGenerate(d, szCodeBuffer);
		}
	}

	template<u32 PROCNUM, u32 memtype, u32 cycle, bool up>
	static u32 FASTCALL MEMOP_STM_SEQUENCE(u32 adr, u32 count, u32 *regs)
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
				for (s32 i = (s32)count; i > 0; i--)
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
				for (s32 i = (s32)count; i > 0; i--)
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
				for (s32 i = (s32)count; i > 0; i--)
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
	static u32 FASTCALL MEMOP_STM(u32 adr, u32 count, u32 *regs_ptr)
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
				for (s32 i = (s32)count; i > 0; i--)
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
				for (s32 i = (s32)count; i > 0; i--)
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
			MEMOP_STM_SEQUENCE<0,MEMTYPE_DTCM,1,true>,
			MEMOP_STM_SEQUENCE<0,MEMTYPE_ERAM,1,true>,
			MEMOP_STM_SEQUENCE<0,MEMTYPE_SWIRAM,1,true>,
		},
		{
			MEMOP_STM_SEQUENCE<1,MEMTYPE_GENERIC,1,true>,
			MEMOP_STM_SEQUENCE<1,MEMTYPE_MAIN,1,true>,
			MEMOP_STM_SEQUENCE<1,MEMTYPE_DTCM,1,true>,
			MEMOP_STM_SEQUENCE<1,MEMTYPE_ERAM,1,true>,
			MEMOP_STM_SEQUENCE<1,MEMTYPE_SWIRAM,1,true>,
		}
	};

	static const MemOp3 STM_Up_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_STM<0,MEMTYPE_GENERIC,1,true>,
			MEMOP_STM<0,MEMTYPE_MAIN,1,true>,
			MEMOP_STM<0,MEMTYPE_DTCM,1,true>,
			MEMOP_STM<0,MEMTYPE_ERAM,1,true>,
			MEMOP_STM<0,MEMTYPE_SWIRAM,1,true>,
		},
		{
			MEMOP_STM<1,MEMTYPE_GENERIC,1,true>,
			MEMOP_STM<1,MEMTYPE_MAIN,1,true>,
			MEMOP_STM<1,MEMTYPE_DTCM,1,true>,
			MEMOP_STM<1,MEMTYPE_ERAM,1,true>,
			MEMOP_STM<1,MEMTYPE_SWIRAM,1,true>,
		}
	};

	static const MemOp3 STM_SEQUENCE_Down_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_STM_SEQUENCE<0,MEMTYPE_GENERIC,1,false>,
			MEMOP_STM_SEQUENCE<0,MEMTYPE_MAIN,1,false>,
			MEMOP_STM_SEQUENCE<0,MEMTYPE_DTCM,1,false>,
			MEMOP_STM_SEQUENCE<0,MEMTYPE_ERAM,1,false>,
			MEMOP_STM_SEQUENCE<0,MEMTYPE_SWIRAM,1,false>,
		},
		{
			MEMOP_STM_SEQUENCE<1,MEMTYPE_GENERIC,1,false>,
			MEMOP_STM_SEQUENCE<1,MEMTYPE_MAIN,1,false>,
			MEMOP_STM_SEQUENCE<1,MEMTYPE_DTCM,1,false>,
			MEMOP_STM_SEQUENCE<1,MEMTYPE_ERAM,1,false>,
			MEMOP_STM_SEQUENCE<1,MEMTYPE_SWIRAM,1,false>,
		}
	};

	static const MemOp3 STM_Down_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_STM<0,MEMTYPE_GENERIC,1,false>,
			MEMOP_STM<0,MEMTYPE_MAIN,1,false>,
			MEMOP_STM<0,MEMTYPE_DTCM,1,false>,
			MEMOP_STM<0,MEMTYPE_ERAM,1,false>,
			MEMOP_STM<0,MEMTYPE_SWIRAM,1,false>,
		},
		{
			MEMOP_STM<1,MEMTYPE_GENERIC,1,false>,
			MEMOP_STM<1,MEMTYPE_MAIN,1,false>,
			MEMOP_STM<1,MEMTYPE_DTCM,1,false>,
			MEMOP_STM<1,MEMTYPE_ERAM,1,false>,
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

		if (StoreR15)
			WRITE_CODE("REG_W(0x%p) = %u;\n", REG_W(15), d.CalcR15(d));

		bool IsOneSequence = (SequenceFlag == 1 || SequenceFlag == 2);

		if (d.W)
			WRITE_CODE("u32 adr_old = REG_R%s(0x%p);\n", REG_R(d.Rn));

		if (d.P)
			WRITE_CODE("u32 adr = (REG_R%s(0x%p) %c 4) & 0xFFFFFFFC;\n", REG_R(d.Rn), d.U ? '+' : '-');
		else
			WRITE_CODE("u32 adr = REG_R%s(0x%p) & 0xFFFFFFFC;\n", REG_R(d.Rn));

		if (d.S)
			WRITE_CODE("u32 oldmode = ((u32 (*)(void*,u8))0x%p)(0x%p,%u);\n", armcpu_switchMode, GETCPUPTR, SYS);

		if (IsOneSequence)
		{
			if (d.U)
				WRITE_CODE("ExecuteCycles+=((u32 (FASTCALL *)(u32, u32, u32*))0x%p)(adr, %u,(u32*)0x%p);\n", STM_SEQUENCE_Up_Tab[PROCNUM][0], Count, Regs[0]);
			else
				WRITE_CODE("ExecuteCycles+=((u32 (FASTCALL *)(u32, u32, u32*))0x%p)(adr, %u,(u32*)0x%p);\n", STM_SEQUENCE_Down_Tab[PROCNUM][0], Count, Regs[0]);
		}
		else
		{
			WRITE_CODE("static const u32* Regs[]={");
			for (u32 i = 0; i < Count; i++)
			{
				WRITE_CODE("0x%p", Regs[i]);
				if (i != Count - 1)
					WRITE_CODE(",");
			}
			WRITE_CODE("};\n");

			if (d.U)
				WRITE_CODE("ExecuteCycles+=((u32 (FASTCALL *)(u32, u32, u32*))0x%p)(adr, %u,(u32*)&Regs[0]);\n", STM_Up_Tab[PROCNUM][0], Count);
			else
				WRITE_CODE("ExecuteCycles+=((u32 (FASTCALL *)(u32, u32, u32*))0x%p)(adr, %u,(u32*)&Regs[0]);\n", STM_Down_Tab[PROCNUM][0], Count);
		}

		if (d.S)
		{
			if (d.W)
				WRITE_CODE("REG_W(0x%p)=adr_old %c %u;\n", REG_W(d.Rn), d.U ? '+' : '-', Count*4);

			WRITE_CODE("((u32 (*)(void*,u8))0x%p)(0x%p,oldmode);\n", armcpu_switchMode, GETCPUPTR);
		}
		else
		{
			if (d.W)
				WRITE_CODE("REG_W(0x%p)=adr_old %c %u;\n", REG_W(d.Rn), d.U ? '+' : '-', Count*4);
		}
	}

	template<u32 PROCNUM, u32 memtype, u32 cycle>
	static u32 FASTCALL MEMOP_SWP(u32 adr, u32 *Rd, u32 Rm)
	{
		u32 tmp = ROR(READ32(GETCPU.mem_if->data, adr), (adr & 3)<<3);
		WRITE32(GETCPU.mem_if->data, adr, Rm);
		*Rd = tmp;

		return MMU_aluMemCycles<PROCNUM>(cycle, MMU_memAccessCycles<PROCNUM,32,MMU_AD_READ>(adr) + MMU_memAccessCycles<PROCNUM,32,MMU_AD_WRITE>(adr));
	}

	template<u32 PROCNUM, u32 memtype, u32 cycle>
	static u32 FASTCALL MEMOP_SWPB(u32 adr, u32 *Rd, u32 Rm)
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
			MEMOP_SWP<0,MEMTYPE_DTCM,4>,
			MEMOP_SWP<0,MEMTYPE_ERAM,4>,
			MEMOP_SWP<0,MEMTYPE_SWIRAM,4>,
		},
		{
			MEMOP_SWP<1,MEMTYPE_GENERIC,4>,
			MEMOP_SWP<1,MEMTYPE_MAIN,4>,
			MEMOP_SWP<1,MEMTYPE_DTCM,4>,
			MEMOP_SWP<1,MEMTYPE_ERAM,4>,
			MEMOP_SWP<1,MEMTYPE_SWIRAM,4>,
		}
	};

	static const MemOp4 SWPB_Tab[2][MEMTYPE_COUNT] = 
	{
		{
			MEMOP_SWPB<0,MEMTYPE_GENERIC,4>,
			MEMOP_SWPB<0,MEMTYPE_MAIN,4>,
			MEMOP_SWPB<0,MEMTYPE_DTCM,4>,
			MEMOP_SWPB<0,MEMTYPE_ERAM,4>,
			MEMOP_SWPB<0,MEMTYPE_SWIRAM,4>,
		},
		{
			MEMOP_SWPB<1,MEMTYPE_GENERIC,4>,
			MEMOP_SWPB<1,MEMTYPE_MAIN,4>,
			MEMOP_SWPB<1,MEMTYPE_DTCM,4>,
			MEMOP_SWPB<1,MEMTYPE_ERAM,4>,
			MEMOP_SWPB<1,MEMTYPE_SWIRAM,4>,
		}
	};

	OPCDECODER_DECL(IR_SWP)
	{
		u32 PROCNUM = d.ProcessID;

		WRITE_CODE("u32 adr = REG_R%s(0x%p);\n", REG_R(d.Rn));

		if (d.B)
			WRITE_CODE("ExecuteCycles+=((u32 (FASTCALL *)(u32, u32*, u32))0x%p)(adr,REGPTR(0x%p),REG_R%s(0x%p));\n", SWPB_Tab[PROCNUM][0], REGPTR(d.Rd), REG_R(d.Rm));
		else
			WRITE_CODE("ExecuteCycles+=((u32 (FASTCALL *)(u32, u32*, u32))0x%p)(adr,REGPTR(0x%p),REG_R%s(0x%p));\n", SWP_Tab[PROCNUM][0], REGPTR(d.Rd), REG_R(d.Rm));
	}

	OPCDECODER_DECL(IR_B)
	{
	}

	OPCDECODER_DECL(IR_BL)
	{
	}

	OPCDECODER_DECL(IR_BX)
	{
	}

	OPCDECODER_DECL(IR_BLX)
	{
	}

	OPCDECODER_DECL(IR_SWI)
	{
	}

	OPCDECODER_DECL(IR_MSR)
	{
	}

	OPCDECODER_DECL(IR_MRS)
	{
	}

	OPCDECODER_DECL(IR_MCR)
	{
	}

	OPCDECODER_DECL(IR_MRC)
	{
	}

	OPCDECODER_DECL(IR_CLZ)
	{
	}

	OPCDECODER_DECL(IR_QADD)
	{
	}

	OPCDECODER_DECL(IR_QSUB)
	{
	}

	OPCDECODER_DECL(IR_QDADD)
	{
	}

	OPCDECODER_DECL(IR_QDSUB)
	{
	}

	OPCDECODER_DECL(IR_BLX_IMM)
	{
	}

	OPCDECODER_DECL(IR_BKPT)
	{
	}
};

static const IROpCDecoder iropcdecoder_set[IR_MAXNUM] = {
#define TABDECL(x) ArmCJit::x##_CDecoder
#include "ArmAnalyze_tabdef.inc"
#undef TABDECL
};

#endif
