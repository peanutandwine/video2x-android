#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# 公共配置：所有构建脚本都 source 本文件。
# 可通过环境变量覆盖任意一项，例如：
#   ANDROID_NDK_ROOT=/opt/ndk API_LEVEL=28 ABI=arm64-v8a ./build_all.sh
# -----------------------------------------------------------------------------
set -euo pipefail

# ---------- Android 工具链 ----------
export ANDROID_NDK_ROOT="${ANDROID_NDK_ROOT:-$HOME/Android/Sdk/ndk/27.1.12297006}"
export API_LEVEL="${API_LEVEL:-26}"
export ABI="${ABI:-arm64-v8a}"
export HOST_TAG="${HOST_TAG:-linux-x86_64}"

# ---------- 各依赖版本 ----------
export FFMPEG_VERSION="${FFMPEG_VERSION:-7.1}"
export NCNN_VERSION="${NCNN_VERSION:-20240820}"
export LIBPLACEBO_VERSION="${LIBPLACEBO_VERSION:-v7.349.0}"
export GLSLANG_VERSION="${GLSLANG_VERSION:-14.3.0}"
export SPDLOG_VERSION="${SPDLOG_VERSION:-v1.15.0}"
export X264_COMMIT="${X264_COMMIT:-31e19f92f00c7003fa115047ce50978bc98c3a0d}" # stable

# ---------- 目录 ----------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export SCRIPT_DIR
export ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
export WORK_DIR="${WORK_DIR:-$SCRIPT_DIR/work}"        # 源码与中间产物
export PREFIX="${PREFIX:-$SCRIPT_DIR/dist/$ABI}"       # 依赖安装前缀（gradle 从这里取）
export BUILD_ROOT="$WORK_DIR/build"

# ---------- 工具链路径 ----------
export TOOLCHAIN="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/$HOST_TAG"
export SYSROOT="$TOOLCHAIN/sysroot"

case "$ABI" in
  arm64-v8a)    TARGET_TRIPLE="aarch64-linux-android" ;;
  armeabi-v7a)  TARGET_TRIPLE="armv7a-linux-androideabi" ;;
  x86_64)       TARGET_TRIPLE="x86_64-linux-android" ;;
  *) echo "不支持的 ABI: $ABI" >&2; exit 1 ;;
esac
export TARGET_TRIPLE

export CC="$TOOLCHAIN/bin/${TARGET_TRIPLE}${API_LEVEL}-clang"
export CXX="$TOOLCHAIN/bin/${TARGET_TRIPLE}${API_LEVEL}-clang++"
export AR="$TOOLCHAIN/bin/llvm-ar"
export RANLIB="$TOOLCHAIN/bin/llvm-ranlib"
export STRIP="$TOOLCHAIN/bin/llvm-strip"
export NM="$TOOLCHAIN/bin/llvm-nm"

export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig:${PKG_CONFIG_PATH:-}"
export CMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake"

export JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

# NDK 自带的 OpenMP 运行库（libvideo2x 以 -fopenmp 构建时会依赖它）
CLANG_VER="$(ls "$TOOLCHAIN/lib/clang" 2>/dev/null | sort -V | tail -1 || true)"
export OMP_LIB="${TOOLCHAIN}/lib/clang/${CLANG_VER}/lib/linux/${TARGET_TRIPLE%%-*}/libomp.so"

mkdir -p "$PREFIX" "$WORK_DIR" "$BUILD_ROOT"

log() { printf '\033[1;32m[build]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[warn]\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }

check_env() {
  [ -d "$ANDROID_NDK_ROOT" ] || die "ANDROID_NDK_ROOT 不存在: $ANDROID_NDK_ROOT"
  [ -x "$CC" ] || die "未找到编译器: $CC"
  command -v cmake >/dev/null || die "缺少 cmake"
  command -v ninja >/dev/null || warn "缺少 ninja（libplacebo 需要，建议 pip install ninja）"
  command -v meson >/dev/null || warn "缺少 meson（libplacebo 需要，建议 pip install meson）"
  command -v pkg-config >/dev/null || die "缺少 pkg-config"
}
