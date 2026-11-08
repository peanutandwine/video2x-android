cmake_minimum_required(VERSION 3.22)
project(video2x_jni CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# 依赖前缀：由 scripts/build_all.sh 产出（FFmpeg / ncnn / libplacebo /
# glslang / spdlog / libvideo2x 统一安装到 scripts/dist/<abi>）。
# gradle 通过 -DVIDEO2X_PREFIX=... 传入。
# ---------------------------------------------------------------------------
if(NOT DEFINED VIDEO2X_PREFIX)
    message(FATAL_ERROR
        "VIDEO2X_PREFIX 未设置。请先运行 scripts/build_all.sh，"
        "或在 gradle 中传入 -DVIDEO2X_PREFIX=<repo>/scripts/dist/arm64-v8a")
endif()

set(V2X_LIB_DIR "${VIDEO2X_PREFIX}/lib")
set(V2X_INC_DIR "${VIDEO2X_PREFIX}/include")
message(STATUS "[video2x] prefix = ${VIDEO2X_PREFIX}")

if(NOT EXISTS "${V2X_LIB_DIR}/libvideo2x.so")
    message(FATAL_ERROR
        "未找到 ${V2X_LIB_DIR}/libvideo2x.so。\n"
        "请先执行 scripts/build_all.sh 完成依赖与核心库的交叉编译。")
endif()

# ---------------------------------------------------------------------------
# 导入预编译库
# ---------------------------------------------------------------------------
add_library(video2x SHARED IMPORTED)
set_target_properties(video2x PROPERTIES
    IMPORTED_LOCATION "${V2X_LIB_DIR}/libvideo2x.so")

set(V2X_IMPORTED video2x)

# FFmpeg（shared）；不存在则跳过，由运行期按 DT_NEEDED 解析
foreach(ff_lib avcodec avformat avfilter avutil swscale swresample)
    if(EXISTS "${V2X_LIB_DIR}/lib${ff_lib}.so")
        add_library(${ff_lib} SHARED IMPORTED)
        set_target_properties(${ff_lib} PROPERTIES
            IMPORTED_LOCATION "${V2X_LIB_DIR}/lib${ff_lib}.so")
        list(APPEND V2X_IMPORTED ${ff_lib})
    endif()
endforeach()

# ---------------------------------------------------------------------------
# JNI 目标
# ---------------------------------------------------------------------------
add_library(video2x_jni SHARED
    video2x_jni.cpp
    anime4k_jni.cpp
    a4k/a4k_shader.cpp
    a4k/a4k_plan.cpp
    a4k/a4k_vk.cpp
)

target_include_directories(video2x_jni PRIVATE
    "${V2X_INC_DIR}"                 # 供 <spdlog/...> 等使用
    "${V2X_INC_DIR}/libvideo2x"      # 供 "libvideo2x.h" / "processor.h" 等使用
    "${CMAKE_CURRENT_SOURCE_DIR}"    # anime4k_bridge.h
    "${CMAKE_CURRENT_SOURCE_DIR}/a4k"
)

find_library(log_lib log)
find_library(android_lib android)
find_library(vulkan_lib vulkan)

# glslang（静态库）：Anime4K 的 GLSL 在运行时编译为 SPIR-V。
# 顺序有依赖：glslang 依赖 MachineIndependent / GenericCodeGen / OSDependent。
set(GLSLANG_LIBS
    "${V2X_LIB_DIR}/libglslang.a"
    "${V2X_LIB_DIR}/libSPIRV.a"
    "${V2X_LIB_DIR}/libMachineIndependent.a"
    "${V2X_LIB_DIR}/libGenericCodeGen.a"
    "${V2X_LIB_DIR}/libOSDependent.a"
    "${V2X_LIB_DIR}/libSPVRemapper.a"
    "${V2X_LIB_DIR}/libglslang-default-resource-limits.a"
)

target_link_libraries(video2x_jni PRIVATE
    ${V2X_IMPORTED}
    ${GLSLANG_LIBS}
    ${vulkan_lib}
    ${log_lib}
    ${android_lib}
)

# 依赖库在运行期由动态链接器解析；链接 JNI 层时允许其存在未定义符号。
# max-page-size=16384 用于满足 Android 16KB page size 对齐要求。
target_link_options(video2x_jni PRIVATE
    "-Wl,--allow-shlib-undefined"
    "-Wl,-z,max-page-size=16384"
    # 静态库符号（glslang / libplacebo 的副本）隐藏起来，避免与
    # libavfilter.so 内部那份 libplacebo/glslang 发生符号抢占。
    "-Wl,--exclude-libs,ALL"
)

# libvideo2x.so 若以 -fopenmp 构建，会依赖 libomp.so。
# 该库由 scripts/build_all.sh 拷贝到 prefix/lib，随 jniLibs 一起打包。
