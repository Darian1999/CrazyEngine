// tests/esl_cookbook_test.cpp
//
// Doc-driven test: parses docs/ESL.md, extracts every Python-fenced code
// block under a "### 5.X" (cookbook) or "### Step X" (tutorial) heading,
// runs each one through crazy::esl::transpile, and asserts the transpile
// succeeded and the generated GLSL has the expected structure.
//
// The point: if a future doc edit introduces a typo or a stale example,
// this test catches it. The test only checks transpile-level correctness
// (it does not require a real OpenGL context — the GLSL is inspected by
// string search only).
//
// Run directly:  ./build/esl_cookbook_test
// Via CTest:     ctest --test-dir build -R esl_cookbook_test --output-on-failure

#include <crazyengine/esl.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstddef>

#ifndef DOC_PATH
#error "DOC_PATH must be defined by CMake (path to docs/ESL.md)."
#endif

namespace {

int g_passes   = 0;
int g_failures = 0;
int g_skipped  = 0;
const char* g_current = "<top>";

#define EXPECT(cond) do {                                                       \
    if (!(cond)) {                                                             \
        std::cerr << "  [FAIL] " << g_current << "  "                          \
                  << #cond << "  (" << __FILE__ << ":" << __LINE__ << ")\n";   \
        ++g_failures;                                                          \
    } else {                                                                   \
        ++g_passes;                                                            \
    }                                                                          \
} while (0)

#define EXPECT_CONTAINS(haystack, needle) do {                                 \
    if ((haystack).find(needle) == std::string::npos) {                        \
        std::cerr << "  [FAIL] " << g_current << "  expected to contain "      \
                  << "'" << (needle) << "'\n";                                \
        ++g_failures;                                                          \
    } else {                                                                   \
        ++g_passes;                                                            \
    }                                                                          \
} while (0)

struct Example {
    std::string name;     // e.g. "5.1 Solid color (no texture)" or "Step 1 — solid color (no texture)"
    std::string source;   // the .esl source as a single string with '\n' separators
};

// -------------------------------------------------------------
// Minimal markdown parser
// -------------------------------------------------------------
//
// Walks the doc line-by-line. Tracks the most recent "### 5.X" or
// "### Step X" heading. When the next ```python``` fenced block opens,
// accumulates its lines as the example source. Closes on ```.
// Snippets (no def vertex()) are filtered out by the test, not the parser.
std::vector<Example> parseExamplesFromDoc(const std::string& doc) {
    std::vector<Example> out;
    std::istringstream iss(doc);
    std::string line;

    std::string currentSection;
    bool inCode = false;
    std::string codeLang;
    std::string codeBody;

    auto isExampleSection = [](const std::string& s) {
        return !s.empty()
            && (s.rfind("5.", 0) == 0 || s.rfind("Step ", 0) == 0);
    };
    auto flush = [&]() {
        if (inCode && isExampleSection(currentSection)
            && codeLang == "python" && !codeBody.empty()) {
            out.push_back({currentSection, codeBody});
        }
        inCode = false;
        codeLang.clear();
        codeBody.clear();
    };

    auto rtrim = [](std::string& s) {
        while (!s.empty()
            && (s.back() == ' ' || s.back() == '\t'
                || s.back() == '\r' || s.back() == '\n')) {
            s.pop_back();
        }
    };

    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (!inCode) {
            // Section header? Track every "### " heading so non-example
            // sections (e.g. "### 3.1") reset currentSection and don't
            // accidentally claim code blocks further down the file.
            if (line.rfind("### ", 0) == 0) {
                std::string header = line.substr(4);
                rtrim(header);
                currentSection = header;
            }
            // Fence open?
            if (line.rfind("```", 0) == 0) {
                std::string lang = line.substr(3);
                rtrim(lang);
                codeLang = lang;
                inCode = true;
                codeBody.clear();
            }
        } else {
            if (line.rfind("```", 0) == 0) {
                flush();
            } else {
                if (!codeBody.empty()) codeBody += '\n';
                codeBody += line;
            }
        }
    }
    flush();  // safety: flush if file ended mid-block (shouldn't happen here)
    return out;
}

// Helper: pull the declared NAME out of a "TOKEN NAME" line, where TOKEN is
// already positioned at offset 0 in `line` (e.g. line = "passColor = varying
// color" with token = "varying " would return "color").
// Returns empty string if the line doesn't look like a declaration.
std::string extractName(const std::string& line, const std::string& token) {
    if (line.rfind(token, 0) != 0) return "";
    size_t a = line.find_first_not_of(' ', token.size());
    if (a == std::string::npos) return "";
    // Identifier runs until the next whitespace, '#' (inline comment), or
    // end-of-line. Any of those terminates the name.
    size_t b = a;
    while (b < line.size() && line[b] != ' ' && line[b] != '\t'
                                && line[b] != '#') ++b;
    if (b == a) return "";  // no identifier
    return line.substr(a, b - a);
}

// -------------------------------------------------------------
// Per-example assertions
// -------------------------------------------------------------

void runExample(const Example& ex) {
    g_current = ex.name.c_str();

    // Filter: only full programs (those that define a vertex stage).
    // This skips incidental code blocks (declaration snippets, transform
    // one-liners, etc.) that would otherwise fail to transpile.
    if (ex.source.find("def vertex()") == std::string::npos) {
        std::cout << "--- " << ex.name << "  (skipped: snippet, not a full program)\n";
        ++g_skipped;
        return;
    }

    std::cout << "--- " << ex.name << "\n";

    auto r = crazy::esl::transpile(ex.source);
    EXPECT(r.ok);
    if (!r.ok) {
        std::cerr << "  transpile error: " << r.error << "\n";
        return;
    }

    // Vertex stage: always present, always contains these.
    EXPECT(!r.vertex.empty());
    EXPECT_CONTAINS(r.vertex, "#version 330 core");
    EXPECT_CONTAINS(r.vertex, "void main()");
    EXPECT_CONTAINS(r.vertex, "gl_Position");

    // Fragment stage: always present, always contains these.
    EXPECT(!r.fragment.empty());
    EXPECT_CONTAINS(r.fragment, "#version 330 core");
    EXPECT_CONTAINS(r.fragment, "void main()");
    EXPECT_CONTAINS(r.fragment, "fragColor");

    // Geometry stage: present iff the source declared it.
    const bool hasGeom = ex.source.find("def geometry():") != std::string::npos;
    if (hasGeom) {
        EXPECT(!r.geometry.empty());
        EXPECT_CONTAINS(r.geometry, "EmitVertex()");
        EXPECT_CONTAINS(r.geometry, "EndPrimitive()");
    } else {
        EXPECT(r.geometry.empty());
    }

    // Cross-check: every declared varying should appear in both vert and frag.
    // Walk the source line by line, looking for "varying " (with trailing
    // space, so the keyword can't be a prefix of a longer word).
    for (size_t i = 0; i < ex.source.size(); ) {
        size_t j = ex.source.find('\n', i);
        if (j == std::string::npos) j = ex.source.size();
        std::string line = ex.source.substr(i, j - i);
        std::string name = extractName(line, "varying ");
        if (!name.empty()) {
            // declaration line in vert: "out TYPE NAME;\n"
            EXPECT_CONTAINS(r.vertex, "out " + name);
            // declaration line in frag: "in TYPE NAME;\n"
            EXPECT_CONTAINS(r.fragment, "in " + name);
        }
        i = j + 1;
    }

    // Cross-check: every declared attribute should appear as a layout(input)
    // with its a_ alias, and as a same-name local in the vertex main.
    for (size_t i = 0; i < ex.source.size(); ) {
        size_t j = ex.source.find('\n', i);
        if (j == std::string::npos) j = ex.source.size();
        std::string line = ex.source.substr(i, j - i);
        std::string name = extractName(line, "attribute ");
        if (!name.empty()) {
            EXPECT_CONTAINS(r.vertex, "a_" + name);
            // The alias is e.g. "vec3 n = a_n;" (one-line declaration + assignment)
            EXPECT_CONTAINS(r.vertex, name + " = a_" + name);
        }
        i = j + 1;
    }
}

} // namespace

int main() {
    std::ifstream in(DOC_PATH);
    if (!in.is_open()) {
        std::cerr << "FAIL: could not open doc at: " << DOC_PATH << "\n";
        return 2;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string doc = ss.str();

    auto examples = parseExamplesFromDoc(doc);
    std::cout << "Found " << examples.size()
              << " example section(s) in docs/ESL.md\n\n";

    if (examples.empty()) {
        std::cerr << "FAIL: parser found no examples. "
                  << "Doc structure may have changed.\n";
        return 3;
    }

    for (const auto& ex : examples) {
        runExample(ex);
    }

    std::cout << "\n=== " << g_passes << " passed, "
              << g_failures << " failed, "
              << g_skipped  << " skipped ===\n";
    return g_failures == 0 ? 0 : 1;
}
