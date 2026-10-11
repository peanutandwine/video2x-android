#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# 一键构建全部依赖与核心库。
# 编译顺序不可调换（存在依赖关系）：
#   glslang → libplacebo → x264 → FFmpeg → ncnn → spdlog → libvideo2x
# -----------------------------------------------------------------------------
source "$(dirname "$0")/config.sh"
check_env

log "目标 ABI = $ABI, API = $API_LEVEL"
log "NDK      = $ANDROID_NDK_ROOT"
log "PREFIX   = $PREFIX"

"$SCRIPT_DIR/fetch_sources.sh"
"$SCRIPT_DIR/build_glslang.sh"
"$SCRIPT_DIR/build_libplacebo.sh"
"$SCRIPT_DIR/build_x264.sh"
"$SCRIPT_DIR/build_ffmpeg.sh"
"$SCRIPT_DIR/build_ncnn.sh"
"$SCRIPT_DIR/build_spdlog.sh"
"$SCRIPT_DIR/build_video2x.sh"

log "=============================================="
log "全部完成。依赖前缀：$PREFIX"
log "现在可执行 ./gradlew :app:assembleDebug"
log "=============================================="
