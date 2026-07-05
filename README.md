# CrazyEngine

A small OpenGL 3.3 game engine with **ESL** (Easy Shading Language) — a tiny
Pythonic shading language that transpiles to GLSL `#version 330 core`. Designed
to be readable, hackable, and friendly to people learning graphics programming.

The goal of ESL is to make writing shaders feel like writing Python: no
`gl_Position`, no `vec3 / vec4 / sampler2D`, no `projection * view * model *
vec4(…)` boilerplate, no semicolons to remember.

## Features

- **ESL — Easy Shading Language** — write shaders in a Pythonic syntax with
  friendly type aliases (`point`, `color`, `point2d`, `texture`, `number`…),
  the `output` / `sample` / `transform` front door, and an optional geometry
  stage. See **[docs/ESL.md](docs/ESL.md)** for the full language reference.
- **3 prefab shaders** in `shaders/`:
  - `basic.esl` — textured-only (used by the demo's floor)
  - `blinn_phong.esl` — classic ambient + diffuse + Blinn-Phong specular with
    a sharp highlight
  - `cook_torrance.esl` — energy-conserving Cook-Torrance / GGX microfacet
    (PBR), with `roughness` / `metalness` parameters
- **User-declared vertex attributes** — up to 3 extras per vertex at locations
  3, 4, 5 for normals, tangents, per-vertex weights, etc. Stamped into
  `Vertex::extras[]` from the CPU via `crazy::setAttribute`.
- **Built-in shapes** — `Mesh::triangle()`, `Mesh::quad()`, `Mesh::cube()`,
  all pre-populated with face-aligned normals in `extras[0]`.
- **Textures** — procedural (`Texture::checker(...)`, `Texture::solid(...)`)
  and file-loaded via stb_image (PNG / JPG / BMP / TGA).
- **Camera + input** — WASD + Space/Shift movement, mouse-look, FPS-style
  cursor lock, scroll-wheel ready.
- **Bullet physics bindings** — beginner-friendly `crazy::physics::World`,
  `RigidBody`, and shapes (`BoxShape`, `SphereShape`, `CapsuleShape`,
  `PlaneShape`, `MeshShape`). Pulled in via CMake `FetchContent` so users
  don't need a system Bullet install; no Bullet types leak into the public
  header — everything stays in `crazy::Vec3 / Vec4 / Mat4`.
- **Doc-driven test** — every Python-fenced code block in the cookbook section
  of `docs/ESL.md` is automatically run through `crazy::esl::transpile` at
  test time, so the docs can never silently drift from a working example.

## Quick start

### Requirements

- CMake ≥ 3.16
- A C++17 compiler (GCC, Clang, or MSVC)
- GLFW 3.3 development package
- OpenGL development package (Linux: `libgl1-mesa-dev` or equivalent;
  Windows/macOS: provided by the OS / Xcode)
- On Linux: X11 or Wayland development packages

### Build & run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/demo
```

The demo renders three objects side by side:

| Object                | Shader                   | Visual                                              |
|-----------------------|--------------------------|-----------------------------------------------------|
| Textured floor        | `basic.esl`              | Flat checker pattern, no lighting                   |
| Spinning cube (left)  | `blinn_phong.esl`        | Sharp white specular highlight                      |
| Spinning cube (right) | `cook_torrance.esl`      | Brushed metal — F0 tinted by the texture, GGX NDF   |

### Controls

| Key                       | Action           |
|---------------------------|------------------|
| `W` / `A` / `S` / `D`     | Move             |
| `Space`                   | Up               |
| `Left Shift`              | Down             |
| `Mouse`                   | Look around      |
| `Esc`                     | Release cursor   |

## Project layout

```
crazyEngine/
├── CMakeLists.txt              # build config (also fetches Bullet 3.25)
├── README.md                   # you are here
├── .gitignore
├── src/
│   ├── main.cpp                # demo entry point
│   ├── engine.cpp              # Engine, Shader, Mesh, Texture, Camera, Input
│   ├── esl.cpp                 # ESL → GLSL transpiler
│   └── physics.cpp             # Bullet bindings (only file exposing Bullet types)
├── include/crazyengine/
│   ├── engine.h                # public engine API
│   ├── esl.h                   # public ESL API
│   └── physics.h               # public physics API (no Bullet types)
├── shaders/
│   ├── basic.esl               # textured-only prefab
│   ├── blinn_phong.esl         # classic Phong/Blinn lighting
│   └── cook_torrance.esl       # PBR (GGX) lighting
├── docs/
│   └── ESL.md                  # full ESL language reference
└── tests/
    ├── esl_cookbook_test.cpp   # runs every docs example through the transpiler
    └── physics_test.cpp        # headless test for the Bullet bindings
```

## Writing your own shader

The shortest possible `.esl` file (a flat-shaded mesh using the built-in
vertex `color`):

```python
def vertex():
    output = transform(position)

def fragment():
    output = color
```

A textured variant with a per-vertex normal (the foundation for any lighting
model) is just a few more lines — see the **[ESL language
reference](docs/ESL.md)** for the full grammar, type aliases, declarations,
the `output` / `sample` / `transform` substitutions, and the optional
geometry stage.

To load a `.esl` from C++:

```cpp
crazy::Shader shader;
if (!shader.loadFromESL("shaders/my_shader.esl")) {
    std::cerr << "Failed to load shader\n";
    return -1;
}
shader.use();
// ... draw calls ...
```

## Using the physics bindings

```cpp
#include <crazyengine/physics.h>

crazy::physics::World world({0.0f, -9.81f, 0.0f});        // gravity
world.addStaticBody(crazy::physics::PlaneShape({0, 1, 0}, 0.0f));

crazy::physics::BoxShape groundShape({0.5f, 0.5f, 0.5f});
auto ball = world.addDynamicBody(groundShape, /*mass=*/1.0f, {0, 5, 0});

while (running()) {
    world.step(deltaTime);
    // ball.position() returns a crazy::Vec3 you can drive straight into
    // Mat4::translate(...).
}
```

Key types (all in `crazy::physics`):

- `World` — owns the dynamics world; gravity get/set, step / stepFixed,
  body creation/removal, bodyCount.
- `RigidBody` — opaque, move-only handle. Has `position()`, `transform()`,
  `linearVelocity()` / `setLinearVelocity()`, `applyCentralForce()` /
  `applyCentralImpulse()`, kinematic-aware `setPosition()`, `type()`.
- `Shape` (abstract) — `BoxShape(SphereShape(CapsuleShape(PlaneShape(MeshShape`.
  Each is cloned into the World on body add, so originals can go out of
  scope immediately.

Bullet's own type names (`btRigidBody`, `btVector3`, ...) never appear in
`include/crazyengine/physics.h` — the cpp does all conversion under the hood.

## Running the tests

```bash
cd build
ctest --output-on-failure
```

The `esl_cookbook_test` parses `docs/ESL.md`, extracts every Python-fenced
code block under a `### 5.X` (cookbook) or `### Step X` (tutorial) heading,
and runs each one through `crazy::esl::transpile`. It then asserts that:

- `ok == true` (no parse errors)
- The generated vertex and fragment GLSL contain the expected structural
  markers (`#version 330 core`, `void main()`, `gl_Position`, `fragColor`,
  `EmitVertex()` if a geometry stage is present, etc.)
- Every declared varying and attribute shows up in the transpiled GLSL with
  the expected alias

So if you edit a cookbook example and accidentally introduce a typo, the
test catches it on the next `ctest` run.

## License

No license file is included yet — by default this is "all rights reserved" by
the author. If you'd like to use this code in a project, open an issue and
we'll add a license (MIT, Unlicense, Apache-2.0, or your pick).
