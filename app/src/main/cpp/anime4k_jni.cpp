// -----------------------------------------------------------------------------
// Anime4K 直连管线（不走 ffmpeg 的 libplacebo 滤镜）
//
// 为什么需要它：
//   ffmpeg 7.x 的 libavutil/hwcontext_vulkan 强制要求 timeline semaphore，
//   而 libplacebo 本身要求 Vulkan 1.2（PL_VK_MIN_VERSION）+ hostQueryReset /
//   timelineSemaphore。骁龙 865 的 Adreno 650 驱动只到 Vulkan 1.1，两条路
//   都走不通。这里用 Vulkan 1.1 重写执行层（见 a4k/），完全绕开 libplacebo。
// -----------------------------------------------------------------------------
#include "anime4k_bridge.h"

#include <jni.h>

#include <android/log.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include "a4k/a4k_vk.h"

#define A4K_TAG "Video2X-A4K"
#define A4K_LOGI(...) __android_log_print(ANDROID_LOG_INFO, A4K_TAG, __VA_ARGS__)
#define A4K_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, A4K_TAG, __VA_ARGS__)

namespace {

std::atomic<bool> g_abort{false};
std::atomic<long> g_frames{0};
std::atomic<long> g_total{0};

std::string jstr(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* p = env->GetStringUTFChars(s, nullptr);
    std::string out = p ? p : "";
    if (p) env->ReleaseStringUTFChars(s, p);
    return out;
}

struct SwsHolder {
    SwsContext* ctx = nullptr;
    int sw = 0, sh = 0, sf = -1, df = -1;

    SwsContext* get(int w, int h, int sfmt, int dfmt, int dw, int dh) {
        if (ctx && sw == w && sh == h && sf == sfmt && df == dfmt) return ctx;
        if (ctx) sws_freeContext(ctx);
        ctx = sws_getContext(
            w, h, (AVPixelFormat)sfmt, dw, dh, (AVPixelFormat)dfmt, SWS_BILINEAR,
            nullptr, nullptr, nullptr
        );
        sw = w;
        sh = h;
        sf = sfmt;
        df = dfmt;
        return ctx;
    }
    ~SwsHolder() {
        if (ctx) sws_freeContext(ctx);
    }
};

// 单帧：任意 pix_fmt -> RGBA
bool to_rgba(AVFrame* f, SwsHolder* sws, std::vector<uint8_t>* buf) {
    const int w = f->width, h = f->height;
    const size_t need = (size_t)w * h * 4;
    if (buf->size() < need) buf->resize(need);

    SwsContext* c = sws->get(w, h, f->format, AV_PIX_FMT_RGBA, w, h);
    if (!c) return false;

    uint8_t* dst[4] = {buf->data(), nullptr, nullptr, nullptr};
    int dst_stride[4] = {w * 4, 0, 0, 0};
    sws_scale(c, f->data, f->linesize, 0, h, dst, dst_stride);
    return true;
}

// RGBA -> YUV420P（编码器输入）
bool rgba_to_yuv420(
    const uint8_t* rgba, int w, int h, SwsHolder* sws, AVFrame* out
) {
    SwsContext* c = sws->get(w, h, AV_PIX_FMT_RGBA, AV_PIX_FMT_YUV420P, w, h);
    if (!c) return false;
    const uint8_t* src[4] = {rgba, nullptr, nullptr, nullptr};
    int src_stride[4] = {w * 4, 0, 0, 0};
    sws_scale(c, src, src_stride, 0, h, out->data, out->linesize);
    return true;
}

int encode_png(const std::string& out_path, const uint8_t* rgba, int w, int h) {
    const AVCodec* enc = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (!enc) {
        A4K_LOGE("png encoder not found");
        return -1;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(enc);
    if (!ctx) return -1;
    ctx->width = w;
    ctx->height = h;
    ctx->pix_fmt = AV_PIX_FMT_RGBA;
    ctx->time_base = (AVRational){1, 25};

    int ret = -1;
    AVFrame* f = nullptr;
    AVPacket* pkt = nullptr;
    AVFormatContext* ofmt = nullptr;

    do {
        if (avcodec_open2(ctx, enc, nullptr) < 0) {
            A4K_LOGE("png encoder open failed");
            break;
        }
        f = av_frame_alloc();
        pkt = av_packet_alloc();
        if (!f || !pkt) break;

        f->format = AV_PIX_FMT_RGBA;
        f->width = w;
        f->height = h;
        if (av_frame_get_buffer(f, 1) < 0) break;
        for (int y = 0; y < h; y++) {
            std::memcpy(f->data[0] + (size_t)y * f->linesize[0], rgba + (size_t)y * w * 4, w * 4);
        }

        if (avformat_alloc_output_context2(&ofmt, nullptr, "image2", out_path.c_str()) < 0) {
            A4K_LOGE("png muxer alloc failed");
            break;
        }
        AVStream* st = avformat_new_stream(ofmt, nullptr);
        if (!st) break;
        avcodec_parameters_from_context(st->codecpar, ctx);
        st->time_base = ctx->time_base;

        if (!(ofmt->oformat->flags & AVFMT_NOFILE)) {
            if (avio_open(&ofmt->pb, out_path.c_str(), AVIO_FLAG_WRITE) < 0) {
                A4K_LOGE("png output open failed");
                break;
            }
        }
        if (avformat_write_header(ofmt, nullptr) < 0) break;

        if (avcodec_send_frame(ctx, f) < 0) break;
        avcodec_send_frame(ctx, nullptr);
        while (avcodec_receive_packet(ctx, pkt) == 0) {
            av_packet_rescale_ts(pkt, ctx->time_base, st->time_base);
            pkt->stream_index = st->index;
            av_interleaved_write_frame(ofmt, pkt);
            av_packet_unref(pkt);
        }
        av_write_trailer(ofmt);
        ret = 0;
    } while (0);

    if (ofmt) {
        if (ofmt->pb && !(ofmt->oformat->flags & AVFMT_NOFILE)) avio_closep(&ofmt->pb);
        avformat_free_context(ofmt);
    }
    if (pkt) av_packet_free(&pkt);
    if (f) av_frame_free(&f);
    avcodec_free_context(&ctx);
    return ret;
}

}  // namespace

// --------------------------------------------------------------------- bridge

int a4k_processed_frames() { return (int)g_frames.load(); }
int a4k_total_frames() { return (int)g_total.load(); }
void a4k_request_abort() { g_abort.store(true); }

// ---------------------------------------------------------------- image path

static int process_image(
    const std::string& in, const std::string& out, const std::string& shader_text,
    int out_w, int out_h
) {
    AVFormatContext* ifmt = nullptr;
    AVCodecContext* dec = nullptr;
    AVFrame* frame = nullptr;
    int ret = -1;
    int v_idx = -1;   // 必须早于任何 goto：跳过带初始化的声明在 C++ 里非法

    if (avformat_open_input(&ifmt, in.c_str(), nullptr, nullptr) != 0) return -1;
    if (avformat_find_stream_info(ifmt, nullptr) < 0) goto done;
    for (unsigned i = 0; i < ifmt->nb_streams; i++) {
        if (ifmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            v_idx = (int)i;
            break;
        }
    }
    if (v_idx < 0) goto done;

    {
        const AVCodec* c = avcodec_find_decoder(ifmt->streams[v_idx]->codecpar->codec_id);
        if (!c) goto done;
        dec = avcodec_alloc_context3(c);
        avcodec_parameters_to_context(dec, ifmt->streams[v_idx]->codecpar);
        if (avcodec_open2(dec, c, nullptr) < 0) goto done;
    }

    frame = av_frame_alloc();
    {
        AVPacket* pkt = av_packet_alloc();
        bool got = false;
        while (!got && av_read_frame(ifmt, pkt) >= 0) {
            if (pkt->stream_index == v_idx) {
                if (avcodec_send_packet(dec, pkt) == 0 && avcodec_receive_frame(dec, frame) == 0) {
                    got = true;
                }
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
        if (!got) goto done;
    }

    {
        SwsHolder sws_in;
        std::vector<uint8_t> rgba;
        if (!to_rgba(frame, &sws_in, &rgba)) goto done;

        a4k::Renderer r;
        std::string err;
        if (!r.init(frame->width, frame->height, out_w, out_h, shader_text, &err)) {
            A4K_LOGE("init failed: %s", err.c_str());
            goto done;
        }
        std::vector<uint8_t> outbuf((size_t)out_w * out_h * 4);
        if (!r.render(rgba.data(), outbuf.data(), &err)) {
            A4K_LOGE("render failed: %s", err.c_str());
            goto done;
        }
        g_frames.store(1);
        g_total.store(1);
        ret = encode_png(out, outbuf.data(), out_w, out_h) == 0 ? 0 : -1;
    }

done:
    if (frame) av_frame_free(&frame);
    if (dec) avcodec_free_context(&dec);
    if (ifmt) avformat_close_input(&ifmt);
    return ret;
}

// ---------------------------------------------------------------- video path

static int process_video(
    const std::string& in, const std::string& out, const std::string& shader_text,
    int out_w, int out_h, const std::string& codec_name, int64_t bit_rate
) {
    AVFormatContext* ifmt = nullptr;
    AVCodecContext* vdec = nullptr;
    AVCodecContext* enc = nullptr;
    AVFormatContext* ofmt = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* enc_frame = nullptr;
    AVPacket* pkt = nullptr;
    AVPacket* epkt = nullptr;

    int ret = -1;
    int videoIdx = -1, audioIdx = -1;
    int64_t outPts = 0;
    bool aborted = false;

    if (avformat_open_input(&ifmt, in.c_str(), nullptr, nullptr) != 0) {
        A4K_LOGE("cannot open input");
        return -1;
    }
    if (avformat_find_stream_info(ifmt, nullptr) < 0) goto cleanup;

    for (unsigned i = 0; i < ifmt->nb_streams; i++) {
        auto t = ifmt->streams[i]->codecpar->codec_type;
        if (t == AVMEDIA_TYPE_VIDEO && videoIdx < 0) videoIdx = (int)i;
        else if (t == AVMEDIA_TYPE_AUDIO && audioIdx < 0) audioIdx = (int)i;
    }
    if (videoIdx < 0) goto cleanup;

    {
        AVCodecParameters* par = ifmt->streams[videoIdx]->codecpar;
        const AVCodec* c = avcodec_find_decoder(par->codec_id);
        if (!c) goto cleanup;
        vdec = avcodec_alloc_context3(c);
        avcodec_parameters_to_context(vdec, par);
        if (avcodec_open2(vdec, c, nullptr) < 0) goto cleanup;
    }

    if (avformat_alloc_output_context2(&ofmt, nullptr, nullptr, out.c_str()) < 0) goto cleanup;

    {
        const AVCodec* c = avcodec_find_encoder_by_name(codec_name.c_str());
        if (!c) {
            A4K_LOGE("encoder %s not found", codec_name.c_str());
            goto cleanup;
        }
        enc = avcodec_alloc_context3(c);
        enc->codec_type = AVMEDIA_TYPE_VIDEO;
        enc->codec_id = c->id;
        enc->pix_fmt = AV_PIX_FMT_YUV420P;
        enc->width = out_w;
        enc->height = out_h;

        AVRational fr = vdec->framerate;
        if (fr.num == 0) fr = av_guess_frame_rate(ifmt, ifmt->streams[videoIdx], nullptr);
        if (fr.num == 0 || fr.den == 0) fr = (AVRational){30, 1};
        enc->time_base = (AVRational){fr.den, fr.num};

        if (bit_rate > 0) {
            enc->bit_rate = bit_rate;
            enc->rc_max_rate = bit_rate;
            enc->rc_buffer_size = (int)(bit_rate * 2);
        } else if (!codec_name.empty() && codec_name.find("mediacodec") == std::string::npos) {
            av_opt_set_double(enc, "crf", 20.0, AV_OPT_SEARCH_CHILDREN);
        } else {
            enc->bit_rate = (int64_t)out_w * out_h * 30 / 12;  // 兜底
        }
        enc->gop_size = 30;
        enc->max_b_frames = 2;
        enc->thread_count = 0;

        if (avcodec_open2(enc, c, nullptr) < 0) {
            A4K_LOGE("encoder open failed");
            goto cleanup;
        }
    }

    {
        AVStream* vout = avformat_new_stream(ofmt, nullptr);
        if (!vout) goto cleanup;
        avcodec_parameters_from_context(vout->codecpar, enc);
        vout->time_base = enc->time_base;

        if (audioIdx >= 0) {
            AVStream* aout = avformat_new_stream(ofmt, nullptr);
            avcodec_parameters_copy(aout->codecpar, ifmt->streams[audioIdx]->codecpar);
            aout->codecpar->codec_tag = 0;
            aout->time_base = ifmt->streams[audioIdx]->time_base;
        }

        if (!(ofmt->oformat->flags & AVFMT_NOFILE)) {
            if (avio_open(&ofmt->pb, out.c_str(), AVIO_FLAG_WRITE) < 0) goto cleanup;
        }
        if (avformat_write_header(ofmt, nullptr) < 0) goto cleanup;
    }

    {
        a4k::Renderer renderer;
        std::string err;
        if (!renderer.init(vdec->width, vdec->height, out_w, out_h, shader_text, &err)) {
            A4K_LOGE("renderer init failed: %s", err.c_str());
            goto cleanup;
        }

        SwsHolder sws_in, sws_out;
        std::vector<uint8_t> rgba((size_t)vdec->width * vdec->height * 4);
        std::vector<uint8_t> outbuf((size_t)out_w * out_h * 4);

        pkt = av_packet_alloc();
        epkt = av_packet_alloc();
        frame = av_frame_alloc();
        enc_frame = av_frame_alloc();
        if (!pkt || !epkt || !frame || !enc_frame) goto cleanup;

        enc_frame->format = AV_PIX_FMT_YUV420P;
        enc_frame->width = out_w;
        enc_frame->height = out_h;
        if (av_frame_get_buffer(enc_frame, 32) < 0) goto cleanup;

        g_total.store((long)ifmt->streams[videoIdx]->nb_frames);

        auto drain_encoder = [&]() {
            while (avcodec_receive_packet(enc, epkt) == 0) {
                av_packet_rescale_ts(epkt, enc->time_base, ofmt->streams[0]->time_base);
                epkt->stream_index = 0;
                av_interleaved_write_frame(ofmt, epkt);
                av_packet_unref(epkt);
            }
        };

        auto handle_frame = [&](AVFrame* f) -> bool {
            if (!to_rgba(f, &sws_in, &rgba)) return false;
            if (!renderer.render(rgba.data(), outbuf.data(), &err)) {
                A4K_LOGE("render failed: %s", err.c_str());
                return false;
            }
            if (av_frame_make_writable(enc_frame) < 0) return false;
            if (!rgba_to_yuv420(outbuf.data(), out_w, out_h, &sws_out, enc_frame)) {
                return false;
            }
            enc_frame->pts = outPts++;
            if (avcodec_send_frame(enc, enc_frame) < 0) {
                A4K_LOGE("enc send frame failed");
                return false;
            }
            drain_encoder();
            g_frames.fetch_add(1);
            return true;
        };

        while (!g_abort.load()) {
            int rr = av_read_frame(ifmt, pkt);
            if (rr < 0) break;
            if (pkt->stream_index == videoIdx) {
                if (avcodec_send_packet(vdec, pkt) == 0) {
                    while (avcodec_receive_frame(vdec, frame) == 0) {
                        if (!handle_frame(frame)) {
                            av_packet_unref(pkt);
                            av_frame_unref(frame);
                            goto cleanup;
                        }
                        av_frame_unref(frame);
                    }
                }
            } else if (pkt->stream_index == audioIdx) {
                AVStream* aout = ofmt->streams[1];
                av_packet_rescale_ts(pkt, ifmt->streams[audioIdx]->time_base, aout->time_base);
                pkt->stream_index = aout->index;
                av_interleaved_write_frame(ofmt, pkt);
            }
            av_packet_unref(pkt);
        }

        if (g_abort.load()) {
            aborted = true;
        } else {
            avcodec_send_packet(vdec, nullptr);
            while (avcodec_receive_frame(vdec, frame) == 0) {
                if (!handle_frame(frame)) {
                    av_frame_unref(frame);
                    goto cleanup;
                }
                av_frame_unref(frame);
            }
            avcodec_send_frame(enc, nullptr);
            drain_encoder();
        }
    }

    av_write_trailer(ofmt);
    ret = aborted ? -2 : 0;

cleanup:
    if (frame) av_frame_free(&frame);
    if (enc_frame) av_frame_free(&enc_frame);
    if (pkt) av_packet_free(&pkt);
    if (epkt) av_packet_free(&epkt);
    if (enc) avcodec_free_context(&enc);
    if (vdec) avcodec_free_context(&vdec);
    if (ofmt) {
        if (ofmt->pb && !(ofmt->oformat->flags & AVFMT_NOFILE)) avio_closep(&ofmt->pb);
        avformat_free_context(ofmt);
    }
    if (ifmt) avformat_close_input(&ifmt);
    A4K_LOGI("video finished ret=%d frames=%ld", ret, g_frames.load());
    return ret;
}

// ----------------------------------------------------------------------- JNI

extern "C" JNIEXPORT jint JNICALL
Java_com_video2x_android_Video2XNative_processAnime4K(
    JNIEnv* env,
    jobject /*thiz*/,
    jstring in_path,
    jstring out_path,
    jstring shader_path,
    jint out_w,
    jint out_h,
    jstring codec,
    jlong bit_rate,
    jboolean is_image
) {
    const std::string in = jstr(env, in_path);
    const std::string out = jstr(env, out_path);
    const std::string shader_path_s = jstr(env, shader_path);
    const std::string codec_s = jstr(env, codec);

    g_abort.store(false);
    g_frames.store(0);
    g_total.store(0);

    if (in.empty() || out.empty() || shader_path_s.empty()) {
        A4K_LOGE("empty path");
        return -1;
    }

    FILE* f = std::fopen(shader_path_s.c_str(), "rb");
    if (!f) {
        A4K_LOGE("cannot open shader: %s", shader_path_s.c_str());
        return -1;
    }
    std::string shader_text;
    {
        char buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) shader_text.append(buf, n);
    }
    std::fclose(f);

    if (out_w <= 0 || out_h <= 0) {
        A4K_LOGE("bad output size %dx%d", (int)out_w, (int)out_h);
        return -1;
    }

    if (is_image) {
        return process_image(in, out, shader_text, out_w, out_h);
    }
    return process_video(
        in, out, shader_text, out_w, out_h,
        codec_s.empty() ? "libx264" : codec_s, bit_rate
    );
}
