#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace spiderless {

// Loads and validates the immutable DLL resources before installing hooks.
void InitializeEmbeddedAssets();

// Exact, ASCII case-insensitive archive path; slash directions are equivalent.
// An empty span means this mod does not provide that file.
std::span<const std::uint8_t> FindEmbeddedAsset(const char* path, std::size_t size) noexcept;

} // namespace spiderless
