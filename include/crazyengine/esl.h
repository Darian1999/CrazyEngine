#pragma once

// ESL — Easy Shading Language for CrazyEngine
// Transpiles a simple .esl file into GLSL #version 330 core shaders.

#include <string>

namespace crazy {
namespace esl {

struct Result {
    std::string vertex;     // generated GLSL vertex shader
    std::string fragment;   // generated GLSL fragment shader
    std::string geometry;   // generated GLSL geometry shader (empty if no 'def geometry():' block)
    bool ok = false;
    std::string error;      // human-readable error if ok == false
};

// Transpile a full .esl source string. Returns ok == false with `error` set
// on parse failure.
Result transpile(const std::string& source);

} // namespace esl
} // namespace crazy
