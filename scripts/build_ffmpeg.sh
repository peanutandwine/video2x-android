#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# FFmpeg：Video2X 的解码/编码/滤镜底座。
#
# 关键开关：
#   --enable-libplacebo  Anime4K 所需（依赖上面编好的 libplacebo + Vulkan）
#   --enable-vulkan      libplacebo filter 需要 Vulkan 硬件设备
#   --enable-mediacodec  硬件解码/编码
#   --enable-libx264     高质量软编码（需 --enable-gpl）
# -----------------------------------------------------------------------------
source "$(dirname "$0")/config.sh"
check_env
cd "$WORK_DIR/ffmpeg" || die "未找到 ffmpeg 源码，请先运行 fetch_sources.sh"

# 打源码补丁（含 mediacodecenc.c 未初始化缓冲区修复）。幂等，可重复执行。
# 注意：必须用 config.sh 导出的绝对路径 SCRIPT_DIR —— 此处工作目录已切到
# ffmpeg 源码目录，$(dirname "$0") 会指向错误位置。
"$SCRIPT_DIR/apply_patches.sh"

export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PKG_CONFIG_PATH"

log "configure FFmpeg ..."
./configure \
  --prefix="$PREFIX" \
  --target-os=android \
  --enable-cross-compile \
  --arch=aarch64 --cpu=armv8-a \
  --sysroot="$SYSROOT" \
  --cc="$CC" --cxx="$CXX" --ar="$AR" --ranlib="$RANLIB" --strip="$STRIP" --nm="$NM" \
  --pkg-config="pkg-config" --pkg-config-flags="--static" \
  --enable-shared --disable-static \
  --disable-programs --disable-doc --disable-debug \
  --enable-jni --enable-mediacodec \
  --enable-vulkan \
  --enable-libplacebo --enable-filter=libplacebo \
  --enable-gpl --enable-libx264 \
  --enable-hwaccel=h264_mediacodec,hevc_mediacodec \
  --enable-decoder=h264_mediacodec,hevc_mediacodec \
  --enable-encoder=h264_mediacodec,hevc_mediacodec \
  --enable-decoder=h264,hevc,mpeg4,vp9,av1 \
  --enable-encoder=libx264,mpeg4 \
  --enable-muxer=mp4,matroska,mov \
  --enable-demuxer=mov,matroska,avi,mpegts \
  --enable-parser=h264,hevc,vp8,vp9,av1 \
  --enable-protocol=file,pipe \
  --enable-swscale --enable-swresample \
  --extra-cflags="-O2 -fPIC -I$PREFIX/include" \
  --extra-ldflags="-L$PREFIX/lib -Wl,-z,max-page-size=16384" \
  || die "FFmpeg configure 失败。若卡在 libplacebo：请确认 libplacebo 为 Vulkan 后端且版本 >= 6.338"

log "编译 FFmpeg ..."
make -j"$JOBS"
make install
log "FFmpeg 安装到 $PREFIX"
