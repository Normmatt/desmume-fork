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

#define TEMPLATE template<int PROCNUM> 
#define OPCDECODER_DECL(name) void FASTCALL name##_CDecoder(const Decoded &d, char *&szCodeBuffer)
#define WRITE_CODE(...) szCodeBuffer += sprintf(szCodeBuffer, __VA_ARGS__)

typedef void (FASTCALL* IROpCDecoder)(const Decoded &d, char *&szCodeBuffer);

namespace ArmCJit
{
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
						WRITE_CODE("u32 c = ((Status_Reg*)%d)->bits.C;\n", &(GETCPU.CPSR));
					else
						WRITE_CODE("u32 c = BIT_N((*(u32*)%d), %d);\n", &(GETCPU.R[d.Rm]), 32-d.Immediate);
				}

				if (d.Immediate == 0)
					WRITE_CODE("u32 shift_op = (*(u32*)%d);\n", &(GETCPU.R[d.Rm]));
				else
					WRITE_CODE("u32 shift_op = (*(u32*)%d)<<%d;\n", &(GETCPU.R[d.Rm]), d.Immediate);
			}
			else
			{
				if (clacCarry)
				{
					WRITE_CODE("u32 c;\n");
					WRITE_CODE("u32 shift_op = (*(u32*)%d)&0xFF;\n", &(GETCPU.R[d.Rs]));
					WRITE_CODE("if (shift_op == 0){\n");
					WRITE_CODE("c=((Status_Reg*)%d)->bits.C;\n", &(GETCPU.CPSR));
					WRITE_CODE("shift_op=(*(u32*)%d);\n", &(GETCPU.R[d.Rm]));
					WRITE_CODE("}else if (shift_op < 32){\n");
					WRITE_CODE("c = BIT_N((*(u32*)%d), 32-shift_op);\n", &(GETCPU.R[d.Rm]));
					WRITE_CODE("shift_op = (*(u32*)%d)<<shift_op;\n", &(GETCPU.R[d.Rm]));
					WRITE_CODE("}else if (shift_op == 32){\n");
					WRITE_CODE("c = BIT0((*(u32*)%d));\n", &(GETCPU.R[d.Rm]));
					WRITE_CODE("shift_op=0;\n");
					WRITE_CODE("}else{\n");
					WRITE_CODE("shift_op=c=0;}\n");
				}
				else
				{
					WRITE_CODE("u32 shift_op = (*(u32*)%d)&0xFF;\n", &(GETCPU.R[d.Rs]));
					WRITE_CODE("if (shift_op >= 32)\n");
					WRITE_CODE("shift_op=0;\n");
					WRITE_CODE("else\n");
					WRITE_CODE("shift_op=(*(u32*)%d)<<shift_op;\n", &(GETCPU.R[d.Rm]));
				}
			}
			break;
		case IRSHIFT_LSR:
			if (!d.R)
			{
				if (clacCarry)
				{
					if (d.Immediate == 0)
						WRITE_CODE("u32 c = BIT31((*(u32*)%d));\n", &(GETCPU.R[d.Rm]));
					else
						WRITE_CODE("u32 c = BIT_N((*(u32*)%d), %d);\n", &(GETCPU.R[d.Rm]), d.Immediate-1);
				}

				if (d.Immediate == 0)
					WRITE_CODE("u32 shift_op = 0;\n");
				else
					WRITE_CODE("u32 shift_op = (*(u32*)%d)>>%d;\n", &(GETCPU.R[d.Rm]), d.Immediate);
			}
			else
			{
				if (clacCarry)
				{
					WRITE_CODE("u32 c;\n");
					WRITE_CODE("u32 shift_op = (*(u32*)%d)&0xFF;\n", &(GETCPU.R[d.Rs]));
					WRITE_CODE("if (shift_op == 0){\n");
					WRITE_CODE("c=((Status_Reg*)%d)->bits.C;\n", &(GETCPU.CPSR));
					WRITE_CODE("shift_op=(*(u32*)%d);\n", &(GETCPU.R[d.Rm]));
					WRITE_CODE("}else if (shift_op < 32){\n");
					WRITE_CODE("c = BIT_N((*(u32*)%d), shift_op-1);\n", &(GETCPU.R[d.Rm]));
					WRITE_CODE("shift_op = (*(u32*)%d)>>shift_op;\n", &(GETCPU.R[d.Rm]));
					WRITE_CODE("}else if (shift_op == 32){\n");
					WRITE_CODE("c = BIT31((*(u32*)%d));\n", &(GETCPU.R[d.Rm]));
					WRITE_CODE("shift_op=0;\n");
					WRITE_CODE("}else{\n");
					WRITE_CODE("shift_op=c=0;}\n");
				}
				else
				{
					WRITE_CODE("u32 shift_op = (*(u32*)%d)&0xFF;\n", &(GETCPU.R[d.Rs]));
					WRITE_CODE("if (shift_op >= 32)\n");
					WRITE_CODE("shift_op=0;\n");
					WRITE_CODE("else\n");
					WRITE_CODE("shift_op=(*(u32*)%d)>>shift_op;\n", &(GETCPU.R[d.Rm]));
				}
			}
			break;
		case IRSHIFT_ASR:
			if (!d.R)
			{
				if (clacCarry)
				{
					if (d.Immediate == 0)
						WRITE_CODE("u32 c = BIT31((*(u32*)%d));\n", &(GETCPU.R[d.Rm]));
					else
						WRITE_CODE("u32 c = BIT_N((*(u32*)%d), %d);\n", &(GETCPU.R[d.Rm]), d.Immediate-1);
				}

				if (d.Immediate == 0)
					WRITE_CODE("u32 shift_op = BIT31((*(u32*)%d))*0xFFFFFFFF;\n", &(GETCPU.R[d.Rm]));
				else
					WRITE_CODE("u32 shift_op = (u32)((*(s32*)%d)>>%u);\n", &(GETCPU.R[d.Rm]), d.Immediate);
			}
			else
			{
				if (clacCarry)
				{
					WRITE_CODE("u32 c;\n");
					WRITE_CODE("u32 shift_op = (*(u32*)%d)&0xFF;\n", &(GETCPU.R[d.Rs]));
					WRITE_CODE("if (shift_op == 0){\n");
					WRITE_CODE("c=((Status_Reg*)%d)->bits.C;\n", &(GETCPU.CPSR));
					WRITE_CODE("shift_op = (*(u32*)%d);\n", &(GETCPU.R[d.Rm]));
					WRITE_CODE("}else if (shift_op < 32){\n");
					WRITE_CODE("c = BIT_N((*(u32*)%d), shift_op-1);\n", &(GETCPU.R[d.Rm]));
					WRITE_CODE("shift_op = (u32)((*(s32*)%d)>>shift_op);\n", &(GETCPU.R[d.Rm]));
					WRITE_CODE("}else{\n");
					WRITE_CODE("c = BIT31((*(u32*)%d));\n", &(GETCPU.R[d.Rm]));
					WRITE_CODE("shift_op = BIT31((*(u32*)%d))*0xFFFFFFFF;}\n", &(GETCPU.R[d.Rm]));
				}
				else
				{
					WRITE_CODE("u32 shift_op = (*(u32*)%d)&0xFF;\n", &(GETCPU.R[d.Rs]));
					WRITE_CODE("if (shift_op == 0)\n");
					WRITE_CODE("shift_op = (*(u32*)%d);\n", &(GETCPU.R[d.Rm]));
					WRITE_CODE("else if (shift_op < 32)\n");
					WRITE_CODE("shift_op = (u32)((*(s32*)%d)>>shift_op);\n", &(GETCPU.R[d.Rm]));
					WRITE_CODE("else\n");
					WRITE_CODE("shift_op = BIT31((*(u32*)%d))*0xFFFFFFFF;\n", &(GETCPU.R[d.Rm]));
				}
			}
			break;
		case IRSHIFT_ROR:
			if (!d.R)
			{
				if (clacCarry)
				{
					if (d.Immediate == 0)
						WRITE_CODE("u32 c = BIT0((*(u32*)%d));\n", &(GETCPU.R[d.Rm]));
					else
						WRITE_CODE("u32 c = BIT_N((*(u32*)%d), %d);\n", &(GETCPU.R[d.Rm]), d.Immediate-1);
				}

				if (d.Immediate == 0)
					WRITE_CODE("u32 shift_op = (((u32)((Status_Reg*)%d)->bits.C)<<31)|((*(u32*)%d)>>1);\n", &(GETCPU.CPSR), &(GETCPU.R[d.Rm]));
				else
					WRITE_CODE("u32 shift_op = ROR((*(u32*)%d), %d);\n", &(GETCPU.R[d.Rm]), d.Immediate);
			}
			else
			{
				if (clacCarry)
				{
					WRITE_CODE("u32 c;\n");
					WRITE_CODE("u32 shift_op = (*(u32*)%d)&0xFF;\n", &(GETCPU.R[d.Rs]));
					WRITE_CODE("if (shift_op == 0){\n");
					WRITE_CODE("c=((Status_Reg*)%d)->bits.C;\n", &(GETCPU.CPSR));
					WRITE_CODE("shift_op = (*(u32*)%d);\n", &(GETCPU.R[d.Rm]));
					WRITE_CODE("}else{\n");
					WRITE_CODE("shift_op &= 0x1F;\n");
					WRITE_CODE("if (shift_op != 0){\n");
					WRITE_CODE("c = BIT_N((*(u32*)%d), shift_op-1);\n", &(GETCPU.R[d.Rm]));
					WRITE_CODE("shift_op = ROR((*(u32*)%d), shift_op);\n", &(GETCPU.R[d.Rm]));
					WRITE_CODE("}else{\n");
					WRITE_CODE("c = BIT31((*(u32*)%d));\n", &(GETCPU.R[d.Rm]));
					WRITE_CODE("shift_op = (*(u32*)%d);}}\n", &(GETCPU.R[d.Rm]));
				
				}
				else
				{
					WRITE_CODE("u32 shift_op = (*(u32*)%d)&0x1F;\n", &(GETCPU.R[d.Rs]));
					WRITE_CODE("if (shift_op == 0)\n");
					WRITE_CODE("shift_op = (*(u32*)%d);\n", &(GETCPU.R[d.Rm]));
					WRITE_CODE("else\n");
					WRITE_CODE("shift_op = ROR((*(u32*)%d), shift_op);\n", &(GETCPU.R[d.Rm]));
				}
			}
			break;
		default:
			INFO("Unknow Shift Op : %d.\n", d.Typ);
			if (clacCarry)
				WRITE_CODE("u32 c = 0;\n");
			WRITE_CODE("u32 shift_op = 0;\n");
			break;
		}
	}

	void FASTCALL DataProcessLoadCPSRGenerate(const Decoded &d, char *&szCodeBuffer)
	{
	}

	void FASTCALL R15ModifiedGenerate(const Decoded &d, char *&szCodeBuffer)
	{
	}

	OPCDECODER_DECL(IR_UND)
	{
		u32 PROCNUM = d.ProcessID;

		WRITE_CODE("TRAPUNDEF((armcpu_t*)%d);\n", GETCPUPTR);
	}

	OPCDECODER_DECL(IR_NOP)
	{
	}

	OPCDECODER_DECL(IR_MOV)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.I)
		{
			WRITE_CODE("(*(u32*)%d)=%d;\n", &(GETCPU.R[d.Rd]), d.Immediate);
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)%d)->bits.C=%d;\n", &(GETCPU.CPSR), BIT31(d.Immediate));
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)%d)->bits.N=%d;\n", &(GETCPU.CPSR), BIT31(d.Immediate));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)%d)->bits.Z=%d;\n", &(GETCPU.CPSR), d.Immediate==0 ? 1 : 0);
			}
		}
		else
		{
			const bool clacCarry = d.S && !d.R15Modified && (d.FlagsSet & FLAG_C);
			IRShiftOpGenerate(d, szCodeBuffer, clacCarry);

			WRITE_CODE("(*(u32*)%d)=shift_op;\n", &(GETCPU.R[d.Rd]));
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)%d)->bits.C=c;\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)%d)->bits.N=BIT31(shift_op);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)%d)->bits.Z=(shift_op==0);\n", &(GETCPU.CPSR));
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
			WRITE_CODE("(*(u32*)%d)=%d;\n", &(GETCPU.R[d.Rd]), ~d.Immediate);
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)%d)->bits.C=%d;\n", &(GETCPU.CPSR), BIT31(d.Immediate));
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)%d)->bits.N=%d;\n", &(GETCPU.CPSR), BIT31(~d.Immediate));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)%d)->bits.Z=%d;\n", &(GETCPU.CPSR), (~d.Immediate)==0 ? 1 : 0);
			}
		}
		else
		{
			const bool clacCarry = d.S && !d.R15Modified && (d.FlagsSet & FLAG_C);
			IRShiftOpGenerate(d, szCodeBuffer, clacCarry);

			WRITE_CODE("shift_op=(*(u32*)%d)=~shift_op;\n", &(GETCPU.R[d.Rd]));
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)%d)->bits.C=c;\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)%d)->bits.N=BIT31(shift_op);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)%d)->bits.Z=(shift_op==0);\n", &(GETCPU.CPSR));
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
			WRITE_CODE("u32 shift_op=(*(u32*)%d)=(*(u32*)%d)&%d;\n", &(GETCPU.R[d.Rd]), &(GETCPU.R[d.Rn]), d.Immediate);
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)%d)->bits.C=%d;\n", &(GETCPU.CPSR), BIT31(d.Immediate));
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)%d)->bits.N=BIT31(shift_op);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)%d)->bits.Z=(shift_op==0);\n", &(GETCPU.CPSR));
			}
		}
		else
		{
			const bool clacCarry = d.S && !d.R15Modified && (d.FlagsSet & FLAG_C);
			IRShiftOpGenerate(d, szCodeBuffer, clacCarry);

			WRITE_CODE("shift_op=(*(u32*)%d)=(*(u32*)%d)&shift_op;\n", &(GETCPU.R[d.Rd]), &(GETCPU.R[d.Rn]));
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)%d)->bits.C=c;\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)%d)->bits.N=BIT31(shift_op);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)%d)->bits.Z=(shift_op==0);\n", &(GETCPU.CPSR));
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
			WRITE_CODE("u32 shift_op=(*(u32*)%d)&%d;\n", &(GETCPU.R[d.Rn]), d.Immediate);

			{
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)%d)->bits.C=%d;\n", &(GETCPU.CPSR), BIT31(d.Immediate));
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)%d)->bits.N=BIT31(shift_op);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)%d)->bits.Z=(shift_op==0);\n", &(GETCPU.CPSR));
			}
		}
		else
		{
			const bool clacCarry = d.S && !d.R15Modified && (d.FlagsSet & FLAG_C);
			IRShiftOpGenerate(d, szCodeBuffer, clacCarry);

			WRITE_CODE("shift_op=(*(u32*)%d)&shift_op;\n", &(GETCPU.R[d.Rn]));
			
			{
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)%d)->bits.C=c;\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)%d)->bits.N=BIT31(shift_op);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)%d)->bits.Z=(shift_op==0);\n", &(GETCPU.CPSR));
			}
		}
	}

	OPCDECODER_DECL(IR_EOR)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.I)
		{
			WRITE_CODE("u32 shift_op=(*(u32*)%d)=(*(u32*)%d)^%d;\n", &(GETCPU.R[d.Rd]), &(GETCPU.R[d.Rn]), d.Immediate);
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)%d)->bits.C=%d;\n", &(GETCPU.CPSR), BIT31(d.Immediate));
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)%d)->bits.N=BIT31(shift_op);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)%d)->bits.Z=(shift_op==0);\n", &(GETCPU.CPSR));
			}
		}
		else
		{
			const bool clacCarry = d.S && !d.R15Modified && (d.FlagsSet & FLAG_C);
			IRShiftOpGenerate(d, szCodeBuffer, clacCarry);

			WRITE_CODE("shift_op=(*(u32*)%d)=(*(u32*)%d)^shift_op;\n", &(GETCPU.R[d.Rd]), &(GETCPU.R[d.Rn]));
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)%d)->bits.C=c;\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)%d)->bits.N=BIT31(shift_op);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)%d)->bits.Z=(shift_op==0);\n", &(GETCPU.CPSR));
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
			WRITE_CODE("u32 shift_op=(*(u32*)%d)^%d;\n", &(GETCPU.R[d.Rn]), d.Immediate);

			{
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)%d)->bits.C=%d;\n", &(GETCPU.CPSR), BIT31(d.Immediate));
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)%d)->bits.N=BIT31(shift_op);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)%d)->bits.Z=(shift_op==0);\n", &(GETCPU.CPSR));
			}
		}
		else
		{
			const bool clacCarry = d.S && !d.R15Modified && (d.FlagsSet & FLAG_C);
			IRShiftOpGenerate(d, szCodeBuffer, clacCarry);

			WRITE_CODE("shift_op=(*(u32*)%d)^shift_op;\n", &(GETCPU.R[d.Rn]));
			
			{
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)%d)->bits.C=c;\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)%d)->bits.N=BIT31(shift_op);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)%d)->bits.Z=(shift_op==0);\n", &(GETCPU.CPSR));
			}
		}
	}

	OPCDECODER_DECL(IR_ORR)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.I)
		{
			WRITE_CODE("u32 shift_op=(*(u32*)%d)=(*(u32*)%d)|%d;\n", &(GETCPU.R[d.Rd]), &(GETCPU.R[d.Rn]), d.Immediate);
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)%d)->bits.C=%d;\n", &(GETCPU.CPSR), BIT31(d.Immediate));
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)%d)->bits.N=BIT31(shift_op);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)%d)->bits.Z=(shift_op==0);\n", &(GETCPU.CPSR));
			}
		}
		else
		{
			const bool clacCarry = d.S && !d.R15Modified && (d.FlagsSet & FLAG_C);
			IRShiftOpGenerate(d, szCodeBuffer, clacCarry);

			WRITE_CODE("shift_op=(*(u32*)%d)=(*(u32*)%d)|shift_op;\n", &(GETCPU.R[d.Rd]), &(GETCPU.R[d.Rn]));
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)%d)->bits.C=c;\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)%d)->bits.N=BIT31(shift_op);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)%d)->bits.Z=(shift_op==0);\n", &(GETCPU.CPSR));
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
			WRITE_CODE("u32 shift_op=(*(u32*)%d)=(*(u32*)%d)&%d;\n", &(GETCPU.R[d.Rd]), &(GETCPU.R[d.Rn]), ~d.Immediate);
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)%d)->bits.C=%d;\n", &(GETCPU.CPSR), BIT31(d.Immediate));
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)%d)->bits.N=BIT31(shift_op);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)%d)->bits.Z=(shift_op==0);\n", &(GETCPU.CPSR));
			}
		}
		else
		{
			const bool clacCarry = d.S && !d.R15Modified && (d.FlagsSet & FLAG_C);
			IRShiftOpGenerate(d, szCodeBuffer, clacCarry);

			WRITE_CODE("shift_op=(*(u32*)%d)=(*(u32*)%d)&(~shift_op);\n", &(GETCPU.R[d.Rd]), &(GETCPU.R[d.Rn]));
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)%d)->bits.C=c;\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)%d)->bits.N=BIT31(shift_op);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)%d)->bits.Z=(shift_op==0);\n", &(GETCPU.CPSR));
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
				WRITE_CODE("u32 v=(*(u32*)%d);\n", &(GETCPU.R[d.Rn]));
			WRITE_CODE("(*(u32*)%d)=(*(u32*)%d)+%d;\n", &(GETCPU.R[d.Rd]), &(GETCPU.R[d.Rn]), d.Immediate);
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)%d)->bits.N=BIT31((*(u32*)%d));\n", &(GETCPU.CPSR), &(GETCPU.R[d.Rd]));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)%d)->bits.Z=((*(u32*)%d)==0);\n", &(GETCPU.CPSR), &(GETCPU.R[d.Rd]));
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)%d)->bits.C=CarryFrom(v, %d);\n", &(GETCPU.CPSR), d.Immediate);
				if (d.FlagsSet & FLAG_V)
					WRITE_CODE("((Status_Reg*)%d)->bits.V=OverflowFromADD((*(u32*)%d), v, %d);\n", &(GETCPU.CPSR), &(GETCPU.R[d.Rd]), d.Immediate);
			}
		}
		else
		{
			IRShiftOpGenerate(d, szCodeBuffer, false);

			if (d.S && !d.R15Modified && ((d.FlagsSet & FLAG_C) || (d.FlagsSet & FLAG_V)))
				WRITE_CODE("u32 v=(*(u32*)%d);\n", &(GETCPU.R[d.Rn]));
			WRITE_CODE("shift_op=(*(u32*)%d)=(*(u32*)%d)+shift_op;\n", &(GETCPU.R[d.Rd]), &(GETCPU.R[d.Rn]));
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_N)
					WRITE_CODE("((Status_Reg*)%d)->bits.N=BIT31((*(u32*)%d));\n", &(GETCPU.CPSR), &(GETCPU.R[d.Rd]));
				if (d.FlagsSet & FLAG_Z)
					WRITE_CODE("((Status_Reg*)%d)->bits.Z=((*(u32*)%d)==0);\n", &(GETCPU.CPSR), &(GETCPU.R[d.Rd]));
				if (d.FlagsSet & FLAG_C)
					WRITE_CODE("((Status_Reg*)%d)->bits.C=CarryFrom(v, shift_op);\n", &(GETCPU.CPSR));
				if (d.FlagsSet & FLAG_V)
					WRITE_CODE("((Status_Reg*)%d)->bits.V=OverflowFromADD((*(u32*)%d), v, shift_op);\n", &(GETCPU.CPSR), &(GETCPU.R[d.Rd]));
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
	}

	OPCDECODER_DECL(IR_SUB)
	{
	}

	OPCDECODER_DECL(IR_SBC)
	{
	}

	OPCDECODER_DECL(IR_RSB)
	{
	}

	OPCDECODER_DECL(IR_RSC)
	{
	}

	OPCDECODER_DECL(IR_CMP)
	{
	}

	OPCDECODER_DECL(IR_CMN)
	{
	}

	OPCDECODER_DECL(IR_MUL)
	{
	}

	OPCDECODER_DECL(IR_MLA)
	{
	}

	OPCDECODER_DECL(IR_UMULL)
	{
	}

	OPCDECODER_DECL(IR_UMLAL)
	{
	}

	OPCDECODER_DECL(IR_SMULL)
	{
	}

	OPCDECODER_DECL(IR_SMLAL)
	{
	}

	OPCDECODER_DECL(IR_SMULxy)
	{
	}

	OPCDECODER_DECL(IR_SMLAxy)
	{
	}

	OPCDECODER_DECL(IR_SMULWy)
	{
	}

	OPCDECODER_DECL(IR_SMLAWy)
	{
	}

	OPCDECODER_DECL(IR_SMLALxy)
	{
	}

	OPCDECODER_DECL(IR_LDR)
	{
	}

	OPCDECODER_DECL(IR_STR)
	{
	}

	OPCDECODER_DECL(IR_LDRx)
	{
	}

	OPCDECODER_DECL(IR_STRx)
	{
	}

	OPCDECODER_DECL(IR_LDRD)
	{
	}

	OPCDECODER_DECL(IR_STRD)
	{
	}

	OPCDECODER_DECL(IR_LDREX)
	{
	}

	OPCDECODER_DECL(IR_STREX)
	{
	}

	OPCDECODER_DECL(IR_LDM)
	{
	}

	OPCDECODER_DECL(IR_STM)
	{
	}

	OPCDECODER_DECL(IR_SWP)
	{
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
