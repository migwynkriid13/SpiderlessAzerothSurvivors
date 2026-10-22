#include <windows.h>
#include <bcrypt.h>

#include "policy.hpp"
#include "target_profile.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace spiderless {
namespace {
using ReadFunction = NativeReadResult* (__fastcall *)(NativeReadResult*, void*, const char*, std::size_t);
constexpr auto kMaxRead = 32ULL * 1024 * 1024;
constexpr auto kProbeBudget = 8ULL * 1024 * 1024;
constexpr std::string_view kModelDb = "DBFilesClient\\CreatureModelData.dbc";
constexpr std::string_view kDisplayDb = "DBFilesClient\\CreatureDisplayInfo.dbc";
constexpr std::array<std::string_view, 5> kAssets{
    "Creature\\Crab\\Crab.m2",
    "Creature\\Crab\\CrabSkinBronze.blp",
    "Creature\\Crab\\CrabSkinIvory.blp",
    "Creature\\Crab\\CrabSkinSaphire.blp",
    "Creature\\Crab\\CrabSkinVermillian.blp",
};

struct Runtime {
    ReadFunction original{};
    std::uint64_t seed{};
    HANDLE log{INVALID_HANDLE_VALUE};
    SRWLOCK logLock = SRWLOCK_INIT;
    std::once_flag probes;
    // Retain one metadata read and five asset probes for process lifetime.
    // Rust owns these allocations; no DLL CRT or guessed host deallocator is used.
    std::array<NativeReadResult, 6> retained{};
};
Runtime* g_runtime{};
std::atomic<unsigned> g_initialize{0};

void Log(Runtime& state, std::string_view message) noexcept {
    AcquireSRWLockExclusive(&state.logLock);
    if (state.log != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(state.log, message.data(), static_cast<DWORD>(message.size()), &written, nullptr);
        WriteFile(state.log, "\r\n", 2, &written, nullptr);
        FlushFileBuffers(state.log);
    }
    ReleaseSRWLockExclusive(&state.logLock);
}

[[noreturn]] void Stop(Runtime& state, const char* reason) noexcept {
    Log(state, reason);
    MessageBoxA(nullptr, reason, "Spiderless Azeroth Survivors: startup stopped", MB_OK | MB_ICONERROR);
    ExitProcess(ERROR_INVALID_DATA);
}

std::wstring ExeDirectory() {
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size()) throw std::runtime_error("Cannot resolve game directory");
    path.resize(length);
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) throw std::runtime_error("Game path has no directory");
    path.resize(slash + 1);
    return path;
}

bool SamePath(const char* path, std::size_t size, std::string_view expected) noexcept {
    if (!path || size != expected.size()) return false;
    for (std::size_t i = 0; i < size; ++i) {
        auto a = static_cast<unsigned char>(path[i]);
        auto b = static_cast<unsigned char>(expected[i]);
        if (a == '/') a = '\\';
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return false;
    }
    return true;
}

bool Valid(const NativeReadResult& read) noexcept {
    return read.capacity != std::numeric_limits<std::uint64_t>::max() &&
           read.data && read.size && read.size <= read.capacity && read.capacity <= kMaxRead;
}

void Probe(Runtime& state, void* chain) {
    std::uint64_t total = 0;
    const auto read = [&](NativeReadResult& out, std::string_view path) {
        if (state.original(&out, chain, path.data(), path.size()) != &out || !Valid(out)) {
            throw std::runtime_error("Required crab assets or creature DBC are missing. Prepare patch-Z.MPQ using the included tool and place it in the game's Data folder.");
        }
        total += out.capacity;
        if (total > kProbeBudget) throw std::runtime_error("Crab asset probe exceeded its memory budget");
    };
    read(state.retained[0], kModelDb);
    for (std::size_t i = 0; i < kAssets.size(); ++i) {
        auto& out = state.retained[i + 1];
        read(out, kAssets[i]);
        if (out.size < 8) throw std::runtime_error("Crab asset is truncated");
        if (i == 0) {
            std::uint32_t version;
            std::memcpy(&version, out.data + 4, 4);
            if (std::memcmp(out.data, "MD20", 4) != 0 || (version != 256 && version != 257)) {
                throw std::runtime_error("Crab mesh must be from a compatible Vanilla client");
            }
        } else if (std::memcmp(out.data, "BLP1", 4) != 0 && std::memcmp(out.data, "BLP2", 4) != 0) {
            throw std::runtime_error("Crab skin has an unsupported format");
        }
    }
    Log(state, "Crab mesh and four skins are readable through the game's actual archive chain.");
}

NativeReadResult* __fastcall ReadHook(NativeReadResult* out, void* chain,
                                    const char* path, std::size_t size) noexcept {
    auto& state = *g_runtime;
    auto* returned = state.original(out, chain, path, size);
    if (!SamePath(path, size, kDisplayDb)) return returned;
    try {
        if (returned != out || !Valid(*out)) throw std::runtime_error("CreatureDisplayInfo read failed");
        std::call_once(state.probes, [&] { Probe(state, chain); });
        const auto& models = state.retained[0];
        const auto count = RemapDisplayInfo(
            {models.data, static_cast<std::size_t>(models.size)},
            {out->data, static_cast<std::size_t>(out->size)}, state.seed);
        Log(state, "Replaced " + std::to_string(count) + " arachnid appearance rows with Vanilla crabs.");
    } catch (const std::exception& error) {
        Stop(state, error.what());
    } catch (...) {
        Stop(state, "Unexpected creature appearance replacement failure");
    }
    return returned;
}

std::uint8_t* FindReadEntry() {
    auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 0x100000) {
        throw std::runtime_error("Invalid game PE header");
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) ||
        nt->FileHeader.NumberOfSections > 96) {
        throw std::runtime_error("Expected a Windows x64 PE image");
    }
    const auto imageSize = nt->OptionalHeader.SizeOfImage;
    auto* sections = IMAGE_FIRST_SECTION(nt);
    std::uint8_t* match = nullptr;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& section = sections[i];
        const auto size = section.Misc.VirtualSize;
        if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        if (section.VirtualAddress > imageSize || size > imageSize - section.VirtualAddress ||
            size < kReadFileSignature.size()) continue;
        auto* start = base + section.VirtualAddress;
        for (std::size_t at = 0; at <= size - kReadFileSignature.size(); ++at) {
            if (start[at] != kReadFileSignature[0]) continue;
            bool equal = true;
            for (std::size_t j = 1; j < kReadFileSignature.size(); ++j) {
                if ((start[at + j] & kReadFileSignatureMask[j]) != kReadFileSignature[j]) {
                    equal = false;
                    break;
                }
            }
            if (!equal) continue;
            if (match) throw std::runtime_error("Asset reader signature is ambiguous after this game update");
            match = start + at;
        }
    }
    if (!match) throw std::runtime_error("Cannot locate a compatible asset reader after this game update");

    // The match must be the beginning of a registered native function, not
    // matching bytes inside another function or data in an executable section.
    DWORD64 unwindBase = 0;
    const auto* entry = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(match), &unwindBase, nullptr);
    if (!entry || unwindBase != reinterpret_cast<DWORD64>(base) ||
        entry->BeginAddress != static_cast<DWORD>(match - base) ||
        entry->EndAddress - entry->BeginAddress < kReadFileSignature.size() ||
        std::memcmp(match, kReadFilePrologue.data(), kReadFilePrologue.size()) != 0) {
        throw std::runtime_error("Asset reader function boundary/prologue is incompatible");
    }
    return match;
}

void AbsoluteJump(std::uint8_t* buffer, const void* address) {
    constexpr std::array<std::uint8_t, 6> prefix{0xff, 0x25, 0, 0, 0, 0};
    std::memcpy(buffer, prefix.data(), prefix.size());
    std::memcpy(buffer + prefix.size(), &address, sizeof(address));
}

void Install(Runtime& state) {
    auto* entry = FindReadEntry();
    constexpr auto copied = kReadFilePrologue.size();
    constexpr auto thunkSize = copied + 14;
    auto* thunk = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, thunkSize,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!thunk) throw std::runtime_error("Cannot allocate reader trampoline");
    std::memcpy(thunk, entry, copied);
    AbsoluteJump(thunk + copied, entry + copied);
    DWORD old;
    if (!VirtualProtect(thunk, thunkSize, PAGE_EXECUTE_READ, &old) ||
        !FlushInstructionCache(GetCurrentProcess(), thunk, thunkSize)) {
        VirtualFree(thunk, 0, MEM_RELEASE);
        throw std::runtime_error("Cannot finalize reader trampoline");
    }
    state.original = reinterpret_cast<ReadFunction>(thunk);
    std::array<std::uint8_t, copied> replacement;
    replacement.fill(0x90);
    AbsoluteJump(replacement.data(), reinterpret_cast<const void*>(&ReadHook));
    if (!VirtualProtect(entry, copied, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(thunk, 0, MEM_RELEASE);
        throw std::runtime_error("Cannot write asset reader detour");
    }
    std::memcpy(entry, replacement.data(), replacement.size());
    DWORD ignored;
    const auto restored = VirtualProtect(entry, copied, old, &ignored);
    const auto flushed = FlushInstructionCache(GetCurrentProcess(), entry, copied);
    if (!restored || !flushed) throw std::runtime_error("Cannot finalize asset reader detour");
    Log(state, "Installed unique asset reader signature. No game hash/version allowlist is used.");
}
}
}

extern "C" DWORD WINAPI SpiderlessInitialize(void*) noexcept {
    using namespace spiderless;
    unsigned expected = 0;
    if (!g_initialize.compare_exchange_strong(expected, 1)) {
        return expected == 2 ? ERROR_SUCCESS : ERROR_ALREADY_INITIALIZED;
    }
    // The entrypoint bootstrap calls this outside the loader lock and exception
    // handler, before restoring the game's original entrypoint context.
    auto* state = new (std::nothrow) Runtime;
    if (!state) return ERROR_OUTOFMEMORY;
    g_runtime = state;
    try {
        const auto logPath = ExeDirectory() + L"SpiderlessAzerothSurvivors.log";
        state->log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (state->log == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create Spiderless log beside the game");
        Log(*state, "Spiderless initializing before the game entrypoint.");
        if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&state->seed), sizeof(state->seed),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
            throw std::runtime_error("Cannot create crab appearance random seed");
        }
        HMODULE pinned;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(&g_initialize), &pinned)) {
            throw std::runtime_error("Cannot retain the mod for process lifetime");
        }
        Install(*state);
        g_initialize.store(2);
        return ERROR_SUCCESS;
    } catch (const std::exception& error) {
        Log(*state, error.what());
    } catch (...) {
        Log(*state, "Unexpected initialization failure");
    }
    g_initialize.store(3);
    return ERROR_INVALID_DATA;
}
