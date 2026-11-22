// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <string_view>

#include <fmt/format.h>

#include "citra_vita/banner_cgfx.h"
#include "common/assert.h"
#include "common/logging/log.h"

namespace VitaFrontend::Banner {

namespace {

/// The CGFX's DATA dictionaries, in order (the banners have 15: no fog animations).
enum Section : u32 {
    Models,
    Textures,
    Luts,
    Materials,
    Shaders,
    Cameras,
    Lights,
    Fogs,
    Environments,
    SkeletalAnimations,
    MaterialAnimations,
    VisibilityAnimations,
    CameraAnimations,
    LightAnimations,
    Emitters,
    SectionCount,
};

// Type ids.
constexpr u32 ModelPlain = 0x40000012;
constexpr u32 ModelSkeletal = 0x40000092;
constexpr u32 TextureImage = 0x20000011;
constexpr u32 TextureCube = 0x20000009;
constexpr u32 BufferInterleaved = 0x40000002;
constexpr u32 BufferFixed = 0x80000000;
constexpr u32 LightFragment = 0x400000a2;
constexpr u32 LightHemisphere = 0x40000122;
constexpr u32 LightAmbient = 0x40000422;
constexpr u32 CameraObject = 0x4000000a;
constexpr u32 ViewRotation = 0x20000000;
constexpr u32 ViewLookAt = 0x40000000;
constexpr u32 ViewAim = 0x80000000;
constexpr u32 ProjectionPerspective = 0x20000000;

// GL data types.
constexpr u32 GlByte = 5120;
constexpr u32 GlUnsignedByte = 5121;
constexpr u32 GlShort = 5122;
constexpr u32 GlUnsignedShort = 5123;
constexpr u32 GlFloat = 5126;

// PICA registers the lighting tables' commands write.
constexpr u32 RegLutIndex = 0x1c5;
constexpr u32 RegLutData0 = 0x1c8;
constexpr u32 RegLutData7 = 0x1cf;

constexpr float Pi = 3.14159265358979f;
/// The longest name read.
constexpr std::size_t MaxName = 256;
/// The largest scene unpacked.
constexpr u32 MaxSceneBytes = 16 * 1024 * 1024;
/// The longest animation, in frames.
constexpr float MaxFrames = 1e6f;
/// The most vertices a submesh draws (its indices are 16 bits).
constexpr std::size_t MaxSubMeshVertices = 0x10000;

/// A vertex attribute's GL type's bytes; 0 for a type a vertex does not take.
u32 GlTypeBytes(u32 type) {
    switch (type) {
    case GlByte:
    case GlUnsignedByte:
        return 1;
    case GlShort:
    case GlUnsignedShort:
        return 2;
    case GlFloat:
        return 4;
    }
    return 0;
}

/// Reads the CGFX's little-endian words, its self-relative pointers (from the pointer's own
/// address, wrapping at 32 bits) and its NUL-terminated names. A read past the end, a float that
/// is not finite, a name longer than MaxName or a list that runs past the end reads as zero
/// (empty) and fails the scene: the first failure stays for the log.
class Reader {
public:
    explicit Reader(std::span<const u8> data_) : data{data_} {}

    [[nodiscard]] bool Failed() const {
        return !error.empty();
    }
    [[nodiscard]] const std::string& Error() const {
        return error;
    }
    template <typename... Args>
    void Fail(fmt::format_string<Args...> format, Args&&... args) const {
        if (error.empty()) {
            error = fmt::format(format, std::forward<Args>(args)...);
        }
    }
    [[nodiscard]] bool Fits(u64 at, u64 bytes) const {
        return at <= data.size() && bytes <= data.size() - at;
    }

    template <typename T>
    T Get(u32 at) const {
        if (!Fits(at, sizeof(T))) {
            Fail("a read at {:#x} past the end ({:#x})", at, data.size());
            return T{};
        }
        T v;
        std::memcpy(&v, data.data() + at, sizeof(T));
        return v;
    }
    u32 U32(u32 at) const {
        return Get<u32>(at);
    }
    float F32(u32 at) const {
        const float v = Get<float>(at);
        if (!std::isfinite(v)) {
            Fail("the float at {:#x} is {}", at, v);
            return 0.0f;
        }
        return v;
    }
    Common::Vec2f Vec2(u32 at) const {
        return {F32(at), F32(at + 4)};
    }
    Common::Vec3f Vec3(u32 at) const {
        return {F32(at), F32(at + 4), F32(at + 8)};
    }
    Common::Vec4f Vec4(u32 at) const {
        return {F32(at), F32(at + 4), F32(at + 8), F32(at + 12)};
    }
    /// The pointer at `at` as an offset, 0 for none.
    u32 Ptr(u32 at) const {
        const u32 rel = U32(at);
        return rel == 0 ? 0 : at + rel;
    }
    std::string Name(u32 at) const {
        const u32 p = Ptr(at);
        if (p == 0 || p >= data.size()) {
            Fail("a name pointer at {:#x}", at);
            return {};
        }
        const auto* begin = reinterpret_cast<const char*>(data.data() + p);
        const auto* end = static_cast<const char*>(
            std::memchr(begin, 0, std::min<std::size_t>(data.size() - p, MaxName + 1)));
        if (end == nullptr) {
            Fail("the name at {:#x} runs past {} bytes or the end", p, MaxName);
            return {};
        }
        return std::string(begin, end);
    }
    /// A list's count at `at` and its pointer after it (its elements' offset, or 0), its
    /// elements `element_bytes` each.
    std::pair<u32, u32> List(u32 at, u32 element_bytes) const {
        const u32 count = U32(at);
        const u32 p = Ptr(at + 4);
        if (count != 0 && (p == 0 || !Fits(p, u64{count} * element_bytes))) {
            Fail("a list of {} at {:#x} past the end", count, at);
            return {0, 0};
        }
        return {count, p};
    }
    /// A list of pointers: the objects' offsets.
    std::vector<u32> Refs(u32 at) const {
        const auto [count, p] = List(at, 4);
        std::vector<u32> out;
        for (u32 i = 0; i < count; i++) {
            out.push_back(Ptr(p + 4 * i));
        }
        return out;
    }
    /// A byte buffer: its length at `at`, its pointer after it.
    std::span<const u8> Bytes(u32 at) const {
        const auto [size, p] = List(at, 1);
        return data.subspan(p, size);
    }
    /// A dictionary's entries (name, value offset) from the DICT at `dict`: its nodes after the
    /// root, each a reference bit, two node indices, a name pointer and a value pointer.
    std::vector<std::pair<std::string, u32>> Dict(u32 dict) const {
        std::vector<std::pair<std::string, u32>> out;
        if (dict == 0) {
            return out;
        }
        if (U32(dict) != 0x54434944) { // "DICT"
            Fail("no DICT at {:#x}", dict);
            return out;
        }
        const u32 count = U32(dict + 8);
        if (!Fits(u64{dict} + 12, (u64{count} + 1) * 16)) {
            Fail("a DICT of {} entries at {:#x} past the end", count, dict);
            return out;
        }
        for (u32 i = 1; i <= count; i++) {
            const u32 node = dict + 12 + 16 * i;
            out.emplace_back(Name(node + 8), Ptr(node + 12));
        }
        return out;
    }
    /// A GfxDict field: its count at `at` and the DICT's pointer after it.
    std::vector<std::pair<std::string, u32>> DictField(u32 at) const {
        return U32(at) == 0 ? std::vector<std::pair<std::string, u32>>{} : Dict(Ptr(at + 4));
    }

private:
    std::span<const u8> data;
    mutable std::string error;
};

/// The scene unpacked; nullopt (and `error` why) for data that is no LZ10/LZ11 stream, ends early,
/// refers back before its start or unpacks to more than MaxSceneBytes.
std::optional<std::vector<u8>> Lz11(std::span<const u8> in, std::string& error) {
    if (in.size() < 4 || (in[0] != 0x10 && in[0] != 0x11)) {
        error = fmt::format("the scene is not LZ10/LZ11 compressed ({:#x})",
                            in.empty() ? 0 : in[0]);
        return std::nullopt;
    }
    const bool lz11 = in[0] == 0x11;
    std::size_t p = 4;
    u32 size = (in[1] | in[2] << 8 | in[3] << 16);
    if (size == 0 && in.size() >= 8) {
        size = in[4] | in[5] << 8 | in[6] << 16 | in[7] << 24;
        p = 8;
    }
    if (size > MaxSceneBytes) {
        error = fmt::format("the scene unpacks to {:#x} bytes", size);
        return std::nullopt;
    }
    std::vector<u8> out;
    out.reserve(size);
    bool ended = false;
    const auto next = [&]() -> u32 {
        if (p >= in.size()) {
            ended = true;
            return 0;
        }
        return in[p++];
    };
    while (out.size() < size) {
        const u32 flags = next();
        for (u32 i = 0; i < 8 && out.size() < size; i++) {
            if ((flags & (0x80 >> i)) == 0) {
                out.push_back(static_cast<u8>(next()));
                if (ended) {
                    error = "the compressed scene ends early";
                    return std::nullopt;
                }
                continue;
            }
            const u32 b0 = next();
            u32 n, disp;
            if (!lz11) {
                const u32 b1 = next();
                n = (b0 >> 4) + 3;
                disp = ((b0 & 0xf) << 8 | b1) + 1;
            } else if (b0 >> 4 == 0) {
                const u32 b1 = next(), b2 = next();
                n = ((b0 & 0xf) << 4 | b1 >> 4) + 0x11;
                disp = ((b1 & 0xf) << 8 | b2) + 1;
            } else if (b0 >> 4 == 1) {
                const u32 b1 = next(), b2 = next(), b3 = next();
                n = ((b0 & 0xf) << 12 | b1 << 4 | b2 >> 4) + 0x111;
                disp = ((b2 & 0xf) << 8 | b3) + 1;
            } else {
                const u32 b1 = next();
                n = (b0 >> 4) + 1;
                disp = ((b0 & 0xf) << 8 | b1) + 1;
            }
            if (ended) {
                error = "the compressed scene ends early";
                return std::nullopt;
            }
            if (disp > out.size()) {
                error = "a back reference before the scene's start";
                return std::nullopt;
            }
            for (u32 k = 0; k < n; k++) {
                out.push_back(out[out.size() - disp]);
            }
        }
    }
    out.resize(size);
    return out;
}

std::vector<u32> Words(const Reader& r, u32 at, u32 count) {
    std::vector<u32> out(count);
    for (u32 i = 0; i < count; i++) {
        out[i] = r.U32(at + 4 * i);
    }
    return out;
}

/// A material animation element's name: Materials["NAME"].PATH, the path one of
/// TextureCoordinators[N].FIELD, MaterialColor.FIELD and TextureMappers[N].FIELD.
struct ElementName {
    enum class Group { Coordinator, Color, Mapper };
    std::string material;
    Group group{};
    u32 index{};
    std::string field;
};

std::optional<ElementName> ParseElementName(std::string_view name) {
    constexpr std::string_view head = "Materials[\"";
    const std::size_t close = name.rfind("\"].");
    if (!name.starts_with(head) || close == std::string_view::npos || close <= head.size()) {
        return std::nullopt;
    }
    ElementName e;
    e.material = name.substr(head.size(), close - head.size());
    std::string_view path = name.substr(close + 3);
    // GROUP[N]. at the path's start, the digit into `index`.
    const auto indexed = [&path](std::string_view group, u32& index) {
        if (!path.starts_with(group) || path.size() < group.size() + 3 ||
            path[group.size()] < '0' || path[group.size()] > '9' ||
            path.substr(group.size() + 1, 2) != "].") {
            return false;
        }
        index = static_cast<u32>(path[group.size()] - '0');
        path.remove_prefix(group.size() + 3);
        return true;
    };
    const auto word = [](std::string_view s) {
        return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '_';
        });
    };
    if (indexed("TextureCoordinators[", e.index)) {
        e.group = ElementName::Group::Coordinator;
        if (!word(path)) {
            return std::nullopt;
        }
    } else if (path.starts_with("MaterialColor.")) {
        e.group = ElementName::Group::Color;
        path.remove_prefix(std::string_view{"MaterialColor."}.size());
        if (!word(path)) {
            return std::nullopt;
        }
    } else if (indexed("TextureMappers[", e.index)) {
        e.group = ElementName::Group::Mapper;
        if (path.empty()) {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }
    e.field = path;
    return e;
}

/// Where `cond` does not hold, fails the scene (the reader keeps the first failure) and returns
/// an empty value.
#define REQUIRE(cond, ...)                                                                         \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            r.Fail(__VA_ARGS__);                                                                   \
            return {};                                                                             \
        }                                                                                          \
    } while (false)

class Parser {
public:
    explicit Parser(std::span<const u8> data) : r{data} {}

    /// The common scene into `scene`, or (`language`) a language's: its textures (in place of
    /// the common ones of their names), tables and models; the rest is the common scene's.
    /// False (Error() why) for a scene this does not take apart.
    [[nodiscard]] bool Parse(Scene& scene, bool language) {
        REQUIRE(r.U32(0) == 0x58464743, "the scene is no CGFX"); // "CGFX"
        revision = r.U32(8);
        const u32 data = r.Get<u16>(6);
        REQUIRE(r.U32(data) == 0x41544144, "no DATA section"); // "DATA"
        std::array<std::vector<std::pair<std::string, u32>>, SectionCount> sections;
        for (u32 i = 0; i < SectionCount; i++) {
            sections[i] = r.DictField(data + 8 + 8 * i);
        }
        for (const auto& [name, at] : sections[Textures]) {
            scene.textures.insert_or_assign(name, ReadTexture(at));
        }
        for (const auto& [set, at] : sections[Luts]) {
            for (const auto& [sampler, s] : r.DictField(at + 0x18)) {
                scene.luts.insert_or_assign(std::make_pair(set, sampler), ReadLut(s));
            }
        }
        REQUIRE(language || !sections[Models].empty(), "the scene has no model");
        for (const auto& [name, at] : sections[Models]) {
            scene.models.push_back(ReadModel(at));
        }
        // The animations: a language scene's elements join the common scene's (a logo can have a
        // material animation of its own).
        const auto add = [&](std::optional<Animation>& into, Section section, bool material) {
            for (const auto& [name, at] : sections[section]) {
                Animation a = ReadAnimation(at, material);
                if (!into) {
                    into = std::move(a);
                    continue;
                }
                into->bones.merge(a.bones);
                std::move(a.materials.begin(), a.materials.end(),
                          std::back_inserter(into->materials));
            }
        };
        add(scene.skeletal, SkeletalAnimations, false);
        add(scene.material, MaterialAnimations, true);
        if (language) {
            // A language scene may repeat the common scene's camera and lights; one of its
            // own is a case not seen.
            if (!sections[Cameras].empty()) {
                REQUIRE(scene.camera &&
                            ReadCamera(sections[Cameras].front().second) == *scene.camera,
                        "a language's scene with a camera of its own");
            }
            for (const auto& [name, at] : sections[Lights]) {
                REQUIRE(std::find(scene.lights.begin(), scene.lights.end(),
                                  ReadLight(name, at)) != scene.lights.end(),
                        "a language's scene with light {} of its own", name);
            }
            REQUIRE(sections[Environments].empty(),
                    "a language's scene with an environment of its own");
            return !r.Failed();
        }
        std::map<std::string, u32> light_index;
        for (const auto& [name, at] : sections[Lights]) {
            if (r.U32(at + 0xb4) == 0) {
                continue; // disabled
            }
            light_index.emplace(name, static_cast<u32>(scene.lights.size()));
            scene.lights.push_back(ReadLight(name, at));
        }
        if (!sections[Environments].empty()) {
            scene.light_sets = ReadLightSets(sections[Environments].front().second, light_index);
        }
        if (!sections[Cameras].empty()) {
            scene.camera = ReadCamera(sections[Cameras].front().second);
        }
        return !r.Failed();
    }

    [[nodiscard]] const std::string& Error() const {
        return r.Error();
    }

private:
    /// The PICA command list's register writes, in order (consecutive writes one per register).
    std::vector<std::pair<u32, u32>> Writes(std::span<const u32> cmds) const {
        std::vector<std::pair<u32, u32>> out;
        std::size_t i = 0;
        while (i + 1 < cmds.size()) {
            u32 param = cmds[i];
            const u32 head = cmds[i + 1];
            i += 2;
            const u32 reg = head & 0xffff, extra = head >> 20 & 0x7ff;
            const bool consecutive = head >> 31;
            for (u32 k = 0; k <= extra; k++) {
                out.emplace_back(consecutive ? reg + k : reg, param);
                if (k < extra) {
                    REQUIRE(i < cmds.size(), "a command list ends early");
                    param = cmds[i++];
                }
            }
            i += i & 1;
        }
        return out;
    }

    /// The value the commands write to `reg` last (`fallback` when they leave it).
    u32 Written(std::span<const u32> cmds, u32 reg, u32 fallback) const {
        for (const auto& [at, v] : Writes(cmds)) {
            if (at == reg) {
                fallback = v;
            }
        }
        return fallback;
    }

    Texture ReadTexture(u32 at) {
        const u32 type = r.U32(at);
        REQUIRE(type == TextureImage || type == TextureCube, "texture type {:#x}", type);
        Texture t;
        t.height = r.U32(at + 0x18);
        t.width = r.U32(at + 0x1c);
        t.levels = r.U32(at + 0x28);
        t.format = r.U32(at + 0x34);
        t.cube = type == TextureCube;
        const u32 faces = t.cube ? 6 : 1;
        for (u32 f = 0; f < faces; f++) {
            const u32 image = r.Ptr(at + 0x38 + 4 * f);
            REQUIRE(image != 0, "a texture without its image");
            const std::span<const u8> raw = r.Bytes(image + 8);
            REQUIRE(f == 0 || raw.size() == t.face_bytes, "a cube's faces differ");
            t.face_bytes = static_cast<u32>(raw.size());
            t.data.insert(t.data.end(), raw.begin(), raw.end());
        }
        return t;
    }

    LutTable ReadLut(u32 at) {
        LutTable t;
        t.absolute = r.U32(at + 8) != 0;
        const std::span<const u8> raw = r.Bytes(at + 0xc);
        std::vector<u32> cmds(raw.size() / 4);
        std::memcpy(cmds.data(), raw.data(), cmds.size() * 4);
        u32 index = 0;
        for (const auto& [reg, v] : Writes(cmds)) {
            if (reg == RegLutIndex) {
                index = v & 0xff;
            } else if (reg >= RegLutData0 && reg <= RegLutData7) {
                REQUIRE(index < 256, "a lighting table past 256 entries");
                t.entries[index++] = v;
            }
        }
        return t;
    }

    Model ReadModel(u32 model) {
        Model scene;
        const u32 type = r.U32(model);
        REQUIRE(type == ModelSkeletal || type == ModelPlain, "model type {:#x}", type);
        for (const auto& [name, at] : r.DictField(model + 0xbc)) {
            scene.materials.push_back(ReadMaterial(name, at));
        }
        for (const u32 at : r.Refs(model + 0xc4)) {
            scene.shapes.push_back(ReadShape(at));
        }
        for (const u32 at : r.Refs(model + 0xb4)) {
            Mesh m;
            m.shape = r.U32(at + 0x18);
            m.material = r.U32(at + 0x1c);
            m.priority = r.Get<u8>(at + 0x25);
            REQUIRE(m.shape < scene.shapes.size() && m.material < scene.materials.size(),
                    "a mesh's shape or material out of range");
            scene.meshes.push_back(m);
        }
        if (type == ModelSkeletal) {
            const u32 skeleton = r.Ptr(model + 0xe0);
            REQUIRE(skeleton != 0, "a skeletal model without its skeleton");
            const auto bones = r.DictField(skeleton + 0x18);
            for (const auto& [name, at] : bones) {
                const u32 index = r.U32(at + 8);
                REQUIRE(index < bones.size(), "bone {} at index {} of {}", name, index,
                        bones.size());
                if (scene.bones.size() <= index) {
                    scene.bones.resize(index + 1);
                }
                Bone& b = scene.bones[index];
                b.name = name;
                b.parent = r.Get<s32>(at + 0xc);
                b.scale = r.Vec3(at + 0x20);
                b.rotation = r.Vec3(at + 0x2c);
                b.translation = r.Vec3(at + 0x38);
                for (u32 i = 0; i < 12; i++) {
                    b.inverse_world[i] = r.F32(at + 0xa4 + 4 * i);
                }
                b.billboard = r.U32(at + 0xd4);
                REQUIRE(b.billboard <= 6, "bone {} billboard mode {}", name, b.billboard);
            }
            for (std::size_t i = 0; i < scene.bones.size(); i++) {
                REQUIRE(!scene.bones[i].name.empty(), "bone {} missing", i);
                REQUIRE(scene.bones[i].parent < static_cast<s32>(i), "bone {} before its parent",
                        i);
            }
        }
        for (const Shape& s : scene.shapes) {
            for (const SubMesh& sub : s.submeshes) {
                for (const u32 b : sub.bones) {
                    REQUIRE(b < scene.bones.size(), "a submesh's bone {} of {}", b,
                            scene.bones.size());
                }
            }
        }
        return scene;
    }

    Material ReadMaterial(const std::string& name, u32 at) {
        Material m;
        m.name = name;
        m.flags = r.U32(at + 0x18);
        m.layer = r.U32(at + 0x20);
        for (u32 i = 0; i < m.colors.size(); i++) {
            m.colors[i] = r.Vec4(at + 0x24 + 0x10 * i);
        }
        m.cull = r.U32(at + 0x110) & 3;
        // GfxFragOp: depth (flags, 4 command words), blend (mode, colour, 6), stencil (4).
        const u32 fo = at + 0x118;
        m.depth_flags = r.U32(fo);
        const auto depth = Words(r, fo + 4, 4);
        m.depth = Written(depth, 0x107, 0);
        const auto blend = Words(r, fo + 0x28, 6);
        m.color_operation = Written(blend, 0x100, 0xe40100);
        m.blend_func = Written(blend, 0x101, 0x01010000);
        m.logic_op = Written(blend, 0x102, 0);
        m.blend_color = Written(blend, 0x103, 0);
        const auto stencil = Words(r, fo + 0x40, 4);
        m.stencil_test = Written(stencil, 0x105, 0);
        m.stencil_op = Written(stencil, 0x106, 0);
        for (u32 i = 0; i < 3; i++) {
            const u32 c = at + 0x16c + 0x58 * i;
            TextureCoord& tc = m.coords[i];
            tc.source = r.U32(c);
            tc.mapping = r.U32(c + 4);
            tc.transform_type = r.U32(c + 0xc);
            tc.scale = r.Vec2(c + 0x10);
            tc.rotation = r.F32(c + 0x18);
            tc.translation = r.Vec2(c + 0x1c);
            for (u32 k = 0; k < 12; k++) {
                tc.transform[k] = r.F32(c + 0x28 + 4 * k);
            }
            REQUIRE(tc.source < 3 && tc.mapping < 3 && tc.transform_type < 3,
                    "material {} texture coordinate {}: source {}, mapping {}, transform {}", name,
                    i, tc.source, tc.mapping, tc.transform_type);
        }
        for (u32 i = 0; i < 3; i++) {
            const u32 tm = r.Ptr(at + 0x274 + 4 * i);
            if (tm == 0) {
                continue;
            }
            TextureMapper mapper;
            const u32 ref = r.Ptr(tm + 8);
            if (ref != 0) {
                mapper.texture = r.Name(ref + 0x18);
            }
            const auto cmds = Words(r, tm + 0x10, 14);
            // Each unit's own registers: unit 0 at 0x81, units 1 and 2 at 0x91 and 0x99.
            const u32 base = i == 0 ? 0x81 : i == 1 ? 0x91 : 0x99;
            mapper.border = Written(cmds, base, 0);
            mapper.param = Written(cmds, base + 2, 0);
            mapper.lod = Written(cmds, base + 3, 0);
            m.mappers[i] = mapper;
        }
        const u32 fs = r.Ptr(at + 0x288);
        REQUIRE(fs != 0, "material {} without its fragment shader", name);
        m.frag_flags = r.U32(fs + 0x10);
        m.fresnel = r.U32(fs + 0x18);
        if (const u32 luts = r.Ptr(fs + 0x28); luts != 0) {
            // GfxFragLightLUTs: ReflecR, ReflecG, ReflecB, Dist0, Dist1, Fresnel.
            constexpr Material::Lut order[] = {Material::ReflecR, Material::ReflecG,
                                               Material::ReflecB, Material::Dist0,
                                               Material::Dist1,   Material::Fresnel};
            for (u32 i = 0; i < 6; i++) {
                const u32 lut = r.Ptr(luts + 4 * i);
                if (lut == 0) {
                    continue;
                }
                const u32 sampler = r.Ptr(lut + 8);
                if (sampler == 0) {
                    continue;
                }
                m.luts[order[i]] =
                    LutUse{r.U32(lut), r.U32(lut + 4), r.Name(sampler + 4), r.Name(sampler + 8)};
            }
        }
        for (u32 i = 0; i < 6; i++) {
            const u32 te = fs + 0x2c + 0x1c * i;
            Material::Stage& s = m.stages[i];
            s.constant = r.U32(te);
            REQUIRE(s.constant <= 10, "material {} stage {} constant {}", name, i, s.constant);
            const auto cmds = Words(r, te + 4, 6);
            const u32 base = i < 4 ? 0xc0 + 8 * i : 0xf0 + 8 * (i - 4);
            s.source = Written(cmds, base, 0);
            s.operand = Written(cmds, base + 1, 0);
            s.combiner = Written(cmds, base + 2, 0);
            s.scale = Written(cmds, base + 4, 0);
        }
        m.alpha_test = Written(Words(r, fs + 0xd4, 2), 0x104, 0);
        const auto fcmds = Words(r, fs + 0xdc, 6);
        m.update_buffer = Written(fcmds, 0xe0, 0);
        m.buffer_color = Written(fcmds, 0xfd, 0);
        m.light_set = r.U32(at + 0x298);
        return m;
    }

    Shape ReadShape(u32 at) {
        Shape s;
        s.position_offset = r.Vec3(at + 0x20);
        bool interleaved = false;
        for (const u32 vb : r.Refs(at + 0x38)) {
            const u32 type = r.U32(vb);
            const u32 name = r.U32(vb + 4);
            if (type == BufferFixed) {
                const u32 format = r.U32(vb + 0xc);
                const u32 elements = r.U32(vb + 0x10);
                REQUIRE(format == GlFloat && elements >= 1 && elements <= 4,
                        "a fixed attribute of type {}, {} elements", format, elements);
                // Its value is a list (a count and a pointer) of the elements; its scale is 0 in
                // every banner: the value is as stored.
                const auto [count, values] = r.List(vb + 0x18, 4);
                REQUIRE(count == elements && values != 0,
                        "a fixed attribute of {} elements with {} values", elements, count);
                Common::Vec4f v{0.0f, 0.0f, 0.0f, 1.0f};
                for (u32 k = 0; k < elements; k++) {
                    v[k] = r.F32(values + 4 * k);
                }
                s.fixed.emplace_back(name, v);
                continue;
            }
            REQUIRE(type == BufferInterleaved, "vertex buffer type {:#x}", type);
            REQUIRE(!interleaved, "a shape with two interleaved buffers");
            interleaved = true;
            const std::span<const u8> raw = r.Bytes(vb + 0x14);
            s.vertices.assign(raw.begin(), raw.end());
            s.stride = r.U32(vb + 0x24);
            REQUIRE(s.stride != 0, "a vertex buffer without its stride");
            s.count = static_cast<u32>(raw.size()) / s.stride;
            for (const u32 a : r.Refs(vb + 0x28)) {
                Attribute attr{r.U32(a + 4), r.U32(a + 0x24), r.U32(a + 0x28), r.F32(a + 0x2c),
                               r.U32(a + 0x30)};
                const u32 bytes = GlTypeBytes(attr.type);
                REQUIRE(bytes != 0 && attr.elements >= 1 && attr.elements <= 4 &&
                            u64{attr.offset} + bytes * attr.elements <= s.stride,
                        "attribute {} of type {} out of its vertex", attr.name, attr.type);
                s.attributes.push_back(attr);
            }
        }
        REQUIRE(interleaved, "a shape without vertices");
        REQUIRE(std::any_of(s.attributes.begin(), s.attributes.end(),
                            [](const Attribute& a) { return a.name == 0; }),
                "a shape without positions");
        for (const u32 sm : r.Refs(at + 0x2c)) {
            SubMesh sub;
            const auto [nbones, bones] = r.List(sm, 4);
            for (u32 i = 0; i < nbones; i++) {
                sub.bones.push_back(r.U32(bones + 4 * i));
            }
            sub.skinning = r.U32(sm + 8);
            REQUIRE(sub.skinning <= 2, "skinning {}", sub.skinning);
            // The submesh's own vertex list: the shape's vertices it uses, in first use.
            std::vector<s32> local(s.count, -1);
            for (const u32 face : r.Refs(sm + 0xc)) {
                for (const u32 fd : r.Refs(face)) {
                    const u32 format = r.U32(fd);
                    const u8 mode = r.Get<u8>(fd + 4);
                    REQUIRE(mode == 0, "primitive mode {} (triangles only)", mode);
                    const std::span<const u8> raw = r.Bytes(fd + 8);
                    const bool shorts = format == GlUnsignedShort;
                    REQUIRE(shorts || format == GlUnsignedByte, "index type {}", format);
                    const u32 n = static_cast<u32>(raw.size()) / (shorts ? 2 : 1);
                    for (u32 i = 0; i < n; i++) {
                        const u32 v = shorts ? (raw[2 * i] | raw[2 * i + 1] << 8) : raw[i];
                        REQUIRE(v < s.count, "index {} of {} vertices", v, s.count);
                        if (local[v] < 0) {
                            REQUIRE(sub.vertices.size() < MaxSubMeshVertices,
                                    "a submesh of more than {} vertices", MaxSubMeshVertices);
                            local[v] = static_cast<s32>(sub.vertices.size());
                            sub.vertices.push_back(v);
                        }
                        sub.indices.push_back(static_cast<u16>(local[v]));
                    }
                }
            }
            REQUIRE(sub.indices.size() % 3 == 0, "a triangle list of {} indices",
                    sub.indices.size());
            s.submeshes.push_back(std::move(sub));
        }
        return s;
    }

    Light ReadLight(const std::string& name, u32 at) {
        Light l;
        l.name = name;
        const u32 type = r.U32(at);
        switch (type) {
        case LightFragment:
            l.kind = Light::Kind::Fragment;
            for (u32 i = 0; i < 4; i++) {
                l.colors[i] = r.Vec4(at + 0xbc + 0x10 * i);
            }
            l.direction = r.Vec3(at + 0x10c);
            // GfxLightType: 0 directional, 1 point, 2 spot (no banner seen with one).
            switch (const u32 kind = r.U32(at + 0xb8)) {
            case 0:
                break;
            case 1:
                l.point = true;
                l.position = r.Vec3(at + 0x48); // the node's translation
                break;
            default:
                r.Fail("light {} of fragment light type {}", name, kind);
                return {};
            }
            break;
        case LightHemisphere:
            l.kind = Light::Kind::Hemisphere;
            l.colors[0] = r.Vec4(at + 0xb8);
            l.colors[1] = r.Vec4(at + 0xc8);
            l.direction = r.Vec3(at + 0xd8);
            break;
        case LightAmbient:
            l.kind = Light::Kind::Ambient;
            l.colors[0] = r.Vec4(at + 0xb8);
            break;
        default:
            r.Fail("light {} of type {:#x}", name, type);
            return {};
        }
        return l;
    }

    /// The scene environment's light sets: set index -> the enabled lights it names.
    std::map<u32, std::vector<u32>> ReadLightSets(u32 env,
                                                  const std::map<std::string, u32>& index) {
        REQUIRE(r.U32(env + 4) == 0x564e4543, "no CENV"); // "CENV"
        std::map<u32, std::vector<u32>> sets;
        const auto [count, list] = r.List(env + 0x20, 4);
        for (u32 i = 0; i < count; i++) {
            const u32 set = r.Ptr(list + 4 * i);
            REQUIRE(set != 0, "light set {} missing", i);
            const u32 id = r.U32(set);
            const auto [n, lights] = r.List(set + 4, 4);
            std::vector<u32>& members = sets[id];
            for (u32 k = 0; k < n; k++) {
                const u32 light = r.Ptr(lights + 4 * k);
                REQUIRE(light != 0, "light set {}'s light {} missing", id, k);
                const std::string name = r.Name(light + 4);
                if (const auto it = index.find(name); it != index.end()) {
                    members.push_back(it->second);
                }
            }
        }
        return sets;
    }

    Camera ReadCamera(u32 at) {
        REQUIRE(r.U32(at) == CameraObject, "camera type {:#x}", r.U32(at));
        Camera c;
        c.eye = r.Vec3(at + 0x48);
        const u32 view = r.Ptr(at + 0xbc);
        const u32 projection = r.Ptr(at + 0xc0);
        REQUIRE(view != 0 && projection != 0, "a camera without its view");
        switch (r.U32(view)) {
        case ViewRotation:
            c.view = Camera::View::Rotation;
            c.target = r.Vec3(view + 8);
            break;
        case ViewLookAt:
            c.view = Camera::View::LookAt;
            c.target = r.Vec3(view + 8);
            c.up = r.Vec3(view + 0x14);
            break;
        case ViewAim:
            c.view = Camera::View::Aim;
            c.target = r.Vec3(view + 8);
            REQUIRE(std::abs(r.F32(view + 0x14)) < 1e-6f, "an aim camera's twist");
            break;
        default:
            r.Fail("camera view type {:#x}", r.U32(view));
            return {};
        }
        REQUIRE(r.U32(projection) == ProjectionPerspective, "camera projection type {:#x}",
                r.U32(projection));
        c.near = r.F32(projection + 4);
        c.far = r.F32(projection + 8);
        c.fovy = r.F32(projection + 0x10);
        REQUIRE(c.near > 0.0f && c.far > c.near && c.fovy > 0.0f && c.fovy < Pi,
                "a camera's near plane {}, far plane {}, field of view {}", c.near, c.far, c.fovy);
        return c;
    }

    /// A curve group (GfxFloatKeyFrameGroup) at `at`.
    Curve ReadCurve(u32 at) {
        Curve c;
        if (r.U32(at + 0xc) & 1) {
            c.keys.push_back({0.0f, r.F32(at + 0x10), 0.0f, 0.0f});
            return c;
        }
        u32 p = r.Ptr(at + 0x14);
        REQUIRE(p != 0, "a curve without its keys");
        const u32 format = r.U32(p + 8);
        const s32 count = r.Get<s32>(p + 0xc);
        p += 0x14;
        enum Quant {
            Hermite128,
            Hermite64,
            Hermite48,
            UnifiedHermite96,
            UnifiedHermite48,
            UnifiedHermite32,
            StepLinear64,
            StepLinear32,
        };
        REQUIRE(format >> 5 <= StepLinear32, "a curve of quantization {}", format >> 5);
        // Every key takes at least 4 bytes.
        REQUIRE(count > 0 && r.Fits(p, u64(count) * 4), "a curve of {} keys at {:#x}", count, p);
        const auto q = static_cast<Quant>(format >> 5);
        const bool linear = (format & 4) != 0;
        c.kind = linear ? Curve::Kind::Linear
                 : (q == StepLinear64 || q == StepLinear32) ? Curve::Kind::Step
                                                           : Curve::Kind::Hermite;
        float vs = 1.0f, vo = 0.0f, fs = 1.0f;
        if (q == UnifiedHermite48) {
            // Its frames are fixed point: no frame scale after the value's.
            vs = r.F32(p);
            vo = r.F32(p + 4);
            p += 8;
        } else if (q != Hermite128 && q != UnifiedHermite96 && q != StepLinear64) {
            vs = r.F32(p);
            vo = r.F32(p + 4);
            fs = r.F32(p + 8);
            p += 12;
        }
        const auto u24 = [&](u32 at24) {
            return u32{r.Get<u8>(at24)} | u32{r.Get<u8>(at24 + 1)} << 8 |
                   u32{r.Get<u8>(at24 + 2)} << 16;
        };
        const auto signed12 = [](u32 v) { return static_cast<float>(static_cast<s32>(v << 20) >> 20); };
        for (s32 k = 0; k < count; k++) {
            float f = 0, v = 0, in = 0, out = 0;
            switch (q) {
            case Hermite128:
                f = r.F32(p), v = r.F32(p + 4), in = r.F32(p + 8), out = r.F32(p + 12);
                p += 16;
                break;
            case Hermite64: {
                const u32 fv = r.U32(p);
                f = static_cast<float>(fv & 0xfff);
                v = static_cast<float>(fv >> 12 & 0xfffff);
                in = r.Get<s16>(p + 4) / 256.0f;
                out = r.Get<s16>(p + 6) / 256.0f;
                p += 8;
                break;
            }
            case Hermite48: {
                f = r.Get<u8>(p);
                v = r.Get<u16>(p + 1);
                const u32 sl = u24(p + 3);
                in = signed12(sl) / 32.0f;
                out = signed12(sl >> 12) / 32.0f;
                p += 6;
                break;
            }
            case UnifiedHermite96:
                f = r.F32(p), v = r.F32(p + 4), in = out = r.F32(p + 8);
                p += 12;
                break;
            case UnifiedHermite48:
                f = r.Get<u16>(p) / 32.0f;
                v = r.Get<u16>(p + 2);
                in = out = r.Get<s16>(p + 4) / 256.0f;
                p += 6;
                break;
            case UnifiedHermite32: {
                f = r.Get<u8>(p);
                const u32 vsl = u24(p + 1);
                v = static_cast<float>(vsl & 0xfff);
                in = out = signed12(vsl >> 12) / 32.0f;
                p += 4;
                break;
            }
            case StepLinear64:
                f = r.F32(p), v = r.F32(p + 4);
                p += 8;
                break;
            case StepLinear32: {
                const u32 fv = r.U32(p);
                f = static_cast<float>(fv & 0xfff);
                v = static_cast<float>(fv >> 12 & 0xfffff);
                p += 4;
                break;
            }
            }
            const float frame = f * fs, value = v * vs + vo;
            REQUIRE(std::isfinite(frame) && std::isfinite(value) &&
                        (c.keys.empty() || frame >= c.keys.back()[0]),
                    "a curve's key {} at {:#x}: frame {}, value {}", k, p, frame, value);
            c.keys.push_back({frame, value, in, out});
        }
        return c;
    }

    /// One slot of an animated value: its constant inline, or a pointer to its curve.
    std::optional<Curve> ReadSlot(u32 at, bool constant, bool exists) {
        if (!exists) {
            return std::nullopt;
        }
        if (constant) {
            Curve c;
            c.keys.push_back({0.0f, r.F32(at), 0.0f, 0.0f});
            return c;
        }
        const u32 group = r.Ptr(at);
        REQUIRE(group != 0, "an animated value without its curve");
        return ReadCurve(group);
    }

    std::optional<BakedTrack> ReadBaked(u32 at, bool rotation) {
        if (at == 0) {
            return std::nullopt;
        }
        BakedTrack t;
        t.start = r.F32(at);
        const float end = r.F32(at + 4);
        const bool constant = r.U32(at + 0xc) != 0;
        const float frames = end - t.start;
        REQUIRE(constant || (frames >= 1.0f && frames <= MaxFrames), "a baked track of {} frames",
                frames);
        const u32 count = constant ? 1 : static_cast<u32>(frames);
        // Each frame is followed by a word: 16 bytes a scale or translation, 20 a rotation.
        const u32 stride = rotation ? 20 : 16;
        REQUIRE(r.Fits(u64{at} + 0x10, u64{count} * stride), "a baked track of {} frames at {:#x}",
                count, at);
        for (u32 i = 0; i < count; i++) {
            const u32 f = at + 0x10 + stride * i;
            t.frames.push_back(rotation ? r.Vec4(f) : Common::MakeVec(r.Vec3(f), 0.0f));
        }
        return t;
    }

    Animation ReadAnimation(u32 at, bool material) {
        Animation a;
        a.frames = r.F32(at + 0x14);
        REQUIRE(a.frames >= 0.0f && a.frames <= MaxFrames, "an animation of {} frames", a.frames);
        static const char* const color_names[] = {
            "Emission",  "Ambient",   "Diffuse",   "Specular0", "Specular1", "Constant0",
            "Constant1", "Constant2", "Constant3", "Constant4", "Constant5"};
        for (const auto& [name, e] : r.DictField(at + 0x18)) {
            const u32 flags = r.U32(e);
            const bool rev5 = revision <= 0x05000000;
            const u32 type = r.U32(e + (rev5 ? 0x10 : 0x8));
            const u32 content = e + (rev5 ? 0x14 : 0xc);
            // GfxPrimitiveType: 0 float, 2 boolean, 3 vector 2D, 4 vector 3D, 5 transform,
            // 6 RGBA, 7 texture, 8 quaternion transform.
            const auto vector = [&](u32 n) {
                std::vector<std::optional<Curve>> curves;
                for (u32 k = 0; k < n; k++) {
                    curves.push_back(ReadSlot(content + 4 * k, (flags >> k & 1) != 0,
                                              (flags >> (n + k) & 1) == 0));
                }
                return curves;
            };
            if (!material) {
                BoneAnimation b;
                if (type == 5) {
                    // Scale x-z, rotation x-z (and an unused w), translation x-z.
                    constexpr u32 slots[9] = {0, 1, 2, 3, 4, 5, 7, 8, 9};
                    for (u32 k = 0; k < 9; k++) {
                        const u32 s = slots[k];
                        b.curves[k] = ReadSlot(content + 4 * s, (flags >> (6 + s) & 1) != 0,
                                               (flags >> (16 + s) & 1) == 0);
                    }
                } else if (type == 8) {
                    // Rotation, translation, scale; each part's own bits (constant 1/2/4 for
                    // translation/rotation/scale, inexistent 8/16/32).
                    if (!(flags & 16)) {
                        b.rotation = ReadBaked(r.Ptr(content), true);
                    }
                    if (!(flags & 8)) {
                        b.translation = ReadBaked(r.Ptr(content + 4), false);
                    }
                    if (!(flags & 32)) {
                        b.scale = ReadBaked(r.Ptr(content + 8), false);
                    }
                } else {
                    REQUIRE(type == 2, "bone {} animated as type {}", name, type);
                    continue;
                }
                b.frames = a.frames;
                a.bones.emplace(name, std::move(b));
                continue;
            }
            const std::optional<ElementName> element = ParseElementName(name);
            REQUIRE(element.has_value(), "material animation element {}", name);
            if (element->group == ElementName::Group::Mapper) {
                continue; // a texture pattern: not read yet
            }
            MaterialAnimation ma;
            ma.material = element->material;
            const std::string& field = element->field;
            if (element->group == ElementName::Group::Coordinator) {
                ma.index = element->index;
                REQUIRE(ma.index < 3, "texture coordinate {} animated", ma.index);
                if (field == "Scale") {
                    ma.target = MaterialAnimation::Target::CoordScale;
                    REQUIRE(type == 3, "{} animated as type {}", name, type);
                    ma.curves = vector(2);
                } else if (field == "Rotate") {
                    ma.target = MaterialAnimation::Target::CoordRotate;
                    REQUIRE(type == 0, "{} animated as type {}", name, type);
                    ma.curves = vector(1);
                } else {
                    REQUIRE(field == "Translate", "texture coordinate field {}", field);
                    ma.target = MaterialAnimation::Target::CoordTranslate;
                    REQUIRE(type == 3, "{} animated as type {}", name, type);
                    ma.curves = vector(2);
                }
            } else {
                const auto it = std::find(std::begin(color_names), std::end(color_names), field);
                REQUIRE(it != std::end(color_names), "material colour {}", field);
                ma.target = MaterialAnimation::Target::Color;
                ma.index = static_cast<u32>(it - std::begin(color_names));
                REQUIRE(type == 6, "{} animated as type {}", name, type);
                ma.curves = vector(4);
            }
            ma.frames = a.frames;
            a.materials.push_back(std::move(ma));
        }
        return a;
    }

    Reader r;
    u32 revision{};
};

#undef REQUIRE

} // Anonymous namespace

float Curve::Evaluate(float frame) const {
    ASSERT(!keys.empty());
    if (keys.size() == 1 || frame <= keys.front()[0]) {
        return keys.front()[1];
    }
    if (frame >= keys.back()[0]) {
        return keys.back()[1];
    }
    const auto next = std::upper_bound(keys.begin(), keys.end(), frame,
                                       [](float f, const auto& k) { return f < k[0]; });
    const auto& k0 = *(next - 1);
    const auto& k1 = *next;
    if (kind == Kind::Step) {
        return k0[1];
    }
    const float d = k1[0] - k0[0];
    const float w = (frame - k0[0]) / d;
    if (kind == Kind::Linear) {
        return k0[1] + (k1[1] - k0[1]) * w;
    }
    // A cubic Hermite with slopes in value per frame.
    const float v0 = k0[1], v1 = k1[1], o0 = k0[3], i1 = k1[2];
    return v0 + (v0 - v1) * (2 * w - 3) * w * w + (w * d) * (w - 1) * (o0 * (w - 1) + i1 * w);
}

std::optional<Scene> ReadBanner(std::span<const u8> banner,
                                std::span<const BannerLanguage> languages, std::string& error) {
    if (banner.size() < 0x88 || std::memcmp(banner.data(), "CBMD", 4) != 0) {
        error = "no CBMD";
        return std::nullopt;
    }
    // The scenes' offsets: the common one, then EUR English, French, German, Italian, Spanish,
    // Dutch, Portuguese, Russian, JPN Japanese, USA English, French, Spanish, Portuguese.
    std::array<u32, 31> offsets;
    std::memcpy(offsets.data(), banner.data() + 8, sizeof(offsets));
    u32 jingle;
    std::memcpy(&jingle, banner.data() + 0x84, 4);
    // A scene runs to the next one or the jingle.
    const auto parse = [&](u32 index, Scene& scene, bool language) {
        const u32 start = offsets[index];
        u32 end = jingle != 0 ? jingle : static_cast<u32>(banner.size());
        for (const u32 other : offsets) {
            if (other > start && other < end) {
                end = other;
            }
        }
        if (start == 0 || start >= end || end > banner.size()) {
            error = fmt::format("the CBMD's scene {} at {:#x}-{:#x} of {:#x}", index, start, end,
                                banner.size());
            return false;
        }
        const std::optional<std::vector<u8>> data =
            Lz11(banner.subspan(start, end - start), error);
        if (!data) {
            return false;
        }
        Parser parser{*data};
        if (!parser.Parse(scene, language)) {
            error = fmt::format("scene {}: {}", index, parser.Error());
            return false;
        }
        return true;
    };
    Scene scene;
    if (!parse(0, scene, false)) {
        return std::nullopt;
    }
    for (const BannerLanguage language : languages) {
        if (const u32 index = static_cast<u32>(language); offsets[index] != 0) {
            if (!parse(index, scene, true)) {
                return std::nullopt;
            }
            break;
        }
    }
    return scene;
}

std::span<const u8> BannerJingle(std::span<const u8> banner) {
    if (banner.size() < 0x88 || std::memcmp(banner.data(), "CBMD", 4) != 0) {
        return {};
    }
    u32 jingle;
    std::memcpy(&jingle, banner.data() + 0x84, 4);
    if (jingle == 0 || jingle >= banner.size()) {
        return {};
    }
    return banner.subspan(jingle);
}

} // namespace VitaFrontend::Banner
