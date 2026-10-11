#!/usr/bin/env bash
# spdlog：日志库。安装头文件与 CMake 配置，供 video2x 的 find_package(spdlog) 使用。
# JNI 层会利用它挂载一个 logcat sink（android_sink）。
source "$(dirname "$0")/config.sh"
check_env
cd "$WORK_DIR"
[ -d spdlog ] || die "未找到 spdlog，请先运行 fetch_sources.sh"

log "配置 spdlog ..."
cmake -S spdlog -B "$BUILD_ROOT/spdlog" \
  -DCMAKE_TOOLCHAIN_FILE="$CMAKE_TOOLCHAIN_FILE" \
  -DANDROID_ABI="$ABI" -DANDROID_PLATFORM="android-$API_LEVEL" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_BUILD_TYPE=Release \
  -DSPDLOG_BUILD_SHARED=OFF \
  -DSPDLOG_BUILD_EXAMPLE=OFF \
  -DSPDLOG_BUILD_EXAMPLE_HO=OFF \
  -DSPDLOG_BUILD_TESTS=OFF \
  -DSPDLOG_BUILD_BENCH=OFF \
  -DSPDLOG_INSTALL=ON

cmake --build "$BUILD_ROOT/spdlog" -j"$JOBS"
cmake --install "$BUILD_ROOT/spdlog"
log "spdlog 安装到 $PREFIX"
