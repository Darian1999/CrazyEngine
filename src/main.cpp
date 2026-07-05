#include <crazyengine/engine.h>
#include <iostream>

int main() {
    crazy::Engine engine({"CrazyEngine Demo", 800, 600});
    engine.setBackgroundColor(0.1f, 0.1f, 0.15f);

    // Load shaders — single .esl file auto-transpiled to GLSL by the engine.
    // We use two prefabs: basic.esl for the textured-but-unlit objects, and
    // blinn_phong.esl for the lit cube so the Blinn-Phong effect is visible.
    crazy::Shader basicShader;
    if (!basicShader.loadFromESL("shaders/basic.esl")) {
        std::cerr << "Failed to load basic shader!\n";
        return -1;
    }
    crazy::Shader blinnPhongShader;
    if (!blinnPhongShader.loadFromESL("shaders/blinn_phong.esl")) {
        std::cerr << "Failed to load Blinn-Phong shader!\n";
        return -1;
    }
    crazy::Shader cookTorranceShader;
    if (!cookTorranceShader.loadFromESL("shaders/cook_torrance.esl")) {
        std::cerr << "Failed to load Cook-Torrance shader!\n";
        return -1;
    }

    // Blinn-Phong static lighting parameters (set once; the camera position
    // is updated per frame inside the draw loop).
    const crazy::Vec3 kLightDir   = {0.3f, 1.0f, 0.5f};   // toward light, world space
    const crazy::Vec3 kLightColor = {1.0f, 0.95f, 0.9f};  // slightly warm white
    const float      kAmbient     = 0.15f;
    const float      kShininess   = 32.0f;

    // Cook-Torrance material parameters: a brushed-metal look that contrasts
    // strongly with the Blinn-Phong cube (which is non-metallic with a sharp
    // highlight). Metalness=1.0 -> F0 == baseColor -> the texture colors
    // become the specular tint, so the checker pattern shows up as tinted
    // reflections rather than a tinted diffuse term.
    const float      kRoughness   = 0.25f;
    const float      kMetalness   = 1.0f;

    // Create meshes
    crazy::Mesh cube   = crazy::Mesh::cube();
    crazy::Mesh tri    = crazy::Mesh::triangle();
    crazy::Mesh plane  = crazy::Mesh::quad();

    // Create textures procedurally so the demo works without any image files
    crazy::Texture checker = crazy::Texture::checker(256, 32);
    crazy::Texture solidRed = crazy::Texture::solid(64, 64, {1.0f, 0.3f, 0.3f});
    (void)solidRed;  // example API: a flat color texture

    // Camera
    crazy::Camera camera;
    camera.position = {0.0f, 1.0f, 5.0f};
    camera.speed = 5.0f;

    bool cursorLocked = true;

    std::cout << "=== CrazyEngine Demo ===\n";
    std::cout << "WASD = move, Space = up, Mouse = look\n";
    std::cout << "Esc = toggle cursor lock, Close window = quit\n";

    while (engine.running()) {
        engine.beginFrame();

        if (engine.input().isKeyPressed(crazy::KEY_ESCAPE)) {
            cursorLocked = !cursorLocked;
            glfwSetInputMode(engine.window(), GLFW_CURSOR,
                cursorLocked ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
        }

        if (cursorLocked) {
            camera.update(engine.input(), engine.deltaTime());
        }

        float t = engine.time();

        // --- Basic textured (unlit) draw setup ---
        basicShader.use();
        basicShader.setMat4("view", camera.viewMatrix());
        basicShader.setMat4("projection", camera.projectionMatrix(engine.aspect()));
        basicShader.setTexture("uTexture", checker);

        // --- Textured floor ---
        {
            crazy::Mat4 model = crazy::Mat4::translate({0.0f, -1.0f, 0.0f});
            model = model * crazy::Mat4::rotate(-90.0f, {1.0f, 0.0f, 0.0f});
            model = model * crazy::Mat4::scale({5.0f, 5.0f, 1.0f});
            basicShader.setMat4("model", model);
            plane.draw();
        }

        // --- Cook-Torrance lit, textured, spinning cube (PBR metallic) ---
        {
            crazy::Mat4 model = crazy::Mat4::translate({1.5f, 0.0f, 0.0f});
            model = model * crazy::Mat4::rotate(t * 30.0f, {1.0f, 1.0f, 0.0f});

            cookTorranceShader.use();
            cookTorranceShader.setMat4("model", model);
            cookTorranceShader.setMat4("view", camera.viewMatrix());
            cookTorranceShader.setMat4("projection", camera.projectionMatrix(engine.aspect()));
            cookTorranceShader.setVec3("uLightDir", kLightDir);
            cookTorranceShader.setVec3("uLightColor", kLightColor);
            cookTorranceShader.setVec3("uViewPos", camera.position);  // updated per frame
            cookTorranceShader.setFloat("uRoughness", kRoughness);
            cookTorranceShader.setFloat("uMetalness", kMetalness);
            cookTorranceShader.setTexture("uTexture", checker);
            cube.draw();
        }

        // --- Blinn-Phong lit, textured, spinning cube ---
        {
            crazy::Mat4 model = crazy::Mat4::translate({-1.5f, 0.0f, 0.0f});
            model = model * crazy::Mat4::rotate(t * 50.0f, {1.0f, 1.0f, 0.0f});

            blinnPhongShader.use();
            blinnPhongShader.setMat4("model", model);
            blinnPhongShader.setMat4("view", camera.viewMatrix());
            blinnPhongShader.setMat4("projection", camera.projectionMatrix(engine.aspect()));
            blinnPhongShader.setVec3("uLightDir", kLightDir);
            blinnPhongShader.setVec3("uLightColor", kLightColor);
            blinnPhongShader.setVec3("uViewPos", camera.position);  // updated per frame
            blinnPhongShader.setFloat("uAmbient", kAmbient);
            blinnPhongShader.setFloat("uShininess", kShininess);
            blinnPhongShader.setTexture("uTexture", checker);
            cube.draw();
        }

        engine.endFrame();
    }

    return 0;
}
