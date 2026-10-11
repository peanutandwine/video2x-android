#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# 把交叉编译产出的所有 .so 汇总到 app/src/main/jniLibs/<abi>/。
#
# Android Gradle 的 jniLibs 要求按 ABI 子目录组织（<dir>/<abi>/*.so），
# 而我们的依赖安装在 <prefix>/lib 下（平铺），因此需要这一步做转换。
# -----------------------------------------------------------------------------
source "$(dirname "$0")/config.sh"

DEST="$ROOT_DIR/app/src/main/jniLibs/$ABI"

[ -d "$PREFIX/lib" ] || die "未找到 $PREFIX/lib，请先运行 build_all.sh"

mkdir -p "$DEST"
rm -f "$DEST"/*.so

shopt -s nullglob
copied=0
for so in "$PREFIX"/lib/*.so; do
  cp -a "$so" "$DEST"/
  copied=$((copied + 1))
done

log "已汇总 $copied 个 .so 到 $DEST"
ls -1 "$DEST"
