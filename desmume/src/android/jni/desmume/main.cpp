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

#include <jni.h>
#include <errno.h>

#include <android/native_window_jni.h>

#include <android/sensor.h>
#include <android/bitmap.h>


#include "main.h"
#include "OGLES2Render.h"
#include "rasterize.h"
#include "SPU.h"
#include "debug.h"
#include "NDSSystem.h"
#include "path.h"
#include "GPU_OSD.h"
#include "addons.h"
#include "slot1.h"
#include "saves.h"
#include "throttle.h"
#include "video.h"
#include "ogl.h"
#include "OpenArchive.h"
#include "sndopensl.h"
#include "cheatSystem.h"
#ifdef HAVE_NEON
#include "neontest.h"
#include "utils/math-neon/math_neon.h"
#endif

#define JNI(X,...) Java_com_opendoorstudios_desmume_DeSmuME_##X(JNIEnv* env, jclass* clazz, __VA_ARGS__)
#define JNI_NOARGS(X) Java_com_opendoorstudios_desmume_DeSmuME_##X(JNIEnv* env, jclass* clazz)

int scanline_filter_a = 0;
int scanline_filter_b = 2;
int scanline_filter_c = 2;
int scanline_filter_d = 4;

GPU3DInterface *core3DList[] = {
	&gpu3DNull,
	&gpu3Dgles2,
	&gpu3DRasterize,
	NULL
};

SoundInterface_struct *SNDCoreList[] = {
	&SNDDummy,
	&SNDOpenSL,
	NULL
};

volatile bool execute = false;
volatile bool paused = true;
volatile BOOL pausedByMinimize = FALSE;
bool autoframeskipenab=1;
int frameskiprate=1;
int lastskiprate=0;
int emu_paused = 0;
bool frameAdvance = false;
bool continuousframeAdvancing = false;
bool staterewindingenabled = false;
struct NDS_fw_config_data fw_config;
bool FrameLimit = true;
int sndcoretype, sndbuffersize;
static int snd_synchmode=0;
static int snd_synchmethod=0;
AndroidBitmapInfo bitmapInfo;
const char* IniName = NULL;
char androidTempPath[1024];
extern bool enableMicrophone;

//triple buffering logic
u16 displayBuffers[3][256*192*4];
volatile int currDisplayBuffer=-1;
volatile int newestDisplayBuffer=-2;
pthread_mutex_t display_mutex;

struct HudStruct2
{
public:
	HudStruct2()
	{
		resetTransient();
	}

	void resetTransient()
	{
		fps = 0;
		fps3d = 0;
		cpuload[0] = cpuload[1] = 0;
		cpuloopIterationCount = 0;
	}

	void reset()
	{
	}

	int fps, fps3d, cpuload[2], cpuloopIterationCount;
};

HudStruct2 Hud;

struct MainLoopData
{
	u64 freq;
	int framestoskip;
	int framesskipped;
	int skipnextframe;
	u64 lastticks;
	u64 curticks;
	u64 diffticks;
	u64 fpsticks;
	int fps;
	int fps3d;
	int fpsframecount;
	int toolframecount;
}  mainLoopData = {0};

VideoInfo video;

class Lock {
public:
	Lock(pthread_mutex_t& cs);
	~Lock();
private:
	pthread_mutex_t* m_cs;
};

Lock::Lock(pthread_mutex_t& cs) : m_cs(&cs) { pthread_mutex_lock(m_cs); }
Lock::~Lock() { pthread_mutex_unlock(m_cs); }

#ifdef USE_PROFILER
bool profiler_start = false;
bool profiler_end = false;
#include <prof.h>
#endif

#ifdef MEASURE_FIRST_FRAMES
int mff_totalFrames = 0;
unsigned int mff_totalTime = 0;
bool mff_do = false;
const int mff_toMeasure = 400;
#endif

void doBitmapDraw(u8* pixels, u8* dest, int width, int height, int stride, int pixelFormat, int verticalOffset, bool rotate);

extern "C" {

void logCallback(const Logger& logger, const char* message)
{
	if(message)
		LOGI("%s", message);
}

bool NDS_Pause(bool showMsg = true)
{
	if(paused) return false;

	emu_halt();
	paused = TRUE;
	SPU_Pause(1);
	while (!paused) {}
	if (showMsg) INFO("Emulation paused\n");

	return true;
}

void NDS_UnPause(bool showMsg = true)
{
	if (/*romloaded &&*/ paused)
	{
		paused = FALSE;
		pausedByMinimize = FALSE;
		execute = TRUE;
		SPU_Pause(0);
		if (showMsg) INFO("Emulation unpaused\n");

	}
}

void nds4droid_pause()
{
	if (execute && emu_paused) NDS_UnPause(false), emu_paused=false;
	if (!emu_paused) NDS_Pause();
	emu_paused = 1;
}

void nds4droid_unpause()
{
	if (emu_paused && autoframeskipenab && frameskiprate) AutoFrameSkip_IgnorePreviousDelay();
	if (!execute && !emu_paused) NDS_Pause(false), emu_paused=true;
	if (emu_paused) NDS_UnPause();
	emu_paused = 0;
}

void nds4droid_display()
{
	Lock lock(display_mutex);

	if(int diff = (currDisplayBuffer+1)%3 - newestDisplayBuffer)
		newestDisplayBuffer += diff;
	else newestDisplayBuffer = (currDisplayBuffer+2)%3;

	memcpy(displayBuffers[newestDisplayBuffer],GPU_screen,256*192*4);
}

static void nds4droid_core()
{
#ifdef MEASURE_FIRST_FRAMES
	unsigned int start = GetTickCount();
#endif
	NDS_beginProcessingInput();
	NDS_endProcessingInput();

	NDS_exec<false>();
	SPU_Emulate_user();
#ifdef MEASURE_FIRST_FRAMES
	unsigned int end = GetTickCount();
	if(mff_do)
	{
		mff_totalTime += (end - start);
		if(++mff_totalFrames == mff_toMeasure)
		{
			LOGI("Total time for first %i frames: %i ms", mff_toMeasure, mff_totalTime);
			mff_do = false;
		}
	}
#endif
}

static void nds4droid_user()
{
	const int kFramesPerToolUpdate = 1;

	Hud.fps = mainLoopData.fps;
	Hud.fps3d = mainLoopData.fps3d;

	nds4droid_display();

	gfx3d.frameCtrRaw++;
	if(gfx3d.frameCtrRaw == 60) {
		mainLoopData.fps3d = gfx3d.frameCtr;
		gfx3d.frameCtrRaw = 0;
		gfx3d.frameCtr = 0;
	}

	mainLoopData.toolframecount++;
	if (mainLoopData.toolframecount == kFramesPerToolUpdate)
	{
		mainLoopData.toolframecount = 0;
	}

	//Update_RAM_Search(); // Update_RAM_Watch() is also called.

	mainLoopData.fpsframecount++;
	mainLoopData.curticks = GetTickCount();
	bool oneSecond = mainLoopData.curticks >= mainLoopData.fpsticks + mainLoopData.freq;
	if(oneSecond) // TODO: print fps on screen in DDraw
	{
		mainLoopData.fps = mainLoopData.fpsframecount;
		mainLoopData.fpsframecount = 0;
		mainLoopData.fpsticks = GetTickCount();
	}

	if(nds.idleFrameCounter==0 || oneSecond)
	{
		//calculate a 16 frame arm9 load average
		for(int cpu=0;cpu<2;cpu++)
		{
			int load = 0;
			//printf("%d: ",cpu);
			for(int i=0;i<16;i++)
			{
				//blend together a few frames to keep low-framerate games from having a jittering load average
				//(they will tend to work 100% for a frame and then sleep for a while)
				//4 frames should handle even the slowest of games
				s32 sample =
					nds.runCycleCollector[cpu][(i+0+nds.idleFrameCounter)&15]
				+	nds.runCycleCollector[cpu][(i+1+nds.idleFrameCounter)&15]
				+	nds.runCycleCollector[cpu][(i+2+nds.idleFrameCounter)&15]
				+	nds.runCycleCollector[cpu][(i+3+nds.idleFrameCounter)&15];
				sample /= 4;
				load = load/8 + sample*7/8;
			}
			//printf("\n");
			load = std::min(100,std::max(0,(int)(load*100/1120380)));
			Hud.cpuload[cpu] = load;
		}
	}

	Hud.cpuloopIterationCount = nds.cpuloopIterationCount;
}

static void nds4droid_throttle(bool allowSleep = true, int forceFrameSkip = -1)
{
	int skipRate = (forceFrameSkip < 0) ? frameskiprate : forceFrameSkip;
	int ffSkipRate = (forceFrameSkip < 0) ? 9 : forceFrameSkip;

	if(lastskiprate != skipRate)
	{
		lastskiprate = skipRate;
		mainLoopData.framestoskip = 0; // otherwise switches to lower frameskip rates will lag behind
	}

	if(!mainLoopData.skipnextframe || forceFrameSkip == 0 || frameAdvance || (continuousframeAdvancing && !FastForward))
	{
		mainLoopData.framesskipped = 0;

		if (mainLoopData.framestoskip > 0)
			mainLoopData.skipnextframe = 1;
	}
	else
	{
		mainLoopData.framestoskip--;

		if (mainLoopData.framestoskip < 1)
			mainLoopData.skipnextframe = 0;
		else
			mainLoopData.skipnextframe = 1;

		mainLoopData.framesskipped++;

		NDS_SkipNextFrame();
	}

	if(FastForward)
	{
		if(mainLoopData.framesskipped < ffSkipRate)
		{
			mainLoopData.skipnextframe = 1;
			mainLoopData.framestoskip = 1;
		}
		if (mainLoopData.framestoskip < 1)
			mainLoopData.framestoskip += ffSkipRate;
	}
	else if((/*autoframeskipenab && frameskiprate ||*/ FrameLimit) && allowSleep)
	{
		SpeedThrottle();
	}

	if (autoframeskipenab && frameskiprate)
	{
		if(!frameAdvance && !continuousframeAdvancing)
		{
			AutoFrameSkip_NextFrame();
			if (mainLoopData.framestoskip < 1)
				mainLoopData.framestoskip += AutoFrameSkip_GetSkipAmount(0,skipRate);
		}
	}
	else
	{
		if (mainLoopData.framestoskip < 1)
			mainLoopData.framestoskip += skipRate;
	}

	if (frameAdvance && allowSleep)
	{
		frameAdvance = false;
		emu_halt();
		SPU_Pause(1);
	}
	if(execute && emu_paused && !frameAdvance)
	{
		// safety net against running out of control in case this ever happens.
		nds4droid_unpause(); nds4droid_pause();
	}

	//ServiceDisplayThreadInvocations();
}


bool doRomLoad(const char* path, const char* logical)
{
#ifdef USE_PROFILER
	if(profiler_start && !profiler_end)
	{
		moncleanup();
		profiler_end = true;

		INFO("profile end\n");
	}
	if(!profiler_start && !profiler_end)
	{
		setenv("CPUPROFILE_FREQUENCY", "1000000", 1);
		monstartup("libdesmumeneon.so");
		profiler_start = true;
		INFO("profile start\n");
	}
#endif
	if(NDS_LoadROM(path, logical) >= 0)
	{
		INFO("Loading %s was successful\n",path);
		nds4droid_unpause();
		if (autoframeskipenab && frameskiprate) AutoFrameSkip_IgnorePreviousDelay();
		return true;
	}
	return false;
}

bool nds4droid_loadrom(const char* path)
{
	char LogicalName[1024], PhysicalName[1024];

	const char* s_nonRomExtensions [] = {"txt", "nfo", "htm", "html", "jpg", "jpeg", "png", "bmp", "gif", "mp3", "wav", "lnk", "exe", "bat", "gmv", "gm2", "lua", "luasav", "sav", "srm", "brm", "cfg", "wch", "gs*", "dst"};

	if(!ObtainFile(path, LogicalName, PhysicalName, "rom", s_nonRomExtensions, ARRAY_SIZE(s_nonRomExtensions)))
		return false;

	return doRomLoad(path, PhysicalName);
}

jint JNI(draw, jobject bitmapMain, jobject bitmapTouch, jboolean rotate)
{
	int todo;
	bool alreadyDisplayed;

	{
		Lock lock(display_mutex);

		//find a buffer to display
		todo = newestDisplayBuffer;
		alreadyDisplayed = (todo == currDisplayBuffer);

		//something new to display:
		if(!alreadyDisplayed) {
			//start displaying a new buffer
			currDisplayBuffer = todo;
			video.srcBuffer = (u8*)displayBuffers[currDisplayBuffer];
		}
	}

	if (!alreadyDisplayed)
	{
		//const int size = video.size();
		const int size = 256*384;
		u16* src = (u16*)video.srcBuffer;
		
		if (video.currentfilter == VideoInfo::NONE && rotate == JNI_FALSE)
		{
			//here the magic happens
			void* pixels = NULL;
			//LOGI("width = %i, height = %i", bitmapInfo.width, bitmapInfo.height);
			if(AndroidBitmap_lockPixels(env,bitmapMain,&pixels) >= 0)
			{
				if(bitmapInfo.format == ANDROID_BITMAP_FORMAT_RGBA_8888)
				{
					for (int i = 0; i < 192; i++)
					{
						u32* dest = (u32*)((u8*)pixels+i*bitmapInfo.stride);
						for (int j = 0; j < 256; j++)
						{
							*dest++ = 0xFF000000u | RGB15TO32_NOALPHA(*src++);
						}
					}
				}
				else if(bitmapInfo.format == ANDROID_BITMAP_FORMAT_RGB_565)
				{
					for (int i = 0; i < 192; i++)
					{
						u32* dest = (u32*)((u8*)pixels+i*bitmapInfo.stride);
						for (int j = 0; j < 256/2; j++)
						{
							const u32 c1 = (RGB15TO16_REVERSE(*src++));
							const u32 c2 = (RGB15TO16_REVERSE(*src++));
#ifdef WORDS_BIGENDIAN
							*dest++ =  (c1<<16) | c2;
#else
							*dest++ =  c1 | (c2<<16);
#endif
						}
					}
				}
				
				AndroidBitmap_unlockPixels(env, bitmapMain);
			}
			if(AndroidBitmap_lockPixels(env,bitmapTouch,&pixels) >= 0)
			{
				if(bitmapInfo.format == ANDROID_BITMAP_FORMAT_RGBA_8888)
				{
					for (int i = 0; i < 192; i++)
					{
						u32* dest = (u32*)((u8*)pixels+i*bitmapInfo.stride);
						for (int j = 0; j < 256; j++)
						{
							*dest++ = 0xFF000000u | RGB15TO32_NOALPHA(*src++);
						}
					}
				}
				else if(bitmapInfo.format == ANDROID_BITMAP_FORMAT_RGB_565)
				{
					for (int i = 0; i < 192; i++)
					{
						u32* dest = (u32*)((u8*)pixels+i*bitmapInfo.stride);
						for (int j = 0; j < 256/2; j++)
						{
							const u32 c1 = (RGB15TO16_REVERSE(*src++));
							const u32 c2 = (RGB15TO16_REVERSE(*src++));
#ifdef WORDS_BIGENDIAN
							*dest++ =  (c1<<16) | c2;
#else
							*dest++ =  c1 | (c2<<16);
#endif
						}
					}
				}
			
				AndroidBitmap_unlockPixels(env, bitmapTouch);
			}
		}
		else
		{
			//convert pixel format to 32bpp for compositing
			//why do we do this over and over? well, we are compositing to
			//filteredbuffer32bpp, and it needs to get refreshed each frame..
			if(bitmapInfo.format == ANDROID_BITMAP_FORMAT_RGBA_8888)
			{
				u32* dest = video.buffer;
				for(int i=0;i<size;++i)
					*dest++ = 0xFF000000u | RGB15TO32_NOALPHA(*src++);

				video.filter();
			}
			else if(bitmapInfo.format == ANDROID_BITMAP_FORMAT_RGB_565)
			{
				u16* dest = (u16*)video.buffer;
				for(int i=0;i<size;++i)
					*dest++ = RGB15TO16_REVERSE(*src++);
			}

			//here the magic happens
			void* pixels = NULL;
			//LOGI("width = %i, height = %i", bitmapInfo.width, bitmapInfo.height);
			if(AndroidBitmap_lockPixels(env,bitmapMain,&pixels) >= 0)
			{
				doBitmapDraw((u8*)video.finalBuffer(), (u8*)pixels, bitmapInfo.width, bitmapInfo.height, bitmapInfo.stride, bitmapInfo.format, 0, rotate == JNI_TRUE);
				
				AndroidBitmap_unlockPixels(env, bitmapMain);
			}
			if(AndroidBitmap_lockPixels(env,bitmapTouch,&pixels) >= 0)
			{
				doBitmapDraw((u8*)video.finalBuffer(), (u8*)pixels, bitmapInfo.width, bitmapInfo.height, bitmapInfo.stride, bitmapInfo.format, video.height / 2, rotate == JNI_TRUE);
				
				AndroidBitmap_unlockPixels(env, bitmapTouch);
			}
		}
	}

	return ((Hud.fps & 0xFF)<<24)|((Hud.fps3d & 0xFF)<<16)|((Hud.cpuload[0] & 0xFF)<<8)|((Hud.cpuload[1] & 0xFF));
}

void JNI(resize, jobject bitmap)
{
	AndroidBitmap_getInfo(env, bitmap, &bitmapInfo);

	if(bitmapInfo.format == ANDROID_BITMAP_FORMAT_RGBA_8888)
		LOGI("bitmapInfo.format == ANDROID_BITMAP_FORMAT_RGBA_8888");
	else if(bitmapInfo.format == ANDROID_BITMAP_FORMAT_RGB_565)
		LOGI("bitmapInfo.format == ANDROID_BITMAP_FORMAT_RGB_565");
}

int JNI_NOARGS(getNativeWidth)
{
	return video.width;
}

int JNI_NOARGS(getNativeHeight)
{
	return video.height;
}

void JNI(setFilter, int index)
{
	video.setfilter(index);
}


void JNI_NOARGS(runCore)
{
//	int frameStart = GetTickCount();
	if(execute)
	{
		nds4droid_core();
		nds4droid_user();
		nds4droid_throttle();
	}
//	int frameEnd = GetTickCount();
//	if(frameCount ++ % 5 == 0)
//	{
//		LOGI("Core frame time %d ms, %d", frameEnd - frameStart, nds.cpuloopIterationCount);
//	}
}

void JNI(setSoundPaused, int set)
{
	if(sndcoretype != 1)
		return;
	SNDOpenSLPaused(set == 0 ? false : true);
}

void JNI(saveState, int slot)
{
	savestate_slot(slot);
}

void JNI(restoreState, int slot)
{
	loadstate_slot(slot);
#ifdef MEASURE_FIRST_FRAMES
	mff_do = true;
	mff_totalFrames = 0;
	mff_totalTime = 0;
#endif
}

void loadSettings(JNIEnv* env)
{
	CommonSettings.num_cores = sysconf(_SC_NPROCESSORS_CONF);
	LOGI("%i cores detected", CommonSettings.num_cores);

	CommonSettings.cheatsDisable = GetPrivateProfileBool(env,"General", "cheatsDisable", false, IniName);
	CommonSettings.autodetectBackupMethod = GetPrivateProfileInt(env,"General", "autoDetectMethod", 0, IniName);
	enableMicrophone = GetPrivateProfileBool(env, "General", "EnableMicrophone", true, IniName);

	CommonSettings.ROM_UseFileMap = GetPrivateProfileBool(env, "Rom", "UseFileMap", false, IniName);

	video.reset();
	video.rotation =  GetPrivateProfileInt(env,"Video","WindowRotate", 0, IniName);
	video.rotation_userset =  GetPrivateProfileInt(env,"Video","WindowRotateSet", video.rotation, IniName);
	video.layout_old = video.layout = GetPrivateProfileInt(env,"Video", "LCDsLayout", 0, IniName);
	if (video.layout > 2)
	{
		video.layout = video.layout_old = 0;
	}
	video.swap = GetPrivateProfileInt(env,"Video", "LCDsSwap", 0, IniName);

	CommonSettings.hud.FpsDisplay = GetPrivateProfileBool(env,"Display","DisplayFps", false, IniName);
	CommonSettings.hud.FrameCounterDisplay = GetPrivateProfileBool(env,"Display","FrameCounter", false, IniName);
	CommonSettings.hud.ShowInputDisplay = GetPrivateProfileBool(env,"Display","DisplayInput", false, IniName);
	CommonSettings.hud.ShowGraphicalInputDisplay = GetPrivateProfileBool(env,"Display","DisplayGraphicalInput", false, IniName);
	CommonSettings.hud.ShowLagFrameCounter = GetPrivateProfileBool(env,"Display","DisplayLagCounter", false, IniName);
	CommonSettings.hud.ShowMicrophone = GetPrivateProfileBool(env,"Display","DisplayMicrophone", false, IniName);
	CommonSettings.hud.ShowRTC = GetPrivateProfileBool(env,"Display","DisplayRTC", false, IniName);
	video.screengap = GetPrivateProfileInt(env,"Display", "ScreenGap", 0, IniName);
	CommonSettings.showGpu.main = GetPrivateProfileInt(env,"Display", "MainGpu", 1, IniName) != 0;
	CommonSettings.showGpu.sub = GetPrivateProfileInt(env,"Display", "SubGpu", 1, IniName) != 0;

	autoframeskipenab = 1;
	frameskiprate = GetPrivateProfileInt(env,"Display", "FrameSkip", 1, IniName);

	CommonSettings.micMode = (TCommonSettings::MicMode)GetPrivateProfileInt(env,"MicSettings", "MicMode", (int)TCommonSettings::InternalNoise, IniName);

	CommonSettings.spu_advanced = GetPrivateProfileBool(env,"Sound", "SpuAdvanced", false, IniName);
	CommonSettings.spuInterpolationMode = (SPUInterpolationMode)GetPrivateProfileInt(env, "Sound","SPUInterpolation", 1, IniName);
	snd_synchmode = GetPrivateProfileInt(env, "Sound","SynchMode",0,IniName);
	snd_synchmethod = GetPrivateProfileInt(env, "Sound","SynchMethod",0,IniName);

	CommonSettings.advanced_timing = GetPrivateProfileBool(env,"Emulation", "AdvancedTiming", false, IniName);
	CommonSettings.CpuMode = GetPrivateProfileInt(env, "Emulation","CpuMode", 0, IniName);

	CommonSettings.GFX3D_Zelda_Shadow_Depth_Hack = GetPrivateProfileInt(env,"3D", "ZeldaShadowDepthHack", 0, IniName);
	CommonSettings.GFX3D_HighResolutionInterpolateColor = GetPrivateProfileBool(env, "3D", "HighResolutionInterpolateColor", 0, IniName);
	CommonSettings.GFX3D_EdgeMark = GetPrivateProfileBool(env, "3D", "EnableEdgeMark", 0, IniName);
	CommonSettings.GFX3D_Fog = GetPrivateProfileBool(env, "3D", "EnableFog", 1, IniName);
	CommonSettings.GFX3D_Texture = GetPrivateProfileBool(env, "3D", "EnableTexture", 1, IniName);
	CommonSettings.GFX3D_LineHack = GetPrivateProfileBool(env, "3D", "EnableLineHack", 0, IniName);

	fw_config.language = GetPrivateProfileInt(env, "Firmware","Language", 1, IniName);

	CommonSettings.wifi.mode = GetPrivateProfileInt(env,"Wifi", "Mode", 0, IniName);
	CommonSettings.wifi.infraBridgeAdapter = GetPrivateProfileInt(env,"Wifi", "BridgeAdapter", 0, IniName);
}

void JNI_NOARGS(reloadFirmware)
{
	NDS_CreateDummyFirmware(&fw_config);
}

void JNI_NOARGS(loadSettings)
{
	loadSettings(env);
}


void JNI(init, jobject _inst)
{
#ifdef HAVE_NEON
	//neontest();
	enable_runfast();
#endif
	INFO("");

	Logger::setCallbackAll(logCallback);

	oglrender_init = android_opengl_init;
	InitDecoder();

	path.ReadPathSettings();
	if (video.layout > 2)
	{
		video.layout = video.layout_old = 0;
	}

	loadSettings(env);

	Desmume_InitOnce();
	//gpu_SetRotateScreen(video.rotation);
	NDS_FillDefaultFirmwareConfigData(&fw_config);
	Hud.reset();

	INFO("Init NDS");

	int slot1_device_type = NDS_SLOT1_RETAIL;
	switch (slot1_device_type)
	{
		case NDS_SLOT1_NONE:
		case NDS_SLOT1_RETAIL:
		case NDS_SLOT1_R4:
		case NDS_SLOT1_RETAIL_NAND:
			break;
		default:
			slot1_device_type = NDS_SLOT1_RETAIL;
			break;
	}

	switch (addon_type)
	{
	case NDS_ADDON_NONE:
		break;
	case NDS_ADDON_CFLASH:
		break;
	case NDS_ADDON_RUMBLEPAK:
		break;
	case NDS_ADDON_GBAGAME:
		if (!strlen(GBAgameName))
		{
			addon_type = NDS_ADDON_NONE;
			break;
		}
		// TODO: check for file exist
		break;
	case NDS_ADDON_GUITARGRIP:
		break;
	case NDS_ADDON_EXPMEMORY:
		break;
	case NDS_ADDON_PIANO:
		break;
	case NDS_ADDON_PADDLE:
		break;
	default:
		addon_type = NDS_ADDON_NONE;
		break;
	}

	slot1Change((NDS_SLOT1_TYPE)slot1_device_type);
	addonsChangePak(addon_type);


	NDS_Init();

	cur3DCore = GetPrivateProfileInt(env, "3D", "Renderer", 1, IniName);
	NDS_3D_ChangeCore(cur3DCore); //OpenGL

	LOG("Init sound core\n");
	sndcoretype = GetPrivateProfileInt(env, "Sound","SoundCore2", SNDCORE_OPENSL, IniName);
	sndbuffersize = GetPrivateProfileInt(env, "Sound","SoundBufferSize2", DESMUME_SAMPLE_RATE*8/60, IniName);
	SPU_ChangeSoundCore(sndcoretype, sndbuffersize);
	SPU_SetSynchMode(snd_synchmode,snd_synchmethod);

	static const char* nickname = "emozilla";
	fw_config.nickname_len = strlen(nickname);
	for(int i = 0 ; i < fw_config.nickname_len ; ++i)
		fw_config.nickname[i] = nickname[i];

	static const char* message = "desmume makes you happy!";
	fw_config.message_len = strlen(message);
	for(int i = 0 ; i < fw_config.message_len ; ++i)
		fw_config.message[i] = message[i];

	fw_config.language = GetPrivateProfileInt(env, "Firmware","Language", 1, IniName);

	video.setfilter(GetPrivateProfileInt(env,"Video", "Filter", video.NONE, IniName));

	NDS_CreateDummyFirmware(&fw_config);

	InitSpeedThrottle();

	mainLoopData.freq = 1000;
	mainLoopData.lastticks = GetTickCount();

	pthread_mutex_init(&display_mutex, NULL);
}

void JNI(changeCpuMode, int type)
{
	armcpu_setjitmode(type);
}

void JNI(change3D, int type)
{
	NDS_3D_ChangeCore(cur3DCore = type);
}

void JNI(changeSound, int type)
{
	SPU_ChangeSoundCore(sndcoretype = type, sndbuffersize);
}

void JNI(changeSoundSynchMode, int synchmode)
{
	SPU_SetSynchMode(snd_synchmode = synchmode,snd_synchmethod);
}

void JNI(changeSoundSynchMethod, int synchmethod)
{
	SPU_SetSynchMode(snd_synchmode,snd_synchmethod = synchmethod);
}

jboolean JNI(loadRom, jstring path)
{
	jboolean isCopy;
	const char* szPath = env->GetStringUTFChars(path, &isCopy);
	bool ret = nds4droid_loadrom(szPath);
	env->ReleaseStringUTFChars(path, szPath);
	return ret ? JNI_TRUE : JNI_FALSE;
}

void JNI(setWorkingDir, jstring path, jstring temp)
{
	jboolean isCopy;
	const char* szPath = env->GetStringUTFChars(path, &isCopy);
	PathInfo::SetWorkingDir(szPath);
	env->ReleaseStringUTFChars(path, szPath);

	szPath = env->GetStringUTFChars(temp, &isCopy);
	strncpy(androidTempPath, szPath, 1024);
	env->ReleaseStringUTFChars(temp, szPath);
}

void JNI(touchScreenTouch, int x, int y)
{
	if(x<0) x = 0; else if(x>255) x = 255;
	if(y<0) y = 0; else if(y>192) y = 192;
	NDS_setTouchPos(x,y);
}

void JNI_NOARGS(touchScreenRelease)
{
	NDS_releaseTouch();
}

void JNI(setButtons, int l, int r, int up, int down, int left, int right, int a, int b, int x, int y, int start, int select, int lid)
{
	NDS_setPad(right, left, down, up, select, start, b, a, y, x, l, r, false, !lid);
}

jint JNI_NOARGS(getNumberOfCheats)
{
	return cheats == NULL ? 0 : cheats->getSize();
}

jstring JNI(getCheatName, int pos)
{
	if(cheats == NULL || pos < 0 || pos >= cheats->getSize())
		return 0;
	return env->NewStringUTF(cheats->getItemByIndex(pos)->description);
}

jboolean JNI(getCheatEnabled, int pos)
{
	if(cheats == NULL || pos < 0 || pos >= cheats->getSize())
		return 0;
	return cheats->getItemByIndex(pos)->enabled ? JNI_TRUE : JNI_FALSE;
}

jstring JNI(getCheatCode, int pos)
{
	if(cheats == NULL || pos < 0 || pos >= cheats->getSize())
		return 0;
	char buffer[1024] = {0};
	cheats->getXXcodeString(*cheats->getItemByIndex(pos), buffer);
	jstring ret = env->NewStringUTF(buffer);
	return ret;
}

jint JNI(getCheatType, int pos)
{
	if(cheats == NULL || pos < 0 || pos >= cheats->getSize())
		return 0;
	return cheats->getItemByIndex(pos)->type;
}

void JNI(addCheat, jstring description, jstring code)
{
	if(cheats == NULL)
		return;
	jboolean isCopy;
	const char* descBuff = env->GetStringUTFChars(description, &isCopy);
	const char* codeBuff = env->GetStringUTFChars(code, &isCopy);
	cheats->add_AR(codeBuff, descBuff, TRUE);
	env->ReleaseStringUTFChars(description, descBuff);
	env->ReleaseStringUTFChars(code, codeBuff);
}

void JNI(updateCheat, jstring description, jstring code, jint pos)
{
	if(cheats == NULL)
		return;
	jboolean isCopy;
	const char* descBuff = env->GetStringUTFChars(description, &isCopy);
	const char* codeBuff = env->GetStringUTFChars(code, &isCopy);
	cheats->update_AR(codeBuff, descBuff, TRUE, pos);
	env->ReleaseStringUTFChars(description, descBuff);
	env->ReleaseStringUTFChars(code, codeBuff);
}

void JNI_NOARGS(saveCheats)
{
	if(cheats)
		cheats->save();
}

void JNI(setCheatEnabled, int pos, jboolean enabled)
{
	if(cheats)
		cheats->getItemByIndex(pos)->enabled = enabled == JNI_TRUE ? true : false;
}

void JNI(deleteCheat, jint pos)
{
	if(cheats)
		cheats->remove(pos);
}

void JNI_NOARGS(closeRom)
{
	NDS_FreeROM();
	execute = false;
	Hud.resetTransient();
	NDS_Reset();
}

void JNI_NOARGS(exit)
{
}

} //end extern "C"

unsigned int GetPrivateProfileInt(JNIEnv* env, const char* lpAppName, const char* lpKeyName, int nDefault, const char* lpFileName)
{
	jclass javaClass = env->FindClass("com/opendoorstudios/desmume/DeSmuME");
	if(!javaClass)
		return nDefault;
	jmethodID getSettingInt = env->GetStaticMethodID(javaClass, "getSettingInt","(Ljava/lang/String;I)I");
	jstring key = env->NewStringUTF(lpKeyName);
	int ret = env->CallStaticIntMethod(javaClass, getSettingInt, key, nDefault);
	return ret;
}

bool GetPrivateProfileBool(JNIEnv* env, const char* lpAppName, const char* lpKeyName, bool bDefault, const char* lpFileName)
{
	return GetPrivateProfileInt(env, lpAppName, lpKeyName, bDefault ? 1 : 0, lpFileName);
}
