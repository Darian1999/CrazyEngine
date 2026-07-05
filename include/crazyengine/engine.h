#pragma once

// CrazyEngine - A simple game engine for everyone
// Just include this one header and you're good to go!

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <string>
#include <vector>
#include <cstdint>

namespace crazy {

// ============================================================
// Math (tiny, no dependencies)
// ============================================================

struct Vec2 { float x, y; };
struct Vec3 { float x, y, z; };
struct Vec4 { float x, y, z, w; };

Vec3 operator+(Vec3 a, Vec3 b);
Vec3 operator-(Vec3 a, Vec3 b);
Vec3 operator*(Vec3 a, float s);
Vec3 operator*(float s, Vec3 a);

struct Mat4 {
    float m[16] = {};

    static Mat4 identity();
    static Mat4 perspective(float fovDeg, float aspect, float near, float far);
    static Mat4 lookAt(Vec3 eye, Vec3 center, Vec3 up);
    static Mat4 translate(Vec3 t);
    static Mat4 rotate(float angleDeg, Vec3 axis);
    static Mat4 scale(Vec3 s);

    Mat4 operator*(const Mat4& b) const;
    const float* ptr() const { return m; }
};

// ============================================================
// Texture (2D only, no array / cube yet — kept simple)
// ============================================================

class Texture {
public:
    Texture() = default;
    ~Texture();
    Texture(Texture&& other) noexcept;
    Texture& operator=(Texture&& other) noexcept;
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    // Load an image from disk using stb_image.h (jpeg/png/bmp/...)
    bool loadFromFile(const std::string& path, bool flipVertically = true);

    // Upload raw pixel data (channels: 1, 2, 3, or 4)
    bool loadFromPixels(int width, int height, int channels, const unsigned char* data);

    // Bind to a texture unit (default: unit 0)
    void bind(int unit = 0) const;
    void unbind() const;

    int width()    const { return m_width; }
    int height()   const { return m_height; }
    int channels() const { return m_channels; }
    uint32_t id()  const { return m_id; }

    // Procedural textures so the demo works without image files
    static Texture checker(int size = 256, int cellSize = 32);
    static Texture solid(int width, int height, Vec3 color);

private:
    uint32_t m_id = 0;
    int m_width = 0, m_height = 0, m_channels = 0;
};

// ============================================================
// Shader
// ============================================================

class Shader {
public:
    Shader() = default;
    ~Shader();
    Shader(Shader&& other) noexcept;
    Shader& operator=(Shader&& other) noexcept;
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
    // Load from file paths (graphics pipeline = vertex + fragment)
    bool loadFromFile(const std::string& vertPath, const std::string& fragPath);
    // Load from file paths (graphics pipeline = vertex + geometry + fragment)
    bool loadFromFile(const std::string& vertPath, const std::string& geomPath, const std::string& fragPath);
    // Load from source strings (vertex + fragment)
    bool loadFromSource(const std::string& vertSrc, const std::string& fragSrc);
    // Load from source strings (vertex + geometry + fragment). Geometry may be empty to skip the geom stage.
    bool loadFromSource(const std::string& vertSrc, const std::string& geomSrc, const std::string& fragSrc);
    // Load from a single .esl (Easy Shading Language) file — automatically transpiled to GLSL
    bool loadFromESL(const std::string& eslPath);
    void use() const;
    void setInt(const std::string& name, int value) const;
    void setFloat(const std::string& name, float value) const;
    void setVec3(const std::string& name, Vec3 v) const;
    void setMat4(const std::string& name, const Mat4& m) const;
    // Bind a texture to a unit AND set the sampler uniform
    void setTexture(const std::string& name, const Texture& tex, int unit = 0) const;
    uint32_t id() const { return m_id; }

private:
    uint32_t m_id = 0;
    static uint32_t compileShader(uint32_t type, const char* source);
};

// ============================================================
// Mesh
// ============================================================

struct Vertex {
    Vec3 position;             // location 0 — built-in
    Vec3 color;                // location 1 — built-in
    Vec2 uv = {0.0f, 0.0f};    // location 2 — built-in
    Vec4 extras[3] = {         // locations 3, 4, 5 — generic user-declared attributes
        {0.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 0.0f},
    };
};

class Mesh {
public:
    Mesh() = default;
    ~Mesh();
    Mesh(Mesh&& other) noexcept;
    Mesh& operator=(Mesh&& other) noexcept;
    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;

    // Upload vertices (+ optional indices) to GPU
    void upload(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices = {});
    void draw() const;

    // Built-in shapes
    static Mesh triangle();
    static Mesh quad();
    static Mesh cube();

private:
    uint32_t m_vao = 0, m_vbo = 0, m_ebo = 0;
    uint32_t m_indexCount = 0;
    uint32_t m_vertexCount = 0;
    void releaseGPU();
};

// ============================================================
// Attribute helpers (stamp per-vertex data into Vertex::extras[slot])
// ============================================================
//
// Stamp a per-vertex attribute into Vertex::extras[slot] of every vertex in
// `verts`. `slot` must be in [0, 2] (locations 3, 4, 5 in the VAO). If
// `data` is shorter than `verts.size()`, the remaining vertices stay at the
// existing/default value. A warning is logged on the first mismatch; the
// function still returns. Out-of-range `slot` is logged and ignored.
void setAttribute(std::vector<Vertex>& verts, int slot,
                  const std::vector<Vec3>& data);
void setAttribute(std::vector<Vertex>& verts, int slot,
                  const std::vector<Vec2>& data);
void setAttribute(std::vector<Vertex>& verts, int slot,
                  const std::vector<float>& data);

// ============================================================
// Input
// ============================================================

enum Key {
    KEY_W = GLFW_KEY_W, KEY_A = GLFW_KEY_A, KEY_S = GLFW_KEY_S, KEY_D = GLFW_KEY_D,
    KEY_SPACE = GLFW_KEY_SPACE, KEY_ESCAPE = GLFW_KEY_ESCAPE,
    KEY_UP = GLFW_KEY_UP, KEY_DOWN = GLFW_KEY_DOWN,
    KEY_LEFT = GLFW_KEY_LEFT, KEY_RIGHT = GLFW_KEY_RIGHT,
    KEY_LEFT_SHIFT = GLFW_KEY_LEFT_SHIFT,
};

class Input {
public:
    bool isKeyDown(Key key) const;
    bool isKeyPressed(Key key) const; // true only on the frame the key was pressed

    Vec2 getMousePosition() const;
    Vec2 getMouseDelta() const;

private:
    friend class Engine;
    bool m_keys[GLFW_KEY_LAST + 1] = {};
    bool m_prevKeys[GLFW_KEY_LAST + 1] = {};
    double m_mouseX = 0, m_mouseY = 0;
    double m_prevMouseX = 0, m_prevMouseY = 0;
    void update();
};

// ============================================================
// Camera
// ============================================================

class Camera {
public:
    Vec3 position = {0.0f, 0.0f, 3.0f};
    float yaw = -90.0f;   // degrees
    float pitch = 0.0f;
    float speed = 3.0f;
    float sensitivity = 0.1f;
    float fov = 45.0f;

    Mat4 viewMatrix() const;
    Mat4 projectionMatrix(float aspect) const;

    // Call once per frame with input to move/look around
    void update(const Input& input, float dt);
};

// ============================================================
// Engine (the main thing)
// ============================================================

struct WindowProps {
    std::string title = "CrazyEngine";
    int width = 800;
    int height = 600;
    bool resizable = true;
};

class Engine {
public:
    Engine(const WindowProps& props = {});
    ~Engine();

    // Returns false when the window is closed
    bool running() const;

    // Call at the start of each frame
    void beginFrame();

    // Call at the end of each frame (swaps buffers, polls events)
    void endFrame();

    // Access subsystems
    Input& input() { return m_input; }
    const Input& input() const { return m_input; }

    // Timing
    float deltaTime() const { return m_dt; }
    float time() const { return m_time; }

    // Window
    int width() const { return m_width; }
    int height() const { return m_height; }
    float aspect() const { return (float)m_width / (float)m_height; }

    GLFWwindow* window() const { return m_window; }

    void setBackgroundColor(float r, float g, float b, float a = 1.0f);

private:
    GLFWwindow* m_window = nullptr;
    bool m_glfwInitialized = false;
    int m_width, m_height;
    float m_dt = 0.0f;
    float m_time = 0.0f;
    float m_lastTime = 0.0f;
    float m_bgR = 0.15f, m_bgG = 0.15f, m_bgB = 0.2f, m_bgA = 1.0f;
    Input m_input;
};

} // namespace crazy
