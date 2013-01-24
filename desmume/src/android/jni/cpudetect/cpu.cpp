/*
	Copyright (C) 2012 Jeffrey Quesnelle

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

#include <cpu-features.h>
#include <jni.h>

#define JNI_NOARGS(X) Java_com_opendoorstudios_desmume_DeSmuME_##X(JNIEnv* env, jclass* clazz)

#define CPUTYPE_COMPAT 0
#define CPUTYPE_V7 1
#define CPUTYPE_NEON 2

extern "C"
{

jint JNI_NOARGS(getCPUType)
{
	AndroidCpuFamily cpuFamily = android_getCpuFamily();
	if (cpuFamily != ANDROID_CPU_FAMILY_ARM)
		return CPUTYPE_COMPAT;

	uint64_t cpuFeatures = android_getCpuFeatures();
	if ((cpuFeatures & ANDROID_CPU_ARM_FEATURE_NEON) != 0)
	{
		return CPUTYPE_NEON;
	}
	else if ((cpuFeatures & ANDROID_CPU_ARM_FEATURE_VFP_D32) != 0)
	{
		return CPUTYPE_NEON;
	}
	else if ((cpuFeatures & ANDROID_CPU_ARM_FEATURE_ARMv7) != 0)
	{
		return CPUTYPE_V7;
	}
    else
    {
        return CPUTYPE_COMPAT;
    }
}

jint JNI_NOARGS(getCPUCount)
{
	return android_getCpuCount();
}

}
