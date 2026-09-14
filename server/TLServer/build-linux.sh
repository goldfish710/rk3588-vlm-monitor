#!/bin/bash

set -e

# 用法: ./build-linux.sh [-p]  (-p 启用本地 Piper TTS)
PIPER_TTS=OFF
while getopts "p" opt; do
    case $opt in
        p) PIPER_TTS=ON ;;
    esac
done

TARGET_SOC=rk3588
TARGET_ARCH=aarch64
BUILD_DEMO_NAME=TLServer

##########################################################
# Toolchain(正点 RK3588 6.1 版工具链)
##########################################################

export GCC_COMPILER=/home/yzy/atk-dlrk3588-toolchain/bin/aarch64-buildroot-linux-gnu

echo "Toolchain:"
echo ${GCC_COMPILER}

export CC=${GCC_COMPILER}-gcc
export CXX=${GCC_COMPILER}-g++

BUILD_TYPE=Release

##########################################################
# Path
##########################################################

ROOT_PWD=$( cd "$( dirname $0 )" && pwd )

BUILD_DEMO_PATH=./

TARGET_SDK=atk_rtsp_ai_server

TARGET_PLATFORM=${TARGET_SOC}_linux
TARGET_PLATFORM=${TARGET_PLATFORM}_${TARGET_ARCH}

INSTALL_DIR=${ROOT_PWD}/install/${TARGET_PLATFORM}/${TARGET_SDK}

BUILD_DIR=${ROOT_PWD}/build/build_${TARGET_SDK}_${TARGET_PLATFORM}_${BUILD_TYPE}

##########################################################
# Print
##########################################################

echo "==========================================="
echo "BUILD_DEMO_NAME=${BUILD_DEMO_NAME}"
echo "TARGET_SOC=${TARGET_SOC}"
echo "TARGET_ARCH=${TARGET_ARCH}"
echo "BUILD_TYPE=${BUILD_TYPE}"
echo "INSTALL_DIR=${INSTALL_DIR}"
echo "BUILD_DIR=${BUILD_DIR}"
echo "CC=${CC}"
echo "CXX=${CXX}"
echo "==========================================="

##########################################################
# Build Dir
##########################################################

if [[ ! -d "${BUILD_DIR}" ]]; then
    mkdir -p ${BUILD_DIR}
fi

##########################################################
# Install Dir
##########################################################

if [[ -d "${INSTALL_DIR}" ]]; then
    rm -rf ${INSTALL_DIR}
fi

##########################################################
# CMake
##########################################################

cd ${BUILD_DIR}

cmake ../../${BUILD_DEMO_PATH} \
    -DPIPER_TTS=${PIPER_TTS} \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=${TARGET_ARCH} \
    -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
    -DCMAKE_INSTALL_PREFIX=${INSTALL_DIR}

##########################################################
# Make
##########################################################

make -j$(nproc)

make install

##########################################################
# Check
##########################################################

echo ""
echo "==========================================="
echo "Build Success!"
echo "==========================================="

echo "Executable:"
ls -lh ${INSTALL_DIR}/TLServer

echo ""

echo "Model:"
ls -lh ${INSTALL_DIR}/model

echo ""

echo "Library:"
ls -lh ${INSTALL_DIR}/lib

echo ""

echo "Install Directory:"
echo ${INSTALL_DIR}

echo "==========================================="