// -----------------------------------------------------------------------------
// Anime4K Vulkan 1.1 渲染器
//
// 背景：libplacebo（ffmpeg 的 libplacebo 滤镜）硬性要求 Vulkan 1.2
// （PL_VK_MIN_VERSION = VK_API_VERSION_1_2，且 required_vk12 里
//  hostQueryReset / timelineSemaphore 都是必需特性）。骁龙 865 的 Adreno 650
// 驱动只暴露 Vulkan 1.1，因此整条 libplacebo 链路在该机型上无法初始化。
//
// 这里用 Vulkan 1.1 的基础能力重写 Anime4K 的执行：解析 mpv user shader
//（//!HOOK / //!BIND / //!SAVE / //!WIDTH / //!HEIGHT / //!WHEN），把每个 pass
// 编译成 fragment shader，用全屏三角形离屏渲染到 RGBA8 纹理。
//
// 只依赖 Vulkan 1.0/1.1 的 core 功能，不碰 timeline semaphore / dynamic rendering。
// -----------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "a4k_plan.h"
#include "a4k_shader.h"

namespace a4k {

class Renderer {
public:
    Renderer();
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    /** 初始化：解析 shader、建 Vulkan 设备、编译所有 pipeline。 */
    bool init(
        int in_w,
        int in_h,
        int out_w,
        int out_h,
        const std::string& shader_text,
        std::string* err
    );

    /** 处理一帧。in/out 均为 RGBA8，行距 = width*4。 */
    bool render(const uint8_t* in_rgba, uint8_t* out_rgba, std::string* err);

    int out_width() const { return plan_.output.w; }
    int out_height() const { return plan_.output.h; }

    void destroy();

    /** 探测当前环境是否能创建可用的 Vulkan 1.1 设备（不需要 shader）。 */
    static bool probe(std::string* info);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Plan plan_;
    Shader shader_;   // Plan.passes 里存的是 Pass*，必须由 Renderer 持有生命周期
};

}  // namespace a4k
