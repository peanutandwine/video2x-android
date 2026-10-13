#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# 编译 libvideo2x（核心库）。
#   - 关闭 CLI（因此不需要 Boost）
#   - 使用外部已编译的 ncnn / spdlog
#   - 应用 Android 资源查找补丁
# 产出：$PREFIX/lib/libvideo2x.so 及头文件、模型资源
# -----------------------------------------------------------------------------
source "$(dirname "$0")/config.sh"
check_env
cd "$WORK_DIR"
[ -d video2x ] || die "未找到 video2x，请先运行 fetch_sources.sh"

log "应用补丁 ..."
"$SCRIPT_DIR/apply_patches.sh"

log "配置 libvideo2x ..."
# 说明：-L 用于补上游缺失的 FFmpeg 库搜索路径；
#       max-page-size=16384 满足 Android 16KB page size 对齐要求
#       （否则 Android 15+ 设备会报「LOAD 区段未对齐」导致无法加载）
cmake -S video2x -B "$BUILD_ROOT/video2x" \
  -DCMAKE_TOOLCHAIN_FILE="$CMAKE_TOOLCHAIN_FILE" \
  -DANDROID_ABI="$ABI" -DANDROID_PLATFORM="android-$API_LEVEL" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_PREFIX_PATH="$PREFIX" \
  -Dncnn_DIR="$PREFIX/lib/cmake/ncnn" \
  -Dspdlog_DIR="$PREFIX/lib/cmake/spdlog" \
  -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH \
  -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH \
  -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH \
  -DCMAKE_SHARED_LINKER_FLAGS="-L$PREFIX/lib -Wl,-z,max-page-size=16384" \
  -DCMAKE_MODULE_LINKER_FLAGS="-L$PREFIX/lib -Wl,-z,max-page-size=16384" \
  -DCMAKE_EXE_LINKER_FLAGS="-L$PREFIX/lib -Wl,-z,max-page-size=16384" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DBUILD_SHARED_LIBS=ON \
  -DVIDEO2X_BUILD_CLI=OFF \
  -DVIDEO2X_USE_EXTERNAL_NCNN=ON \
  -DVIDEO2X_USE_EXTERNAL_SPDLOG=ON \
  -DVIDEO2X_USE_EXTERNAL_BOOST=ON \
  -DOpenMP_C_FLAGS="-fopenmp" \
  -DOpenMP_CXX_FLAGS="-fopenmp" \
  -DOpenMP_C_LIB_NAMES="omp" \
  -DOpenMP_CXX_LIB_NAMES="omp" \
  -DOpenMP_omp_LIBRARY="$OMP_LIB"

log "编译 libvideo2x ..."
cmake --build "$BUILD_ROOT/video2x" -j"$JOBS"
cmake --install "$BUILD_ROOT/video2x"

# libomp.so：libvideo2x.so 以 -fopenmp 构建时会依赖它，需随 APK 一起打包
if [ -f "$OMP_LIB" ]; then
  cp -f "$OMP_LIB" "$PREFIX/lib/"
  log "已拷贝 libomp.so"
else
  warn "未找到 libomp.so（$OMP_LIB），若运行时报缺 libomp 请手动补齐"
fi

# 头文件兜底：若上游 install 规则未包含头文件，则手动拷贝
if [ ! -d "$PREFIX/include/libvideo2x" ]; then
  mkdir -p "$PREFIX/include"
  cp -r "$WORK_DIR/video2x/include/libvideo2x" "$PREFIX/include/" 2>/dev/null || true
  log "已手动拷贝 libvideo2x 头文件"
fi

# 资源（RealESRGAN / RealCUGAN / Anime4K 着色器）→ 供 app 打包进 assets
mkdir -p "$PREFIX/res"
if [ -d "$WORK_DIR/video2x/models" ]; then
  cp -r "$WORK_DIR/video2x/models/." "$PREFIX/res/"
fi
# RIFE 模型位于其子模块内
for d in "$WORK_DIR"/video2x/third_party/librife_ncnn_vulkan/models; do
  [ -d "$d" ] && cp -r "$d" "$PREFIX/res/rife" 2>/dev/null || true
done

log "libvideo2x 安装完成：$PREFIX"
log "  lib: $(ls "$PREFIX/lib" 2>/dev/null | tr '\n' ' ')"
