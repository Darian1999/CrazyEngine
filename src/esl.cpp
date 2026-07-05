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
//   def         ::= "def" ("vertex" | "geometry" | "fragment") "():"           e.g.  def vertex():
//   body        ::= (INDENTED_LINE)+                                             e.g. 4-space indent
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
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    rtrim(s);
    // After rtrim the string may have shrunk past our leading whitespace count
    // (e.g. input was all whitespace -> now empty). substr(a) on a shrunk-from
    // shorter string throws std::out_of_range, so collapse to empty instead.
    // Use erase(0, a) instead of s = s.substr(a) to avoid an allocation.
    if (a >= s.size()) { s.clear(); return; }
    if (a > 0) s.erase(0, a);
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

// Truncate s in-place at the first '#'. Using resize avoids the substring
// allocation that the previous s.substr(0, p) version incurred on every
// comment-stripped line.
void stripComment(std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '#') {
            s.resize(i);
            return;
        }
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
    if (rest == "vertex():" )  { isVertex   = true; return true; }
    if (rest == "geometry():") { isGeom     = true; return true; }
    if (rest == "fragment():") { isFragment = true; return true; }
    return false;
}

// Stages used by both the parse loop and the per-line transform.
enum Stage { DECLS, VERTEX, GEOMETRY, FRAGMENT };

// ---- Single-pass body-line transform ----------------------------------
//
// Original implementation ran 3 (or 4 for fragment) find-and-replace passes
// per body line, each re-scanning the line and re-allocating scratch
// strings. This implementation:
//   1. Does a fast-path skip when none of the substitution tokens is even
//      present (most arithmetic body lines — `passColor = color`,
//      `vec3 n = a_n;`, etc. — go through the fast path).
//   2. Otherwise walks the line once, applying all substitutions inline,
//      tracking paren depth so `transform(...)` correctly inserts `, 1.0`
//      before its matching `)` (works through nested `sample(...)` and
//      even nested `transform(...)`).
//
// All word-boundary semantics match the original implementation:
//   * `output`    — preceded by non-ident char; followed (after optional
//                   whitespace) by `=`.
//   * `sample(`   — preceded by non-ident char; immediately followed by
//                   `(` (no whitespace allowed, matching the original
//                   `replaceCall` which searched for `"sample("`).
//   * `transform` — preceded by non-ident char; followed (after optional
//                   whitespace) by `(`.
//   * `gl_FragColor` (fragment only) — no word-boundary check, matching
//                   the original literal `replaceAll`.
//
// Result: appended to `outBody` followed by a single '\n'. The trailing
// auto-`;` injection is preserved.
void processBodyLine(const std::string& line, Stage stage, std::string& outBody) {
    // Fast-path: cheap O(n) substring pre-checks. If none of the trigger
    // tokens appears in the line, we know no substitution can fire — emit
    // the line verbatim with the auto-semicolon.
    bool couldHave =
        line.find("output")    != std::string::npos
     || line.find("sample(")   != std::string::npos
     || (stage == VERTEX   && line.find("transform")    != std::string::npos)
     || (stage == FRAGMENT && line.find("gl_FragColor") != std::string::npos);

    if (!couldHave) {
        char last = line.empty() ? '\0' : line.back();
        outBody += line;
        if (last != ';' && last != '{' && last != '}') outBody += ';';
        outBody += '\n';
        return;
    }

    // Slow path: walk the line once and apply every substitution inline.
    std::string out;
    out.reserve(line.size() + 64);

    const char* outName = (stage == FRAGMENT) ? "fragColor" : "gl_Position";

    int parenDepth = 0;
    // Stack of parenDepths at which a `transform(` was opened. When a
    // closing `)` brings us back to one of these depths, we insert ", 1.0"
    // BEFORE emitting the `)`. Almost always zero or one entry.
    std::vector<int> transformDepths;
    transformDepths.reserve(2);

    const size_t len = line.size();
    size_t i = 0;
    while (i < len) {
        // gl_FragColor  ->  fragColor  (fragment only) — UNCONDITIONAL
        // literal substitution, matching the original replaceAll behavior.
        // Sits ABOVE the wordStart gate so identifiers containing
        // `gl_FragColor` as a substring are still rewritten.
        if (stage == FRAGMENT && i + 12 <= len && line.compare(i, 12, "gl_FragColor") == 0) {
            out.append("fragColor", 9);
            i += 12;
            continue;
        }

        bool wordStart = (i == 0) || !isIdentChar(line[i - 1], false);

        if (wordStart) {
            // output = X  ->  outName = X
            if (i + 6 <= len && line.compare(i, 6, "output") == 0) {
                size_t o = i + 6;
                while (o < len && (line[o] == ' ' || line[o] == '\t')) ++o;
                if (o < len && line[o] == '=') {
                    out.append(outName);
                    i += 6;  // fall-back into normal streaming for ws and '='
                    continue;
                }
            }
            // sample(  ->  texture(
            if (i + 7 <= len && line.compare(i, 7, "sample(") == 0) {
                out.append("texture(", 8);
                i += 7;
                ++parenDepth;  // synthetic paren balance
                continue;
            }
            // transform(  ->  projection * view * model * vec4(  (vertex only)
            if (stage == VERTEX && i + 9 <= len && line.compare(i, 9, "transform") == 0) {
                size_t o = i + 9;
                while (o < len && (line[o] == ' ' || line[o] == '\t')) ++o;
                if (o < len && line[o] == '(') {
                    out.append("projection * view * model * vec4(");
                    i = o + 1;  // skip past the original `(`
                    ++parenDepth;
                    transformDepths.push_back(parenDepth);
                    continue;
                }
            }
        }

        char c = line[i];
        if (c == '(') {
            ++parenDepth;
        } else if (c == ')') {
            // Insert ", 1.0" before the `)` that closes a transform(...)-born
            // vec4(...). If the line was unterminated (we hit end-of-string
            // before matching), the stack is left non-empty but no insert
            // happens — matching the original behavior.
            if (!transformDepths.empty() && transformDepths.back() == parenDepth) {
                out.append(", 1.0", 5);
                transformDepths.pop_back();
            }
            --parenDepth;
        }
        out.push_back(c);
        ++i;
    }

    char last = out.empty() ? '\0' : out.back();
    if (last != ';' && last != '{' && last != '}') out += ';';
    outBody += out;
    outBody += '\n';
}

// ---- Result builder ----------------------------------------------------
//
// Original used std::ostringstream with ~40 `<<` ops per shader — each
// goes through locale-independent facet machinery. Direct string append
// is cheaper and lets us reserve exactly the size we know we'll need.

static void appendDeclStructDecls(std::string& out,
                                   const std::vector<Decl>& decls,
                                   const char* keyword) {
    out.append(keyword);
    for (const auto& d : decls) {
        out.append(" ");
        out.append(d.type);
        out.append(" ");
        out.append(d.name);
        out.append(";\n");
    }
}

Result buildGlsl(const std::vector<Decl>& uniforms,
                 const std::vector<Decl>& varyings,
                 const std::vector<Decl>& attributes,
                 const std::string& vertBody,
                 const std::string& geomBody,
                 const std::string& fragBody) {
    std::string vert, geom, frag;

    // Per-line preamble size estimates so the output buffer reservation
    // covers the actual growth even when many uniforms/varyings/attributes
    // are declared. Without this the reserved capacity is dominated by
    // the body size only, and the preamble loops trigger several reallocs.
    constexpr size_t kUniformLine    = 32;   // "uniform TYPE NAME;\n"
    constexpr size_t kVaryingOut     = 28;   // "out TYPE NAME;\n"  (vert)
    constexpr size_t kVaryingInOut   = 64;   // both lines for geom
    constexpr size_t kVaryingIn      = 28;   // "in TYPE NAME;\n"  (frag)
    constexpr size_t kAttrLayout     = 56;   // "layout(...) in TYPE a_NAME;\n"
    constexpr size_t kAttrAlias      = 40;   // "    TYPE NAME = a_NAME;\n"
    constexpr size_t kVertPreamble   = 256;
    constexpr size_t kGeomPreamble   = 256;
    constexpr size_t kFragPreamble   = 128;

    // ---- Vertex ----
    vert.reserve(vertBody.size()
                 + uniforms.size()  * kUniformLine
                 + varyings.size()  * kVaryingOut
                 + attributes.size() * (kAttrLayout + kAttrAlias)
                 + kVertPreamble);

    vert.append(
        "#version 330 core\n"
        "// Generated by CrazyEngine ESL transpiler\n\n"
        "layout(location = 0) in vec3 a_position;\n"
        "layout(location = 1) in vec3 a_color;\n"
        "layout(location = 2) in vec2 a_uv;\n"
        "uniform mat4 model;\n"
        "uniform mat4 view;\n"
        "uniform mat4 projection;\n");
    appendDeclStructDecls(vert, uniforms, "uniform");
    appendDeclStructDecls(vert, varyings, "out");

    for (const auto& a : attributes) {
        vert.append("layout(location = ");
        vert.append(std::to_string(a.location));
        vert.append(") in ");
        vert.append(a.type);
        vert.append(" a_");
        vert.append(a.name);
        vert.append(";\n");
    }

    vert.append("\nvoid main() {\n");
    vert.append("    vec3 position = a_position;\n");
    vert.append("    vec3 color    = a_color;\n");
    vert.append("    vec2 uv       = a_uv;\n");
    for (const auto& a : attributes) {
        vert.append("    ");
        vert.append(a.type);
        vert.append(" ");
        vert.append(a.name);
        vert.append(" = a_");
        vert.append(a.name);
        vert.append(";\n");
    }
    vert.append(vertBody);
    vert.append("}\n");

    // ---- Geometry (optional) ----
    if (!geomBody.empty()) {
        geom.reserve(geomBody.size()
                     + uniforms.size()  * kUniformLine
                     + varyings.size()  * kVaryingInOut
                     + kGeomPreamble);

        geom.append(
            "#version 330 core\n"
            "// Generated by CrazyEngine ESL transpiler\n\n"
            "layout(triangles) in;\n"
            "layout(triangle_strip, max_vertices = 6) out;\n"
            "uniform mat4 model;\n"
            "uniform mat4 view;\n"
            "uniform mat4 projection;\n");
        appendDeclStructDecls(geom, uniforms, "uniform");
        for (const auto& v : varyings) {
            geom.append("in ");
            geom.append(v.type);
            geom.append(" v_");
            geom.append(v.name);
            geom.append("[3];\n");
            geom.append("out ");
            geom.append(v.type);
            geom.append(" ");
            geom.append(v.name);
            geom.append(";\n");
        }
        geom.append(
            "\nvoid main() {\n"
            "    vec3 gpos[3] = vec3[3](\n"
            "        gl_in[0].gl_Position.xyz,\n"
            "        gl_in[1].gl_Position.xyz,\n"
            "        gl_in[2].gl_Position.xyz\n"
            "    );\n"
            "    int gi = 0;\n");
        geom.append(geomBody);
        geom.append("}\n");
    }

    // ---- Fragment ----
    frag.reserve(fragBody.size()
                 + uniforms.size() * kUniformLine
                 + varyings.size() * kVaryingIn
                 + kFragPreamble);

    frag.append(
        "#version 330 core\n"
        "// Generated by CrazyEngine ESL transpiler\n\n"
        "precision mediump float;\n"
        "out vec4 fragColor;\n");
    for (const auto& v : varyings)  frag.append("in ")     .append(v.type).append(" ").append(v.name).append(";\n");
    appendDeclStructDecls(frag, uniforms, "uniform");

    frag.append("\nvoid main() {\n");
    frag.append(fragBody);
    frag.append("}\n");

    return { vert, frag, geom, true, "" };
}

constexpr int kMaxAttributes = 3; // bounded by engine extras[3] slots (locations 3, 4, 5)

Result transpileInternal(const std::string& source) {
    std::vector<Decl> uniforms, varyings, attributes;
    std::string vertBody, geomBody, fragBody;
    // Reserve based on source size — bodies are typically ~2x source length
    // once uniform/varying decls and formatting chunks are appended.
    vertBody.reserve(source.size() * 2 + 256);
    geomBody.reserve(source.size()     + 256);
    fragBody.reserve(source.size()     + 256);

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
            // All body-line substitutions (output=, sample(, transform(,
            // gl_FragColor, plus auto-semicolon) are handled by a single
            // character-by-character pass — much cheaper than the previous
            // 3-4 separate find+replace scans per line.
            processBodyLine(content, stage, *target);
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
