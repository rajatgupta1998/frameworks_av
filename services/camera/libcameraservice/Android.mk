LOCAL_PATH:= $(call my-dir)

#
# libcameraservice
#

include $(CLEAR_VARS)

LOCAL_SRC_FILES:=               \
    CameraService.cpp \
    CameraClient.cpp \
    Camera2Client.cpp \
    ProCamera2Client.cpp \
    CameraDeviceBase.cpp \
    Camera2Device.cpp \
    Camera3Device.cpp \
    camera2/Parameters.cpp \
    camera2/FrameProcessor.cpp \
    camera2/StreamingProcessor.cpp \
    camera2/JpegProcessor.cpp \
    camera2/CallbackProcessor.cpp \
    camera2/ZslProcessor.cpp \
    camera2/BurstCapture.cpp \
    camera2/JpegCompressor.cpp \
    camera2/CaptureSequencer.cpp \
    camera2/ProFrameProcessor.cpp \

LOCAL_SHARED_LIBRARIES:= \
    libui \
    libutils \
    libbinder \
    libcutils \
    libmedia \
    libcamera_client \
    libgui \
    libhardware \
    libsync \
    libcamera_metadata \
    libjpeg

LOCAL_C_INCLUDES += \
    system/media/camera/include \
    external/jpeg


LOCAL_CFLAGS += -Wall -Wextra

LOCAL_MODULE:= libcameraservice

include $(BUILD_SHARED_LIBRARY)
