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

#define TEMPLATE template<u32 PROCNUM> 

typedef void (FASTCALL* IROpCDecoder)(const Decoded &d, char *&szCodeBuffer);
typedef u32 (* ArmOpCompiled)();

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
	enum ARMCPU_CPSR
	{
		CPSR_MODE,
		CPSR_T,
		CPSR_F,
		CPSR_I,
		CPSR_Q,
		CPSR_V,
		CPSR_C,
		CPSR_Z,
		CPSR_N,
	};

	void FASTCALL UnpackCPSR(RegisterMap &regMap, ARMCPU_CPSR flg, u32 out)
	{
	}

	void FASTCALL PackCPSR(RegisterMap &regMap, ARMCPU_CPSR flg, u32 in)
	{
	}

	void FASTCALL SetCPSR(RegisterMap &regMap, ARMCPU_CPSR flg, bool in)
	{
	}

	void FASTCALL IRShiftOpGenerate(const Decoded &d, RegisterMap &regMap, u32 shiftop, u32 cflg)
	{
		u32 PROCNUM = d.ProcessID;

		switch (d.Typ)
		{
		case IRSHIFT_LSL:
			if (!d.R)
			{
				if (cflg != INVALID_REG_ID)
				{
					if (d.Immediate == 0)
						UnpackCPSR(regMap, CPSR_C, cflg);
					else
					{
						if (regMap.IsImm(REGID(d.Rm)))
						{
							u32 tmp = (regMap.GetImm(REGID(d.Rm))>>(32-d.Immediate)) != 0;

							jit_movi_ui(LOCALREG(cflg), tmp);
						}
						else
						{
							u32 rm = regMap.MapReg(REGID(d.Rm));

							jit_rshi_ui(LOCALREG(cflg), LOCALREG(rm), 32-d.Immediate);
							jit_andi_ui(LOCALREG(cflg), LOCALREG(cflg), 1);
						}
					}
				}

				if (d.Immediate == 0)
				{
					if (regMap.IsImm(REGID(d.Rm)))
					{
						u32 tmp = regMap.GetImm(REGID(d.Rm));

						jit_movi_ui(LOCALREG(shiftop), tmp);
					}
					else
					{
						u32 rm = regMap.MapReg(REGID(d.Rm));

						jit_movr_ui(LOCALREG(shiftop), LOCALREG(rm));
					}
				}
				else
				{
					if (regMap.IsImm(REGID(d.Rm)))
					{
						u32 tmp = regMap.GetImm(REGID(d.Rm))<<d.Immediate;

						jit_movi_ui(LOCALREG(shiftop), tmp);
					}
					else
					{
						u32 rm = regMap.MapReg(REGID(d.Rm));

						jit_lshi_ui(LOCALREG(shiftop), LOCALREG(rm), d.Immediate);
					}
				}
			}
			else
			{
				if (cflg != INVALID_REG_ID)
				{
					u32 rs = regMap.MapReg(REGID(d.Rs));
					regMap.Lock(rs);
					u32 rm = regMap.MapReg(REGID(d.Rm));
					regMap.Lock(rm);
					u32 tmp = regMap.AllocTempReg();

					jit_andi_ui(LOCALREG(shiftop), LOCALREG(rs), 0xFF);
					jit_insn *eq0 = jit_beqi_ui(jit_forward(), LOCALREG(shiftop), 0);
					jit_insn *lt32 = jit_blti_ui(jit_forward(), LOCALREG(shiftop), 32);
					jit_insn *eq32 = jit_beqi_ui(jit_forward(), LOCALREG(shiftop), 32);
					jit_movi_ui(LOCALREG(shiftop), 0);
					jit_movi_ui(LOCALREG(cflg), 0);
					jit_insn *done1 = jit_jmpi(jit_forward());
					jit_patch(eq32);
					jit_movi_ui(LOCALREG(shiftop), 0);
					jit_andi_ui(LOCALREG(cflg), LOCALREG(rm), 1);
					jit_insn *done2 = jit_jmpi(jit_forward());
					jit_patch(eq0);
					jit_movr_ui(LOCALREG(shiftop), LOCALREG(rm));
					UnpackCPSR(regMap, CPSR_C, cflg);
					jit_insn *done3 = jit_jmpi(jit_forward());
					jit_patch(lt32);
					jit_rsbi_ui(LOCALREG(tmp), LOCALREG(shiftop), 32);
					jit_rshr_ui(LOCALREG(cflg), LOCALREG(rm), LOCALREG(tmp));
					jit_andi_ui(LOCALREG(cflg), LOCALREG(cflg), 1);
					jit_lshr_ui(LOCALREG(shiftop), LOCALREG(rm), LOCALREG(shiftop));
					jit_patch(done1);
					jit_patch(done2);
					jit_patch(done3);

					regMap.ReleaseTempReg(tmp);
					regMap.Unlock(rm);
					regMap.Unlock(rs);
				}
				else
				{
					u32 rs = regMap.MapReg(REGID(d.Rs));
					regMap.Lock(rs);
					u32 rm = regMap.MapReg(REGID(d.Rm));
					regMap.Lock(rm);

					jit_andi_ui(LOCALREG(shiftop), LOCALREG(rs), 0xFF);
					jit_insn *lt32 = jit_blti_ui(jit_forward(), LOCALREG(shiftop), 32);
					jit_movi_ui(LOCALREG(shiftop), 0);
					jit_insn *done = jit_jmpi(jit_forward());
					jit_patch(lt32);
					jit_lshr_ui(LOCALREG(shiftop), LOCALREG(rm), LOCALREG(shiftop));
					jit_patch(done);

					regMap.Unlock(rm);
					regMap.Unlock(rs);
				}
			}
			break;
		case IRSHIFT_LSR:
			if (!d.R)
			{
				if (cflg != INVALID_REG_ID)
				{
					if (d.Immediate == 0)
					{
						if (regMap.IsImm(REGID(d.Rm)))
						{
							u32 tmp = BIT31(regMap.GetImm(REGID(d.Rm)));

							jit_movi_ui(LOCALREG(cflg), tmp);
						}
						else
						{
							u32 rm = regMap.MapReg(REGID(d.Rm));

							jit_rshi_ui(LOCALREG(cflg), LOCALREG(rm), 31);
							//jit_andi_ui(LOCALREG(cflg), LOCALREG(cflg), 1);
						}
					}
					else
					{
						if (regMap.IsImm(REGID(d.Rm)))
						{
							u32 tmp = BIT_N(regMap.GetImm(REGID(d.Rm)), d.Immediate-1);

							jit_movi_ui(LOCALREG(cflg), tmp);
						}
						else
						{
							u32 rm = regMap.MapReg(REGID(d.Rm));

							jit_rshi_ui(LOCALREG(cflg), LOCALREG(rm), d.Immediate-1);
							jit_andi_ui(LOCALREG(cflg), LOCALREG(cflg), 1);
						}
					}
				}

				if (d.Immediate == 0)
					jit_movi_ui(LOCALREG(shiftop), 0);
				else
				{
					if (regMap.IsImm(REGID(d.Rm)))
					{
						u32 tmp = regMap.GetImm(REGID(d.Rm))>>d.Immediate;

						jit_movi_ui(LOCALREG(shiftop), tmp);
					}
					else
					{
						u32 rm = regMap.MapReg(REGID(d.Rm));

						jit_rshi_ui(LOCALREG(shiftop), LOCALREG(rm), d.Immediate);
					}
				}
			}
			else
			{
				if (cflg != INVALID_REG_ID)
				{
					u32 rs = regMap.MapReg(REGID(d.Rs));
					regMap.Lock(rs);
					u32 rm = regMap.MapReg(REGID(d.Rm));
					regMap.Lock(rm);
					u32 tmp = regMap.AllocTempReg();

					jit_andi_ui(LOCALREG(shiftop), LOCALREG(rs), 0xFF);
					jit_insn *eq0 = jit_beqi_ui(jit_forward(), LOCALREG(shiftop), 0);
					jit_insn *lt32 = jit_blti_ui(jit_forward(), LOCALREG(shiftop), 32);
					jit_insn *eq32 = jit_beqi_ui(jit_forward(), LOCALREG(shiftop), 32);
					jit_movi_ui(LOCALREG(shiftop), 0);
					jit_movi_ui(LOCALREG(cflg), 0);
					jit_insn *done1 = jit_jmpi(jit_forward());
					jit_patch(eq32);
					jit_movi_ui(LOCALREG(shiftop), 0);
					jit_rshi_ui(LOCALREG(cflg), LOCALREG(rm), 31);
					//jit_andi_ui(LOCALREG(cflg), LOCALREG(cflg), 1);
					jit_insn *done2 = jit_jmpi(jit_forward());
					jit_patch(eq0);
					jit_movr_ui(LOCALREG(shiftop), LOCALREG(rm));
					UnpackCPSR(regMap, CPSR_C, cflg);
					jit_insn *done3 = jit_jmpi(jit_forward());
					jit_patch(lt32);
					jit_subi_ui(LOCALREG(tmp), LOCALREG(shiftop), 1);
					jit_rshr_ui(LOCALREG(cflg), LOCALREG(rm), LOCALREG(tmp));
					jit_andi_ui(LOCALREG(cflg), LOCALREG(cflg), 1);
					jit_rshr_ui(LOCALREG(shiftop), LOCALREG(rm), LOCALREG(shiftop));
					jit_patch(done1);
					jit_patch(done2);
					jit_patch(done3);

					regMap.ReleaseTempReg(tmp);
					regMap.Unlock(rm);
					regMap.Unlock(rs);
				}
				else
				{
					u32 rs = regMap.MapReg(REGID(d.Rs));
					regMap.Lock(rs);
					u32 rm = regMap.MapReg(REGID(d.Rm));
					regMap.Lock(rm);

					jit_andi_ui(LOCALREG(shiftop), LOCALREG(rs), 0xFF);
					jit_insn *lt32 = jit_blti_ui(jit_forward(), LOCALREG(shiftop), 32);
					jit_movi_ui(LOCALREG(shiftop), 0);
					jit_insn *done = jit_jmpi(jit_forward());
					jit_patch(lt32);
					jit_rshr_ui(LOCALREG(shiftop), LOCALREG(rm), LOCALREG(shiftop));
					jit_patch(done);

					regMap.Unlock(rm);
					regMap.Unlock(rs);
				}
			}
			break;
		case IRSHIFT_ASR:
			if (!d.R)
			{
				if (cflg != INVALID_REG_ID)
				{
					if (d.Immediate == 0)
					{
						if (regMap.IsImm(REGID(d.Rm)))
						{
							u32 tmp = BIT31(regMap.GetImm(REGID(d.Rm)));

							jit_movi_ui(LOCALREG(cflg), tmp);
						}
						else
						{
							u32 rm = regMap.MapReg(REGID(d.Rm));

							jit_rshi_ui(LOCALREG(cflg), LOCALREG(rm), 31);
							//jit_andi_ui(LOCALREG(cflg), LOCALREG(cflg), 1);
						}
					}
					else
					{
						if (regMap.IsImm(REGID(d.Rm)))
						{
							u32 tmp = BIT_N(regMap.GetImm(REGID(d.Rm)), d.Immediate-1);

							jit_movi_ui(LOCALREG(cflg), tmp);
						}
						else
						{
							u32 rm = regMap.MapReg(REGID(d.Rm));

							jit_rshi_ui(LOCALREG(cflg), LOCALREG(rm), d.Immediate-1);
							jit_andi_ui(LOCALREG(cflg), LOCALREG(cflg), 1);
						}
					}
				}

				if (d.Immediate == 0)
				{
					if (regMap.IsImm(REGID(d.Rm)))
					{
						u32 tmp = BIT31(regMap.GetImm(REGID(d.Rm))) * 0xFFFFFFFF;

						jit_movi_ui(LOCALREG(shiftop), tmp);
					}
					else
					{
						u32 rm = regMap.MapReg(REGID(d.Rm));

						//jit_rshi_ui(LOCALREG(cflg), LOCALREG(rm), 31);
						//jit_muli_ui(LOCALREG(cflg), LOCALREG(cflg), 0xFFFFFFFF);
						jit_rshi_i(LOCALREG(cflg), LOCALREG(rm), 31);
					}
				}
				else
				{
					if (regMap.IsImm(REGID(d.Rm)))
					{
						u32 tmp = (u32)((s32)regMap.MapReg(REGID(d.Rm)) >> d.Immediate);

						jit_movi_ui(LOCALREG(shiftop), tmp);
					}
					else
					{
						u32 rm = regMap.MapReg(REGID(d.Rm));

						jit_rshi_i(LOCALREG(shiftop), LOCALREG(rm), d.Immediate);
					}
				}
			}
			else
			{
				if (cflg != INVALID_REG_ID)
				{
					u32 rs = regMap.MapReg(REGID(d.Rs));
					regMap.Lock(rs);
					u32 rm = regMap.MapReg(REGID(d.Rm));
					regMap.Lock(rm);
					u32 tmp = regMap.AllocTempReg();

					jit_andi_ui(LOCALREG(shiftop), LOCALREG(rs), 0xFF);
					jit_insn *eq0 = jit_beqi_ui(jit_forward(), LOCALREG(shiftop), 0);
					jit_insn *lt32 = jit_blti_ui(jit_forward(), LOCALREG(shiftop), 32);
					//jit_rshi_ui(LOCALREG(shiftop), LOCALREG(rm), 31);
					//jit_muli_ui(LOCALREG(shiftop), LOCALREG(shiftop), 0xFFFFFFFF);
					jit_rshi_i(LOCALREG(shiftop), LOCALREG(rm), 31);
					jit_rshi_ui(LOCALREG(cflg), LOCALREG(rm), 31);
					//jit_andi_ui(LOCALREG(cflg), LOCALREG(cflg), 1);
					jit_insn *done1 = jit_jmpi(jit_forward());
					jit_patch(eq0);
					jit_movr_ui(LOCALREG(shiftop), LOCALREG(rm));
					UnpackCPSR(regMap, CPSR_C, cflg);
					jit_insn *done2 = jit_jmpi(jit_forward());
					jit_patch(lt32);
					jit_subi_ui(LOCALREG(tmp), LOCALREG(shiftop), 1);
					jit_rshr_ui(LOCALREG(cflg), LOCALREG(rm), LOCALREG(tmp));
					jit_andi_ui(LOCALREG(cflg), LOCALREG(cflg), 1);
					jit_rshr_i(LOCALREG(shiftop), LOCALREG(rm), LOCALREG(shiftop));
					jit_patch(done1);
					jit_patch(done2);

					regMap.ReleaseTempReg(tmp);
					regMap.Unlock(rm);
					regMap.Unlock(rs);
				}
				else
				{
					u32 rs = regMap.MapReg(REGID(d.Rs));
					regMap.Lock(rs);
					u32 rm = regMap.MapReg(REGID(d.Rm));
					regMap.Lock(rm);

					jit_andi_ui(LOCALREG(shiftop), LOCALREG(rs), 0xFF);
					jit_insn *eq0 = jit_beqi_ui(jit_forward(), LOCALREG(shiftop), 0);
					jit_insn *lt32 = jit_blti_ui(jit_forward(), LOCALREG(shiftop), 32);
					//jit_rshi_ui(LOCALREG(shiftop), LOCALREG(rm), 31);
					//jit_muli_ui(LOCALREG(shiftop), LOCALREG(shiftop), 0xFFFFFFFF);
					jit_rshi_i(LOCALREG(shiftop), LOCALREG(rm), 31);
					jit_insn *done1 = jit_jmpi(jit_forward());
					jit_patch(eq0);
					jit_movr_ui(LOCALREG(shiftop), LOCALREG(rm));
					jit_insn *done2 = jit_jmpi(jit_forward());
					jit_patch(lt32);
					jit_rshr_i(LOCALREG(shiftop), LOCALREG(rm), LOCALREG(shiftop));
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
				if (cflg != INVALID_REG_ID)
				{
					if (d.Immediate == 0)
					{
						if (regMap.IsImm(REGID(d.Rm)))
						{
							u32 tmp = BIT0(regMap.GetImm(REGID(d.Rm)));

							jit_movi_ui(LOCALREG(cflg), tmp);
						}
						else
						{
							u32 rm = regMap.MapReg(REGID(d.Rm));

							jit_andi_ui(LOCALREG(cflg), LOCALREG(rm), 1);
						}
					}
					else
					{
						if (regMap.IsImm(REGID(d.Rm)))
						{
							u32 tmp = BIT_N(regMap.GetImm(REGID(d.Rm)), d.Immediate-1);

							jit_movi_ui(LOCALREG(cflg), tmp);
						}
						else
						{
							u32 rm = regMap.MapReg(REGID(d.Rm));

							jit_rshi_ui(LOCALREG(cflg), LOCALREG(rm), d.Immediate-1);
							jit_andi_ui(LOCALREG(cflg), LOCALREG(cflg), 1);
						}
					}
				}

				if (d.Immediate == 0)
				{
					if (regMap.IsImm(REGID(d.Rm)))
					{
						u32 tmp = regMap.GetImm(REGID(d.Rm)) >> 1;

						UnpackCPSR(regMap, CPSR_C, shiftop);
						jit_lshi_ui(LOCALREG(shiftop), LOCALREG(shiftop), 31);
						jit_ori_ui(LOCALREG(shiftop), LOCALREG(shiftop), tmp);
					}
					else
					{
						u32 rm = regMap.MapReg(REGID(d.Rm));
						regMap.Lock(rm);
						u32 tmp = regMap.AllocTempReg();

						UnpackCPSR(regMap, CPSR_C, tmp);
						jit_lshi_ui(LOCALREG(tmp), LOCALREG(tmp), 31);
						jit_rshi_ui(LOCALREG(shiftop), LOCALREG(rm), 1);
						jit_orr_ui(LOCALREG(shiftop), LOCALREG(shiftop), LOCALREG(tmp));

						regMap.ReleaseTempReg(tmp);
						regMap.Unlock(rm);
					}
				}
				else
				{
					if (regMap.IsImm(REGID(d.Rm)))
					{
						u32 tmp = ROR(regMap.GetImm(REGID(d.Rm)), d.Immediate);

						jit_movi_ui(LOCALREG(shiftop), tmp);
					}
					else
					{
						u32 rm = regMap.MapReg(REGID(d.Rm));
						regMap.Lock(rm);
						u32 tmp = regMap.AllocTempReg();

						jit_rshi_ui(LOCALREG(tmp), LOCALREG(rm), d.Immediate);
						jit_lshi_ui(LOCALREG(shiftop), LOCALREG(rm), 32-d.Immediate);
						jit_orr_ui(LOCALREG(shiftop), LOCALREG(shiftop), LOCALREG(tmp));

						regMap.ReleaseTempReg(tmp);
						regMap.Unlock(rm);
					}
				}
			}
			else
			{
				if (cflg != INVALID_REG_ID)
				{
					u32 rs = regMap.MapReg(REGID(d.Rs));
					regMap.Lock(rs);
					u32 rm = regMap.MapReg(REGID(d.Rm));
					regMap.Lock(rm);
					u32 tmp = regMap.AllocTempReg();

					jit_andi_ui(LOCALREG(shiftop), LOCALREG(rs), 0xFF);
					jit_insn *eq0 = jit_beqi_ui(jit_forward(), LOCALREG(shiftop), 0);
					jit_andi_ui(LOCALREG(shiftop), LOCALREG(rs), 0x1F);
					jit_insn *eq0_1F = jit_beqi_ui(jit_forward(), LOCALREG(shiftop), 0);
					jit_subi_ui(LOCALREG(tmp), LOCALREG(shiftop), 1);
					jit_rshr_ui(LOCALREG(cflg), LOCALREG(rm), LOCALREG(tmp));
					jit_andi_ui(LOCALREG(cflg), LOCALREG(cflg), 1);
					jit_rsbi_ui(LOCALREG(tmp), LOCALREG(shiftop), 32);
					jit_rshr_ui(LOCALREG(shiftop), LOCALREG(rm), LOCALREG(shiftop));
					jit_lshr_ui(LOCALREG(tmp), LOCALREG(rm), LOCALREG(tmp));
					jit_orr_ui(LOCALREG(shiftop), LOCALREG(shiftop), LOCALREG(tmp));
					jit_insn *done1 = jit_jmpi(jit_forward());
					jit_patch(eq0_1F);
					jit_rshi_ui(LOCALREG(cflg), LOCALREG(rm), 31);
					jit_movr_ui(LOCALREG(shiftop), LOCALREG(rm));
					jit_insn *done2 = jit_jmpi(jit_forward());
					jit_patch(eq0);
					UnpackCPSR(regMap, CPSR_C, cflg);
					jit_movr_ui(LOCALREG(shiftop), LOCALREG(rm));
					jit_patch(done1);
					jit_patch(done2);

					regMap.ReleaseTempReg(tmp);
					regMap.Unlock(rm);
					regMap.Unlock(rs);
				}
				else
				{
					u32 rs = regMap.MapReg(REGID(d.Rs));
					regMap.Lock(rs);
					u32 rm = regMap.MapReg(REGID(d.Rm));
					regMap.Lock(rm);
					u32 tmp = regMap.AllocTempReg();

					jit_andi_ui(LOCALREG(shiftop), LOCALREG(rs), 0x1F);
					jit_insn *eq0 = jit_beqi_ui(jit_forward(), LOCALREG(shiftop), 0);
					jit_rsbi_ui(LOCALREG(tmp), LOCALREG(shiftop), 32);
					jit_rshr_ui(LOCALREG(shiftop), LOCALREG(rm), LOCALREG(shiftop));
					jit_lshr_ui(LOCALREG(tmp), LOCALREG(rm), LOCALREG(tmp));
					jit_orr_ui(LOCALREG(shiftop), LOCALREG(shiftop), LOCALREG(tmp));
					jit_insn *done = jit_jmpi(jit_forward());
					jit_patch(eq0);
					jit_movr_ui(LOCALREG(shiftop), LOCALREG(rm));
					jit_patch(done);

					regMap.ReleaseTempReg(tmp);
					regMap.Unlock(rm);
					regMap.Unlock(rs);
				}
			}
			break;
		default:
			INFO("Unknow Shift Op : %u.\n", d.Typ);
			if (cflg != INVALID_REG_ID)
				jit_movi_ui(LOCALREG(cflg), 0);
			jit_movi_ui(LOCALREG(shiftop), 0);
			break;
		}
	}

	void FASTCALL DataProcessLoadCPSRGenerate(const Decoded &d)
	{
		u32 PROCNUM = d.ProcessID;
	}

//------------------------------------------------------------
//                         IROp decoder
//------------------------------------------------------------
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
