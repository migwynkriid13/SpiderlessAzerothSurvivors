#include <windows.h>
#include <avrt.h>

#include "proxy.hpp"

#include <atomic>
#include <cstddef>

namespace {

struct Functions {
    decltype(&AvSetMmThreadCharacteristicsW) characteristics{};
    decltype(&AvSetMmThreadPriority) priority{};
    decltype(&AvRevertMmThreadCharacteristics) revert{};
};

constinit Functions g_functions{};
constinit std::atomic<bool> g_ready{false};
static_assert(std::atomic<bool>::is_always_lock_free);

const Functions* ReadyFunctions() noexcept {
    if (g_ready.load(std::memory_order_acquire)) return &g_functions;
    // A different DLL could call an AVRT export from its own DllMain before the
    // EXE entrypoint. Never load a DLL or wait for bootstrap under that lock.
    SetLastError(ERROR_NOT_READY);
    return nullptr;
}

} // namespace

namespace spiderless::proxy {

bool LoadSystemAvrt(HMODULE self) noexcept {
    wchar_t systemPath[32768]{};
    const auto length = GetSystemDirectoryW(systemPath, ARRAYSIZE(systemPath));
    constexpr wchar_t suffix[] = L"\\avrt.dll";
    if (!length || length + ARRAYSIZE(suffix) > ARRAYSIZE(systemPath)) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return false;
    }
    for (std::size_t i = 0; i < ARRAYSIZE(suffix); ++i) systemPath[length + i] = suffix[i];
    const auto real = LoadLibraryExW(systemPath, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!real) return false;

    // A .local redirection can override even a requested absolute path. Do not
    // recurse into ourselves or forward to another app-local module.
    wchar_t loadedPath[32768]{};
    const auto loadedLength = GetModuleFileNameW(real, loadedPath, ARRAYSIZE(loadedPath));
    if (real == self || !loadedLength || loadedLength >= ARRAYSIZE(loadedPath) ||
        CompareStringOrdinal(systemPath, -1, loadedPath, -1, TRUE) != CSTR_EQUAL) {
        SetLastError(ERROR_BAD_PATHNAME);
        return false;
    }
    Functions functions{
        reinterpret_cast<decltype(Functions::characteristics)>(
            GetProcAddress(real, "AvSetMmThreadCharacteristicsW")),
        reinterpret_cast<decltype(Functions::priority)>(GetProcAddress(real, "AvSetMmThreadPriority")),
        reinterpret_cast<decltype(Functions::revert)>(
            GetProcAddress(real, "AvRevertMmThreadCharacteristics")),
    };
    if (!functions.characteristics || !functions.priority || !functions.revert) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return false;
    }
    g_functions = functions;
    g_ready.store(true, std::memory_order_release);
    return true;
}

} // namespace spiderless::proxy

// The .def file exposes these under the original Windows API names. Keeping
// distinct C++ identifiers avoids defining the SDK's dllimport declarations.
extern "C" HANDLE WINAPI SpiderlessAvSetMmThreadCharacteristicsW(
    LPCWSTR taskName, LPDWORD taskIndex) noexcept {
    const auto* functions = ReadyFunctions();
    return functions ? functions->characteristics(taskName, taskIndex) : nullptr;
}

extern "C" BOOL WINAPI SpiderlessAvSetMmThreadPriority(HANDLE handle, AVRT_PRIORITY priority) noexcept {
    const auto* functions = ReadyFunctions();
    return functions ? functions->priority(handle, priority) : FALSE;
}

extern "C" BOOL WINAPI SpiderlessAvRevertMmThreadCharacteristics(HANDLE handle) noexcept {
    const auto* functions = ReadyFunctions();
    return functions ? functions->revert(handle) : FALSE;
}
