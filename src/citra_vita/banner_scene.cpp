// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <tuple>

#include <fmt/format.h>

#include "citra_vita/banner_scene.h"

namespace VitaFrontend::Banner {

namespace {

using Mat4 = SceneAnimator::Mat4;
using Common::Vec2f;
using Common::Vec3f;
using Common::Vec4f;

constexpr float Pi = 3.14159265358979f;
/// The HOME Menu turns a banner that is not animated once in this many frames.
constexpr u32 HomeTurnFrames = 600;
constexpr float Aspect = 400.0f / 240.0f;

// GL data types.
constexpr u32 GlByte = 5120;
constexpr u32 GlUnsignedByte = 5121;
constexpr u32 GlShort = 5122;
constexpr u32 GlUnsignedShort = 5123;
constexpr u32 GlFloat = 5126;

Mat4 Identity() {
    Mat4 m{};
    for (u32 i = 0; i < 4; i++) {
        m[i][i] = 1.0f;
    }
    return m;
}

Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 m{};
    for (u32 r = 0; r < 4; r++) {
        for (u32 c = 0; c < 4; c++) {
            m[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c] + a[r][3] * b[3][c];
        }
    }
    return m;
}

Vec4f Transform(const Mat4& m, const Vec4f& v) {
    return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z + m[0][3] * v.w,
            m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z + m[1][3] * v.w,
            m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z + m[2][3] * v.w,
            m[3][0] * v.x + m[3][1] * v.y + m[3][2] * v.z + m[3][3] * v.w};
}

Vec3f Point(const Mat4& m, const Vec3f& p) {
    return Transform(m, Common::MakeVec(p, 1.0f)).xyz();
}

Vec3f Direction(const Mat4& m, const Vec3f& d) {
    return Transform(m, Common::MakeVec(d, 0.0f)).xyz();
}

Vec3f Normalized(const Vec3f& v) {
    const float l = v.Length();
    return l > 1e-12f ? v / l : v;
}

Mat4 RotX(float a) {
    Mat4 m = Identity();
    const float c = std::cos(a), s = std::sin(a);
    m[1][1] = c, m[1][2] = -s, m[2][1] = s, m[2][2] = c;
    return m;
}

Mat4 RotY(float a) {
    Mat4 m = Identity();
    const float c = std::cos(a), s = std::sin(a);
    m[0][0] = c, m[0][2] = s, m[2][0] = -s, m[2][2] = c;
    return m;
}

Mat4 RotZ(float a) {
    Mat4 m = Identity();
    const float c = std::cos(a), s = std::sin(a);
    m[0][0] = c, m[0][1] = -s, m[1][0] = s, m[1][1] = c;
    return m;
}

/// Scale, then rotation about X, Y and Z, then translation.
Mat4 Trs(const Vec3f& s, const Vec3f& r, const Vec3f& t) {
    Mat4 m = RotZ(r.z) * RotY(r.y) * RotX(r.x);
    for (u32 row = 0; row < 3; row++) {
        m[row][0] *= s.x, m[row][1] *= s.y, m[row][2] *= s.z;
    }
    m[0][3] = t.x, m[1][3] = t.y, m[2][3] = t.z;
    return m;
}

/// A 3x4 matrix stored by rows.
Mat4 Rows34(const std::array<float, 12>& v) {
    Mat4 m = Identity();
    for (u32 r = 0; r < 3; r++) {
        for (u32 c = 0; c < 4; c++) {
            m[r][c] = v[r * 4 + c];
        }
    }
    return m;
}

void SetQuaternion(Mat4& m, const Vec4f& q, const Vec3f& s) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float r[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                           {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                           {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    for (u32 row = 0; row < 3; row++) {
        m[row][0] = r[row][0] * s.x, m[row][1] = r[row][1] * s.y, m[row][2] = r[row][2] * s.z;
    }
}

Mat4 LookAt(const Vec3f& eye, const Vec3f& target, const Vec3f& up) {
    const Vec3f f = Normalized(target - eye);
    const Vec3f s = Normalized(Cross(f, up));
    const Vec3f u = Cross(s, f);
    Mat4 m = Identity();
    const Vec3f rows[3] = {s, u, -f};
    for (u32 r = 0; r < 3; r++) {
        m[r][0] = rows[r].x, m[r][1] = rows[r].y, m[r][2] = rows[r].z;
        m[r][3] = -Dot(rows[r], eye);
    }
    return m;
}

Mat4 Perspective(float fovy, float aspect, float near, float far) {
    const float f = 1.0f / std::tan(fovy / 2);
    Mat4 m{};
    m[0][0] = f / aspect;
    m[1][1] = f;
    m[2][2] = (far + near) / (near - far);
    m[2][3] = 2 * far * near / (near - far);
    m[3][2] = -1.0f;
    return m;
}

/// A rotation and translation's inverse.
Mat4 RigidInverse(const Mat4& m) {
    Mat4 out = Identity();
    for (u32 r = 0; r < 3; r++) {
        for (u32 c = 0; c < 3; c++) {
            out[r][c] = m[c][r];
        }
    }
    for (u32 r = 0; r < 3; r++) {
        out[r][3] = -(out[r][0] * m[0][3] + out[r][1] * m[1][3] + out[r][2] * m[2][3]);
    }
    return out;
}

/// A texture coordinate's matrix from its scale, rotation and translation, as NW4C's DCC
/// conventions build it: 0 Maya, 1 SoftImage, 2 3ds Max.
Mat4 TextureMatrix(const Vec2f& s, float r, const Vec2f& t, u32 kind) {
    const float c = std::cos(r), n = std::sin(r);
    Mat4 m = Identity();
    m[0][0] = s.x * c, m[1][0] = s.y * n, m[0][1] = -s.x * n, m[1][1] = s.y * c;
    switch (kind) {
    case 0:
        m[0][3] = s.x * ((0.5f * n - 0.5f * c) + 0.5f - t.x);
        m[1][3] = s.y * ((-0.5f * n - 0.5f * c) + 0.5f - t.y);
        break;
    case 1:
        m[0][3] = s.x * (-c * t.x - n * t.y);
        m[1][3] = s.y * (n * t.x - c * t.y);
        break;
    default:
        m[0][3] = s.x * c * (-t.x - 0.5f) - s.x * n * (t.y - 0.5f) + 0.5f;
        m[1][3] = s.y * n * (-t.x - 0.5f) + s.y * c * (t.y - 0.5f) + 0.5f;
        break;
    }
    return m;
}

/// A billboard bone's world matrix: its place and scale kept, turned to the view. World and
/// Y axial keep the world's Y as the view sees it, Screen the bone's own rotation; the viewpoint
/// modes face the camera's position instead of its direction; Y axial turns about that Y alone.
Mat4 Billboard(const Mat4& m, u32 mode, const Vec3f& rotation, const Mat4& view) {
    Vec3f scale;
    for (u32 c = 0; c < 3; c++) {
        scale[c] = Vec3f{m[0][c], m[1][c], m[2][c]}.Length();
    }
    const Vec3f p_view = Point(view, {m[0][3], m[1][3], m[2][3]});
    Vec3f y;
    if (mode == 3 || mode == 4) {
        const Mat4 r = RotZ(rotation.z) * RotY(rotation.y) * RotX(rotation.x);
        y = {r[0][1], r[1][1], r[2][1]};
    } else {
        y = Direction(view, {0.0f, 1.0f, 0.0f});
    }
    y = Normalized(y);
    Vec3f z = (mode == 2 || mode == 4 || mode == 6) ? -Normalized(p_view) : Vec3f{0, 0, 1};
    const Vec3f x = Normalized(Cross(y, z));
    if (mode == 5 || mode == 6) {
        z = Cross(x, y);
    } else {
        y = Cross(z, x);
    }
    // The view's rotation undone: the axes in world space.
    Mat4 out = m;
    const Vec3f axes[3] = {x, y, z};
    for (u32 c = 0; c < 3; c++) {
        for (u32 r = 0; r < 3; r++) {
            out[r][c] =
                (view[0][r] * axes[c].x + view[1][r] * axes[c].y + view[2][r] * axes[c].z) *
                scale[c];
        }
    }
    return out;
}

/// The normal's quaternion: the rotation that turns +Z to it.
std::array<float, 4> NormalQuaternion(const Vec3f& n) {
    if (n.z < -0.999999f) {
        return {1.0f, 0.0f, 0.0f, 0.0f};
    }
    const Vec4f q{-n.y, n.x, 0.0f, 1.0f + n.z};
    const float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return {q.x / l, q.y / l, q.z / l, q.w / l};
}

/// `frame` in an animation of `frames` frames, looped (an animation of one frame or none: 0).
float Looped(u32 frame, float frames) {
    return frames > 1.0f ? static_cast<float>(frame % static_cast<u32>(frames)) : 0.0f;
}

Vec3f Clamp01(const Vec3f& v) {
    return {std::clamp(v.x, 0.0f, 1.0f), std::clamp(v.y, 0.0f, 1.0f),
            std::clamp(v.z, 0.0f, 1.0f)};
}

} // Anonymous namespace

SceneAnimator::SceneAnimator(const Scene& scene_) : scene{scene_} {
    bool finite = true;
    for (const Model& model : scene.models) {
    ModelData md;
    md.model = &model;
    for (const Shape& s : model.shapes) {
        ShapeData d;
        const auto read = [&s, &finite](u32 v, const Attribute& a) {
            Vec4f out{0.0f, 0.0f, 0.0f, 1.0f};
            const u8* p = s.vertices.data() + v * s.stride + a.offset;
            for (u32 k = 0; k < a.elements; k++) {
                float x = 0.0f;
                switch (a.type) {
                case GlByte:
                    x = static_cast<s8>(p[k]);
                    break;
                case GlUnsignedByte:
                    x = p[k];
                    break;
                case GlShort: {
                    s16 h;
                    std::memcpy(&h, p + 2 * k, 2);
                    x = h;
                    break;
                }
                case GlUnsignedShort: {
                    u16 h;
                    std::memcpy(&h, p + 2 * k, 2);
                    x = h;
                    break;
                }
                case GlFloat:
                    std::memcpy(&x, p + 4 * k, 4);
                    break;
                }
                out[k] = x * a.scale;
                finite = finite && std::isfinite(out[k]);
            }
            return out;
        };
        const auto find = [&s](u32 name) -> const Attribute* {
            for (const Attribute& a : s.attributes) {
                if (a.name == name) {
                    return &a;
                }
            }
            return nullptr;
        };
        const auto fixed = [&s](u32 name) -> std::optional<Vec4f> {
            for (const auto& [n, v] : s.fixed) {
                if (n == name) {
                    return v;
                }
            }
            return std::nullopt;
        };
        // A fixed attribute's value for every vertex, where the shape has one.
        const auto column = [&](u32 name, Vec4f fallback, u32* elements) {
            std::vector<Vec4f> out(s.count, fallback);
            if (const auto f = fixed(name)) {
                std::fill(out.begin(), out.end(), *f);
                return out;
            }
            if (const Attribute* a = find(name)) {
                if (elements != nullptr) {
                    *elements = a->elements;
                }
                for (u32 v = 0; v < s.count; v++) {
                    out[v] = read(v, *a);
                    if (name == 3 && a->elements == 3) {
                        out[v].w = 1.0f; // RGB colours are opaque
                    }
                }
            }
            return out;
        };
        for (const Vec4f& p : column(0, {}, nullptr)) {
            d.positions.push_back(p.xyz() + s.position_offset);
        }
        if (find(1) != nullptr || fixed(1)) {
            for (const Vec4f& n : column(1, {}, nullptr)) {
                d.normals.push_back(n.xyz());
            }
        }
        d.colors = column(3, {1.0f, 1.0f, 1.0f, 1.0f}, nullptr);
        for (u32 k = 0; k < 3; k++) {
            for (const Vec4f& uv : column(4 + k, {}, nullptr)) {
                d.uvs[k].push_back(uv.xy());
            }
        }
        if (find(7) != nullptr || fixed(7)) {
            for (const Vec4f& b : column(7, {}, nullptr)) {
                d.bone_indices.push_back({static_cast<u32>(std::lround(b.x)),
                                          static_cast<u32>(std::lround(b.y)),
                                          static_cast<u32>(std::lround(b.z)),
                                          static_cast<u32>(std::lround(b.w))});
            }
        }
        d.weights = 1;
        d.bone_weights = column(8, {1.0f, 0.0f, 0.0f, 0.0f}, &d.weights);
        // Each skinned vertex's bones in its submesh's matrix palette (one matrix without bones).
        for (const SubMesh& sub : s.submeshes) {
            if (sub.skinning == 0 || d.bone_indices.empty()) {
                continue;
            }
            const u32 used = sub.skinning == 2 ? d.weights : 1;
            const std::size_t palette = std::max<std::size_t>(sub.bones.size(), 1);
            for (const u32 v : sub.vertices) {
                for (u32 k = 0; k < used; k++) {
                    if (d.bone_indices[v][k] >= palette && error.empty()) {
                        error = fmt::format("vertex {} on palette entry {} of {}", v,
                                            d.bone_indices[v][k], palette);
                    }
                }
            }
        }
        md.shapes.push_back(std::move(d));
    }
    // The scene's animations reach every model's bones and materials by name.
    md.bone_animations.assign(model.bones.size(), nullptr);
    if (scene.skeletal) {
        for (std::size_t i = 0; i < model.bones.size(); i++) {
            if (const auto it = scene.skeletal->bones.find(model.bones[i].name);
                it != scene.skeletal->bones.end()) {
                md.bone_animations[i] = &it->second;
            }
        }
    }
    md.material_animations.resize(model.materials.size());
    if (scene.material) {
        for (const MaterialAnimation& a : scene.material->materials) {
            for (std::size_t i = 0; i < model.materials.size(); i++) {
                if (model.materials[i].name == a.material) {
                    md.material_animations[i].push_back(&a);
                }
            }
        }
    }
    // The PICA's eight lights for each mesh that has normals (without, it takes none).
    for (const Mesh& mesh : model.meshes) {
        const Material& mat = model.materials[mesh.material];
        if (md.shapes[mesh.shape].normals.empty()) {
            continue;
        }
        const std::vector<u32> set = LightSet(mat);
        const auto lights = std::count_if(set.begin(), set.end(), [this](u32 k) {
            return scene.lights[k].kind == Light::Kind::Fragment;
        });
        if (lights > 8 && error.empty()) {
            error = fmt::format("material {} lit by {} lights", mat.name, lights);
        }
    }
    models.push_back(std::move(md));
    }
    if (!finite && error.empty()) {
        error = "a vertex attribute is not finite";
    }

    if (scene.camera) {
        const Camera& c = *scene.camera;
        switch (c.view) {
        case Camera::View::Aim:
            view = LookAt(c.eye, c.target, {0.0f, 1.0f, 0.0f});
            break;
        case Camera::View::LookAt:
            view = LookAt(c.eye, c.target, c.up);
            break;
        case Camera::View::Rotation:
            view = RigidInverse(Trs({1.0f, 1.0f, 1.0f}, c.target, c.eye));
            break;
        }
        projection = Perspective(c.fovy, Aspect, c.near, c.far);
    } else {
        // No camera in the file: the banner template's, as several banners carry it: 24 units
        // tall at the target with a 30-degree vertical field, from +Z at (0, 1, 0).
        const float d = 12.0f / std::tan(15.0f * Pi / 180.0f);
        view = LookAt({0.0f, 1.0f, d}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
        projection = Perspective(30.0f * Pi / 180.0f, Aspect, 1.0f, 1000.0f);
    }
}

std::vector<u32> SceneAnimator::LightSet(const Material& mat) const {
    if (!scene.light_sets) {
        std::vector<u32> all(scene.lights.size());
        for (u32 k = 0; k < all.size(); k++) {
            all[k] = k;
        }
        return all;
    }
    const auto it = scene.light_sets->find(mat.light_set);
    return it != scene.light_sets->end() ? it->second : std::vector<u32>{};
}

u32 SceneAnimator::Loop() const {
    return scene.skeletal && scene.skeletal->frames > 1 ? static_cast<u32>(scene.skeletal->frames)
                                                        : 0;
}

SceneAnimator::Mat4 SceneAnimator::Turn(u32 frame) const {
    // The HOME Menu's: one turn a loop of the banner's own animation. Parts meant to stand still
    // turn once a loop the other way.
    const u32 period = Loop() != 0 ? Loop() : HomeTurnFrames;
    return RotY(-2.0f * Pi * static_cast<float>(frame % period) / static_cast<float>(period));
}

std::vector<Mat4> SceneAnimator::BoneWorld(const ModelData& md, u32 frame) const {
    const Mat4 turn = Turn(frame);
    const std::vector<Bone>& bones = md.model->bones;
    std::vector<Mat4> world;
    world.reserve(bones.size());
    for (std::size_t i = 0; i < bones.size(); i++) {
        const Bone& b = bones[i];
        Vec3f s = b.scale, r = b.rotation, t = b.translation;
        std::optional<Vec4f> quaternion;
        if (const BoneAnimation* a = md.bone_animations[i]) {
            const float f = Looped(frame, a->frames);
            const auto baked = [f](const BakedTrack& track) {
                const float at = std::clamp(f - track.start, 0.0f,
                                            static_cast<float>(track.frames.size() - 1));
                return track.frames[static_cast<std::size_t>(at)];
            };
            if (a->scale) {
                s = baked(*a->scale).xyz();
            }
            if (a->translation) {
                t = baked(*a->translation).xyz();
            }
            if (a->rotation) {
                quaternion = baked(*a->rotation);
            }
            for (u32 k = 0; k < 9; k++) {
                if (a->curves[k]) {
                    const float v = a->curves[k]->Evaluate(f);
                    (k < 3 ? s : k < 6 ? r : t)[k % 3] = v;
                }
            }
        }
        Mat4 local = Trs(s, r, t);
        if (quaternion) {
            SetQuaternion(local, *quaternion, s);
        }
        Mat4 m = b.parent < 0 ? turn * local : world[b.parent] * local;
        if (b.billboard != 0) {
            m = Billboard(m, b.billboard, r, view);
        }
        world.push_back(m);
    }
    return world;
}

void SceneAnimator::Evaluate(u32 frame, Frame& out) const {
    out.vertices.clear();
    out.indices.clear();
    out.draws.clear();
    std::vector<std::vector<Mat4>> worlds;
    for (const ModelData& md : models) {
        worlds.push_back(BoneWorld(md, frame));
    }
    const Mat4 turn = Turn(frame);
    // The top screen's quarter turn: the PICA's x is the picture's up, its y the picture's left.
    const Mat4 clip = projection * view;

    // Every model's meshes in their layers' order, translucent ones back to front.
    std::vector<std::tuple<u32, float, u32, u32, u32>> order;
    for (u32 k = 0; k < models.size(); k++) {
        const Model& model = *models[k].model;
        const std::vector<Mat4>& world = worlds[k];
        for (u32 i = 0; i < model.meshes.size(); i++) {
            const Mesh& mesh = model.meshes[i];
            const Material& mat = model.materials[mesh.material];
            float depth = 0.0f;
            if (mat.layer != 0) {
                const Shape& shape = model.shapes[mesh.shape];
                const ShapeData& d = models[k].shapes[mesh.shape];
                Vec3f centre{};
                for (const Vec3f& p : d.positions) {
                    centre += p;
                }
                centre /= static_cast<float>(std::max<std::size_t>(d.positions.size(), 1));
                const Mat4 m = !shape.submeshes.empty() && !shape.submeshes[0].bones.empty() &&
                                       !world.empty()
                                   ? world[shape.submeshes[0].bones[0]]
                                   : turn;
                depth = -Transform(clip, Common::MakeVec(Point(m, centre), 1.0f)).w;
            }
            order.emplace_back(mat.layer, depth, mesh.priority, k, i);
        }
    }
    std::sort(order.begin(), order.end());

    for (const auto& [layer, depth, priority, k, index] : order) {
        const ModelData& md = models[k];
        const Model& model = *md.model;
        const std::vector<Mat4>& world = worlds[k];
        const Mesh& mesh = model.meshes[index];
        const Shape& shape = model.shapes[mesh.shape];
        const ShapeData& d = md.shapes[mesh.shape];
        const Material& mat = model.materials[mesh.material];

        // The material at this frame: its colours and texture coordinates as animated.
        auto colors = mat.colors;
        std::array<TextureCoord, 3> coords = mat.coords;
        std::array<bool, 3> coord_animated{};
        for (const MaterialAnimation* a : md.material_animations[mesh.material]) {
            std::array<std::optional<float>, 4> v;
            for (std::size_t k = 0; k < a->curves.size(); k++) {
                if (a->curves[k]) {
                    v[k] = a->curves[k]->Evaluate(Looped(frame, a->frames));
                }
            }
            using Target = MaterialAnimation::Target;
            if (a->target == Target::Color) {
                for (u32 k = 0; k < 4; k++) {
                    if (v[k]) {
                        colors[a->index][k] = *v[k];
                    }
                }
                continue;
            }
            TextureCoord& tc = coords[a->index];
            coord_animated[a->index] = true;
            if (a->target == Target::CoordRotate) {
                if (v[0]) {
                    tc.rotation = *v[0];
                }
                continue;
            }
            Vec2f& field = a->target == Target::CoordScale ? tc.scale : tc.translation;
            for (u32 k = 0; k < 2; k++) {
                if (v[k]) {
                    field[k] = *v[k];
                }
            }
        }
        std::array<Mat4, 3> coord_matrix;
        for (u32 k = 0; k < 3; k++) {
            const TextureCoord& tc = coords[k];
            coord_matrix[k] = coord_animated[k]
                                  ? TextureMatrix(tc.scale, tc.rotation, tc.translation,
                                                  tc.transform_type)
                                  : Rows34(tc.transform);
        }

        const std::vector<u32> set = LightSet(mat);
        const Light* hemisphere = nullptr;
        Draw draw;
        draw.material = &mat;
        draw.colors = colors;
        const auto color = [&colors](MaterialColor c) {
            return colors[static_cast<u32>(c)].xyz();
        };
        draw.global_ambient = color(MaterialColor::Emission);
        // Reflection's tables take specular 1's colour (the player turns them on as this does);
        // without its red table the flag does nothing.
        const bool reflection =
            (mat.frag_flags & 32) != 0 && mat.luts[Material::ReflecR].has_value();
        for (const u32 k : set) {
            const Light& l = scene.lights[k];
            switch (l.kind) {
            case Light::Kind::Ambient:
                draw.global_ambient += l.colors[0].xyz() * color(MaterialColor::Ambient);
                break;
            case Light::Kind::Hemisphere:
                if (hemisphere == nullptr) {
                    hemisphere = &l;
                }
                break;
            case Light::Kind::Fragment:
                // The lights stay in the world while the HOME turn turns the model.
                draw.lights.push_back(
                    {l.point ? Point(view, l.position) : Normalized(Direction(view, -l.direction)),
                     l.colors[0].xyz() * color(MaterialColor::Ambient),
                     l.colors[1].xyz() * color(MaterialColor::Diffuse),
                     l.colors[2].xyz() * color(MaterialColor::Specular0),
                     reflection ? Vec3f{1.0f, 1.0f, 1.0f}
                                : l.colors[3].xyz() * color(MaterialColor::Specular1),
                     l.point});
                break;
            }
        }
        if (d.normals.empty()) {
            // Without normals the lights give their ambient alone.
            for (const DrawLight& l : draw.lights) {
                draw.global_ambient += l.ambient;
            }
            draw.lights.clear();
        }
        if (draw.lights.empty()) {
            draw.lights.push_back({{0.0f, 0.0f, 1.0f}, {}, {}, {}, {}});
        }
        draw.global_ambient = Clamp01(draw.global_ambient);
        const bool hemi = (mat.flags & 4) != 0 && hemisphere != nullptr && !d.normals.empty();

        for (const SubMesh& sub : shape.submeshes) {
            // The submesh's matrix palette: its bones' world matrices, times the bind pose's
            // inverse for smooth skinning (whose vertices are in model space).
            std::vector<Mat4> palette;
            for (const u32 b : sub.bones) {
                palette.push_back(sub.skinning == 2
                                      ? world[b] * Rows34(model.bones[b].inverse_world)
                                      : world[b]);
            }
            if (palette.empty()) {
                palette.push_back(turn);
            }
            draw.first_vertex = static_cast<u32>(out.vertices.size());
            draw.first_index = static_cast<u32>(out.indices.size());
            draw.index_count = static_cast<u32>(sub.indices.size());
            for (const u32 v : sub.vertices) {
                Mat4 m{};
                if (sub.skinning == 2 && !d.bone_indices.empty()) {
                    for (u32 k = 0; k < d.weights; k++) {
                        const u32 b = d.bone_indices[v][k];
                        const float w = d.bone_weights[v][k];
                        for (u32 r = 0; r < 4; r++) {
                            for (u32 c = 0; c < 4; c++) {
                                m[r][c] += palette[b][r][c] * w;
                            }
                        }
                    }
                } else if (sub.skinning == 1 && !d.bone_indices.empty()) {
                    m = palette[d.bone_indices[v][0]];
                } else {
                    m = palette[0];
                }
                const Vec3f wp = Point(m, d.positions[v]);
                const Vec3f vp = Point(view, wp);
                const Vec4f c = Transform(clip, Common::MakeVec(wp, 1.0f));
                GpuVertex g{};
                // The picture's up is the PICA's +x and its right the PICA's -y; depth from 0
                // at the near plane to -w at the far one.
                g.position = {c.y, -c.x, -(c.z + c.w) / 2.0f, c.w};
                Vec3f wn{0.0f, 0.0f, 1.0f}, nv{0.0f, 0.0f, 1.0f};
                if (!d.normals.empty()) {
                    wn = Normalized(Direction(m, d.normals[v]));
                    nv = Normalized(Direction(view, wn));
                }
                g.quaternion = NormalQuaternion(nv);
                g.view = {-vp.x, -vp.y, -vp.z};
                Vec4f col = d.colors[v];
                if (hemi) {
                    // The vertex shader's hemisphere light: ground to sky by the normal's height
                    // along the light's direction.
                    const float w = Dot(wn, Normalized(hemisphere->direction)) * 0.5f + 0.5f;
                    const Vec3f h =
                        hemisphere->colors[0].xyz() * (1.0f - w) + hemisphere->colors[1].xyz() * w;
                    col = Common::MakeVec(col.xyz() * h, col.w);
                }
                g.color = {col.x, col.y, col.z, col.w};
                std::array<Vec3f, 3> tc;
                for (u32 k = 0; k < 3; k++) {
                    const TextureCoord& coord = coords[k];
                    Vec2f uv = d.uvs[coord.source][v];
                    if (coord.mapping == 1) {
                        // A cube map: the view's direction (taken as straight ahead) reflected
                        // off the normal, back in world space.
                        const Vec3f e{0.0f, 0.0f, -1.0f};
                        const Vec3f r = e - nv * (2.0f * Dot(nv, e));
                        tc[k] = Normalized(Direction(RigidInverse(view), r));
                        continue;
                    }
                    if (coord.mapping == 2) {
                        uv = {nv.x * 0.5f + 0.5f, nv.y * 0.5f + 0.5f};
                    }
                    const Mat4& t = coord_matrix[k];
                    tc[k] = {t[0][0] * uv.x + t[0][1] * uv.y + t[0][3],
                             t[1][0] * uv.x + t[1][1] * uv.y + t[1][3], 0.0f};
                }
                g.texcoord0 = {tc[0].x, tc[0].y, tc[0].z};
                g.texcoord1 = {tc[1].x, tc[1].y};
                g.texcoord2 = {tc[2].x, tc[2].y};
                out.vertices.push_back(g);
            }
            out.indices.insert(out.indices.end(), sub.indices.begin(), sub.indices.end());
            out.draws.push_back(draw);
        }
    }
}

} // namespace VitaFrontend::Banner
