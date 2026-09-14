cmake_minimum_required(VERSION 3.10)

project(TLServer)


##########################################################
# Toolchain
##########################################################

# 平台：RK3588（正点 6.1 版工具链，sysroot 自带 MPP/RGA/rknn 全套库）
set(TOOLCHAIN_DIR
    /home/yzy/atk-dlrk3588-toolchain
)


set(CMAKE_SYSROOT
    ${TOOLCHAIN_DIR}/aarch64-buildroot-linux-gnu/sysroot
)


set(SYSROOT
    ${CMAKE_SYSROOT}/usr/include
)


set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# kaldi-native-fbank 预编译静态库未开 -fPIC，而 buildroot 工具链默认 PIE 链接会报
# R_AARCH64_ADR_PREL_PG_HI21 重定位错误；板端常驻进程不需要 PIE，关闭即可
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -no-pie")



##########################################################
# Compile option
##########################################################

add_definitions(
    -g
    -O0
    -ggdb
    -gdwarf
    -funwind-tables
    -rdynamic
    -Wno-write-strings
    -Wno-return-type
)



##########################################################
# Path
##########################################################

set(SERVER_DIR
    ${CMAKE_CURRENT_SOURCE_DIR}
)


set(CPP_DIR
    ${SERVER_DIR}/../cpp
)


set(MODEL_DIR
    ${SERVER_DIR}/../model
)



##########################################################
# MPP
##########################################################

# 正点工具链 sysroot 自带 MPP/RGA/rknn 全套库,直接使用
set(MPP_INCLUDE_DIR
    ${SYSROOT}/usr/include
)
set(MPP_INCLUDE_DIR_ROCKCHIP
    ${SYSROOT}/usr/include/rockchip
)
set(MPP_LIB_DIR
    ${SYSROOT}/usr/lib
)

##########################################################
# 板端 VLM（RKLLM 1.3.0）
##########################################################
# librkllmrt.so NEEDED libgomp.so.1(板端部署 lib/ 自带),本机 buildroot
# 工具链无 libgomp → 放宽动态库未解析符号(与官方 demo CMake 同策略)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--allow-shlib-undefined")
set(RKLLM_DIR
    ${CPP_DIR}/3rdparty/rkllm
)
set(LIBRKLLMRT
    ${RKLLM_DIR}/librkllmrt.so
)
set(LIBGOMP
    ${RKLLM_DIR}/libgomp.so.1
)



##########################################################
# live555
##########################################################

set(LIVE555_INCLUDE_DIR
/home/yzy/toolchain/live/install_aarch64/include
)


set(LIVE555_LIB_DIR
/home/yzy/toolchain/live/install_aarch64/lib
)



##########################################################
# Paho MQTT C
##########################################################

set(PAHO_DIR
/home/yzy/vscode/paho.mqtt.c-1.3.8/install_new
)


set(PAHO_INCLUDE_DIR
${PAHO_DIR}/include
)


set(PAHO_LIB_DIR
${PAHO_DIR}/lib
)



##########################################################
# fdk-aac（AAC 编码器，静态链接）
##########################################################

set(FDKAAC_DIR
/home/yzy/toolchain/fdk-aac/install_aarch64
)


set(FDKAAC_INCLUDE_DIR
${FDKAAC_DIR}/include
)


set(FDKAAC_LIB_DIR
${FDKAAC_DIR}/lib
)



##########################################################
# RGA
##########################################################

set(RGA_INCLUDE_DIR
    ${SYSROOT}/rga
)



##########################################################
# Third party
##########################################################

add_subdirectory(
    ${CPP_DIR}/3rdparty
    ${CMAKE_BINARY_DIR}/3rdparty.out
)



##########################################################
# utils
##########################################################

add_subdirectory(
    ${CPP_DIR}/utils
    ${CMAKE_BINARY_DIR}/utils.out
)



##########################################################
# include
##########################################################

include_directories(


    ${SERVER_DIR}


    ${CPP_DIR}


    ${CPP_DIR}/rknpu2


    ${CPP_DIR}/utils

    ${CPP_DIR}/3rdparty/kaldi_native_fbank/include

    ${CPP_DIR}/3rdparty/rkllm/include

    ${CPP_DIR}/3rdparty/cJSON


    ${LIBRKNNRT_INCLUDES}


    ${LIBJPEG_INCLUDES}


    ${LIBRGA_INCLUDES}


    ${RGA_INCLUDE_DIR}


    ${SYSROOT}


    ${SYSROOT}/opencv4


    ${MPP_INCLUDE_DIR}


    ${MPP_INCLUDE_DIR_ROCKCHIP}


    ${LIVE555_INCLUDE_DIR}


    ${PAHO_INCLUDE_DIR}


    ${FDKAAC_INCLUDE_DIR}


    ${SERVER_DIR}/3rdparty/minimp4

)



##########################################################
# library path
##########################################################

link_directories(


    ${MPP_LIB_DIR}


    ${LIVE555_LIB_DIR}


    ${PAHO_LIB_DIR}


    ${FDKAAC_LIB_DIR}


    ${CPP_DIR}/3rdparty/kaldi_native_fbank/Linux/aarch64

)





##########################################################
# OpenCV
##########################################################

set(OPENCV_LIBS


    opencv_core

    opencv_flann

    opencv_videoio

    opencv_video

    opencv_highgui

    opencv_imgcodecs

    opencv_imgproc

)





##########################################################
# Source
##########################################################

set(SOURCES


    ##################################################
    # RTSP server
    ##################################################

    TLmain.cpp

    thread_util.cpp


    # const.cpp


    EncodeThread.cpp


    SharedQueue.cpp


    LiveSource.cpp


    H265LiveServerMediaSubsession.cpp


    H264LiveServerMediaSubsession.cpp


    v4l2.cpp


    mpp.cpp

    config.cpp

    ##################################################
    # YOLOv8 pose
    ##################################################

    ${CPP_DIR}/postprocess.cc


    ${CPP_DIR}/rknpu2/yolov8-pose.cc



    ##################################################
    # pose classifier + person tracker
    ##################################################

    ${CPP_DIR}/pose_classifier.cpp

    ${CPP_DIR}/person_tracker.cpp



    ##################################################
    # MQTT
    ##################################################

    ${CPP_DIR}/mqtt_client.cpp
    ${CPP_DIR}/mqtt_thread.cpp
    ${CPP_DIR}/EventManager.cpp
    ${CPP_DIR}/Logger.cpp

    ##################################################
    # Video recorder
    ##################################################
    VideoRecorder.cpp

    ##################################################
    # Audio capture (ALSA) + AAC encoder + MP4 muxer
    ##################################################
    AudioCapture.cpp

    AacEncoder.cpp

    ${CPP_DIR}/asr_thread.cpp

    ${CPP_DIR}/https_client.cpp

    ${CPP_DIR}/chat_engine.cpp

    ${CPP_DIR}/frame_provider.cpp

    ${CPP_DIR}/vlm_engine.cpp

    ${CPP_DIR}/tts_backend_cloud.cpp

    ${CPP_DIR}/tts_backend_piper.cpp

    ${CPP_DIR}/3rdparty/cJSON/cJSON.c

    AudioPlayback.cpp

    Mp4Muxer.cpp

    Mp4Muxer.cpp

    minimp4_impl.cpp

    ##################################################
    # RTSP audio (AAC)
    ##################################################
    AACLiveSource.cpp

    AACLiveServerMediaSubsession.cpp
)





##########################################################
# executable
##########################################################

add_executable(

    TLServer

    ${SOURCES}

)





##########################################################
# pthread
##########################################################

find_package(Threads REQUIRED)





##########################################################
# Link
##########################################################

target_link_libraries(

TLServer



    ##################################################
    # utils
    ##################################################

    imagedrawing

    imageutils

    fileutils



    ##################################################
    # RKNN
    ##################################################

    ${LIBRKNNRT}



    ##################################################
    # RGA
    ##################################################

    rga



    ##################################################
    # OpenCV
    ##################################################

    ${OPENCV_LIBS}



    ##################################################
    # MQTT
    ##################################################

    paho-mqtt3c



    ##################################################
    # live555
    ##################################################

    liveMedia

    groupsock

    BasicUsageEnvironment

    UsageEnvironment



    ##################################################
    # MPP
    ##################################################

    rockchip_mpp



    ##################################################
    # system
    ##################################################

    Threads::Threads

    pthread

    dl

    rt

    ssl

    crypto

    sqlite3

    asound

    fdk-aac

    kaldi-native-fbank-core

)

# 板端 VLM（RKLLM 运行时）
target_link_libraries(
TLServer

${LIBRKLLMRT}
)

##########################################################
# 本地 Piper TTS（全离线；-DPIPER_TTS=ON 启用）
##########################################################

option(PIPER_TTS "本地 Piper TTS（RK3588 NPU decoder，全离线）" OFF)

if(PIPER_TTS)
    add_definitions(-DPIPER_TTS)
    add_subdirectory(
        ${CPP_DIR}/3rdparty/paroli
        ${CMAKE_BINARY_DIR}/paroli.out
    )
    # piper_core 内链接 phonemize 包自带的 onnxruntime/espeak-ng/piper_phonemize（动态库，
    # 板端部署 lib/；buildroot 工具链缺 libgomp 已全局 --allow-shlib-undefined 兜底）
    target_link_libraries(
    TLServer

    piper_core
    )
    # 板端部署：phonemize 包自带的三个动态库 + espeak-ng-data（模型目录旁）
    set(PIPER_PHONEMIZE_ROOT_LOCAL ${CPP_DIR}/3rdparty/piper-phonemize)
    install(
    PROGRAMS
    ${PIPER_PHONEMIZE_ROOT_LOCAL}/lib/libonnxruntime.so.1.14.1
    ${PIPER_PHONEMIZE_ROOT_LOCAL}/lib/libespeak-ng.so.1
    ${PIPER_PHONEMIZE_ROOT_LOCAL}/lib/libpiper_phonemize.so.1
    DESTINATION lib
    )
    install(
    DIRECTORY ${PIPER_PHONEMIZE_ROOT_LOCAL}/share/espeak-ng-data
    DESTINATION model/piper
    )
endif()





##########################################################
# install
##########################################################

set(
CMAKE_INSTALL_RPATH
"$ORIGIN/lib"
)



install(

TARGETS TLServer

DESTINATION .

)



install(

FILES

${LIBRKNNRT}

DESTINATION lib

)

# RKLLM 运行时 + libgomp（librkllmrt 板端依赖；固件若无 libgomp 由 lib/ 兜底）
install(
PROGRAMS
${LIBRKLLMRT}
${LIBGOMP}
DESTINATION lib
)



# 模型在子目录（重转产物），另带 ASR 词表
file(GLOB RKNN_FILES
"${MODEL_DIR}/rk3588/*.rknn"
)
install(FILES
${MODEL_DIR}/rk3588/vocab.txt
DESTINATION model
)



install(

FILES

${RKNN_FILES}

DESTINATION model

)