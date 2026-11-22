// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <string>
#include <vector>

#include "citra_vita/banner_cgfx.h"
#include "common/common_types.h"
#include "common/vector_math.h"

namespace VitaFrontend::Banner {

/// A vertex as the banner's vertex shader passes it on to the PICA's rasteriser: its clip
/// position (the top screen's quarter turn and the PICA's depth range applied), the normal as a
/// quaternion and the view vector in view space, the colour and the texture coordinates (the
/// first with a third component for a cube map).
struct GpuVertex {
    std::array<float, 4> position;
    std::array<float, 4> quaternion;
    std::array<float, 4> color;
    std::array<float, 3> texcoord0;
    std::array<float, 2> texcoord1;
    std::array<float, 2> texcoord2;
    std::array<float, 3> view;
};
static_assert(sizeof(GpuVertex) == 22 * sizeof(float));

/// A fragment light as the PICA's light registers take it: its direction towards the light in
/// view space, or a point light's position in view space; its colours times the material's.
struct DrawLight {
    Common::Vec3f direction;
    Common::Vec3f ambient, diffuse, specular0, specular1;
    bool point{};
};

/// One submesh's draw: its material, its colours at this frame, and its vertices and
/// triangles in the frame's lists.
struct Draw {
    const Material* material{};
    std::array<Common::Vec4f, static_cast<u32>(MaterialColor::Count)> colors{};
    u32 first_vertex{}, first_index{}, index_count{};
    /// Fragment lighting: the global ambient (emission and the ambient lights) and the lights
    /// (at least one; a mesh without normals gets a dark one).
    Common::Vec3f global_ambient;
    std::vector<DrawLight> lights;
};

struct Frame {
    std::vector<GpuVertex> vertices;
    std::vector<u16> indices;
    std::vector<Draw> draws;
};

/// The banner's scene at a frame, as the HOME Menu animates it: the skeletal and material
/// animations looped, the whole model turned about Y once a loop (once in 600 frames when it is
/// not animated), billboards facing the camera.
class SceneAnimator {
public:
    explicit SceneAnimator(const Scene& scene);

    /// Why the scene cannot be drawn (a vertex on a bone its submesh does not have, a vertex
    /// value that is not finite, more lights on a mesh than the PICA's eight); empty when it can.
    [[nodiscard]] const std::string& Error() const {
        return error;
    }

    /// The scene at `frame` (60 a second) into `out`. Only for a scene without an Error.
    void Evaluate(u32 frame, Frame& out) const;

    using Mat4 = std::array<std::array<float, 4>, 4>;

private:
    /// A shape's vertex attributes as floats.
    struct ShapeData {
        std::vector<Common::Vec3f> positions, normals;
        std::vector<Common::Vec4f> colors;
        std::array<std::vector<Common::Vec2f>, 3> uvs;
        std::vector<std::array<u32, 4>> bone_indices;
        std::vector<Common::Vec4f> bone_weights;
        u32 weights{};
    };

    /// The skeletal animation's loop in frames (0: none).
    u32 Loop() const;
    /// The HOME Menu's turn of the whole model at `frame`.
    Mat4 Turn(u32 frame) const;
    struct ModelData {
        const Model* model{};
        std::vector<ShapeData> shapes;
        /// Each bone's animation, by the bones' order (null: none).
        std::vector<const BoneAnimation*> bone_animations;
        /// Each material's animated elements.
        std::vector<std::vector<const MaterialAnimation*>> material_animations;
    };

    std::vector<Mat4> BoneWorld(const ModelData& m, u32 frame) const;
    /// The lights on the material: its light set's (all of them without sets), by index.
    std::vector<u32> LightSet(const Material& mat) const;

    const Scene& scene;
    std::vector<ModelData> models;
    Mat4 view{}, projection{};
    std::string error;
};

} // namespace VitaFrontend::Banner
