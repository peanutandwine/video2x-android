// -----------------------------------------------------------------------------
// mpv user shader（Anime4K .glsl）解析 + GLSL 生成
//
// 只实现 Anime4K 用到的子集：
//   //!DESC / //!HOOK / //!BIND / //!SAVE / //!WIDTH / //!HEIGHT
//   //!COMPONENTS / //!WHEN
// 指令语义与 libplacebo 的 custom_mpv.c 一致（RPN 表达式、stage 排序）。
// -----------------------------------------------------------------------------
#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace a4k {

struct TexInfo {
    int w = 0;
    int h = 0;
};

using TexMap = std::map<std::string, TexInfo>;

enum class Stage {
    Main = 0,
    PreKernel = 1,
    PostKernel = 2,
    Output = 3,
};

struct Pass {
    std::string desc;
    Stage stage = Stage::Main;
    std::vector<std::string> binds;
    std::string save;
    std::vector<std::string> width;   // RPN
    std::vector<std::string> height;  // RPN
    std::vector<std::string> when;    // RPN（空表示无条件）
    int components = 4;
    std::string body;
};

struct BoundTex {
    std::string name;
    TexInfo info;
};

struct Shader {
    std::vector<Pass> passes;  // 已按 stage 稳定排序
};

/** 解析 mpv user shader 文本。失败时返回 false 并填充 err。 */
bool parse_mpv_shader(const std::string& text, Shader* out, std::string* err);

/** 求值 RPN 尺寸表达式；变量形如 MAIN.w / OUTPUT.h / conv1_tf.w */
float eval_size(
    const std::vector<std::string>& toks, const TexMap& texs, bool* ok
);

/** 求值 RPN 条件表达式（结果非 0 为真）。 */
bool eval_cond(const std::vector<std::string>& toks, const TexMap& texs);

/**
 * 生成某个 pass 的完整 fragment shader。
 *
 * @param p          pass
 * @param bound      bind 名 -> 纹理尺寸（顺序即 binding 序号）
 * @param stage_in   当前 hook 阶段的输入纹理尺寸
 * @param out_w/h    该 pass 的输出尺寸
 */
std::string build_glsl(
    const Pass& p,
    const std::vector<BoundTex>& bound,
    TexInfo stage_in,
    int out_w,
    int out_h
);

/** 顶点着色器：全屏三角形 */
extern const char* kFullscreenVert;

}  // namespace a4k
