#include "a4k_plan.h"

#include <algorithm>

namespace a4k {

bool build_plan(
    const Shader& sh, TexInfo native, TexInfo output, Plan* out, std::string* err
) {
    out->passes.clear();
    out->native = native;
    out->output = output;

    TexMap texs;
    texs["NATIVE"] = native;
    texs["MAIN"] = native;
    texs["OUTPUT"] = output;

    Stage last_stage = Stage::Main;
    TexInfo stage_in = native;

    for (const auto& p : sh.passes) {
        if (p.stage != last_stage) {
            // 新阶段的输入恒为上一阶段结束时的 MAIN
            stage_in = texs["MAIN"];
            last_stage = p.stage;
        }

        if (!eval_cond(p.when, texs)) continue;

        bool ok = true;
        int w = 0, h = 0;
        if (p.width.empty()) {
            w = stage_in.w;
        } else {
            w = (int)eval_size(p.width, texs, &ok);
        }
        if (p.height.empty()) {
            h = stage_in.h;
        } else {
            h = (int)eval_size(p.height, texs, &ok);
        }
        if (!ok || w <= 0 || h <= 0) {
            if (err) *err = "bad size for pass: " + p.desc;
            return false;
        }

        PlannedPass pp;
        pp.pass = &p;
        pp.stage_in = stage_in;
        pp.out_w = w;
        pp.out_h = h;

        for (const auto& b : p.binds) {
            if (b == "HOOKED") {
                pp.bound.push_back({b, stage_in});
                continue;
            }
            auto it = texs.find(b);
            if (it == texs.end()) {
                if (err) *err = "unresolved BIND '" + b + "' in pass: " + p.desc;
                return false;
            }
            pp.bound.push_back({b, it->second});
        }

        out->passes.push_back(std::move(pp));

        if (!p.save.empty()) {
            texs[p.save] = {w, h};
        } else {
            texs["MAIN"] = {w, h};
        }
    }

    if (out->passes.empty()) {
        if (err) *err = "plan is empty";
        return false;
    }
    return true;
}

}  // namespace a4k
