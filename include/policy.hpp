#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace spiderless {
// A source display chooses one appearance for the whole process. Repeated DBC
// reads must produce the same mapping, including UI and sound catalog readers.
// Return sorted source display IDs so live appearance updates can limit size
// changes to the creatures replaced by this catalog.
std::vector<std::uint32_t> RemapDisplayInfo(std::span<const std::uint8_t> models,
                                          std::span<std::uint8_t> displays,
                                          std::uint64_t seed);
}
