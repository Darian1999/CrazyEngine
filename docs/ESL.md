# ESL — Easy Shading Language

ESL is a tiny, Pythonic shading language for CrazyEngine. A single `.esl` file
declares uniforms, varyings, and per-vertex attributes, plus the vertex and
fragment (and optionally geometry) program bodies, and is transpiled to GLSL
`#version 330 core` automatically.

The goal is to make writing shaders feel like writing Python: no `gl_Position`,
no `vec3 / vec4 / sampler2D`, no `projection * view * model * vec4(…)`
boilerplate, no semicolons to remember. Just a few lines of readable code that
compile to working GPU programs.

```python
# shaders/basic.esl
tex       = uniform texture
passColor = varying color
passUV    = varying point2d
n         = attribute point

def vertex():
    passColor = color
    passUV    = uv
    output    = transform(position)

def fragment():
    output = sample(tex, passUV) * vec4(passColor, 1.0)
```

That file is the complete demo shader. The engine reads it, transpiles it to
GLSL, compiles, links, and you're rendering.

---

## Table of Contents

1. [Quickstart — 30 seconds to a working shader](#1-quickstart--30-seconds-to-a-working-shader)
2. [Tutorial — a lit, textured cube in five steps](#2-tutorial--a-lit-textured-cube-in-five-steps)
3. [Reference](#3-reference)
   - 3.1 [File shape & grammar](#31-file-shape--grammar)
   - 3.2 [Declarations: `uniform` / `varying` / `attribute`](#32-declarations-uniform--varying--attribute)
   - 3.3 [Friendly type aliases](#33-friendly-type-aliases)
   - 3.4 [User-declared attributes (locations 3–5)](#34-user-declared-attributes-locations-35)
   - 3.5 [Stage markers](#35-stage-markers)
   - 3.6 [Built-in symbols (auto-injected)](#36-built-in-symbols-auto-injected)
   - 3.7 [Friendly syntax: `output` / `sample` / `transform`](#37-friendly-syntax-output--sample--transform)
   - 3.8 [Geometry stage (optional)](#38-geometry-stage-optional)
4. [How data flows: CPU → GPU → pixel](#4-how-data-flows-cpu--gpu--pixel)
5. [Cookbook](#5-cookbook)
6. [C++ API](#6-c-api)
7. [Limits & errors](#7-limits--errors)
8. [What the transpiler actually produces](#8-what-the-transpiler-actually-produces)
9. [Quick reference](#9-quick-reference)

---

## 1. Quickstart — 30 seconds to a working shader

Write `shaders/flat.esl`:

```python
def vertex():
    output = transform(position)

def fragment():
    output = color
```

In C++:

```cpp
crazy::Shader shader;
shader.loadFromESL("shaders/flat.esl");
shader.use();
// draw your mesh...
```

That's it. The cube (or whatever you draw) will be drawn with its per-vertex
`color` interpolated across the surface — no texture, no lighting, just
gradients. Every other feature in this document is a progressive refinement
of this minimal case.

---

## 2. Tutorial — a lit, textured cube in five steps

This builds up the demo shader piece by piece. Each step is a complete,
working `.esl` file.

### Step 1 — solid color (no texture)

```python
def vertex():
    output = transform(position)

def fragment():
    output = color
```

`position` (location 0) and `color` (location 1) are built-in vertex
attributes — no declaration needed. `transform(p)` expands to
`projection * view * model * vec4(p, 1.0)`.

### Step 2 — add a texture

```python
tex    = uniform texture
passUV = varying point2d

def vertex():
    passUV = uv
    output = transform(position)

def fragment():
    output = sample(tex, passUV)
```

`uv` is the third built-in attribute (location 2). The `varying point2d` line
declares `passUV` as a varying: it's written in the vertex stage and read in
the fragment stage, with values automatically interpolated by the GPU between
vertices. `sample(tex, passUV)` becomes `texture(tex, passUV)` after
transpile.

Don't forget to set the sampler uniform on the CPU side:

```cpp
shader.setTexture("tex", myTexture);   // binds to GL_TEXTURE0 and sets the uniform
```

### Step 3 — multiply the texture by the vertex color

```python
tex       = uniform texture
passColor = varying color
passUV    = varying point2d

def vertex():
    passColor = color
    passUV    = uv
    output    = transform(position)

def fragment():
    output = sample(tex, passUV) * vec4(passColor, 1.0)
```

`vec4(passColor, 1.0)` is the only "raw" GLSL left in the file — it promotes
the `vec3` color to a `vec4` so it can be multiplied by the texture sample
(which returns `vec4`). You can also keep the colors as `vec3` and write
`output = sample(tex, passUV).rgb * passColor`, whichever reads better.

### Step 4 — add a per-vertex normal

The engine already binds three `Vec4 extras` slots in the `Vertex` struct
(locations 3, 4, 5). You can declare a custom attribute and have the engine
fill in data for you via `crazy::setAttribute` — or use the built-in shapes
which already populate `extras[0]` with face-aligned normals.

```python
tex        = uniform texture
passColor  = varying color
passUV     = varying point2d
passNormal = varying point
n          = attribute point

def vertex():
    passColor  = color
    passUV     = uv
    passNormal = n
    output     = transform(position)

def fragment():
    # cheap directional lighting: surface is bright where its normal faces up
    float light = clamp(passNormal.z, 0.0, 1.0) * 0.5 + 0.5
    output = sample(tex, passUV) * vec4(passColor * light, 1.0)
```

`n` is auto-aliased inside the **vertex** shader, so it's available as a
plain `vec3` local there. The fragment stage does **not** see attributes
directly — you have to pipe any per-vertex data into the fragment stage
through a varying, as shown above with `passNormal`. (Reading `n` in the
fragment body would be a compile error: "undeclared identifier 'n'".)

### Step 5 — animate with time

```python
tex       = uniform texture
uTime     = uniform number
passColor = varying color
passUV    = varying point2d
n         = attribute point

def vertex():
    passColor = color
    passUV    = uv
    output    = transform(position + n * sin(uTime * 3.0) * 0.05)

def fragment():
    output = sample(tex, passUV) * vec4(passColor, 1.0)
```

CPU side, set the time each frame:

```cpp
shader.setFloat("uTime", engine.time());
```

The cube now pulses along its face normals — a free, no-attribute effect once
you have a normal to read.

That's the full progression: solid → textured → lit → animated. Every other
shader you write in ESL is a variation on this pattern.

---

## 3. Reference

### 3.1 File shape & grammar

A `.esl` file is a sequence of **top-level lines** (no indent) followed by
**stage bodies** (indented under `def …():` markers):

```python
# comment
NAME = KIND TYPE          # declaration
NAME = KIND TYPE_ALIAS    # declaration with a friendly type

def vertex():             # stage marker (begins vertex body)
    body line              # each body line is auto-terminated with ';'
    body line

def fragment():           # stage marker (begins fragment body)
    body line
```

**Rules:**

- Comments start with `#` and run to end of line. They can appear anywhere.
- Blank lines are ignored.
- Each top-level line is either a **declaration** or a **def** marker.
- Body lines must be indented with at least one space or tab.
- Statements in bodies don't need semicolons — the transpiler appends one
  unless the line already ends with `;`, `{`, or `}`. So `output = color` and
  `output = color;` are equivalent.
- Indent with spaces (recommended) or tabs; just be consistent within a file.

The grammar is small enough to fit in one line:

```
file        := (toplevel | blank | comment)*
toplevel    := declaration | stage_marker
declaration := IDENT "=" ("uniform" | "varying" | "attribute") TYPE
stage_marker:= "def" ("vertex" | "geometry" | "fragment") "()" ":"
body        := (INDENTED_LINE)+
```

### 3.2 Declarations: `uniform` / `varying` / `attribute`

```
NAME = KIND TYPE
```

| Kind        | Where it lives                                | How you set it on the CPU                                              |
|-------------|-----------------------------------------------|------------------------------------------------------------------------|
| `uniform`   | Shader-global, constant for a draw call       | `shader.setMat4("NAME", value)` / `setVec3` / `setFloat` / `setInt` / `setTexture` |
| `varying`   | Vertex writes → fragment reads (auto-interp.) | _nothing_ — it's a per-vertex output                                  |
| `attribute` | Per-vertex input, VAO locations 3/4/5         | `crazy::setAttribute(verts, slot, data)` before `Mesh::upload`        |

**`NAME`** is a C-style identifier (`[A-Za-z_][A-Za-z0-9_]*`).

**`TYPE`** is a GLSL type or one of the friendly aliases below. GLSL names
(`vec3`, `sampler2D`, `mat4`, …) always work — the alias table is purely
additive.

### 3.3 Friendly type aliases

ESL ships a small set of semantic aliases that read more naturally than raw
GLSL types. They are purely cosmetic — they map 1:1 to GLSL types at
declaration time.

| Friendly name | Resolves to     | Use it for                                       |
|---------------|-----------------|--------------------------------------------------|
| `point`       | `vec3`          | positions, normals, directions                   |
| `color`       | `vec3`          | per-vertex colors (semantically equivalent to `point`) |
| `point2d`     | `vec2`          | UVs, screen-space coords                         |
| `uv_coord`    | `vec2`          | UVs specifically (semantically equivalent to `point2d`) |
| `matrix`      | `mat4`          | model / view / projection / custom transforms    |
| `texture`     | `sampler2D`     | 2D samplers                                      |
| `number`      | `float`         | scalars                                          |
| `integer`     | `int`           | integer scalars                                  |

`point` and `color` are the same type (`vec3`); the difference is purely in
the eye of the reader. Same for `point2d` / `uv_coord`. Pick whichever reads
better in context.

### 3.4 User-declared attributes (locations 3–5)

Locations 0, 1, 2 are reserved for the engine's built-in vertex inputs
(`position`, `color`, `uv`). When you need a **fourth, fifth, or sixth** input
— a normal, a tangent, a second UV set, a per-vertex weight — declare it
with `attribute`:

```python
n  = attribute point    # layout(location = 3) in vec3 n;
u2 = attribute point2d  # layout(location = 4) in vec2 u2;
w  = attribute number   # layout(location = 5) in float w;
```

ESL assigns the next free location (starting at 3) in **declaration order**.
You don't (and can't) specify a number; the engine and the shader agree
implicitly.

**Inside the vertex body, each declared attribute is available as a same-name
local variable.** The transpiler emits a `vec3 n = a_n;` alias right before
your body, so writing `n` in the vertex body "just works" — no need to
remember the `a_` prefix.

#### Per-vertex data on the CPU side

Locations 3, 4, 5 are bound in the VAO to `Vertex::extras[0]`, `extras[1]`,
`extras[2]` respectively (each a `Vec4`). Populate them with the
`crazy::setAttribute` helper before `Mesh::upload`:

```cpp
std::vector<crazy::Vertex> verts(24);
// ... fill verts[i].position / color / uv ...

std::vector<crazy::Vec3> normals(24, /* face-aligned */);
crazy::setAttribute(verts, /*slot=*/0, normals);   // location 3 → 'n' in ESL

crazy::Mesh m;
m.upload(verts, idx);
```

There are three `setAttribute` overloads — `std::vector<Vec3>`, `Vec2`, and
`float` — covering every legal per-vertex type. The unused components of the
underlying `Vec4` are zeroed automatically. See [§6.2](#62-setattribute-stamping-per-vertex-data)
for the full API.

#### Built-in shapes already ship normals

`Mesh::triangle()`, `Mesh::quad()`, and `Mesh::cube()` already populate
`extras[0]` with sensible face-aligned normals. So if you only need one
attribute, the engine has done the work for you — just declare `n = attribute
point` and use it.

#### Why three slots?

The `Vertex` struct's `extras[3]` is a fixed-size array of three `Vec4`s
(locations 3, 4, 5). OpenGL maps the shader's `vec2` / `vec3` / `float`
declaration onto the subset of components of a `Vec4` buffer, so a single
ESL attribute declaration can carry any scalar-or-vector per-vertex data
without a per-mesh remap step. Declaring a fourth attribute would fail with
a transpile error pointing at the offending line.

#### Type restrictions on attributes

For semantic sanity, attribute declarations reject:

- matrix types (`mat2`, `mat3`, `mat4`) — those go in `uniform`
- sampler types (`sampler2D`, `samplerCube`, …) — those are uniforms
- image types (`image2D`, …) — those are uniforms too
- atomic types — also uniforms

You can use `point`, `color`, `point2d`, `uv_coord`, `number`, `integer`, or
plain `vec2` / `vec3` / `vec4` / `float` / `int`.

### 3.5 Stage markers

```python
def vertex():     # vertex stage body follows
def geometry():   # optional geometry stage body follows
def fragment():   # fragment stage body follows
```

Each marker begins a block. All indented lines below it (until the next
un-indented line) are that stage's body.

- `def vertex():` is required.
- `def fragment():` is required.
- `def geometry():` is **optional** — see [§3.8](#38-geometry-stage-optional).
  When present, the pipeline becomes vertex → geometry → fragment.

### 3.6 Built-in symbols (auto-injected)

These symbols are available in every `.esl` file with no declaration needed.
The transpiler wires them up behind the scenes.

**Vertex stage:**

| Symbol          | Type     | Source                       |
|-----------------|----------|------------------------------|
| `position`      | `vec3`   | VAO location 0               |
| `color`         | `vec3`   | VAO location 1               |
| `uv`            | `vec2`   | VAO location 2               |
| `model`         | `mat4`   | uniform set per draw         |
| `view`          | `mat4`   | uniform set per draw         |
| `projection`    | `mat4`   | uniform set per draw         |
| `NAME`          | (your)   | from your `attribute` decls  |

**Geometry stage** (only when `def geometry():` is present):

| Symbol         | Type     | Source                                   |
|----------------|----------|------------------------------------------|
| `gpos[3]`      | `vec3[3]`| the 3 input vertex positions             |
| `gi`           | `int`    | current input vertex index (mutable)     |
| `EmitVertex()` | function | emit current output vertex               |
| `EndPrimitive()`| function| emit current primitive                    |
| `v_NAME[3]`    | (your)   | `in` array of the vertex stage's varyings|
| `NAME`         | (your)   | `out` declarations of the same varyings  |

**Fragment stage:**

| Symbol      | Type     | Source                                |
|-------------|----------|---------------------------------------|
| `fragColor` | `vec4`   | the single fragment output (write here with `output = …` or `fragColor = …`) |
| `gl_FragColor` | `vec4` | legacy alias (still works, mapped to `fragColor` internally) |

### 3.7 Friendly syntax: `output` / `sample` / `transform`

These three abstractions desugar to native GLSL during transpile. They are
the main reason ESL reads like Python instead of C.

#### `output = …`

Instead of writing the stage-specific GLSL output name, just write `output`:

| Stage     | `output = X` becomes     |
|-----------|--------------------------|
| vertex    | `gl_Position = X;`       |
| geometry  | `gl_Position = X;`       |
| fragment  | `fragColor = X;`         |

The legacy `gl_FragColor = …` form in fragment bodies still works (it's
auto-mapped to `fragColor` to match GLSL 330's `out` syntax requirement).

#### `sample(tex, uv)`

Instead of the GLSL built-in `texture(tex, uv)`, write `sample(tex, uv)`. It
reads more like a method call. Substitution is word-boundary aware, so
identifiers like `mySampler` or `retexture` are not affected.

```python
def fragment():
    output = sample(tex, passUV)              # basic sample
    output = sample(tex, passUV) * 2.0        # multiplied sample
    output = vec4(sample(tex, passUV).rgb, 1) # alpha from constant
```

`sample` is a pure syntactic rename; the underlying GLSL is identical.

#### `transform(point)`

In the **vertex stage only**, `transform(p)` expands to the MVP idiom:

```python
transform(p)   #   projection * view * model * vec4(p, 1.0)
```

The substitution is **paren-balanced**, so nested calls work:

```python
output = transform(position + n * 0.05)
# -> gl_Position = projection * view * model * vec4(position + n * 0.05, 1.0)
```

You can use the expansion directly if you need a custom transform (e.g. only
view-projection, no model): `output = projection * view * vec4(position, 1.0)`.

#### `transform` is vertex-only

If you use it in `def fragment():` or `def geometry():`, the transpiler will
pass it through unchanged and the GLSL compiler will complain that
`projection` is undefined. It's defined in the vertex and geometry stages
(where it's auto-injected) but not the fragment stage.

### 3.8 Geometry stage (optional)

Add `def geometry():` to insert a stage between vertex and fragment. The
default config is:

```glsl
layout(triangles) in;
layout(triangle_strip, max_vertices = 6) out;
```

So you receive 3 input vertices per primitive and may emit up to 6 output
vertices. This is enough for the canonical "explode a triangle into 3
outward-pushed triangles" effect (3 verts in, 9 verts out — but you re-emit
each as a 1-vertex primitive via `EndPrimitive()`, giving you 3 * 1-vert
primitives, or alternatively emit a single 3-vertex `triangle_strip`).

#### How varyings cross the geometry stage

Varyings you declared at the top level are received as `in TYPE v_NAME[3]`
inside the geometry body (the `v_` prefix is just to avoid a name collision
with the same-type `out TYPE NAME` declaration in the same scope). You are
responsible for routing the data per vertex. The canonical pattern:

```python
tex       = uniform texture
passColor = varying color
passUV    = varying point2d

def vertex():
    passColor = color
    passUV    = uv
    output    = transform(position)

def geometry():
    # Compute the centroid of the input triangle
    vec3 mid = (gpos[0] + gpos[1] + gpos[2]) / 3.0
    vec3 lift = vec3(0.0, 0.4, 0.0)

    # Re-emit each vertex slightly lifted above the centroid
    gi = 0
    passColor = v_passColor[gi]
    passUV    = v_passUV[gi]
    output    = transform(mid + (gpos[gi] - mid) * 1.3 + lift)
    EmitVertex()

    gi = 1
    passColor = v_passColor[gi]
    passUV    = v_passUV[gi]
    output    = transform(mid + (gpos[gi] - mid) * 1.3 + lift)
    EmitVertex()

    gi = 2
    passColor = v_passColor[gi]
    passUV    = v_passUV[gi]
    output    = transform(mid + (gpos[gi] - mid) * 1.3 + lift)
    EmitVertex()

    EndPrimitive()

def fragment():
    output = sample(tex, passUV) * vec4(passColor, 1.0)
```

The loop is unrolled because ESL bodies don't have a `for` keyword, and a raw
GLSL `for (...):` header would get a stray `;` appended by the auto-semicolon
injector (the `:` isn't in the skip set of `;`, `{`, `}`), producing broken
GLSL. If you want to use a GLSL `for` in a body, end the header line with
`{` so the injector leaves it alone.

#### To opt out

Omit `def geometry():` entirely. `Shader::loadFromESL` will detect the empty
geometry stage and route through the 2-stage `loadFromSource(vert, frag)`
path automatically.

---

## 4. How data flows: CPU → GPU → pixel

This section explains the round trip so you know where each piece of state
comes from.

```
  CPU side                                GPU side
  -------                                 --------
  Mesh::triangle() / quad() / cube()      Vertex Shader
    │                                       │
    │ builds std::vector<Vertex>             │ reads position, color, uv
    │                                       │ reads your declared attributes
    ▼                                       │
  setAttribute(verts, slot, data)            │ writes varyings
    │ (optional — extras default to 0)       │ writes gl_Position
    ▼                                       ▼
  mesh.upload(verts, indices)             Rasterizer
    │ uploads to VBO/EBO                     │ (interpolates varyings)
    │ sets up VAO with attribute pointers     ▼
    │   0 → position (3 floats)            Fragment Shader
    │   1 → color    (3 floats)              │ reads interpolated varyings
    │   2 → uv       (2 floats)              │ samples textures
    │   3 → extras[0] (4 floats)             │ writes fragColor
    │   4 → extras[1] (4 floats)             ▼
    │   5 → extras[2] (4 floats)            Framebuffer
    │                                        │ (one pixel color per fragment)
    ▼
  shader.use()
  shader.setMat4("model", M)
  shader.setMat4("view", V)
  shader.setMat4("projection", P)
  shader.setTexture("tex", tex)
  mesh.draw()  ───────────────────────►   Draw call
```

**Key facts:**

- The `Vertex` struct is the **contract between the CPU and the vertex
  shader**. Field order, types, and the auto-bound locations are fixed.
- `setAttribute(verts, slot, data)` writes into `verts[i].extras[slot]`,
  which is then uploaded as the next VAO attribute pointer (slot 0 → location
  3, slot 1 → 4, slot 2 → 5).
- The shader sees attributes as `a_NAME` (raw GLSL) but the **ESL transpiler
  auto-aliases** them to `NAME` inside the vertex body. So in your vertex
  body, just write the plain name.
- The three built-in uniforms (`model`, `view`, `projection`) are
  auto-injected — you don't declare them. The engine has to set them per
  draw call.
- Varyings are **per-vertex outputs** that the GPU interpolates linearly
  across each triangle before the fragment shader runs.

---

## 5. Cookbook

Each pattern below is a complete, drop-in `.esl` file. Adapt the names and
math to your use case.

### 5.1 Solid color (no texture)

```python
def vertex():
    output = transform(position)

def fragment():
    output = color
```

### 5.2 Textured with vertex tint

```python
tex       = uniform texture
passColor = varying color
passUV    = varying point2d

def vertex():
    passColor = color
    passUV    = uv
    output    = transform(position)

def fragment():
    output = sample(tex, passUV) * vec4(passColor, 1.0)
```

### 5.3 Cheap directional lighting

```python
uLightDir   = uniform point
tex         = uniform texture
passColor   = varying color
passUV      = varying point2d
passNormal  = varying point
n           = attribute point

def vertex():
    passColor  = color
    passUV     = uv
    passNormal = n
    output     = transform(position)

def fragment():
    float light = max(dot(passNormal, -uLightDir), 0.0)
    output = sample(tex, passUV) * vec4(passColor * (0.3 + 0.7 * light), 1.0)
```

CPU side: `shader.setVec3("uLightDir", glm::normalize(glm::vec3(0.3f, -1.0f, -0.5f)));`

### 5.4 Time-based animation

```python
uTime      = uniform number
tex        = uniform texture
passColor  = varying color
passUV     = varying point2d
n          = attribute point

def vertex():
    passColor = color
    passUV    = uv
    output    = transform(position + n * sin(uTime * 3.0) * 0.05)

def fragment():
    output = sample(tex, passUV) * vec4(passColor, 1.0)
```

CPU side: `shader.setFloat("uTime", engine.time());` each frame.

### 5.5 Two attributes (normal + tangent)

```python
n          = attribute point
t          = attribute point
passColor  = varying color
passUV     = varying point2d
passNormal = varying point

def vertex():
    passColor  = color
    passUV     = uv
    passNormal = n
    output     = transform(position + n * 0.05 + t * 0.02)

def fragment():
    output = vec4(passColor * (0.5 + 0.5 * passNormal.z), 1.0)
```

CPU side: stamp both `extras[0]` and `extras[1]` with their own data vectors.

### 5.6 Geometry explode

```python
uTime   = uniform number
tex     = uniform texture
passColor = varying color
passUV    = varying point2d

def vertex():
    passColor = color
    passUV    = uv
    output    = transform(position)

def geometry():
    vec3 mid = (gpos[0] + gpos[1] + gpos[2]) / 3.0
    vec3 lift = vec3(0.0, sin(uTime * 2.0) * 0.4, 0.0)

    gi = 0
    passColor = v_passColor[gi]
    passUV    = v_passUV[gi]
    output    = transform(mid + (gpos[gi] - mid) * 1.3 + lift)
    EmitVertex()

    gi = 1
    passColor = v_passColor[gi]
    passUV    = v_passUV[gi]
    output    = transform(mid + (gpos[gi] - mid) * 1.3 + lift)
    EmitVertex()

    gi = 2
    passColor = v_passColor[gi]
    passUV    = v_passUV[gi]
    output    = transform(mid + (gpos[gi] - mid) * 1.3 + lift)
    EmitVertex()

    EndPrimitive()

def fragment():
    output = sample(tex, passUV) * vec4(passColor, 1.0)
```

### 5.7 Postprocess / screen quad

For a fullscreen quad, your `Mesh` is just two triangles in NDC. The vertex
shader can write `gl_Position` directly with no transform at all:

```python
tex = uniform texture

def vertex():
    output = vec4(position, 1.0)   # identity: position is already in clip space

def fragment():
    output = sample(tex, uv)
```

Use this for blits, postprocess passes, etc.

---

## 6. C++ API

### 6.1 Loading an `.esl` file

```cpp
crazy::Shader shader;
if (!shader.loadFromESL("shaders/basic.esl")) {
    std::cerr << "ESL transpile / link failed\n";
    return -1;
}
shader.use();
```

`loadFromESL` reads the file, runs `crazy::esl::transpile` on it, and routes
to either the 2-stage (`vert + frag`) or 3-stage (`vert + geom + frag`)
`loadFromSource` path based on whether the transpiled geometry stage is
empty. Compile and link errors from the GLSL stage are printed to `stderr`.

If you want to inspect the generated GLSL (debugging / hot-reload), call
`crazy::esl::transpile` directly:

```cpp
#include <crazyengine/esl.h>

crazy::esl::Result r = crazy::esl::transpile(source);
if (!r.ok) {
    std::cerr << "ESL error: " << r.error << "\n";
} else {
    std::cout << "=== VERTEX ===\n" << r.vertex << "\n";
    std::cout << "=== FRAGMENT ===\n" << r.fragment << "\n";
    if (!r.geometry.empty()) {
        std::cout << "=== GEOMETRY ===\n" << r.geometry << "\n";
    }
}
```

`crazy::esl::Result` is `{ vertex, fragment, geometry, ok, error }`. When
`ok` is `false`, `error` holds a human-readable message including the line
number.

### 6.2 `setAttribute`: stamping per-vertex data

Three overloads, one per supported shape:

```cpp
void setAttribute(std::vector<Vertex>& verts, int slot, const std::vector<Vec3>& data);
void setAttribute(std::vector<Vertex>& verts, int slot, const std::vector<Vec2>& data);
void setAttribute(std::vector<Vertex>& verts, int slot, const std::vector<float>& data);
```

**Parameters:**

- `verts` — the vertex vector you intend to upload.
- `slot` — which `extras[i]` to write into. **Must be 0, 1, or 2** (locations
  3, 4, 5). Out-of-range values are logged to `stderr` and ignored.
- `data` — one entry per vertex, or fewer (mismatched sizes are tolerated —
  see below).

**Behavior:**

- `Vec3` data is written into `.xyz` of `extras[slot]`, with `.w` zeroed.
- `Vec2` data is written into `.xy`, with `.z` and `.w` zeroed.
- `float` data is written into `.x`, with `.y`, `.z`, `.w` zeroed.
- If `data.size() < verts.size()`, the remaining vertices keep their current
  value (typically the `Vec4` default of `(0,0,0,0)`).
- If `data.size() > verts.size()`, only the first `verts.size()` entries are
  applied.
- Any size mismatch is logged once to `stderr` as a warning. The function
  never throws.

**Example:**

```cpp
std::vector<crazy::Vertex> verts(24);
// ... fill verts[i].position / color / uv ...

// 24 face-aligned normals, one per vertex
std::vector<crazy::Vec3> normals(24, /* ... */);
crazy::setAttribute(verts, 0, normals);   // location 3 → 'n' in ESL

// 24 per-vertex weights in [0, 1]
std::vector<float> weights(24, /* ... */);
crazy::setAttribute(verts, 1, weights);   // location 4 → 'w' in ESL

crazy::Mesh m;
m.upload(verts, indices);
```

For an animated mesh, rebuild the `std::vector<Vertex>` and call
`mesh.upload(verts, indices)` again each frame. `Mesh::upload` is safe to
call repeatedly — it cleans up the old GPU resources first.

---

## 7. Limits & errors

### 7.1 The 3-attribute cap

ESL accepts **at most 3 `attribute` declarations** (locations 3, 4, 5). This
is bounded by `Vertex::extras[3]`. Declaring a fourth produces:

```
Too many attribute declarations (line 12): engine can only bind 3
extras slots (locations 3-5); reduce attribute count or extend
engine Vertex
```

To raise the cap, extend `Vertex::extras[N]` in `engine.h`, the
`glEnableVertexAttribArray` / `glVertexAttribPointer` block in `engine.cpp`'s
`Mesh::upload`, and the `kMaxAttributes` constant in `esl.cpp`. All three
have to match.

### 7.2 Type rejections

The following declarations are rejected with a clear error:

| Declaration                          | Error                                                                          |
|--------------------------------------|--------------------------------------------------------------------------------|
| 4th `attribute`                      | "Too many attribute declarations (line N): engine can only bind 3 …"           |
| `attribute` with `mat*` / `sampler*` / `image*` / `atomic*` | "attribute 'NAME' has unsupported type 'TYPE' (line N) — per-vertex attribute types must be scalar/vector…" |
| Body line before any `def …():`      | "Line N: indented body line but no active 'def vertex():' / 'def geometry():' / 'def fragment():' block" |
| `transform` in fragment stage        | (Passed through unchanged → GLSL compile error about undefined `projection`)   |
| Missing `def vertex():` or `def fragment():` | "Missing or empty 'def …():' block"                                   |

### 7.3 Compile / link errors

GLSL compile and link errors are printed to `stderr` with the `[CrazyEngine]`
prefix. If the transpile succeeds but the GPU driver rejects the GLSL, the
error message includes the offending source line. `Shader::loadFromESL`
returns `false` and `m_id` is `0`; check with `shader.id() == 0`.

### 7.4 Geometry stage limits

The default geometry config is `layout(triangles) in` and
`layout(triangle_strip, max_vertices = 6) out`. If you need a different
config (e.g. `points` in or a higher `max_vertices`), you can drop down to
raw GLSL via `Shader::loadFromSource` — ESL doesn't expose this knob.

---

## 8. What the transpiler actually produces

ESL is a thin front-end for GLSL `#version 330 core`. This section shows the
exact output for the demo shader, so you know there's no magic happening.

**Input** (`shaders/basic.esl`):

```python
tex       = uniform texture
passColor = varying color
passUV    = varying point2d
n         = attribute point

def vertex():
    passColor = color
    passUV    = uv
    output    = transform(position)

def fragment():
    output = sample(tex, passUV) * vec4(passColor, 1.0)
```

**Output (vertex):**

```glsl
#version 330 core
// Generated by CrazyEngine ESL transpiler

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_color;
layout(location = 2) in vec2 a_uv;
uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform sampler2D tex;
out vec3 passColor;
out vec2 passUV;
layout(location = 3) in vec3 a_n;

void main() {
    vec3 position = a_position;
    vec3 color    = a_color;
    vec2 uv       = a_uv;
    vec3 n        = a_n;
    passColor = color;
    passUV = uv;
    gl_Position = projection * view * model * vec4(position, 1.0);
}
```

**Output (fragment):**

```glsl
#version 330 core
// Generated by CrazyEngine ESL transpiler

precision mediump float;
out vec4 fragColor;
in vec3 passColor;
in vec2 passUV;
uniform sampler2D tex;

void main() {
    fragColor = texture(tex, passUV) * vec4(passColor, 1.0);
}
```

Note how the friendly syntax has been desugared:
- `output = transform(position)` → `gl_Position = projection * view * model * vec4(position, 1.0)`
- `output = sample(tex, passUV) * vec4(passColor, 1.0)` → `fragColor = texture(tex, passUV) * vec4(passColor, 1.0)`
- `n = attribute point` → `layout(location = 3) in vec3 a_n;` + `vec3 n = a_n;` alias

If you ever need to debug a shader, the simplest workflow is:

```cpp
crazy::esl::Result r = crazy::esl::transpile(readFile("shaders/foo.esl"));
std::cout << r.vertex << "\n---\n" << r.fragment << "\n";
```

Then you can paste the generated GLSL into any GLSL debugger.

---

## 9. Quick reference

### Cheat sheet

| You want to…                                       | Write…                                                |
|----------------------------------------------------|-------------------------------------------------------|
| Declare a 2D sampler uniform                       | `tex = uniform texture`                               |
| Declare a `vec3` varying                           | `passColor = varying color`                           |
| Declare a per-vertex input                         | `n = attribute point`                                 |
| Write the fragment output                          | `output = …` (in `def fragment():`)                  |
| Sample a 2D texture                                | `sample(tex, uv)`                                     |
| Apply MVP to a position                            | `transform(position)` (in `def vertex():`)           |
| Add a comment                                      | starts with `#`                                       |
| Insert a stage                                     | `def vertex():` / `def geometry():` / `def fragment():` |
| Skip a line                                        | leave it blank                                        |
| Terminate a body statement                         | _optional_ — the transpiler adds `;` for you          |

### Type alias lookup

| ESL type   | GLSL type   |
|------------|-------------|
| `point`    | `vec3`      |
| `color`    | `vec3`      |
| `point2d`  | `vec2`      |
| `uv_coord` | `vec2`      |
| `matrix`   | `mat4`      |
| `texture`  | `sampler2D` |
| `number`   | `float`     |
| `integer`  | `int`       |

### Built-in symbols

| Stage    | Available without declaring                              |
|----------|----------------------------------------------------------|
| vertex   | `position`, `color`, `uv`, `model`, `view`, `projection`, your declared `attribute` names |
| geometry | `gpos[3]`, `gi`, `EmitVertex()`, `EndPrimitive()`, `v_NAME[3]` for each varying, `NAME` for each varying's `out` |
| fragment | `fragColor` (or legacy `gl_FragColor`)                   |

### Limits at a glance

| Resource                     | Cap                                            |
|------------------------------|------------------------------------------------|
| `attribute` declarations     | 3 (locations 3, 4, 5)                          |
| Geometry stage out vertices  | 6 (`max_vertices = 6`)                         |
| Geometry stage in primitives | triangles                                      |
| Built-in vertex attributes   | 3 (`position`, `color`, `uv`)                  |
| Built-in uniforms            | 3 (`model`, `view`, `projection`)              |

### Friendly syntax substitutions

| ESL form                              | Becomes                                                                                | Where                       |
|---------------------------------------|----------------------------------------------------------------------------------------|-----------------------------|
| `output = X`                          | `gl_Position = X;` (vert/geom) or `fragColor = X;` (frag)                              | any stage body              |
| `sample(name, uv)`                    | `texture(name, uv)`                                                                    | any stage body              |
| `transform(p)`                        | `projection * view * model * vec4(p, 1.0)`                                             | vertex body only            |
| `gl_FragColor = X` (legacy)           | `fragColor = X;`                                                                       | fragment body               |
| Friendly type in declaration          | the GLSL type it aliases                                                               | declaration only            |
| `n = attribute point`                 | `layout(location = 3) in vec3 a_n;` + `vec3 n = a_n;` alias inside vertex `main()`     | declaration + vertex body   |

---

That's the whole language. It fits on a postcard and gets out of your way.
Happy shading.
