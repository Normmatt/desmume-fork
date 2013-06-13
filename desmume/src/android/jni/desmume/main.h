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

#ifndef _MAIN_H
#define _MAIN_H

#include <jni.h>
#include <android/log.h>
#include <pthread.h>

unsigned int GetCPUCount(JNIEnv* env);

unsigned int GetPrivateProfileInt(JNIEnv* env, const char* lpAppName, const char* lpKeyName, int nDefault, const char* lpFileName);

bool GetPrivateProfileBool(JNIEnv* env, const char* lpAppName, const char* lpKeyName, bool bDefault, const char* lpFileName);

unsigned int GetTickCount();
void Sleep(int ms);

class Lock {
public:
	Lock();
	Lock(pthread_mutex_t& cs);
	~Lock();
private:
	pthread_mutex_t* m_cs;
};

#ifdef __cplusplus
extern "C" {
#endif

#define APPNAME "desmume"

#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, APPNAME, __VA_ARGS__))
#define LOGW(...) ((void)__android_log_print(ANDROID_LOG_WARN, APPNAME, __VA_ARGS__))
#define LOGD(...) ((void)__android_log_print(ANDROID_LOG_DEBUG, APPNAME, __VA_ARGS__))


//JNI callbacks go here

#ifdef __cplusplus
} //end extern "C"
#endif

#endif
