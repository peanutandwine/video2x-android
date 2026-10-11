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

// FFmpeg 头（用于 AVHWDeviceType）
extern "C" {
#include <libavutil/hwcontext.h>
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

    video2x::encoder::EncoderConfig enc_cfg;
    enc_cfg.codec = codec_name.empty() ? std::string("libx264") : codec_name;

    // 像素格式：
    //  · MediaCodec 编码器在多数设备上只接受 NV12 —— FFmpeg 实测报
    //    "some devices don't support yuv420p as encoder input format"，
    //    随后 MediaCodec configure 失败(-10000)。
    //  · 软编码器（libx264 / mpeg4）统一用 YUV420P。
    // 上游的 ncnn_mat_to_avframe() 通过 swscale 转换，NV12 是支持的。
    const bool is_mediacodec = enc_cfg.codec.find("mediacodec") != std::string::npos;
    enc_cfg.pix_fmt = is_mediacodec ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;

    enc_cfg.bit_rate = static_cast<int64_t>(bit_rate);

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

    enc_cfg.copy_audio_streams = true;
    enc_cfg.copy_subtitle_streams = true;

    // 打印编码器**实际收到**的参数（用于核对参数是否真的传到位）
    video2x::logger()->info(
        "encoder cfg: codec={} pix_fmt={} gop_size={} bit_rate={} threads={}",
        enc_cfg.codec,
        is_mediacodec ? "nv12" : "yuv420p",
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
// 进度查询 / 中止
// ---------------------------------------------------------------------------
JNIEXPORT jlong JNICALL
Java_com_video2x_android_Video2XNative_getProcessedFrames(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_proc) {
        return 0;
    }
    return static_cast<jlong>(g_proc->get_processed_frames());
}

JNIEXPORT jlong JNICALL
Java_com_video2x_android_Video2XNative_getTotalFrames(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_proc) {
        return 0;
    }
    return static_cast<jlong>(g_proc->get_total_frames());
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
        LOGI("abort requested");
    }
}

}  // extern "C"
