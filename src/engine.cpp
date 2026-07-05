#include "crazyengine/engine.h"

// stb_image: single-header library, define STB_IMAGE_IMPLEMENTATION in exactly one cpp
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

#include "crazyengine/esl.h"

#include <fstream>
#include <sstream>
#include <iostream>
#include <cmath>
#include <cstring>
#include <cstddef>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace crazy {

// ============================================================
// Math
// ============================================================

Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
Vec3 operator*(float s, Vec3 a) { return a * s; }

static float degToRad(float deg) { return deg * (float)M_PI / 180.0f; }

Mat4 Mat4::identity() {
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

Mat4 Mat4::perspective(float fovDeg, float aspect, float near, float far) {
    Mat4 r{};
    float tanHalf = tanf(degToRad(fovDeg) / 2.0f);
    r.m[0]  = 1.0f / (aspect * tanHalf);
    r.m[5]  = 1.0f / tanHalf;
    r.m[10] = -(far + near) / (far - near);
    r.m[11] = -1.0f;
    r.m[14] = -(2.0f * far * near) / (far - near);
    return r;
}

Mat4 Mat4::lookAt(Vec3 eye, Vec3 center, Vec3 up) {
    Vec3 f = center - eye;
    float len = sqrtf(f.x*f.x + f.y*f.y + f.z*f.z);
    f = f * (1.0f / len);

    // r = cross(f, up) — camera's right axis
    Vec3 r;
    r.x = f.y * up.z - f.z * up.y;
    r.y = f.z * up.x - f.x * up.z;
    r.z = f.x * up.y - f.y * up.x;
    len = sqrtf(r.x*r.x + r.y*r.y + r.z*r.z);
    r = r * (1.0f / len);

    // u = cross(r, f) — true up
    Vec3 u;
    u.x = r.y * f.z - r.z * f.y;
    u.y = r.z * f.x - r.x * f.z;
    u.z = r.x * f.y - r.y * f.x;

    Mat4 rm = identity();
    rm.m[0] = r.x;  rm.m[4] = r.y;  rm.m[8]  = r.z;
    rm.m[1] = u.x;  rm.m[5] = u.y;  rm.m[9]  = u.z;
    rm.m[2] = -f.x; rm.m[6] = -f.y; rm.m[10] = -f.z;
    rm.m[12] = -(r.x*eye.x + r.y*eye.y + r.z*eye.z);
    rm.m[13] = -(u.x*eye.x + u.y*eye.y + u.z*eye.z);
    rm.m[14] =  (f.x*eye.x + f.y*eye.y + f.z*eye.z);
    return rm;
}

Mat4 Mat4::translate(Vec3 t) {
    Mat4 r = identity();
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
    return r;
}

Mat4 Mat4::rotate(float angleDeg, Vec3 axis) {
    float a = degToRad(angleDeg);
    float c = cosf(a), s = sinf(a);
    float len = sqrtf(axis.x*axis.x + axis.y*axis.y + axis.z*axis.z);
    if (len < 1e-6f) return identity();  // zero-length axis: no rotation
    axis = axis * (1.0f / len);
    float x = axis.x, y = axis.y, z = axis.z;

    Mat4 r = identity();
    r.m[0] = c + x*x*(1-c);     r.m[4] = x*y*(1-c) - z*s;  r.m[8]  = x*z*(1-c) + y*s;
    r.m[1] = y*x*(1-c) + z*s;   r.m[5] = c + y*y*(1-c);     r.m[9]  = y*z*(1-c) - x*s;
    r.m[2] = z*x*(1-c) - y*s;   r.m[6] = z*y*(1-c) + x*s;   r.m[10] = c + z*z*(1-c);
    return r;
}

Mat4 Mat4::scale(Vec3 s) {
    Mat4 r{};
    r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z; r.m[15] = 1.0f;
    return r;
}

Mat4 Mat4::operator*(const Mat4& b) const {
    Mat4 r{};
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0;
            for (int k = 0; k < 4; k++) {
                sum += m[k*4 + row] * b.m[col*4 + k];
            }
            r.m[col*4 + row] = sum;
        }
    }
    return r;
}

// ============================================================
// Attribute helpers
// ============================================================

namespace {
bool validateAttrSlot(int slot, const char* fnName) {
    if (slot < 0 || slot > 2) {
        std::cerr << "[CrazyEngine] " << fnName
                  << ": slot " << slot << " is out of range [0,2]"
                  << " (engine binds locations 3, 4, 5). Ignored.\n";
        return false;
    }
    return true;
}
} // namespace

void setAttribute(std::vector<Vertex>& verts, int slot,
                  const std::vector<Vec3>& data) {
    if (!validateAttrSlot(slot, "setAttribute(Vec3)")) return;
    size_t n = std::min(verts.size(), data.size());
    for (size_t i = 0; i < n; ++i) {
        verts[i].extras[slot].x = data[i].x;
        verts[i].extras[slot].y = data[i].y;
        verts[i].extras[slot].z = data[i].z;
        verts[i].extras[slot].w = 0.0f;
    }
    if (data.size() != verts.size()) {
        std::cerr << "[CrazyEngine] setAttribute(Vec3): data.size() "
                  << data.size() << " != verts.size() " << verts.size()
                  << "; remaining vertices keep prior value.\n";
    }
}

void setAttribute(std::vector<Vertex>& verts, int slot,
                  const std::vector<Vec2>& data) {
    if (!validateAttrSlot(slot, "setAttribute(Vec2)")) return;
    size_t n = std::min(verts.size(), data.size());
    for (size_t i = 0; i < n; ++i) {
        verts[i].extras[slot].x = data[i].x;
        verts[i].extras[slot].y = data[i].y;
        verts[i].extras[slot].z = 0.0f;
        verts[i].extras[slot].w = 0.0f;
    }
    if (data.size() != verts.size()) {
        std::cerr << "[CrazyEngine] setAttribute(Vec2): data.size() "
                  << data.size() << " != verts.size() " << verts.size()
                  << "; remaining vertices keep prior value.\n";
    }
}

void setAttribute(std::vector<Vertex>& verts, int slot,
                  const std::vector<float>& data) {
    if (!validateAttrSlot(slot, "setAttribute(float)")) return;
    size_t n = std::min(verts.size(), data.size());
    for (size_t i = 0; i < n; ++i) {
        verts[i].extras[slot].x = data[i];
        verts[i].extras[slot].y = 0.0f;
        verts[i].extras[slot].z = 0.0f;
        verts[i].extras[slot].w = 0.0f;
    }
    if (data.size() != verts.size()) {
        std::cerr << "[CrazyEngine] setAttribute(float): data.size() "
                  << data.size() << " != verts.size() " << verts.size()
                  << "; remaining vertices keep prior value.\n";
    }
}

// ============================================================
// Texture
// ============================================================

static GLenum textureFormatForChannels(int channels) {
    switch (channels) {
        case 1: return GL_RED;
        case 2: return GL_RG;
        case 3: return GL_RGB;
        case 4: return GL_RGBA;
        default: return GL_RGB;
    }
}

static GLenum textureInternalFormatForChannels(int channels) {
    switch (channels) {
        case 1: return GL_R8;
        case 2: return GL_RG8;
        case 3: return GL_RGB8;
        case 4: return GL_RGBA8;
        default: return GL_RGB8;
    }
}

Texture::~Texture() {
    if (m_id) glDeleteTextures(1, &m_id);
}

Texture::Texture(Texture&& other) noexcept
    : m_id(other.m_id), m_width(other.m_width), m_height(other.m_height), m_channels(other.m_channels) {
    other.m_id = 0;
    other.m_width = other.m_height = other.m_channels = 0;
}

Texture& Texture::operator=(Texture&& other) noexcept {
    if (this != &other) {
        if (m_id) glDeleteTextures(1, &m_id);
        m_id = other.m_id;
        m_width  = other.m_width;
        m_height = other.m_height;
        m_channels = other.m_channels;
        other.m_id = 0;
        other.m_width = other.m_height = other.m_channels = 0;
    }
    return *this;
}

bool Texture::loadFromFile(const std::string& path, bool flipVertically) {
    stbi_set_flip_vertically_on_load(flipVertically);
    int w = 0, h = 0, channels = 0;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &channels, 0);
    if (!data) {
        std::cerr << "[CrazyEngine] Failed to load texture: " << path
                  << " (" << (stbi_failure_reason() ? stbi_failure_reason() : "unknown") << ")\n";
        return false;
    }
    bool ok = loadFromPixels(w, h, channels, data);
    stbi_image_free(data);
    return ok;
}

bool Texture::loadFromPixels(int width, int height, int channels, const unsigned char* data) {
    if (!data || width <= 0 || height <= 0) return false;
    if (channels < 1 || channels > 4) {
        std::cerr << "[CrazyEngine] Unsupported channel count: " << channels << "\n";
        return false;
    }

    if (m_id) glDeleteTextures(1, &m_id);
    m_width    = width;
    m_height   = height;
    m_channels = channels;

    glGenTextures(1, &m_id);
    bind();  // bind to GL_TEXTURE0 by default

    GLenum format         = textureFormatForChannels(channels);
    GLenum internalFormat = textureInternalFormatForChannels(channels);

    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, GL_UNSIGNED_BYTE, data);
    glGenerateMipmap(GL_TEXTURE_2D);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    return true;
}

void Texture::bind(int unit) const {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, m_id);
}

void Texture::unbind() const {
    glBindTexture(GL_TEXTURE_2D, 0);
}

Texture Texture::checker(int size, int cellSize) {
    Texture tex;
    if (size < 1) size = 1;
    if (cellSize < 1) cellSize = 1;
    std::vector<unsigned char> pixels((size_t)size * size * 4);
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            size_t i = ((size_t)y * size + x) * 4;
            bool white = ((x / cellSize) + (y / cellSize)) % 2 == 0;
            unsigned char c = white ? 255 : 50;
            pixels[i + 0] = c;
            pixels[i + 1] = c;
            pixels[i + 2] = c;
            pixels[i + 3] = 255;
        }
    }
    tex.loadFromPixels(size, size, 4, pixels.data());
    return tex;
}

Texture Texture::solid(int width, int height, Vec3 color) {
    Texture tex;
    if (width < 1 || height < 1) return tex;
    std::vector<unsigned char> pixels((size_t)width * height * 4);
    for (int i = 0; i < width * height; i++) {
        pixels[i * 4 + 0] = (unsigned char)(color.x * 255.0f);
        pixels[i * 4 + 1] = (unsigned char)(color.y * 255.0f);
        pixels[i * 4 + 2] = (unsigned char)(color.z * 255.0f);
        pixels[i * 4 + 3] = 255;
    }
    tex.loadFromPixels(width, height, 4, pixels.data());
    return tex;
}

// ============================================================
// Shader
// ============================================================

static std::string readFile(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "[CrazyEngine] Failed to open file: " << path << "\n";
        return "";
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

Shader::~Shader() {
    if (m_id) glDeleteProgram(m_id);
}

Shader::Shader(Shader&& other) noexcept : m_id(other.m_id) {
    other.m_id = 0;
}

Shader& Shader::operator=(Shader&& other) noexcept {
    if (this != &other) {
        if (m_id) glDeleteProgram(m_id);
        m_id = other.m_id;
        other.m_id = 0;
    }
    return *this;
}

uint32_t Shader::compileShader(uint32_t type, const char* source) {
    uint32_t id = glCreateShader(type);
    glShaderSource(id, 1, &source, nullptr);
    glCompileShader(id);

    int ok;
    glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(id, 512, nullptr, log);
        std::cerr << "[CrazyEngine] Shader compile error:\n" << log << "\n";
        glDeleteShader(id);
        return 0;
    }
    return id;
}

bool Shader::loadFromFile(const std::string& vertPath, const std::string& fragPath) {
    return loadFromSource(readFile(vertPath), readFile(fragPath));
}

bool Shader::loadFromFile(const std::string& vertPath, const std::string& geomPath, const std::string& fragPath) {
    return loadFromSource(readFile(vertPath), readFile(geomPath), readFile(fragPath));
}

bool Shader::loadFromESL(const std::string& eslPath) {
    if (m_id) { glDeleteProgram(m_id); m_id = 0; }

    std::string src = readFile(eslPath);
    if (src.empty()) return false;

    auto result = esl::transpile(src);
    if (!result.ok) {
        std::cerr << "[CrazyEngine] ESL transpile error in " << eslPath << ":\n  "
                  << result.error << "\n";
        return false;
    }

    if (result.geometry.empty()) {
        return loadFromSource(result.vertex, result.fragment);
    }
    return loadFromSource(result.vertex, result.geometry, result.fragment);
}

bool Shader::loadFromSource(const std::string& vertSrc, const std::string& fragSrc) {
    uint32_t v = compileShader(GL_VERTEX_SHADER,   vertSrc.c_str());
    uint32_t f = compileShader(GL_FRAGMENT_SHADER, fragSrc.c_str());
    if (!v || !f) {
        if (v) glDeleteShader(v);
        if (f) glDeleteShader(f);
        return false;
    }

    if (m_id) { glDeleteProgram(m_id); m_id = 0; }
    m_id = glCreateProgram();
    glAttachShader(m_id, v);
    glAttachShader(m_id, f);
    glLinkProgram(m_id);

    int ok;
    glGetProgramiv(m_id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(m_id, 512, nullptr, log);
        std::cerr << "[CrazyEngine] Shader link error:\n" << log << "\n";
        glDeleteProgram(m_id);
        m_id = 0;
    }

    glDeleteShader(v);
    glDeleteShader(f);
    return m_id != 0;
}

bool Shader::loadFromSource(const std::string& vertSrc, const std::string& geomSrc, const std::string& fragSrc) {
    uint32_t v = compileShader(GL_VERTEX_SHADER,   vertSrc.c_str());
    uint32_t g = geomSrc.empty() ? 0 : compileShader(GL_GEOMETRY_SHADER, geomSrc.c_str());
    uint32_t f = compileShader(GL_FRAGMENT_SHADER, fragSrc.c_str());
    if (!v || !g || !f) {
        if (v) glDeleteShader(v);
        if (g) glDeleteShader(g);
        if (f) glDeleteShader(f);
        return false;
    }

    if (m_id) { glDeleteProgram(m_id); m_id = 0; }
    m_id = glCreateProgram();
    glAttachShader(m_id, v);
    if (g) glAttachShader(m_id, g);
    glAttachShader(m_id, f);
    glLinkProgram(m_id);

    int ok;
    glGetProgramiv(m_id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(m_id, 512, nullptr, log);
        std::cerr << "[CrazyEngine] Shader link error:\n" << log << "\n";
        glDeleteProgram(m_id);
        m_id = 0;
    }

    glDeleteShader(v);
    if (g) glDeleteShader(g);
    glDeleteShader(f);
    return m_id != 0;
}

void Shader::use() const { glUseProgram(m_id); }

void Shader::setInt(const std::string& name, int value) const {
    glUniform1i(glGetUniformLocation(m_id, name.c_str()), value);
}

void Shader::setFloat(const std::string& name, float value) const {
    glUniform1f(glGetUniformLocation(m_id, name.c_str()), value);
}

void Shader::setVec3(const std::string& name, Vec3 v) const {
    glUniform3f(glGetUniformLocation(m_id, name.c_str()), v.x, v.y, v.z);
}

void Shader::setMat4(const std::string& name, const Mat4& m) const {
    glUniformMatrix4fv(glGetUniformLocation(m_id, name.c_str()), 1, GL_FALSE, m.ptr());
}

void Shader::setTexture(const std::string& name, const Texture& tex, int unit) const {
    tex.bind(unit);
    glUniform1i(glGetUniformLocation(m_id, name.c_str()), unit);
}

// ============================================================
// Mesh
// ============================================================

Mesh::~Mesh() {
    releaseGPU();
}

Mesh::Mesh(Mesh&& other) noexcept
    : m_vao(other.m_vao), m_vbo(other.m_vbo), m_ebo(other.m_ebo), m_indexCount(other.m_indexCount), m_vertexCount(other.m_vertexCount) {
    other.m_vao = other.m_vbo = other.m_ebo = 0;
    other.m_indexCount = 0;
    other.m_vertexCount = 0;
}

Mesh& Mesh::operator=(Mesh&& other) noexcept {
    if (this != &other) {
        if (m_ebo) glDeleteBuffers(1, &m_ebo);
        if (m_vbo) glDeleteBuffers(1, &m_vbo);
        if (m_vao) glDeleteVertexArrays(1, &m_vao);
        m_vao = other.m_vao; m_vbo = other.m_vbo; m_ebo = other.m_ebo; m_indexCount = other.m_indexCount; m_vertexCount = other.m_vertexCount;
        other.m_vao = other.m_vbo = other.m_ebo = 0;
        other.m_indexCount = 0;
        other.m_vertexCount = 0;
    }
    return *this;
}

void Mesh::releaseGPU() {
    if (m_ebo) glDeleteBuffers(1, &m_ebo);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    m_vao = m_vbo = m_ebo = 0;
}

void Mesh::upload(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices) {
    // Free any existing GPU resources to avoid leaking them on re-upload
    if (m_vao || m_vbo || m_ebo) releaseGPU();

    m_indexCount = (uint32_t)indices.size();
    m_vertexCount = (uint32_t)vertices.size();

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);

    glBindVertexArray(m_vao);

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(), GL_STATIC_DRAW);

    // position: location 0
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, position));

    // color: location 1
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, color));

    // uv: location 2
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, uv));

    // User-declared custom attributes: 3 generic Vec4 slots at locations 3, 4, 5.
    // OpenGL happily maps a vec2/vec3/float declared in the shader onto the
    // subset of components of a Vec4 buffer, so any single-attribute TYPE
    // (point, color, point2d, uv_coord, number) can drive a slot here.
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, extras[0]));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, extras[1]));
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, extras[2]));

    if (m_indexCount > 0) {
        glGenBuffers(1, &m_ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint32_t), indices.data(), GL_STATIC_DRAW);
    }

    glBindVertexArray(0);
}

void Mesh::draw() const {
    glBindVertexArray(m_vao);
    if (m_indexCount > 0)
        glDrawElements(GL_TRIANGLES, m_indexCount, GL_UNSIGNED_INT, nullptr);
    else
        glDrawArrays(GL_TRIANGLES, 0, m_vertexCount);
    glBindVertexArray(0);
}

Mesh Mesh::triangle() {
    Mesh m;
    std::vector<Vertex> v = {
        {{-0.5f, -0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
        {{ 0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
        {{ 0.0f,  0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.5f, 1.0f}},
    };
    // Stamp face-aligned normal (slot 0 -> location 3 -> 'n' attribute in ESL)
    setAttribute(v, 0, std::vector<Vec3>(3, Vec3{0.0f, 0.0f, 1.0f}));
    m.upload(v);
    return m;
}

Mesh Mesh::quad() {
    Mesh m;
    std::vector<Vertex> v = {
        {{-0.5f, -0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
        {{ 0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
        {{ 0.5f,  0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
        {{-0.5f,  0.5f, 0.0f}, {1.0f, 1.0f, 0.0f}, {0.0f, 1.0f}},
    };
    setAttribute(v, 0, std::vector<Vec3>(4, Vec3{0.0f, 0.0f, 1.0f}));
    m.upload(v, {0, 1, 2, 2, 3, 0});
    return m;
}

Mesh Mesh::cube() {
    Mesh m;
    // 6 faces * 4 verts = 24 vertices so each face has its own UVs
    std::vector<Vertex> v = {
        // Front (z=+0.5)
        {{-0.5f, -0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
        {{ 0.5f, -0.5f,  0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
        {{ 0.5f,  0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
        {{-0.5f,  0.5f,  0.5f}, {1.0f, 1.0f, 0.0f}, {0.0f, 1.0f}},
        // Back (z=-0.5) — x mirrored so it doesn't look backwards
        {{ 0.5f, -0.5f, -0.5f}, {0.0f, 1.0f, 1.0f}, {0.0f, 0.0f}},
        {{-0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
        {{-0.5f,  0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}, {1.0f, 1.0f}},
        {{ 0.5f,  0.5f, -0.5f}, {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f}},
        // Left (x=-0.5)
        {{-0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
        {{-0.5f, -0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}},
        {{-0.5f,  0.5f,  0.5f}, {1.0f, 1.0f, 0.0f}, {1.0f, 1.0f}},
        {{-0.5f,  0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}, {0.0f, 1.0f}},
        // Right (x=+0.5)
        {{ 0.5f, -0.5f,  0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
        {{ 0.5f, -0.5f, -0.5f}, {0.0f, 1.0f, 1.0f}, {1.0f, 0.0f}},
        {{ 0.5f,  0.5f, -0.5f}, {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f}},
        {{ 0.5f,  0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
        // Top (y=+0.5)
        {{-0.5f,  0.5f,  0.5f}, {1.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
        {{ 0.5f,  0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
        {{ 0.5f,  0.5f, -0.5f}, {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f}},
        {{-0.5f,  0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}, {0.0f, 1.0f}},
        // Bottom (y=-0.5)
        {{-0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
        {{ 0.5f, -0.5f, -0.5f}, {0.0f, 1.0f, 1.0f}, {1.0f, 0.0f}},
        {{ 0.5f, -0.5f,  0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}},
        {{-0.5f, -0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}},
    };

    // 6 face-aligned normals (front, back, left, right, top, bottom).
    std::vector<Vec3> normals = {
        { 0.0f,  0.0f,  1.0f}, { 0.0f,  0.0f,  1.0f}, { 0.0f,  0.0f,  1.0f}, { 0.0f,  0.0f,  1.0f}, // front
        { 0.0f,  0.0f, -1.0f}, { 0.0f,  0.0f, -1.0f}, { 0.0f,  0.0f, -1.0f}, { 0.0f,  0.0f, -1.0f}, // back
        {-1.0f,  0.0f,  0.0f}, {-1.0f,  0.0f,  0.0f}, {-1.0f,  0.0f,  0.0f}, {-1.0f,  0.0f,  0.0f}, // left
        { 1.0f,  0.0f,  0.0f}, { 1.0f,  0.0f,  0.0f}, { 1.0f,  0.0f,  0.0f}, { 1.0f,  0.0f,  0.0f}, // right
        { 0.0f,  1.0f,  0.0f}, { 0.0f,  1.0f,  0.0f}, { 0.0f,  1.0f,  0.0f}, { 0.0f,  1.0f,  0.0f}, // top
        { 0.0f, -1.0f,  0.0f}, { 0.0f, -1.0f,  0.0f}, { 0.0f, -1.0f,  0.0f}, { 0.0f, -1.0f,  0.0f}, // bottom
    };
    setAttribute(v, 0, normals);

    std::vector<uint32_t> idx;
    for (uint32_t face = 0; face < 6; face++) {
        uint32_t base = face * 4;
        idx.push_back(base + 0);
        idx.push_back(base + 1);
        idx.push_back(base + 2);
        idx.push_back(base + 2);
        idx.push_back(base + 3);
        idx.push_back(base + 0);
    }

    m.upload(v, idx);
    return m;
}

// ============================================================
// Input
// ============================================================

bool Input::isKeyDown(Key key) const {
    return m_keys[(int)key];
}

bool Input::isKeyPressed(Key key) const {
    return m_keys[(int)key] && !m_prevKeys[(int)key];
}

Vec2 Input::getMousePosition() const {
    return {(float)m_mouseX, (float)m_mouseY};
}

Vec2 Input::getMouseDelta() const {
    return {(float)(m_mouseX - m_prevMouseX), (float)(m_mouseY - m_prevMouseY)};
}

void Input::update() {
    memcpy(m_prevKeys, m_keys, sizeof(m_keys));
    m_prevMouseX = m_mouseX;
    m_prevMouseY = m_mouseY;
}

// ============================================================
// Camera
// ============================================================

Mat4 Camera::viewMatrix() const {
    float yawRad = degToRad(yaw);
    float pitchRad = degToRad(pitch);

    Vec3 front;
    front.x = cosf(yawRad) * cosf(pitchRad);
    front.y = sinf(pitchRad);
    front.z = sinf(yawRad) * cosf(pitchRad);
    // normalize
    float len = sqrtf(front.x*front.x + front.y*front.y + front.z*front.z);
    front = front * (1.0f / len);

    Vec3 center = position + front;
    return Mat4::lookAt(position, center, {0.0f, 1.0f, 0.0f});
}

Mat4 Camera::projectionMatrix(float aspect) const {
    return Mat4::perspective(fov, aspect, 0.1f, 100.0f);
}

void Camera::update(const Input& input, float dt) {
    float yawRad = degToRad(yaw);
    float pitchRad = degToRad(pitch);

    Vec3 front;
    front.x = cosf(yawRad) * cosf(pitchRad);
    front.y = sinf(pitchRad);
    front.z = sinf(yawRad) * cosf(pitchRad);
    float len = sqrtf(front.x*front.x + front.y*front.y + front.z*front.z);
    front = front * (1.0f / len);

    Vec3 right;
    right.x = -sinf(yawRad);  // strafe-right vector (matches cross(front, up))
    right.y = 0.0f;
    right.z = cosf(yawRad);
    len = sqrtf(right.x*right.x + right.z*right.z);
    right = right * (1.0f / len);

    Vec3 up = {0.0f, 1.0f, 0.0f};

    float moveSpeed = speed * dt;

    if (input.isKeyDown(KEY_W)) position = position + front * moveSpeed;
    if (input.isKeyDown(KEY_S)) position = position - front * moveSpeed;
    if (input.isKeyDown(KEY_A)) position = position - right * moveSpeed;
    if (input.isKeyDown(KEY_D)) position = position + right * moveSpeed;
    if (input.isKeyDown(KEY_SPACE)) position = position + up * moveSpeed;
    if (input.isKeyDown(KEY_LEFT_SHIFT)) position = position - up * moveSpeed; // move down

    // Mouse look
    Vec2 delta = input.getMouseDelta();
    yaw   += delta.x * sensitivity;
    pitch -= delta.y * sensitivity;  // mouse Y is screen-down, pitch positive is look-up

    // Clamp pitch
    if (pitch >  89.0f) pitch =  89.0f;
    if (pitch < -89.0f) pitch = -89.0f;
}

// ============================================================
// Engine
// ============================================================

static void framebufferSizeCallback(GLFWwindow* /*win*/, int width, int height) {
    glViewport(0, 0, width, height);
}

Engine::Engine(const WindowProps& props) : m_width(props.width), m_height(props.height) {
    if (!glfwInit()) {
        std::cerr << "[CrazyEngine] Failed to initialize GLFW\n";
        return;
    }
    m_glfwInitialized = true;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_RESIZABLE, props.resizable ? GLFW_TRUE : GLFW_FALSE);

    m_window = glfwCreateWindow(m_width, m_height, props.title.c_str(), nullptr, nullptr);
    if (!m_window) {
        std::cerr << "[CrazyEngine] Failed to create GLFW window\n";
        return;  // destructor handles glfwTerminate()
    }

    glfwMakeContextCurrent(m_window);
    glfwSetWindowUserPointer(m_window, this);
    glfwSetFramebufferSizeCallback(m_window, framebufferSizeCallback);

    // Hide and capture cursor for FPS-style mouse look
    glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    if (!gladLoadGL()) {
        std::cerr << "[CrazyEngine] Failed to initialize GLAD\n";
        return;
    }

    glEnable(GL_DEPTH_TEST);
    glViewport(0, 0, m_width, m_height);

    std::cout << "[CrazyEngine] OpenGL " << glGetString(GL_VERSION) << "\n";
    std::cout << "[CrazyEngine] Renderer: " << glGetString(GL_RENDERER) << "\n";

    m_lastTime = (float)glfwGetTime();
}

Engine::~Engine() {
    if (m_window) glfwDestroyWindow(m_window);
    if (m_glfwInitialized) glfwTerminate();
}

bool Engine::running() const {
    return m_window && !glfwWindowShouldClose(m_window);
}

void Engine::beginFrame() {
    float now = (float)glfwGetTime();
    m_dt = now - m_lastTime;
    m_lastTime = now;
    m_time += m_dt;

    glfwPollEvents();
    m_input.update();

    // Update key states
    for (int i = 0; i <= GLFW_KEY_LAST; i++) {
        m_input.m_keys[i] = glfwGetKey(m_window, i) == GLFW_PRESS;
    }

    // Update mouse position
    glfwGetCursorPos(m_window, &m_input.m_mouseX, &m_input.m_mouseY);

    glClearColor(m_bgR, m_bgG, m_bgB, m_bgA);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Update window size
    glfwGetFramebufferSize(m_window, &m_width, &m_height);
}

void Engine::endFrame() {
    glfwSwapBuffers(m_window);
}

void Engine::setBackgroundColor(float r, float g, float b, float a) {
    m_bgR = r; m_bgG = g; m_bgB = b; m_bgA = a;
}

} // namespace crazy
