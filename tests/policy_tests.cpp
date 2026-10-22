#include "policy.hpp"
#include "scale_policy.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;
using Display = std::array<std::uint32_t, 12>;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void Near(float actual, float expected, const char* message) {
    Require(std::abs(actual - expected) < 0.0001f, message);
}

std::uint32_t FloatWord(float value) {
    std::uint32_t word;
    std::memcpy(&word, &value, sizeof(word));
    return word;
}

void AppendWord(Bytes& bytes, std::uint32_t word) {
    const auto at = bytes.size();
    bytes.resize(at + sizeof(word));
    std::memcpy(bytes.data() + at, &word, sizeof(word));
}

template<std::size_t Fields>
Bytes Dbc(const std::vector<std::array<std::uint32_t, Fields>>& rows,
          const std::string& strings) {
    Bytes bytes{'W', 'D', 'B', 'C'};
    AppendWord(bytes, static_cast<std::uint32_t>(rows.size()));
    AppendWord(bytes, Fields);
    AppendWord(bytes, Fields * 4);
    AppendWord(bytes, static_cast<std::uint32_t>(strings.size()));
    for (const auto& row : rows) {
        for (const auto word : row) AppendWord(bytes, word);
    }
    bytes.insert(bytes.end(), strings.begin(), strings.end());
    return bytes;
}

struct Fixture {
    Bytes models;
    Bytes displays;
    std::vector<Display> originalRows;

    Fixture() {
        std::string modelStrings(1, '\0');
        const auto addModel = [&](const char* path) {
            const auto offset = static_cast<std::uint32_t>(modelStrings.size());
            modelStrings.append(path).push_back('\0');
            return offset;
        };
        std::vector<std::array<std::uint32_t, 16>> modelRows(4);
        modelRows[0][0] = 401;
        modelRows[0][2] = addModel("Creature/Spider/Spider.mdx");
        modelRows[1][0] = 402;
        modelRows[1][2] = addModel("CREATURE\\CRYPTFIEND\\CryptFiend.mdx");
        modelRows[2][0] = 800;
        modelRows[2][2] = addModel("Creature\\Crab\\Crab.mdx");
        modelRows[3][0] = 900;
        modelRows[3][2] = addModel("Creature\\Wolf\\Wolf.mdx");
        models = Dbc(modelRows, modelStrings);

        std::string displayStrings(1, '\0');
        for (const auto* skin : {"CrabSkinBronze", "CrabSkinIvory",
                                 "CrabSkinSaphire", "CrabSkinVermillian"}) {
            Display row{};
            row[0] = static_cast<std::uint32_t>(20000 + originalRows.size());
            row[1] = 800;
            row[4] = FloatWord(1.0f);
            row[6] = static_cast<std::uint32_t>(displayStrings.size());
            displayStrings.append(skin).push_back('\0');
            originalRows.push_back(row);
        }
        // These identities deliberately reproduce the role overlaps found in
        // the current roster. Model IDs above are arbitrary fixture values.
        for (const auto id : {16586u, 1091u, 366u, 15931u}) {
            Display row{id, id == 15931 ? 402u : 401u, 12, 25,
                        FloatWord(0.8f), 220, 0, 0, 0, 41, 42, 43};
            originalRows.push_back(row);
        }
        originalRows.push_back({777, 900, 12, 25, FloatWord(1.2f), 230});
        displays = Dbc(originalRows, displayStrings);
    }
};

void TestCatalogAndRoles() {
    Fixture fixture;
    const auto unchanged = fixture.displays;
    const auto ids = spiderless::RemapDisplayInfo(fixture.models, fixture.displays, 1234);
    Require(ids == std::vector<std::uint32_t>({366, 1091, 15931, 16586}),
            "Replacement IDs must derive from model paths and be sorted");
    for (std::size_t i = 0; i < fixture.originalRows.size(); ++i) {
        Display row;
        std::memcpy(row.data(), fixture.displays.data() + 20 + i * sizeof(row), sizeof(row));
        const auto replaced = std::binary_search(ids.begin(), ids.end(), row[0]);
        for (std::size_t field = 0; field < row.size(); ++field) {
            if (replaced && (field == 1 || field == 3 || field == 6 || field == 7 || field == 8)) continue;
            Require(row[field] == fixture.originalRows[i][field],
                    "DBC metadata including original scale must stay intact");
        }
        if (replaced) Require(row[1] == 800 && row[3] == 0 && row[7] == 0 && row[8] == 0,
                              "Crab model and every appearance slot must change together");
    }
    auto secondRead = unchanged;
    Require(spiderless::RemapDisplayInfo(fixture.models, secondRead, 1234) == ids &&
            secondRead == fixture.displays, "Repeated reads must preserve the same catalog and colors");

    using spiderless::CreatureRole;
    using spiderless::ReplacementScale;
    Near(ReplacementScale(ids, 1091, 0.6f, CreatureRole::Regular), 1.2f,
         "Black Widow Hatchling needs regular 2x size");
    Near(ReplacementScale(ids, 1091, 2.8f, CreatureRole::Boss), 11.2f,
         "Naraxis sharing display 1091 needs boss 4x size");
    Near(ReplacementScale(ids, 366, 1.7f, CreatureRole::Regular), 3.4f,
         "Ordinary Carrion Recluse needs regular 2x size");
    Near(ReplacementScale(ids, 366, 1.7f * 1.35f, CreatureRole::Guardian), 9.18f,
         "Guardian Carrion Recluse needs 4x its existing guardian scale");
    Near(ReplacementScale(ids, 16586, 1.7f, CreatureRole::Regular), 3.4f,
         "Guardian of Icecrown is an ordinary elite despite its name");
    Near(ReplacementScale(ids, 777, 1.2f, CreatureRole::Boss), 1.2f,
         "Unreplaced creatures must retain their scale even when bosses");
    Near(ReplacementScale(ids, 1091, 2.8f, CreatureRole::Boss), 11.2f,
         "Appearance restoration must multiply original scale once");
    Require(ReplacementScale(ids, 1091, 0, CreatureRole::Boss) == 0,
            "Zero scale must remain zero");
    Require(ReplacementScale(ids, 1091, -1, CreatureRole::Boss) == -1,
            "Negative scale must be preserved");
    const auto huge = std::numeric_limits<float>::max();
    Require(ReplacementScale(ids, 1091, huge, CreatureRole::Boss) == huge,
            "Scaling must not introduce an infinite value");
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    Require(std::isnan(ReplacementScale(ids, 1091, nan, CreatureRole::Boss)),
            "Invalid native scale must not be changed");
}

void TestDuplicateDisplayRejected() {
    Fixture fixture;
    // Replace the second spider row's ID with the first spider row's ID.
    std::memcpy(fixture.displays.data() + 20 + 5 * sizeof(Display),
                fixture.displays.data() + 20 + 4 * sizeof(Display), 4);
    bool rejected = false;
    try { spiderless::RemapDisplayInfo(fixture.models, fixture.displays, 1234); }
    catch (const std::runtime_error&) { rejected = true; }
    Require(rejected, "Ambiguous source display IDs must be rejected");
}
}

int main() {
    try {
        TestCatalogAndRoles();
        TestDuplicateDisplayRejected();
        std::cout << "Catalog and per-creature scaling regressions passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
