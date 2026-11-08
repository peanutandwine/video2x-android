#include "a4k_vk.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#define A4K_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "Anime4K-VK", __VA_ARGS__)
#define A4K_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "Anime4K-VK", __VA_ARGS__)
#else
#include <cstdarg>
static void a4k_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
}
#define A4K_LOGI(...) a4k_log(__VA_ARGS__)
#define A4K_LOGE(...) a4k_log(__VA_ARGS__)
#endif

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

namespace a4k {

namespace {

constexpr VkFormat kFmt = VK_FORMAT_R8G8B8A8_UNORM;

struct Tex {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    int w = 0;
    int h = 0;
};

struct PassRes {
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    int nbind = 0;
};

bool compile_glsl(const std::string& src, bool frag, std::vector<uint32_t>* spirv, std::string* err) {
    EShLanguage stage = frag ? EShLangFragment : EShLangVertex;
    glslang::TShader sh(stage);
    const char* s = src.c_str();
    sh.setStrings(&s, 1);
    sh.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 110);
    sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);

    if (!sh.parse(GetDefaultResources(), 450, false, EShMsgDefault)) {
        if (err) *err = std::string("glsl parse: ") + sh.getInfoLog();
        return false;
    }
    glslang::TProgram prog;
    prog.addShader(&sh);
    if (!prog.link(EShMsgDefault)) {
        if (err) *err = std::string("glsl link: ") + prog.getInfoLog();
        return false;
    }
    glslang::GlslangToSpv(*prog.getIntermediate(stage), *spirv);
    return true;
}

// 简易双线性缩放，仅用于「shader 输出尺寸 != 目标尺寸」的兜底
void resize_bilinear(
    const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh
) {
    for (int y = 0; y < dh; y++) {
        float fy = ((y + 0.5f) * sh / dh) - 0.5f;
        int y0 = (int)std::floor(fy);
        float ty = fy - y0;
        int y1 = std::min(y0 + 1, sh - 1);
        y0 = std::max(0, std::min(y0, sh - 1));
        for (int x = 0; x < dw; x++) {
            float fx = ((x + 0.5f) * sw / dw) - 0.5f;
            int x0 = (int)std::floor(fx);
            float tx = fx - x0;
            int x1 = std::min(x0 + 1, sw - 1);
            x0 = std::max(0, std::min(x0, sw - 1));
            const uint8_t* p00 = src + ((size_t)y0 * sw + x0) * 4;
            const uint8_t* p01 = src + ((size_t)y0 * sw + x1) * 4;
            const uint8_t* p10 = src + ((size_t)y1 * sw + x0) * 4;
            const uint8_t* p11 = src + ((size_t)y1 * sw + x1) * 4;
            uint8_t* o = dst + ((size_t)y * dw + x) * 4;
            for (int c = 0; c < 4; c++) {
                float a = p00[c] * (1 - tx) + p01[c] * tx;
                float b = p10[c] * (1 - tx) + p11[c] * tx;
                o[c] = (uint8_t)std::clamp((int)(a * (1 - ty) + b * ty + 0.5f), 0, 255);
            }
        }
    }
}

}  // namespace

struct Renderer::Impl {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkQueue queue = VK_NULL_HANDLE;

    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool = VK_NULL_HANDLE;

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    size_t staging_size = 0;

    std::vector<PassRes> passes;
    std::map<std::string, Tex*> live;
    std::map<std::pair<int, int>, std::vector<Tex*>> pool;
    std::vector<Tex*> owned;

    bool inited = false;

    uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(phys, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
        }
        return UINT32_MAX;
    }

    Tex* new_tex(int w, int h, std::string* err) {
        Tex* t = new Tex();
        t->w = w;
        t->h = h;

        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = kFmt;
        ci.extent = {(uint32_t)w, (uint32_t)h, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(dev, &ci, nullptr, &t->image) != VK_SUCCESS) {
            delete t;
            if (err) *err = "vkCreateImage failed";
            return nullptr;
        }
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(dev, t->image, &req);
        uint32_t mt = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt == UINT32_MAX) {
            vkDestroyImage(dev, t->image, nullptr);
            delete t;
            if (err) *err = "no device-local memory";
            return nullptr;
        }
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = mt;
        if (vkAllocateMemory(dev, &ai, nullptr, &t->mem) != VK_SUCCESS) {
            vkDestroyImage(dev, t->image, nullptr);
            delete t;
            if (err) *err = "vkAllocateMemory(image) failed";
            return nullptr;
        }
        vkBindImageMemory(dev, t->image, t->mem, 0);

        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = t->image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = kFmt;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(dev, &vi, nullptr, &t->view);

        VkFramebufferCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass = render_pass;
        fi.attachmentCount = 1;
        fi.pAttachments = &t->view;
        fi.width = w;
        fi.height = h;
        fi.layers = 1;
        vkCreateFramebuffer(dev, &fi, nullptr, &t->fb);

        owned.push_back(t);
        return t;
    }

    void free_tex(Tex* t) {
        if (!t) return;
        if (t->fb) vkDestroyFramebuffer(dev, t->fb, nullptr);
        if (t->view) vkDestroyImageView(dev, t->view, nullptr);
        if (t->image) vkDestroyImage(dev, t->image, nullptr);
        if (t->mem) vkFreeMemory(dev, t->mem, nullptr);
        owned.erase(std::remove(owned.begin(), owned.end(), t), owned.end());
        delete t;
    }

    Tex* acquire(int w, int h, std::string* err) {
        auto key = std::make_pair(w, h);
        auto it = pool.find(key);
        if (it != pool.end() && !it->second.empty()) {
            Tex* t = it->second.back();
            it->second.pop_back();
            return t;
        }
        return new_tex(w, h, err);
    }

    void recycle(Tex* t) {
        if (!t) return;
        pool[{t->w, t->h}].push_back(t);
    }

    void barrier(
        VkCommandBuffer c, VkImage img, VkAccessFlags src, VkAccessFlags dst,
        VkImageLayout from, VkImageLayout to
    ) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = src;
        b.dstAccessMask = dst;
        vkCmdPipelineBarrier(
            c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b
        );
    }

    bool ensure_staging(size_t need, std::string* err) {
        if (staging_size >= need) return true;
        if (staging) vkDestroyBuffer(dev, staging, nullptr);
        if (staging_mem) vkFreeMemory(dev, staging_mem, nullptr);
        staging = VK_NULL_HANDLE;
        staging_mem = VK_NULL_HANDLE;
        staging_size = 0;

        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = need;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(dev, &bi, nullptr, &staging) != VK_SUCCESS) {
            if (err) *err = "vkCreateBuffer(staging) failed";
            return false;
        }
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(dev, staging, &req);
        uint32_t mt = mem_type(
            req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
        );
        if (mt == UINT32_MAX) {
            if (err) *err = "no host-visible memory";
            return false;
        }
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = mt;
        vkAllocateMemory(dev, &ai, nullptr, &staging_mem);
        vkBindBufferMemory(dev, staging, staging_mem, 0);
        staging_size = need;
        return true;
    }

    bool create_device(std::string* err) {
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "video2x-anime4k";
        app.apiVersion = VK_API_VERSION_1_1;

        VkInstanceCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ici.pApplicationInfo = &app;
        if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
            if (err) *err = "vkCreateInstance failed";
            return false;
        }

        uint32_t n = 0;
        vkEnumeratePhysicalDevices(instance, &n, nullptr);
        if (!n) {
            if (err) *err = "no vulkan device";
            return false;
        }
        std::vector<VkPhysicalDevice> devs(n);
        vkEnumeratePhysicalDevices(instance, &n, devs.data());

        for (auto d : devs) {
            uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> qs(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qs.data());
            for (uint32_t i = 0; i < qn; i++) {
                if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                    phys = d;
                    queue_family = i;
                    break;
                }
            }
            if (phys) break;
        }
        if (!phys) {
            if (err) *err = "no graphics queue family";
            return false;
        }

        VkPhysicalDeviceProperties prop{};
        vkGetPhysicalDeviceProperties(phys, &prop);
        A4K_LOGI(
            "device: %s (Vulkan %u.%u.%u)", prop.deviceName,
            VK_VERSION_MAJOR(prop.apiVersion), VK_VERSION_MINOR(prop.apiVersion),
            VK_VERSION_PATCH(prop.apiVersion)
        );

        float prio = 1.0f;
        VkDeviceQueueCreateInfo qi{};
        qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = queue_family;
        qi.queueCount = 1;
        qi.pQueuePriorities = &prio;

        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qi;
        if (vkCreateDevice(phys, &dci, nullptr, &dev) != VK_SUCCESS) {
            if (err) *err = "vkCreateDevice failed";
            return false;
        }
        vkGetDeviceQueue(dev, queue_family, 0, &queue);

        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = queue_family;
        vkCreateCommandPool(dev, &pci, nullptr, &cmd_pool);

        VkCommandBufferAllocateInfo cbi{};
        cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbi.commandPool = cmd_pool;
        cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbi.commandBufferCount = 1;
        vkAllocateCommandBuffers(dev, &cbi, &cmd);

        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 0.0f;
        vkCreateSampler(dev, &si, nullptr, &sampler);

        VkAttachmentDescription att{};
        att.format = kFmt;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
        att.finalLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_GENERAL};
        VkSubpassDescription sp{};
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments = &ref;

        VkSubpassDependency dep[2]{};
        dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dep[0].dstSubpass = 0;
        dep[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_TRANSFER_BIT;
        dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep[1].srcSubpass = 0;
        dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep[1].dstStageMask =
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;

        VkRenderPassCreateInfo rci{};
        rci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rci.attachmentCount = 1;
        rci.pAttachments = &att;
        rci.subpassCount = 1;
        rci.pSubpasses = &sp;
        rci.dependencyCount = 2;
        rci.pDependencies = dep;
        if (vkCreateRenderPass(dev, &rci, nullptr, &render_pass) != VK_SUCCESS) {
            if (err) *err = "vkCreateRenderPass failed";
            return false;
        }

        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024};
        VkDescriptorPoolCreateInfo dpci{};
        dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets = 256;
        dpci.poolSizeCount = 1;
        dpci.pPoolSizes = &ps;
        if (vkCreateDescriptorPool(dev, &dpci, nullptr, &desc_pool) != VK_SUCCESS) {
            if (err) *err = "vkCreateDescriptorPool failed";
            return false;
        }
        return true;
    }
};

// ---------------------------------------------------------------- lifetime

Renderer::Renderer() : impl_(new Impl()) {}

Renderer::~Renderer() { destroy(); }

void Renderer::destroy() {
    Impl* d = impl_.get();
    if (!d) return;

    if (d->dev) vkDeviceWaitIdle(d->dev);

    for (auto* t : d->owned) {
        if (t->fb) vkDestroyFramebuffer(d->dev, t->fb, nullptr);
        if (t->view) vkDestroyImageView(d->dev, t->view, nullptr);
        if (t->image) vkDestroyImage(d->dev, t->image, nullptr);
        if (t->mem) vkFreeMemory(d->dev, t->mem, nullptr);
        delete t;
    }
    d->owned.clear();
    d->pool.clear();
    d->live.clear();

    for (auto& p : d->passes) {
        if (p.pipeline) vkDestroyPipeline(d->dev, p.pipeline, nullptr);
        if (p.pipe_layout) vkDestroyPipelineLayout(d->dev, p.pipe_layout, nullptr);
        if (p.layout) vkDestroyDescriptorSetLayout(d->dev, p.layout, nullptr);
    }
    d->passes.clear();

    if (d->staging) vkDestroyBuffer(d->dev, d->staging, nullptr);
    if (d->staging_mem) vkFreeMemory(d->dev, d->staging_mem, nullptr);
    if (d->desc_pool) vkDestroyDescriptorPool(d->dev, d->desc_pool, nullptr);
    if (d->render_pass) vkDestroyRenderPass(d->dev, d->render_pass, nullptr);
    if (d->sampler) vkDestroySampler(d->dev, d->sampler, nullptr);
    if (d->cmd_pool) vkDestroyCommandPool(d->dev, d->cmd_pool, nullptr);
    if (d->dev) vkDestroyDevice(d->dev, nullptr);
    if (d->instance) vkDestroyInstance(d->instance, nullptr);

    d->staging = VK_NULL_HANDLE;
    d->staging_mem = VK_NULL_HANDLE;
    d->desc_pool = VK_NULL_HANDLE;
    d->render_pass = VK_NULL_HANDLE;
    d->sampler = VK_NULL_HANDLE;
    d->cmd_pool = VK_NULL_HANDLE;
    d->dev = VK_NULL_HANDLE;
    d->instance = VK_NULL_HANDLE;
    d->phys = VK_NULL_HANDLE;
    d->queue = VK_NULL_HANDLE;
    d->cmd = VK_NULL_HANDLE;
    d->inited = false;
}

bool Renderer::probe(std::string* info) {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) return false;

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    bool ok = n > 0;
    if (ok && info) {
        std::vector<VkPhysicalDevice> devs(n);
        vkEnumeratePhysicalDevices(inst, &n, devs.data());
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(devs[0], &p);
        *info = std::string(p.deviceName) + " (Vulkan " +
                std::to_string(VK_VERSION_MAJOR(p.apiVersion)) + "." +
                std::to_string(VK_VERSION_MINOR(p.apiVersion)) + ")";
    }
    vkDestroyInstance(inst, nullptr);
    return ok;
}

bool Renderer::init(
    int in_w, int in_h, int out_w, int out_h, const std::string& shader_text, std::string* err
) {
    Impl* d = impl_.get();

    if (!parse_mpv_shader(shader_text, &shader_, err)) return false;
    if (!build_plan(shader_, {in_w, in_h}, {out_w, out_h}, &plan_, err)) return false;

    static std::once_flag glslang_once;
    static bool glslang_ok = false;
    std::call_once(glslang_once, [] { glslang_ok = glslang::InitializeProcess(); });
    if (!glslang_ok) {
        if (err) *err = "glslang init failed";
        return false;
    }
    if (!d->create_device(err)) return false;

    std::vector<uint32_t> vert_spv;
    if (!compile_glsl(kFullscreenVert, false, &vert_spv, err)) return false;

    VkShaderModule vert_mod = VK_NULL_HANDLE;
    {
        VkShaderModuleCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = vert_spv.size() * 4;
        ci.pCode = vert_spv.data();
        if (vkCreateShaderModule(d->dev, &ci, nullptr, &vert_mod) != VK_SUCCESS) {
            if (err) *err = "vkCreateShaderModule(vert) failed";
            return false;
        }
    }

    d->passes.resize(plan_.passes.size());
    for (size_t i = 0; i < plan_.passes.size(); i++) {
        const auto& pp = plan_.passes[i];
        std::string glsl = build_glsl(*pp.pass, pp.bound, pp.stage_in, pp.out_w, pp.out_h);
        std::vector<uint32_t> spv;
        std::string gerr;
        if (!compile_glsl(glsl, true, &spv, &gerr)) {
            if (err) {
                *err = "pass " + std::to_string(i) + " (" + pp.pass->desc + "): " + gerr;
            }
            vkDestroyShaderModule(d->dev, vert_mod, nullptr);
            return false;
        }

        VkShaderModule frag_mod = VK_NULL_HANDLE;
        VkShaderModuleCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = spv.size() * 4;
        ci.pCode = spv.data();
        vkCreateShaderModule(d->dev, &ci, nullptr, &frag_mod);

        PassRes& pr = d->passes[i];
        pr.nbind = (int)pp.bound.size();

        std::vector<VkDescriptorSetLayoutBinding> binds(pr.nbind);
        for (int b = 0; b < pr.nbind; b++) {
            binds[b].binding = b;
            binds[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            binds[b].descriptorCount = 1;
            binds[b].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = (uint32_t)pr.nbind;
        lci.pBindings = binds.data();
        vkCreateDescriptorSetLayout(d->dev, &lci, nullptr, &pr.layout);

        VkPipelineLayoutCreateInfo pli{};
        pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &pr.layout;
        vkCreatePipelineLayout(d->dev, &pli, nullptr, &pr.pipe_layout);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert_mod;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag_mod;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vp{};
        vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = 1;
        vp.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState cb{};
        cb.blendEnable = VK_FALSE;
        cb.colorWriteMask = 0xF;

        VkPipelineColorBlendStateCreateInfo cbs{};
        cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cbs.attachmentCount = 1;
        cbs.pAttachments = &cb;

        VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        ds.dynamicStateCount = 2;
        ds.pDynamicStates = dyn;

        VkGraphicsPipelineCreateInfo gp{};
        gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp.stageCount = 2;
        gp.pStages = stages;
        gp.pVertexInputState = &vi;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vp;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms;
        gp.pColorBlendState = &cbs;
        gp.pDynamicState = &ds;
        gp.layout = pr.pipe_layout;
        gp.renderPass = d->render_pass;
        gp.subpass = 0;

        VkResult r =
            vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gp, nullptr, &pr.pipeline);
        vkDestroyShaderModule(d->dev, frag_mod, nullptr);
        if (r != VK_SUCCESS) {
            if (err) *err = "vkCreateGraphicsPipelines failed: " + std::to_string((int)r);
            vkDestroyShaderModule(d->dev, vert_mod, nullptr);
            return false;
        }
    }
    vkDestroyShaderModule(d->dev, vert_mod, nullptr);

    d->inited = true;
    A4K_LOGI("init ok: %dx%d -> %dx%d, %zu passes", in_w, in_h, out_w, out_h,
             plan_.passes.size());
    return true;
}

// ---------------------------------------------------------------- render

bool Renderer::render(const uint8_t* in_rgba, uint8_t* out_rgba, std::string* err) {
    Impl* d = impl_.get();
    if (!d->inited) {
        if (err) *err = "not initialized";
        return false;
    }

    const int W = plan_.native.w, H = plan_.native.h;
    const size_t in_bytes = (size_t)W * H * 4;
    const size_t out_bytes = (size_t)plan_.output.w * plan_.output.h * 4;
    const size_t need = std::max(in_bytes, out_bytes);

    auto fail = [&](const std::string& m, bool cmd_open) {
        if (err) *err = m;
        if (cmd_open) vkEndCommandBuffer(d->cmd);
        return false;
    };

    if (!d->ensure_staging(need, err)) return false;

    // 重置本帧状态
    for (auto& kv : d->live) d->recycle(kv.second);
    d->live.clear();
    vkResetDescriptorPool(d->dev, d->desc_pool, 0);

    vkResetCommandBuffer(d->cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(d->cmd, &bi);

    // ---- 上传 ----
    {
        void* mapped = nullptr;
        if (vkMapMemory(d->dev, d->staging_mem, 0, in_bytes, 0, &mapped) != VK_SUCCESS) {
            return fail("vkMapMemory(in) failed", true);
        }
        std::memcpy(mapped, in_rgba, in_bytes);
        vkUnmapMemory(d->dev, d->staging_mem);
    }

    Tex* main_tex = d->acquire(W, H, err);
    if (!main_tex) return fail("acquire main failed", true);

    d->barrier(
        d->cmd, main_tex->image, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL
    );
    {
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {(uint32_t)W, (uint32_t)H, 1};
        vkCmdCopyBufferToImage(
            d->cmd, d->staging, main_tex->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region
        );
    }
    d->barrier(
        d->cmd, main_tex->image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL
    );

    for (size_t i = 0; i < plan_.passes.size(); i++) {
        const auto& pp = plan_.passes[i];
        PassRes& pr = d->passes[i];

        std::vector<Tex*> srcs;
        srcs.reserve(pp.bound.size());
        for (const auto& b : pp.bound) {
            Tex* t = nullptr;
            if (b.name == "HOOKED") {
                t = main_tex;
            } else {
                auto it = d->live.find(b.name);
                if (it != d->live.end()) t = it->second;
            }
            if (!t) {
                return fail(
                    "missing texture '" + b.name + "' at pass " + std::to_string(i), true
                );
            }
            srcs.push_back(t);
        }

        std::string target_name = pp.pass->save.empty() ? "MAIN" : pp.pass->save;

        // 目标纹理：不能与任何输入复用同一张图（读写冲突），池里取不到就新建
        Tex* target = nullptr;
        {
            auto key = std::make_pair(pp.out_w, pp.out_h);
            auto it = d->pool.find(key);
            if (it != d->pool.end() && !it->second.empty()) {
                target = it->second.back();
                it->second.pop_back();
            }
            if (!target) {
                target = d->new_tex(pp.out_w, pp.out_h, err);
                if (!target) return fail("alloc target failed at pass " + std::to_string(i), true);
            }
        }

        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = d->desc_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &pr.layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(d->dev, &ai, &set) != VK_SUCCESS) {
            return fail("vkAllocateDescriptorSets failed", true);
        }

        std::vector<VkDescriptorImageInfo> infos(srcs.size());
        std::vector<VkWriteDescriptorSet> writes(srcs.size());
        for (size_t b = 0; b < srcs.size(); b++) {
            infos[b].sampler = d->sampler;
            infos[b].imageView = srcs[b]->view;
            infos[b].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet = set;
            writes[b].dstBinding = (uint32_t)b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[b].pImageInfo = &infos[b];
        }
        if (!writes.empty()) {
            vkUpdateDescriptorSets(
                d->dev, (uint32_t)writes.size(), writes.data(), 0, nullptr
            );
        }

        VkClearValue clear{};
        clear.color = {{0.f, 0.f, 0.f, 1.f}};
        VkRenderPassBeginInfo rbi{};
        rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rbi.renderPass = d->render_pass;
        rbi.framebuffer = target->fb;
        rbi.renderArea = {{0, 0}, {(uint32_t)pp.out_w, (uint32_t)pp.out_h}};
        rbi.clearValueCount = 1;
        rbi.pClearValues = &clear;

        vkCmdBeginRenderPass(d->cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(d->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pr.pipeline);
        VkViewport vpt{0.f, 0.f, (float)pp.out_w, (float)pp.out_h, 0.f, 1.f};
        VkRect2D sc{{0, 0}, {(uint32_t)pp.out_w, (uint32_t)pp.out_h}};
        vkCmdSetViewport(d->cmd, 0, 1, &vpt);
        vkCmdSetScissor(d->cmd, 0, 1, &sc);
        vkCmdBindDescriptorSets(
            d->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pr.pipe_layout, 0, 1, &set, 0, nullptr
        );
        vkCmdDraw(d->cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(d->cmd);

        d->barrier(
            d->cmd, target->image, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL
        );

        // 旧 MAIN 被覆盖 → 回收
        if (target_name == "MAIN" && main_tex != target) {
            d->recycle(main_tex);
            main_tex = target;
        } else if (target_name != "MAIN") {
            auto it = d->live.find(target_name);
            if (it != d->live.end()) {
                d->recycle(it->second);
            }
        }
        d->live[target_name] = target;
    }

    // ---- 下载 ----
    const int rw = main_tex->w, rh = main_tex->h;
    const bool need_scale = (rw != plan_.output.w || rh != plan_.output.h);

    d->barrier(
        d->cmd, main_tex->image, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL
    );
    {
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {(uint32_t)rw, (uint32_t)rh, 1};
        vkCmdCopyImageToBuffer(
            d->cmd, main_tex->image, VK_IMAGE_LAYOUT_GENERAL, d->staging, 1, &region
        );
    }
    vkEndCommandBuffer(d->cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &d->cmd;
    if (vkQueueSubmit(d->queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) {
        if (err) *err = "vkQueueSubmit failed";
        return false;
    }
    vkQueueWaitIdle(d->queue);

    const size_t raw_bytes = (size_t)rw * rh * 4;
    void* mapped = nullptr;
    if (vkMapMemory(d->dev, d->staging_mem, 0, raw_bytes, 0, &mapped) != VK_SUCCESS) {
        if (err) *err = "vkMapMemory(out) failed";
        return false;
    }
    if (need_scale) {
        resize_bilinear(
            (const uint8_t*)mapped, rw, rh, out_rgba, plan_.output.w, plan_.output.h
        );
    } else {
        std::memcpy(out_rgba, mapped, out_bytes);
    }
    vkUnmapMemory(d->dev, d->staging_mem);
    return true;
}

}  // namespace a4k
