#pragma once

// crazyengine/physics.h — Bullet Physics bindings for CrazyEngine
//
// Beginner-friendly wrapper around bullet3 (rigid-body dynamics only).
// No Bullet types leak into this header: everything user-facing uses
// crazy::Vec3 / Vec4 / Mat4 so the existing Mesh + Shader code can drive
// bodies without conversion boilerplate.
//
// Patterns match the rest of the engine: move-only RAII classes, no copy,
// no dependency surprises (Bullet is pulled in via CMake FetchContent).
//
// Build:
//   #include <crazyengine/physics.h>
//   crazy::physics::World world({0.0f, -9.81f, 0.0f});
//   crazy::physics::BoxShape box({0.5f, 0.5f, 0.5f});
//   auto body = world.addDynamicBody(box, /*mass=*/1.0f, {0.0f, 5.0f, 0.0f});
//   while (running) {
//       world.step(deltaTime);
//       Mat4 model = crazy::Mat4::translate(body.position());
//       shader.setMat4("model", model);
//   }

#include "crazyengine/engine.h"   // for Vec3, Vec4, Mat4

#include <memory>                // std::unique_ptr (Shape::clone)
#include <vector>                // MeshShape storage
#include <cstddef>               // size_t

namespace crazy {
namespace physics {

// ============================================================
// Transform — position + axis-angle rotation (matches Mat4::rotate signature)
// ============================================================
//
// Bullet internally stores rotations as quaternions, but our public API keeps
// things in axis-angle form so users can compose Mat4::rotate(angle, axis)
// directly. This struct also has a toMatrix() helper.
struct RigidTransform {
    Vec3 position       = {0.0f, 0.0f, 0.0f};
    Vec3 rotationAxis   = {0.0f, 1.0f, 0.0f}; // axis of the rotation
    float rotationAngleDeg = 0.0f;           // degrees, right-handed

    // Compose a model matrix: T(position) * R(angle, axis).
    Mat4 toMatrix() const;
};

// ============================================================
// BodyType — what role a rigid body plays in the simulation
// ============================================================
//
// - Static:    never moves, ignored by the solver. Use for terrain / walls.
// - Kinematic: moves under explicit user control but pushes dynamic bodies.
// - Dynamic:   physics-driven subject to gravity, forces, and collisions.
enum class BodyType {
    Static    = 0,
    Kinematic = 1,
    Dynamic   = 2,
};

// ============================================================
// Shape — abstract collision-shape base (Box / Sphere / Capsule / Plane / Mesh)
// ============================================================
//
// Shapes are *copied* into the World when a body is added. You don't have to
// keep a Shape alive after addDynamicBody() / addStaticBody(). They are
// reference types so you can pass them by const-ref, store them in
// std::unique_ptr<Shape>, or build one inline as a temporary.
class Shape {
public:
    virtual ~Shape() = default;
    Shape() = default;
    Shape(Shape&&) noexcept = default;
    Shape& operator=(Shape&&) noexcept = default;
    Shape(const Shape&) = delete;
    Shape& operator=(const Shape&) = delete;

    // Polymorphic deep copy — World uses this to own its own copy of every
    // shape so users don't have to manage lifetimes separately.
    virtual std::unique_ptr<Shape> clone() const = 0;
};

// Half-extents box (a box of size 2*halfExtents).
class BoxShape : public Shape {
public:
    explicit BoxShape(Vec3 halfExtents);
    std::unique_ptr<Shape> clone() const override;
    Vec3 halfExtents() const;
private:
    Vec3 m_halfExtents;
};

// Sphere centered at the body's origin.
class SphereShape : public Shape {
public:
    explicit SphereShape(float radius);
    std::unique_ptr<Shape> clone() const override;
    float radius() const;
private:
    float m_radius;
};

// Capsule of `height` total length along Z and `radius` circular caps.
// (Bullet 3.25's btCapsuleShape has its long axis along Z; if you want Y-up,
// rotate the body 90 deg about X when adding it.)
class CapsuleShape : public Shape {
public:
    CapsuleShape(float radius, float height);
    std::unique_ptr<Shape> clone() const override;
    float radius() const;
    float height() const;
private:
    float m_radius;
    float m_height;
};

// Infinite static plane defined by `normal . x = constant`.
// Typical ground plane: PlaneShape({0,1,0}, 0.0f).
class PlaneShape : public Shape {
public:
    PlaneShape(Vec3 normal, float constant);
    std::unique_ptr<Shape> clone() const override;
    Vec3 normal() const;
    float constant() const;
private:
    Vec3 m_normal;
    float m_constant;
};

// Triangle mesh for static collision only. Vertices are 3-floats (XYZ);
// indices are 3-uint32s per triangle. Pass MeshShape::upload(...) once after
// construction and before adding a body.
//
// Memory note: every World::addStaticBody(meshShape, ...) clones the shape
// and rebuilds the BVH on the clone side, so N bodies built from one
// MeshShape hold N copies in memory. For a single large terrain that's fine;
// for thousands of small repeats, keep that in mind.
class MeshShape : public Shape {
public:
    MeshShape();
    std::unique_ptr<Shape> clone() const override;

    // vertices is XYZ flattened (length = 3 * numVertices).
    // indices is triangle-index flattened (length = 3 * numTriangles).
    // Index values must be < numVertices, and numIndices must be a multiple
    // of 3. Returns true on accept, false (with a warning) on reject.
    bool upload(const float* vertices, size_t numVertices,
                const uint32_t* indices, size_t numIndices);

    bool empty() const;            // true before a successful upload()
    size_t numTriangles() const;
    size_t numVertices() const;

private:
    // Hidden friend accessor — defined at crazy::physics::MeshDataAccessor in
    // physics.cpp. Reads m_vertices/m_indices/m_numTriangles without exposing
    // those types on the public surface.
    friend struct MeshDataAccessor;

    std::vector<float>    m_vertices;  // 3 floats per vertex (xyz xyz ...)
    std::vector<uint32_t> m_indices;   // 3 indices per triangle (tri tri ...)
    size_t                m_numTriangles = 0;
};

// ============================================================
// RigidBody — opaque, lightweight non-owning handle to a body
// ============================================================
//
// After world.addDynamicBody(...) returns, store the handle as a local and
// reuse it. The handle becomes invalid if the matching World is destroyed,
// or if you call world.removeBody(body).
//
// valid() is always safe to call; calling anything else on an invalid
// handle is a no-op.
class RigidBody {
public:
    RigidBody() = default;
    ~RigidBody();

    // Move-only: movable, non-copyable.
    RigidBody(const RigidBody&) = delete;
    RigidBody& operator=(const RigidBody&) = delete;
    RigidBody(RigidBody&& other) noexcept;
    RigidBody& operator=(RigidBody&& other) noexcept;

    bool valid() const;

    BodyType type() const;

    // World-space position (read after step() / write to teleport).
    Vec3 position() const;
    void setPosition(Vec3 p);

    // Full world-space transform (axis-angle again, not quaternion).
    RigidTransform transform() const;
    void setTransform(const RigidTransform& t);

    // Linear velocity (m/s). Only meaningful for Dynamic bodies.
    Vec3 linearVelocity() const;
    void setLinearVelocity(Vec3 v);

    // Apply a force or impulse (Dynamic bodies only — silently ignored on
    // Static/Kinematic). An impulse is instant; a force accumulates over the
    // next step.
    void applyCentralForce(Vec3 force);
    void applyCentralImpulse(Vec3 impulse);

    // Build a handle from a raw Bullet pointer. Marked public so the cpp's
    // helpers can construct handles, and explicit to discourage direct
    // construction in user code (always get a handle from World::*Body()).
    explicit RigidBody(void* rawHandle) noexcept : m_handle(rawHandle) {}

private:
    // World removes bodies through this handle and needs to read it directly.
    friend class World;
    void* m_handle = nullptr; // btRigidBody*, opaque
};

// ============================================================
// World — owns the dynamics world, all bodies, and all shapes
// ============================================================
class World {
public:
    explicit World(Vec3 gravity = {0.0f, -9.81f, 0.0f});
    ~World();

    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&& other) noexcept;
    World& operator=(World&& other) noexcept;

    // ----- accessories -----
    void setGravity(Vec3 g);
    Vec3 gravity() const;

    // ----- body creation -----
    // Each method clones shape internally; the original can be destroyed
    // immediately. For dynamic bodies, mass must be > 0; static/kinematic
    // use mass = 0.
    RigidBody addDynamicBody(const Shape& shape, float mass = 1.0f,
                             Vec3 position = {0.0f, 0.0f, 0.0f},
                             Vec3 rotationAxis = {0.0f, 1.0f, 0.0f},
                             float rotationAngleDeg = 0.0f);

    RigidBody addStaticBody(const Shape& shape,
                            Vec3 position = {0.0f, 0.0f, 0.0f},
                            Vec3 rotationAxis = {0.0f, 1.0f, 0.0f},
                            float rotationAngleDeg = 0.0f);

    RigidBody addKinematicBody(const Shape& shape,
                               Vec3 position = {0.0f, 0.0f, 0.0f},
                               Vec3 rotationAxis = {0.0f, 1.0f, 0.0f},
                               float rotationAngleDeg = 0.0f);

    // Remove and destroy a body. body.valid() becomes false after this; safe
    // to call with an already-invalid handle (no-op).
    void removeBody(RigidBody& body);

    // Number of rigid bodies in this world (counts owned bodies only;
    // future ghost/soft bodies, if added, will not inflate this number).
    int bodyCount() const;

    // ----- simulation -----
    // Variable-timestep step. For real-time game loops prefer stepFixed().
    void step(float dt);

    // Substepping step: simulates at fixedDt per internal step, with up to
    // maxSubsteps steps per call to keep up with realDt. Recommended mode
    // for real-time gameplay.
    void stepFixed(float realDt,
                   float fixedDt = 1.0f / 60.0f,
                   int maxSubsteps = 10);

private:
    // Opaque PIMPL-ish storage; full type defined in physics.cpp.
    struct Impl;
    void* m_impl = nullptr;
};

} // namespace physics
} // namespace crazy
