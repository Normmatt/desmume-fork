/*
	Copyright (C) 2013 DeSmuME team

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
#include <jni.h>
#include <android/log.h>

#include <EGL/egl.h>

#include "OGLES2Render.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APPNAME "desmume"

#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, APPNAME, __VA_ARGS__))
#define LOGW(...) ((void)__android_log_print(ANDROID_LOG_WARN, APPNAME, __VA_ARGS__))
#define LOGD(...) ((void)__android_log_print(ANDROID_LOG_DEBUG, APPNAME, __VA_ARGS__))

#ifdef __cplusplus
} //end extern "C"
#endif

static bool oglAlreadyInit = false;

static EGLDisplay display = EGL_NO_DISPLAY;
static EGLSurface surface = EGL_NO_SURFACE;
static EGLContext context = EGL_NO_CONTEXT;

/**
 * Initialize an EGL context for the current display.
 */
bool android_opengl_init()
{
	if(oglAlreadyInit) return true;

	const EGLint attribs[] = {
            EGL_RED_SIZE, 8,
			EGL_GREEN_SIZE, 8,
			EGL_BLUE_SIZE, 8,
			EGL_ALPHA_SIZE, 8,
			EGL_DEPTH_SIZE, 16,
			EGL_STENCIL_SIZE, 8,
			EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
			EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
			EGL_NONE
    };
	EGLint major, minor;
    EGLint w, h, format;
    EGLint numConfigs;
    EGLConfig config = NULL;

    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);

    eglInitialize(display, &major, &minor);

    /* Here, the application chooses the configuration it desires. In this
     * sample, we have a very simplified selection process, where we pick
     * the first EGLConfig that matches our criteria */
    eglChooseConfig(display, attribs, &config, 1, &numConfigs);

	const EGLint surfaceAttribs[] = {
            EGL_WIDTH, 256,
			EGL_HEIGHT, 256,
			EGL_LARGEST_PBUFFER, EGL_FALSE,
			EGL_NONE
    };

    surface = eglCreatePbufferSurface(display, config, surfaceAttribs);

	const EGLint contextAttribs[] = {
			EGL_CONTEXT_CLIENT_VERSION, 2,
			EGL_NONE
    };

    context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);

    if (eglMakeCurrent(display, surface, surface, context) == EGL_FALSE) {
        LOGW("Unable to eglMakeCurrent\n");
        return false;
    }

    LOGI("EGL(%u.%u): Created OpenGLES\n", major ,minor);

	oglAlreadyInit = true;

    return true;
}

