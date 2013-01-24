# Android ndk makefile for desmume

LOCAL_PATH := $(call my-dir)
LOCAL_BUILD_PATH := $(call my-dir)

include $(CLEAR_VARS)

include $(LOCAL_BUILD_PATH)/cpudetect/cpudetect.mk

ifeq ($(TARGET_ARCH_ABI),armeabi-v7a)
#include $(LOCAL_BUILD_PATH)/desmume_v7.mk
include $(LOCAL_BUILD_PATH)/desmume_neon.mk
else
include $(LOCAL_BUILD_PATH)/desmume_compat.mk
endif
