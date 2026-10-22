#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace spiderless {
// A source display chooses one appearance for the whole process. Repeated DBC
// reads must produce the same mapping, including UI and sound catalog readers.
std::size_t RemapDisplayInfo(std::span<const std::uint8_t> models,
                           std::span<std::uint8_t> displays, std::uint64_t seed);
}
