// -----------------------------------------------------------------------------
// Video2X Android JNI 桥
//
// 设计要点：
//  1. 完全复用上游 libvideo2x 的 VideoProcessor，不修改其核心逻辑；
//  2. 日志：通过上游公开的 logger_manager::LoggerManager::reconfigure_logger()
//     替换 sink —— 零侵入，无需改上游源码；
//     · initLogger() 先挂 logcat sink（应用早期、尚未授权时用）
//     · setLogFile() 之后改为写文本文件（默认 /storage/emulated/0/Video2x/log）
//  3. FileSink 基于 spdlog::sinks::base_sink（纯头文件模板），
//     因此不需要链接 libspdlog，也不必改动上游的 spdlog 构建方式；
//  4. 资源查找：上游 fsutils::find_resource() 依赖 /proc/self/exe 与 /usr/share，
//     在 Android 上均不成立，故通过环境变量 VIDEO2X_RESOURCE_DIR 指定模型根目录
//     （配合 patches/0001-fsutils-resource-dir.patch 生效）。
// -----------------------------------------------------------------------------

#include <jni.h>

#include <android/log.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <spdlog/sinks/android_sink.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

// FFmpeg 头：AVHWDeviceType / av_get_pix_fmt_name / 滤镜转码（avfilter/avformat/avcodec）
extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
}

#include "libvideo2x/libvideo2x.h"
#include "libvideo2x/logger_manager.h"
#include "libvideo2x/processor.h"

#define LOG_TAG "Video2X-JNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

// 当前正在执行的处理器（个人使用场景下同时只允许一个任务）
std::mutex g_mutex;
std::shared_ptr<video2x::VideoProcessor> g_proc;

// 旋转任务的独立中止标志（与超分任务的 g_proc 互不影响）
std::atomic<bool> g_rotate_abort{false};

// 旋转进度（供 UI 轮询；超分任务用 g_proc，旋转任务用这两个原子计数）
std::atomic<long long> g_rotate_frames{0};
std::atomic<long long> g_rotate_total{0};

/**
 * 在滤镜图中追加一个滤镜节点并连接到上一个节点。
 * @return 新创建的滤镜上下文；失败返回 nullptr（调用方负责善后）
 */
AVFilterContext* append_filter(
    AVFilterGraph* graph,
    AVFilterContext* prev,
    const char* name,
    const char* args
) {
    const AVFilter* f = avfilter_get_by_name(name);
    if (!f) {
        LOGE("rotateVideo: filter '%s' not found", name);
        return nullptr;
    }
    AVFilterContext* ctx = nullptr;
    if (avfilter_graph_create_filter(&ctx, f, name, args, nullptr, graph) < 0) {
        LOGE("rotateVideo: cannot create filter '%s'", name);
        return nullptr;
    }
    if (prev && avfilter_link(prev, 0, ctx, 0) < 0) {
        LOGE("rotateVideo: link to filter '%s' failed", name);
        return nullptr;
    }
    return ctx;
}

std::string jstr(JNIEnv* env, jstring s) {
    if (s == nullptr) {
        return {};
    }
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out(c != nullptr ? c : "");
    if (c != nullptr) {
        env->ReleaseStringUTFChars(s, c);
    }
    return out;
}

// 单帧静态图片的扩展名判断。
//
// 上游只提供 VideoProcessor（文件级 API），没有独立的图片接口；但 FFmpeg 会把
// 单张图片解复用成「只有 1 帧的视频流」，因此图片可以走完全相同的管线 ——
// 只是编码侧必须换成图片编码器（见下方 process() 里的图片分支）。
//
// 注意排除 gif：多帧输入时 image2 muxer 在写第二帧会直接失败
// （"Cannot write more than one file with the same name"）。
bool is_image_file(const std::string& path) {
    const auto dot = path.rfind('.');
    if (dot == std::string::npos) {
        return false;
    }
    std::string ext = path.substr(dot + 1);
    std::transform(
        ext.begin(), ext.end(), ext.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); }
    );
    return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "webp" ||
           ext == "bmp" || ext == "tif" || ext == "tiff";
}

// Kotlin 侧 Algo 枚举 → 上游 ProcessorType
//   1 = Anime4K(libplacebo), 2 = RealESRGAN, 3 = RealCUGAN, 4 = RIFE
bool map_algo(jint algo, video2x::processors::ProcessorType& out) {
    using video2x::processors::ProcessorType;
    switch (algo) {
        case 1:
            out = ProcessorType::Libplacebo;
            return true;
        case 2:
            out = ProcessorType::RealESRGAN;
            return true;
        case 3:
            out = ProcessorType::RealCUGAN;
            return true;
        case 4:
            out = ProcessorType::RIFE;
            return true;
        default:
            return false;
    }
}

/**
 * 写文本文件的 spdlog sink。
 *
 * 基于 base_sink（纯头文件模板），避免依赖 libspdlog 的编译产物，
 * 从而无需改动上游的 spdlog 链接方式。
 */
class FileSink final : public spdlog::sinks::base_sink<std::mutex> {
   public:
    explicit FileSink(const std::string& path)
        : ofs_(path, std::ios::out | std::ios::app) {}

    bool is_open() const { return ofs_.is_open(); }

   protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t formatted;
        base_sink<std::mutex>::formatter_->format(msg, formatted);
        ofs_.write(formatted.data(), static_cast<std::streamsize>(formatted.size()));
        ofs_.flush();  // 实时刷盘，便于任务异常中断时仍能看到日志
    }

    void flush_() override { ofs_.flush(); }

   private:
    std::ofstream ofs_;
};

}  // namespace

extern "C" {

// ---------------------------------------------------------------------------
// 日志初始化：先挂 logcat sink（应用启动早期使用）
// ---------------------------------------------------------------------------
JNIEXPORT void JNICALL
Java_com_video2x_android_Video2XNative_initLogger(JNIEnv* env, jobject /*thiz*/, jstring tag) {
    std::string t = jstr(env, tag);
    if (t.empty()) {
        t = "video2x";
    }
    try {
        auto sink = std::make_shared<spdlog::sinks::android_sink_mt>(t);
        std::vector<spdlog::sink_ptr> sinks{sink};
        video2x::logger_manager::LoggerManager::instance().reconfigure_logger(
            "video2x", sinks, "%+"
        );
        video2x::logger_manager::LoggerManager::instance().hook_ffmpeg_logging();
        LOGI("logger initialized (logcat), tag=%s", t.c_str());
    } catch (const std::exception& e) {
        LOGE("initLogger failed: %s", e.what());
    } catch (...) {
        LOGE("initLogger failed: unknown exception");
    }
}

// ---------------------------------------------------------------------------
// 把日志输出切换到文本文件（父目录需已存在且可写）
// 返回 true 表示切换成功
// ---------------------------------------------------------------------------
JNIEXPORT jboolean JNICALL
Java_com_video2x_android_Video2XNative_setLogFile(
    JNIEnv* env, jobject /*thiz*/, jstring path
) {
    const std::string p = jstr(env, path);
    if (p.empty()) {
        return JNI_FALSE;
    }
    try {
        auto sink = std::make_shared<FileSink>(p);
        if (!sink->is_open()) {
            LOGE("setLogFile: cannot open %s", p.c_str());
            return JNI_FALSE;
        }
        std::vector<spdlog::sink_ptr> sinks{sink};
        video2x::logger_manager::LoggerManager::instance().reconfigure_logger(
            "video2x", sinks, "%+"
        );
        video2x::logger_manager::LoggerManager::instance().hook_ffmpeg_logging();
        LOGI("log file = %s", p.c_str());
        return JNI_TRUE;
    } catch (const std::exception& e) {
        LOGE("setLogFile failed: %s", e.what());
        return JNI_FALSE;
    } catch (...) {
        LOGE("setLogFile failed: unknown exception");
        return JNI_FALSE;
    }
}

JNIEXPORT void JNICALL
Java_com_video2x_android_Video2XNative_setLogLevel(JNIEnv* env, jobject /*thiz*/, jstring level) {
    const std::string l = jstr(env, level);
    try {
        video2x::logger_manager::LoggerManager::instance().set_log_level(l);
    } catch (...) {
        // 忽略非法级别
    }
}

// ---------------------------------------------------------------------------
// 资源根目录：模型与着色器所在目录（绝对路径）
// 例如 /data/user/0/com.video2x.android/files/models
// ---------------------------------------------------------------------------
JNIEXPORT void JNICALL
Java_com_video2x_android_Video2XNative_setResourceDir(
    JNIEnv* env, jobject /*thiz*/, jstring dir
) {
    const std::string d = jstr(env, dir);
    if (!d.empty()) {
        ::setenv("VIDEO2X_RESOURCE_DIR", d.c_str(), 1);
        LOGI("resource dir = %s", d.c_str());
    }
}

// ---------------------------------------------------------------------------
// 执行处理（阻塞）
// 返回值：0 成功；负数对应 FFmpeg AVERROR；-1 为参数/异常错误
// ---------------------------------------------------------------------------
JNIEXPORT jint JNICALL
Java_com_video2x_android_Video2XNative_process(
    JNIEnv* env,
    jobject /*thiz*/,
    jstring in_path,
    jstring out_path,
    jint algo,
    jstring model,
    jint scale,
    jint noise_level,
    jint width,
    jint height,
    jint frm_rate_mul,
    jstring shader,
    jstring codec,
    jlong bit_rate
) {
    const std::string in = jstr(env, in_path);
    const std::string out = jstr(env, out_path);
    const std::string model_name = jstr(env, model);
    const std::string shader_name = jstr(env, shader);
    std::string codec_name = jstr(env, codec);

    if (in.empty() || out.empty()) {
        LOGE("process: empty in/out path");
        return -1;
    }

    using namespace video2x::processors;

    ProcessorConfig proc_cfg;
    proc_cfg.scaling_factor = scale;
    proc_cfg.width = width;
    proc_cfg.height = height;

    if (!map_algo(algo, proc_cfg.processor_type)) {
        LOGE("process: unsupported algo=%d", static_cast<int>(algo));
        return -1;
    }

    switch (proc_cfg.processor_type) {
        case ProcessorType::Libplacebo: {
            LibplaceboConfig c;
            c.shader_path = shader_name;  // 如 "anime4k-v4-a"
            proc_cfg.config = c;
            break;
        }
        case ProcessorType::RealESRGAN: {
            RealESRGANConfig c;
            c.tta_mode = false;
            c.model_name = model_name;  // 如 "realesr-animevideov3"
            proc_cfg.config = c;
            break;
        }
        case ProcessorType::RealCUGAN: {
            RealCUGANConfig c;
            c.tta_mode = false;
            c.num_threads = 1;
            c.syncgap = 3;
            // model_name 是「变体目录名」（models-se / models-pro / models-nose），
            // 上游会拼成 models/realcugan/<model_name>/up<N>x-<noise>.param
            c.model_name = model_name;
            proc_cfg.config = c;
            // 倍率与降噪等级是独立的：倍率走 scaling_factor，
            // 降噪走 ProcessorConfig.noise_level（-1 保守 / 0 无降噪 / N 降噪Nx）
            proc_cfg.noise_level = noise_level;
            break;
        }
        case ProcessorType::RIFE: {
            RIFEConfig c;
            c.tta_mode = false;
            c.tta_temporal_mode = false;
            c.uhd_mode = false;
            c.num_threads = 0;          // 0 = 自动
            c.model_name = model_name;  // 如 "rife-v4.6"
            proc_cfg.config = c;
            proc_cfg.frm_rate_mul = frm_rate_mul > 0 ? frm_rate_mul : 2;
            break;
        }
        default:
            LOGE("process: unhandled processor type");
            return -1;
    }

    // ---- 图片模式 ----
    // 图片与视频走完全相同的 VideoProcessor，差别只在编码侧：
    //   · codec 换成 png —— 输出扩展名是 .png，FFmpeg 据此选 image2 muxer
    //   · pix_fmt 用 RGB24 —— PNG 本身在 RGB 域，走 YUV 反而白过一次色彩转换
    //   · 单张图没有码率 / GOP / 音频字幕的概念，一律关掉
    const bool is_image = is_image_file(in);

    if (is_image && proc_cfg.processor_type == ProcessorType::RIFE) {
        LOGE("process: RIFE 不适用于单张图片（插帧依赖帧间关系）");
        return -1;
    }

    video2x::encoder::EncoderConfig enc_cfg;
    enc_cfg.codec = codec_name.empty() ? std::string("libx264") : codec_name;

    const bool is_mediacodec =
        !is_image && enc_cfg.codec.find("mediacodec") != std::string::npos;

    if (is_image) {
        enc_cfg.codec = "png";
        enc_cfg.pix_fmt = AV_PIX_FMT_RGB24;
        enc_cfg.bit_rate = 0;
        enc_cfg.copy_audio_streams = false;
        enc_cfg.copy_subtitle_streams = false;
    } else {
        // 像素格式：
        //  · MediaCodec 编码器在多数设备上只接受 NV12 —— FFmpeg 实测报
        //    "some devices don't support yuv420p as encoder input format"，
        //    随后 MediaCodec configure 失败(-10000)。
        //  · 软编码器（libx264 / mpeg4）统一用 YUV420P。
        // 上游的 ncnn_mat_to_avframe() 通过 swscale 转换，NV12 是支持的。
        enc_cfg.pix_fmt = is_mediacodec ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
        enc_cfg.bit_rate = static_cast<int64_t>(bit_rate);
        enc_cfg.copy_audio_streams = true;
        enc_cfg.copy_subtitle_streams = true;
    }

    if (is_mediacodec) {
        // MediaCodec 若不指定 gop_size，会退化为 i-frame-interval=1（每帧都是 I 帧），
        // 输出体积暴涨且速度变慢。这里给一个合理的 GOP 长度。
        enc_cfg.gop_size = 30;

        // MediaCodec 要求 bitrate 必须为正数：用户填 0（自动）时，
        // 对 libx264 意味着走默认 CRF，但对 MediaCodec 会导致
        // Encoder configure failed(-10000)。这里补一个保守默认值。
        if (enc_cfg.bit_rate <= 0) {
            enc_cfg.bit_rate = 8LL * 1000 * 1000;  // 8 Mbps
        }
    }

    // 打印编码器**实际收到**的参数（用于核对参数是否真的传到位）
    const char* pix_fmt_name = av_get_pix_fmt_name(enc_cfg.pix_fmt);
    video2x::logger()->info(
        "encoder cfg: mode={} codec={} pix_fmt={} gop_size={} bit_rate={} threads={}",
        is_image ? "image" : "video",
        enc_cfg.codec,
        pix_fmt_name != nullptr ? pix_fmt_name : "none",
        enc_cfg.gop_size,
        enc_cfg.bit_rate,
        enc_cfg.thread_count
    );

    try {
        auto proc = std::make_shared<video2x::VideoProcessor>(
            proc_cfg,
            enc_cfg,
            /* vk_device_idx = */ 0,  // Android 通常只有一个 Vulkan 设备
            AV_HWDEVICE_TYPE_NONE,    // 先使用软件解码（见方案 §5.4）
            /* benchmark = */ false
        );

        {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_proc = proc;
        }

        const int ret = proc->process(in, out);

        {
            std::lock_guard<std::mutex> lk(g_mutex);
            if (g_proc == proc) {
                g_proc.reset();
            }
        }

        LOGI("process finished, ret=%d", ret);
        return ret;
    } catch (const std::exception& e) {
        LOGE("process exception: %s", e.what());
        std::lock_guard<std::mutex> lk(g_mutex);
        g_proc.reset();
        return -1;
    } catch (...) {
        LOGE("process exception: unknown");
        std::lock_guard<std::mutex> lk(g_mutex);
        g_proc.reset();
        return -1;
    }
}

// ---------------------------------------------------------------------------
// 视频旋转 / 翻转（纯软件路径，使用 libavfilter + libx264，完全不经过
// libplacebo / Vulkan —— 因此不会触发横屏超分场景下的 GPU 卡死）。
//
// transform 为位掩码：
//   bit0 (1) = 顺时针旋转 90°（transpose=dir=1）
//   bit1 (2) = 逆时针旋转 90°（transpose=dir=2）
//   bit2 (4) = 水平翻转（hflip）
//   bit3 (8) = 垂直翻转（vflip）
// 各项可自由组合（例如 1+4 = 顺时针 90° + 水平翻转）。
//
// 视频流：解码 → 软件滤镜变换 → 重新编码（libx264，YUV420P）。
// 音频流：直接 stream-copy，不重新编码，保留原内容。
// 返回值：0 成功；-2 用户中止；负数对应 FFmpeg AVERROR；-1 为参数/异常错误。
// ---------------------------------------------------------------------------
JNIEXPORT jint JNICALL
Java_com_video2x_android_Video2XNative_rotateVideo(
    JNIEnv* env,
    jobject /*thiz*/,
    jstring in_path,
    jstring out_path,
    jint transform,
    jstring codec,
    jlong bit_rate
) {
    const std::string in = jstr(env, in_path);
    const std::string out = jstr(env, out_path);
    std::string codec_name = jstr(env, codec);

    if (in.empty() || out.empty()) {
        LOGE("rotateVideo: empty in/out path");
        return -1;
    }
    if (transform == 0) {
        LOGE("rotateVideo: no transform requested (transform=0)");
        return -1;
    }
    if (codec_name.empty()) {
        codec_name = "libx264";
    }

    g_rotate_abort.store(false);
    g_rotate_frames.store(0);

    AVFormatContext* ifmt = nullptr;
    AVCodecContext* vdec_ctx = nullptr;
    AVFormatContext* ofmt = nullptr;
    AVFilterGraph* graph = nullptr;
    AVFilterContext* src_ctx = nullptr;
    AVFilterContext* sink_ctx = nullptr;
    AVCodecContext* enc_ctx = nullptr;
    AVPacket* pkt = nullptr;
    AVPacket* epkt = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* filt = nullptr;
    int ret = -1;

    // 以下变量提前声明，避免被下方的 goto cleanup 跳过初始化
    int outW = 0, outH = 0;
    AVRational outSar = {1, 1};
    const AVCodec* enc = nullptr;
    AVStream* vout = nullptr;
    AVStream* aout = nullptr;
    AVFilterContext* last = nullptr;
    int videoIdx = -1, audioIdx = -1;
    AVCodecParameters* vpar = nullptr;
    const AVCodec* vdec = nullptr;
    int inW = 0, inH = 0;
    AVRational inSar = {1, 1};
    AVRational inTb = {1, 1};
    AVRational inFr = {1, 1};
    int64_t totalFrames = 0;
    int64_t outPts = 0;
    bool aborted = false;

    if (avformat_open_input(&ifmt, in.c_str(), nullptr, nullptr) != 0) {
        LOGE("rotateVideo: cannot open input %s", in.c_str());
        return -1;
    }
    if (avformat_find_stream_info(ifmt, nullptr) < 0) {
        LOGE("rotateVideo: cannot find stream info");
        goto cleanup;
    }

    for (unsigned i = 0; i < ifmt->nb_streams; i++) {
        const auto type = ifmt->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO && videoIdx < 0) videoIdx = static_cast<int>(i);
        else if (type == AVMEDIA_TYPE_AUDIO && audioIdx < 0) audioIdx = static_cast<int>(i);
    }
    if (videoIdx < 0) {
        LOGE("rotateVideo: no video stream found");
        goto cleanup;
    }

    vpar = ifmt->streams[videoIdx]->codecpar;
    vdec = avcodec_find_decoder(vpar->codec_id);
    if (!vdec) {
        LOGE("rotateVideo: video decoder not found (%d)", static_cast<int>(vpar->codec_id));
        goto cleanup;
    }
    vdec_ctx = avcodec_alloc_context3(vdec);
    avcodec_parameters_to_context(vdec_ctx, vpar);
    if (avcodec_open2(vdec_ctx, vdec, nullptr) < 0) {
        LOGE("rotateVideo: cannot open video decoder");
        goto cleanup;
    }

    inW = vdec_ctx->width;
    inH = vdec_ctx->height;
    inSar = vdec_ctx->sample_aspect_ratio;
    inTb = ifmt->streams[videoIdx]->time_base;
    inFr = vdec_ctx->framerate;
    if (inFr.num == 0) {
        inFr = av_guess_frame_rate(ifmt, ifmt->streams[videoIdx], nullptr);
    }
    if (inFr.num == 0 || inFr.den == 0) {
        inFr = (AVRational){30, 1};
    }
    totalFrames = ifmt->streams[videoIdx]->nb_frames;

    if (avformat_alloc_output_context2(&ofmt, nullptr, nullptr, out.c_str()) < 0) {
        LOGE("rotateVideo: cannot alloc output context for %s", out.c_str());
        goto cleanup;
    }

    // ---- 滤镜图：buffer → [transpose/hflip/vflip...] → buffersink ----
    graph = avfilter_graph_alloc();
    if (!graph) {
        LOGE("rotateVideo: cannot alloc filter graph");
        goto cleanup;
    }
    char bufargs[320];
    std::snprintf(bufargs, sizeof(bufargs),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:frame_rate=%d/%d:pixel_aspect=%d/%d",
             inW, inH, vdec_ctx->pix_fmt,
             inTb.num, inTb.den,
             inFr.num, inFr.den,
             inSar.num ? inSar.num : 1, inSar.den ? inSar.den : 1);

    if (avfilter_graph_create_filter(&src_ctx, avfilter_get_by_name("buffer"), "in", bufargs, nullptr, graph) < 0 ||
        avfilter_graph_create_filter(&sink_ctx, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, graph) < 0) {
        LOGE("rotateVideo: cannot create buffer src/sink");
        goto cleanup;
    }

    last = src_ctx;
    if (transform & 1) {  // 顺时针 90°
        last = append_filter(graph, last, "transpose", "dir=1");
        if (!last) goto cleanup;
    }
    if (transform & 2) {  // 逆时针 90°
        last = append_filter(graph, last, "transpose", "dir=2");
        if (!last) goto cleanup;
    }
    if (transform & 4) {  // 水平翻转
        last = append_filter(graph, last, "hflip", "");
        if (!last) goto cleanup;
    }
    if (transform & 8) {  // 垂直翻转
        last = append_filter(graph, last, "vflip", "");
        if (!last) goto cleanup;
    }
    if (avfilter_link(last, 0, sink_ctx, 0) < 0) {
        LOGE("rotateVideo: link to buffersink failed");
        goto cleanup;
    }
    if (avfilter_graph_config(graph, nullptr) < 0) {
        LOGE("rotateVideo: filter graph config failed");
        goto cleanup;
    }

    outW = av_buffersink_get_w(sink_ctx);
    outH = av_buffersink_get_h(sink_ctx);
    outSar = av_buffersink_get_sample_aspect_ratio(sink_ctx);
    LOGI("rotateVideo: %dx%d sar=%d/%d -> %dx%d (transform=0x%x codec=%s)",
         inW, inH, inSar.num, inSar.den, outW, outH, static_cast<unsigned>(transform), codec_name.c_str());

    // ---- 视频编码器 ----
    enc = avcodec_find_encoder_by_name(codec_name.c_str());
    if (!enc) {
        LOGE("rotateVideo: encoder '%s' not found", codec_name.c_str());
        goto cleanup;
    }
    enc_ctx = avcodec_alloc_context3(enc);
    enc_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
    enc_ctx->codec_id = enc->id;
    enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    enc_ctx->width = outW;
    enc_ctx->height = outH;
    enc_ctx->bit_rate = bit_rate > 0 ? bit_rate : 8LL * 1000 * 1000;  // 默认 8 Mbps
    enc_ctx->rc_max_rate = enc_ctx->bit_rate;
    enc_ctx->rc_buffer_size = static_cast<int>(enc_ctx->bit_rate * 2);
    if (inFr.num != 0) {
        enc_ctx->time_base.num = inFr.den;
        enc_ctx->time_base.den = inFr.num;
    } else {
        enc_ctx->time_base = (AVRational){1, 30};
    }
    enc_ctx->gop_size = 30;
    enc_ctx->max_b_frames = 2;
    enc_ctx->thread_count = 0;  // 自动
    if (avcodec_open2(enc_ctx, enc, nullptr) < 0) {
        LOGE("rotateVideo: cannot open encoder");
        goto cleanup;
    }

    vout = avformat_new_stream(ofmt, nullptr);
    avcodec_parameters_from_context(vout->codecpar, enc_ctx);
    vout->time_base = enc_ctx->time_base;
    vout->sample_aspect_ratio = outSar;

    // ---- 音频流 copy ----
    if (audioIdx >= 0) {
        aout = avformat_new_stream(ofmt, nullptr);
        avcodec_parameters_copy(aout->codecpar, ifmt->streams[audioIdx]->codecpar);
        aout->codecpar->codec_tag = 0;
        aout->time_base = ifmt->streams[audioIdx]->time_base;
    }

    if (!(ofmt->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&ofmt->pb, out.c_str(), AVIO_FLAG_WRITE) < 0) {
            LOGE("rotateVideo: cannot open output file %s", out.c_str());
            goto cleanup;
        }
    }
    if (avformat_write_header(ofmt, nullptr) < 0) {
        LOGE("rotateVideo: write header failed");
        goto cleanup;
    }

    g_rotate_total.store(totalFrames > 0 ? totalFrames : 0);

    pkt = av_packet_alloc();
    epkt = av_packet_alloc();
    frame = av_frame_alloc();
    filt = av_frame_alloc();
    if (!pkt || !epkt || !frame || !filt) {
        LOGE("rotateVideo: alloc failed");
        goto cleanup;
    }

    while (!g_rotate_abort.load()) {
        ret = av_read_frame(ifmt, pkt);
        if (ret < 0) break;  // EOF 或错误
        if (pkt->stream_index == videoIdx) {
            if (avcodec_send_packet(vdec_ctx, pkt) < 0) {
                av_packet_unref(pkt);
                continue;
            }
            while (avcodec_receive_frame(vdec_ctx, frame) == 0) {
                if (g_rotate_abort.load()) break;
                if (av_buffersrc_add_frame(src_ctx, frame) < 0) {
                    LOGE("rotateVideo: buffersrc add frame failed");
                    av_packet_unref(pkt);
                    goto cleanup;
                }
                while (av_buffersink_get_frame(sink_ctx, filt) == 0) {
                    filt->pts = outPts++;
                    if (avcodec_send_frame(enc_ctx, filt) < 0) {
                        LOGE("rotateVideo: enc send frame failed");
                        av_frame_unref(filt);
                        break;
                    }
                    while (avcodec_receive_packet(enc_ctx, epkt) == 0) {
                        av_packet_rescale_ts(epkt, enc_ctx->time_base, vout->time_base);
                        epkt->stream_index = vout->index;
                        av_interleaved_write_frame(ofmt, epkt);
                        av_packet_unref(epkt);
                    }
                    av_frame_unref(filt);
                    g_rotate_frames.fetch_add(1);
                }
            }
        } else if (pkt->stream_index == audioIdx) {
            av_packet_rescale_ts(pkt, ifmt->streams[audioIdx]->time_base, aout->time_base);
            pkt->stream_index = aout->index;
            av_interleaved_write_frame(ofmt, pkt);
        }
        av_packet_unref(pkt);
    }

    if (g_rotate_abort.load()) {
        aborted = true;
    } else {
        // ---- flush 解码器 / 滤镜 / 编码器 ----
        avcodec_send_packet(vdec_ctx, nullptr);
        while (avcodec_receive_frame(vdec_ctx, frame) == 0) {
            av_buffersrc_add_frame(src_ctx, frame);
            while (av_buffersink_get_frame(sink_ctx, filt) == 0) {
                filt->pts = outPts++;
                avcodec_send_frame(enc_ctx, filt);
                while (avcodec_receive_packet(enc_ctx, epkt) == 0) {
                    av_packet_rescale_ts(epkt, enc_ctx->time_base, vout->time_base);
                    epkt->stream_index = vout->index;
                    av_interleaved_write_frame(ofmt, epkt);
                    av_packet_unref(epkt);
                }
                av_frame_unref(filt);
                g_rotate_frames.fetch_add(1);
            }
        }
        av_buffersrc_add_frame(src_ctx, nullptr);
        while (av_buffersink_get_frame(sink_ctx, filt) == 0) {
            filt->pts = outPts++;
            avcodec_send_frame(enc_ctx, filt);
            while (avcodec_receive_packet(enc_ctx, epkt) == 0) {
                av_packet_rescale_ts(epkt, enc_ctx->time_base, vout->time_base);
                epkt->stream_index = vout->index;
                av_interleaved_write_frame(ofmt, epkt);
                av_packet_unref(epkt);
            }
            av_frame_unref(filt);
            g_rotate_frames.fetch_add(1);
        }
        avcodec_send_frame(enc_ctx, nullptr);
        while (avcodec_receive_packet(enc_ctx, epkt) == 0) {
            av_packet_rescale_ts(epkt, enc_ctx->time_base, vout->time_base);
            epkt->stream_index = vout->index;
            av_interleaved_write_frame(ofmt, epkt);
            av_packet_unref(epkt);
        }
    }

    av_write_trailer(ofmt);
    ret = aborted ? -2 : 0;

cleanup:
    if (frame) av_frame_free(&frame);
    if (filt) av_frame_free(&filt);
    if (pkt) av_packet_free(&pkt);
    if (epkt) av_packet_free(&epkt);
    if (enc_ctx) avcodec_free_context(&enc_ctx);
    if (graph) avfilter_graph_free(&graph);
    if (vdec_ctx) avcodec_free_context(&vdec_ctx);
    if (ofmt) {
        if (ofmt->pb && !(ofmt->oformat->flags & AVFMT_NOFILE)) avio_closep(&ofmt->pb);
        avformat_free_context(ofmt);
    }
    if (ifmt) avformat_close_input(&ifmt);

    LOGI("rotateVideo finished, ret=%d aborted=%d", ret, (int)g_rotate_abort.load());
    return ret;
}

// ---------------------------------------------------------------------------
// 进度查询 / 中止
// ---------------------------------------------------------------------------
JNIEXPORT jlong JNICALL
Java_com_video2x_android_Video2XNative_getProcessedFrames(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_proc) {
        return static_cast<jlong>(g_proc->get_processed_frames());
    }
    return static_cast<jlong>(g_rotate_frames.load());
}

JNIEXPORT jlong JNICALL
Java_com_video2x_android_Video2XNative_getTotalFrames(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_proc) {
        return static_cast<jlong>(g_proc->get_total_frames());
    }
    return static_cast<jlong>(g_rotate_total.load());
}

JNIEXPORT jint JNICALL
Java_com_video2x_android_Video2XNative_getState(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_proc) {
        return -1;
    }
    return static_cast<jint>(g_proc->get_state());
}

JNIEXPORT void JNICALL
Java_com_video2x_android_Video2XNative_abort(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_proc) {
        g_proc->abort();
        LOGI("abort requested (supersample)");
    }
    g_rotate_abort.store(true);
    LOGI("abort requested (rotate)");
}

}  // extern "C"
