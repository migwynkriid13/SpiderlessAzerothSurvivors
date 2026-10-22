#include <windows.h>

#include "embedded_assets.hpp"

#include <array>
#include <atomic>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

namespace spiderless {
namespace {
struct Asset {
    std::string_view path;
    WORD resource;
    std::span<const std::uint8_t> bytes;
};

constinit std::array<Asset, 6> g_assets{{
    {"Creature\\Crab\\Crab.m2", 100, {}},
    {"Creature\\Crab\\CrabSkinBronze.blp", 101, {}},
    {"Creature\\Crab\\CrabSkinIvory.blp", 102, {}},
    {"Creature\\Crab\\CrabSkinSaphire.blp", 103, {}},
    {"Creature\\Crab\\CrabSkinVermillian.blp", 104, {}},
    {"Creature\\Crab\\CrabReflect.blp", 105, {}},
}};
constinit std::atomic<bool> g_ready{false};
static_assert(std::atomic<bool>::is_always_lock_free);

bool SamePath(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        auto left = static_cast<unsigned char>(a[i]);
        auto right = static_cast<unsigned char>(b[i]);
        if (left == '/') left = '\\';
        if (right == '/') right = '\\';
        if (left >= 'A' && left <= 'Z') left += 'a' - 'A';
        if (right >= 'A' && right <= 'Z') right += 'a' - 'A';
        if (left != right) return false;
    }
    return true;
}

std::span<const std::uint8_t> FindLoaded(std::string_view path) noexcept {
    for (const auto& asset : g_assets) {
        if (SamePath(path, asset.path)) return asset.bytes;
    }
    return {};
}

std::uint32_t Word(const std::uint8_t* at) noexcept {
    std::uint32_t value;
    std::memcpy(&value, at, 4);
    return value;
}

void ValidateModel(std::span<const std::uint8_t> model) {
    if (model.size() < 0x64 || std::memcmp(model.data(), "MD20", 4) != 0 ||
        (Word(model.data() + 4) != 256 && Word(model.data() + 4) != 257)) {
        throw std::runtime_error("Embedded crab model has an incompatible format");
    }
    const auto count = Word(model.data() + 0x5c);
    const auto offset = Word(model.data() + 0x60);
    if (count > 64 || std::uint64_t(offset) + std::uint64_t(count) * 16 > model.size()) {
        throw std::runtime_error("Embedded crab model texture table is invalid");
    }
    bool monsterSkin = false;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto* texture = model.data() + offset + i * 16;
        const auto type = Word(texture);
        if (type == 11) monsterSkin = true;
        if (type != 0) continue;
        const auto length = Word(texture + 8);
        const auto nameAt = Word(texture + 12);
        if (!length || std::uint64_t(nameAt) + length > model.size()) {
            throw std::runtime_error("Embedded crab texture reference is invalid");
        }
        const auto* name = reinterpret_cast<const char*>(model.data() + nameAt);
        const auto* end = static_cast<const char*>(std::memchr(name, 0, length));
        if (!end || FindLoaded({name, static_cast<std::size_t>(end - name)}).empty()) {
            throw std::runtime_error("Embedded crab model has an unbundled texture dependency");
        }
    }
    if (!monsterSkin) throw std::runtime_error("Embedded crab model cannot use the randomized skins");
}
}

void InitializeEmbeddedAssets() {
    if (g_ready.load(std::memory_order_acquire)) return;
    HMODULE self;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&g_assets), &self)) {
        throw std::runtime_error("Cannot locate the embedded crab resources");
    }
    std::uint64_t total = 0;
    for (auto& asset : g_assets) {
        const auto resource = FindResourceW(self, MAKEINTRESOURCEW(asset.resource), RT_RCDATA);
        if (!resource) throw std::runtime_error("A required crab resource is missing from this DLL");
        const auto size = SizeofResource(self, resource);
        const auto loaded = LoadResource(self, resource);
        const auto* bytes = loaded ? static_cast<const std::uint8_t*>(LockResource(loaded)) : nullptr;
        total += size;
        if (!bytes || size < 8 || total > 8ULL * 1024 * 1024) {
            throw std::runtime_error("An embedded crab resource is empty or too large");
        }
        asset.bytes = {bytes, size};
        if (asset.resource != 100 && std::memcmp(bytes, "BLP1", 4) != 0 &&
            std::memcmp(bytes, "BLP2", 4) != 0) {
            throw std::runtime_error("An embedded crab skin is not a BLP texture");
        }
    }
    ValidateModel(g_assets[0].bytes);
    g_ready.store(true, std::memory_order_release);
}

std::span<const std::uint8_t> FindEmbeddedAsset(const char* path, std::size_t size) noexcept {
    if (!path || !g_ready.load(std::memory_order_acquire)) return {};
    return FindLoaded({path, size});
}

} // namespace spiderless
