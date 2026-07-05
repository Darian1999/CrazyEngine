// src/physics.cpp — Bullet Physics bindings for CrazyEngine.
//
// This file is the ONLY place Bullet types appear in the project. Every type
// in the public header crazyengine/physics.h is crazy::Vec3 / Vec4 / Mat4
// or our own PIMPL/handle class.
//
// Lifecycle:
//   - World owns: Bullet's CollisionConfiguration, Dispatcher, Broadphase,
//     SequentialImpulseConstraintSolver, DiscreteDynamicsWorld, plus all
//     btCollisionShape* clones and btRigidBody* bodies.
//   - On destruction the world removes each body in reverse order (Bullet
//     then deletes its internal motion state), then deletes every shape.
//   - Bodies are added via World::addDynamicBody / addStaticBody /
//     addKinematicBody. Each method clones the user's Shape and converts
//     the clone to a btCollisionShape* via buildBulletShape(). The user's
//     original Shape can go out of scope immediately after.

#include "crazyengine/physics.h"

// Bullet. Pulled in via FetchContent in CMakeLists.txt (pin: 3.25).
#include <btBulletDynamicsCommon.h>
#include <BulletCollision/CollisionShapes/btBoxShape.h>
#include <BulletCollision/CollisionShapes/btSphereShape.h>
#include <BulletCollision/CollisionShapes/btCapsuleShape.h>
#include <BulletCollision/CollisionShapes/btStaticPlaneShape.h>
#include <BulletCollision/CollisionShapes/btBvhTriangleMeshShape.h>
#include <BulletCollision/CollisionShapes/btTriangleMesh.h>
// Explicit include for the collision flags (CF_STATIC_OBJECT, etc.).
#include <BulletCollision/CollisionDispatch/btCollisionObject.h>

#include <cmath>
#include <memory>
#include <vector>
#include <iostream>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace crazy {
namespace physics {

// ============================================================
// MeshDataAccessor — hidden friend defined at namespace scope
// ============================================================
//
// The header's `friend struct MeshDataAccessor;` (inside MeshShape) resolves
// to this struct. It must be visible to the cpp helpers in the anonymous
// namespace below, so it lives at crazy::physics scope, BEFORE that block.
// Reading the raw mesh storage here also lets us avoid making MeshShape's
// vectors part of the public surface.
struct MeshDataAccessor {
    static const std::vector<float>&    verts(const MeshShape& m) { return m.m_vertices; }
    static const std::vector<uint32_t>& idxs (const MeshShape& m) { return m.m_indices;   }
    static size_t                       ntri (const MeshShape& m) { return m.m_numTriangles; }
};

// ============================================================
// crazy:: -> bt:: conversions
// ============================================================

namespace {

btVector3 toBt(Vec3 v) { return btVector3(v.x, v.y, v.z); }
Vec3      fromBt(const btVector3& v) { return Vec3{v.x(), v.y(), v.z()}; }

float degToRad(float deg) { return deg * float(M_PI) / 180.0f; }

btQuaternion quatFromAxisAngle(Vec3 axis, float angleDeg) {
    const float a = degToRad(angleDeg);
    const float lenSq = axis.x*axis.x + axis.y*axis.y + axis.z*axis.z;
    if (lenSq < 1e-6f) return btQuaternion::getIdentity();
    const float len = std::sqrt(lenSq);
    axis.x /= len; axis.y /= len; axis.z /= len;
    const float half = a * 0.5f;
    const float s = std::sin(half);
    return btQuaternion(axis.x * s, axis.y * s, axis.z * s, std::cos(half));
}

struct AxisAngle { Vec3 axis; float angleDeg; };

AxisAngle axisAngleFromQuat(const btQuaternion& q) {
    float x = q.x(), y = q.y(), z = q.z(), w = q.w();
    // Force canonical form (w >= 0): (+q, -q) describe the same rotation;
    // this gives a unique axis-angle in [0, pi].
    if (w < 0.0f) { x = -x; y = -y; z = -z; w = -w; }
    const float wClamped = w > 1.0f ? 1.0f : (w < -1.0f ? -1.0f : w);
    const float angleRad = 2.0f * std::acos(wClamped);
    const float sinHalf = std::sin(angleRad * 0.5f);
    if (sinHalf < 1e-6f) {
        return AxisAngle{{0.0f, 1.0f, 0.0f}, 0.0f};
    }
    return AxisAngle{
        Vec3{ x / sinHalf, y / sinHalf, z / sinHalf },
        float(angleRad * 180.0 / M_PI)
    };
}

// ============================================================
// buildBulletShape dispatch
// ============================================================

btCollisionShape* buildBulletMeshShape(const MeshShape& m) {
    if (MeshDataAccessor::ntri(m) == 0) {
        return new btSphereShape(0.01f); // defensive — caller should reject empty
    }
    const std::vector<float>&    vertices = MeshDataAccessor::verts(m);
    const std::vector<uint32_t>& indices  = MeshDataAccessor::idxs(m);
    const size_t nTri = MeshDataAccessor::ntri(m);

    auto* tm = new btTriangleMesh();
    auto vertAt = [&](uint32_t i) {
        const size_t base = (size_t)i * 3;
        return btVector3(vertices[base + 0],
                         vertices[base + 1],
                         vertices[base + 2]);
    };
    for (size_t t = 0; t < nTri; ++t) {
        tm->addTriangle(
            vertAt(indices[t * 3 + 0]),
            vertAt(indices[t * 3 + 1]),
            vertAt(indices[t * 3 + 2]),
            /*removeDuplicateVertices=*/false
        );
    }
    return new btBvhTriangleMeshShape(tm, /*useQuantization=*/true);
}

btCollisionShape* buildBulletShape(const Shape& s) {
    if (const auto* p = dynamic_cast<const BoxShape*>(&s)) {
        return new btBoxShape(toBt(p->halfExtents()));
    }
    if (const auto* p = dynamic_cast<const SphereShape*>(&s)) {
        return new btSphereShape(p->radius());
    }
    if (const auto* p = dynamic_cast<const CapsuleShape*>(&s)) {
        // Bullet 3.25's btCapsuleShape has its long axis along Z.
        return new btCapsuleShape(p->radius(), p->height());
    }
    if (const auto* p = dynamic_cast<const PlaneShape*>(&s)) {
        Vec3 n = p->normal();
        const float lenSq = n.x*n.x + n.y*n.y + n.z*n.z;
        if (lenSq < 1e-6f) {
            std::cerr << "[CrazyEngine/physics] PlaneShape normal is zero; "
                      << "defaulting to (0,1,0).\n";
            n = {0.0f, 1.0f, 0.0f};
        } else {
            const float len = std::sqrt(lenSq);
            n = Vec3{n.x / len, n.y / len, n.z / len};
        }
        return new btStaticPlaneShape(toBt(n), p->constant());
    }
    if (const auto* p = dynamic_cast<const MeshShape*>(&s)) {
        return buildBulletMeshShape(*p);
    }
    std::cerr << "[CrazyEngine/physics] Unknown Shape subclass; "
              << "using degenerate empty sphere (no collisions).\n";
    return new btSphereShape(0.01f);
}

// Helper: build a btRigidBody* with the right flags from a BodyType.
btRigidBody* buildBody(btCollisionShape* shape, float mass,
                       const RigidTransform& t, BodyType type) {
    // A non-positive mass demotes to a static body — BUT a kinematic body
    // should keep its mass=0 identity even though Bullet stores zero mass
    // for it too. Branch by explicit type, not just mass.
    if (mass <= 0.0f && type == BodyType::Dynamic) {
        type = BodyType::Static;
        mass = 0.0f;
    }

    btTransform xform;
    xform.setIdentity();
    xform.setOrigin(toBt(t.position));
    xform.setRotation(quatFromAxisAngle(t.rotationAxis, t.rotationAngleDeg));

    btVector3 inertia(0, 0, 0);
    if (mass > 0.0f) {
        shape->calculateLocalInertia(mass, inertia);
    }

    auto* motionState = new btDefaultMotionState(xform);
    btRigidBody::btRigidBodyConstructionInfo ci(mass, motionState, shape, inertia);
    auto* body = new btRigidBody(ci);

    // btRigidBody's default collision flags for mass=0 already include
    // CF_STATIC_OBJECT. Just OR'ing our flag onto whatever the body started
    // with yields both flags set, and our type() then reports the wrong
    // kind. Clear the conflicting bits explicitly before setting ours.
    int flags = body->getCollisionFlags();
    flags &= ~(btCollisionObject::CF_STATIC_OBJECT |
               btCollisionObject::CF_KINEMATIC_OBJECT);
    switch (type) {
        case BodyType::Static:
            flags |= btCollisionObject::CF_STATIC_OBJECT;     break;
        case BodyType::Kinematic:
            flags |= btCollisionObject::CF_KINEMATIC_OBJECT;  break;
        case BodyType::Dynamic:
        default:
            break;
    }
    body->setCollisionFlags(flags);
    body->setWorldTransform(xform);
    return body;
}

} // anonymous namespace

// ============================================================
// RigidTransform::toMatrix
// ============================================================

Mat4 RigidTransform::toMatrix() const {
    return Mat4::translate(position) * Mat4::rotate(rotationAngleDeg, rotationAxis);
}

// ============================================================
// Shape subclasses (the bullet-side shape is built lazily by
// buildBulletShape() above).
// ============================================================

BoxShape::BoxShape(Vec3 halfExtents) : m_halfExtents(halfExtents) {}
std::unique_ptr<Shape> BoxShape::clone() const {
    return std::make_unique<BoxShape>(m_halfExtents);
}
Vec3 BoxShape::halfExtents() const { return m_halfExtents; }

SphereShape::SphereShape(float radius) : m_radius(radius) {}
std::unique_ptr<Shape> SphereShape::clone() const {
    return std::make_unique<SphereShape>(m_radius);
}
float SphereShape::radius() const { return m_radius; }

CapsuleShape::CapsuleShape(float radius, float height)
    : m_radius(radius), m_height(height) {}
std::unique_ptr<Shape> CapsuleShape::clone() const {
    return std::make_unique<CapsuleShape>(m_radius, m_height);
}
float CapsuleShape::radius() const { return m_radius; }
float CapsuleShape::height() const { return m_height; }

PlaneShape::PlaneShape(Vec3 normal, float constant)
    : m_normal(normal), m_constant(constant) {}
std::unique_ptr<Shape> PlaneShape::clone() const {
    return std::make_unique<PlaneShape>(m_normal, m_constant);
}
Vec3 PlaneShape::normal() const { return m_normal; }
float PlaneShape::constant() const { return m_constant; }

MeshShape::MeshShape() : m_numTriangles(0) {}

bool MeshShape::upload(const float* vertices, size_t numVertices,
                       const uint32_t* indices, size_t numIndices) {
    if (!vertices || !indices || numVertices == 0 || numIndices == 0) {
        std::cerr << "[CrazyEngine/physics] MeshShape::upload rejected: "
                  << "null buffers or zero counts.\n";
        return false;
    }
    if (numIndices % 3 != 0) {
        std::cerr << "[CrazyEngine/physics] MeshShape::upload rejected: "
                  << "numIndices (" << numIndices << ") must be a multiple of 3.\n";
        return false;
    }
    for (size_t i = 0; i < numIndices; ++i) {
        if (indices[i] >= numVertices) {
            std::cerr << "[CrazyEngine/physics] MeshShape::upload rejected: "
                      << "index " << indices[i] << " >= numVertices ("
                      << numVertices << ").\n";
            return false;
        }
    }
    m_vertices.assign(vertices, vertices + numVertices * 3);
    m_indices.assign(indices, indices + numIndices);
    m_numTriangles = numIndices / 3;
    return true;
}

std::unique_ptr<Shape> MeshShape::clone() const {
    auto copy = std::make_unique<MeshShape>();
    copy->m_vertices    = m_vertices;
    copy->m_indices     = m_indices;
    copy->m_numTriangles = m_numTriangles;
    return copy;
}
bool   MeshShape::empty() const       { return m_numTriangles == 0; }
size_t MeshShape::numTriangles() const { return m_numTriangles; }
size_t MeshShape::numVertices() const  { return m_vertices.size() / 3; }

// ============================================================
// RigidBody — opaque non-owning handle around a btRigidBody*
// ============================================================

RigidBody::~RigidBody() { m_handle = nullptr; } // body owned by its World

RigidBody::RigidBody(RigidBody&& other) noexcept : m_handle(other.m_handle) {
    other.m_handle = nullptr;
}

RigidBody& RigidBody::operator=(RigidBody&& other) noexcept {
    if (this != &other) {
        m_handle = other.m_handle;
        other.m_handle = nullptr;
    }
    return *this;
}

bool RigidBody::valid() const { return m_handle != nullptr; }

BodyType RigidBody::type() const {
    if (!m_handle) return BodyType::Static;
    const int flags = static_cast<btRigidBody*>(m_handle)->getCollisionFlags();
    if (flags & btCollisionObject::CF_STATIC_OBJECT)    return BodyType::Static;
    if (flags & btCollisionObject::CF_KINEMATIC_OBJECT) return BodyType::Kinematic;
    return BodyType::Dynamic;
}

Vec3 RigidBody::position() const {
    return m_handle
        ? fromBt(static_cast<btRigidBody*>(m_handle)->getCenterOfMassPosition())
        : Vec3{0, 0, 0};
}

void RigidBody::setPosition(Vec3 p) {
    if (!m_handle) return;
    auto* b = static_cast<btRigidBody*>(m_handle);
    btTransform t = b->getWorldTransform();
    t.setOrigin(toBt(p));
    b->setWorldTransform(t);
    // Kinematic bodies use velocity to communicate motion to the solver —
    // zeroing it would silently cancel an in-flight push. For dynamic/static
    // bodies, zero velocities + clearForces prevents phantom impulses.
    const int flags = b->getCollisionFlags();
    if (!(flags & btCollisionObject::CF_KINEMATIC_OBJECT)) {
        b->setLinearVelocity(btVector3(0, 0, 0));
        b->setAngularVelocity(btVector3(0, 0, 0));
        b->clearForces();
    }
}

RigidTransform RigidBody::transform() const {
    RigidTransform out;
    if (!m_handle) return out;
    auto* b = static_cast<btRigidBody*>(m_handle);
    const btTransform& t = b->getWorldTransform();
    out.position       = fromBt(t.getOrigin());
    auto aa = axisAngleFromQuat(t.getRotation());
    out.rotationAxis   = aa.axis;
    out.rotationAngleDeg = aa.angleDeg;
    return out;
}

void RigidBody::setTransform(const RigidTransform& t) {
    if (!m_handle) return;
    auto* b = static_cast<btRigidBody*>(m_handle);
    btTransform xform;
    xform.setIdentity();
    xform.setOrigin(toBt(t.position));
    xform.setRotation(quatFromAxisAngle(t.rotationAxis, t.rotationAngleDeg));
    b->setWorldTransform(xform);
    // Kinematic-aware: leave velocity untouched so a kinematic body's
    // motion is preserved (the solver reads velocity to propagate push).
    const int flags = b->getCollisionFlags();
    if (!(flags & btCollisionObject::CF_KINEMATIC_OBJECT)) {
        b->setLinearVelocity(btVector3(0, 0, 0));
        b->setAngularVelocity(btVector3(0, 0, 0));
        b->clearForces();
    }
}

Vec3 RigidBody::linearVelocity() const {
    return m_handle
        ? fromBt(static_cast<btRigidBody*>(m_handle)->getLinearVelocity())
        : Vec3{0, 0, 0};
}
void RigidBody::setLinearVelocity(Vec3 v) {
    if (!m_handle) return;
    static_cast<btRigidBody*>(m_handle)->setLinearVelocity(toBt(v));
}

void RigidBody::applyCentralForce(Vec3 force) {
    if (!m_handle) return;
    auto* b = static_cast<btRigidBody*>(m_handle);
    if (b->getMass() == 0.0f) return; // static — silent no-op
    b->applyCentralForce(toBt(force));
}

void RigidBody::applyCentralImpulse(Vec3 impulse) {
    if (!m_handle) return;
    auto* b = static_cast<btRigidBody*>(m_handle);
    if (b->getMass() == 0.0f) return; // static — silent no-op
    b->applyCentralImpulse(toBt(impulse));
}

// ============================================================
// World::Impl — Bullet's many separate objects + our ownership vectors
// ============================================================

struct World::Impl {
    btDefaultCollisionConfiguration*      cfg        = nullptr;
    btCollisionDispatcher*                dispatcher = nullptr;
    btDbvtBroadphase*                    broadphase = nullptr;
    btSequentialImpulseConstraintSolver* solver     = nullptr;
    btDiscreteDynamicsWorld*             world      = nullptr;

    std::vector<std::unique_ptr<btCollisionShape>> ownedShapes;
    std::vector<std::unique_ptr<btRigidBody>>      ownedBodies;

    ~Impl() {
        // Order matters: bodies first (removed from world), then shapes.
        if (world) {
            for (int i = world->getNumCollisionObjects() - 1; i >= 0; --i) {
                btCollisionObject* obj = world->getCollisionObjectArray()[i];
                btRigidBody* rb = btRigidBody::upcast(obj);
                if (rb) world->removeRigidBody(rb);
            }
        }
        ownedBodies.clear();   // unique_ptr deletes each body; body deletes motion state
        ownedShapes.clear();   // unique_ptr deletes each shape (Bvh deletes its mesh)
        delete world;
        delete solver;
        delete broadphase;
        delete dispatcher;
        delete cfg;
    }
};

// ============================================================
// World — public methods
// ============================================================

World::World(Vec3 gravity) : m_impl(new Impl) {
    Impl& i = *static_cast<Impl*>(m_impl);
    i.cfg        = new btDefaultCollisionConfiguration();
    i.dispatcher = new btCollisionDispatcher(i.cfg);
    i.broadphase = new btDbvtBroadphase();
    i.solver     = new btSequentialImpulseConstraintSolver();
    i.world      = new btDiscreteDynamicsWorld(i.dispatcher, i.broadphase,
                                               i.solver, i.cfg);
    i.world->setGravity(toBt(gravity));
}

World::~World() {
    delete static_cast<Impl*>(m_impl);
}

World::World(World&& other) noexcept : m_impl(other.m_impl) {
    other.m_impl = nullptr;
}

World& World::operator=(World&& other) noexcept {
    if (this != &other) {
        delete static_cast<Impl*>(m_impl);
        m_impl = other.m_impl;
        other.m_impl = nullptr;
    }
    return *this;
}

void World::setGravity(Vec3 g) {
    static_cast<Impl*>(m_impl)->world->setGravity(toBt(g));
}
Vec3 World::gravity() const {
    return fromBt(static_cast<Impl*>(m_impl)->world->getGravity());
}

RigidBody World::addDynamicBody(const Shape& shape, float mass,
                                Vec3 position, Vec3 rotationAxis, float rotationAngleDeg) {
    if (mass <= 0.0f) {
        std::cerr << "[CrazyEngine/physics] addDynamicBody: mass=" << mass
                  << " is non-positive; promoted to static body.\n"
                  << "  hint: use addStaticBody() (or set a positive mass) "
                  << "for an unmovable body.\n";
    }
    Impl& i = *static_cast<Impl*>(m_impl);
    auto shapeClone = shape.clone();
    btCollisionShape* btShape = buildBulletShape(*shapeClone);
    i.ownedShapes.emplace_back(btShape);

    RigidTransform t{position, rotationAxis, rotationAngleDeg};
    btRigidBody* body = buildBody(btShape, mass, t, BodyType::Dynamic);
    i.ownedBodies.emplace_back(body);
    i.world->addRigidBody(body);
    return RigidBody(static_cast<void*>(body));   // World owns the actual body
}

RigidBody World::addStaticBody(const Shape& shape,
                               Vec3 position, Vec3 rotationAxis, float rotationAngleDeg) {
    Impl& i = *static_cast<Impl*>(m_impl);
    auto shapeClone = shape.clone();
    btCollisionShape* btShape = buildBulletShape(*shapeClone);
    i.ownedShapes.emplace_back(btShape);

    RigidTransform t{position, rotationAxis, rotationAngleDeg};
    btRigidBody* body = buildBody(btShape, /*mass=*/0.0f, t, BodyType::Static);
    i.ownedBodies.emplace_back(body);
    i.world->addRigidBody(body);
    return RigidBody(static_cast<void*>(body));
}

RigidBody World::addKinematicBody(const Shape& shape,
                                  Vec3 position, Vec3 rotationAxis, float rotationAngleDeg) {
    Impl& i = *static_cast<Impl*>(m_impl);
    auto shapeClone = shape.clone();
    btCollisionShape* btShape = buildBulletShape(*shapeClone);
    i.ownedShapes.emplace_back(btShape);

    RigidTransform t{position, rotationAxis, rotationAngleDeg};
    btRigidBody* body = buildBody(btShape, /*mass=*/0.0f, t, BodyType::Kinematic);
    i.ownedBodies.emplace_back(body);
    i.world->addRigidBody(body);
    return RigidBody(static_cast<void*>(body));
}

void World::removeBody(RigidBody& body) {
    if (!body.valid()) return;
    Impl& i = *static_cast<Impl*>(m_impl);
    auto* btBody = static_cast<btRigidBody*>(body.m_handle);
    i.world->removeRigidBody(btBody);
    for (auto it = i.ownedBodies.begin(); it != i.ownedBodies.end(); ++it) {
        if (it->get() == btBody) {
            i.ownedBodies.erase(it);
            break;
        }
    }
    body.m_handle = nullptr;
}

int World::bodyCount() const {
    const Impl& i = *static_cast<const Impl*>(m_impl);
    return (int)i.ownedBodies.size();
}

void World::step(float dt) {
    if (dt <= 0.0f) return;
    static_cast<Impl*>(m_impl)->world->stepSimulation(dt);
}

void World::stepFixed(float realDt, float fixedDt, int maxSubsteps) {
    if (realDt <= 0.0f) return;
    if (fixedDt <= 0.0f) fixedDt = 1.0f / 60.0f;
    if (maxSubsteps < 1) maxSubsteps = 1;
    static_cast<Impl*>(m_impl)->world->stepSimulation(realDt, maxSubsteps, fixedDt);
}

} // namespace physics
} // namespace crazy
