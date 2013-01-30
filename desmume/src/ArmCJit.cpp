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

typedef u32 (FASTCALL* IROpCDecoder)(const Decoded &d, char *&szCodeBuffer);
#define TEMPLATE template<int PROCNUM> 

#define WRITE_CODE(...) szCodeBuffer += sprintf(szCodeBuffer, __VA_ARGS__)

namespace ArmCJit
{
	void IRShiftOpGenerate(const Decoded &d, bool clacCarry, char *&szCodeBuffer)
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
			break;
		}
	}

	void IROpGenerate(const Decoded &d, char *&szCodeBuffer)
	{
		switch (d.IROp)
		{
		default:
			INFO("Unknow Op : %d.\n", d.IROp);
			break;
		}
	}
};

static const IROpCDecoder iropcdecoder_set[IR_MAXNUM];

#endif
