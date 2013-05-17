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
#include <stddef.h>

#include "ArmLJit.h"
#include "ArmAnalyze.h"
#include "instructions.h"
#include "Disassembler.h"

#include "armcpu.h"
#include "MMU.h"
#include "MMU_timing.h"
#include "JitCommon.h"
#include "utils/MemBuffer.h"
#include "utils/lightning/lightning.h"

#ifdef HAVE_JIT

#define GETCPUPTR (&ARMPROC)
#define GETCPU (ARMPROC)

#define TEMPLATE template<u32 PROCNUM> 
#define OPDECODER_DECL(name) void FASTCALL name##_Decoder(const Decoded &d, RegisterMap &regMap)

typedef void (FASTCALL* IROpDecoder)(const Decoded &d, RegisterMap &regMap);
typedef u32 (* ArmOpCompiled)();
typedef u32 (FASTCALL* Interpreter)(const Decoded &d);

#define HWORD(i)   ((s32)(((s32)(i))>>16))
#define LWORD(i)   (s32)(((s32)((i)<<16))>>16)

namespace ArmLJit
{
//------------------------------------------------------------
//                         RegisterMap Imp
//------------------------------------------------------------
	enum LocalRegType
	{
		LRT_R,
		LRT_V,
	};

	static jit_gpr_t LocalMap[JIT_V_NUM + JIT_R_NUM];

	FORCEINLINE jit_gpr_t LOCALREG_INIT()
	{
		for (u32 i = 0; i < JIT_V_NUM; i++)
			LocalMap[i] = JIT_V(i);

		for (u32 i = JIT_V_NUM; i < JIT_V_NUM + JIT_R_NUM; i++)
			LocalMap[i] = JIT_R(i);
	}

	FORCEINLINE jit_gpr_t LOCALREG(u32 i)
	{
		return LocalMap[i];
	}

	FORCEINLINE LocalRegType LOCALREGTYPE(u32 i)
	{
		if (i < JIT_V_NUM)
			return LRT_V;

		return LRT_R;
	}

	FORCEINLINE RegisterMap::GuestRegId REGID(u32 i)
	{
		return (RegisterMap::GuestRegId)(i);
	}

	class RegisterMapImp : public RegisterMap
	{
		public:
			void CallABI(void* funptr, 
						const std::vector<ABIOp> &args, 
						const std::vector<GuestRegId> &flushs, 
						u32 hostreg_ret, 
						ImmData::Type type_ret);

		protected:
			void StartBlock();
			void EndBlock();

			void StoreGuestReg(u32 hostreg, GuestRegId guestreg);
			void LoadGuestReg(u32 hostreg, GuestRegId guestreg);

			void StoreImm(GuestRegId guestreg, const ImmData &data);
			void LoadImm(u32 hostreg, const ImmData &data);

			bool IsPerdureHostReg(u32 hostreg);

		private:
			int m_StackExecyc;
			int m_StackCpuptr;
	};

	void RegisterMapImp::CallABI(void* funptr, 
								const std::vector<ABIOp> &args, 
								const std::vector<GuestRegId> &flushs, 
								u32 hostreg_ret, 
								ImmData::Type type_ret)
	{
		jit_prepare(args.size());

		for (size_t i = 0; i < args.size(); i++)
		{
			const ABIOp &Op = args[i];

			switch (Op.type)
			{
			case ABIOp::IMM:
				{
					u32 tmp = AllocTempReg();

					switch (Op.immdata.type)
					{
					case ImmData::IMM8:
						jit_movi_ui(LOCALREG(tmp), Op.immdata.imm8);
						jit_pusharg_uc(LOCALREG(tmp));
						break;
					case ImmData::IMM16:
						jit_movi_ui(LOCALREG(tmp), Op.immdata.imm16);
						jit_pusharg_us(LOCALREG(tmp));
						break;
					case ImmData::IMM32:
						jit_movi_ui(LOCALREG(tmp), Op.immdata.imm32);
						jit_pusharg_ui(LOCALREG(tmp));
						break;
					case ImmData::IMMPTR:
						jit_movi_p(LOCALREG(tmp), Op.immdata.immptr);
						jit_pusharg_p(LOCALREG(tmp));
						break;

					default:
						break;
					}

					ReleaseTempReg(tmp);
				}
				break;

			case ABIOp::GUSETREG:
				{
					u32 reg = MapReg((GuestRegId)Op.regdata);
					Lock(reg);

					jit_pusharg_ui(LOCALREG(reg));

					Unlock(reg);
				}
				break;

			case ABIOp::HOSTREG:
				jit_pusharg_ui(LOCALREG(Op.regdata));
				break;

			case ABIOp::TEMPREG:
				{
					u32 tmp = Op.regdata;

					jit_pusharg_ui(LOCALREG(tmp));
					ReleaseTempReg(tmp);
				}
				break;

			default:
				break;
			}
		}

		for (size_t i = 0; i < flushs.size(); i++)
		{
			FlushGuestReg(flushs[i]);
		}

		for (u32 i = 0; i < m_HostRegCount; i++)
		{
			if (m_State.HostRegs[i].alloced && !IsPerdureHostReg(i))
				FlushHostReg(i);
		}

		jit_finish(funptr);

		if (hostreg_ret != INVALID_REG_ID)
		{
			switch (type_ret)
			{
			case ImmData::IMM8:
				jit_retval_uc(LOCALREG(hostreg_ret));
				break;
			case ImmData::IMM16:
				jit_retval_us(LOCALREG(hostreg_ret));
				break;
			case ImmData::IMM32:
				jit_retval_ui(LOCALREG(hostreg_ret));
				break;
			case ImmData::IMMPTR:
				jit_retval_p(LOCALREG(hostreg_ret));
				break;

			default:
				break;
			}
		}
	}

	void RegisterMapImp::StartBlock()
	{
		jit_prolog(0);

		m_StackExecyc = jit_allocai(sizeof(u32));
		m_StackCpuptr = jit_allocai(sizeof(armcpu_t*));

		SetImm32(EXECUTECYCLES, 0);
		SetImmPtr(CPUPTR, m_Cpu);
	}

	void RegisterMapImp::EndBlock()
	{
		if (IsImm(EXECUTECYCLES))
		{
			jit_movi_ui(JIT_RET, GetImm32(EXECUTECYCLES));
			jit_ret();
		}
		else
		{
			u32 execyc = MapReg(EXECUTECYCLES);
			Lock(execyc);

			jit_movr_ui(JIT_RET, LOCALREG(execyc));
			jit_ret();

			Unlock(execyc);
		}
	}

	void RegisterMapImp::StoreGuestReg(u32 hostreg, GuestRegId guestreg)
	{
		if (guestreg >= R0 && guestreg <= SPSR)
		{
			u32 cpuptr = MapReg(CPUPTR);
			Lock(cpuptr);

			if (guestreg >= R0 && guestreg <= R15)
				jit_stxi_ui(offsetof(armcpu_t, R[guestreg]), LOCALREG(cpuptr), LOCALREG(hostreg));
			else if (guestreg == CPSR)
				jit_stxi_ui(offsetof(armcpu_t, CPSR), LOCALREG(cpuptr), LOCALREG(hostreg));
			else if (guestreg == SPSR)
				jit_stxi_ui(offsetof(armcpu_t, SPSR), LOCALREG(cpuptr), LOCALREG(hostreg));

			Unlock(cpuptr);
		}
		else if (guestreg == EXECUTECYCLES)
		{
			jit_stxi_ui(m_StackExecyc, JIT_FP, LOCALREG(hostreg));
		}
		else if (guestreg == CPUPTR)
		{
			jit_stxi_ui(m_StackCpuptr, JIT_FP, LOCALREG(hostreg));
		}
	}

	void RegisterMapImp::LoadGuestReg(u32 hostreg, GuestRegId guestreg)
	{
		if (guestreg >= R0 && guestreg <= SPSR)
		{
			u32 cpuptr = MapReg(CPUPTR);
			Lock(cpuptr);

			if (guestreg >= R0 && guestreg <= R15)
				jit_ldxi_ui(LOCALREG(hostreg), LOCALREG(cpuptr), offsetof(armcpu_t, R[guestreg]));
			else if (guestreg == CPSR)
				jit_ldxi_ui(LOCALREG(hostreg), LOCALREG(cpuptr), offsetof(armcpu_t, CPSR));
			else if (guestreg == SPSR)
				jit_ldxi_ui(LOCALREG(hostreg), LOCALREG(cpuptr), offsetof(armcpu_t, SPSR));

			Unlock(cpuptr);
		}
		else if (guestreg == EXECUTECYCLES)
		{
			jit_ldxi_ui(LOCALREG(hostreg), JIT_FP, m_StackExecyc);
		}
		else if (guestreg == CPUPTR)
		{
			jit_ldxi_ui(LOCALREG(hostreg), JIT_FP, m_StackCpuptr);
		}
	}

	void RegisterMapImp::StoreImm(GuestRegId guestreg, const ImmData &data)
	{
		u32 tmp = AllocTempReg();

		switch (data.type)
		{
		case ImmData::IMM8:
			jit_movi_ui(LOCALREG(tmp), data.imm8);
			break;
		case ImmData::IMM16:
			jit_movi_ui(LOCALREG(tmp), data.imm16);
			break;
		case ImmData::IMM32:
			jit_movi_ui(LOCALREG(tmp), data.imm32);
			break;
		case ImmData::IMMPTR:
			jit_movi_p(LOCALREG(tmp), data.immptr);
			break;

		default:
			break;
		}
		StoreGuestReg(tmp, guestreg);

		ReleaseTempReg(tmp);
	}

	void RegisterMapImp::LoadImm(u32 hostreg, const ImmData &data)
	{
		switch (data.type)
		{
		case ImmData::IMM8:
			jit_movi_ui(LOCALREG(hostreg), data.imm8);
			break;
		case ImmData::IMM16:
			jit_movi_ui(LOCALREG(hostreg), data.imm16);
			break;
		case ImmData::IMM32:
			jit_movi_ui(LOCALREG(hostreg), data.imm32);
			break;
		case ImmData::IMMPTR:
			jit_movi_p(LOCALREG(hostreg), data.immptr);
			break;

		default:
			break;
		}
	}

	bool RegisterMapImp::IsPerdureHostReg(u32 hostreg)
	{
		return LOCALREGTYPE(hostreg) == LRT_V;
	}

//------------------------------------------------------------
//                         Memory type
//------------------------------------------------------------

//------------------------------------------------------------
//                         Help function
//------------------------------------------------------------
#ifdef WORDS_BIGENDIAN
	static const u32 PSR_MODE_BITSHIFT = 27;
	static const u32 PSR_MODE_BITMASK = 0xF8000000;
	static const u32 PSR_T_BITSHIFT = 26;
	static const u32 PSR_T_BITMASK = 1 << PSR_T_BITSHIFT;
	static const u32 PSR_F_BITSHIFT = 25;
	static const u32 PSR_F_BITMASK = 1 << PSR_F_BITSHIFT;
	static const u32 PSR_I_BITSHIFT = 24;
	static const u32 PSR_I_BITMASK = 1 << PSR_I_BITSHIFT;
	static const u32 PSR_Q_BITSHIFT = 4;
	static const u32 PSR_Q_BITMASK = 1 << PSR_Q_BITSHIFT;
	static const u32 PSR_V_BITSHIFT = 3;
	static const u32 PSR_V_BITMASK = 1 << PSR_V_BITSHIFT;
	static const u32 PSR_C_BITSHIFT = 2;
	static const u32 PSR_C_BITMASK = 1 << PSR_C_BITSHIFT;
	static const u32 PSR_Z_BITSHIFT = 1;
	static const u32 PSR_Z_BITMASK = 1 << PSR_Z_BITSHIFT;
	static const u32 PSR_N_BITSHIFT = 0;
	static const u32 PSR_N_BITMASK = 1 << PSR_N_BITSHIFT;
#else
	static const u32 PSR_MODE_BITSHIFT = 0;
	static const u32 PSR_MODE_BITMASK = 31;
	static const u32 PSR_T_BITSHIFT = 5;
	static const u32 PSR_T_BITMASK = 1 << PSR_T_BITSHIFT;
	static const u32 PSR_F_BITSHIFT = 6;
	static const u32 PSR_F_BITMASK = 1 << PSR_F_BITSHIFT;
	static const u32 PSR_I_BITSHIFT = 7;
	static const u32 PSR_I_BITMASK = 1 << PSR_I_BITSHIFT;
	static const u32 PSR_Q_BITSHIFT = 27;
	static const u32 PSR_Q_BITMASK = 1 << PSR_Q_BITSHIFT;
	static const u32 PSR_V_BITSHIFT = 28;
	static const u32 PSR_V_BITMASK = 1 << PSR_V_BITSHIFT;
	static const u32 PSR_C_BITSHIFT = 29;
	static const u32 PSR_C_BITMASK = 1 << PSR_C_BITSHIFT;
	static const u32 PSR_Z_BITSHIFT = 30;
	static const u32 PSR_Z_BITMASK = 1 << PSR_Z_BITSHIFT;
	static const u32 PSR_N_BITSHIFT = 31;
	static const u32 PSR_N_BITMASK = 1 << PSR_N_BITSHIFT;
#endif

	enum ARMCPU_PSR
	{
		PSR_MODE,
		PSR_T,
		PSR_F,
		PSR_I,
		PSR_Q,
		PSR_V,
		PSR_C,
		PSR_Z,
		PSR_N,
	};

	void FASTCALL UnpackPSR(ARMCPU_PSR flg, u32 in, u32 out)
	{
		u32 shift, mask;

		switch (flg)
		{
		case PSR_MODE:
			shift = PSR_MODE_BITSHIFT;
			mask = PSR_MODE_BITMASK;
			break;
		case PSR_T:
			shift = PSR_T_BITSHIFT;
			mask = PSR_T_BITMASK;
			break;
		case PSR_F:
			shift = PSR_F_BITSHIFT;
			mask = PSR_F_BITMASK;
			break;
		case PSR_I:
			shift = PSR_I_BITSHIFT;
			mask = PSR_I_BITMASK;
			break;
		case PSR_Q:
			shift = PSR_Q_BITSHIFT;
			mask = PSR_Q_BITMASK;
			break;
		case PSR_V:
			shift = PSR_V_BITSHIFT;
			mask = PSR_V_BITMASK;
			break;
		case PSR_C:
			shift = PSR_C_BITSHIFT;
			mask = PSR_C_BITMASK;
			break;
		case PSR_Z:
			shift = PSR_Z_BITSHIFT;
			mask = PSR_Z_BITMASK;
			break;
		case PSR_N:
			shift = PSR_N_BITSHIFT;
			mask = PSR_N_BITMASK;
			break;

		default:
			shift = 0;
			mask = 0;
			break;
		}

		if (mask != (1 << 31) || shift != 31)
		{
			jit_andi_ui(LOCALREG(out), LOCALREG(in), mask);
			in = out;
		}
		if (shift != 0)
			jit_rshi_ui(LOCALREG(out), LOCALREG(in), shift);
	}

	void FASTCALL UnpackCPSR(RegisterMap &regMap, ARMCPU_PSR flg, u32 out)
	{
		u32 cpsr = regMap.MapReg(RegisterMap::CPSR);
		regMap.Lock(cpsr);

		UnpackPSR(flg, cpsr, out);

		regMap.Unlock(cpsr);
	}

	void FASTCALL PackCPSR(RegisterMap &regMap, ARMCPU_PSR flg, u32 in, bool tmp_in = true)
	{
		u32 shift, mask;

		switch (flg)
		{
		case PSR_MODE:
			shift = PSR_MODE_BITSHIFT;
			mask = PSR_MODE_BITMASK;
			break;
		case PSR_T:
			shift = PSR_T_BITSHIFT;
			mask = PSR_T_BITMASK;
			break;
		case PSR_F:
			shift = PSR_F_BITSHIFT;
			mask = PSR_F_BITMASK;
			break;
		case PSR_I:
			shift = PSR_I_BITSHIFT;
			mask = PSR_I_BITMASK;
			break;
		case PSR_Q:
			shift = PSR_Q_BITSHIFT;
			mask = PSR_Q_BITMASK;
			break;
		case PSR_V:
			shift = PSR_V_BITSHIFT;
			mask = PSR_V_BITMASK;
			break;
		case PSR_C:
			shift = PSR_C_BITSHIFT;
			mask = PSR_C_BITMASK;
			break;
		case PSR_Z:
			shift = PSR_Z_BITSHIFT;
			mask = PSR_Z_BITMASK;
			break;
		case PSR_N:
			shift = PSR_N_BITSHIFT;
			mask = PSR_N_BITMASK;
			break;

		default:
			shift = 0;
			mask = 0;
			break;
		}

		u32 cpsr = regMap.MapReg(RegisterMap::CPSR);
		regMap.Lock(cpsr);

		jit_andi_ui(LOCALREG(cpsr), LOCALREG(cpsr), ~mask);
		if (shift == 0)
			jit_orr_ui(LOCALREG(cpsr), LOCALREG(cpsr), LOCALREG(in));
		else
		{
			if (tmp_in)
			{
				jit_lshi_ui(LOCALREG(in), LOCALREG(in), shift);
				jit_orr_ui(LOCALREG(cpsr), LOCALREG(cpsr), LOCALREG(in));
			}
			else
			{
				u32 tmp = regMap.AllocTempReg();

				jit_lshi_ui(LOCALREG(tmp), LOCALREG(in), shift);
				jit_orr_ui(LOCALREG(cpsr), LOCALREG(cpsr), LOCALREG(tmp));

				regMap.ReleaseTempReg(tmp);
			}
		}

		regMap.Unlock(cpsr);
	}

	void FASTCALL PackCPSRImm(RegisterMap &regMap, ARMCPU_PSR flg, u32 in)
	{
		u32 shift, mask;

		switch (flg)
		{
		case PSR_MODE:
			shift = PSR_MODE_BITSHIFT;
			mask = PSR_MODE_BITMASK;
			break;
		case PSR_T:
			shift = PSR_T_BITSHIFT;
			mask = PSR_T_BITMASK;
			break;
		case PSR_F:
			shift = PSR_F_BITSHIFT;
			mask = PSR_F_BITMASK;
			break;
		case PSR_I:
			shift = PSR_I_BITSHIFT;
			mask = PSR_I_BITMASK;
			break;
		case PSR_Q:
			shift = PSR_Q_BITSHIFT;
			mask = PSR_Q_BITMASK;
			break;
		case PSR_V:
			shift = PSR_V_BITSHIFT;
			mask = PSR_V_BITMASK;
			break;
		case PSR_C:
			shift = PSR_C_BITSHIFT;
			mask = PSR_C_BITMASK;
			break;
		case PSR_Z:
			shift = PSR_Z_BITSHIFT;
			mask = PSR_Z_BITMASK;
			break;
		case PSR_N:
			shift = PSR_N_BITSHIFT;
			mask = PSR_N_BITMASK;
			break;

		default:
			shift = 0;
			mask = 0;
			break;
		}

		if (flg != PSR_MODE)
			in = !!in;

		u32 cpsr = regMap.MapReg(RegisterMap::CPSR);
		regMap.Lock(cpsr);

		if (in)
			jit_ori_ui(LOCALREG(cpsr), LOCALREG(cpsr), in << shift);
		else
			jit_andi_ui(LOCALREG(cpsr), LOCALREG(cpsr), ~mask);

		regMap.Unlock(cpsr);
	}

	struct ShiftOut
	{
		u32 shiftop;
		u32 cflg;
		bool shiftopimm;
		bool cflgimm;

		ShiftOut()
			: shiftop(INVALID_REG_ID)
			, cflg(INVALID_REG_ID)
			, shiftopimm(false)
			, cflgimm(false)
		{
		}

		void CleanShiftOp(RegisterMap &regMap)
		{
			if (!shiftopimm && shiftop != INVALID_REG_ID)
				regMap.ReleaseTempReg(shiftop);
		}

		void CleanCflg(RegisterMap &regMap)
		{
			if (!cflgimm && cflg != INVALID_REG_ID)
				regMap.ReleaseTempReg(cflg);
		}

		void Clean(RegisterMap &regMap)
		{
			CleanShiftOp(regMap);
			CleanCflg(regMap);
		}
	};

	ShiftOut FASTCALL IRShiftOpGenerate(const Decoded &d, RegisterMap &regMap, bool clacCarry)
	{
		u32 PROCNUM = d.ProcessID;

		ShiftOut Out;

		switch (d.Typ)
		{
		case IRSHIFT_LSL:
			if (!d.R)
			{
				if (clacCarry)
				{
					if (d.Immediate == 0)
					{
						Out.cflg = regMap.AllocTempReg();

						UnpackCPSR(regMap, PSR_C, Out.cflg);
					}
					else
					{
						if (regMap.IsImm(REGID(d.Rm)))
						{
							Out.cflg = (regMap.GetImm32(REGID(d.Rm))>>(32-d.Immediate)) != 0;
							Out.cflgimm = true;
						}
						else
						{
							Out.cflg = regMap.AllocTempReg();

							u32 rm = regMap.MapReg(REGID(d.Rm));
							regMap.Lock(rm);

							jit_rshi_ui(LOCALREG(Out.cflg), LOCALREG(rm), 32-d.Immediate);
							jit_andi_ui(LOCALREG(Out.cflg), LOCALREG(Out.cflg), 1);

							regMap.Unlock(rm);
						}
					}
				}

				if (d.Immediate == 0)
				{
					if (regMap.IsImm(REGID(d.Rm)))
					{
						Out.shiftop = regMap.GetImm32(REGID(d.Rm));
						Out.shiftopimm = true;
					}
					else
					{
						Out.shiftop = regMap.AllocTempReg();

						u32 rm = regMap.MapReg(REGID(d.Rm));
						regMap.Lock(rm);

						jit_movr_ui(LOCALREG(Out.shiftop), LOCALREG(rm));

						regMap.Unlock(rm);
					}
				}
				else
				{
					if (regMap.IsImm(REGID(d.Rm)))
					{
						Out.shiftop = regMap.GetImm32(REGID(d.Rm))<<d.Immediate;
						Out.shiftopimm = true;
					}
					else
					{
						Out.shiftop = regMap.AllocTempReg();

						u32 rm = regMap.MapReg(REGID(d.Rm));
						regMap.Lock(rm);

						jit_lshi_ui(LOCALREG(Out.shiftop), LOCALREG(rm), d.Immediate);

						regMap.Unlock(rm);
					}
				}
			}
			else
			{
				if (clacCarry)
				{
					Out.cflg = regMap.AllocTempReg();
					Out.shiftop = regMap.AllocTempReg();

					UnpackCPSR(regMap, PSR_C, Out.cflg);

					u32 rs = regMap.MapReg(REGID(d.Rs));
					regMap.Lock(rs);
					u32 rm = regMap.MapReg(REGID(d.Rm));
					regMap.Lock(rm);
					u32 tmp = regMap.AllocTempReg();

					jit_andi_ui(LOCALREG(Out.shiftop), LOCALREG(rs), 0xFF);
					jit_insn *eq0 = jit_beqi_ui(jit_forward(), LOCALREG(Out.shiftop), 0);
					jit_insn *lt32 = jit_blti_ui(jit_forward(), LOCALREG(Out.shiftop), 32);
					jit_insn *eq32 = jit_beqi_ui(jit_forward(), LOCALREG(Out.shiftop), 32);
					jit_movi_ui(LOCALREG(Out.shiftop), 0);
					jit_movi_ui(LOCALREG(Out.cflg), 0);
					jit_insn *done1 = jit_jmpi(jit_forward());
					jit_patch(eq32);
					jit_movi_ui(LOCALREG(Out.shiftop), 0);
					jit_andi_ui(LOCALREG(Out.cflg), LOCALREG(rm), 1);
					jit_insn *done2 = jit_jmpi(jit_forward());
					jit_patch(eq0);
					jit_movr_ui(LOCALREG(Out.shiftop), LOCALREG(rm));
					jit_insn *done3 = jit_jmpi(jit_forward());
					jit_patch(lt32);
					jit_rsbi_ui(LOCALREG(tmp), LOCALREG(Out.shiftop), 32);
					jit_rshr_ui(LOCALREG(Out.cflg), LOCALREG(rm), LOCALREG(tmp));
					jit_andi_ui(LOCALREG(Out.cflg), LOCALREG(Out.cflg), 1);
					jit_lshr_ui(LOCALREG(Out.shiftop), LOCALREG(rm), LOCALREG(Out.shiftop));
					jit_patch(done1);
					jit_patch(done2);
					jit_patch(done3);

					regMap.ReleaseTempReg(tmp);
					regMap.Unlock(rm);
					regMap.Unlock(rs);
				}
				else
				{
					Out.shiftop = regMap.AllocTempReg();

					u32 rs = regMap.MapReg(REGID(d.Rs));
					regMap.Lock(rs);
					u32 rm = regMap.MapReg(REGID(d.Rm));
					regMap.Lock(rm);

					jit_andi_ui(LOCALREG(Out.shiftop), LOCALREG(rs), 0xFF);
					jit_insn *lt32 = jit_blti_ui(jit_forward(), LOCALREG(Out.shiftop), 32);
					jit_movi_ui(LOCALREG(Out.shiftop), 0);
					jit_insn *done = jit_jmpi(jit_forward());
					jit_patch(lt32);
					jit_lshr_ui(LOCALREG(Out.shiftop), LOCALREG(rm), LOCALREG(Out.shiftop));
					jit_patch(done);

					regMap.Unlock(rm);
					regMap.Unlock(rs);
				}
			}
			break;
		case IRSHIFT_LSR:
			if (!d.R)
			{
				if (clacCarry)
				{
					if (d.Immediate == 0)
					{
						if (regMap.IsImm(REGID(d.Rm)))
						{
							Out.cflg = BIT31(regMap.GetImm32(REGID(d.Rm)));
							Out.cflgimm = true;
						}
						else
						{
							Out.cflg = regMap.AllocTempReg();

							u32 rm = regMap.MapReg(REGID(d.Rm));
							regMap.Lock(rm);

							jit_rshi_ui(LOCALREG(Out.cflg), LOCALREG(rm), 31);
							//jit_andi_ui(LOCALREG(Out.cflg), LOCALREG(Out.cflg), 1);

							regMap.Unlock(rm);
						}
					}
					else
					{
						if (regMap.IsImm(REGID(d.Rm)))
						{
							Out.cflg = BIT_N(regMap.GetImm32(REGID(d.Rm)), d.Immediate-1);
							Out.cflgimm = true;
						}
						else
						{
							Out.cflg = regMap.AllocTempReg();

							u32 rm = regMap.MapReg(REGID(d.Rm));
							regMap.Lock(rm);

							jit_rshi_ui(LOCALREG(Out.cflg), LOCALREG(rm), d.Immediate-1);
							jit_andi_ui(LOCALREG(Out.cflg), LOCALREG(Out.cflg), 1);

							regMap.Unlock(rm);
						}
					}
				}

				if (d.Immediate == 0)
				{
					Out.shiftop = 0;
					Out.shiftopimm = true;
				}
				else
				{
					if (regMap.IsImm(REGID(d.Rm)))
					{
						Out.shiftop = regMap.GetImm32(REGID(d.Rm))>>d.Immediate;
						Out.shiftopimm = true;
					}
					else
					{
						Out.shiftop = regMap.AllocTempReg();

						u32 rm = regMap.MapReg(REGID(d.Rm));
						regMap.Lock(rm);

						jit_rshi_ui(LOCALREG(Out.shiftop), LOCALREG(rm), d.Immediate);

						regMap.Unlock(rm);
					}
				}
			}
			else
			{
				if (clacCarry)
				{
					Out.cflg = regMap.AllocTempReg();
					Out.shiftop = regMap.AllocTempReg();

					UnpackCPSR(regMap, PSR_C, Out.cflg);

					u32 rs = regMap.MapReg(REGID(d.Rs));
					regMap.Lock(rs);
					u32 rm = regMap.MapReg(REGID(d.Rm));
					regMap.Lock(rm);
					u32 tmp = regMap.AllocTempReg();

					jit_andi_ui(LOCALREG(Out.shiftop), LOCALREG(rs), 0xFF);
					jit_insn *eq0 = jit_beqi_ui(jit_forward(), LOCALREG(Out.shiftop), 0);
					jit_insn *lt32 = jit_blti_ui(jit_forward(), LOCALREG(Out.shiftop), 32);
					jit_insn *eq32 = jit_beqi_ui(jit_forward(), LOCALREG(Out.shiftop), 32);
					jit_movi_ui(LOCALREG(Out.shiftop), 0);
					jit_movi_ui(LOCALREG(Out.cflg), 0);
					jit_insn *done1 = jit_jmpi(jit_forward());
					jit_patch(eq32);
					jit_movi_ui(LOCALREG(Out.shiftop), 0);
					jit_rshi_ui(LOCALREG(Out.cflg), LOCALREG(rm), 31);
					//jit_andi_ui(LOCALREG(Out.cflg), LOCALREG(Out.cflg), 1);
					jit_insn *done2 = jit_jmpi(jit_forward());
					jit_patch(eq0);
					jit_movr_ui(LOCALREG(Out.shiftop), LOCALREG(rm));
					jit_insn *done3 = jit_jmpi(jit_forward());
					jit_patch(lt32);
					jit_subi_ui(LOCALREG(tmp), LOCALREG(Out.shiftop), 1);
					jit_rshr_ui(LOCALREG(Out.cflg), LOCALREG(rm), LOCALREG(tmp));
					jit_andi_ui(LOCALREG(Out.cflg), LOCALREG(Out.cflg), 1);
					jit_rshr_ui(LOCALREG(Out.shiftop), LOCALREG(rm), LOCALREG(Out.shiftop));
					jit_patch(done1);
					jit_patch(done2);
					jit_patch(done3);

					regMap.ReleaseTempReg(tmp);
					regMap.Unlock(rm);
					regMap.Unlock(rs);
				}
				else
				{
					Out.shiftop = regMap.AllocTempReg();

					u32 rs = regMap.MapReg(REGID(d.Rs));
					regMap.Lock(rs);
					u32 rm = regMap.MapReg(REGID(d.Rm));
					regMap.Lock(rm);

					jit_andi_ui(LOCALREG(Out.shiftop), LOCALREG(rs), 0xFF);
					jit_insn *lt32 = jit_blti_ui(jit_forward(), LOCALREG(Out.shiftop), 32);
					jit_movi_ui(LOCALREG(Out.shiftop), 0);
					jit_insn *done = jit_jmpi(jit_forward());
					jit_patch(lt32);
					jit_rshr_ui(LOCALREG(Out.shiftop), LOCALREG(rm), LOCALREG(Out.shiftop));
					jit_patch(done);

					regMap.Unlock(rm);
					regMap.Unlock(rs);
				}
			}
			break;
		case IRSHIFT_ASR:
			if (!d.R)
			{
				if (clacCarry)
				{
					if (d.Immediate == 0)
					{
						if (regMap.IsImm(REGID(d.Rm)))
						{
							Out.cflg = BIT31(regMap.GetImm32(REGID(d.Rm)));
							Out.cflgimm = true;
						}
						else
						{
							Out.cflg = regMap.AllocTempReg();

							u32 rm = regMap.MapReg(REGID(d.Rm));
							regMap.Lock(rm);

							jit_rshi_ui(LOCALREG(Out.cflg), LOCALREG(rm), 31);
							//jit_andi_ui(LOCALREG(Out.cflg), LOCALREG(Out.cflg), 1);

							regMap.Unlock(rm);
						}
					}
					else
					{
						if (regMap.IsImm(REGID(d.Rm)))
						{
							Out.cflg = BIT_N(regMap.GetImm32(REGID(d.Rm)), d.Immediate-1);
							Out.cflgimm = true;
						}
						else
						{
							Out.cflg = regMap.AllocTempReg();

							u32 rm = regMap.MapReg(REGID(d.Rm));
							regMap.Lock(rm);

							jit_rshi_ui(LOCALREG(Out.cflg), LOCALREG(rm), d.Immediate-1);
							jit_andi_ui(LOCALREG(Out.cflg), LOCALREG(Out.cflg), 1);

							regMap.Unlock(rm);
						}
					}
				}

				if (d.Immediate == 0)
				{
					if (regMap.IsImm(REGID(d.Rm)))
					{
						Out.shiftop = BIT31(regMap.GetImm32(REGID(d.Rm))) * 0xFFFFFFFF;
						Out.shiftopimm = true;
					}
					else
					{
						Out.shiftop = regMap.AllocTempReg();

						u32 rm = regMap.MapReg(REGID(d.Rm));
						regMap.Lock(rm);

						//jit_rshi_ui(LOCALREG(Out.shiftop), LOCALREG(rm), 31);
						//jit_muli_ui(LOCALREG(Out.shiftop), LOCALREG(Out.shiftop), 0xFFFFFFFF);
						jit_rshi_i(LOCALREG(Out.shiftop), LOCALREG(rm), 31);

						regMap.Unlock(rm);
					}
				}
				else
				{
					if (regMap.IsImm(REGID(d.Rm)))
					{
						Out.shiftop = (u32)((s32)regMap.MapReg(REGID(d.Rm)) >> d.Immediate);
						Out.shiftopimm = true;
					}
					else
					{
						Out.shiftop = regMap.AllocTempReg();

						u32 rm = regMap.MapReg(REGID(d.Rm));
						regMap.Lock(rm);

						jit_rshi_i(LOCALREG(Out.shiftop), LOCALREG(rm), d.Immediate);

						regMap.Unlock(rm);
					}
				}
			}
			else
			{
				if (clacCarry)
				{
					Out.cflg = regMap.AllocTempReg();
					Out.shiftop = regMap.AllocTempReg();

					UnpackCPSR(regMap, PSR_C, Out.cflg);

					u32 rs = regMap.MapReg(REGID(d.Rs));
					regMap.Lock(rs);
					u32 rm = regMap.MapReg(REGID(d.Rm));
					regMap.Lock(rm);
					u32 tmp = regMap.AllocTempReg();

					jit_andi_ui(LOCALREG(Out.shiftop), LOCALREG(rs), 0xFF);
					jit_insn *eq0 = jit_beqi_ui(jit_forward(), LOCALREG(Out.shiftop), 0);
					jit_insn *lt32 = jit_blti_ui(jit_forward(), LOCALREG(Out.shiftop), 32);
					//jit_rshi_ui(LOCALREG(Out.shiftop), LOCALREG(rm), 31);
					//jit_muli_ui(LOCALREG(Out.shiftop), LOCALREG(Out.shiftop), 0xFFFFFFFF);
					jit_rshi_i(LOCALREG(Out.shiftop), LOCALREG(rm), 31);
					jit_rshi_ui(LOCALREG(Out.cflg), LOCALREG(rm), 31);
					//jit_andi_ui(LOCALREG(Out.cflg), LOCALREG(Out.cflg), 1);
					jit_insn *done1 = jit_jmpi(jit_forward());
					jit_patch(eq0);
					jit_movr_ui(LOCALREG(Out.shiftop), LOCALREG(rm));
					jit_insn *done2 = jit_jmpi(jit_forward());
					jit_patch(lt32);
					jit_subi_ui(LOCALREG(tmp), LOCALREG(Out.shiftop), 1);
					jit_rshr_ui(LOCALREG(Out.cflg), LOCALREG(rm), LOCALREG(tmp));
					jit_andi_ui(LOCALREG(Out.cflg), LOCALREG(Out.cflg), 1);
					jit_rshr_i(LOCALREG(Out.shiftop), LOCALREG(rm), LOCALREG(Out.shiftop));
					jit_patch(done1);
					jit_patch(done2);

					regMap.ReleaseTempReg(tmp);
					regMap.Unlock(rm);
					regMap.Unlock(rs);
				}
				else
				{
					Out.shiftop = regMap.AllocTempReg();

					u32 rs = regMap.MapReg(REGID(d.Rs));
					regMap.Lock(rs);
					u32 rm = regMap.MapReg(REGID(d.Rm));
					regMap.Lock(rm);

					jit_andi_ui(LOCALREG(Out.shiftop), LOCALREG(rs), 0xFF);
					jit_insn *eq0 = jit_beqi_ui(jit_forward(), LOCALREG(Out.shiftop), 0);
					jit_insn *lt32 = jit_blti_ui(jit_forward(), LOCALREG(Out.shiftop), 32);
					//jit_rshi_ui(LOCALREG(Out.shiftop), LOCALREG(rm), 31);
					//jit_muli_ui(LOCALREG(Out.shiftop), LOCALREG(Out.shiftop), 0xFFFFFFFF);
					jit_rshi_i(LOCALREG(Out.shiftop), LOCALREG(rm), 31);
					jit_insn *done1 = jit_jmpi(jit_forward());
					jit_patch(eq0);
					jit_movr_ui(LOCALREG(Out.shiftop), LOCALREG(rm));
					jit_insn *done2 = jit_jmpi(jit_forward());
					jit_patch(lt32);
					jit_rshr_i(LOCALREG(Out.shiftop), LOCALREG(rm), LOCALREG(Out.shiftop));
					jit_patch(done1);
					jit_patch(done2);

					regMap.Unlock(rm);
					regMap.Unlock(rs);
				}
			}
			break;
		case IRSHIFT_ROR:
			if (!d.R)
			{
				if (clacCarry)
				{
					if (d.Immediate == 0)
					{
						if (regMap.IsImm(REGID(d.Rm)))
						{
							Out.cflg = BIT0(regMap.GetImm32(REGID(d.Rm)));
							Out.cflgimm = true;
						}
						else
						{
							Out.cflg = regMap.AllocTempReg();

							u32 rm = regMap.MapReg(REGID(d.Rm));
							regMap.Lock(rm);

							jit_andi_ui(LOCALREG(Out.cflg), LOCALREG(rm), 1);

							regMap.Unlock(rm);
						}
					}
					else
					{
						if (regMap.IsImm(REGID(d.Rm)))
						{
							Out.cflg = BIT_N(regMap.GetImm32(REGID(d.Rm)), d.Immediate-1);
							Out.cflgimm = true;
						}
						else
						{
							Out.cflg = regMap.AllocTempReg();

							u32 rm = regMap.MapReg(REGID(d.Rm));
							regMap.Lock(rm);

							jit_rshi_ui(LOCALREG(Out.cflg), LOCALREG(rm), d.Immediate-1);
							jit_andi_ui(LOCALREG(Out.cflg), LOCALREG(Out.cflg), 1);

							regMap.Unlock(rm);
						}
					}
				}

				if (d.Immediate == 0)
				{
					if (regMap.IsImm(REGID(d.Rm)))
					{
						Out.shiftop = regMap.AllocTempReg();

						u32 tmp = regMap.GetImm32(REGID(d.Rm)) >> 1;

						UnpackCPSR(regMap, PSR_C, Out.shiftop);
						jit_lshi_ui(LOCALREG(Out.shiftop), LOCALREG(Out.shiftop), 31);
						jit_ori_ui(LOCALREG(Out.shiftop), LOCALREG(Out.shiftop), tmp);
					}
					else
					{
						Out.shiftop = regMap.AllocTempReg();

						u32 rm = regMap.MapReg(REGID(d.Rm));
						regMap.Lock(rm);
						u32 tmp = regMap.AllocTempReg();

						UnpackCPSR(regMap, PSR_C, tmp);
						jit_lshi_ui(LOCALREG(tmp), LOCALREG(tmp), 31);
						jit_rshi_ui(LOCALREG(Out.shiftop), LOCALREG(rm), 1);
						jit_orr_ui(LOCALREG(Out.shiftop), LOCALREG(Out.shiftop), LOCALREG(tmp));

						regMap.ReleaseTempReg(tmp);
						regMap.Unlock(rm);
					}
				}
				else
				{
					if (regMap.IsImm(REGID(d.Rm)))
					{
						Out.shiftop = ROR(regMap.GetImm32(REGID(d.Rm)), d.Immediate);
						Out.shiftopimm = true;
					}
					else
					{
						Out.shiftop = regMap.AllocTempReg();

						u32 rm = regMap.MapReg(REGID(d.Rm));
						regMap.Lock(rm);
						u32 tmp = regMap.AllocTempReg();

						jit_rshi_ui(LOCALREG(tmp), LOCALREG(rm), d.Immediate);
						jit_lshi_ui(LOCALREG(Out.shiftop), LOCALREG(rm), 32-d.Immediate);
						jit_orr_ui(LOCALREG(Out.shiftop), LOCALREG(Out.shiftop), LOCALREG(tmp));

						regMap.ReleaseTempReg(tmp);
						regMap.Unlock(rm);
					}
				}
			}
			else
			{
				if (clacCarry)
				{
					Out.cflg = regMap.AllocTempReg();
					Out.shiftop = regMap.AllocTempReg();

					UnpackCPSR(regMap, PSR_C, Out.cflg);

					u32 rs = regMap.MapReg(REGID(d.Rs));
					regMap.Lock(rs);
					u32 rm = regMap.MapReg(REGID(d.Rm));
					regMap.Lock(rm);
					u32 tmp = regMap.AllocTempReg();

					jit_andi_ui(LOCALREG(Out.shiftop), LOCALREG(rs), 0xFF);
					jit_insn *eq0 = jit_beqi_ui(jit_forward(), LOCALREG(Out.shiftop), 0);
					jit_andi_ui(LOCALREG(Out.shiftop), LOCALREG(rs), 0x1F);
					jit_insn *eq0_1F = jit_beqi_ui(jit_forward(), LOCALREG(Out.shiftop), 0);
					jit_subi_ui(LOCALREG(tmp), LOCALREG(Out.shiftop), 1);
					jit_rshr_ui(LOCALREG(Out.cflg), LOCALREG(rm), LOCALREG(tmp));
					jit_andi_ui(LOCALREG(Out.cflg), LOCALREG(Out.cflg), 1);
					jit_rsbi_ui(LOCALREG(tmp), LOCALREG(Out.shiftop), 32);
					jit_rshr_ui(LOCALREG(Out.shiftop), LOCALREG(rm), LOCALREG(Out.shiftop));
					jit_lshr_ui(LOCALREG(tmp), LOCALREG(rm), LOCALREG(tmp));
					jit_orr_ui(LOCALREG(Out.shiftop), LOCALREG(Out.shiftop), LOCALREG(tmp));
					jit_insn *done1 = jit_jmpi(jit_forward());
					jit_patch(eq0_1F);
					jit_rshi_ui(LOCALREG(Out.cflg), LOCALREG(rm), 31);
					jit_movr_ui(LOCALREG(Out.shiftop), LOCALREG(rm));
					jit_insn *done2 = jit_jmpi(jit_forward());
					jit_patch(eq0);
					jit_movr_ui(LOCALREG(Out.shiftop), LOCALREG(rm));
					jit_patch(done1);
					jit_patch(done2);

					regMap.ReleaseTempReg(tmp);
					regMap.Unlock(rm);
					regMap.Unlock(rs);
				}
				else
				{
					Out.shiftop = regMap.AllocTempReg();

					u32 rs = regMap.MapReg(REGID(d.Rs));
					regMap.Lock(rs);
					u32 rm = regMap.MapReg(REGID(d.Rm));
					regMap.Lock(rm);
					u32 tmp = regMap.AllocTempReg();

					jit_andi_ui(LOCALREG(Out.shiftop), LOCALREG(rs), 0x1F);
					jit_insn *eq0 = jit_beqi_ui(jit_forward(), LOCALREG(Out.shiftop), 0);
					jit_rsbi_ui(LOCALREG(tmp), LOCALREG(Out.shiftop), 32);
					jit_rshr_ui(LOCALREG(Out.shiftop), LOCALREG(rm), LOCALREG(Out.shiftop));
					jit_lshr_ui(LOCALREG(tmp), LOCALREG(rm), LOCALREG(tmp));
					jit_orr_ui(LOCALREG(Out.shiftop), LOCALREG(Out.shiftop), LOCALREG(tmp));
					jit_insn *done = jit_jmpi(jit_forward());
					jit_patch(eq0);
					jit_movr_ui(LOCALREG(Out.shiftop), LOCALREG(rm));
					jit_patch(done);

					regMap.ReleaseTempReg(tmp);
					regMap.Unlock(rm);
					regMap.Unlock(rs);
				}
			}
			break;
		default:
			INFO("Unknow Shift Op : %u.\n", d.Typ);
			if (clacCarry)
			{
				Out.cflg = 0;
				Out.cflgimm = true;
			}
			Out.shiftop = 0;
			Out.shiftopimm = true;
			break;
		}

		return Out;
	}

	void FASTCALL DataProcessLoadCPSRGenerate(const Decoded &d, RegisterMap &regMap)
	{
		u32 PROCNUM = d.ProcessID;

		std::vector<ABIOp> args;
		std::vector<RegisterMap::GuestRegId> flushs;

		u32 tmp = regMap.AllocTempReg(true);

		{
			args.clear();
			flushs.clear();

			u32 tmp2 = regMap.AllocTempReg();
			u32 spsr = regMap.MapReg(RegisterMap::SPSR);
			regMap.Lock(spsr);
			jit_movr_ui(LOCALREG(tmp), LOCALREG(spsr));
			regMap.Unlock(spsr);
			UnpackPSR(PSR_MODE, tmp, tmp2);

			for (u32 i = RegisterMap::R8; i <= RegisterMap::R14; i++)
				flushs.push_back((RegisterMap::GuestRegId)i);
			flushs.push_back(RegisterMap::CPSR);
			flushs.push_back(RegisterMap::SPSR);

			ABIOp op;

			op.type = ABIOp::GUSETREG;
			op.regdata = RegisterMap::CPUPTR;
			args.push_back(op);

			op.type = ABIOp::TEMPREG;
			op.regdata = tmp2;
			args.push_back(op);

			regMap.CallABI(armcpu_switchMode, args, flushs);
		}

		u32 cpsr = regMap.MapReg(RegisterMap::CPSR);
		regMap.Lock(cpsr);
		jit_movr_ui(LOCALREG(cpsr), LOCALREG(tmp));
		regMap.Unlock(cpsr);

		regMap.ReleaseTempReg(tmp);

		{
			args.clear();
			flushs.clear();

			ABIOp op;

			op.type = ABIOp::GUSETREG;
			op.regdata = RegisterMap::CPUPTR;
			args.push_back(op);

			regMap.CallABI(armcpu_changeCPSR, args, flushs);
		}

		tmp = regMap.AllocTempReg();

		UnpackCPSR(regMap, PSR_T, tmp);
		jit_lshi_ui(LOCALREG(tmp), LOCALREG(tmp), 1);
		jit_ori_ui(LOCALREG(tmp), LOCALREG(tmp), 0xFFFFFFFC);

		u32 r15 = regMap.MapReg(RegisterMap::R15, RegisterMap::MAP_DIRTY);
		regMap.Lock(r15);

		jit_andr_ui(LOCALREG(r15), LOCALREG(r15), LOCALREG(tmp));

		regMap.Unlock(r15);

		regMap.ReleaseTempReg(tmp);
	}

	void FASTCALL LDM_S_LoadCPSRGenerate(const Decoded &d, RegisterMap &regMap)
	{
		u32 PROCNUM = d.ProcessID;

		DataProcessLoadCPSRGenerate(d, regMap);
	}

	void FASTCALL R15ModifiedGenerate(const Decoded &d, RegisterMap &regMap)
	{
		u32 PROCNUM = d.ProcessID;

		u32 cpuptr = regMap.MapReg(RegisterMap::CPUPTR);
		regMap.Lock(cpuptr);
		u32 r15 = regMap.MapReg(RegisterMap::R15);
		regMap.Lock(r15);

		jit_stxi_ui(offsetof(armcpu_t, instruct_adr), LOCALREG(cpuptr), LOCALREG(r15));

		regMap.Unlock(r15);
		regMap.Unlock(cpuptr);
	}

	void FASTCALL MUL_Mxx_END(const Decoded &d, RegisterMap &regMap, u32 base, u32 v)
	{
		u32 tmp = regMap.AllocTempReg();
		u32 execyc = regMap.MapReg(RegisterMap::EXECUTECYCLES);

		jit_addi_ui(LOCALREG(execyc), LOCALREG(execyc), base);
		jit_andi_ui(LOCALREG(tmp), LOCALREG(v), 0xFFFFFF00);
		jit_insn *eq_l1 = jit_beqi_ui(jit_forward(), LOCALREG(tmp), 0);
		jit_andi_ui(LOCALREG(tmp), LOCALREG(v), 0xFFFF0000);
		jit_insn *eq_l2 = jit_beqi_ui(jit_forward(), LOCALREG(tmp), 0);
		jit_andi_ui(LOCALREG(tmp), LOCALREG(v), 0xFF000000);
		jit_insn *eq_l3 = jit_beqi_ui(jit_forward(), LOCALREG(tmp), 0);
		jit_addi_ui(LOCALREG(execyc), LOCALREG(execyc), 4);
		jit_insn *done4 = jit_jmpi(jit_forward());
		jit_patch(eq_l1);
		jit_addi_ui(LOCALREG(execyc), LOCALREG(execyc), 1);
		jit_insn *done1 = jit_jmpi(jit_forward());
		jit_patch(eq_l2);
		jit_addi_ui(LOCALREG(execyc), LOCALREG(execyc), 2);
		jit_insn *done2 = jit_jmpi(jit_forward());
		jit_patch(eq_l3);
		jit_addi_ui(LOCALREG(execyc), LOCALREG(execyc), 3);
		jit_patch(done1);
		jit_patch(done2);
		jit_patch(done4);
	}

	void FASTCALL MUL_Mxx_END_Imm(const Decoded &d, RegisterMap &regMap, u32 base, u32 v)
	{
		if ((v & 0xFFFFFF00) == 0)
			base += 1;
		else if ((v & 0xFFFF0000) == 0)
			base += 2;
		else if ((v & 0xFF000000) == 0)
			base += 3;
		else 
			base += 4;

		u32 execyc = regMap.MapReg(RegisterMap::EXECUTECYCLES);
		
		jit_addi_ui(LOCALREG(execyc), LOCALREG(execyc), base);
	}

	jit_insn* FASTCALL PrepareSLZone()
	{
		static const u32 ALIGN_SIZE = 4 - 1;
		static const u32 SLZONE_SIZE = 32*4;

		jit_insn* bk_ptr = jit_get_label();

		uintptr_t new_ptr = (uintptr_t)jit_get_ip().ptr + (SLZONE_SIZE + ALIGN_SIZE);
		jit_insn* new_ptr_align = (jit_insn*)(new_ptr & ~ALIGN_SIZE);

		jit_set_ip(new_ptr_align);

		memset(bk_ptr, 0xCC, new_ptr_align - bk_ptr);

		PROGINFO("PrepareSLZone(), bk : %#p, new : %#p, size : %u\n", bk_ptr, new_ptr_align, new_ptr_align - bk_ptr);

		return bk_ptr;
	}

	void FASTCALL Fallback2Interpreter(const Decoded &d, RegisterMap &regMap)
	{
	}

	void FASTCALL CheckReschedule(const Decoded &d, RegisterMap &regMap)
	{
	}

//------------------------------------------------------------
//                         IROp decoder
//------------------------------------------------------------
	OPDECODER_DECL(IR_UND)
	{
		u32 PROCNUM = d.ProcessID;

		INFO("IR_UND\n");

		u32 cpuptr = regMap.MapReg(RegisterMap::CPUPTR);
		regMap.Lock(cpuptr);
		u32 tmp = regMap.AllocTempReg();

		if (d.ThumbFlag)
			jit_movi_ui(LOCALREG(tmp), d.Instruction.ThumbOp);
		else
			jit_movi_ui(LOCALREG(tmp), d.Instruction.ArmOp);
		jit_stxi_ui(offsetof(armcpu_t, instruction), LOCALREG(cpuptr), LOCALREG(tmp));

		jit_movi_ui(LOCALREG(tmp), d.Address);
		jit_stxi_ui(offsetof(armcpu_t, instruct_adr), LOCALREG(cpuptr), LOCALREG(tmp));

		regMap.Unlock(cpuptr);
		regMap.ReleaseTempReg(tmp);

		{
			std::vector<ABIOp> args;
			std::vector<RegisterMap::GuestRegId> flushs;

			ABIOp op;
			op.type = ABIOp::GUSETREG;
			op.regdata = RegisterMap::CPUPTR;
			args.push_back(op);

			regMap.CallABI(TRAPUNDEF, args, flushs);
		}
	}

	OPDECODER_DECL(IR_NOP)
	{
	}

	OPDECODER_DECL(IR_DUMMY)
	{
	}

	OPDECODER_DECL(IR_T32P1)
	{
		u32 PROCNUM = d.ProcessID;

		INFO("IR_T32P1\n");

		u32 cpuptr = regMap.MapReg(RegisterMap::CPUPTR);
		regMap.Lock(cpuptr);
		u32 tmp = regMap.AllocTempReg();

		if (d.ThumbFlag)
			jit_movi_ui(LOCALREG(tmp), d.Instruction.ThumbOp);
		else
			jit_movi_ui(LOCALREG(tmp), d.Instruction.ArmOp);
		jit_stxi_ui(offsetof(armcpu_t, instruction), LOCALREG(cpuptr), LOCALREG(tmp));

		jit_movi_ui(LOCALREG(tmp), d.Address);
		jit_stxi_ui(offsetof(armcpu_t, instruct_adr), LOCALREG(cpuptr), LOCALREG(tmp));

		regMap.Unlock(cpuptr);
		regMap.ReleaseTempReg(tmp);

		{
			std::vector<ABIOp> args;
			std::vector<RegisterMap::GuestRegId> flushs;

			ABIOp op;
			op.type = ABIOp::GUSETREG;
			op.regdata = RegisterMap::CPUPTR;
			args.push_back(op);

			regMap.CallABI(TRAPUNDEF, args, flushs);
		}
	}

	OPDECODER_DECL(IR_T32P2)
	{
		u32 PROCNUM = d.ProcessID;

		INFO("IR_T32P2\n");

		u32 cpuptr = regMap.MapReg(RegisterMap::CPUPTR);
		regMap.Lock(cpuptr);
		u32 tmp = regMap.AllocTempReg();

		if (d.ThumbFlag)
			jit_movi_ui(LOCALREG(tmp), d.Instruction.ThumbOp);
		else
			jit_movi_ui(LOCALREG(tmp), d.Instruction.ArmOp);
		jit_stxi_ui(offsetof(armcpu_t, instruction), LOCALREG(cpuptr), LOCALREG(tmp));

		jit_movi_ui(LOCALREG(tmp), d.Address);
		jit_stxi_ui(offsetof(armcpu_t, instruct_adr), LOCALREG(cpuptr), LOCALREG(tmp));

		regMap.Unlock(cpuptr);
		regMap.ReleaseTempReg(tmp);

		{
			std::vector<ABIOp> args;
			std::vector<RegisterMap::GuestRegId> flushs;

			ABIOp op;
			op.type = ABIOp::GUSETREG;
			op.regdata = RegisterMap::CPUPTR;
			args.push_back(op);

			regMap.CallABI(TRAPUNDEF, args, flushs);
		}
	}

	OPDECODER_DECL(IR_MOV)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.I)
		{
			regMap.SetImm32(REGID(d.Rd), d.Immediate);

			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
					PackCPSRImm(regMap, PSR_C, BIT31(d.Immediate));
				if (d.FlagsSet & FLAG_N)
					PackCPSRImm(regMap, PSR_N, BIT31(d.Immediate));
				if (d.FlagsSet & FLAG_Z)
					PackCPSRImm(regMap, PSR_Z, d.Immediate==0 ? 1 : 0);
			}
		}
		else
		{
			const bool clacCarry = d.S && !d.R15Modified && (d.FlagsSet & FLAG_C);
			ShiftOut shift_out = IRShiftOpGenerate(d, regMap, clacCarry);

			u32 rd = INVALID_REG_ID;

			if (shift_out.shiftopimm)
				regMap.SetImm32(REGID(d.Rd), shift_out.shiftop);
			else
			{
				rd = regMap.MapReg(REGID(d.Rd), RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
				regMap.Lock(rd);

				jit_movr_ui(LOCALREG(rd), LOCALREG(shift_out.shiftop));
			}
			
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
				{
					if (shift_out.cflgimm)
						PackCPSRImm(regMap, PSR_C, shift_out.cflg);
					else
					{
						PackCPSR(regMap, PSR_C, shift_out.cflg);
						shift_out.CleanCflg(regMap);
					}
				}
				if (d.FlagsSet & FLAG_N)
				{
					if (regMap.IsImm(REGID(d.Rd)))
						PackCPSRImm(regMap, PSR_N, BIT31(regMap.GetImm32(REGID(d.Rd))));
					else
					{
						jit_rshi_ui(LOCALREG(shift_out.shiftop), LOCALREG(rd), 31);
						PackCPSR(regMap, PSR_N, shift_out.shiftop);
					}
				}
				if (d.FlagsSet & FLAG_Z)
				{
					if (regMap.IsImm(REGID(d.Rd)))
						PackCPSRImm(regMap, PSR_Z, regMap.GetImm32(REGID(d.Rd))==0);
					else
					{
						jit_eqi_ui(LOCALREG(shift_out.shiftop), LOCALREG(rd), 0);
						PackCPSR(regMap, PSR_Z, shift_out.shiftop);
					}
				}
			}

			if (rd != INVALID_REG_ID)
				regMap.Unlock(rd);

			shift_out.Clean(regMap);
		}

		if (d.R15Modified)
		{
			if (d.S)
			{
				DataProcessLoadCPSRGenerate(d, regMap);
			}

			R15ModifiedGenerate(d, regMap);
		}
	}

	OPDECODER_DECL(IR_MVN)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.I)
		{
			regMap.SetImm32(REGID(d.Rd), ~d.Immediate);

			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
					PackCPSRImm(regMap, PSR_C, BIT31(d.Immediate));
				if (d.FlagsSet & FLAG_N)
					PackCPSRImm(regMap, PSR_N, BIT31(~d.Immediate));
				if (d.FlagsSet & FLAG_Z)
					PackCPSRImm(regMap, PSR_Z, ~d.Immediate==0 ? 1 : 0);
			}
		}
		else
		{
			const bool clacCarry = d.S && !d.R15Modified && (d.FlagsSet & FLAG_C);
			ShiftOut shift_out = IRShiftOpGenerate(d, regMap, clacCarry);

			u32 rd = INVALID_REG_ID;

			if (shift_out.shiftopimm)
				regMap.SetImm32(REGID(d.Rd), ~shift_out.shiftop);
			else
			{
				rd = regMap.MapReg(REGID(d.Rd), RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
				regMap.Lock(rd);

				jit_notr_ui(LOCALREG(rd), LOCALREG(shift_out.shiftop));
			}
			
			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
				{
					if (shift_out.cflgimm)
						PackCPSRImm(regMap, PSR_C, shift_out.cflg);
					else
					{
						PackCPSR(regMap, PSR_C, shift_out.cflg);
						shift_out.CleanCflg(regMap);
					}
				}
				if (d.FlagsSet & FLAG_N)
				{
					if (regMap.IsImm(REGID(d.Rd)))
						PackCPSRImm(regMap, PSR_N, BIT31(regMap.GetImm32(REGID(d.Rd))));
					else
					{
						jit_rshi_ui(LOCALREG(shift_out.shiftop), LOCALREG(rd), 31);
						PackCPSR(regMap, PSR_N, shift_out.shiftop);
					}
				}
				if (d.FlagsSet & FLAG_Z)
				{
					if (regMap.IsImm(REGID(d.Rd)))
						PackCPSRImm(regMap, PSR_Z, regMap.GetImm32(REGID(d.Rd))==0);
					else
					{
						jit_eqi_ui(LOCALREG(shift_out.shiftop), LOCALREG(rd), 0);
						PackCPSR(regMap, PSR_Z, shift_out.shiftop);
					}
				}
			}

			if (rd != INVALID_REG_ID)
				regMap.Unlock(rd);

			shift_out.Clean(regMap);
		}

		if (d.R15Modified)
		{
			if (d.S)
			{
				DataProcessLoadCPSRGenerate(d, regMap);
			}

			R15ModifiedGenerate(d, regMap);
		}
	}

	OPDECODER_DECL(IR_AND)
	{
		u32 PROCNUM = d.ProcessID;

		u32 rd = INVALID_REG_ID;

		if (d.I)
		{
			if (regMap.IsImm(REGID(d.Rn)))
				regMap.SetImm32(REGID(d.Rd), regMap.GetImm32(REGID(d.Rn)) & d.Immediate);
			else
			{
				u32 rn = regMap.MapReg(REGID(d.Rn));
				regMap.Lock(rn);

				rd = regMap.MapReg(REGID(d.Rd), RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
				regMap.Lock(rd);

				jit_andi_ui(LOCALREG(rd), LOCALREG(rn), d.Immediate);

				regMap.Unlock(rn);
			}

			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
					PackCPSRImm(regMap, PSR_C, BIT31(d.Immediate));
			}
		}
		else
		{
			const bool clacCarry = d.S && !d.R15Modified && (d.FlagsSet & FLAG_C);
			ShiftOut shift_out = IRShiftOpGenerate(d, regMap, clacCarry);

			if (regMap.IsImm(REGID(d.Rn)) && shift_out.shiftopimm)
				regMap.SetImm32(REGID(d.Rd), regMap.GetImm32(REGID(d.Rn)) & shift_out.shiftop);
			else
			{
				u32 rn = regMap.MapReg(REGID(d.Rn));
				regMap.Lock(rn);

				rd = regMap.MapReg(REGID(d.Rd), RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
				regMap.Lock(rd);

				if (shift_out.shiftopimm)
					jit_andi_ui(LOCALREG(rd), LOCALREG(rn), shift_out.shiftop);
				else
					jit_andr_ui(LOCALREG(rd), LOCALREG(rn), LOCALREG(shift_out.shiftop));

				regMap.Unlock(rn);
			}

			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
				{
					if (shift_out.cflgimm)
						PackCPSRImm(regMap, PSR_C, shift_out.cflg);
					else
						PackCPSR(regMap, PSR_C, shift_out.cflg);
				}
			}

			shift_out.Clean(regMap);
		}

		if (d.S && !d.R15Modified)
		{
			if (d.FlagsSet & FLAG_N)
			{
				if (regMap.IsImm(REGID(d.Rd)))
					PackCPSRImm(regMap, PSR_N, BIT31(regMap.GetImm32(REGID(d.Rd))));
				else
				{
					u32 tmp = regMap.AllocTempReg();

					jit_rshi_ui(LOCALREG(tmp), LOCALREG(rd), 31);
					PackCPSR(regMap, PSR_N, tmp);
						
					regMap.ReleaseTempReg(tmp);
				}
			}
			if (d.FlagsSet & FLAG_Z)
			{
				if (regMap.IsImm(REGID(d.Rd)))
					PackCPSRImm(regMap, PSR_Z, regMap.GetImm32(REGID(d.Rd))==0);
				else
				{
					u32 tmp = regMap.AllocTempReg();

					jit_eqi_ui(LOCALREG(tmp), LOCALREG(rd), 0);
					PackCPSR(regMap, PSR_Z, tmp);
						
					regMap.ReleaseTempReg(tmp);
				}
			}
		}

		if (rd != INVALID_REG_ID)
			regMap.Unlock(rd);

		if (d.R15Modified)
		{
			if (d.S)
			{
				DataProcessLoadCPSRGenerate(d, regMap);
			}

			R15ModifiedGenerate(d, regMap);
		}
	}

	OPDECODER_DECL(IR_TST)
	{
		u32 PROCNUM = d.ProcessID;

		u32 dst = INVALID_REG_ID;
		bool dstimm = false;

		if (d.I)
		{
			if (regMap.IsImm(REGID(d.Rn)))
			{
				dst = regMap.GetImm32(REGID(d.Rn)) & d.Immediate;
				dstimm = true;
			}
			else
			{
				u32 rn = regMap.MapReg(REGID(d.Rn));
				regMap.Lock(rn);

				dst = regMap.AllocTempReg();

				jit_andi_ui(LOCALREG(dst), LOCALREG(rn), d.Immediate);

				regMap.Unlock(rn);
			}

			{
				if (d.FlagsSet & FLAG_C)
					PackCPSRImm(regMap, PSR_C, BIT31(d.Immediate));
			}
		}
		else
		{
			const bool clacCarry = (d.FlagsSet & FLAG_C);
			ShiftOut shift_out = IRShiftOpGenerate(d, regMap, clacCarry);

			if (regMap.IsImm(REGID(d.Rn)) && shift_out.shiftopimm)
			{
				dst = regMap.GetImm32(REGID(d.Rn)) & shift_out.shiftop;
				dstimm = true;
			}
			else
			{
				u32 rn = regMap.MapReg(REGID(d.Rn));
				regMap.Lock(rn);

				dst = regMap.AllocTempReg();

				if (shift_out.shiftopimm)
					jit_andi_ui(LOCALREG(dst), LOCALREG(rn), shift_out.shiftop);
				else
					jit_andr_ui(LOCALREG(dst), LOCALREG(rn), LOCALREG(shift_out.shiftop));

				regMap.Unlock(rn);
			}
			
			{
				if (d.FlagsSet & FLAG_C)
				{
					if (shift_out.cflgimm)
						PackCPSRImm(regMap, PSR_C, shift_out.cflg);
					else
						PackCPSR(regMap, PSR_C, shift_out.cflg);
				}

				shift_out.Clean(regMap);
			}
		}

		{
			if (d.FlagsSet & FLAG_N)
			{
				if (dstimm)
					PackCPSRImm(regMap, PSR_N, BIT31(dst));
				else
				{
					u32 tmp = regMap.AllocTempReg();

					jit_rshi_ui(LOCALREG(tmp), LOCALREG(dst), 31);
					PackCPSR(regMap, PSR_N, tmp);
						
					regMap.ReleaseTempReg(tmp);
				}
			}
			if (d.FlagsSet & FLAG_Z)
			{
				if (dstimm)
					PackCPSRImm(regMap, PSR_Z, dst==0);
				else
				{
					u32 tmp = regMap.AllocTempReg();

					jit_eqi_ui(LOCALREG(tmp), LOCALREG(dst), 0);
					PackCPSR(regMap, PSR_Z, tmp);
						
					regMap.ReleaseTempReg(tmp);
				}
			}
		}

		if (!dstimm)
			regMap.ReleaseTempReg(dst);
	}

	OPDECODER_DECL(IR_EOR)
	{
		u32 PROCNUM = d.ProcessID;

		u32 rd = INVALID_REG_ID;

		if (d.I)
		{
			if (regMap.IsImm(REGID(d.Rn)))
				regMap.SetImm32(REGID(d.Rd), regMap.GetImm32(REGID(d.Rn)) ^ d.Immediate);
			else
			{
				u32 rn = regMap.MapReg(REGID(d.Rn));
				regMap.Lock(rn);

				rd = regMap.MapReg(REGID(d.Rd), RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
				regMap.Lock(rd);

				jit_xori_ui(LOCALREG(rd), LOCALREG(rn), d.Immediate);

				regMap.Unlock(rn);
			}

			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
					PackCPSRImm(regMap, PSR_C, BIT31(d.Immediate));
			}
		}
		else
		{
			const bool clacCarry = d.S && !d.R15Modified && (d.FlagsSet & FLAG_C);
			ShiftOut shift_out = IRShiftOpGenerate(d, regMap, clacCarry);

			if (regMap.IsImm(REGID(d.Rn)) && shift_out.shiftopimm)
				regMap.SetImm32(REGID(d.Rd), regMap.GetImm32(REGID(d.Rn)) ^ shift_out.shiftop);
			else
			{
				u32 rn = regMap.MapReg(REGID(d.Rn));
				regMap.Lock(rn);

				rd = regMap.MapReg(REGID(d.Rd), RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
				regMap.Lock(rd);

				if (shift_out.shiftopimm)
					jit_xori_ui(LOCALREG(rd), LOCALREG(rn), shift_out.shiftop);
				else
					jit_xorr_ui(LOCALREG(rd), LOCALREG(rn), LOCALREG(shift_out.shiftop));

				regMap.Unlock(rn);
			}

			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
				{
					if (shift_out.cflgimm)
						PackCPSRImm(regMap, PSR_C, shift_out.cflg);
					else
						PackCPSR(regMap, PSR_C, shift_out.cflg);
				}
			}

			shift_out.Clean(regMap);
		}

		if (d.S && !d.R15Modified)
		{
			if (d.FlagsSet & FLAG_N)
			{
				if (regMap.IsImm(REGID(d.Rd)))
					PackCPSRImm(regMap, PSR_N, BIT31(regMap.GetImm32(REGID(d.Rd))));
				else
				{
					u32 tmp = regMap.AllocTempReg();

					jit_rshi_ui(LOCALREG(tmp), LOCALREG(rd), 31);
					PackCPSR(regMap, PSR_N, tmp);
						
					regMap.ReleaseTempReg(tmp);
				}
			}
			if (d.FlagsSet & FLAG_Z)
			{
				if (regMap.IsImm(REGID(d.Rd)))
					PackCPSRImm(regMap, PSR_Z, regMap.GetImm32(REGID(d.Rd))==0);
				else
				{
					u32 tmp = regMap.AllocTempReg();

					jit_eqi_ui(LOCALREG(tmp), LOCALREG(rd), 0);
					PackCPSR(regMap, PSR_Z, tmp);
						
					regMap.ReleaseTempReg(tmp);
				}
			}
		}

		if (rd != INVALID_REG_ID)
			regMap.Unlock(rd);

		if (d.R15Modified)
		{
			if (d.S)
			{
				DataProcessLoadCPSRGenerate(d, regMap);
			}

			R15ModifiedGenerate(d, regMap);
		}
	}

	OPDECODER_DECL(IR_TEQ)
	{
		u32 PROCNUM = d.ProcessID;

		u32 dst = INVALID_REG_ID;
		bool dstimm = false;

		if (d.I)
		{
			if (regMap.IsImm(REGID(d.Rn)))
			{
				dst = regMap.GetImm32(REGID(d.Rn)) ^ d.Immediate;
				dstimm = true;
			}
			else
			{
				u32 rn = regMap.MapReg(REGID(d.Rn));
				regMap.Lock(rn);

				dst = regMap.AllocTempReg();

				jit_xori_ui(LOCALREG(dst), LOCALREG(rn), d.Immediate);

				regMap.Unlock(rn);
			}

			{
				if (d.FlagsSet & FLAG_C)
					PackCPSRImm(regMap, PSR_C, BIT31(d.Immediate));
			}
		}
		else
		{
			const bool clacCarry = (d.FlagsSet & FLAG_C);
			ShiftOut shift_out = IRShiftOpGenerate(d, regMap, clacCarry);

			if (regMap.IsImm(REGID(d.Rn)) && shift_out.shiftopimm)
			{
				dst = regMap.GetImm32(REGID(d.Rn)) ^ shift_out.shiftop;
				dstimm = true;
			}
			else
			{
				u32 rn = regMap.MapReg(REGID(d.Rn));
				regMap.Lock(rn);

				dst = regMap.AllocTempReg();

				if (shift_out.shiftopimm)
					jit_xori_ui(LOCALREG(dst), LOCALREG(rn), shift_out.shiftop);
				else
					jit_xorr_ui(LOCALREG(dst), LOCALREG(rn), LOCALREG(shift_out.shiftop));

				regMap.Unlock(rn);
			}
			
			{
				if (d.FlagsSet & FLAG_C)
				{
					if (shift_out.cflgimm)
						PackCPSRImm(regMap, PSR_C, shift_out.cflg);
					else
						PackCPSR(regMap, PSR_C, shift_out.cflg);
				}

				shift_out.Clean(regMap);
			}
		}

		{
			if (d.FlagsSet & FLAG_N)
			{
				if (dstimm)
					PackCPSRImm(regMap, PSR_N, BIT31(dst));
				else
				{
					u32 tmp = regMap.AllocTempReg();

					jit_rshi_ui(LOCALREG(tmp), LOCALREG(dst), 31);
					PackCPSR(regMap, PSR_N, tmp);
						
					regMap.ReleaseTempReg(tmp);
				}
			}
			if (d.FlagsSet & FLAG_Z)
			{
				if (dstimm)
					PackCPSRImm(regMap, PSR_Z, dst==0);
				else
				{
					u32 tmp = regMap.AllocTempReg();

					jit_eqi_ui(LOCALREG(tmp), LOCALREG(dst), 0);
					PackCPSR(regMap, PSR_Z, tmp);
						
					regMap.ReleaseTempReg(tmp);
				}
			}
		}

		if (!dstimm)
			regMap.ReleaseTempReg(dst);
	}

	OPDECODER_DECL(IR_ORR)
	{
		u32 PROCNUM = d.ProcessID;

		u32 rd = INVALID_REG_ID;

		if (d.I)
		{
			if (regMap.IsImm(REGID(d.Rn)))
				regMap.SetImm32(REGID(d.Rd), regMap.GetImm32(REGID(d.Rn)) | d.Immediate);
			else
			{
				u32 rn = regMap.MapReg(REGID(d.Rn));
				regMap.Lock(rn);

				rd = regMap.MapReg(REGID(d.Rd), RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
				regMap.Lock(rd);

				jit_ori_ui(LOCALREG(rd), LOCALREG(rn), d.Immediate);

				regMap.Unlock(rn);
			}

			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
					PackCPSRImm(regMap, PSR_C, BIT31(d.Immediate));
			}
		}
		else
		{
			const bool clacCarry = d.S && !d.R15Modified && (d.FlagsSet & FLAG_C);
			ShiftOut shift_out = IRShiftOpGenerate(d, regMap, clacCarry);

			if (regMap.IsImm(REGID(d.Rn)) && shift_out.shiftopimm)
				regMap.SetImm32(REGID(d.Rd), regMap.GetImm32(REGID(d.Rn)) | shift_out.shiftop);
			else
			{
				u32 rn = regMap.MapReg(REGID(d.Rn));
				regMap.Lock(rn);

				rd = regMap.MapReg(REGID(d.Rd), RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
				regMap.Lock(rd);

				if (shift_out.shiftopimm)
					jit_ori_ui(LOCALREG(rd), LOCALREG(rn), shift_out.shiftop);
				else
					jit_orr_ui(LOCALREG(rd), LOCALREG(rn), LOCALREG(shift_out.shiftop));

				regMap.Unlock(rn);
			}

			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
				{
					if (shift_out.cflgimm)
						PackCPSRImm(regMap, PSR_C, shift_out.cflg);
					else
						PackCPSR(regMap, PSR_C, shift_out.cflg);
				}
			}

			shift_out.Clean(regMap);
		}

		if (d.S && !d.R15Modified)
		{
			if (d.FlagsSet & FLAG_N)
			{
				if (regMap.IsImm(REGID(d.Rd)))
					PackCPSRImm(regMap, PSR_N, BIT31(regMap.GetImm32(REGID(d.Rd))));
				else
				{
					u32 tmp = regMap.AllocTempReg();

					jit_rshi_ui(LOCALREG(tmp), LOCALREG(rd), 31);
					PackCPSR(regMap, PSR_N, tmp);
						
					regMap.ReleaseTempReg(tmp);
				}
			}
			if (d.FlagsSet & FLAG_Z)
			{
				if (regMap.IsImm(REGID(d.Rd)))
					PackCPSRImm(regMap, PSR_Z, regMap.GetImm32(REGID(d.Rd))==0);
				else
				{
					u32 tmp = regMap.AllocTempReg();

					jit_eqi_ui(LOCALREG(tmp), LOCALREG(rd), 0);
					PackCPSR(regMap, PSR_Z, tmp);
						
					regMap.ReleaseTempReg(tmp);
				}
			}
		}

		if (rd != INVALID_REG_ID)
			regMap.Unlock(rd);

		if (d.R15Modified)
		{
			if (d.S)
			{
				DataProcessLoadCPSRGenerate(d, regMap);
			}

			R15ModifiedGenerate(d, regMap);
		}
	}

	OPDECODER_DECL(IR_BIC)
	{
		u32 PROCNUM = d.ProcessID;

		u32 rd = INVALID_REG_ID;

		if (d.I)
		{
			if (regMap.IsImm(REGID(d.Rn)))
				regMap.SetImm32(REGID(d.Rd), regMap.GetImm32(REGID(d.Rn)) & (~d.Immediate));
			else
			{
				u32 rn = regMap.MapReg(REGID(d.Rn));
				regMap.Lock(rn);

				rd = regMap.MapReg(REGID(d.Rd), RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
				regMap.Lock(rd);

				jit_andi_ui(LOCALREG(rd), LOCALREG(rn), ~d.Immediate);

				regMap.Unlock(rn);
			}

			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
					PackCPSRImm(regMap, PSR_C, BIT31(d.Immediate));
			}
		}
		else
		{
			const bool clacCarry = d.S && !d.R15Modified && (d.FlagsSet & FLAG_C);
			ShiftOut shift_out = IRShiftOpGenerate(d, regMap, clacCarry);

			if (regMap.IsImm(REGID(d.Rn)) && shift_out.shiftopimm)
				regMap.SetImm32(REGID(d.Rd), regMap.GetImm32(REGID(d.Rn)) & (~shift_out.shiftop));
			else
			{
				u32 rn = regMap.MapReg(REGID(d.Rn));
				regMap.Lock(rn);

				rd = regMap.MapReg(REGID(d.Rd), RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
				regMap.Lock(rd);

				if (shift_out.shiftopimm)
					jit_andi_ui(LOCALREG(rd), LOCALREG(rn), ~shift_out.shiftop);
				else
				{
					jit_notr_ui(LOCALREG(shift_out.shiftop), LOCALREG(shift_out.shiftop));
					jit_orr_ui(LOCALREG(rd), LOCALREG(rn), LOCALREG(shift_out.shiftop));
				}

				regMap.Unlock(rn);
			}

			if (d.S && !d.R15Modified)
			{
				if (d.FlagsSet & FLAG_C)
				{
					if (shift_out.cflgimm)
						PackCPSRImm(regMap, PSR_C, shift_out.cflg);
					else
						PackCPSR(regMap, PSR_C, shift_out.cflg);
				}
			}

			shift_out.Clean(regMap);
		}

		if (d.S && !d.R15Modified)
		{
			if (d.FlagsSet & FLAG_N)
			{
				if (regMap.IsImm(REGID(d.Rd)))
					PackCPSRImm(regMap, PSR_N, BIT31(regMap.GetImm32(REGID(d.Rd))));
				else
				{
					u32 tmp = regMap.AllocTempReg();

					jit_rshi_ui(LOCALREG(tmp), LOCALREG(rd), 31);
					PackCPSR(regMap, PSR_N, tmp);
						
					regMap.ReleaseTempReg(tmp);
				}
			}
			if (d.FlagsSet & FLAG_Z)
			{
				if (regMap.IsImm(REGID(d.Rd)))
					PackCPSRImm(regMap, PSR_Z, regMap.GetImm32(REGID(d.Rd))==0);
				else
				{
					u32 tmp = regMap.AllocTempReg();

					jit_eqi_ui(LOCALREG(tmp), LOCALREG(rd), 0);
					PackCPSR(regMap, PSR_Z, tmp);
						
					regMap.ReleaseTempReg(tmp);
				}
			}
		}

		if (rd != INVALID_REG_ID)
			regMap.Unlock(rd);

		if (d.R15Modified)
		{
			if (d.S)
			{
				DataProcessLoadCPSRGenerate(d, regMap);
			}

			R15ModifiedGenerate(d, regMap);
		}
	}

	OPDECODER_DECL(IR_ADD)
	{
		u32 PROCNUM = d.ProcessID;

		u32 rd = INVALID_REG_ID;

		if (d.I)
		{
			if (regMap.IsImm(REGID(d.Rn)))
			{
				u32 v = regMap.GetImm32(REGID(d.Rn));

				regMap.SetImm32(REGID(d.Rd), regMap.GetImm32(REGID(d.Rn)) + d.Immediate);

				if (d.S && !d.R15Modified)
				{
					if (d.FlagsSet & FLAG_C)
						PackCPSRImm(regMap, PSR_C, CarryFrom(v, d.Immediate));
					if (d.FlagsSet & FLAG_V)
						PackCPSRImm(regMap, PSR_V, OverflowFromADD(regMap.GetImm32(REGID(d.Rd)), v, d.Immediate));
				}
			}
			else
			{
				u32 rn = regMap.MapReg(REGID(d.Rn));
				regMap.Lock(rn);

				rd = regMap.MapReg(REGID(d.Rd), RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
				regMap.Lock(rd);

				u32 v_tmp = INVALID_REG_ID;
				u32 c_tmp = INVALID_REG_ID;

				if (d.S && !d.R15Modified && ((d.FlagsSet & FLAG_V)))
				{
					v_tmp = regMap.AllocTempReg();

					jit_movr_ui(LOCALREG(v_tmp), LOCALREG(rn));
				}

				if (d.S && !d.R15Modified && ((d.FlagsSet & FLAG_C)))
				{
					c_tmp = regMap.AllocTempReg();

					jit_movi_ui(LOCALREG(c_tmp), 0);
					jit_addci_ui(LOCALREG(rd), LOCALREG(rn), d.Immediate);
					jit_addxi_ui(LOCALREG(c_tmp), LOCALREG(c_tmp), 0);
				}
				else
					jit_addi_ui(LOCALREG(rd), LOCALREG(rn), d.Immediate);

				regMap.Unlock(rn);

				if (d.S && !d.R15Modified)
				{
					if (d.FlagsSet & FLAG_C)
					{
						PackCPSR(regMap, PSR_C, c_tmp);

						regMap.ReleaseTempReg(c_tmp);
					}
					if (d.FlagsSet & FLAG_V)
					{
						u32 tmp = regMap.AllocTempReg();

						jit_xori_ui(LOCALREG(tmp), LOCALREG(v_tmp), d.Immediate);
						jit_notr_ui(LOCALREG(tmp), LOCALREG(tmp));
						jit_xori_ui(LOCALREG(v_tmp), LOCALREG(rd), d.Immediate);
						jit_andr_ui(LOCALREG(v_tmp), LOCALREG(tmp), LOCALREG(v_tmp));
						jit_rshi_ui(LOCALREG(v_tmp), LOCALREG(v_tmp), 31);

						regMap.ReleaseTempReg(tmp);

						PackCPSR(regMap, PSR_V, v_tmp);

						regMap.ReleaseTempReg(v_tmp);
					}
				}
			}
		}
		else
		{
			ShiftOut shift_out = IRShiftOpGenerate(d, regMap, false);

			if (regMap.IsImm(REGID(d.Rn)) && shift_out.shiftopimm)
			{
				u32 v = regMap.GetImm32(REGID(d.Rn));

				regMap.SetImm32(REGID(d.Rd), regMap.GetImm32(REGID(d.Rn)) + shift_out.shiftop);

				if (d.S && !d.R15Modified)
				{
					if (d.FlagsSet & FLAG_C)
						PackCPSRImm(regMap, PSR_C, CarryFrom(v, shift_out.shiftop));
					if (d.FlagsSet & FLAG_V)
						PackCPSRImm(regMap, PSR_V, OverflowFromADD(regMap.GetImm32(REGID(d.Rd)), v, shift_out.shiftop));
				}
			}
			else
			{
				u32 rn = regMap.MapReg(REGID(d.Rn));
				regMap.Lock(rn);

				rd = regMap.MapReg(REGID(d.Rd), RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
				regMap.Lock(rd);

				u32 v_tmp = INVALID_REG_ID;
				u32 c_tmp = INVALID_REG_ID;

				if (d.S && !d.R15Modified && ((d.FlagsSet & FLAG_V)))
				{
					v_tmp = regMap.AllocTempReg();

					jit_movr_ui(LOCALREG(v_tmp), LOCALREG(rn));
				}

				if (d.S && !d.R15Modified && ((d.FlagsSet & FLAG_C)))
				{
					c_tmp = regMap.AllocTempReg();

					jit_movi_ui(LOCALREG(c_tmp), 0);
					if (shift_out.shiftopimm)
						jit_addci_ui(LOCALREG(rd), LOCALREG(rn), shift_out.shiftop);
					else
						jit_addcr_ui(LOCALREG(rd), LOCALREG(rn), LOCALREG(shift_out.shiftop));
					jit_addxi_ui(LOCALREG(c_tmp), LOCALREG(c_tmp), 0);
				}
				else
				{
					if (shift_out.shiftopimm)
						jit_addi_ui(LOCALREG(rd), LOCALREG(rn), shift_out.shiftop);
					else
						jit_addr_ui(LOCALREG(rd), LOCALREG(rn), LOCALREG(shift_out.shiftop));
				}

				regMap.Unlock(rn);

				if (d.S && !d.R15Modified)
				{
					if (d.FlagsSet & FLAG_C)
					{
						PackCPSR(regMap, PSR_C, c_tmp);

						regMap.ReleaseTempReg(c_tmp);
					}
					if (d.FlagsSet & FLAG_V)
					{
						u32 tmp = regMap.AllocTempReg();

						if (shift_out.shiftopimm)
						{
							jit_xori_ui(LOCALREG(tmp), LOCALREG(v_tmp), shift_out.shiftop);
							jit_notr_ui(LOCALREG(tmp), LOCALREG(tmp));
							jit_xori_ui(LOCALREG(v_tmp), LOCALREG(rd), shift_out.shiftop);
							jit_andr_ui(LOCALREG(v_tmp), LOCALREG(tmp), LOCALREG(v_tmp));
							jit_rshi_ui(LOCALREG(v_tmp), LOCALREG(v_tmp), 31);
						}
						else
						{
							jit_xorr_ui(LOCALREG(tmp), LOCALREG(v_tmp), LOCALREG(shift_out.shiftop));
							jit_notr_ui(LOCALREG(tmp), LOCALREG(tmp));
							jit_xorr_ui(LOCALREG(v_tmp), LOCALREG(rd), LOCALREG(shift_out.shiftop));
							jit_andr_ui(LOCALREG(v_tmp), LOCALREG(tmp), LOCALREG(v_tmp));
							jit_rshi_ui(LOCALREG(v_tmp), LOCALREG(v_tmp), 31);
						}

						regMap.ReleaseTempReg(tmp);

						PackCPSR(regMap, PSR_V, v_tmp);

						regMap.ReleaseTempReg(v_tmp);
					}
				}
			}

			shift_out.Clean(regMap);
		}

		if (d.S && !d.R15Modified)
		{
			if (d.FlagsSet & FLAG_N)
			{
				if (regMap.IsImm(REGID(d.Rd)))
					PackCPSRImm(regMap, PSR_N, BIT31(regMap.GetImm32(REGID(d.Rd))));
				else
				{
					u32 tmp = regMap.AllocTempReg();

					jit_rshi_ui(LOCALREG(tmp), LOCALREG(rd), 31);
					PackCPSR(regMap, PSR_N, tmp);
						
					regMap.ReleaseTempReg(tmp);
				}
			}
			if (d.FlagsSet & FLAG_Z)
			{
				if (regMap.IsImm(REGID(d.Rd)))
					PackCPSRImm(regMap, PSR_Z, regMap.GetImm32(REGID(d.Rd))==0);
				else
				{
					u32 tmp = regMap.AllocTempReg();

					jit_eqi_ui(LOCALREG(tmp), LOCALREG(rd), 0);
					PackCPSR(regMap, PSR_Z, tmp);
						
					regMap.ReleaseTempReg(tmp);
				}
			}
		}

		if (rd != INVALID_REG_ID)
			regMap.Unlock(rd);

		if (d.R15Modified)
		{
			if (d.S)
			{
				DataProcessLoadCPSRGenerate(d, regMap);
			}

			R15ModifiedGenerate(d, regMap);
		}
	}

	OPDECODER_DECL(IR_ADC)
	{
		u32 PROCNUM = d.ProcessID;

		u32 rd = INVALID_REG_ID;

		if (d.I)
		{
			{
				u32 cflg = regMap.AllocTempReg();
				UnpackCPSR(regMap, PSR_C, cflg);

				u32 rn = regMap.MapReg(REGID(d.Rn));
				regMap.Lock(rn);

				rd = regMap.MapReg(REGID(d.Rd), RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
				regMap.Lock(rd);

				u32 v_tmp = INVALID_REG_ID;
				u32 c_tmp = INVALID_REG_ID;

				if (d.S && !d.R15Modified && ((d.FlagsSet & FLAG_V)))
				{
					v_tmp = regMap.AllocTempReg();

					jit_movr_ui(LOCALREG(v_tmp), LOCALREG(rn));
				}

				if (d.S && !d.R15Modified && ((d.FlagsSet & FLAG_C)))
				{
					c_tmp = regMap.AllocTempReg();

					jit_movi_ui(LOCALREG(c_tmp), 0);
					jit_addci_ui(LOCALREG(rd), LOCALREG(rn), d.Immediate);
					jit_addxi_ui(LOCALREG(c_tmp), LOCALREG(c_tmp), 0);
					jit_addcr_ui(LOCALREG(rd), LOCALREG(rd), LOCALREG(cflg));
					jit_addxi_ui(LOCALREG(c_tmp), LOCALREG(c_tmp), 0);
					jit_nei_ui(LOCALREG(c_tmp), LOCALREG(c_tmp), 0);
				}
				else
				{
					jit_addi_ui(LOCALREG(rd), LOCALREG(rn), d.Immediate);
					jit_addr_ui(LOCALREG(rd), LOCALREG(rd), LOCALREG(cflg));
				}

				regMap.Unlock(rn);

				regMap.ReleaseTempReg(cflg);

				if (d.S && !d.R15Modified)
				{
					if (d.FlagsSet & FLAG_C)
					{
						PackCPSR(regMap, PSR_C, c_tmp);

						regMap.ReleaseTempReg(c_tmp);
					}
					if (d.FlagsSet & FLAG_V)
					{
						u32 tmp = regMap.AllocTempReg();

						jit_xori_ui(LOCALREG(tmp), LOCALREG(v_tmp), d.Immediate);
						jit_notr_ui(LOCALREG(tmp), LOCALREG(tmp));
						jit_xori_ui(LOCALREG(v_tmp), LOCALREG(rd), d.Immediate);
						jit_andr_ui(LOCALREG(v_tmp), LOCALREG(tmp), LOCALREG(v_tmp));
						jit_rshi_ui(LOCALREG(v_tmp), LOCALREG(v_tmp), 31);

						regMap.ReleaseTempReg(tmp);

						PackCPSR(regMap, PSR_V, v_tmp);

						regMap.ReleaseTempReg(v_tmp);
					}
				}
			}
		}
		else
		{
			ShiftOut shift_out = IRShiftOpGenerate(d, regMap, false);

			{
				u32 cflg = regMap.AllocTempReg();
				UnpackCPSR(regMap, PSR_C, cflg);

				u32 rn = regMap.MapReg(REGID(d.Rn));
				regMap.Lock(rn);

				rd = regMap.MapReg(REGID(d.Rd), RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
				regMap.Lock(rd);

				u32 v_tmp = INVALID_REG_ID;
				u32 c_tmp = INVALID_REG_ID;

				if (d.S && !d.R15Modified && ((d.FlagsSet & FLAG_V)))
				{
					v_tmp = regMap.AllocTempReg();

					jit_movr_ui(LOCALREG(v_tmp), LOCALREG(rn));
				}

				if (d.S && !d.R15Modified && ((d.FlagsSet & FLAG_C)))
				{
					c_tmp = regMap.AllocTempReg();

					jit_movi_ui(LOCALREG(c_tmp), 0);
					if (shift_out.shiftopimm)
						jit_addci_ui(LOCALREG(rd), LOCALREG(rn), shift_out.shiftop);
					else
						jit_addcr_ui(LOCALREG(rd), LOCALREG(rn), LOCALREG(shift_out.shiftop));
					jit_addxi_ui(LOCALREG(c_tmp), LOCALREG(c_tmp), 0);
					jit_addcr_ui(LOCALREG(rd), LOCALREG(rd), LOCALREG(cflg));
					jit_addxi_ui(LOCALREG(c_tmp), LOCALREG(c_tmp), 0);
					jit_nei_ui(LOCALREG(c_tmp), LOCALREG(c_tmp), 0);
				}
				else
				{
					if (shift_out.shiftopimm)
						jit_addi_ui(LOCALREG(rd), LOCALREG(rn), shift_out.shiftop);
					else
						jit_addr_ui(LOCALREG(rd), LOCALREG(rn), LOCALREG(shift_out.shiftop));
					jit_addr_ui(LOCALREG(rd), LOCALREG(rd), LOCALREG(cflg));
				}

				regMap.Unlock(rn);

				regMap.ReleaseTempReg(cflg);

				if (d.S && !d.R15Modified)
				{
					if (d.FlagsSet & FLAG_C)
					{
						PackCPSR(regMap, PSR_C, c_tmp);

						regMap.ReleaseTempReg(c_tmp);
					}
					if (d.FlagsSet & FLAG_V)
					{
						u32 tmp = regMap.AllocTempReg();

						if (shift_out.shiftopimm)
						{
							jit_xori_ui(LOCALREG(tmp), LOCALREG(v_tmp), shift_out.shiftop);
							jit_notr_ui(LOCALREG(tmp), LOCALREG(tmp));
							jit_xori_ui(LOCALREG(v_tmp), LOCALREG(rd), shift_out.shiftop);
							jit_andr_ui(LOCALREG(v_tmp), LOCALREG(tmp), LOCALREG(v_tmp));
							jit_rshi_ui(LOCALREG(v_tmp), LOCALREG(v_tmp), 31);
						}
						else
						{
							jit_xorr_ui(LOCALREG(tmp), LOCALREG(v_tmp), LOCALREG(shift_out.shiftop));
							jit_notr_ui(LOCALREG(tmp), LOCALREG(tmp));
							jit_xorr_ui(LOCALREG(v_tmp), LOCALREG(rd), LOCALREG(shift_out.shiftop));
							jit_andr_ui(LOCALREG(v_tmp), LOCALREG(tmp), LOCALREG(v_tmp));
							jit_rshi_ui(LOCALREG(v_tmp), LOCALREG(v_tmp), 31);
						}

						regMap.ReleaseTempReg(tmp);

						PackCPSR(regMap, PSR_V, v_tmp);

						regMap.ReleaseTempReg(v_tmp);
					}
				}
			}

			shift_out.Clean(regMap);
		}

		if (d.S && !d.R15Modified)
		{
			if (d.FlagsSet & FLAG_N)
			{
				if (regMap.IsImm(REGID(d.Rd)))
					PackCPSRImm(regMap, PSR_N, BIT31(regMap.GetImm32(REGID(d.Rd))));
				else
				{
					u32 tmp = regMap.AllocTempReg();

					jit_rshi_ui(LOCALREG(tmp), LOCALREG(rd), 31);
					PackCPSR(regMap, PSR_N, tmp);
						
					regMap.ReleaseTempReg(tmp);
				}
			}
			if (d.FlagsSet & FLAG_Z)
			{
				if (regMap.IsImm(REGID(d.Rd)))
					PackCPSRImm(regMap, PSR_Z, regMap.GetImm32(REGID(d.Rd))==0);
				else
				{
					u32 tmp = regMap.AllocTempReg();

					jit_eqi_ui(LOCALREG(tmp), LOCALREG(rd), 0);
					PackCPSR(regMap, PSR_Z, tmp);
						
					regMap.ReleaseTempReg(tmp);
				}
			}
		}

		if (rd != INVALID_REG_ID)
			regMap.Unlock(rd);

		if (d.R15Modified)
		{
			if (d.S)
			{
				DataProcessLoadCPSRGenerate(d, regMap);
			}

			R15ModifiedGenerate(d, regMap);
		}
	}

	OPDECODER_DECL(IR_SUB)
	{
		u32 PROCNUM = d.ProcessID;

		u32 rd = INVALID_REG_ID;

		if (d.I)
		{
			if (regMap.IsImm(REGID(d.Rn)))
			{
				u32 v = regMap.GetImm32(REGID(d.Rn));

				regMap.SetImm32(REGID(d.Rd), regMap.GetImm32(REGID(d.Rn)) - d.Immediate);

				if (d.S && !d.R15Modified)
				{
					if (d.FlagsSet & FLAG_C)
						PackCPSRImm(regMap, PSR_C, !BorrowFrom(v, d.Immediate));
					if (d.FlagsSet & FLAG_V)
						PackCPSRImm(regMap, PSR_V, OverflowFromSUB(regMap.GetImm32(REGID(d.Rd)), v, d.Immediate));
				}
			}
			else
			{
				u32 rn = regMap.MapReg(REGID(d.Rn));
				regMap.Lock(rn);

				rd = regMap.MapReg(REGID(d.Rd), RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
				regMap.Lock(rd);

				u32 v_tmp = INVALID_REG_ID;
				u32 c_tmp = INVALID_REG_ID;

				if (d.S && !d.R15Modified && ((d.FlagsSet & FLAG_V)))
				{
					v_tmp = regMap.AllocTempReg();

					jit_movr_ui(LOCALREG(v_tmp), LOCALREG(rn));
				}

				if (d.S && !d.R15Modified && ((d.FlagsSet & FLAG_C)))
				{
					c_tmp = regMap.AllocTempReg();

					jit_movi_ui(LOCALREG(c_tmp), 0);
					jit_subci_ui(LOCALREG(rd), LOCALREG(rn), d.Immediate);
					jit_subxi_ui(LOCALREG(c_tmp), LOCALREG(c_tmp), 0);
					jit_nei_ui(LOCALREG(c_tmp), LOCALREG(c_tmp), 0);
				}
				else
					jit_subi_ui(LOCALREG(rd), LOCALREG(rn), d.Immediate);

				regMap.Unlock(rn);

				if (d.S && !d.R15Modified)
				{
					if (d.FlagsSet & FLAG_C)
					{
						PackCPSR(regMap, PSR_C, c_tmp);

						regMap.ReleaseTempReg(c_tmp);
					}
					if (d.FlagsSet & FLAG_V)
					{
						u32 tmp = regMap.AllocTempReg();

						jit_xori_ui(LOCALREG(tmp), LOCALREG(v_tmp), d.Immediate);
						jit_xorr_ui(LOCALREG(v_tmp), LOCALREG(rd), LOCALREG(v_tmp));
						jit_andr_ui(LOCALREG(v_tmp), LOCALREG(tmp), LOCALREG(v_tmp));
						jit_rshi_ui(LOCALREG(v_tmp), LOCALREG(v_tmp), 31);

						regMap.ReleaseTempReg(tmp);

						PackCPSR(regMap, PSR_V, v_tmp);

						regMap.ReleaseTempReg(v_tmp);
					}
				}
			}
		}
		else
		{
			ShiftOut shift_out = IRShiftOpGenerate(d, regMap, false);

			if (regMap.IsImm(REGID(d.Rn)) && shift_out.shiftopimm)
			{
				u32 v = regMap.GetImm32(REGID(d.Rn));

				regMap.SetImm32(REGID(d.Rd), regMap.GetImm32(REGID(d.Rn)) - shift_out.shiftop);

				if (d.S && !d.R15Modified)
				{
					if (d.FlagsSet & FLAG_C)
						PackCPSRImm(regMap, PSR_C, !BorrowFrom(v, shift_out.shiftop));
					if (d.FlagsSet & FLAG_V)
						PackCPSRImm(regMap, PSR_V, OverflowFromSUB(regMap.GetImm32(REGID(d.Rd)), v, shift_out.shiftop));
				}
			}
			else
			{
				u32 rn = regMap.MapReg(REGID(d.Rn));
				regMap.Lock(rn);

				rd = regMap.MapReg(REGID(d.Rd), RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
				regMap.Lock(rd);

				u32 v_tmp = INVALID_REG_ID;
				u32 c_tmp = INVALID_REG_ID;

				if (d.S && !d.R15Modified && ((d.FlagsSet & FLAG_V)))
				{
					v_tmp = regMap.AllocTempReg();

					jit_movr_ui(LOCALREG(v_tmp), LOCALREG(rn));
				}

				if (d.S && !d.R15Modified && ((d.FlagsSet & FLAG_C)))
				{
					c_tmp = regMap.AllocTempReg();

					jit_movi_ui(LOCALREG(c_tmp), 0);
					if (shift_out.shiftopimm)
						jit_subci_ui(LOCALREG(rd), LOCALREG(rn), shift_out.shiftop);
					else
						jit_subcr_ui(LOCALREG(rd), LOCALREG(rn), LOCALREG(shift_out.shiftop));
					jit_subxi_ui(LOCALREG(c_tmp), LOCALREG(c_tmp), 0);
					jit_nei_ui(LOCALREG(c_tmp), LOCALREG(c_tmp), 0);
				}
				else
				{
					if (shift_out.shiftopimm)
						jit_subi_ui(LOCALREG(rd), LOCALREG(rn), shift_out.shiftop);
					else
						jit_subr_ui(LOCALREG(rd), LOCALREG(rn), LOCALREG(shift_out.shiftop));
				}

				regMap.Unlock(rn);

				if (d.S && !d.R15Modified)
				{
					if (d.FlagsSet & FLAG_C)
					{
						PackCPSR(regMap, PSR_C, c_tmp);

						regMap.ReleaseTempReg(c_tmp);
					}
					if (d.FlagsSet & FLAG_V)
					{
						u32 tmp = regMap.AllocTempReg();

						if (shift_out.shiftopimm)
						{
							jit_xori_ui(LOCALREG(tmp), LOCALREG(v_tmp), shift_out.shiftop);
							jit_xorr_ui(LOCALREG(v_tmp), LOCALREG(rd), LOCALREG(v_tmp));
							jit_andr_ui(LOCALREG(v_tmp), LOCALREG(tmp), LOCALREG(v_tmp));
							jit_rshi_ui(LOCALREG(v_tmp), LOCALREG(v_tmp), 31);
						}
						else
						{
							jit_xorr_ui(LOCALREG(tmp), LOCALREG(v_tmp), LOCALREG(shift_out.shiftop));
							jit_xorr_ui(LOCALREG(v_tmp), LOCALREG(rd), LOCALREG(v_tmp));
							jit_andr_ui(LOCALREG(v_tmp), LOCALREG(tmp), LOCALREG(v_tmp));
							jit_rshi_ui(LOCALREG(v_tmp), LOCALREG(v_tmp), 31);
						}

						regMap.ReleaseTempReg(tmp);

						PackCPSR(regMap, PSR_V, v_tmp);

						regMap.ReleaseTempReg(v_tmp);
					}
				}
			}

			shift_out.Clean(regMap);
		}

		if (d.S && !d.R15Modified)
		{
			if (d.FlagsSet & FLAG_N)
			{
				if (regMap.IsImm(REGID(d.Rd)))
					PackCPSRImm(regMap, PSR_N, BIT31(regMap.GetImm32(REGID(d.Rd))));
				else
				{
					u32 tmp = regMap.AllocTempReg();

					jit_rshi_ui(LOCALREG(tmp), LOCALREG(rd), 31);
					PackCPSR(regMap, PSR_N, tmp);
						
					regMap.ReleaseTempReg(tmp);
				}
			}
			if (d.FlagsSet & FLAG_Z)
			{
				if (regMap.IsImm(REGID(d.Rd)))
					PackCPSRImm(regMap, PSR_Z, regMap.GetImm32(REGID(d.Rd))==0);
				else
				{
					u32 tmp = regMap.AllocTempReg();

					jit_eqi_ui(LOCALREG(tmp), LOCALREG(rd), 0);
					PackCPSR(regMap, PSR_Z, tmp);
						
					regMap.ReleaseTempReg(tmp);
				}
			}
		}

		if (rd != INVALID_REG_ID)
			regMap.Unlock(rd);

		if (d.R15Modified)
		{
			if (d.S)
			{
				DataProcessLoadCPSRGenerate(d, regMap);
			}

			R15ModifiedGenerate(d, regMap);
		}
	}

	OPDECODER_DECL(IR_SBC)
	{
		u32 PROCNUM = d.ProcessID;

		u32 rd = INVALID_REG_ID;

		if (d.I)
		{
			{
				u32 cflg = regMap.AllocTempReg();
				UnpackCPSR(regMap, PSR_C, cflg);
				jit_subi_ui(LOCALREG(cflg), LOCALREG(cflg), 1);

				u32 rn = regMap.MapReg(REGID(d.Rn));
				regMap.Lock(rn);

				rd = regMap.MapReg(REGID(d.Rd), RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
				regMap.Lock(rd);

				u32 v_tmp = INVALID_REG_ID;

				if (d.S && !d.R15Modified && ((d.FlagsSet & FLAG_C) || (d.FlagsSet & FLAG_V)))
				{
					v_tmp = regMap.AllocTempReg();

					jit_movr_ui(LOCALREG(v_tmp), LOCALREG(rn));
				}

				jit_subi_ui(LOCALREG(rd), LOCALREG(rn), d.Immediate);
				jit_subr_ui(LOCALREG(rd), LOCALREG(rd), LOCALREG(cflg));

				regMap.Unlock(rn);

				regMap.ReleaseTempReg(cflg);

				if (d.S && !d.R15Modified)
				{
					if (d.FlagsSet & FLAG_C)
					{
						u32 tmp = regMap.AllocTempReg();

						jit_lei_ui(LOCALREG(tmp), LOCALREG(v_tmp), d.Immediate);
						PackCPSR(regMap, PSR_C, tmp);

						regMap.ReleaseTempReg(tmp);
					}
					if (d.FlagsSet & FLAG_V)
					{
						u32 tmp = regMap.AllocTempReg();

						jit_xori_ui(LOCALREG(tmp), LOCALREG(v_tmp), d.Immediate);
						jit_xorr_ui(LOCALREG(v_tmp), LOCALREG(rd), LOCALREG(v_tmp));
						jit_andr_ui(LOCALREG(v_tmp), LOCALREG(tmp), LOCALREG(v_tmp));
						jit_rshi_ui(LOCALREG(v_tmp), LOCALREG(v_tmp), 31);

						regMap.ReleaseTempReg(tmp);

						PackCPSR(regMap, PSR_V, v_tmp);
					}
				}

				if (v_tmp != INVALID_REG_ID)
					regMap.ReleaseTempReg(v_tmp);
			}
		}
		else
		{
			ShiftOut shift_out = IRShiftOpGenerate(d, regMap, false);

			{
				u32 cflg = regMap.AllocTempReg();
				UnpackCPSR(regMap, PSR_C, cflg);
				jit_subi_ui(LOCALREG(cflg), LOCALREG(cflg), 1);

				u32 rn = regMap.MapReg(REGID(d.Rn));
				regMap.Lock(rn);

				rd = regMap.MapReg(REGID(d.Rd), RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
				regMap.Lock(rd);

				u32 v_tmp = INVALID_REG_ID;

				if (d.S && !d.R15Modified && ((d.FlagsSet & FLAG_C) || (d.FlagsSet & FLAG_V)))
				{
					v_tmp = regMap.AllocTempReg();

					jit_movr_ui(LOCALREG(v_tmp), LOCALREG(rn));
				}

				if (shift_out.shiftopimm)
					jit_subi_ui(LOCALREG(rd), LOCALREG(rn), shift_out.shiftop);
				else
					jit_subr_ui(LOCALREG(rd), LOCALREG(rn), LOCALREG(shift_out.shiftop));
				jit_subr_ui(LOCALREG(rd), LOCALREG(rd), LOCALREG(cflg));

				regMap.Unlock(rn);

				regMap.ReleaseTempReg(cflg);

				if (d.S && !d.R15Modified)
				{
					if (d.FlagsSet & FLAG_C)
					{
						u32 tmp = regMap.AllocTempReg();

						PackCPSR(regMap, PSR_C, tmp);

						regMap.ReleaseTempReg(tmp);
					}
					if (d.FlagsSet & FLAG_V)
					{
						u32 tmp = regMap.AllocTempReg();

						if (shift_out.shiftopimm)
						{
							jit_xori_ui(LOCALREG(tmp), LOCALREG(v_tmp), shift_out.shiftop);
							jit_xorr_ui(LOCALREG(v_tmp), LOCALREG(rd), LOCALREG(v_tmp));
							jit_andr_ui(LOCALREG(v_tmp), LOCALREG(tmp), LOCALREG(v_tmp));
							jit_rshi_ui(LOCALREG(v_tmp), LOCALREG(v_tmp), 31);
						}
						else
						{
							jit_xorr_ui(LOCALREG(tmp), LOCALREG(v_tmp), LOCALREG(shift_out.shiftop));
							jit_xorr_ui(LOCALREG(v_tmp), LOCALREG(rd), LOCALREG(v_tmp));
							jit_andr_ui(LOCALREG(v_tmp), LOCALREG(tmp), LOCALREG(v_tmp));
							jit_rshi_ui(LOCALREG(v_tmp), LOCALREG(v_tmp), 31);
						}

						regMap.ReleaseTempReg(tmp);

						PackCPSR(regMap, PSR_V, v_tmp);
					}
				}

				if (v_tmp != INVALID_REG_ID)
					regMap.ReleaseTempReg(v_tmp);
			}

			shift_out.Clean(regMap);
		}

		if (d.S && !d.R15Modified)
		{
			if (d.FlagsSet & FLAG_N)
			{
				if (regMap.IsImm(REGID(d.Rd)))
					PackCPSRImm(regMap, PSR_N, BIT31(regMap.GetImm32(REGID(d.Rd))));
				else
				{
					u32 tmp = regMap.AllocTempReg();

					jit_rshi_ui(LOCALREG(tmp), LOCALREG(rd), 31);
					PackCPSR(regMap, PSR_N, tmp);
						
					regMap.ReleaseTempReg(tmp);
				}
			}
			if (d.FlagsSet & FLAG_Z)
			{
				if (regMap.IsImm(REGID(d.Rd)))
					PackCPSRImm(regMap, PSR_Z, regMap.GetImm32(REGID(d.Rd))==0);
				else
				{
					u32 tmp = regMap.AllocTempReg();

					jit_eqi_ui(LOCALREG(tmp), LOCALREG(rd), 0);
					PackCPSR(regMap, PSR_Z, tmp);
						
					regMap.ReleaseTempReg(tmp);
				}
			}
		}

		if (rd != INVALID_REG_ID)
			regMap.Unlock(rd);

		if (d.R15Modified)
		{
			if (d.S)
			{
				DataProcessLoadCPSRGenerate(d, regMap);
			}

			R15ModifiedGenerate(d, regMap);
		}
	}

	OPDECODER_DECL(IR_RSB)
	{
		u32 PROCNUM = d.ProcessID;

		u32 rd = INVALID_REG_ID;

		if (d.I)
		{
			if (regMap.IsImm(REGID(d.Rn)))
			{
				u32 v = regMap.GetImm32(REGID(d.Rn));

				regMap.SetImm32(REGID(d.Rd), d.Immediate - regMap.GetImm32(REGID(d.Rn)));

				if (d.S && !d.R15Modified)
				{
					if (d.FlagsSet & FLAG_C)
						PackCPSRImm(regMap, PSR_C, !BorrowFrom(d.Immediate, v));
					if (d.FlagsSet & FLAG_V)
						PackCPSRImm(regMap, PSR_V, OverflowFromSUB(regMap.GetImm32(REGID(d.Rd)), d.Immediate, v));
				}
			}
			else
			{
				u32 rn = regMap.MapReg(REGID(d.Rn));
				regMap.Lock(rn);

				rd = regMap.MapReg(REGID(d.Rd), RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
				regMap.Lock(rd);

				u32 v_tmp = INVALID_REG_ID;

				if (d.S && !d.R15Modified && ((d.FlagsSet & FLAG_C) || (d.FlagsSet & FLAG_V)))
				{
					v_tmp = regMap.AllocTempReg();

					jit_movr_ui(LOCALREG(v_tmp), LOCALREG(rn));
				}

				jit_rsbi_ui(LOCALREG(rd), LOCALREG(rn), d.Immediate);

				regMap.Unlock(rn);

				if (d.S && !d.R15Modified)
				{
					if (d.FlagsSet & FLAG_C)
					{
						u32 tmp = regMap.AllocTempReg();

						jit_lei_ui(LOCALREG(tmp), LOCALREG(v_tmp), d.Immediate);
						PackCPSR(regMap, PSR_C, tmp);

						regMap.ReleaseTempReg(tmp);
					}
					if (d.FlagsSet & FLAG_V)
					{
						u32 tmp = regMap.AllocTempReg();

						jit_xori_ui(LOCALREG(tmp), LOCALREG(v_tmp), d.Immediate);
						jit_xori_ui(LOCALREG(v_tmp), LOCALREG(rd), d.Immediate);
						jit_andr_ui(LOCALREG(v_tmp), LOCALREG(tmp), LOCALREG(v_tmp));
						jit_rshi_ui(LOCALREG(v_tmp), LOCALREG(v_tmp), 31);

						regMap.ReleaseTempReg(tmp);

						PackCPSR(regMap, PSR_V, v_tmp);
					}
				}

				if (v_tmp != INVALID_REG_ID)
					regMap.ReleaseTempReg(v_tmp);
			}
		}
		else
		{
			ShiftOut shift_out = IRShiftOpGenerate(d, regMap, false);

			if (regMap.IsImm(REGID(d.Rn)) && shift_out.shiftopimm)
			{
				u32 v = regMap.GetImm32(REGID(d.Rn));

				regMap.SetImm32(REGID(d.Rd), shift_out.shiftop - regMap.GetImm32(REGID(d.Rn)));

				if (d.S && !d.R15Modified)
				{
					if (d.FlagsSet & FLAG_C)
						PackCPSRImm(regMap, PSR_C, !BorrowFrom(v, shift_out.shiftop));
					if (d.FlagsSet & FLAG_V)
						PackCPSRImm(regMap, PSR_V, OverflowFromSUB(regMap.GetImm32(REGID(d.Rd)), shift_out.shiftop, v));
				}
			}
			else
			{
				u32 rn = regMap.MapReg(REGID(d.Rn));
				regMap.Lock(rn);

				rd = regMap.MapReg(REGID(d.Rd), RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
				regMap.Lock(rd);

				u32 v_tmp = INVALID_REG_ID;

				if (d.S && !d.R15Modified && ((d.FlagsSet & FLAG_C) || (d.FlagsSet & FLAG_V)))
				{
					v_tmp = regMap.AllocTempReg();

					jit_movr_ui(LOCALREG(v_tmp), LOCALREG(rn));
				}

				if (shift_out.shiftopimm)
					jit_rsbi_ui(LOCALREG(rd), LOCALREG(rn), shift_out.shiftop);
				else
					jit_rsbr_ui(LOCALREG(rd), LOCALREG(rn), LOCALREG(shift_out.shiftop));

				regMap.Unlock(rn);

				if (d.S && !d.R15Modified)
				{
					if (d.FlagsSet & FLAG_C)
					{
						u32 tmp = regMap.AllocTempReg();

						if (shift_out.shiftopimm)
							jit_lei_ui(LOCALREG(tmp), LOCALREG(v_tmp), shift_out.shiftop);
						else
							jit_ler_ui(LOCALREG(tmp), LOCALREG(v_tmp), LOCALREG(shift_out.shiftop));
						PackCPSR(regMap, PSR_C, tmp);

						regMap.ReleaseTempReg(tmp);
					}
					if (d.FlagsSet & FLAG_V)
					{
						u32 tmp = regMap.AllocTempReg();

						if (shift_out.shiftopimm)
						{
							jit_xori_ui(LOCALREG(tmp), LOCALREG(v_tmp), shift_out.shiftop);
							jit_xori_ui(LOCALREG(v_tmp), LOCALREG(rd), shift_out.shiftop);
							jit_andr_ui(LOCALREG(v_tmp), LOCALREG(tmp), LOCALREG(v_tmp));
							jit_rshi_ui(LOCALREG(v_tmp), LOCALREG(v_tmp), 31);
						}
						else
						{
							jit_xorr_ui(LOCALREG(tmp), LOCALREG(v_tmp), LOCALREG(shift_out.shiftop));
							jit_xorr_ui(LOCALREG(v_tmp), LOCALREG(rd), LOCALREG(shift_out.shiftop));
							jit_andr_ui(LOCALREG(v_tmp), LOCALREG(tmp), LOCALREG(v_tmp));
							jit_rshi_ui(LOCALREG(v_tmp), LOCALREG(v_tmp), 31);
						}

						regMap.ReleaseTempReg(tmp);

						PackCPSR(regMap, PSR_V, v_tmp);
					}
				}

				if (v_tmp != INVALID_REG_ID)
					regMap.ReleaseTempReg(v_tmp);
			}

			shift_out.Clean(regMap);
		}

		if (d.S && !d.R15Modified)
		{
			if (d.FlagsSet & FLAG_N)
			{
				if (regMap.IsImm(REGID(d.Rd)))
					PackCPSRImm(regMap, PSR_N, BIT31(regMap.GetImm32(REGID(d.Rd))));
				else
				{
					u32 tmp = regMap.AllocTempReg();

					jit_rshi_ui(LOCALREG(tmp), LOCALREG(rd), 31);
					PackCPSR(regMap, PSR_N, tmp);
						
					regMap.ReleaseTempReg(tmp);
				}
			}
			if (d.FlagsSet & FLAG_Z)
			{
				if (regMap.IsImm(REGID(d.Rd)))
					PackCPSRImm(regMap, PSR_Z, regMap.GetImm32(REGID(d.Rd))==0);
				else
				{
					u32 tmp = regMap.AllocTempReg();

					jit_eqi_ui(LOCALREG(tmp), LOCALREG(rd), 0);
					PackCPSR(regMap, PSR_Z, tmp);
						
					regMap.ReleaseTempReg(tmp);
				}
			}
		}

		if (rd != INVALID_REG_ID)
			regMap.Unlock(rd);

		if (d.R15Modified)
		{
			if (d.S)
			{
				DataProcessLoadCPSRGenerate(d, regMap);
			}

			R15ModifiedGenerate(d, regMap);
		}
	}

	OPDECODER_DECL(IR_RSC)
	{
		u32 PROCNUM = d.ProcessID;

		u32 rd = INVALID_REG_ID;

		if (d.I)
		{
			{
				u32 cflg = regMap.AllocTempReg();
				UnpackCPSR(regMap, PSR_C, cflg);
				jit_subi_ui(LOCALREG(cflg), LOCALREG(cflg), 1);

				u32 rn = regMap.MapReg(REGID(d.Rn));
				regMap.Lock(rn);

				rd = regMap.MapReg(REGID(d.Rd), RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
				regMap.Lock(rd);

				u32 v_tmp = INVALID_REG_ID;

				if (d.S && !d.R15Modified && ((d.FlagsSet & FLAG_C) || (d.FlagsSet & FLAG_V)))
				{
					v_tmp = regMap.AllocTempReg();

					jit_movr_ui(LOCALREG(v_tmp), LOCALREG(rn));
				}

				jit_rsbi_ui(LOCALREG(rd), LOCALREG(rn), d.Immediate);
				jit_subr_ui(LOCALREG(rd), LOCALREG(rd), LOCALREG(cflg));

				regMap.Unlock(rn);

				regMap.ReleaseTempReg(cflg);

				if (d.S && !d.R15Modified)
				{
					if (d.FlagsSet & FLAG_C)
					{
						u32 tmp = regMap.AllocTempReg();

						jit_gti_ui(LOCALREG(tmp), LOCALREG(v_tmp), d.Immediate);
						PackCPSR(regMap, PSR_C, tmp);

						regMap.ReleaseTempReg(tmp);
					}
					if (d.FlagsSet & FLAG_V)
					{
						u32 tmp = regMap.AllocTempReg();

						jit_xori_ui(LOCALREG(tmp), LOCALREG(v_tmp), d.Immediate);
						jit_xori_ui(LOCALREG(v_tmp), LOCALREG(rd), d.Immediate);
						jit_andr_ui(LOCALREG(v_tmp), LOCALREG(tmp), LOCALREG(v_tmp));
						jit_rshi_ui(LOCALREG(v_tmp), LOCALREG(v_tmp), 31);

						regMap.ReleaseTempReg(tmp);

						PackCPSR(regMap, PSR_V, v_tmp);
					}
				}

				if (v_tmp != INVALID_REG_ID)
					regMap.ReleaseTempReg(v_tmp);
			}
		}
		else
		{
			ShiftOut shift_out = IRShiftOpGenerate(d, regMap, false);

			{
				u32 cflg = regMap.AllocTempReg();
				UnpackCPSR(regMap, PSR_C, cflg);
				jit_subi_ui(LOCALREG(cflg), LOCALREG(cflg), 1);

				u32 rn = regMap.MapReg(REGID(d.Rn));
				regMap.Lock(rn);

				rd = regMap.MapReg(REGID(d.Rd), RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
				regMap.Lock(rd);

				u32 v_tmp = INVALID_REG_ID;

				if (d.S && !d.R15Modified && ((d.FlagsSet & FLAG_C) || (d.FlagsSet & FLAG_V)))
				{
					v_tmp = regMap.AllocTempReg();

					jit_movr_ui(LOCALREG(v_tmp), LOCALREG(rn));
				}

				if (shift_out.shiftopimm)
					jit_rsbi_ui(LOCALREG(rd), LOCALREG(rn), shift_out.shiftop);
				else
					jit_rsbr_ui(LOCALREG(rd), LOCALREG(rn), LOCALREG(shift_out.shiftop));
				jit_subr_ui(LOCALREG(rd), LOCALREG(rd), LOCALREG(cflg));

				regMap.Unlock(rn);

				regMap.ReleaseTempReg(cflg);

				if (d.S && !d.R15Modified)
				{
					if (d.FlagsSet & FLAG_C)
					{
						u32 tmp = regMap.AllocTempReg();

						if (shift_out.shiftopimm)
							jit_gti_ui(LOCALREG(tmp), LOCALREG(v_tmp), shift_out.shiftop);
						else
							jit_gtr_ui(LOCALREG(tmp), LOCALREG(v_tmp), LOCALREG(shift_out.shiftop));
						PackCPSR(regMap, PSR_C, tmp);

						regMap.ReleaseTempReg(tmp);
					}
					if (d.FlagsSet & FLAG_V)
					{
						u32 tmp = regMap.AllocTempReg();

						if (shift_out.shiftopimm)
						{
							jit_xori_ui(LOCALREG(tmp), LOCALREG(v_tmp), shift_out.shiftop);
							jit_xori_ui(LOCALREG(v_tmp), LOCALREG(rd), shift_out.shiftop);
							jit_andr_ui(LOCALREG(v_tmp), LOCALREG(tmp), LOCALREG(v_tmp));
							jit_rshi_ui(LOCALREG(v_tmp), LOCALREG(v_tmp), 31);
						}
						else
						{
							jit_xorr_ui(LOCALREG(tmp), LOCALREG(v_tmp), LOCALREG(shift_out.shiftop));
							jit_xorr_ui(LOCALREG(v_tmp), LOCALREG(rd), LOCALREG(shift_out.shiftop));
							jit_andr_ui(LOCALREG(v_tmp), LOCALREG(tmp), LOCALREG(v_tmp));
							jit_rshi_ui(LOCALREG(v_tmp), LOCALREG(v_tmp), 31);
						}

						regMap.ReleaseTempReg(tmp);

						PackCPSR(regMap, PSR_V, v_tmp);
					}
				}

				if (v_tmp != INVALID_REG_ID)
					regMap.ReleaseTempReg(v_tmp);
			}

			shift_out.Clean(regMap);
		}

		if (d.S && !d.R15Modified)
		{
			if (d.FlagsSet & FLAG_N)
			{
				if (regMap.IsImm(REGID(d.Rd)))
					PackCPSRImm(regMap, PSR_N, BIT31(regMap.GetImm32(REGID(d.Rd))));
				else
				{
					u32 tmp = regMap.AllocTempReg();

					jit_rshi_ui(LOCALREG(tmp), LOCALREG(rd), 31);
					PackCPSR(regMap, PSR_N, tmp);
						
					regMap.ReleaseTempReg(tmp);
				}
			}
			if (d.FlagsSet & FLAG_Z)
			{
				if (regMap.IsImm(REGID(d.Rd)))
					PackCPSRImm(regMap, PSR_Z, regMap.GetImm32(REGID(d.Rd))==0);
				else
				{
					u32 tmp = regMap.AllocTempReg();

					jit_eqi_ui(LOCALREG(tmp), LOCALREG(rd), 0);
					PackCPSR(regMap, PSR_Z, tmp);
						
					regMap.ReleaseTempReg(tmp);
				}
			}
		}

		if (rd != INVALID_REG_ID)
			regMap.Unlock(rd);

		if (d.R15Modified)
		{
			if (d.S)
			{
				DataProcessLoadCPSRGenerate(d, regMap);
			}

			R15ModifiedGenerate(d, regMap);
		}
	}

	OPDECODER_DECL(IR_CMP)
	{
		u32 PROCNUM = d.ProcessID;

		u32 dst = INVALID_REG_ID;
		bool dstimm = false;

		if (d.I)
		{
			if (regMap.IsImm(REGID(d.Rn)))
			{
				u32 v = regMap.GetImm32(REGID(d.Rn));

				dst = regMap.GetImm32(REGID(d.Rn)) - d.Immediate;
				dstimm = true;

				{
					if (d.FlagsSet & FLAG_C)
						PackCPSRImm(regMap, PSR_C, !BorrowFrom(v, d.Immediate));
					if (d.FlagsSet & FLAG_V)
						PackCPSRImm(regMap, PSR_V, OverflowFromSUB(dst, v, d.Immediate));
				}
			}
			else
			{
				u32 rn = regMap.MapReg(REGID(d.Rn));
				regMap.Lock(rn);

				dst = regMap.AllocTempReg();

				u32 c_tmp = INVALID_REG_ID;

				if (d.FlagsSet & FLAG_C)
				{
					c_tmp = regMap.AllocTempReg();

					jit_movi_ui(LOCALREG(c_tmp), 0);
					jit_subci_ui(LOCALREG(dst), LOCALREG(rn), d.Immediate);
					jit_subxi_ui(LOCALREG(c_tmp), LOCALREG(c_tmp), 0);
					jit_nei_ui(LOCALREG(c_tmp), LOCALREG(c_tmp), 0);
				}
				else
					jit_subi_ui(LOCALREG(dst), LOCALREG(rn), d.Immediate);

				{
					if (d.FlagsSet & FLAG_C)
					{
						PackCPSR(regMap, PSR_C, c_tmp);
						
						regMap.ReleaseTempReg(c_tmp);
					}
					if (d.FlagsSet & FLAG_V)
					{
						u32 flg = regMap.AllocTempReg();
						u32 flg_tmp = regMap.AllocTempReg();

						jit_xori_ui(LOCALREG(flg), LOCALREG(rn), d.Immediate);
						jit_xorr_ui(LOCALREG(flg_tmp), LOCALREG(dst), LOCALREG(rn));
						jit_andr_ui(LOCALREG(flg), LOCALREG(flg), LOCALREG(flg_tmp));
						jit_rshi_ui(LOCALREG(flg), LOCALREG(flg), 31);

						regMap.ReleaseTempReg(flg_tmp);

						PackCPSR(regMap, PSR_V, flg);

						regMap.ReleaseTempReg(flg);
					}
				}

				regMap.Unlock(rn);
			}
		}
		else
		{
			ShiftOut shift_out = IRShiftOpGenerate(d, regMap, false);

			if (regMap.IsImm(REGID(d.Rn)) && shift_out.shiftopimm)
			{
				u32 v = regMap.GetImm32(REGID(d.Rn));

				dst = regMap.GetImm32(REGID(d.Rn)) - shift_out.shiftop;
				dstimm = true;

				if (d.S && !d.R15Modified)
				{
					if (d.FlagsSet & FLAG_C)
						PackCPSRImm(regMap, PSR_C, !BorrowFrom(v, shift_out.shiftop));
					if (d.FlagsSet & FLAG_V)
						PackCPSRImm(regMap, PSR_V, OverflowFromSUB(dst, v, shift_out.shiftop));
				}
			}
			else
			{
				u32 rn = regMap.MapReg(REGID(d.Rn));
				regMap.Lock(rn);

				dst = regMap.AllocTempReg();

				u32 c_tmp = INVALID_REG_ID;

				if (d.FlagsSet & FLAG_C)
				{
					c_tmp = regMap.AllocTempReg();

					jit_movi_ui(LOCALREG(c_tmp), 0);
					if (shift_out.shiftopimm)
						jit_subci_ui(LOCALREG(dst), LOCALREG(rn), shift_out.shiftop);
					else
						jit_subcr_ui(LOCALREG(dst), LOCALREG(rn), LOCALREG(shift_out.shiftop));
					jit_subxi_ui(LOCALREG(c_tmp), LOCALREG(c_tmp), 0);
					jit_nei_ui(LOCALREG(c_tmp), LOCALREG(c_tmp), 0);
				}
				else
				{
					if (shift_out.shiftopimm)
						jit_subi_ui(LOCALREG(dst), LOCALREG(rn), shift_out.shiftop);
					else
						jit_subr_ui(LOCALREG(dst), LOCALREG(rn), LOCALREG(shift_out.shiftop));
				}

				{
					if (d.FlagsSet & FLAG_C)
					{
						PackCPSR(regMap, PSR_C, c_tmp);

						regMap.ReleaseTempReg(c_tmp);
					}
					if (d.FlagsSet & FLAG_V)
					{
						u32 flg = regMap.AllocTempReg();
						u32 flg_tmp = regMap.AllocTempReg();

						if (shift_out.shiftopimm)
						{
							jit_xori_ui(LOCALREG(flg), LOCALREG(rn), d.Immediate);
							jit_xorr_ui(LOCALREG(flg_tmp), LOCALREG(dst), LOCALREG(rn));
							jit_andr_ui(LOCALREG(flg), LOCALREG(flg), LOCALREG(flg_tmp));
							jit_rshi_ui(LOCALREG(flg), LOCALREG(flg), 31);
						}
						else
						{
							jit_xorr_ui(LOCALREG(flg), LOCALREG(rn), LOCALREG(shift_out.shiftop));
							jit_xorr_ui(LOCALREG(flg_tmp), LOCALREG(dst), LOCALREG(rn));
							jit_andr_ui(LOCALREG(flg), LOCALREG(flg), LOCALREG(flg_tmp));
							jit_rshi_ui(LOCALREG(flg), LOCALREG(flg), 31);
						}

						regMap.ReleaseTempReg(flg_tmp);

						PackCPSR(regMap, PSR_V, flg);

						regMap.ReleaseTempReg(flg);
					}
				}

				regMap.Unlock(rn);
			}

			shift_out.Clean(regMap);
		}

		{
			if (d.FlagsSet & FLAG_N)
			{
				if (dstimm)
					PackCPSRImm(regMap, PSR_N, BIT31(dst));
				else
				{
					u32 tmp = regMap.AllocTempReg();

					jit_rshi_ui(LOCALREG(tmp), LOCALREG(dst), 31);
					PackCPSR(regMap, PSR_N, tmp);
						
					regMap.ReleaseTempReg(tmp);
				}
			}
			if (d.FlagsSet & FLAG_Z)
			{
				if (dstimm)
					PackCPSRImm(regMap, PSR_Z, dst==0);
				else
				{
					u32 tmp = regMap.AllocTempReg();

					jit_eqi_ui(LOCALREG(tmp), LOCALREG(dst), 0);
					PackCPSR(regMap, PSR_Z, tmp);
						
					regMap.ReleaseTempReg(tmp);
				}
			}
		}

		if (!dstimm)
			regMap.ReleaseTempReg(dst);
	}

	OPDECODER_DECL(IR_CMN)
	{
		u32 PROCNUM = d.ProcessID;

		u32 dst = INVALID_REG_ID;
		bool dstimm = false;

		if (d.I)
		{
			if (regMap.IsImm(REGID(d.Rn)))
			{
				u32 v = regMap.GetImm32(REGID(d.Rn));

				dst = regMap.GetImm32(REGID(d.Rn)) + d.Immediate;
				dstimm = true;

				{
					if (d.FlagsSet & FLAG_C)
						PackCPSRImm(regMap, PSR_C, CarryFrom(v, d.Immediate));
					if (d.FlagsSet & FLAG_V)
						PackCPSRImm(regMap, PSR_V, OverflowFromADD(dst, v, d.Immediate));
				}
			}
			else
			{
				u32 rn = regMap.MapReg(REGID(d.Rn));
				regMap.Lock(rn);

				dst = regMap.AllocTempReg();

				u32 c_tmp = INVALID_REG_ID;

				if (d.FlagsSet & FLAG_C)
				{
					c_tmp = regMap.AllocTempReg();

					jit_movi_ui(LOCALREG(c_tmp), 0);
					jit_addci_ui(LOCALREG(dst), LOCALREG(rn), d.Immediate);
					jit_addxi_ui(LOCALREG(c_tmp), LOCALREG(c_tmp), 0);
				}
				else
					jit_addi_ui(LOCALREG(dst), LOCALREG(rn), d.Immediate);

				{
					if (d.FlagsSet & FLAG_C)
					{
						PackCPSR(regMap, PSR_C, c_tmp);

						regMap.ReleaseTempReg(c_tmp);
					}
					if (d.FlagsSet & FLAG_V)
					{
						u32 flg = regMap.AllocTempReg();
						u32 flg_tmp = regMap.AllocTempReg();

						jit_xori_ui(LOCALREG(flg), LOCALREG(rn), d.Immediate);
						jit_notr_ui(LOCALREG(flg), LOCALREG(flg));
						jit_xori_ui(LOCALREG(flg_tmp), LOCALREG(dst), d.Immediate);
						jit_andr_ui(LOCALREG(flg), LOCALREG(flg), LOCALREG(flg_tmp));
						jit_rshi_ui(LOCALREG(flg), LOCALREG(flg), 31);

						regMap.ReleaseTempReg(flg_tmp);

						PackCPSR(regMap, PSR_V, flg);

						regMap.ReleaseTempReg(flg);
					}
				}

				regMap.Unlock(rn);
			}
		}
		else
		{
			ShiftOut shift_out = IRShiftOpGenerate(d, regMap, false);

			if (regMap.IsImm(REGID(d.Rn)) && shift_out.shiftopimm)
			{
				u32 v = regMap.GetImm32(REGID(d.Rn));

				dst = regMap.GetImm32(REGID(d.Rn)) + shift_out.shiftop;
				dstimm = true;

				{
					if (d.FlagsSet & FLAG_C)
						PackCPSRImm(regMap, PSR_C, CarryFrom(v, shift_out.shiftop));
					if (d.FlagsSet & FLAG_V)
						PackCPSRImm(regMap, PSR_V, OverflowFromADD(dst, v, shift_out.shiftop));
				}
			}
			else
			{
				u32 rn = regMap.MapReg(REGID(d.Rn));
				regMap.Lock(rn);

				dst = regMap.AllocTempReg();

				u32 c_tmp = INVALID_REG_ID;

				if (d.FlagsSet & FLAG_C)
				{
					c_tmp = regMap.AllocTempReg();

					jit_movi_ui(LOCALREG(c_tmp), 0);
					if (shift_out.shiftopimm)
						jit_addci_ui(LOCALREG(dst), LOCALREG(rn), shift_out.shiftop);
					else
						jit_addcr_ui(LOCALREG(dst), LOCALREG(rn), LOCALREG(shift_out.shiftop));
					jit_addxi_ui(LOCALREG(c_tmp), LOCALREG(c_tmp), 0);
				}
				else
				{
					if (shift_out.shiftopimm)
						jit_addi_ui(LOCALREG(dst), LOCALREG(rn), shift_out.shiftop);
					else
						jit_addr_ui(LOCALREG(dst), LOCALREG(rn), LOCALREG(shift_out.shiftop));
				}

				{
					if (d.FlagsSet & FLAG_C)
					{
						PackCPSR(regMap, PSR_C, c_tmp);

						regMap.ReleaseTempReg(c_tmp);
					}
					if (d.FlagsSet & FLAG_V)
					{
						u32 flg = regMap.AllocTempReg();
						u32 flg_tmp = regMap.AllocTempReg();

						if (shift_out.shiftopimm)
						{
							jit_xori_ui(LOCALREG(flg), LOCALREG(rn), shift_out.shiftop);
							jit_notr_ui(LOCALREG(flg), LOCALREG(flg));
							jit_xori_ui(LOCALREG(flg_tmp), LOCALREG(dst), shift_out.shiftop);
							jit_andr_ui(LOCALREG(flg), LOCALREG(flg), LOCALREG(flg_tmp));
							jit_rshi_ui(LOCALREG(flg), LOCALREG(flg), 31);
						}
						else
						{
							jit_xorr_ui(LOCALREG(flg), LOCALREG(rn), LOCALREG(shift_out.shiftop));
							jit_notr_ui(LOCALREG(flg), LOCALREG(flg));
							jit_xorr_ui(LOCALREG(flg_tmp), LOCALREG(dst), LOCALREG(shift_out.shiftop));
							jit_andr_ui(LOCALREG(flg), LOCALREG(flg), LOCALREG(flg_tmp));
							jit_rshi_ui(LOCALREG(flg), LOCALREG(flg), 31);
						}

						regMap.ReleaseTempReg(flg_tmp);

						PackCPSR(regMap, PSR_V, flg);

						regMap.ReleaseTempReg(flg);
					}
				}

				regMap.Unlock(rn);
			}

			shift_out.Clean(regMap);
		}

		{
			if (d.FlagsSet & FLAG_N)
			{
				if (dstimm)
					PackCPSRImm(regMap, PSR_N, BIT31(dst));
				else
				{
					u32 tmp = regMap.AllocTempReg();

					jit_rshi_ui(LOCALREG(tmp), LOCALREG(dst), 31);
					PackCPSR(regMap, PSR_N, tmp);
						
					regMap.ReleaseTempReg(tmp);
				}
			}
			if (d.FlagsSet & FLAG_Z)
			{
				if (dstimm)
					PackCPSRImm(regMap, PSR_Z, dst==0);
				else
				{
					u32 tmp = regMap.AllocTempReg();

					jit_eqi_ui(LOCALREG(tmp), LOCALREG(dst), 0);
					PackCPSR(regMap, PSR_Z, tmp);
						
					regMap.ReleaseTempReg(tmp);
				}
			}
		}

		if (!dstimm)
			regMap.ReleaseTempReg(dst);
	}

	OPDECODER_DECL(IR_MUL)
	{
		u32 PROCNUM = d.ProcessID;

		if (regMap.IsImm(REGID(d.Rs)) && regMap.IsImm(REGID(d.Rm)))
		{
			u32 v = regMap.GetImm32(REGID(d.Rs));
			if ((s32)v < 0)
				v = ~v;

			MUL_Mxx_END_Imm(d, regMap, 1, v);

			regMap.SetImm32(REGID(d.Rd), regMap.GetImm32(REGID(d.Rs)) * regMap.GetImm32(REGID(d.Rm)));

			if (d.S)
			{
				if (d.FlagsSet & FLAG_N)
					PackCPSRImm(regMap, PSR_N, BIT31(regMap.GetImm32(REGID(d.Rd))));
				if (d.FlagsSet & FLAG_Z)
					PackCPSRImm(regMap, PSR_Z, regMap.GetImm32(REGID(d.Rd))==0);
			}
		}
		else
		{
			u32 v = INVALID_REG_ID;
			bool vimm = false;

			if (regMap.IsImm(REGID(d.Rs)))
			{
				v = regMap.GetImm32(REGID(d.Rs));
				vimm = true;

				if ((s32)v < 0)
					v = ~v;
			}
			else
				v = regMap.AllocTempReg();

			u32 rs = regMap.MapReg(REGID(d.Rs));
			regMap.Lock(rs);

			if (!vimm)
			{
				jit_movr_ui(LOCALREG(v), LOCALREG(rs));
				jit_rshi_i(LOCALREG(v), LOCALREG(v), 31);
				jit_xorr_ui(LOCALREG(v), LOCALREG(v), LOCALREG(rs));

				MUL_Mxx_END(d, regMap, 1, v);

				regMap.ReleaseTempReg(v);
			}
			else
				MUL_Mxx_END_Imm(d, regMap, 1, v);

			u32 rm = regMap.MapReg(REGID(d.Rm));
			regMap.Lock(rm);

			u32 rd = regMap.MapReg(REGID(d.Rd), RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
			regMap.Lock(rd);

			jit_mulr_ui(LOCALREG(rd), LOCALREG(rs), LOCALREG(rm));

			regMap.Unlock(rm);
			regMap.Unlock(rs);

			if (d.S)
			{
				if (d.FlagsSet & FLAG_N)
				{
					u32 tmp = regMap.AllocTempReg();

					jit_rshi_ui(LOCALREG(tmp), LOCALREG(rd), 31);
					PackCPSR(regMap, PSR_N, tmp);

					regMap.ReleaseTempReg(tmp);
				}
				if (d.FlagsSet & FLAG_Z)
				{
					u32 tmp = regMap.AllocTempReg();

					jit_eqi_ui(LOCALREG(tmp), LOCALREG(rd), 0);
					PackCPSR(regMap, PSR_Z, tmp);

					regMap.ReleaseTempReg(tmp);
				}
			}

			regMap.Unlock(rd);
		}
	}

	OPDECODER_DECL(IR_MLA)
	{
		u32 PROCNUM = d.ProcessID;

		if (regMap.IsImm(REGID(d.Rs)) && regMap.IsImm(REGID(d.Rm)) && regMap.IsImm(REGID(d.Rn)))
		{
			u32 v = regMap.GetImm32(REGID(d.Rs));

			if ((s32)v < 0)
				v = ~v;

			MUL_Mxx_END_Imm(d, regMap, 2, v);

			regMap.SetImm32(REGID(d.Rd), regMap.GetImm32(REGID(d.Rs)) * regMap.GetImm32(REGID(d.Rm)) + regMap.GetImm32(REGID(d.Rn)));

			if (d.S)
			{
				if (d.FlagsSet & FLAG_N)
					PackCPSRImm(regMap, PSR_N, BIT31(regMap.GetImm32(REGID(d.Rd))));
				if (d.FlagsSet & FLAG_Z)
					PackCPSRImm(regMap, PSR_Z, regMap.GetImm32(REGID(d.Rd))==0);
			}
		}
		else
		{
			u32 v = INVALID_REG_ID;
			bool vimm = false;

			if (regMap.IsImm(REGID(d.Rs)))
			{
				v = regMap.GetImm32(REGID(d.Rs));
				vimm = true;

				if ((s32)v < 0)
					v = ~v;
			}
			else
				v = regMap.AllocTempReg();

			u32 rs = regMap.MapReg(REGID(d.Rs));
			regMap.Lock(rs);

			if (!vimm)
			{
				jit_movr_ui(LOCALREG(v), LOCALREG(rs));
				jit_rshi_i(LOCALREG(v), LOCALREG(v), 31);
				jit_xorr_ui(LOCALREG(v), LOCALREG(v), LOCALREG(rs));

				MUL_Mxx_END(d, regMap, 2, v);

				regMap.ReleaseTempReg(v);
			}
			else
				MUL_Mxx_END_Imm(d, regMap, 2, v);

			u32 rm = regMap.MapReg(REGID(d.Rm));
			regMap.Lock(rm);

			u32 rn = regMap.MapReg(REGID(d.Rn));
			regMap.Lock(rn);

			u32 rd = regMap.MapReg(REGID(d.Rd), RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
			regMap.Lock(rd);

			if (rd != rn)
			{
				jit_mulr_ui(LOCALREG(rd), LOCALREG(rs), LOCALREG(rm));
				jit_addr_ui(LOCALREG(rd), LOCALREG(rd), LOCALREG(rn));
			}
			else
			{
				u32 tmp = regMap.AllocTempReg();

				jit_mulr_ui(LOCALREG(tmp), LOCALREG(rs), LOCALREG(rm));
				jit_addr_ui(LOCALREG(rd), LOCALREG(tmp), LOCALREG(rn));

				regMap.ReleaseTempReg(tmp);
			}

			regMap.Unlock(rn);
			regMap.Unlock(rm);
			regMap.Unlock(rs);

			if (d.S)
			{
				if (d.FlagsSet & FLAG_N)
				{
					u32 tmp = regMap.AllocTempReg();

					jit_rshi_ui(LOCALREG(tmp), LOCALREG(rd), 31);
					PackCPSR(regMap, PSR_N, tmp);

					regMap.ReleaseTempReg(tmp);
				}
				if (d.FlagsSet & FLAG_Z)
				{
					u32 tmp = regMap.AllocTempReg();

					jit_eqi_ui(LOCALREG(tmp), LOCALREG(rd), 0);
					PackCPSR(regMap, PSR_Z, tmp);

					regMap.ReleaseTempReg(tmp);
				}
			}

			regMap.Unlock(rd);
		}
	}

	OPDECODER_DECL(IR_UMULL)
	{
		u32 PROCNUM = d.ProcessID;

		if (regMap.IsImm(REGID(d.Rs)) && regMap.IsImm(REGID(d.Rm)))
		{
			u32 v = regMap.GetImm32(REGID(d.Rs));

			MUL_Mxx_END_Imm(d, regMap, 2, v);

			u64 res = (u64)regMap.GetImm32(REGID(d.Rs)) * regMap.GetImm32(REGID(d.Rm));

			regMap.SetImm32(REGID(d.Rn), (u32)res);
			regMap.SetImm32(REGID(d.Rd), (u32)(res>>32));

			if (d.S)
			{
				if (d.FlagsSet & FLAG_N)
					PackCPSRImm(regMap, PSR_N, BIT31(regMap.GetImm32(REGID(d.Rd))));
				if (d.FlagsSet & FLAG_Z)
					PackCPSRImm(regMap, PSR_Z, res==0);
			}
		}
		else
		{
			u32 v = INVALID_REG_ID;
			bool vimm = false;

			if (regMap.IsImm(REGID(d.Rs)))
			{
				v = regMap.GetImm32(REGID(d.Rs));
				vimm = true;
			}

			u32 rs = regMap.MapReg(REGID(d.Rs));
			regMap.Lock(rs);

			if (!vimm)
				MUL_Mxx_END(d, regMap, 2, rs);
			else
				MUL_Mxx_END_Imm(d, regMap, 2, v);

			u32 rm = regMap.MapReg(REGID(d.Rm));
			regMap.Lock(rm);

			u32 rn = regMap.MapReg(REGID(d.Rn), RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
			regMap.Lock(rn);

			u32 rd = regMap.MapReg(REGID(d.Rd), RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
			regMap.Lock(rd);

			if (rn != rs && rn != rm)
			{
				jit_mulr_ui(LOCALREG(rn), LOCALREG(rs), LOCALREG(rm));
				jit_hmulr_ui(LOCALREG(rd), LOCALREG(rs), LOCALREG(rm));
			}
			else
			{
				u32 tmp = regMap.AllocTempReg();

				jit_mulr_ui(LOCALREG(tmp), LOCALREG(rs), LOCALREG(rm));
				jit_hmulr_ui(LOCALREG(rd), LOCALREG(rs), LOCALREG(rm));
				jit_movr_ui(LOCALREG(rn), LOCALREG(tmp));

				regMap.ReleaseTempReg(tmp);
			}

			regMap.Unlock(rm);
			regMap.Unlock(rs);

			if (d.S)
			{
				if (d.FlagsSet & FLAG_N)
				{
					u32 tmp = regMap.AllocTempReg();

					jit_rshi_ui(LOCALREG(tmp), LOCALREG(rd), 31);
					PackCPSR(regMap, PSR_N, tmp);

					regMap.ReleaseTempReg(tmp);
				}
				if (d.FlagsSet & FLAG_Z)
				{
					u32 tmp = regMap.AllocTempReg();

					jit_andr_ui(LOCALREG(tmp), LOCALREG(rn), LOCALREG(rd));
					jit_eqi_ui(LOCALREG(tmp), LOCALREG(tmp), 0);
					PackCPSR(regMap, PSR_Z, tmp);

					regMap.ReleaseTempReg(tmp);
				}
			}

			regMap.Unlock(rn);
			regMap.Unlock(rd);
		}
	}

	OPDECODER_DECL(IR_UMLAL)
	{
		u32 PROCNUM = d.ProcessID;

		u32 v = INVALID_REG_ID;
		bool vimm = false;

		if (regMap.IsImm(REGID(d.Rs)))
		{
			v = regMap.GetImm32(REGID(d.Rs));
			vimm = true;
		}

		u32 rs = regMap.MapReg(REGID(d.Rs));
		regMap.Lock(rs);

		if (!vimm)
			MUL_Mxx_END(d, regMap, 3, rs);
		else
			MUL_Mxx_END_Imm(d, regMap, 3, v);

		u32 rm = regMap.MapReg(REGID(d.Rm));
		regMap.Lock(rm);

		u32 hi = regMap.AllocTempReg();
		u32 lo = regMap.AllocTempReg();

		jit_mulr_ui(LOCALREG(lo), LOCALREG(rs), LOCALREG(rm));
		jit_hmulr_ui(LOCALREG(hi), LOCALREG(rs), LOCALREG(rm));

		regMap.Unlock(rm);
		regMap.Unlock(rs);

		u32 rn = regMap.MapReg(REGID(d.Rn), RegisterMap::MAP_DIRTY);
		regMap.Lock(rn);

		u32 rd = regMap.MapReg(REGID(d.Rd), RegisterMap::MAP_DIRTY);
		regMap.Lock(rd);

		jit_addcr_ui(LOCALREG(rn), LOCALREG(rn), LOCALREG(lo));
		jit_addxr_ui(LOCALREG(rd), LOCALREG(rd), LOCALREG(hi));

		regMap.ReleaseTempReg(lo);
		regMap.ReleaseTempReg(hi);

		if (d.S)
		{
			if (d.FlagsSet & FLAG_N)
			{
				u32 tmp = regMap.AllocTempReg();

				jit_rshi_ui(LOCALREG(tmp), LOCALREG(rd), 31);
				PackCPSR(regMap, PSR_N, tmp);

				regMap.ReleaseTempReg(tmp);
			}
			if (d.FlagsSet & FLAG_Z)
			{
				u32 tmp = regMap.AllocTempReg();

				jit_andr_ui(LOCALREG(tmp), LOCALREG(rn), LOCALREG(rd));
				jit_eqi_ui(LOCALREG(tmp), LOCALREG(tmp), 0);
				PackCPSR(regMap, PSR_Z, tmp);

				regMap.ReleaseTempReg(tmp);
			}
		}

		regMap.Unlock(rn);
		regMap.Unlock(rd);
	}

	OPDECODER_DECL(IR_SMULL)
	{
		u32 PROCNUM = d.ProcessID;

		if (regMap.IsImm(REGID(d.Rs)) && regMap.IsImm(REGID(d.Rm)))
		{
			u32 v = regMap.GetImm32(REGID(d.Rs));
			if ((s32)v < 0)
				v = ~v;

			MUL_Mxx_END_Imm(d, regMap, 2, v);

			u64 res = (s64)regMap.GetImm32(REGID(d.Rs)) * regMap.GetImm32(REGID(d.Rm));

			regMap.SetImm32(REGID(d.Rn), (u32)res);
			regMap.SetImm32(REGID(d.Rd), (u32)(res>>32));

			if (d.S)
			{
				if (d.FlagsSet & FLAG_N)
					PackCPSRImm(regMap, PSR_N, BIT31(regMap.GetImm32(REGID(d.Rd))));
				if (d.FlagsSet & FLAG_Z)
					PackCPSRImm(regMap, PSR_Z, res==0);
			}
		}
		else
		{
			u32 v = INVALID_REG_ID;
			bool vimm = false;

			if (regMap.IsImm(REGID(d.Rs)))
			{
				v = regMap.GetImm32(REGID(d.Rs));
				vimm = true;

				if ((s32)v < 0)
					v = ~v;
			}
			else
				v = regMap.AllocTempReg();

			u32 rs = regMap.MapReg(REGID(d.Rs));
			regMap.Lock(rs);

			if (!vimm)
			{
				jit_movr_ui(LOCALREG(v), LOCALREG(rs));
				jit_rshi_i(LOCALREG(v), LOCALREG(v), 31);
				jit_xorr_ui(LOCALREG(v), LOCALREG(v), LOCALREG(rs));

				MUL_Mxx_END(d, regMap, 2, v);

				regMap.ReleaseTempReg(v);
			}
			else
				MUL_Mxx_END_Imm(d, regMap, 2, v);

			u32 rm = regMap.MapReg(REGID(d.Rm));
			regMap.Lock(rm);

			u32 rn = regMap.MapReg(REGID(d.Rn), RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
			regMap.Lock(rn);

			u32 rd = regMap.MapReg(REGID(d.Rd), RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
			regMap.Lock(rd);

			if (rn != rs && rn != rm)
			{
				jit_mulr_i(LOCALREG(rn), LOCALREG(rs), LOCALREG(rm));
				jit_hmulr_i(LOCALREG(rd), LOCALREG(rs), LOCALREG(rm));
			}
			else
			{
				u32 tmp = regMap.AllocTempReg();

				jit_mulr_i(LOCALREG(tmp), LOCALREG(rs), LOCALREG(rm));
				jit_hmulr_i(LOCALREG(rd), LOCALREG(rs), LOCALREG(rm));
				jit_movr_i(LOCALREG(rn), LOCALREG(tmp));

				regMap.ReleaseTempReg(tmp);
			}

			regMap.Unlock(rm);
			regMap.Unlock(rs);

			if (d.S)
			{
				if (d.FlagsSet & FLAG_N)
				{
					u32 tmp = regMap.AllocTempReg();

					jit_rshi_ui(LOCALREG(tmp), LOCALREG(rd), 31);
					PackCPSR(regMap, PSR_N, tmp);

					regMap.ReleaseTempReg(tmp);
				}
				if (d.FlagsSet & FLAG_Z)
				{
					u32 tmp = regMap.AllocTempReg();

					jit_andr_ui(LOCALREG(tmp), LOCALREG(rn), LOCALREG(rd));
					jit_eqi_ui(LOCALREG(tmp), LOCALREG(tmp), 0);
					PackCPSR(regMap, PSR_Z, tmp);

					regMap.ReleaseTempReg(tmp);
				}
			}

			regMap.Unlock(rn);
			regMap.Unlock(rd);
		}
	}

	OPDECODER_DECL(IR_SMLAL)
	{
		u32 PROCNUM = d.ProcessID;

		u32 v = INVALID_REG_ID;
		bool vimm = false;

		if (regMap.IsImm(REGID(d.Rs)))
		{
			v = regMap.GetImm32(REGID(d.Rs));
			vimm = true;

			if ((s32)v < 0)
				v = ~v;
		}
		else
			v = regMap.AllocTempReg();

		u32 rs = regMap.MapReg(REGID(d.Rs));
		regMap.Lock(rs);

		if (!vimm)
		{
			jit_movr_ui(LOCALREG(v), LOCALREG(rs));
			jit_rshi_i(LOCALREG(v), LOCALREG(v), 31);
			jit_xorr_ui(LOCALREG(v), LOCALREG(v), LOCALREG(rs));

			MUL_Mxx_END(d, regMap, 3, v);

			regMap.ReleaseTempReg(v);
		}
		else
			MUL_Mxx_END_Imm(d, regMap, 3, v);

		u32 rm = regMap.MapReg(REGID(d.Rm));
		regMap.Lock(rm);

		u32 hi = regMap.AllocTempReg();
		u32 lo = regMap.AllocTempReg();

		jit_mulr_i(LOCALREG(lo), LOCALREG(rs), LOCALREG(rm));
		jit_hmulr_i(LOCALREG(hi), LOCALREG(rs), LOCALREG(rm));

		regMap.Unlock(rm);
		regMap.Unlock(rs);

		u32 rn = regMap.MapReg(REGID(d.Rn), RegisterMap::MAP_DIRTY);
		regMap.Lock(rn);

		u32 rd = regMap.MapReg(REGID(d.Rd), RegisterMap::MAP_DIRTY);
		regMap.Lock(rd);

		jit_addcr_ui(LOCALREG(rn), LOCALREG(rn), LOCALREG(lo));
		jit_addxr_ui(LOCALREG(rd), LOCALREG(rd), LOCALREG(hi));

		regMap.ReleaseTempReg(lo);
		regMap.ReleaseTempReg(hi);

		if (d.S)
		{
			if (d.FlagsSet & FLAG_N)
			{
				u32 tmp = regMap.AllocTempReg();

				jit_rshi_ui(LOCALREG(tmp), LOCALREG(rd), 31);
				PackCPSR(regMap, PSR_N, tmp);

				regMap.ReleaseTempReg(tmp);
			}
			if (d.FlagsSet & FLAG_Z)
			{
				u32 tmp = regMap.AllocTempReg();

				jit_andr_ui(LOCALREG(tmp), LOCALREG(rn), LOCALREG(rd));
				jit_eqi_ui(LOCALREG(tmp), LOCALREG(tmp), 0);
				PackCPSR(regMap, PSR_Z, tmp);

				regMap.ReleaseTempReg(tmp);
			}
		}

		regMap.Unlock(rn);
		regMap.Unlock(rd);
	}

	OPDECODER_DECL(IR_SMULxy)
	{
		u32 PROCNUM = d.ProcessID;

		if (regMap.IsImm(REGID(d.Rs)) && regMap.IsImm(REGID(d.Rm)))
		{
			s32 tmp1, tmp2;

			if (d.X)
				tmp1 = HWORD(regMap.GetImm32(REGID(d.Rm)));
			else
				tmp1 = LWORD(regMap.GetImm32(REGID(d.Rm)));

			if (d.Y)
				tmp2 = HWORD(regMap.GetImm32(REGID(d.Rs)));
			else
				tmp2 = LWORD(regMap.GetImm32(REGID(d.Rs)));

			regMap.SetImm32(REGID(d.Rd), (u32)(tmp1 * tmp2));
		}
		else
		{
			u32 tmp1, tmp2;

			{
				u32 rm = regMap.MapReg(REGID(d.Rm));
				regMap.Lock(rm);

				tmp1 = regMap.AllocTempReg();

				if (d.X)
					jit_rshi_i(LOCALREG(tmp1), LOCALREG(rm), 16);
				else
					jit_extr_s_ui(LOCALREG(tmp1), LOCALREG(rm));

				regMap.Unlock(rm);
			}

			{
				u32 rs = regMap.MapReg(REGID(d.Rs));
				regMap.Lock(rs);

				tmp2 = regMap.AllocTempReg();

				if (d.Y)
					jit_rshi_i(LOCALREG(tmp2), LOCALREG(rs), 16);
				else
					jit_extr_s_ui(LOCALREG(tmp2), LOCALREG(rs));

				regMap.Unlock(rs);
			}

			u32 rd = regMap.MapReg(REGID(d.Rd), RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
			regMap.Lock(rd);

			jit_mulr_ui(LOCALREG(rd), LOCALREG(tmp1), LOCALREG(tmp2));

			regMap.Unlock(rd);

			regMap.ReleaseTempReg(tmp1);
			regMap.ReleaseTempReg(tmp2);
		}
	}

	OPDECODER_DECL(IR_SMLAxy)
	{
		u32 PROCNUM = d.ProcessID;

		if (regMap.IsImm(REGID(d.Rs)) && regMap.IsImm(REGID(d.Rm)) && regMap.IsImm(REGID(d.Rn)))
		{
			s32 tmp1, tmp2;

			if (d.X)
				tmp1 = HWORD(regMap.GetImm32(REGID(d.Rm)));
			else
				tmp1 = LWORD(regMap.GetImm32(REGID(d.Rm)));

			if (d.Y)
				tmp2 = HWORD(regMap.GetImm32(REGID(d.Rs)));
			else
				tmp2 = LWORD(regMap.GetImm32(REGID(d.Rs)));

			u32 mul = (u32)(tmp1 * tmp2);
			u32 mla = mul + regMap.GetImm32(REGID(d.Rn));

			if (OverflowFromADD(mla, mul, regMap.GetImm32(REGID(d.Rn))))
				PackCPSRImm(regMap, PSR_Q, 1);

			regMap.SetImm32(REGID(d.Rd), mla);
		}
		else
		{
			u32 tmp1, tmp2;

			{
				u32 rm = regMap.MapReg(REGID(d.Rm));
				regMap.Lock(rm);

				tmp1 = regMap.AllocTempReg();

				if (d.X)
					jit_rshi_i(LOCALREG(tmp1), LOCALREG(rm), 16);
				else
					jit_extr_s_ui(LOCALREG(tmp1), LOCALREG(rm));

				regMap.Unlock(rm);
			}

			{
				u32 rs = regMap.MapReg(REGID(d.Rs));
				regMap.Lock(rs);

				tmp2 = regMap.AllocTempReg();

				if (d.Y)
					jit_rshi_i(LOCALREG(tmp2), LOCALREG(rs), 16);
				else
					jit_extr_s_ui(LOCALREG(tmp2), LOCALREG(rs));

				regMap.Unlock(rs);
			}

			u32 rn = regMap.MapReg(REGID(d.Rn));
			regMap.Lock(rn);
			u32 rd = regMap.MapReg(REGID(d.Rd), RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
			regMap.Lock(rd);

			jit_mulr_ui(LOCALREG(tmp1), LOCALREG(tmp1), LOCALREG(tmp2));
			jit_movr_ui(LOCALREG(tmp2), LOCALREG(rn));
			jit_addr_ui(LOCALREG(rd), LOCALREG(tmp1), LOCALREG(rn));

			jit_xorr_ui(LOCALREG(tmp2), LOCALREG(tmp1), LOCALREG(tmp2));
			jit_notr_ui(LOCALREG(tmp2), LOCALREG(tmp2));
			jit_xorr_ui(LOCALREG(tmp1), LOCALREG(rd), LOCALREG(tmp1));
			jit_andr_ui(LOCALREG(tmp1), LOCALREG(tmp1), LOCALREG(tmp2));
			jit_rshi_ui(LOCALREG(tmp1), LOCALREG(tmp1), 31);

			regMap.Unlock(rd);
			regMap.Unlock(rn);

			regMap.ReleaseTempReg(tmp2);

			u32 cpsr = regMap.MapReg(RegisterMap::CPSR);

			jit_lshi_ui(LOCALREG(tmp1), LOCALREG(tmp1), PSR_Q_BITSHIFT);
			jit_orr_ui(LOCALREG(cpsr), LOCALREG(cpsr), LOCALREG(tmp1));

			regMap.Unlock(cpsr);

			regMap.ReleaseTempReg(tmp1);
		}
	}

	OPDECODER_DECL(IR_SMULWy)
	{
		u32 PROCNUM = d.ProcessID;

		// fallback to interpreter
		regMap.FlushGuestReg(REGID(d.Rd));
		regMap.FlushGuestReg(REGID(d.Rs));
		regMap.FlushGuestReg(REGID(d.Rm));

		Fallback2Interpreter(d, regMap);
	}

	OPDECODER_DECL(IR_SMLAWy)
	{
		u32 PROCNUM = d.ProcessID;

		// fallback to interpreter
		regMap.FlushGuestReg(REGID(d.Rd));
		regMap.FlushGuestReg(REGID(d.Rn));
		regMap.FlushGuestReg(REGID(d.Rs));
		regMap.FlushGuestReg(REGID(d.Rm));
		regMap.FlushGuestReg(RegisterMap::CPSR);

		Fallback2Interpreter(d, regMap);
	}

	OPDECODER_DECL(IR_SMLALxy)
	{
		u32 PROCNUM = d.ProcessID;

		// fallback to interpreter
		regMap.FlushGuestReg(REGID(d.Rd));
		regMap.FlushGuestReg(REGID(d.Rn));
		regMap.FlushGuestReg(REGID(d.Rs));
		regMap.FlushGuestReg(REGID(d.Rm));

		Fallback2Interpreter(d, regMap);
	}

	OPDECODER_DECL(IR_LDR)
	{
		u32 PROCNUM = d.ProcessID;

		// fallback to interpreter
	}

	OPDECODER_DECL(IR_STR)
	{
		u32 PROCNUM = d.ProcessID;

		// fallback to interpreter
	}

	OPDECODER_DECL(IR_LDRx)
	{
		u32 PROCNUM = d.ProcessID;

		// fallback to interpreter
	}

	OPDECODER_DECL(IR_STRx)
	{
		u32 PROCNUM = d.ProcessID;

		// fallback to interpreter
	}

	OPDECODER_DECL(IR_LDRD)
	{
		u32 PROCNUM = d.ProcessID;

		// fallback to interpreter
	}

	OPDECODER_DECL(IR_STRD)
	{
		u32 PROCNUM = d.ProcessID;

		// fallback to interpreter
	}

	OPDECODER_DECL(IR_LDREX)
	{
		u32 PROCNUM = d.ProcessID;

		// fallback to interpreter
	}

	OPDECODER_DECL(IR_STREX)
	{
		u32 PROCNUM = d.ProcessID;

		// fallback to interpreter
	}

	OPDECODER_DECL(IR_LDM)
	{
		u32 PROCNUM = d.ProcessID;

		// fallback to interpreter
	}

	OPDECODER_DECL(IR_STM)
	{
		u32 PROCNUM = d.ProcessID;

		// fallback to interpreter
	}

	OPDECODER_DECL(IR_SWP)
	{
		u32 PROCNUM = d.ProcessID;

		// fallback to interpreter
	}

	OPDECODER_DECL(IR_B)
	{
		u32 PROCNUM = d.ProcessID;

		regMap.SetImm32(RegisterMap::R15, d.Immediate);

		R15ModifiedGenerate(d, regMap);
	}

	OPDECODER_DECL(IR_BL)
	{
		u32 PROCNUM = d.ProcessID;

		regMap.SetImm32(RegisterMap::R14, d.CalcNextInstruction(d) | d.ThumbFlag);
		regMap.SetImm32(RegisterMap::R15, d.Immediate);

		R15ModifiedGenerate(d, regMap);
	}

	OPDECODER_DECL(IR_BX)
	{
		u32 PROCNUM = d.ProcessID;

		u32 tmp = regMap.AllocTempReg();

		u32 rn = regMap.MapReg(REGID(d.Rn));
		regMap.Lock(rn);

		jit_andi_ui(LOCALREG(tmp), LOCALREG(rn), 1);

		PackCPSR(regMap, PSR_T, tmp, false);

		u32 r15 = regMap.MapReg(RegisterMap::R15, RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
		regMap.Lock(r15);

		jit_lshi_ui(LOCALREG(tmp), LOCALREG(tmp), 1);
		jit_ori_ui(LOCALREG(tmp), LOCALREG(tmp), 0xFFFFFFFC);
		jit_andr_ui(LOCALREG(r15), LOCALREG(rn), LOCALREG(tmp));

		regMap.Unlock(r15);

		regMap.Unlock(rn);

		regMap.ReleaseTempReg(tmp);

		R15ModifiedGenerate(d, regMap);
	}

	OPDECODER_DECL(IR_BLX)
	{
		u32 PROCNUM = d.ProcessID;

		u32 tmp = regMap.AllocTempReg();

		u32 rn = regMap.MapReg(REGID(d.Rn));
		regMap.Lock(rn);

		jit_andi_ui(LOCALREG(tmp), LOCALREG(rn), 1);

		PackCPSR(regMap, PSR_T, tmp, false);

		u32 r15 = regMap.MapReg(RegisterMap::R15, RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
		regMap.Lock(r15);

		jit_lshi_ui(LOCALREG(tmp), LOCALREG(tmp), 1);
		jit_ori_ui(LOCALREG(tmp), LOCALREG(tmp), 0xFFFFFFFC);
		jit_andr_ui(LOCALREG(r15), LOCALREG(rn), LOCALREG(tmp));

		regMap.Unlock(r15);

		regMap.Unlock(rn);

		regMap.ReleaseTempReg(tmp);

		regMap.SetImm32(RegisterMap::R14, d.CalcNextInstruction(d) | d.ThumbFlag);

		R15ModifiedGenerate(d, regMap);
	}

	OPDECODER_DECL(IR_SWI)
	{
		u32 PROCNUM = d.ProcessID;

		std::vector<ABIOp> args;
		std::vector<RegisterMap::GuestRegId> flushs;

		bool bypassBuiltinSWI = 
			(GETCPU.intVector == 0x00000000 && PROCNUM==0)
			|| (GETCPU.intVector == 0xFFFF0000 && PROCNUM==1);

		if (GETCPU.swi_tab && !bypassBuiltinSWI)
		{
			u32 cpuptr = regMap.MapReg(RegisterMap::CPUPTR);
			regMap.Lock(cpuptr);

			u32 tmp = regMap.AllocTempReg();

			if (d.MayHalt)
			{
				jit_movi_ui(LOCALREG(tmp), d.Address);
				jit_stxi_ui(offsetof(armcpu_t, instruct_adr), LOCALREG(cpuptr), LOCALREG(tmp));

				regMap.SetImm32(RegisterMap::R15, d.CalcNextInstruction(d));
				regMap.FlushGuestReg(RegisterMap::R15);
			}

			regMap.Unlock(cpuptr);

			{
				args.clear();
				flushs.clear();

				for (u32 i = RegisterMap::R0; i <= RegisterMap::R3; i++)
					flushs.push_back((RegisterMap::GuestRegId)i);

				regMap.CallABI(GETCPU.swi_tab[d.Immediate], args, flushs, tmp);
			}

			u32 execyc = regMap.MapReg(RegisterMap::EXECUTECYCLES);

			jit_addr_ui(LOCALREG(execyc), LOCALREG(execyc), LOCALREG(tmp));
			jit_addi_ui(LOCALREG(execyc), LOCALREG(execyc), 3);

			regMap.Unlock(execyc);

			regMap.ReleaseTempReg(tmp);

			if (d.MayHalt)
				R15ModifiedGenerate(d, regMap);
		}
		else
		{
			u32 tmp = regMap.AllocTempReg(true);

			u32 cpsr = regMap.MapReg(RegisterMap::CPSR);
			regMap.Lock(cpsr);

			jit_movr_ui(LOCALREG(tmp), LOCALREG(cpsr));

			regMap.Unlock(cpsr);

			{
				args.clear();
				flushs.clear();

				for (u32 i = RegisterMap::R8; i <= RegisterMap::R14; i++)
					flushs.push_back((RegisterMap::GuestRegId)i);
				flushs.push_back(RegisterMap::CPSR);
				flushs.push_back(RegisterMap::SPSR);

				ABIOp op;

				op.type = ABIOp::GUSETREG;
				op.regdata = RegisterMap::CPUPTR;
				args.push_back(op);

				op.type = ABIOp::IMM;
				op.immdata.type = ImmData::IMM8;
				op.immdata.imm8 = SVC;
				args.push_back(op);

				regMap.CallABI(armcpu_switchMode, args, flushs);
			}

			regMap.SetImm32(RegisterMap::R14, d.CalcNextInstruction(d));

			u32 spsr = regMap.MapReg(RegisterMap::SPSR);

			jit_movr_ui(LOCALREG(spsr), LOCALREG(tmp));

			regMap.Unlock(spsr);

			regMap.ReleaseTempReg(tmp);

			PackCPSRImm(regMap, PSR_T, 0);
			PackCPSRImm(regMap, PSR_I, 1);

			{
				args.clear();
				flushs.clear();

				ABIOp op;

				op.type = ABIOp::GUSETREG;
				op.regdata = RegisterMap::CPUPTR;
				args.push_back(op);

				regMap.CallABI(armcpu_changeCPSR, args, flushs);
			}

			u32 r15 = regMap.MapReg(RegisterMap::R15, RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
			regMap.Lock(r15);
			u32 cpuptr = regMap.MapReg(RegisterMap::CPUPTR);
			regMap.Lock(cpuptr);

			jit_ldxi_ui(LOCALREG(r15), LOCALREG(cpuptr), offsetof(armcpu_t, intVector));
			jit_addi_ui(LOCALREG(r15), LOCALREG(r15), 0x08);

			regMap.Unlock(cpuptr);
			regMap.Unlock(r15);

			u32 execyc = regMap.MapReg(RegisterMap::EXECUTECYCLES);

			jit_addi_ui(LOCALREG(execyc), LOCALREG(execyc), 3);

			regMap.Unlock(execyc);

			R15ModifiedGenerate(d, regMap);
		}
	}

	OPDECODER_DECL(IR_MSR)
	{
		u32 PROCNUM = d.ProcessID;

		std::vector<ABIOp> args;
		std::vector<RegisterMap::GuestRegId> flushs;
		std::vector<u32> states;

		if (d.P)
		{
			u32 byte_mask = (BIT0(d.OpData)?0x000000FF:0x00000000) |
							(BIT1(d.OpData)?0x0000FF00:0x00000000) |
							(BIT2(d.OpData)?0x00FF0000:0x00000000) |
							(BIT3(d.OpData)?0xFF000000:0x00000000);

			u32 state_start = regMap.StoreState();

			u32 tmp = regMap.AllocTempReg();

			UnpackCPSR(regMap, PSR_MODE, tmp);
			jit_insn *done1 = jit_bgei_ui(jit_forward(), LOCALREG(tmp), USR);
			jit_insn *done2 = jit_bgei_ui(jit_forward(), LOCALREG(tmp), SYS);

			regMap.ReleaseTempReg(tmp);

			u32 spsr = regMap.MapReg(RegisterMap::SPSR, RegisterMap::MAP_DIRTY);
			regMap.Lock(spsr);

			if (d.I)
			{
				jit_andi_ui(LOCALREG(spsr), LOCALREG(spsr), ~byte_mask);
				jit_ori_ui(LOCALREG(spsr), LOCALREG(spsr), byte_mask & d.Immediate);
			}
			else
			{
				u32 rm = regMap.MapReg(REGID(d.Rm));
				regMap.Lock(rm);
				u32 tmp = regMap.AllocTempReg();

				jit_andi_ui(LOCALREG(spsr), LOCALREG(spsr), ~byte_mask);
				jit_andi_ui(LOCALREG(tmp), LOCALREG(rm), byte_mask);
				jit_orr_ui(LOCALREG(spsr), LOCALREG(spsr), LOCALREG(tmp));

				regMap.ReleaseTempReg(tmp);
				regMap.Unlock(rm);
			}

			regMap.Unlock(spsr);

			{
				args.clear();
				flushs.clear();

				ABIOp op;

				op.type = ABIOp::GUSETREG;
				op.regdata = RegisterMap::CPUPTR;
				args.push_back(op);

				regMap.CallABI(armcpu_changeCPSR, args, flushs);
			}

			jit_insn* pt_end1 = PrepareSLZone();

			u32 state_end1 = regMap.StoreState();

			jit_patch(done1);
			jit_patch(done2);

			regMap.RestoreState(state_start);
			u32 state_end2 = regMap.StoreState();

			// merge states
			states.clear();
			states.push_back(state_end1);
			states.push_back(state_end2);

			u32 state_merge = regMap.CalcStates(state_start, states);

			// generate merge code
			regMap.RestoreState(state_end2);
			regMap.MergeToStates(state_merge);

			// backup pc
			jit_insn* lable_end = jit_get_label();

			// generate merge code
			regMap.RestoreState(state_end1);
			jit_set_ip(pt_end1);
			regMap.MergeToStates(state_merge);
			jit_jmpi(lable_end);

			// restore pc
			regMap.RestoreState(state_merge);
			jit_set_ip(lable_end);

			regMap.CleanState(state_start);
			regMap.CleanState(state_end1);
			regMap.CleanState(state_end2);
			regMap.CleanState(state_merge);
		}
		else
		{
			u32 byte_mask_usr = (BIT3(d.OpData)?0xFF000000:0x00000000);
			u32 byte_mask_other = (BIT0(d.OpData)?0x000000FF:0x00000000) |
								(BIT1(d.OpData)?0x0000FF00:0x00000000) |
								(BIT2(d.OpData)?0x00FF0000:0x00000000) |
								(BIT3(d.OpData)?0xFF000000:0x00000000);

			u32 byte_mask = regMap.AllocTempReg(true);
			u32 mode = regMap.AllocTempReg();

			UnpackCPSR(regMap, PSR_MODE, mode);
			jit_insn *eq_usr = jit_beqi_ui(jit_forward(), LOCALREG(mode), USR);
			jit_movi_ui(LOCALREG(byte_mask), byte_mask_other);
			jit_insn *eq_usr_done = jit_jmpi(jit_forward());
			jit_patch(eq_usr);
			jit_movi_ui(LOCALREG(byte_mask), byte_mask_usr);
			jit_patch(eq_usr_done);

			if (BIT0(d.OpData))
			{
				jit_insn *done = jit_bgei_ui(jit_forward(), LOCALREG(mode), USR);
				regMap.ReleaseTempReg(mode);

				u32 state_start = regMap.StoreState();

				{
					args.clear();
					flushs.clear();

					for (u32 i = RegisterMap::R8; i <= RegisterMap::R14; i++)
						flushs.push_back((RegisterMap::GuestRegId)i);
					flushs.push_back(RegisterMap::CPSR);
					flushs.push_back(RegisterMap::SPSR);

					ABIOp op;

					op.type = ABIOp::GUSETREG;
					op.regdata = RegisterMap::CPUPTR;
					args.push_back(op);

					if (d.I)
					{
						op.type = ABIOp::IMM;
						op.immdata.type = ImmData::IMM8;
						op.immdata.imm8 = d.Immediate & 0x1F;
						args.push_back(op);
					}
					else
					{
						u32 rm = regMap.MapReg(REGID(d.Rm));
						regMap.Lock(rm);
						u32 tmp = regMap.AllocTempReg();

						jit_andi_ui(LOCALREG(tmp), LOCALREG(rm), 0x1F);

						regMap.Unlock(rm);

						op.type = ABIOp::TEMPREG;
						op.regdata = tmp;
						args.push_back(op);
					}

					regMap.CallABI(armcpu_switchMode, args, flushs);
				}

				jit_insn* pt_end1 = PrepareSLZone();

				u32 state_end1 = regMap.StoreState();

				jit_patch(done);

				regMap.RestoreState(state_start);
				u32 state_end2 = regMap.StoreState();

				// merge states
				states.clear();
				states.push_back(state_end1);
				states.push_back(state_end2);

				u32 state_merge = regMap.CalcStates(state_start, states);

				// generate merge code
				regMap.RestoreState(state_end2);
				regMap.MergeToStates(state_merge);

				// backup pc
				jit_insn* lable_end = jit_get_label();

				// generate merge code
				regMap.RestoreState(state_end1);
				jit_set_ip(pt_end1);
				regMap.MergeToStates(state_merge);
				jit_jmpi(lable_end);

				// restore pc
				regMap.RestoreState(state_merge);
				jit_set_ip(lable_end);

				regMap.CleanState(state_start);
				regMap.CleanState(state_end1);
				regMap.CleanState(state_end2);
				regMap.CleanState(state_merge);
			}
			else
				regMap.ReleaseTempReg(mode);

			u32 cpsr = regMap.MapReg(RegisterMap::CPSR, RegisterMap::MAP_DIRTY);
			regMap.Lock(cpsr);
			u32 tmp = regMap.AllocTempReg();

			jit_notr_ui(LOCALREG(tmp), LOCALREG(byte_mask));
			jit_andr_ui(LOCALREG(cpsr), LOCALREG(cpsr), LOCALREG(tmp));

			if (d.I)
				jit_andi_ui(LOCALREG(tmp), LOCALREG(byte_mask), d.Immediate);
			else
			{
				u32 rm = regMap.MapReg(REGID(d.Rm));
				regMap.Lock(rm);

				jit_andr_ui(LOCALREG(tmp), LOCALREG(byte_mask), LOCALREG(rm));

				regMap.Unlock(rm);
			}

			jit_orr_ui(LOCALREG(cpsr), LOCALREG(cpsr), LOCALREG(tmp));

			regMap.ReleaseTempReg(tmp);
			regMap.Unlock(cpsr);

			regMap.ReleaseTempReg(byte_mask);

			{
				args.clear();
				flushs.clear();

				ABIOp op;

				op.type = ABIOp::GUSETREG;
				op.regdata = RegisterMap::CPUPTR;
				args.push_back(op);

				regMap.CallABI(armcpu_changeCPSR, args, flushs);
			}
		}
	}

	OPDECODER_DECL(IR_MRS)
	{
		u32 PROCNUM = d.ProcessID;

		u32 rd = regMap.MapReg(REGID(d.Rd), RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
		regMap.Lock(rd);
		u32 psr = regMap.MapReg(d.P ? RegisterMap::SPSR : RegisterMap::CPSR);
		regMap.Lock(psr);

		jit_movr_ui(LOCALREG(rd), LOCALREG(psr));

		regMap.Unlock(psr);
		regMap.Unlock(rd);
	}

	OPDECODER_DECL(IR_MCR)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.CPNum == 15)
		{
			std::vector<ABIOp> args;
			std::vector<RegisterMap::GuestRegId> flushs;

			{
				args.clear();
				flushs.clear();

				flushs.push_back(RegisterMap::CPSR);

				ABIOp op;

				op.type = ABIOp::GUSETREG;
				op.regdata = REGID(d.Rd);
				args.push_back(op);

				op.type = ABIOp::IMM;
				op.immdata.type = ImmData::IMM8;
				op.immdata.imm8 = d.CRn;
				args.push_back(op);

				op.type = ABIOp::IMM;
				op.immdata.type = ImmData::IMM8;
				op.immdata.imm8 = d.CRm;
				args.push_back(op);

				op.type = ABIOp::IMM;
				op.immdata.type = ImmData::IMM8;
				op.immdata.imm8 = d.CPOpc;
				args.push_back(op);

				op.type = ABIOp::IMM;
				op.immdata.type = ImmData::IMM8;
				op.immdata.imm8 = d.CP;
				args.push_back(op);

				regMap.CallABI(armcp15_moveARM2CP, args, flushs);
			}
		}
		else
		{
			INFO("ARM%c: MCR P%i, 0, R%i, C%i, C%i, %i, %i (don't allocated coprocessor)\n", 
				PROCNUM?'7':'9', d.CPNum, d.Rd, d.CRn, d.CRm, d.CPOpc, d.CP);
		}
	}

	OPDECODER_DECL(IR_MRC)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.CPNum == 15)
		{
			std::vector<ABIOp> args;
			std::vector<RegisterMap::GuestRegId> flushs;

			if (d.Rd == 15)
			{
				// fallback to interpreter
				regMap.FlushGuestReg(RegisterMap::CPSR);

				Fallback2Interpreter(d, regMap);
			}
			else
			{
				args.clear();
				flushs.clear();

				flushs.push_back(RegisterMap::CPSR);
				flushs.push_back(REGID(d.Rd));

				ABIOp op;

				op.type = ABIOp::IMM;
				op.immdata.type = ImmData::IMMPTR;
				op.immdata.immptr = &GETCPU.R[d.Rd];
				args.push_back(op);

				op.type = ABIOp::IMM;
				op.immdata.type = ImmData::IMM8;
				op.immdata.imm8 = d.CRn;
				args.push_back(op);

				op.type = ABIOp::IMM;
				op.immdata.type = ImmData::IMM8;
				op.immdata.imm8 = d.CRm;
				args.push_back(op);

				op.type = ABIOp::IMM;
				op.immdata.type = ImmData::IMM8;
				op.immdata.imm8 = d.CPOpc;
				args.push_back(op);

				op.type = ABIOp::IMM;
				op.immdata.type = ImmData::IMM8;
				op.immdata.imm8 = d.CP;
				args.push_back(op);

				regMap.CallABI(armcp15_moveCP2ARM, args, flushs);
			}
		}
		else
		{
			INFO("ARM%c: MRC P%i, 0, R%i, C%i, C%i, %i, %i (don't allocated coprocessor)\n", 
				PROCNUM?'7':'9', d.CPNum, d.Rd, d.CRn, d.CRm, d.CPOpc, d.CP);
		}
	}

	OPDECODER_DECL(IR_CLZ)
	{
		u32 PROCNUM = d.ProcessID;

		// fallback to interpreter
	}

	OPDECODER_DECL(IR_QADD)
	{
		u32 PROCNUM = d.ProcessID;

		// fallback to interpreter
	}

	OPDECODER_DECL(IR_QSUB)
	{
		u32 PROCNUM = d.ProcessID;

		// fallback to interpreter
	}

	OPDECODER_DECL(IR_QDADD)
	{
		u32 PROCNUM = d.ProcessID;

		// fallback to interpreter
	}

	OPDECODER_DECL(IR_QDSUB)
	{
		u32 PROCNUM = d.ProcessID;

		// fallback to interpreter
	}

	OPDECODER_DECL(IR_BLX_IMM)
	{
		u32 PROCNUM = d.ProcessID;

		if(d.ThumbFlag)
			PackCPSRImm(regMap, PSR_T, 0);
		else
			PackCPSRImm(regMap, PSR_T, 1);

		regMap.SetImm32(RegisterMap::R14, d.CalcNextInstruction(d) | d.ThumbFlag);
		regMap.SetImm32(RegisterMap::R15, d.Immediate);

		R15ModifiedGenerate(d, regMap);
	}

	OPDECODER_DECL(IR_BKPT)
	{
		u32 PROCNUM = d.ProcessID;

		INFO("ARM%c: Unimplemented opcode BKPT\n", PROCNUM?'7':'9');
	}
};

static void cpuReserve()
{
}

static void cpuShutdown()
{
}

static void cpuReset()
{
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
	return 0;
}

static u32 cpuGetCacheReserve()
{
	return 0;
}

static void cpuSetCacheReserve(u32 reserveInMegs)
{
}

static const char* cpuDescription()
{
	return "Arm LJit";
}

CpuBase arm_ljit =
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
