#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# 对上游 video2x 源码打最小补丁（幂等，可重复执行）。
#
# 唯一改动：fsutils::find_resource() 增加对 VIDEO2X_RESOURCE_DIR 的查找。
#   上游实现只探测：资源原样路径 / AppImage / /usr/local/share / /usr/share /
#   /proc/self/exe 同目录 —— 这些在 Android 上全部不成立，会导致模型与
#   Anime4K 着色器「找不到文件」。JNI 层会在启动时把 VIDEO2X_RESOURCE_DIR
#   设为 App 私有目录（filesDir/models）。
#
# 其余功能（日志、进度、中止、路径）均通过上游公开 API 或 JNI 侧实现，
# 无需改动上游代码。
# -----------------------------------------------------------------------------
source "$(dirname "$0")/config.sh"

SRC="$WORK_DIR/video2x/src/fsutils.cpp"
if [ -f "$SRC" ]; then
python3 - "$SRC" <<'PY'
import sys, pathlib

path = pathlib.Path(sys.argv[1])
src = path.read_text(encoding="utf-8")

MARK = "VIDEO2X_RESOURCE_DIR"
ANCHOR = "    candidates.push_back(resource);\n"

if MARK in src:
    print("[patch] 已打过补丁，跳过")
    sys.exit(0)

if ANCHOR not in src:
    print("[patch] 未找到锚点，可能上游结构已变化，请手动处理 src/fsutils.cpp", file=sys.stderr)
    sys.exit(1)

injection = ANCHOR + """
#ifndef _WIN32
    // Android: explicit resource root supplied by the embedding application.
    // The JNI layer sets VIDEO2X_RESOURCE_DIR to the app's private models dir.
    if (const char* resdir = std::getenv("VIDEO2X_RESOURCE_DIR")) {
        candidates.push_back(std::filesystem::path(resdir) / resource);
    }
#endif
"""

src = src.replace(ANCHOR, injection, 1)
path.write_text(src, encoding="utf-8")
print("[patch] 已为 src/fsutils.cpp 注入 VIDEO2X_RESOURCE_DIR 支持")
PY
fi

# -----------------------------------------------------------------------------
# FFmpeg 补丁：libavcodec/mediacodecenc.c 的未初始化栈缓冲区。
#
# mediacodec_init_bsf() 中 `char str[128];` 未初始化。当 crop 为 0（输出尺寸
# 已按 16 对齐）而 extract_extradata 为真（mp4 输出会让 video2x 置上
# AV_CODEC_FLAG_GLOBAL_HEADER）时，代码跳过 snprintf、直接对未初始化缓冲区
# 做 av_strlcatf 追加，得到的垃圾字符串交给 av_bsf_list_parse_str() 会解析
# 失败并返回 AVERROR_BSF_NOT_FOUND —— 表现为 MediaCodec 编码器初始化报
# "Bitstream filter not found" 且无法打开编码器。
#
# 修复：显式零初始化该缓冲区，使 crop==0 分支得到合法的 "extract_extradata"。
# 该改动不影响 crop 非 0 的既有路径。
# -----------------------------------------------------------------------------
FFSRC="$WORK_DIR/ffmpeg/libavcodec/mediacodecenc.c"
if [ -f "$FFSRC" ]; then
python3 - "$FFSRC" <<'PY'
import sys, pathlib

path = pathlib.Path(sys.argv[1])
src = path.read_text(encoding="utf-8")

MARK = "char str[128] = { 0 };"
OLD = """    MediaCodecEncContext *s = avctx->priv_data;
    char str[128];
    int ret;
"""
NEW = """    MediaCodecEncContext *s = avctx->priv_data;
    char str[128] = { 0 };
    int ret;
"""

if MARK in src:
    print("[patch] FFmpeg 已打过补丁，跳过")
    sys.exit(0)

if OLD not in src:
    print("[patch] FFmpeg 未找到锚点，可能上游结构已变化，请手动处理 "
          "libavcodec/mediacodecenc.c", file=sys.stderr)
    sys.exit(1)

src = src.replace(OLD, NEW, 1)
path.write_text(src, encoding="utf-8")
print("[patch] FFmpeg mediacodecenc.c: str[128] 已初始化为 {0}")
PY
fi

# -----------------------------------------------------------------------------
# 补丁 3：video2x/src/conversions.cpp —— 跨帧缓存 SwsContext。
#
# 上游的 avframe_to_ncnn_mat() 与 ncnn_mat_to_avframe() 每次都
# sws_getContext() / sws_freeContext()，即每帧两次。对带缩放的转换
# （1440x1920 -> 2880x3840）sws_getContext 需要计算滤波器系数表，开销可观；
# 而整段视频里源/目标尺寸与像素格式是固定的，完全应复用同一个 context。
#
# 这里用 thread_local 的 map 按参数组合缓存（两种组合各留一份，避免互相冲刷）。
# 由于 context 由缓存持有，本文件内的 sws_freeContext() 被置空操作。
# -----------------------------------------------------------------------------
CONV="$WORK_DIR/video2x/src/conversions.cpp"
if [ -f "$CONV" ]; then
python3 - "$CONV" <<'PY'
import sys, pathlib

path = pathlib.Path(sys.argv[1])
src = path.read_text(encoding="utf-8")

if "v2x_cached_sws_getContext" in src:
    print("[patch] conversions.cpp 已打过补丁，跳过")
    sys.exit(0)

ANCHOR = '#include "logger_manager.h"\n'
if ANCHOR not in src:
    print("[patch] conversions.cpp 未找到锚点", file=sys.stderr)
    sys.exit(1)

INJECT = ANCHOR + '''
#include <map>
#include <tuple>

// ---- video2x-android: 跨帧缓存 SwsContext ----
namespace {

struct V2xSwsKey {
    int sw, sh, sf, dw, dh, df;
    bool operator<(const V2xSwsKey& o) const {
        return std::tie(sw, sh, sf, dw, dh, df) < std::tie(o.sw, o.sh, o.sf, o.dw, o.dh, o.df);
    }
};

SwsContext* v2x_cached_sws_getContext(
    int sw,
    int sh,
    int sf,
    int dw,
    int dh,
    int df,
    int flags,
    SwsFilter* src_filter,
    SwsFilter* dst_filter,
    const double* param
) {
    static thread_local std::map<V2xSwsKey, SwsContext*> cache;
    V2xSwsKey key{sw, sh, sf, dw, dh, df};
    auto it = cache.find(key);
    if (it != cache.end()) {
        return it->second;
    }
    SwsContext* ctx = sws_getContext(
        sw,
        sh,
        static_cast<AVPixelFormat>(sf),
        dw,
        dh,
        static_cast<AVPixelFormat>(df),
        flags,
        src_filter,
        dst_filter,
        param
    );
    cache.emplace(key, ctx);
    return ctx;
}

}  // namespace

// 本文件内所有 sws_getContext() 走缓存；context 归缓存所有，故置空 free。
#define sws_getContext v2x_cached_sws_getContext
#define sws_freeContext(ctx) ((void)0)
'''

src = src.replace(ANCHOR, INJECT, 1)
path.write_text(src, encoding="utf-8")
print("[patch] conversions.cpp: SwsContext 已改为跨帧缓存")
PY
fi

# -----------------------------------------------------------------------------
# 补丁 4：video2x/src/filter_realesrgan.cpp —— 性能剖析 + tilesize 诊断。
#
# 分三段计时（convert_in / infer / convert_out），每帧一行日志，用于定位
# 「一帧 5 秒」到底花在哪一环；同时把 GPU heap budget 与自动选出的 tilesize
# 打出来（tilesize 越小，prepadding 重叠冗余越大，越慢）。
# -----------------------------------------------------------------------------
FRG="$WORK_DIR/video2x/src/filter_realesrgan.cpp"
if [ -f "$FRG" ]; then
python3 - "$FRG" <<'PY'
import sys, pathlib

path = pathlib.Path(sys.argv[1])
src = path.read_text(encoding="utf-8")

if "v2x_t0" in src:
    print("[patch] filter_realesrgan.cpp 已打过补丁，跳过")
    sys.exit(0)

def sub(old, new, what):
    global src
    if old not in src:
        print("[patch] filter_realesrgan.cpp 未找到锚点: " + what, file=sys.stderr)
        sys.exit(1)
    src = src.replace(old, new, 1)

# 1) 引入 <chrono>
sub(
    '#include "logger_manager.h"\n',
    '#include "logger_manager.h"\n\n#include <chrono>\n',
    "include"
)

# 2) filter() 起点计时
sub(
    '''int FilterRealesrgan::filter(AVFrame* in_frame, AVFrame** out_frame) {
    int ret;

    // Convert the input frame to RGB24
    ncnn::Mat in_mat = conversions::avframe_to_ncnn_mat(in_frame);
''',
    '''int FilterRealesrgan::filter(AVFrame* in_frame, AVFrame** out_frame) {
    int ret;
    const auto v2x_t0 = std::chrono::steady_clock::now();

    // Convert the input frame to RGB24
    ncnn::Mat in_mat = conversions::avframe_to_ncnn_mat(in_frame);
    const auto v2x_t1 = std::chrono::steady_clock::now();
''',
    "filter head"
)

# 3) 推理结束计时
sub(
    '''    ret = realesrgan_->process(in_mat, out_mat);
    if (ret != 0) {
        logger()->error("Real-ESRGAN processing failed");
        return ret;
    }
''',
    '''    ret = realesrgan_->process(in_mat, out_mat);
    const auto v2x_t2 = std::chrono::steady_clock::now();
    if (ret != 0) {
        logger()->error("Real-ESRGAN processing failed");
        return ret;
    }
''',
    "process"
)

# 4) 输出转换结束计时 + 汇总日志
sub(
    '''    // Rescale PTS to encoder's time base
    (*out_frame)->pts = av_rescale_q(in_frame->pts, in_time_base_, out_time_base_);

    // Return the processed frame to the caller
    return ret;
}
''',
    '''    const auto v2x_t3 = std::chrono::steady_clock::now();

    auto v2x_ms = [](const std::chrono::steady_clock::time_point& a,
                     const std::chrono::steady_clock::time_point& b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    logger()->info(
        "[profile] {}x{} -> {}x{} | convert_in={:.1f}ms infer={:.1f}ms convert_out={:.1f}ms "
        "| filter_total={:.1f}ms",
        in_mat.w,
        in_mat.h,
        out_mat.w,
        out_mat.h,
        v2x_ms(v2x_t0, v2x_t1),
        v2x_ms(v2x_t1, v2x_t2),
        v2x_ms(v2x_t2, v2x_t3),
        v2x_ms(v2x_t0, v2x_t3)
    );

    // Rescale PTS to encoder's time base
    (*out_frame)->pts = av_rescale_q(in_frame->pts, in_time_base_, out_time_base_);

    // Return the processed frame to the caller
    return ret;
}
''',
    "filter tail"
)

# 5) init() 里打印 GPU heap budget 与实际 tilesize
sub(
    '''    } else {
        realesrgan_->tilesize = 32;
    }

    return 0;
}
''',
    '''    } else {
        realesrgan_->tilesize = 32;
    }

    logger()->info(
        "[profile] GPU heap_budget={}MB tilesize={} prepadding={}",
        heap_budget,
        realesrgan_->tilesize,
        realesrgan_->prepadding
    );

    return 0;
}
''',
    "tilesize"
)

path.write_text(src, encoding="utf-8")
print("[patch] filter_realesrgan.cpp: 已注入分段计时与 tilesize 诊断")
PY
fi

# -----------------------------------------------------------------------------
# 修复 5：进度计数在 EAGAIN 时重复累加（表现为 860/859）
#
# 现象：任务结束/接近结束时，进度显示的处理帧数比总帧数还多 1。
#
# 原因：VideoProcessor::process_frames() 的主循环里，无论处理器是否真的产出了
# 帧，都会执行一次 frame_idx_.fetch_add(1)。而 libplacebo（Anime4K）的 filter
# graph 存在内部延迟 —— av_buffersink_get_frame() 会返回 AVERROR(EAGAIN)，
# 语义是「输入帧已收下，输出尚未就绪」。被延迟的那一帧会在 flush() 阶段
# 吐出、写入编码器并**再次**计数。结果：同一帧计两次、只写一次。
#
# 佐证：x264 结束时打印的统计（frame I + P + B 之和）等于真实输出帧数，
# 且与 total_frames_ 一致 —— 说明多出来的那次纯属重复计数，输出本身正确。
#
# 这也解释了为何 Real-ESRGAN 不出现该现象：ncnn 是同步处理，不会返回 EAGAIN。
# -----------------------------------------------------------------------------
L2X="$WORK_DIR/video2x/src/libvideo2x.cpp"
if [ -f "$L2X" ]; then
python3 - "$L2X" <<'PY'
import pathlib, sys

path = pathlib.Path(sys.argv[1])
src = path.read_text(encoding="utf-8")

if "EAGAIN 不计入进度" in src:
    print("[patch] libvideo2x.cpp: EAGAIN 计数修复已存在，跳过")
    sys.exit(0)

ANCHOR = """                av_frame_unref(frame.get());
                frame_idx_.fetch_add(1);
"""

if src.count(ANCHOR) != 1:
    print(f"[patch] libvideo2x.cpp: 锚点命中 {src.count(ANCHOR)} 次（期望 1），跳过")
    sys.exit(0)

INJECT = """                av_frame_unref(frame.get());

                // EAGAIN 不计入进度：处理器的 filter graph 有内部延迟
                //（libplacebo 尤其明显），此时输入已被收下、输出尚未就绪。
                // 该帧的实际产出会在后续调用或 flush() 阶段写入并计数，
                // 此处若也累加会让计数超出总数（表现为 860/859）。
                if (ret == AVERROR(EAGAIN)) {
                    continue;
                }

                frame_idx_.fetch_add(1);
"""

src = src.replace(ANCHOR, INJECT, 1)
path.write_text(src, encoding="utf-8")
print("[patch] libvideo2x.cpp: 已修复 EAGAIN 时的重复计数")
PY
else
  echo "[patch] 未找到 libvideo2x.cpp，跳过"
fi

log "补丁应用完成"
