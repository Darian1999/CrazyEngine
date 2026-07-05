// tests/esl_bench.cpp
//
// Microbenchmark for crazy::esl::transpile. Runs N iterations over a
// synthesized large input so the per-call cost dominates setup; prints
// median + mean ms. Not a correctness test (the cookbook test covers that).
//
// Build: see the CMakeLists snippet at the end of this file.

#include <crazyengine/esl.h>

#include <chrono>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

// Build a representative shader source by concatenating several distinct
// sub-shaders (each within the engine's limits) in rotation. Every shader
// shape is hit: with/without attribute, with/without geometry stage, with
// transform(), with sample(), with gl_FragColor, with friendly types.
std::string synthesizeLargeSource(int reps) {
    // 4 distinct sub-shaders, each under the 3-attribute cap, each exercising
    // a different combination of substitution paths.
    static const char* kPatterns[] = {
        // 0: full kitchen sink (transform, sample, output, friendly types;
        //    swapped attribute for a built-in so we stay under the 3-attribute cap)
        "tex       = uniform texture\n"
        "uLightDir = uniform point\n"
        "passColor = varying color\n"
        "passUV    = varying point2d\n"
        "passNormal = varying point\n"
        "\n"
        "def vertex():\n"
        "    passColor  = color\n"
        "    passUV     = uv\n"
        "    passNormal = position\n"
        "    output     = transform(position + position * 0.05)\n"
        "\n"
        "def fragment():\n"
        "    float light = max(dot(passNormal, -uLightDir), 0.0)\n"
        "    output = sample(tex, passUV) * vec4(passColor * light, 1.0)\n",

        // 1: with geometry + gl_FragColor legacy + transform
        "uTime     = uniform number\n"
        "tex       = uniform texture\n"
        "passColor = varying color\n"
        "passUV    = varying point2d\n"
        "\n"
        "def vertex():\n"
        "    passColor = color\n"
        "    passUV    = uv\n"
        "    output    = transform(position)\n"
        "\n"
        "def geometry():\n"
        "    passColor = v_passColor[gi]\n"
        "    passUV    = v_passUV[gi]\n"
        "    output    = transform(gpos[gi] * (1.0 + sin(uTime)))\n"
        "    EmitVertex()\n"
        "    EndPrimitive()\n"
        "\n"
        "def fragment():\n"
        "    gl_FragColor = sample(tex, passUV) * vec4(passColor, 1.0)\n",

        // 2: minimal — exercises fast-path body lines (most lines have no substitutions)
        "passColor = varying color\n"
        "\n"
        "def vertex():\n"
        "    passColor = color\n"
        "    output = transform(position)\n"
        "\n"
        "def fragment():\n"
        "    output = passColor\n",

        // 3: many body lines with substitutions (output= / sample() / transform())
        "tex    = uniform texture\n"
        "passUV = varying point2d\n"
        "k      = uniform matrix\n"
        "\n"
        "def vertex():\n"
        "    passUV = uv\n"
        "    vec3 a = position + uv.xyy * 0.01\n"
        "    vec3 b = passUV.xyy + position\n"
        "    output = transform(a + b)\n"
        "\n"
        "def fragment():\n"
        "    vec4 c = sample(tex, passUV)\n"
        "    vec4 d = sample(tex, passUV * 0.5)\n"
        "    output = c * d * vec4(1.0)\n",
    };
    constexpr int kN = sizeof(kPatterns) / sizeof(kPatterns[0]);

    static const size_t lens[kN] = {
        std::strlen(kPatterns[0]),
        std::strlen(kPatterns[1]),
        std::strlen(kPatterns[2]),
        std::strlen(kPatterns[3]),
    };
    size_t totalLen = 0;
    for (int i = 0; i < reps; ++i) totalLen += lens[i % kN];

    std::string out;
    out.reserve(totalLen + 64);
    for (int i = 0; i < reps; ++i) out.append(kPatterns[i % kN]);
    return out;
}

// Single representative doc-style example, used for the per-iteration
// timing of a realistic single-shader call.
std::string realisticSource() {
    return
        "tex       = uniform texture\n"
        "uLightDir = uniform point\n"
        "uTime     = uniform number\n"
        "passColor = varying color\n"
        "passUV    = varying point2d\n"
        "passNormal = varying point\n"
        "n         = attribute point\n"
        "\n"
        "def vertex():\n"
        "    passColor  = color\n"
        "    passUV     = uv\n"
        "    passNormal = n\n"
        "    output     = transform(position + n * sin(uTime * 3.0) * 0.05)\n"
        "\n"
        "def fragment():\n"
        "    float light = max(dot(passNormal, -uLightDir), 0.0)\n"
        "    output = sample(tex, passUV) * vec4(passColor * (0.3 + 0.7 * light), 1.0)\n";
}

double median(std::vector<double>& v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

} // namespace

int main(int argc, char** argv) {
    // Defaults are tuned to be quick but loud enough to read 10%.
    int repsIn = (argc > 1) ? std::atoi(argv[1]) : 4000;
    int iters  = (argc > 2) ? std::atoi(argv[2]) : 11;  // odd # so median = real median
    bool warmupDump = (argc > 3) && std::string(argv[3]) == "verify";

    auto source = synthesizeLargeSource(repsIn);
    std::cout << "bench: synth input size = " << source.size() << " bytes, "
              << repsIn << " shader copies, " << iters << " timed iters\n";

    // Warmup so the first iter doesn't pay allocator/full-page-table cost.
    {
        auto r = crazy::esl::transpile(source);
        if (!r.ok) { std::cerr << "transpile failed: " << r.error << "\n"; return 2; }
        if (warmupDump) {
            std::cout << "--- vertex (first 800 chars) ---\n"
                      << r.vertex.substr(0, std::min<size_t>(r.vertex.size(), 800))
                      << "\n--- fragment (first 800 chars) ---\n"
                      << r.fragment.substr(0, std::min<size_t>(r.fragment.size(), 800))
                      << "\n";
        }
    }

    std::vector<double> ms;
    ms.reserve(iters);
    for (int i = 0; i < iters; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        auto r = crazy::esl::transpile(source);
        auto t1 = std::chrono::steady_clock::now();
        if (!r.ok) { std::cerr << "transpile failed: " << r.error << "\n"; return 2; }
        // Touch the result vector so the compiler can't elide the call.
        volatile size_t sink = r.vertex.size() + r.fragment.size() + r.geometry.size();
        (void)sink;
        double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        ms.push_back(us / 1000.0);
    }

    double total = 0.0;
    for (auto v : ms) total += v;
    double mean = total / ms.size();
    double med  = median(ms);
    std::printf("transpile(): mean=%.3f ms  median=%.3f ms  (iters=%d, min=%.3f, max=%.3f)\n",
                mean, med, iters,
                *std::min_element(ms.begin(), ms.end()),
                *std::max_element(ms.begin(), ms.end()));
    return 0;
}
