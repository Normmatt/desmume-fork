# Android ndk makefile for desmume

NDK_TOOLCHAIN_VERSION=4.7
APP_STL := gnustl_static
#APP_ABI := x86
APP_ABI := armeabi armeabi-v7a
#APP_ABI := armeabi-v7a
APP_PLATFORM := android-9
# For releases
APP_CFLAGS := -Ofast -ftree-vectorize -fsingle-precision-constant -fprefetch-loop-arrays -fvariable-expansion-in-unroller -ffast-math -funroll-loops -fomit-frame-pointer -fno-math-errno -funsafe-math-optimizations -ffinite-math-only -fdata-sections -fbranch-target-load-optimize2 -fno-stack-protector -flto -fforce-addr -funswitch-loops -ftree-loop-im -ftree-loop-ivcanon -fivopts -Wno-psabi #-fno-exceptions
#APP_CFLAGS := -Ofast -fsingle-precision-constant -funroll-loops -fsched-pressure -ffunction-sections -fdata-sections -fbranch-target-load-optimize -fno-stack-protector -Wno-psabi
#APP_CFLAGS := -O3 -funroll-loops -fno-math-errno -fdata-sections -fno-stack-protector -Wno-psabi
# For debugging
#APP_CFLAGS := -O0 -Wno-psabi