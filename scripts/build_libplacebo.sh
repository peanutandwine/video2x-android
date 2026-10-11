#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# libplacebo：Anime4K 着色器的渲染后端。
#
# 关键点：必须启用 **Vulkan 后端**（-Dvulkan=enabled），因为 FFmpeg 的
# libplacebo avfilter 通过 Vulkan 设备工作（见方案 §2.2 路线 A）。
# 这与 mpv-android 的 -Dvulkan=disabled（GL 后端）不同。
#
# 这是全案风险最高的一步。如果 Vulkan 后端在 Android 上失败，
# 请回到方案 §2.2 的路线 B（GLES 后端 + 重写 filter_libplacebo.cpp）。
# -----------------------------------------------------------------------------
source "$(dirname "$0")/config.sh"
check_env
cd "$WORK_DIR"
[ -d libplacebo ] || die "未找到 libplacebo，请先运行 fetch_sources.sh"
command -v meson >/dev/null || die "需要 meson（pip install meson ninja）"

# NDK 自带 Vulkan 头与 libvulkan.so，但没有 pkg-config 描述文件，
# 而 libplacebo 的 meson 通过 dependency('vulkan') 探测，因此这里补一个。
mkdir -p "$PREFIX/lib/pkgconfig"
# FFmpeg 7.1 要求 Vulkan 头 >= 1.3.277（VK_HEADER_VERSION >= 277），
# 而 NDK r27 自带的是 275。因此让 vulkan.pc 指向 Vulkan-Headers 的新头，
# 仅从 NDK 取 libvulkan.so 用于链接。
VK_HDR_VER="$(grep -oE '#define VK_HEADER_VERSION +[0-9]+' \
  "$WORK_DIR/vulkan-headers/include/vulkan/vulkan_core.h" 2>/dev/null | grep -oE '[0-9]+' | head -1)"
VK_PC_VER="1.3.${VK_HDR_VER:-277}"
cat > "$PREFIX/lib/pkgconfig/vulkan.pc" <<EOF
prefix=$SYSROOT/usr
includedir=$WORK_DIR/vulkan-headers/include
libdir=$SYSROOT/usr/lib/${TARGET_TRIPLE}/${API_LEVEL}
Name: Vulkan
Description: Vulkan loader (Android NDK) + Vulkan-Headers
Version: $VK_PC_VER
Cflags: -I\${includedir}
Libs: -L\${libdir} -lvulkan
EOF
log "已生成 vulkan.pc (version=$VK_PC_VER, headers=$WORK_DIR/vulkan-headers/include)"

# meson 交叉编译描述文件
cat > "$BUILD_ROOT/libplacebo-crossfile.ini" <<EOF
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$AR'
strip = '$STRIP'
pkg-config = 'pkg-config'

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[properties]
needs_exe_wrapper = true

# 关键：libplacebo 通过 cxx.find_library('SPIRV' / 'MachineIndependent' ...)
# 与 cc.has_header('glslang/build_info.h') 探测 glslang，
# meson 不会自动搜索 --prefix，必须显式给出 -I/-L。
[built-in options]
c_args        = ['-I$PREFIX/include', '-I$WORK_DIR/vulkan-headers/include']
c_link_args   = ['-L$PREFIX/lib']
cpp_args      = ['-I$PREFIX/include', '-I$WORK_DIR/vulkan-headers/include']
cpp_link_args = ['-L$PREFIX/lib']
EOF

# meson 的 cxx.find_library() / cc.has_header() 在链接探测阶段只搜索编译器
# 默认目录，既不认 --prefix，也不认 crossfile 的 c_link_args 与 LIBRARY_PATH
# （均实测无效）。因此把 glslang 的静态库与头文件软链到 NDK sysroot，
# 使其对 meson 的探测可见。
SYSROOT_LIB="$SYSROOT/usr/lib/${TARGET_TRIPLE}"
SYSROOT_INC="$SYSROOT/usr/include"
mkdir -p "$SYSROOT_LIB" "$SYSROOT_INC"
for f in "$PREFIX"/lib/*.a "$PREFIX"/lib/*.so; do
  [ -e "$f" ] && ln -sf "$f" "$SYSROOT_LIB/$(basename "$f")"
done
if [ -d "$PREFIX/include/glslang" ]; then
  ln -sfn "$PREFIX/include/glslang" "$SYSROOT_INC/glslang"
fi
log "已将 glslang 产物软链至 NDK sysroot 以便 meson 探测"

# 重新配置前清空旧的 build 目录，避免 meson 报 "directory already configured"
rm -rf "$BUILD_ROOT/libplacebo"

# libplacebo 需要 Vulkan registry (vk.xml) 来生成 src/vulkan/utils_gen.c；
# NDK 只带 vulkan.h，不含 registry，必须显式指定（来自 Vulkan-Headers）。
VULKAN_REG=""
if [ -f "$WORK_DIR/vulkan-headers/registry/vk.xml" ]; then
  VULKAN_REG="-Dvulkan-registry=$WORK_DIR/vulkan-headers/registry/vk.xml"
  log "使用 vk.xml: $WORK_DIR/vulkan-headers/registry/vk.xml"
else
  warn "未找到 vk.xml，libplacebo 编译可能失败（需要 Vulkan-Headers/registry/vk.xml）"
fi

log "配置 libplacebo（Vulkan 后端）..."
meson setup "$BUILD_ROOT/libplacebo" libplacebo \
  --cross-file "$BUILD_ROOT/libplacebo-crossfile.ini" \
  --prefix "$PREFIX" \
  --default-library static \
  --buildtype release \
  -Dvulkan=enabled \
  $VULKAN_REG \
  -Dopengl=disabled \
  -Dd3d11=disabled \
  -Dglslang=enabled \
  -Dshaderc=disabled \
  -Dlcms=disabled \
  -Ddemos=false \
  -Dtests=false \
  -Dprefer_static=true

log "编译 libplacebo ..."
ninja -C "$BUILD_ROOT/libplacebo" -j"$JOBS"
ninja -C "$BUILD_ROOT/libplacebo" install

# 静态链接 libc++ 的 workaround：meson 生成的 .pc 不含 -lc++，
# 桌面无影响，Android 上静态链接会缺符号。
PC="$PREFIX/lib/pkgconfig/libplacebo.pc"
if [ -f "$PC" ] && ! grep -q -- '-lc++' "$PC"; then
  sed -i 's/^Libs:\(.*\)$/Libs:\1 -lc++/' "$PC"
  log "已为 libplacebo.pc 追加 -lc++"
fi

log "libplacebo 安装到 $PREFIX"
