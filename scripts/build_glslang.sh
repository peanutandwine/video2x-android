#!/usr/bin/env bash
# glslang：GLSL → SPIR-V 编译器。
# libplacebo（编译 Anime4K 着色器）与 ncnn（在线编译 Vulkan shader）都依赖它。
source "$(dirname "$0")/config.sh"
check_env
cd "$WORK_DIR"
[ -d glslang ] || die "未找到 glslang，请先运行 fetch_sources.sh"

log "配置 glslang ..."
cmake -S glslang -B "$BUILD_ROOT/glslang" \
  -DCMAKE_TOOLCHAIN_FILE="$CMAKE_TOOLCHAIN_FILE" \
  -DANDROID_ABI="$ABI" -DANDROID_PLATFORM="android-$API_LEVEL" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DBUILD_SHARED_LIBS=OFF \
  -DENABLE_GLSLANG_BINARIES=OFF \
  -DGLSLANG_TESTS=OFF \
  -DENABLE_OPT=OFF \
  -DALLOW_EXTERNAL_SPIRV_TOOLS=OFF

log "编译 glslang ..."
cmake --build "$BUILD_ROOT/glslang" -j"$JOBS"
cmake --install "$BUILD_ROOT/glslang"
log "glslang 安装到 $PREFIX"
