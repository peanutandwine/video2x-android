// 把解析出的 pass 链展开成可执行的渲染计划（尺寸、绑定、跳过条件一次算清）。
#pragma once

#include "a4k_shader.h"

#include <string>
#include <vector>

namespace a4k {

struct PlannedPass {
    const Pass* pass = nullptr;
    std::vector<BoundTex> bound;
    TexInfo stage_in;
    int out_w = 0;
    int out_h = 0;
};

struct Plan {
    std::vector<PlannedPass> passes;
    TexInfo native{0, 0};
    TexInfo output{0, 0};
};

/**
 * @param native  输入尺寸（NATIVE / 初始 MAIN）
 * @param output  目标输出尺寸（OUTPUT）
 */
bool build_plan(
    const Shader& sh, TexInfo native, TexInfo output, Plan* out, std::string* err
);

}  // namespace a4k
