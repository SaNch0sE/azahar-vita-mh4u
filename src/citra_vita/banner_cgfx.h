// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/vector_math.h"

/**
 * A 3DS title's banner as the HOME Menu shows it: the ExeFS's "banner" file (CBMD) holds an
 * LZ11-compressed CGFX scene (NintendoWare for CTR's model format: a skinned model, its
 * materials as PICA register values, textures in the PICA's own tiled formats, lighting tables,
 * lights, a camera and the skeletal and material animations) and the jingle (CWAV).
 *
 * The layouts are SPICA's, corrected where it is wrong for real banners: billboard modes one
 * off, UnifiedHermite48 curves without frame scale, revision 5 quaternion track flags 8 bytes
 * off and a word after each baked frame. The light sets (CENV) are read too.
 */
namespace VitaFrontend::Banner {

/// One animated float: keys of (frame, value, in slope, out slope), or a constant.
struct Curve {
    enum class Kind { Hermite, Linear, Step };
    Kind kind{Kind::Hermite};
    std::vector<std::array<float, 4>> keys;

    [[nodiscard]] float Evaluate(float frame) const;
};

/// A quaternion bone track's part: one value a frame from `start` (a constant has one).
struct BakedTrack {
    float start{};
    std::vector<Common::Vec4f> frames;
};

/// A bone's animation: either curves for its scale, rotation (Euler) and translation, or baked
/// tracks with the rotation as a quaternion.
struct BoneAnimation {
    /// Its animation's loop, in frames.
    float frames{};
    /// Scale x, y, z, rotation x, y, z, translation x, y, z.
    std::array<std::optional<Curve>, 9> curves;
    std::optional<BakedTrack> scale, rotation, translation;
};

/// A material animation's element: a texture coordinate's scale, rotation or translation, or
/// one of the material's colours.
struct MaterialAnimation {
    enum class Target { CoordScale, CoordRotate, CoordTranslate, Color };
    std::string material;
    Target target{};
    /// The texture coordinate's index, or the colour's (MaterialColors' order).
    u32 index{};
    /// Its animation's loop, in frames.
    float frames{};
    std::vector<std::optional<Curve>> curves;
};

/// The scene's animations: the common scene's, with a language scene's elements added (each
/// element loops on its own animation's frames); `frames` is the common one's.
struct Animation {
    float frames{};
    std::map<std::string, BoneAnimation> bones;
    std::vector<MaterialAnimation> materials;
};

/// A texture as the PICA reads it: its levels (a cube: each face's levels, face after face,
/// +X -X +Y -Y +Z -Z) in its tiled format.
struct Texture {
    u32 width{}, height{}, levels{}, format{};
    bool cube{};
    std::vector<u8> data;
    u32 face_bytes{};
};

/// A fragment lighting table: its 256 entries as the PICA's LUT data registers take them.
struct LutTable {
    bool absolute{};
    std::array<u32, 256> entries{};
};

/// A vertex attribute: its name (0 position, 1 normal, 3 colour, 4-6 texture coordinates,
/// 7 bone indices, 8 bone weights), GL data type, elements, scale and offset in the vertex.
struct Attribute {
    u32 name{}, type{}, elements{};
    float scale{};
    u32 offset{};
};

struct SubMesh {
    /// The skinning: 0 none, 1 rigid (one bone a vertex), 2 smooth (weighted).
    u32 skinning{};
    std::vector<u32> bones;
    /// The vertices the submesh uses (the shape's), and its triangles over that list.
    std::vector<u32> vertices;
    std::vector<u16> indices;
};

struct Shape {
    Common::Vec3f position_offset;
    u32 count{};
    u32 stride{};
    std::vector<u8> vertices;
    std::vector<Attribute> attributes;
    /// Attributes with one value for every vertex: name, value.
    std::vector<std::pair<u32, Common::Vec4f>> fixed;
    std::vector<SubMesh> submeshes;
};

struct Mesh {
    u32 shape{}, material{};
    u8 priority{};
};

/// The material's colours in MaterialColors' order.
enum class MaterialColor : u32 {
    Emission,
    Ambient,
    Diffuse,
    Specular0,
    Specular1,
    Constant0,
    Count = Constant0 + 6,
};

struct TextureCoord {
    /// The UV set it reads, its mapping (0 UV, 1 cube environment, 2 sphere environment) and
    /// its transform's convention (0 Maya, 1 SoftImage, 2 3ds Max).
    u32 source{}, mapping{}, transform_type{};
    Common::Vec2f scale;
    float rotation{};
    Common::Vec2f translation;
    /// The stored matrix (3x4 by rows), which the unanimated coordinate uses.
    std::array<float, 12> transform{};
};

struct TextureMapper {
    std::string texture;
    /// The texture unit's PARAM (filters, wrap), BORDER_COLOR and LOD register values.
    u32 param{}, border{}, lod{};
};

/// A lighting table's use: its input (LightingLutInput), scale (LightingScale) and table.
struct LutUse {
    u32 input{}, scale{};
    std::string set, sampler;
};

struct Material {
    std::string name;
    /// GfxMaterialFlags: 1 fragment lighting, 4 hemisphere lighting.
    u32 flags{};
    /// The draw layer: 0 opaque, 1 translucent, 2 subtractive, 3 additive (drawn in order).
    u32 layer{};
    std::array<Common::Vec4f, static_cast<u32>(MaterialColor::Count)> colors{};
    u32 cull{};
    /// GfxFragOpDepth's flags (1 test, 2 write) and the depth register's value (DEPTH_COLOR_MASK).
    u32 depth_flags{}, depth{};
    /// COLOR_OPERATION, BLEND_FUNC, LOGIC_OP, BLEND_COLOR, STENCIL_TEST, STENCIL_OP and
    /// FRAGOP_ALPHA_TEST as the material sets them.
    u32 color_operation{}, blend_func{}, logic_op{}, blend_color{};
    u32 stencil_test{}, stencil_op{}, alpha_test{};
    struct Stage {
        /// The colour its constant is: 0-5 Constant0-5, 6-10 Emission to Specular1.
        u32 constant{};
        u32 source{}, operand{}, combiner{}, scale{};
    };
    std::array<Stage, 6> stages{};
    u32 update_buffer{}, buffer_color{};
    /// NW4C's fragment lighting flags (1 clamp highlights, 2 distribution 0, 4 distribution 1,
    /// 8 geometric factor 0, 16 geometric factor 1, 32 reflection) and the Fresnel selector (1
    /// primary alpha, 2 secondary alpha).
    u32 frag_flags{}, fresnel{};
    enum Lut { Dist0, Dist1, ReflecR, ReflecG, ReflecB, Fresnel, LutCount };
    std::array<std::optional<LutUse>, LutCount> luts;
    std::array<TextureCoord, 3> coords{};
    std::array<std::optional<TextureMapper>, 3> mappers;
    u32 light_set{};
};

struct Bone {
    std::string name;
    s32 parent{};
    Common::Vec3f scale, rotation, translation;
    /// The bind pose's inverse world matrix, 3x4 by rows.
    std::array<float, 12> inverse_world{};
    /// NW4C's billboard mode: 0 off, 1 world, 2 world viewpoint, 3 screen, 4 screen viewpoint,
    /// 5 Y axial, 6 Y axial viewpoint.
    u32 billboard{};
};

struct Light {
    enum class Kind { Fragment, Hemisphere, Ambient };
    Kind kind{};
    std::string name;
    /// Fragment: ambient, diffuse, specular 0, specular 1. Hemisphere: ground, sky.
    /// Ambient: its colour.
    std::array<Common::Vec4f, 4> colors{};
    Common::Vec3f direction;
    /// Fragment: a point light (NW4C's type 1), lit from its position rather than its direction.
    bool point{};
    Common::Vec3f position;

    bool operator==(const Light&) const = default;
};

struct Camera {
    enum class View { Rotation, LookAt, Aim };
    View view{};
    Common::Vec3f eye;
    /// LookAt and Aim: the target. Rotation: the rotation.
    Common::Vec3f target;
    Common::Vec3f up{0.0f, 1.0f, 0.0f};
    float fovy{}, near{}, far{};

    bool operator==(const Camera&) const = default;
};

/// A model: its meshes, each a shape drawn with a material, and the skeleton they hang on.
struct Model {
    std::vector<Material> materials;
    std::vector<Shape> shapes;
    std::vector<Mesh> meshes;
    std::vector<Bone> bones;
};

struct Scene {
    std::map<std::string, Texture> textures;
    /// By the set's and the sampler's name.
    std::map<std::pair<std::string, std::string>, LutTable> luts;
    /// The common scene's model, then the console language's (the title's logo, where the
    /// banner has one a language).
    std::vector<Model> models;
    /// The enabled lights; the light sets list them by index (no sets: all of them light).
    std::vector<Light> lights;
    std::optional<std::map<u32, std::vector<u32>>> light_sets;
    std::optional<Camera> camera;
    std::optional<Animation> skeletal, material;
};

/// A CBMD's language scenes the game list shows, by their index among its scenes (the others
/// are European French to Russian and American French to Portuguese).
enum class BannerLanguage : u32 {
    EuropeanEnglish = 1,
    Japanese = 9,
    AmericanEnglish = 10,
};

/// The banner file's scene as the HOME Menu shows it: the common scene with the first of
/// `languages` the banner has a scene for added (with none of them, the common scene alone).
/// nullopt, and `error` why, for a file that is no CBMD or one this reader does not take apart
/// (damaged, or of a kind not seen).
std::optional<Scene> ReadBanner(std::span<const u8> banner,
                                std::span<const BannerLanguage> languages, std::string& error);

/// The banner's jingle, the CWAV after the scenes (empty when there is none).
std::span<const u8> BannerJingle(std::span<const u8> banner);

} // namespace VitaFrontend::Banner
