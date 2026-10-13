#!/usr/bin/env bash
# ncnn：Real-ESRGAN / Real-CUGAN / RIFE 的推理后端（Vulkan 加速）。
# 以静态库构建，最终被静态链接进 libvideo2x.so，减少 APK 内 .so 数量。
source "$(dirname "$0")/config.sh"
check_env
cd "$WORK_DIR"
[ -d ncnn ] || die "未找到 ncnn，请先运行 fetch_sources.sh"

log "配置 ncnn ..."
cmake -S ncnn -B "$BUILD_ROOT/ncnn" \
  -DCMAKE_TOOLCHAIN_FILE="$CMAKE_TOOLCHAIN_FILE" \
  -DANDROID_ABI="$ABI" -DANDROID_PLATFORM="android-$API_LEVEL" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DNCNN_VULKAN=ON \
  -DNCNN_VULKAN_ONLINE_SPIRV=ON \
  -DNCNN_SYSTEM_GLSLANG=ON \
  -Dglslang_DIR="$PREFIX/lib/cmake/glslang" \
  -DNCNN_BUILD_TOOLS=OFF \
  -DNCNN_BUILD_EXAMPLES=OFF \
  -DNCNN_BUILD_TESTS=OFF \
  -DNCNN_BUILD_BENCHMARK=OFF \
  -DNCNN_BUILD_SHARED_LIBS=OFF \
  -DNCNN_DISABLE_RTTI=ON \
  -DNCNN_DISABLE_EXCEPTION=ON \
  -DNCNN_INT8=OFF

log "编译 ncnn ..."
cmake --build "$BUILD_ROOT/ncnn" -j"$JOBS"
cmake --install "$BUILD_ROOT/ncnn"
log "ncnn 安装到 $PREFIX"
