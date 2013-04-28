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

	static jit_gpr_t LocalMap[JIT_R_NUM + JIT_V_NUM];

	FORCEINLINE jit_gpr_t LOCALREG_INIT()
	{
		for (u32 i = 0; i < JIT_R_NUM; i++)
			LocalMap[i] = JIT_R(i);

		for (u32 i = JIT_R_NUM; i < JIT_R_NUM + JIT_V_NUM; i++)
			LocalMap[i] = JIT_V(i);
	}

	FORCEINLINE jit_gpr_t LOCALREG(u32 i)
	{
		return LocalMap[i];
	}

	FORCEINLINE LocalRegType LOCALREGTYPE(u32 i)
	{
		if (i < JIT_R_NUM)
			return LRT_R;

		return LRT_V;
	}

	FORCEINLINE RegisterMap::GuestRegId REGID(u32 i)
	{
		return (RegisterMap::GuestRegId)(i);
	}

	class RegisterMapImp : public RegisterMap
	{
		public:
			void CallABIBefore();
			void CallABIAfter();

		protected:
			void StartBlock();
			void EndBlock();
			void StartSubBlock();
			void EndSubBlock();
			void StoreGuestRegImp(u32 hostreg, GuestRegId guestreg);
			void LoadGuestRegImp(u32 hostreg, GuestRegId guestreg);
			void StoreImm(u32 hostreg, u32 data);
			void LoadImm(u32 hostreg, u32 data);
	};

	void RegisterMapImp::CallABIBefore()
	{
	}

	void RegisterMapImp::CallABIAfter()
	{
	}

	void RegisterMapImp::StartBlock()
	{
	}

	void RegisterMapImp::EndBlock()
	{
	}

	void RegisterMapImp::StartSubBlock()
	{
	}

	void RegisterMapImp::EndSubBlock()
	{
	}

	void RegisterMapImp::StoreGuestRegImp(u32 hostreg, GuestRegId guestreg)
	{
	}

	void RegisterMapImp::LoadGuestRegImp(u32 hostreg, GuestRegId guestreg)
	{
	}

	void RegisterMapImp::StoreImm(u32 hostreg, u32 data)
	{
	}

	void RegisterMapImp::LoadImm(u32 hostreg, u32 data)
	{
	}

//------------------------------------------------------------
//                         Memory type
//------------------------------------------------------------

//------------------------------------------------------------
//                         Help function
//------------------------------------------------------------
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
	}

	void FASTCALL UnpackCPSR(RegisterMap &regMap, ARMCPU_PSR flg, u32 out)
	{
	}

	void FASTCALL PackCPSR(RegisterMap &regMap, ARMCPU_PSR flg, u32 in)
	{
	}

	void FASTCALL PackCPSRImm(RegisterMap &regMap, ARMCPU_PSR flg, u32 in)
	{
	}

	struct ShiftOut
	{
		u32 shiftop;
		u32 cflg;
		bool shiftopimm;
		bool cflgimm;

		bool cleaned;

		ShiftOut()
			: shiftop(INVALID_REG_ID)
			, cflg(INVALID_REG_ID)
			, shiftopimm(false)
			, cflgimm(false)

			, cleaned(false)
		{
		}

		void Clean(RegisterMap &regMap)
		{
			if (!cleaned)
			{
				if (!shiftopimm)
					regMap.ReleaseTempReg(shiftop);
				if (!cflgimm)
					regMap.ReleaseTempReg(cflg);

				cleaned = true;
			}
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
							Out.cflg = (regMap.GetImm(REGID(d.Rm))>>(32-d.Immediate)) != 0;
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
						Out.shiftop = regMap.GetImm(REGID(d.Rm));
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
						Out.shiftop = regMap.GetImm(REGID(d.Rm))<<d.Immediate;
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
					UnpackCPSR(regMap, PSR_C, Out.cflg);
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
							Out.cflg = BIT31(regMap.GetImm(REGID(d.Rm)));
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
							Out.cflg = BIT_N(regMap.GetImm(REGID(d.Rm)), d.Immediate-1);
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
						Out.shiftop = regMap.GetImm(REGID(d.Rm))>>d.Immediate;
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
					UnpackCPSR(regMap, PSR_C, Out.cflg);
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
							Out.cflg = BIT31(regMap.GetImm(REGID(d.Rm)));
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
							Out.cflg = BIT_N(regMap.GetImm(REGID(d.Rm)), d.Immediate-1);
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
						Out.shiftop = BIT31(regMap.GetImm(REGID(d.Rm))) * 0xFFFFFFFF;
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
					UnpackCPSR(regMap, PSR_C, Out.cflg);
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
							Out.cflg = BIT0(regMap.GetImm(REGID(d.Rm)));
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
							Out.cflg = BIT_N(regMap.GetImm(REGID(d.Rm)), d.Immediate-1);
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

						u32 tmp = regMap.GetImm(REGID(d.Rm)) >> 1;

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
						Out.shiftop = ROR(regMap.GetImm(REGID(d.Rm)), d.Immediate);
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
					UnpackCPSR(regMap, PSR_C, Out.cflg);
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

		u32 tmp1 = regMap.AllocTempReg(true);
		u32 tmp2 = regMap.AllocTempReg();
		u32 spsr = regMap.MapReg(RegisterMap::SPSR);
		jit_movr_ui(LOCALREG(tmp1), LOCALREG(spsr));
		UnpackPSR(PSR_MODE, tmp1, tmp2);

		for (u32 i = RegisterMap::R8; i <= RegisterMap::R14; i++)
			regMap.FlushGuestReg((RegisterMap::GuestRegId)i);
		regMap.FlushGuestReg(RegisterMap::CPSR);
		regMap.FlushGuestReg(RegisterMap::SPSR);

		jit_prepare(2);
		jit_pusharg_uc(LOCALREG(tmp2));
		regMap.ReleaseTempReg(tmp2);
		u32 cpuptr = regMap.MapReg(RegisterMap::CPUPTR);
		jit_pusharg_p(LOCALREG(cpuptr));
		regMap.CallABIBefore();
		jit_finish(armcpu_switchMode);
		regMap.CallABIAfter();

		u32 cpsr = regMap.MapReg(RegisterMap::CPSR);
		jit_movr_ui(LOCALREG(cpsr), LOCALREG(tmp1));
		cpuptr = regMap.MapReg(RegisterMap::CPUPTR);
		jit_prepare(1);
		jit_pusharg_p(LOCALREG(cpuptr));
		regMap.CallABIBefore();
		jit_finish(armcpu_changeCPSR);
		regMap.CallABIAfter();

		UnpackCPSR(regMap, PSR_T, tmp1);
		jit_lshi_ui(LOCALREG(tmp1), LOCALREG(tmp1), 1);
		jit_ori_ui(LOCALREG(tmp1), LOCALREG(tmp1), 0xFFFFFFFC);
		u32 r15 = regMap.MapReg(RegisterMap::R15);
		jit_andr_ui(LOCALREG(r15), LOCALREG(r15), LOCALREG(tmp1));

		regMap.ReleaseTempReg(tmp1);
	}

	void FASTCALL LDM_S_LoadCPSRGenerate(const Decoded &d, RegisterMap &regMap)
	{
		u32 PROCNUM = d.ProcessID;

		DataProcessLoadCPSRGenerate(d, regMap);
	}

	void FASTCALL R15ModifiedGenerate(const Decoded &d, RegisterMap &regMap)
	{
		u32 PROCNUM = d.ProcessID;

		u32 off = offsetof(armcpu_t, instruct_adr);

		u32 cpuptr = regMap.MapReg(RegisterMap::CPUPTR);
		regMap.Lock(cpuptr);
		u32 r15 = regMap.MapReg(RegisterMap::R15);
		regMap.Lock(r15);

		jit_stxi_ui(off, LOCALREG(cpuptr), LOCALREG(r15));

		regMap.Unlock(r15);
		regMap.Unlock(cpuptr);
	}

//------------------------------------------------------------
//                         IROp decoder
//------------------------------------------------------------
	OPDECODER_DECL(IR_UND)
	{
		u32 PROCNUM = d.ProcessID;

		INFO("IR_UND\n");

		u32 tmp = regMap.AllocTempReg();
		u32 cpuptr = regMap.MapReg(RegisterMap::CPUPTR);
		regMap.Lock(cpuptr);

		u32 off = offsetof(armcpu_t, instruction);
		if (d.ThumbFlag)
			jit_movi_ui(LOCALREG(tmp), d.Instruction.ThumbOp);
		else
			jit_movi_ui(LOCALREG(tmp), d.Instruction.ArmOp);
		jit_stxi_ui(off, LOCALREG(cpuptr), LOCALREG(tmp));

		off = offsetof(armcpu_t, instruct_adr);
		jit_movi_ui(LOCALREG(tmp), d.Address);
		jit_stxi_ui(off, LOCALREG(cpuptr), LOCALREG(tmp));

		jit_prepare(1);
		jit_pusharg_p(LOCALREG(cpuptr));
		regMap.Unlock(cpuptr);
		regMap.CallABIBefore();
		jit_finish(TRAPUNDEF);
		regMap.CallABIAfter();
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

		u32 tmp = regMap.AllocTempReg();
		u32 cpuptr = regMap.MapReg(RegisterMap::CPUPTR);
		regMap.Lock(cpuptr);

		u32 off = offsetof(armcpu_t, instruction);
		if (d.ThumbFlag)
			jit_movi_ui(LOCALREG(tmp), d.Instruction.ThumbOp);
		else
			jit_movi_ui(LOCALREG(tmp), d.Instruction.ArmOp);
		jit_stxi_ui(off, LOCALREG(cpuptr), LOCALREG(tmp));

		off = offsetof(armcpu_t, instruct_adr);
		jit_movi_ui(LOCALREG(tmp), d.Address);
		jit_stxi_ui(off, LOCALREG(cpuptr), LOCALREG(tmp));

		jit_prepare(1);
		jit_pusharg_p(LOCALREG(cpuptr));
		regMap.Unlock(cpuptr);
		regMap.CallABIBefore();
		jit_finish(TRAPUNDEF);
		regMap.CallABIAfter();
	}

	OPDECODER_DECL(IR_T32P2)
	{
		u32 PROCNUM = d.ProcessID;

		INFO("IR_T32P2\n");

		u32 tmp = regMap.AllocTempReg();
		u32 cpuptr = regMap.MapReg(RegisterMap::CPUPTR);
		regMap.Lock(cpuptr);

		u32 off = offsetof(armcpu_t, instruction);
		if (d.ThumbFlag)
			jit_movi_ui(LOCALREG(tmp), d.Instruction.ThumbOp);
		else
			jit_movi_ui(LOCALREG(tmp), d.Instruction.ArmOp);
		jit_stxi_ui(off, LOCALREG(cpuptr), LOCALREG(tmp));

		off = offsetof(armcpu_t, instruct_adr);
		jit_movi_ui(LOCALREG(tmp), d.Address);
		jit_stxi_ui(off, LOCALREG(cpuptr), LOCALREG(tmp));

		jit_prepare(1);
		jit_pusharg_p(LOCALREG(cpuptr));
		regMap.Unlock(cpuptr);
		regMap.CallABIBefore();
		jit_finish(TRAPUNDEF);
		regMap.CallABIAfter();
	}

	OPDECODER_DECL(IR_MOV)
	{
		u32 PROCNUM = d.ProcessID;

		if (d.I)
		{
			regMap.SetImm(REGID(d.Rd), d.Immediate);

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

			if (shift_out.shiftopimm)
				regMap.SetImm(REGID(d.Rd), shift_out.shiftop);
			else
			{
				u32 rd = regMap.MapReg(REGID(d.Rd), RegisterMap::MAP_DIRTY | RegisterMap::MAP_NOTINIT);
				jit_movr_ui(LOCALREG(rd), LOCALREG(shift_out.shiftop));
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
				if (d.FlagsSet & FLAG_N)
				{
					if (regMap.IsImm(REGID(d.Rd)))
						PackCPSRImm(regMap, PSR_N, BIT31(regMap.GetImm(REGID(d.Rd))));
					else
					{
						u32 tmp = regMap.AllocTempReg();
						jit_rshi_ui(LOCALREG(tmp), LOCALREG(shift_out.shiftop), 31);
						PackCPSR(regMap, PSR_N, tmp);
						regMap.ReleaseTempReg(tmp);
					}
				}
				if (d.FlagsSet & FLAG_Z)
				{
					if (regMap.IsImm(REGID(d.Rd)))
						PackCPSRImm(regMap, PSR_Z, regMap.GetImm(REGID(d.Rd))==0);
					else
					{
						u32 tmp = regMap.AllocTempReg();
						jit_eqi_ui(LOCALREG(tmp), LOCALREG(shift_out.shiftop), 0);
						PackCPSR(regMap, PSR_N, tmp);
						regMap.ReleaseTempReg(tmp);
					}
				}
			}

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
			regMap.SetImm(REGID(d.Rd), ~d.Immediate);

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
				regMap.SetImm(REGID(d.Rd), ~shift_out.shiftop);
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
						PackCPSR(regMap, PSR_C, shift_out.cflg);
				}
				if (d.FlagsSet & FLAG_N)
				{
					if (regMap.IsImm(REGID(d.Rd)))
						PackCPSRImm(regMap, PSR_N, BIT31(regMap.GetImm(REGID(d.Rd))));
					else
					{
						jit_rshi_ui(LOCALREG(shift_out.shiftop), LOCALREG(rd), 31);
						PackCPSR(regMap, PSR_N, shift_out.shiftop);
					}
				}
				if (d.FlagsSet & FLAG_Z)
				{
					if (regMap.IsImm(REGID(d.Rd)))
						PackCPSRImm(regMap, PSR_Z, regMap.GetImm(REGID(d.Rd))==0);
					else
					{
						jit_eqi_ui(LOCALREG(shift_out.shiftop), LOCALREG(rd), 0);
						PackCPSR(regMap, PSR_N, shift_out.shiftop);
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
				regMap.SetImm(REGID(d.Rd), regMap.GetImm(REGID(d.Rn)) & d.Immediate);
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
				regMap.SetImm(REGID(d.Rd), regMap.GetImm(REGID(d.Rn)) & shift_out.shiftop);
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
					PackCPSRImm(regMap, PSR_N, BIT31(regMap.GetImm(REGID(d.Rd))));
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
					PackCPSRImm(regMap, PSR_Z, regMap.GetImm(REGID(d.Rd))==0);
				else
				{
					u32 tmp = regMap.AllocTempReg();

					jit_eqi_ui(LOCALREG(tmp), LOCALREG(rd), 0);
					PackCPSR(regMap, PSR_N, tmp);
						
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
				dst = regMap.GetImm(REGID(d.Rn)) & d.Immediate;
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
				dst = regMap.GetImm(REGID(d.Rn)) & shift_out.shiftop;
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
					PackCPSR(regMap, PSR_N, tmp);
						
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
				regMap.SetImm(REGID(d.Rd), regMap.GetImm(REGID(d.Rn)) ^ d.Immediate);
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
				regMap.SetImm(REGID(d.Rd), regMap.GetImm(REGID(d.Rn)) ^ shift_out.shiftop);
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
					PackCPSRImm(regMap, PSR_N, BIT31(regMap.GetImm(REGID(d.Rd))));
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
					PackCPSRImm(regMap, PSR_Z, regMap.GetImm(REGID(d.Rd))==0);
				else
				{
					u32 tmp = regMap.AllocTempReg();

					jit_eqi_ui(LOCALREG(tmp), LOCALREG(rd), 0);
					PackCPSR(regMap, PSR_N, tmp);
						
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
				dst = regMap.GetImm(REGID(d.Rn)) ^ d.Immediate;
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
				dst = regMap.GetImm(REGID(d.Rn)) ^ shift_out.shiftop;
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
					PackCPSR(regMap, PSR_N, tmp);
						
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
				regMap.SetImm(REGID(d.Rd), regMap.GetImm(REGID(d.Rn)) | d.Immediate);
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
				regMap.SetImm(REGID(d.Rd), regMap.GetImm(REGID(d.Rn)) | shift_out.shiftop);
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
					PackCPSRImm(regMap, PSR_N, BIT31(regMap.GetImm(REGID(d.Rd))));
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
					PackCPSRImm(regMap, PSR_Z, regMap.GetImm(REGID(d.Rd))==0);
				else
				{
					u32 tmp = regMap.AllocTempReg();

					jit_eqi_ui(LOCALREG(tmp), LOCALREG(rd), 0);
					PackCPSR(regMap, PSR_N, tmp);
						
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
				regMap.SetImm(REGID(d.Rd), regMap.GetImm(REGID(d.Rn)) & (~d.Immediate));
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
				regMap.SetImm(REGID(d.Rd), regMap.GetImm(REGID(d.Rn)) & (~shift_out.shiftop));
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
					PackCPSRImm(regMap, PSR_N, BIT31(regMap.GetImm(REGID(d.Rd))));
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
					PackCPSRImm(regMap, PSR_Z, regMap.GetImm(REGID(d.Rd))==0);
				else
				{
					u32 tmp = regMap.AllocTempReg();

					jit_eqi_ui(LOCALREG(tmp), LOCALREG(rd), 0);
					PackCPSR(regMap, PSR_N, tmp);
						
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
