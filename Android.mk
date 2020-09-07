LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= src/abstractcat.cpp
LOCAL_MODULE:= abstractcat
include $(BUILD_EXECUTABLE)
