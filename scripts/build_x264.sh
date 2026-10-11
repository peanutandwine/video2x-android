#!/usr/bin/env bash
# x264：提供高质量 H.264 软编码器（libx264）。
# 若不追求编码质量，可跳过本步，改用 Android MediaCodec 硬件编码器
# （VideoProcessor 的 EncoderConfig.codec 设为 "h264_mediacodec"）。
source "$(dirname "$0")/config.sh"
check_env
cd "$WORK_DIR"
[ -d x264 ] || die "未找到 x264，请先运行 fetch_sources.sh"

cd "$WORK_DIR/x264"
make distclean >/dev/null 2>&1 || true

log "配置 x264 ..."
# AS 必须显式指向 clang：
# x264 默认使用 ${cross_prefix}as，即 llvm-as —— 那是 LLVM IR 汇编器，
# 而非 GNU 汇编器，无法处理 x264 的 .S 文件，于是会**静默跳过**全部
# aarch64 汇编、退化为纯 C 实现（性能损失数倍）。
CC="$CC" AS="$CC" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP" \
./configure \
  --prefix="$PREFIX" \
  --host=aarch64-linux-android \
  --cross-prefix="$TOOLCHAIN/bin/llvm-" \
  --sysroot="$SYSROOT" \
  --enable-static --enable-pic --disable-cli \
  --extra-cflags="-O2 -fPIC"

log "编译 x264 ..."
make -j"$JOBS"
make install
log "x264 安装到 $PREFIX"
