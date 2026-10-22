#include "policy.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace spiderless {
namespace {
constexpr std::size_t kHeader = 20;
constexpr std::size_t kDisplayStride = 48;

std::uint32_t Word(const std::uint8_t* at) {
    std::uint32_t value;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

class Table {
public:
    Table(std::span<const std::uint8_t> bytes, std::uint32_t fields) : bytes_(bytes) {
        if (bytes.size() < kHeader || std::memcmp(bytes.data(), "WDBC", 4) != 0 ||
            Word(bytes.data() + 8) != fields || Word(bytes.data() + 12) != fields * 4) {
            throw std::runtime_error("Unsupported creature DBC layout");
        }
        count_ = Word(bytes.data() + 4);
        stride_ = fields * 4;
        stringsSize_ = Word(bytes.data() + 16);
        const auto at = kHeader + std::uint64_t(count_) * stride_;
        if (at + stringsSize_ != bytes.size() || count_ > 1000000) {
            throw std::runtime_error("Invalid creature DBC record/string bounds");
        }
        stringsAt_ = static_cast<std::size_t>(at);
    }

    std::uint32_t Count() const { return count_; }
    const std::uint8_t* Row(std::uint32_t i) const {
        return bytes_.data() + kHeader + std::size_t(i) * stride_;
    }
    std::string_view Text(std::uint32_t offset) const {
        if (offset >= stringsSize_) throw std::runtime_error("Invalid creature string offset");
        const auto* begin = reinterpret_cast<const char*>(bytes_.data() + stringsAt_ + offset);
        const auto* end = static_cast<const char*>(std::memchr(begin, 0, stringsSize_ - offset));
        if (!end) throw std::runtime_error("Unterminated creature string");
        return {begin, static_cast<std::size_t>(end - begin)};
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::uint32_t count_{}, stride_{}, stringsSize_{};
    std::size_t stringsAt_{};
};

std::string Normalize(std::string_view text) {
    std::string value(text);
    for (auto& ch : value) {
        if (ch == '/') ch = '\\';
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    }
    return value;
}

std::uint64_t Mix(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}
}

std::vector<std::uint32_t> RemapDisplayInfo(std::span<const std::uint8_t> models,
                                          std::span<std::uint8_t> displays,
                                          std::uint64_t seed) {
    const Table modelTable(models, 16);
    const Table displayTable(displays, 12);
    std::unordered_set<std::uint32_t> spiders;
    std::uint32_t crabModel = 0;
    for (std::uint32_t i = 0; i < modelTable.Count(); ++i) {
        const auto* row = modelTable.Row(i);
        const auto path = Normalize(modelTable.Text(Word(row + 8)));
        if (path.find("spider") != std::string::npos ||
            path.find("tarantula") != std::string::npos ||
            path.find("cryptfiend") != std::string::npos ||
            path.find("cryptlord") != std::string::npos) {
            spiders.insert(Word(row));
        }
        if (path == "creature\\crab\\crab.mdx" || path == "creature\\crab\\crab.m2") {
            if (crabModel != 0) throw std::runtime_error("Ambiguous Vanilla crab model");
            crabModel = Word(row);
        }
    }
    if (!crabModel || spiders.empty()) throw std::runtime_error("Creature model families changed");

    // Snapshot the four provisioned Vanilla colors before changing any row.
    constexpr std::array<std::string_view, 4> allowed{
        "crabskinbronze", "crabskinivory", "crabskinsaphire", "crabskinvermillian"
    };
    std::vector<std::array<std::uint8_t, kDisplayStride>> crabs;
    std::unordered_set<std::string> skins;
    for (std::uint32_t i = 0; i < displayTable.Count(); ++i) {
        const auto* row = displayTable.Row(i);
        if (Word(row + 4) != crabModel) continue;
        const auto skin = Normalize(displayTable.Text(Word(row + 24)));
        if (std::find(allowed.begin(), allowed.end(), skin) == allowed.end() ||
            !skins.insert(skin).second) continue;
        if (Word(row + 12) != 0 || !displayTable.Text(Word(row + 28)).empty() ||
            !displayTable.Text(Word(row + 32)).empty()) {
            throw std::runtime_error("Vanilla crab appearance schema changed");
        }
        std::array<std::uint8_t, kDisplayStride> copy;
        std::memcpy(copy.data(), row, kDisplayStride);
        crabs.push_back(copy);
    }
    if (crabs.size() != 4) throw std::runtime_error("Expected four provisioned Vanilla crab skins");

    std::vector<std::uint32_t> changed;
    for (std::uint32_t i = 0; i < displayTable.Count(); ++i) {
        auto* row = displays.data() + kHeader + std::size_t(i) * kDisplayStride;
        if (!spiders.contains(Word(row + 4))) continue;
        const auto& crab = crabs[Mix(seed ^ Word(row)) % crabs.size()];
        // Preserve ID, scale, alpha, sound and other metadata. The model and ALL
        // texture slots must change together; string offsets are in this DBC.
        for (std::size_t field : {1u, 3u, 6u, 7u, 8u}) {
            std::memcpy(row + field * 4, crab.data() + field * 4, 4);
        }
        changed.push_back(Word(row));
    }
    if (changed.empty()) throw std::runtime_error("No spider appearances were replaced");
    std::sort(changed.begin(), changed.end());
    if (std::adjacent_find(changed.begin(), changed.end()) != changed.end()) {
        throw std::runtime_error("Duplicate replaced creature display ID");
    }
    return changed;
}
}
