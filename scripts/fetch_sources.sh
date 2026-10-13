#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# 拉取所有上游源码。默认使用 git clone（需要能访问 GitHub）。
# 若网络受限，可自行下载后放入 $WORK_DIR 并保持目录名一致。
# -----------------------------------------------------------------------------
source "$(dirname "$0")/config.sh"

cd "$WORK_DIR"

clone() {
  local name="$1" url="$2" ref="${3:-}"
  if [ -d "$name" ]; then
    log "$name 已存在，跳过"
    return
  fi
  log "clone $name ..."
  if [ -n "$ref" ]; then
    git clone --depth 1 --branch "$ref" "$url" "$name"
  else
    git clone --depth 1 "$url" "$name"
  fi
}

# video2x 本体（必须带 submodule：三个 ncnn 算法库 + ncnn + spdlog + boost）
if [ ! -d video2x ]; then
  log "clone video2x (含 submodule) ..."
  git clone --depth 1 --recurse-submodules --shallow-submodules \
    https://github.com/k4yt3x/video2x.git video2x
fi

clone glslang   https://github.com/KhronosGroup/glslang.git  "$GLSLANG_VERSION"
clone libplacebo https://github.com/haasn/libplacebo.git     "$LIBPLACEBO_VERSION"
clone vulkan-headers https://github.com/KhronosGroup/Vulkan-Headers.git ""  # 提供 registry/vk.xml
clone ncnn      https://github.com/Tencent/ncnn.git          ""
clone spdlog    https://github.com/gabime/spdlog.git         "$SPDLOG_VERSION"
clone x264      https://code.videolan.org/videolan/x264.git  ""

# glslang 需要 SPIRV-Headers（其 External 子模块）
if [ -d glslang ] && [ ! -d glslang/External/spirv-headers/.git ] && [ ! -e glslang/External/spirv-headers/include ]; then
  log "初始化 glslang 子模块 ..."
  git -C glslang submodule update --init --recursive --depth 1 || \
    warn "glslang 子模块初始化失败，请手动处理 External/spirv-headers"
fi

# FFmpeg 用官方源码包
if [ ! -d ffmpeg ]; then
  log "下载 FFmpeg $FFMPEG_VERSION ..."
  curl -fL -o "ffmpeg-${FFMPEG_VERSION}.tar.xz" \
    "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz"
  tar xf "ffmpeg-${FFMPEG_VERSION}.tar.xz"
  mv "ffmpeg-${FFMPEG_VERSION}" ffmpeg
fi

log "源码准备完成，位于 $WORK_DIR"
