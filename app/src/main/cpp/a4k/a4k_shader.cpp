#include "a4k_shader.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace a4k {

namespace {

bool starts_with(const std::string& s, const char* p) {
    return s.rfind(p, 0) == 0;
}

std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream is(s);
    std::string w;
    while (is >> w) out.push_back(w);
    return out;
}

bool is_number(const std::string& s) {
    if (s.empty()) return false;
    char* end = nullptr;
    std::strtod(s.c_str(), &end);
    return end && *end == '\0';
}

Stage stage_of(const std::string& s) {
    if (s == "MAIN" || s == "MAINPRESUB" || s == "RGB") return Stage::Main;
    if (s == "PREKERNEL") return Stage::PreKernel;
    if (s == "POSTKERNEL") return Stage::PostKernel;
    if (s == "OUTPUT" || s == "PREOUTPUT" || s == "SCALED") return Stage::Output;
    return Stage::Main;
}

void flush(Shader* sh, Pass* cur, bool* has) {
    if (!*has) return;
    // 没有任何 body 的 pass 没有意义（例如只有指令的残块）
    if (!cur->body.empty()) sh->passes.push_back(*cur);
    *cur = Pass{};
    *has = false;
}

bool lookup(const TexMap& texs, const std::string& name, TexInfo* out) {
    auto it = texs.find(name);
    if (it == texs.end()) return false;
    *out = it->second;
    return true;
}

}  // namespace

bool parse_mpv_shader(const std::string& text, Shader* out, std::string* err) {
    out->passes.clear();

    Pass cur;
    bool has_cur = false;
    bool started = false;
    std::string pending_desc;

    std::istringstream is(text);
    std::string line;

    while (std::getline(is, line)) {
        // 去掉行尾 \r
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (starts_with(line, "//!")) {
            std::string body = line.substr(3);
            auto words = split_ws(body);
            if (words.empty()) continue;
            const std::string& cmd = words[0];

            if (cmd == "DESC") {
                pending_desc = body.size() > 5 ? body.substr(5) : "";
                // 去掉前导空格
                while (!pending_desc.empty() && pending_desc.front() == ' ')
                    pending_desc.erase(pending_desc.begin());
                started = true;
                continue;
            }

            if (cmd == "HOOK") {
                flush(out, &cur, &has_cur);
                cur = Pass{};
                cur.desc = pending_desc;
                pending_desc.clear();
                std::string stage = words.size() > 1 ? words[1] : "MAIN";
                cur.stage = stage_of(stage);
                has_cur = true;
                started = true;
                continue;
            }

            if (!started) continue;  // 文件头部的杂项指令
            if (!has_cur) {
                // 没有 HOOK 的指令块（有些 shader 省略 HOOK，默认 MAIN）
                cur.desc = pending_desc;
                pending_desc.clear();
                has_cur = true;
            }

            if (cmd == "BIND") {
                if (words.size() > 1) cur.binds.push_back(words[1]);
            } else if (cmd == "SAVE") {
                if (words.size() > 1) cur.save = words[1];
            } else if (cmd == "COMPONENTS") {
                if (words.size() > 1) cur.components = std::atoi(words[1].c_str());
            } else if (cmd == "WIDTH") {
                cur.width.assign(words.begin() + 1, words.end());
            } else if (cmd == "HEIGHT") {
                cur.height.assign(words.begin() + 1, words.end());
            } else if (cmd == "WHEN") {
                cur.when.assign(words.begin() + 1, words.end());
            }
            // 其余指令（OFFSET / COMPUTE / BLOCK 等）Anime4K 未使用，忽略
            continue;
        }

        if (!started || !has_cur) continue;  // 丢弃 license 注释块等

        cur.body += line;
        cur.body += '\n';
    }
    flush(out, &cur, &has_cur);

    if (out->passes.empty()) {
        if (err) *err = "no shader pass found";
        return false;
    }

    // stage 优先、文件顺序其次（stable_sort 保证后者）
    std::stable_sort(
        out->passes.begin(), out->passes.end(),
        [](const Pass& a, const Pass& b) {
            return static_cast<int>(a.stage) < static_cast<int>(b.stage);
        }
    );
    return true;
}

// --------------------------------------------------------------------- RPN

namespace {

float eval_rpn_impl(const std::vector<std::string>& toks, const TexMap& texs, bool* ok) {
    std::vector<float> st;
    *ok = true;

    for (const auto& raw : toks) {
        std::string tok = raw;
        if (tok.empty()) continue;

        auto ends = [&](const char* suf) {
            size_t n = std::char_traits<char>::length(suf);
            return tok.size() > n && tok.compare(tok.size() - n, n, suf) == 0;
        };

        if (ends(".w") || ends(".width") || ends(".h") || ends(".height")) {
            bool want_w = ends(".w") || ends(".width");
            size_t cut = want_w ? (ends(".w") ? 2 : 6) : (ends(".h") ? 2 : 7);
            std::string name = tok.substr(0, tok.size() - cut);
            TexInfo ti;
            if (!lookup(texs, name, &ti)) {
                *ok = false;
                return 0.f;
            }
            st.push_back(static_cast<float>(want_w ? ti.w : ti.h));
            continue;
        }

        if (tok.size() == 1) {
            char c = tok[0];
            if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
                c == '>' || c == '<' || c == '=') {
                if (st.size() < 2) { *ok = false; return 0.f; }
                float b = st.back(); st.pop_back();
                float a = st.back(); st.pop_back();
                float r = 0.f;
                switch (c) {
                    case '+': r = a + b; break;
                    case '-': r = a - b; break;
                    case '*': r = a * b; break;
                    case '/': r = b == 0.f ? 0.f : a / b; break;
                    case '%': r = b == 0.f ? 0.f : std::fmod(a, b); break;
                    case '>': r = a > b ? 1.f : 0.f; break;
                    case '<': r = a < b ? 1.f : 0.f; break;
                    case '=': r = a == b ? 1.f : 0.f; break;
                }
                st.push_back(r);
                continue;
            }
            if (c == '!') {
                if (st.empty()) { *ok = false; return 0.f; }
                float a = st.back(); st.pop_back();
                st.push_back(a == 0.f ? 1.f : 0.f);
                continue;
            }
        }

        if (!is_number(tok)) {
            *ok = false;
            return 0.f;
        }
        st.push_back(static_cast<float>(std::atof(tok.c_str())));
    }

    if (st.size() != 1) { *ok = false; return 0.f; }
    return st[0];
}

}  // namespace

float eval_size(const std::vector<std::string>& toks, const TexMap& texs, bool* ok) {
    if (toks.empty()) { *ok = false; return 0.f; }
    return eval_rpn_impl(toks, texs, ok);
}

bool eval_cond(const std::vector<std::string>& toks, const TexMap& texs) {
    if (toks.empty()) return true;
    bool ok = false;
    float v = eval_rpn_impl(toks, texs, &ok);
    return ok && v != 0.f;
}

// -------------------------------------------------------------------- GLSL

const char* kFullscreenVert =
    "#version 450\n"
    "layout(location = 0) out vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));\n"
    "    v_uv = p;\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

namespace {

std::string f2s(float v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
    std::string s(buf);
    // GLSL 里 1 不是 float 字面量
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
        s.find('E') == std::string::npos) {
        s += ".0";
    }
    return s;
}

void emit_tex_macros(std::string& g, const std::string& name, TexInfo ti) {
    g += "#define " + name + "_pos v_uv\n";
    g += "#define " + name + "_size vec2(" + f2s((float)ti.w) + ", " + f2s((float)ti.h) + ")\n";
    g += "#define " + name + "_pt vec2(" + f2s(1.f / (float)ti.w) + ", " +
         f2s(1.f / (float)ti.h) + ")\n";
    g += "#define " + name + "_mul vec4(1.0)\n";
    g += "#define " + name + "_tex(pos) texture(" + name + ", pos)\n";
    g += "#define " + name + "_texOff(off) " + name + "_tex(" + name +
         "_pos + " + name + "_pt * vec2(off))\n";
    g += "#define " + name + "_gather(pos, comp) textureGather(" + name + ", pos, comp)\n";
}

}  // namespace

std::string build_glsl(
    const Pass& p,
    const std::vector<BoundTex>& bound,
    TexInfo stage_in,
    int out_w,
    int out_h
) {
    std::string g;
    g.reserve(4096 + p.body.size());

    g += "#version 450\n";
    g += "layout(location = 0) in vec2 v_uv;\n";
    g += "layout(location = 0) out vec4 out_color;\n";

    for (size_t i = 0; i < bound.size(); i++) {
        g += "layout(binding = " + std::to_string(i) + ") uniform sampler2D " +
             bound[i].name + ";\n";
    }

    // 宏定义：HOOKED 指向当前阶段输入
    for (const auto& b : bound) {
        TexInfo ti = (b.name == "HOOKED") ? stage_in : b.info;
        emit_tex_macros(g, b.name, ti);
    }

    // MAIN stage 里 MAIN 与 HOOKED 指向同一张纹理。上游 Anime4K 有的 pass 只
    // 声明 BIND HOOKED 却在代码里写 MAIN_texOff，libplacebo 靠内部别名兜住；
    // 这里补同样的别名，否则 GLSL 编译不过。
    if (p.stage == Stage::Main) {
        bool has_main = false, has_hooked = false;
        for (const auto& b : bound) {
            if (b.name == "MAIN") has_main = true;
            if (b.name == "HOOKED") has_hooked = true;
        }
        if (has_hooked && !has_main) {
            static const char* kAlias[] = {"pos", "size", "pt", "mul", "tex", "texOff", "gather"};
            for (const char* a : kAlias) {
                g += std::string("#define MAIN_") + a + " HOOKED_" + a + "\n";
            }
        }
    }

    g += "#define OUTPUT_size vec2(" + f2s((float)out_w) + ", " + f2s((float)out_h) + ")\n";
    g += "\n";
    g += p.body;
    g += "\n";
    g += "void main() {\n";
    g += "    out_color = hook();\n";
    g += "}\n";
    return g;
}

}  // namespace a4k
