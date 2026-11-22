// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <cryptopp/sha.h>
#include <fmt/format.h>
#include <lodepng.h>
#include <psp2/promoterutil.h>
#include <psp2/sysmodule.h>

#include "citra_vita/livearea.h"
#include "common/file_util.h"
#include "common/logging/log.h"

namespace VitaFrontend::LiveArea {

namespace {

/// Where a bubble is put together; the promoter moves it to ux0:app/ID.
constexpr char StagingDirectory[] = "ux0:data/azahar/bubble";
/// The file in the bubble its eboot reads the title's path from (bubble/main.c).
constexpr char TitleFile[] = "title.txt";
/// What the user is told when a bubble cannot be made from Azahar's own files.
constexpr char ReinstallAdvice[] = "Could not pin this title. Install Azahar's vpk again with "
                                   "VitaShell, then try again.";

/// A title's bubble ID: "AZB" and six hex digits of its path's hash (FNV-1a), nine characters as
/// the system wants a title ID.
std::string BubbleId(const std::string& path) {
    u64 hash = 0xcbf29ce484222325ull;
    for (const unsigned char c : path) {
        hash = (hash ^ c) * 0x100000001b3ull;
    }
    return fmt::format("AZB{:06X}", static_cast<u32>(hash ^ (hash >> 32)) & 0xffffffu);
}

/// A param.sfo's fields, rewritten in place: each keeps the room its file gave it.
class Sfo {
public:
    explicit Sfo(std::string data) : bytes{std::move(data)} {}

    bool SetString(std::string_view key, const std::string& value) {
        const std::optional<Field> field = Find(key);
        if (!field || field->format != 0x0204 || field->room == 0) {
            return false;
        }
        // Shortened to the room, a whole character at a time: its continuation bytes, then its
        // first (a first byte left behind would end the field in half a character).
        std::string text = value;
        while (text.size() >= field->room) {
            while (!text.empty() && (static_cast<u8>(text.back()) & 0xc0) == 0x80) {
                text.pop_back();
            }
            if (!text.empty()) {
                text.pop_back();
            }
        }
        std::fill_n(bytes.begin() + field->data, field->room, '\0');
        std::copy(text.begin(), text.end(), bytes.begin() + field->data);
        Put32(field->entry + 4, static_cast<u32>(text.size() + 1));
        return true;
    }

    bool SetInteger(std::string_view key, u32 value) {
        const std::optional<Field> field = Find(key);
        if (!field || field->format != 0x0404) {
            return false;
        }
        Put32(field->data, value);
        return true;
    }

    const std::string& Bytes() const {
        return bytes;
    }

private:
    struct Field {
        std::size_t entry; ///< its index entry
        u16 format;        ///< 0x0204 a UTF-8 string, 0x0404 a 32-bit integer
        u32 room;
        std::size_t data;
    };

    u32 Get32(std::size_t at) const {
        u32 value = 0;
        std::memcpy(&value, bytes.data() + at, 4);
        return value;
    }
    void Put32(std::size_t at, u32 value) {
        std::memcpy(bytes.data() + at, &value, 4);
    }

    std::optional<Field> Find(std::string_view key) const {
        // "\0PSF", the version, the keys' and the data's offsets, the number of fields; then a
        // 16-byte index entry a field.
        if (bytes.size() < 20 || Get32(0) != 0x46535000) {
            return std::nullopt;
        }
        const u32 keys = Get32(8), data = Get32(12), count = Get32(16);
        for (u32 i = 0; i < count; ++i) {
            const std::size_t entry = 20 + std::size_t{i} * 16;
            if (entry + 16 > bytes.size()) {
                return std::nullopt;
            }
            u16 key_offset = 0, format = 0;
            std::memcpy(&key_offset, bytes.data() + entry, 2);
            std::memcpy(&format, bytes.data() + entry + 2, 2);
            const std::size_t name = std::size_t{keys} + key_offset;
            if (name >= bytes.size() || std::string_view{bytes.data() + name} != key) {
                continue;
            }
            const Field field{entry, format, Get32(entry + 8), std::size_t{data} + Get32(entry + 12)};
            if (field.data + field.room > bytes.size()) {
                return std::nullopt;
            }
            return field;
        }
        return std::nullopt;
    }

    std::string bytes;
};

u32 Be32(const std::string& bytes, std::size_t at) {
    return static_cast<u32>(static_cast<u8>(bytes[at])) << 24 |
           static_cast<u32>(static_cast<u8>(bytes[at + 1])) << 16 |
           static_cast<u32>(static_cast<u8>(bytes[at + 2])) << 8 |
           static_cast<u32>(static_cast<u8>(bytes[at + 3]));
}

/// The hash the promoter checks a homebrew package's head.bin with: the data's SHA-1, its bytes
/// laid out as a 64-byte key, and that key's SHA-1, of which the first 16 bytes.
std::array<u8, 16> PackageHash(const u8* data, std::size_t size) {
    std::array<u8, CryptoPP::SHA1::DIGESTSIZE> digest;
    CryptoPP::SHA1().CalculateDigest(digest.data(), data, size);
    std::array<u8, 64> key{};
    std::copy_n(&digest[4], 8, &key[0]);
    std::copy_n(&digest[4], 8, &key[8]);
    std::copy_n(&digest[12], 4, &key[16]);
    key[20] = digest[16];
    key[21] = digest[1];
    key[22] = digest[2];
    key[23] = digest[3];
    std::copy_n(&key[16], 8, &key[24]);
    CryptoPP::SHA1().CalculateDigest(digest.data(), key.data(), key.size());
    std::array<u8, 16> hash;
    std::copy_n(digest.begin(), hash.size(), hash.begin());
    return hash;
}

/// Azahar's own head.bin made the bubble's: its content ID, and the three hashes over it again
/// (the header's, the package information's, the whole head's, each over the one before).
bool MakeHead(std::string& head, const std::string& id) {
    if (head.size() < 0x100) {
        return false;
    }
    const std::string content = fmt::format("EP9000-{}_00-0000000000000000", id);
    std::fill_n(head.begin() + 0x30, 0x30, '\0');
    std::copy(content.begin(), content.end(), head.begin() + 0x30);
    const auto hash = [&](std::size_t from, std::size_t size, std::size_t at) {
        if (from + size > head.size() || at + 16 > head.size()) {
            return false;
        }
        const auto value = PackageHash(reinterpret_cast<const u8*>(head.data()) + from, size);
        std::copy(value.begin(), value.end(), head.begin() + at);
        return true;
    };
    const u32 header = Be32(head, 0xd0);
    const u32 info = Be32(head, 0x08), info_size = Be32(head, 0x10), info_at = Be32(head, 0xd4);
    const u32 whole = Be32(head, 0xe8);
    return info_size >= 64 && hash(0, header, header) && hash(info, info_size - 64, info_at) &&
           hash(0, whole, whole);
}

/// An image of A8B8G8R8 texels.
struct Image {
    u32 width, height;
    std::vector<u32> texels;
};

constexpr u32 Rgb(u8 r, u8 g, u8 b) {
    return 0xff000000u | u32{b} << 16 | u32{g} << 8 | r;
}

// The menus' light ground and its pattern, white, and the icons' blue (vita_ui.cpp's palette).
constexpr u32 Ground = Rgb(229, 230, 232);
constexpr u32 Pattern = Rgb(239, 240, 242);
constexpr u32 White = Rgb(255, 255, 255);
constexpr u32 Blue = Rgb(72, 145, 219);

Image Filled(u32 width, u32 height, u32 colour) {
    return {width, height, std::vector<u32>(std::size_t{width} * height, colour)};
}

/// The menus' diagonal bands over the image.
void Stripes(Image& image) {
    for (u32 y = 0; y < image.height; ++y) {
        for (u32 x = 0; x < image.width; ++x) {
            if ((x + y) % 36 < 14) {
                image.texels[std::size_t{y} * image.width + x] = Pattern;
            }
        }
    }
}

/// A square of `size` at (x, y): the 48x48 icon scaled (bilinear), or plain blue without one.
void DrawIcon(Image& image, const std::vector<u32>& icon, u32 x, u32 y, u32 size) {
    constexpr u32 Side = 48;
    for (u32 j = 0; j < size; ++j) {
        for (u32 i = 0; i < size; ++i) {
            u32 colour = Blue;
            if (icon.size() == Side * Side) {
                const float u = std::clamp((i + 0.5f) * Side / size - 0.5f, 0.0f, Side - 1.0f);
                const float v = std::clamp((j + 0.5f) * Side / size - 0.5f, 0.0f, Side - 1.0f);
                const u32 u0 = static_cast<u32>(u), v0 = static_cast<u32>(v);
                const u32 u1 = std::min(u0 + 1, Side - 1), v1 = std::min(v0 + 1, Side - 1);
                const float fu = u - u0, fv = v - v0;
                colour = 0xff000000u;
                for (int channel = 0; channel < 3; ++channel) {
                    const auto at = [&](u32 a, u32 b) {
                        return static_cast<float>(icon[b * Side + a] >> (channel * 8) & 0xff);
                    };
                    const float top = at(u0, v0) + (at(u1, v0) - at(u0, v0)) * fu;
                    const float bottom = at(u0, v1) + (at(u1, v1) - at(u0, v1)) * fu;
                    colour |= static_cast<u32>(top + (bottom - top) * fv + 0.5f) << (channel * 8);
                }
            }
            image.texels[std::size_t{y + j} * image.width + x + i] = colour;
        }
    }
}

/// At most 256 colours for the images together, as the LiveArea's 8-bit PNGs carry (Azahar's
/// own are): a median cut of their colours, weighted by how many pixels have each.
std::vector<u32> MakePalette(const std::vector<const Image*>& images) {
    std::unordered_map<u32, u32> counts;
    for (const Image* image : images) {
        for (const u32 texel : image->texels) {
            ++counts[texel];
        }
    }
    struct Colour {
        u32 value, count;
        u8 Channel(int c) const {
            return static_cast<u8>(value >> (c * 8));
        }
    };
    std::vector<Colour> colours;
    for (const auto& [value, count] : counts) {
        colours.push_back({value, count});
    }
    struct Box {
        std::size_t begin, end;
    };
    std::vector<Box> boxes{{0, colours.size()}};
    boxes.reserve(256);
    while (boxes.size() < 256) {
        // The box whose widest channel is widest, split at its pixels' median on it.
        int best = -1, channel = 0, widest = 0;
        for (std::size_t b = 0; b < boxes.size(); ++b) {
            if (boxes[b].end - boxes[b].begin < 2) {
                continue;
            }
            for (int c = 0; c < 3; ++c) {
                const auto [low, high] = std::minmax_element(
                    colours.begin() + boxes[b].begin, colours.begin() + boxes[b].end,
                    [c](const Colour& x, const Colour& y) { return x.Channel(c) < y.Channel(c); });
                const int range = high->Channel(c) - low->Channel(c);
                if (range > widest) {
                    best = static_cast<int>(b);
                    channel = c;
                    widest = range;
                }
            }
        }
        if (best < 0) {
            break;
        }
        const Box box = boxes[static_cast<std::size_t>(best)];
        std::sort(colours.begin() + box.begin, colours.begin() + box.end,
                  [channel](const Colour& x, const Colour& y) {
                      return x.Channel(channel) < y.Channel(channel);
                  });
        u64 total = 0;
        for (std::size_t i = box.begin; i < box.end; ++i) {
            total += colours[i].count;
        }
        std::size_t split = box.begin + 1;
        for (u64 seen = colours[box.begin].count; split < box.end - 1 && seen * 2 < total;
             ++split) {
            seen += colours[split].count;
        }
        boxes[static_cast<std::size_t>(best)].end = split;
        boxes.push_back({split, box.end});
    }
    std::vector<u32> palette;
    for (const Box& box : boxes) {
        std::array<u64, 3> sum{};
        u64 weight = 0;
        for (std::size_t i = box.begin; i < box.end; ++i) {
            for (int c = 0; c < 3; ++c) {
                sum[c] += u64{colours[i].Channel(c)} * colours[i].count;
            }
            weight += colours[i].count;
        }
        u32 value = 0xff000000u;
        for (int c = 0; c < 3; ++c) {
            value |= static_cast<u32>((sum[c] + weight / 2) / weight) << (c * 8);
        }
        palette.push_back(value);
    }
    return palette;
}

/// The image as an 8-bit palette PNG, each pixel the palette's nearest colour.
std::vector<u8> EncodePng(const Image& image, const std::vector<u32>& palette) {
    std::unordered_map<u32, u8> nearest;
    std::vector<u8> indices(image.texels.size());
    for (std::size_t p = 0; p < image.texels.size(); ++p) {
        const u32 texel = image.texels[p];
        auto found = nearest.find(texel);
        if (found == nearest.end()) {
            u8 best = 0;
            int best_distance = INT32_MAX;
            for (std::size_t i = 0; i < palette.size(); ++i) {
                int distance = 0;
                for (int c = 0; c < 3; ++c) {
                    const int d = static_cast<int>(texel >> (c * 8) & 0xff) -
                                  static_cast<int>(palette[i] >> (c * 8) & 0xff);
                    distance += d * d;
                }
                if (distance < best_distance) {
                    best = static_cast<u8>(i);
                    best_distance = distance;
                }
            }
            found = nearest.emplace(texel, best).first;
        }
        indices[p] = found->second;
    }
    lodepng::State state;
    for (LodePNGColorMode* mode : {&state.info_png.color, &state.info_raw}) {
        mode->colortype = LCT_PALETTE;
        mode->bitdepth = 8;
        for (const u32 colour : palette) {
            lodepng_palette_add(mode, colour & 0xff, colour >> 8 & 0xff, colour >> 16 & 0xff, 0xff);
        }
    }
    state.encoder.auto_convert = 0;
    std::vector<u8> png;
    if (const unsigned error = lodepng::encode(png, indices, image.width, image.height, state)) {
        LOG_ERROR(Frontend, "bubble: PNG {}x{}: {}", image.width, image.height,
                  lodepng_error_text(error));
        return {};
    }
    return png;
}

bool WriteBytes(const std::string& path, const std::vector<u8>& bytes) {
    return !bytes.empty() &&
           FileUtil::WriteStringToFile(
               false, path,
               std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()}) ==
               bytes.size();
}

/// The promoter loaded for `call` (as VitaShell loads it): its result, or the first failure's.
int WithPromoter(const std::function<int()>& call) {
    u32 arguments[] = {0x180000, 0xffffffffu, 0xffffffffu, 1, 0xffffffffu, 0xffffffffu};
    int module_result = -1;
    const SceSysmoduleOpt options{16, &module_result, {-1, -1}};
    int result = sceSysmoduleLoadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF,
                                                       sizeof(arguments), arguments, &options);
    if (result < 0 || module_result < 0) {
        return result < 0 ? result : module_result;
    }
    result = sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
    if (result >= 0) {
        result = scePromoterUtilityInit();
        if (result >= 0) {
            result = call();
            int operation = 0;
            if (result >= 0 && scePromoterUtilityGetResult(&operation) >= 0 && operation < 0) {
                result = operation;
            }
            scePromoterUtilityExit();
        }
        sceSysmoduleUnloadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
    }
    const SceSysmoduleOpt unload{};
    sceSysmoduleUnloadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, 0, nullptr, &unload);
    return result;
}

} // anonymous namespace

bool IsPinned(const std::string& path) {
    std::string pinned;
    return FileUtil::ReadFileToString(
               true, fmt::format("ux0:app/{}/{}", BubbleId(path), TitleFile), pinned) > 0 &&
           pinned == path;
}

std::string Pin(const std::string& path, const std::string& name, const std::vector<u32>& icon) {
    const std::string id = BubbleId(path);
    const std::string dir = StagingDirectory;
    // What a pin that stopped half way left.
    if (FileUtil::Exists(dir)) {
        FileUtil::DeleteDirRecursively(dir);
    }
    const auto fail = [&](std::string_view step) {
        LOG_ERROR(Frontend, "bubble {} for {}: {} failed", id, path, step);
        FileUtil::DeleteDirRecursively(dir);
        return std::string{ReinstallAdvice};
    };
    if (!FileUtil::CreateFullPath(dir + "/sce_sys/livearea/contents/") ||
        !FileUtil::CreateFullPath(dir + "/sce_sys/package/")) {
        return fail("its directories");
    }
    if (!FileUtil::Copy("app0:bubble/eboot.bin", dir + "/eboot.bin")) {
        return fail("the launcher's copy");
    }
    if (!FileUtil::Copy("app0:sce_sys/livearea/contents/template.xml",
                        dir + "/sce_sys/livearea/contents/template.xml")) {
        return fail("the LiveArea template's copy");
    }
    if (FileUtil::WriteStringToFile(true, dir + "/" + TitleFile, path) != path.size()) {
        return fail("title.txt");
    }

    // Azahar's own param.sfo and head.bin, made the bubble's.
    std::string sfo_bytes;
    if (FileUtil::ReadFileToString(false, "app0:sce_sys/param.sfo", sfo_bytes) == 0) {
        return fail("reading Azahar's param.sfo");
    }
    Sfo sfo{std::move(sfo_bytes)};
    // A bubble needs none of the extended memory Azahar asks for (ATTRIBUTE2).
    if (!sfo.SetString("TITLE", name) || !sfo.SetString("STITLE", name) ||
        !sfo.SetString("TITLE_ID", id) || !sfo.SetInteger("ATTRIBUTE2", 0)) {
        return fail("param.sfo's fields");
    }
    if (FileUtil::WriteStringToFile(false, dir + "/sce_sys/param.sfo", sfo.Bytes()) !=
        sfo.Bytes().size()) {
        return fail("writing param.sfo");
    }
    std::string head;
    if (FileUtil::ReadFileToString(false, "ux0:app/" AZAHAR_VITA_TITLEID "/sce_sys/package/head.bin",
                                   head) == 0 ||
        !MakeHead(head, id)) {
        return fail("Azahar's head.bin");
    }
    if (FileUtil::WriteStringToFile(false, dir + "/sce_sys/package/head.bin", head) !=
        head.size()) {
        return fail("writing head.bin");
    }

    // The bubble's icon, the LiveArea's background (the menus' bands and the icon on a white
    // tile, under the gate the page puts in its middle) and its gate (the icon on white).
    Image bubble = Filled(128, 128, White);
    DrawIcon(bubble, icon, 0, 0, 128);
    Image background = Filled(840, 500, Ground);
    Stripes(background);
    for (u32 y = 318; y < 318 + 168; ++y) {
        std::fill_n(background.texels.begin() + std::size_t{y} * 840 + 336, 168, White);
    }
    DrawIcon(background, icon, 344, 326, 152);
    Image gate = Filled(280, 158, White);
    DrawIcon(gate, icon, 87, 26, 106);
    const std::vector<u32> palette = MakePalette({&bubble, &background, &gate});
    if (!WriteBytes(dir + "/sce_sys/icon0.png", EncodePng(bubble, palette)) ||
        !WriteBytes(dir + "/sce_sys/livearea/contents/bg.png", EncodePng(background, palette)) ||
        !WriteBytes(dir + "/sce_sys/livearea/contents/startup.png", EncodePng(gate, palette))) {
        return fail("the images");
    }

    const int result =
        WithPromoter([&] { return scePromoterUtilityPromotePkgWithRif(dir.c_str(), 1); });
    if (result < 0) {
        LOG_ERROR(Frontend, "bubble {} for {}: promote 0x{:08x}", id, path,
                  static_cast<u32>(result));
        FileUtil::DeleteDirRecursively(dir);
        return "Could not pin this title. Restart the Vita, then try again.";
    }
    LOG_INFO(Frontend, "bubble {} pins {}", id, path);
    return {};
}

std::string Unpin(const std::string& path) {
    const std::string id = BubbleId(path);
    const int result = WithPromoter([&] { return scePromoterUtilityDeletePkg(id.c_str()); });
    if (result < 0) {
        LOG_ERROR(Frontend, "bubble {} for {}: delete 0x{:08x}", id, path,
                  static_cast<u32>(result));
        return "Could not unpin this title. Restart the Vita, then try again.";
    }
    LOG_INFO(Frontend, "bubble {} for {} removed", id, path);
    return {};
}

} // namespace VitaFrontend::LiveArea
