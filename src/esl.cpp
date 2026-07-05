#include "crazyengine/esl.h"

#include <sstream>
#include <vector>
#include <string>
#include <unordered_map>
#include <cctype>
#include <cstddef>

// ESL — Pythonic Easy Shading Language.
//
// Grammar (a tiny subset of Python):
//
//   file        ::= (toplevel)*
//   toplevel    ::= declaration | def
//   declaration ::= IDENT "=" ("uniform" | "varying" | "attribute") glsl_type   e.g.  n = attribute point
//   def         ::= "def" ("vertex" | "geometry" | "fragment") "()" ":"          e.g.  def vertex():
//   body        ::= (INDENTED_LINE)+                                              e.g. 4-space indent
//
// Comments start with "#" and run to end of line. No semicolons required in
// statements — the transpiler auto-appends ";" to body lines that don't already
// end with ";", "{", or "}".
//
// User-facing syntax has been abstracted away from raw GLSL:
//   - Friendly type aliases: point|color -> vec3, point2d|uv_coord -> vec2,
//                              matrix -> mat4, texture -> sampler2D,
//                              number -> float, integer -> int.
//   - Output writes use "output = …" (mapped to gl_Position in vert/geom and
//     fragColor in frag). The legacy "gl_FragColor = …" form still works.
//   - Sampler lookup uses "sample(name, uv)" (mapped to "texture(name, uv)").
//   - "transform(point)" expands to "projection * view * model * vec4(point, 1.0)"
//     in the vertex stage only (paren-balanced so nested expressions still work).
// Original GLSL names continue to work — abstracts are a friendlier front door.
//
// Built-in vertex inputs (no decl needed): vec3 position, vec3 color, vec2 uv (locations 0, 1, 2)
// Built-in uniforms (no decl needed): mat4 model, mat4 view, mat4 projection
//
// Geometry stage is OPTIONAL. When `def geometry():` is present, the transpiler
// injects a preamble at the top of the body:
//   vec3 gpos[3] = vec3[3](gl_in[0].gl_Position.xyz, gl_in[1].gl_Position.xyz, gl_in[2].gl_Position.xyz);
//   int gi = 0;        // user-mutable index for the current input vertex
// The default config is `layout(triangles) in` / `layout(triangle_strip, max_vertices = 6) out`.

namespace crazy {
namespace esl {

namespace {

void rtrim(std::string& s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.pop_back();
    }
}

void trim(std::string& s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    rtrim(s);
    // After rtrim the string may have shrunk past our leading whitespace count
    // (e.g. input was all whitespace -> now empty). substr(a) on a shrunk-from
    // shorter string throws std::out_of_range, so collapse to empty instead.
    if (a >= s.size()) { s.clear(); return; }
    if (a > 0) s = s.substr(a);
}

int countIndent(const std::string& line) {
    int n = 0;
    while (n < (int)line.size() && (line[n] == ' ' || line[n] == '\t')) ++n;
    return n;
}

bool isIdentChar(char c, bool first) {
    if (first) return std::isalpha((unsigned char)c) || c == '_';
    return std::isalnum((unsigned char)c) || c == '_';
}

bool isIdentifier(const std::string& s) {
    if (s.empty()) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (!isIdentChar(s[i], i == 0)) return false;
    }
    return true;
}

void stripComment(std::string& s) {
    auto p = s.find('#');
    if (p != std::string::npos) s = s.substr(0, p);
}

void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

// (Word-boundary substitution is implemented inline by the helpers below
// for their respective call shapes — no shared helper.)

// In vert/geom/frag bodies, when the user writes `name(...)`, optionally
// rewrite the call to `replacement(...)`. Only fires when name is a complete
// identifier immediately followed by `(`.
void replaceCall(std::string& s, const std::string& name, const std::string& replacement) {
    const std::string needle = name + "(";
    size_t pos = 0;
    while (true) {
        size_t found = s.find(needle, pos);
        if (found == std::string::npos) break;
        bool precededByIdent =
            found > 0 && (std::isalnum((unsigned char)s[found - 1]) || s[found - 1] == '_');
        if (precededByIdent) {
            pos = found + needle.size();
            continue;
        }
        s.replace(found, needle.size(), replacement + "(");
        pos = found + replacement.size() + 1;
    }
}

// Replace `output =` (with word-boundary on the LHS identifier) by `replacement`,
// so user can write a friendlier output statement instead of gl_Position = …
void replaceOutputAssign(std::string& s, const std::string& replacement) {
    const std::string needle = "output";
    size_t pos = 0;
    while (true) {
        size_t found = s.find(needle, pos);
        if (found == std::string::npos) break;
        bool precededByIdent =
            found > 0 && (std::isalnum((unsigned char)s[found - 1]) || s[found - 1] == '_');
        if (precededByIdent) { pos = found + needle.size(); continue; }
        // Require `=` (with optional whitespace) immediately after `output`.
        size_t o = found + needle.size();
        while (o < s.size() && (s[o] == ' ' || s[o] == '\t')) ++o;
        if (o >= s.size() || s[o] != '=') { pos = found + needle.size(); continue; }
        s.replace(found, needle.size(), replacement);
        pos = found + replacement.size();
    }
}

// Expand `transform(point)` to `projection * view * model * vec4(point, 1.0)`.
// Paren-balance from the substituted open paren to find the matching close
// paren of the new vec4(...) and insert `, 1.0` before it.
void expandTransform(std::string& s) {
    const std::string needle    = "transform";
    const std::string expansion = "projection * view * model * vec4";
    size_t pos = 0;
    while (true) {
        size_t found = s.find(needle, pos);
        if (found == std::string::npos) break;
        bool precededByIdent =
            found > 0 && (std::isalnum((unsigned char)s[found - 1]) || s[found - 1] == '_');
        if (precededByIdent) { pos = found + needle.size(); continue; }
        // Require `(` (optionally preceded by whitespace) after `transform`.
        size_t o = found + needle.size();
        while (o < s.size() && (s[o] == ' ' || s[o] == '\t')) ++o;
        if (o >= s.size() || s[o] != '(') { pos = found + needle.size(); continue; }
        s.replace(found, needle.size(), expansion);
        size_t scan = found + expansion.size();
        // scan starts on the `(` of the new vec4(...) call (== original `(` of transform(...)).
        int depth = 1;
        size_t closePos = scan + 1;
        while (closePos < s.size() && depth > 0) {
            if (s[closePos] == '(') ++depth;
            else if (s[closePos] == ')') --depth;
            if (depth == 0) break;
            ++closePos;
        }
        if (depth != 0) break; // unterminated — abandon further expansion
        s.insert(closePos, ", 1.0");
        pos = closePos + std::string(", 1.0").size();
    }
}

// Friendly type aliases resolved in declarations. GLSL names still work.
const std::unordered_map<std::string, std::string>& typeAliases() {
    static const std::unordered_map<std::string, std::string> m = {
        {"point",    "vec3"},
        {"color",    "vec3"},
        {"point2d",  "vec2"},
        {"uv_coord", "vec2"},
        {"matrix",   "mat4"},
        {"texture",  "sampler2D"},
        {"number",   "float"},
        {"integer",  "int"},
    };
    return m;
}

void resolveTypeAlias(std::string& t) {
    auto it = typeAliases().find(t);
    if (it != typeAliases().end()) t = it->second;
}

struct Decl {
    std::string kind;   // "uniform" | "varying" | "attribute"
    std::string name;
    std::string type;
    int         location = -1;  // set only for "attribute" (uniform/varying leave at -1)
};

// Parse a Pythonic declaration line: IDENT "=" ("uniform"|"varying") TYPE
bool parseDecl(const std::string& line, Decl& out, int lineNum, std::string& err) {
    auto eq = line.find('=');
    if (eq == std::string::npos) {
        err = "expected '=' in declaration on line " + std::to_string(lineNum);
        return false;
    }

    std::string lhs = line.substr(0, eq);
    std::string rhs = line.substr(eq + 1);
    trim(lhs); trim(rhs);

    if (!isIdentifier(lhs)) {
        err = "left-hand side of '=' must be a simple identifier (line " +
              std::to_string(lineNum) + ")";
        return false;
    }

    auto sp = rhs.find(' ');
    if (sp == std::string::npos) {
        err = "right-hand side must be 'uniform TYPE' or 'varying TYPE' (line " +
              std::to_string(lineNum) + ")";
        return false;
    }
    std::string kw = rhs.substr(0, sp);
    std::string type = rhs.substr(sp + 1);
    trim(kw); trim(type);

    if (kw != "uniform" && kw != "varying" && kw != "attribute") {
        err = "only 'uniform' / 'varying' / 'attribute' declarations are supported (line " +
              std::to_string(lineNum) + ")";
        return false;
    }
    if (type.empty()) {
        err = "missing GLSL type after '" + kw + "' (line " + std::to_string(lineNum) + ")";
        return false;
    }

    // Resolve friendly type aliases (point/point2d/color/matrix/texture/...).
    // Backward-compatible: plain GLSL names are passed through unchanged.
    resolveTypeAlias(type);

    // Attribute declarations cannot carry matrix / texture / sampler-like types.
    if (kw == "attribute") {
        if (type == "mat4" || type == "mat3" || type == "mat2"
            || type.find("sampler") == 0
            || type.find("image") == 0
            || type.find("atomic") == 0) {
            err = "attribute '" + lhs + "' has unsupported type '" + type +
                  "' (line " + std::to_string(lineNum) +
                  ") — per-vertex attribute types must be scalar/vector (float, vec2, vec3, vec4)";
            return false;
        }
    }

    out.kind = kw;
    out.name = lhs;
    out.type = type;
    return true;
}

// Recognise "def vertex():", "def geometry():", or "def fragment():".
// (Content has leading spaces already trimmed.)
bool parseDef(const std::string& content, bool& isVertex, bool& isGeom, bool& isFragment) {
    isVertex = isGeom = isFragment = false;
    const std::string p = "def ";
    if (content.rfind(p, 0) != 0) return false;
    std::string rest = content.substr(p.size());
    trim(rest);
    if (rest == "vertex():")   { isVertex   = true; return true; }
    if (rest == "geometry():") { isGeom     = true; return true; }
    if (rest == "fragment():") { isFragment = true; return true; }
    return false;
}

Result buildGlsl(const std::vector<Decl>& uniforms,
                 const std::vector<Decl>& varyings,
                 const std::vector<Decl>& attributes,
                 const std::string& vertBody,
                 const std::string& geomBody,
                 const std::string& fragBody) {
    std::ostringstream vert, geom, frag;

    // ---- Vertex ----
    vert << "#version 330 core\n";
    vert << "// Generated by CrazyEngine ESL transpiler\n\n";
    vert << "layout(location = 0) in vec3 a_position;\n";
    vert << "layout(location = 1) in vec3 a_color;\n";
    vert << "layout(location = 2) in vec2 a_uv;\n";
    vert << "uniform mat4 model;\n";
    vert << "uniform mat4 view;\n";
    vert << "uniform mat4 projection;\n";
    for (auto& u : uniforms)  vert << "uniform " << u.type << " " << u.name << ";\n";
    for (auto& v : varyings)  vert << "out "    << v.type << " " << v.name << ";\n";
    for (auto& a : attributes) {
        vert << "layout(location = " << a.location << ") in " << a.type << " a_" << a.name << ";\n";
    }

    vert << "\nvoid main() {\n";
    vert << "    vec3 position = a_position;\n";
    vert << "    vec3 color    = a_color;\n";
    vert << "    vec2 uv       = a_uv;\n";
    for (auto& a : attributes) {
        vert << "    " << a.type << " " << a.name << " = a_" << a.name << ";\n";
    }
    vert << vertBody;
    vert << "}\n";

    // ---- Geometry (optional) ----
    if (!geomBody.empty()) {
        geom << "#version 330 core\n";
        geom << "// Generated by CrazyEngine ESL transpiler\n\n";
        geom << "layout(triangles) in;\n";
        geom << "layout(triangle_strip, max_vertices = 6) out;\n";
        geom << "uniform mat4 model;\n";
        geom << "uniform mat4 view;\n";
        geom << "uniform mat4 projection;\n";
        for (auto& u : uniforms) geom << "uniform " << u.type << " " << u.name << ";\n";
        // Varyings from vertex are received by the geom stage as `in v_NAME[3]`
        // (the `v_` prefix avoids a collision with the same-name `out NAME`).
        // Routing the data is the user's responsibility:
        //   passColor = v_passColor[gi];
        //   ...
        //   EmitVertex();
        for (auto& v : varyings) {
            geom << "in "  << v.type << " v_" << v.name << "[3];\n";
            geom << "out " << v.type << " "    << v.name << ";\n";
        }

        geom << "\nvoid main() {\n";
        geom << "    vec3 gpos[3] = vec3[3](\n";
        geom << "        gl_in[0].gl_Position.xyz,\n";
        geom << "        gl_in[1].gl_Position.xyz,\n";
        geom << "        gl_in[2].gl_Position.xyz\n";
        geom << "    );\n";
        geom << "    int gi = 0;\n";
        geom << geomBody;
        geom << "}\n";
    }

    // ---- Fragment ----
    frag << "#version 330 core\n";
    frag << "// Generated by CrazyEngine ESL transpiler\n\n";
    frag << "precision mediump float;\n";
    frag << "out vec4 fragColor;\n";
    for (auto& v : varyings) frag << "in "     << v.type << " " << v.name << ";\n";
    for (auto& u : uniforms) frag << "uniform " << u.type << " " << u.name << ";\n";

    frag << "\nvoid main() {\n";
    frag << fragBody;
    frag << "}\n";

    return { vert.str(), frag.str(), geom.str(), true, "" };
}

constexpr int kMaxAttributes = 3; // bounded by engine extras[3] slots (locations 3, 4, 5)

Result transpileInternal(const std::string& source) {
    std::vector<Decl> uniforms, varyings, attributes;
    std::string vertBody, geomBody, fragBody;
    enum Stage { DECLS, VERTEX, GEOMETRY, FRAGMENT };
    Stage stage = DECLS;

    std::istringstream iss(source);
    std::string raw;
    int lineNum = 0;

    int nextAttrLocation = 3;  // engine reserves 0/1/2 for position/color/uv

    while (std::getline(iss, raw)) {
        ++lineNum;

        // Strip comment first so leading whitespace inside a comment is harmless.
        stripComment(raw);

        int indent = countIndent(raw);
        std::string content = raw;
        trim(content);
        if (content.empty()) continue;

        if (indent == 0) {
            stage = DECLS;

            // def vertex(): / def geometry(): / def fragment():
            bool isVert = false, isGeom = false, isFrag = false;
            if (parseDef(content, isVert, isGeom, isFrag)) {
                if (isVert)       stage = VERTEX;
                else if (isGeom)  stage = GEOMETRY;
                else              stage = FRAGMENT;
                continue;
            }

            // Otherwise it's a declaration
            Decl d;
            std::string err;
            if (!parseDecl(content, d, lineNum, err)) {
                return { "", "", "", false, err };
            }
            if      (d.kind == "uniform")   uniforms.push_back(d);
            else if (d.kind == "varying")   varyings.push_back(d);
            else if (d.kind == "attribute") {
                if ((int)attributes.size() >= kMaxAttributes) {
                    return { "", "", "", false,
                        "Too many attribute declarations (line " + std::to_string(lineNum) +
                        "): engine can only bind " + std::to_string(kMaxAttributes) +
                        " extras slots (locations 3-" + std::to_string(kMaxAttributes + 2) +
                        "); reduce attribute count or extend engine Vertex" };
                }
                d.location = nextAttrLocation++;
                attributes.push_back(d);
            }
        } else {
            if (stage == DECLS) {
                return { "", "", "", false,
                    "Line " + std::to_string(lineNum) +
                    ": indented body line but no active 'def vertex():' / 'def geometry():' / 'def fragment():' block" };
            }
            std::string* target = nullptr;
            switch (stage) {
                case VERTEX:   target = &vertBody; break;
                case GEOMETRY: target = &geomBody; break;
                case FRAGMENT: target = &fragBody; break;
                default:       target = nullptr;    break;
            }
            std::string line = content;
            // Front-door abstractions: `output = …` substitutes for the
            // stage-appropriate gl_Position / fragColor write; `sample(...)`
            // becomes `texture(...)` for sampler lookups; `transform(point)`
            // expands to `projection * view * model * vec4(point, 1.0)` in
            // the vertex stage only.
            switch (stage) {
                case VERTEX:   replaceOutputAssign(line, "gl_Position"); break;
                case GEOMETRY: replaceOutputAssign(line, "gl_Position"); break;
                case FRAGMENT:
                    replaceAll(line, "gl_FragColor", "fragColor");  // legacy
                    replaceOutputAssign(line, "fragColor");         // new
                    break;
                default: break;
            }
            replaceCall(line, "sample", "texture");
            if (stage == VERTEX) expandTransform(line);
            // GLSL needs semicolons; Pythonic ESL doesn't use them, so inject
            // one at the end of every body line that's not already terminated
            // by ';', '{', or '}' (handles braces for inline if-blocks, etc.).
            char last = line.empty() ? '\0' : line.back();
            if (last != ';' && last != '{' && last != '}') line += ';';
            *target += line + "\n";
        }
    }

    if (vertBody.empty()) return { "", "", "", false, "Missing or empty 'def vertex():' block" };
    if (fragBody.empty()) return { "", "", "", false, "Missing or empty 'def fragment():' block" };

    return buildGlsl(uniforms, varyings, attributes, vertBody, geomBody, fragBody);
}

} // namespace

Result transpile(const std::string& source) {
    return transpileInternal(source);
}

} // namespace esl
} // namespace crazy
