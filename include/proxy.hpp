#pragma once

#include <windows.h>

namespace spiderless::proxy {

// Called once by the entrypoint bootstrap, after the loader has finished.
// Keeps the real system module loaded for the lifetime of the process.
bool LoadSystemAvrt(HMODULE self) noexcept;

} // namespace spiderless::proxy
