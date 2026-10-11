package com.video2x.android

/** 与上游四种处理器一一对应 */
enum class Algo(
    val id: Int,
    val label: String,
    /** UI 是否提供「输出倍率」选项（Anime4K 用它自动推算目标尺寸） */
    val needsScale: Boolean,
    /** native 是否需要显式目标宽高（libplacebo 必须传，ncnn 由模型决定） */
    val needsDimensions: Boolean,
    /** 需要「插帧倍数」（RIFE） */
    val needsFrameMultiplier: Boolean,
) {
    ANIME4K(Video2XNative.ALGO_ANIME4K, "Anime4K（GLSL 着色器）", true, true, false),
    REALESRGAN(Video2XNative.ALGO_REALESRGAN, "Real-ESRGAN（ncnn）", true, false, false),
    REALCUGAN(Video2XNative.ALGO_REALCUGAN, "Real-CUGAN（ncnn）", true, false, false),
    RIFE(Video2XNative.ALGO_RIFE, "RIFE（插帧）", false, false, true),
}

/** 通用选项（编码器 / 着色器 / RIFE 模型） */
data class ModelOption(val id: String, val label: String)

/**
 * Real-ESRGAN 模型。
 * scales 必须与 models/realesrgan 下实际存在的文件一致：
 *   仅 realesr-animevideov3 提供 x2/x3/x4，其余模型只有 x4。
 */
data class EsrganModel(val id: String, val label: String, val scales: List<Int>)

/**
 * Real-CUGAN 的模型组合。
 * 上游构造的路径为 models/realcugan/<variant>/up<scale>x-<noise>.param，
 * 其中 variant / scale / noise 三者相互独立，且并非任意组合都存在，
 * 因此这里直接穷举实际存在的组合。
 */
data class CuganCombo(
    val variant: String,
    val scale: Int,
    val noise: Int,
    val label: String,
)

object Catalogs {

    /** 编码器（对应 EncoderConfig.codec） */
    val codecs = listOf(
        ModelOption("libx264", "H.264 / libx264（软编码，最稳）"),
        ModelOption("mpeg4", "MPEG-4（软编码，兼容性最好）"),
        ModelOption("h264_mediacodec", "H.264 / MediaCodec（硬件编码，挑设备）"),
        ModelOption("hevc_mediacodec", "H.265 / MediaCodec（硬件编码，挑设备）"),
    )

    /** Anime4K 着色器，对应 models/libplacebo 下的 .glsl */
    val anime4kShaders = listOf(
        ModelOption("anime4k-v4-a", "Anime4K v4 A（快速）"),
        ModelOption("anime4k-v4-b", "Anime4K v4 B（较好）"),
        ModelOption("anime4k-v4-c", "Anime4K v4 C（最好/较慢）"),
        ModelOption("anime4k-v4-a+a", "Anime4K v4 A+A"),
        ModelOption("anime4k-v4-b+b", "Anime4K v4 B+B"),
        ModelOption("anime4k-v4-c+a", "Anime4K v4 C+A"),
        ModelOption("anime4k-v4.1-gan", "Anime4K v4.1 GAN"),
    )

    // ---------------------------------------------------------- Real-ESRGAN

    val esrganModels = listOf(
        EsrganModel("realesr-animevideov3", "realesr-animevideov3（动画视频，推荐）", listOf(2, 3, 4)),
        EsrganModel("realesrgan-plus", "realesrgan-plus（通用，仅 x4）", listOf(4)),
        EsrganModel("realesrgan-plus-anime", "realesrgan-plus-anime（动画，仅 x4）", listOf(4)),
        EsrganModel("realesr-generalv3", "realesr-generalv3（通用，仅 x4）", listOf(4)),
    )

    // ----------------------------------------------------------- Real-CUGAN

    val cuganCombos = listOf(
        // models-se
        CuganCombo("models-se", 2, 0, "models-se · x2 · 无降噪"),
        CuganCombo("models-se", 2, -1, "models-se · x2 · 保守"),
        CuganCombo("models-se", 2, 1, "models-se · x2 · 降噪1x"),
        CuganCombo("models-se", 2, 2, "models-se · x2 · 降噪2x"),
        CuganCombo("models-se", 2, 3, "models-se · x2 · 降噪3x"),
        CuganCombo("models-se", 3, -1, "models-se · x3 · 保守"),
        CuganCombo("models-se", 3, 0, "models-se · x3 · 无降噪"),
        CuganCombo("models-se", 3, 3, "models-se · x3 · 降噪3x"),
        CuganCombo("models-se", 4, -1, "models-se · x4 · 保守"),
        CuganCombo("models-se", 4, 0, "models-se · x4 · 无降噪"),
        CuganCombo("models-se", 4, 3, "models-se · x4 · 降噪3x"),
        // models-pro
        CuganCombo("models-pro", 2, -1, "models-pro · x2 · 保守"),
        CuganCombo("models-pro", 2, 0, "models-pro · x2 · 无降噪"),
        CuganCombo("models-pro", 2, 3, "models-pro · x2 · 降噪3x"),
        CuganCombo("models-pro", 3, -1, "models-pro · x3 · 保守"),
        CuganCombo("models-pro", 3, 0, "models-pro · x3 · 无降噪"),
        CuganCombo("models-pro", 3, 3, "models-pro · x3 · 降噪3x"),
        // models-nose
        CuganCombo("models-nose", 2, 0, "models-nose · x2 · 无降噪"),
    )

    // ---------------------------------------------------------------- RIFE

    /** RIFE 模型（需自行放入 filesDir/models/rife，默认未打包进 APK） */
    val rifeModels = listOf(
        ModelOption("rife", "rife"),
        ModelOption("rife-anime", "rife-anime"),
        ModelOption("rife-v2", "rife-v2"),
        ModelOption("rife-HD", "rife-HD"),
        ModelOption("rife-UHD", "rife-UHD"),
    )
    val frameMultipliers = listOf(2, 4, 8)
}
