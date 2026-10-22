#include <windows.h>
#include <bcrypt.h>
#include <intrin.h>

#include "policy.hpp"
#include "scale_policy.hpp"
#include "embedded_assets.hpp"
#include "target_profile.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cmath>
#include <limits>
#include <mutex>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace spiderless {
namespace {
using ReadFunction = NativeReadResult* (__fastcall *)(NativeReadResult*, void*, const char*, std::size_t);
using ContainsFunction = bool (__fastcall *)(void*, const char*, std::size_t);
using CreateFunction = void (__fastcall *)(void*, std::uint64_t, std::uint32_t,
    std::uint32_t, float, float, bool, const void*);
using AppearanceFunction = void (__fastcall *)(void*, std::uint64_t, std::uint32_t, float);
using AllocateFunction = void* (__fastcall *)(std::size_t, std::size_t);
constexpr auto kMaxRead = 32ULL * 1024 * 1024;
constexpr auto kProbeBudget = 8ULL * 1024 * 1024;
constexpr std::string_view kModelDb = "DBFilesClient\\CreatureModelData.dbc";
constexpr std::string_view kDisplayDb = "DBFilesClient\\CreatureDisplayInfo.dbc";
struct Runtime {
    ReadFunction original{};
    ContainsFunction originalContains{};
    AllocateFunction allocate{};
    CreateFunction originalCreate{};
    AppearanceFunction originalAppearance{};
    const void* bossReturn{};
    const void* guardianReturn{};
    const void* ordinaryReturn{};
    const void* wardReturn{};
    std::mutex scaleLock;
    std::vector<std::uint32_t> replacedDisplays;
    bool displayCatalogReady{};
    void* activeGame{};
    std::unordered_map<std::uint64_t, CreatureRole> specialRoles;
    std::uint64_t seed{};
    HANDLE log{INVALID_HANDLE_VALUE};
    SRWLOCK logLock = SRWLOCK_INIT;
    std::once_flag probes;
    // The game owns this metadata allocation. Retain it for process lifetime.
    NativeReadResult models{};
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
    auto& out = state.models;
    if (state.original(&out, chain, kModelDb.data(), kModelDb.size()) != &out || !Valid(out)) {
        throw std::runtime_error("CreatureModelData is missing or incompatible");
    }
    if (out.capacity > kProbeBudget) {
        throw std::runtime_error("Creature model metadata exceeded its memory budget");
    }
    Log(state, "Creature model metadata is readable through the game's archive chain; crab assets are embedded in this DLL.");
}

bool __fastcall ContainsHook(void* chain, const char* path, std::size_t size) noexcept {
    if (!FindEmbeddedAsset(path, size).empty()) return true;
    return g_runtime->originalContains(chain, path, size);
}

NativeReadResult* __fastcall ReadHook(NativeReadResult* out, void* chain,
                                    const char* path, std::size_t size) noexcept {
    auto& state = *g_runtime;
    if (const auto bytes = FindEmbeddedAsset(path, size); !bytes.empty()) {
        // The native reader's own Vec<u8> clone uses this exact verified host
        // allocator with alignment 1. The game receives ownership and frees it
        // normally; no static resource pointer or DLL CRT allocation escapes.
        auto* memory = static_cast<std::uint8_t*>(state.allocate(bytes.size(), 1));
        if (!memory) Stop(state, "Cannot allocate an embedded crab asset");
        std::memcpy(memory, bytes.data(), bytes.size());
        *out = {bytes.size(), memory, bytes.size()};
        return out;
    }
    auto* returned = state.original(out, chain, path, size);
    if (!SamePath(path, size, kDisplayDb)) return returned;
    try {
        if (returned != out || !Valid(*out)) throw std::runtime_error("CreatureDisplayInfo read failed");
        std::call_once(state.probes, [&] { Probe(state, chain); });
        const auto& models = state.models;
        const auto replaced = RemapDisplayInfo(
            {models.data, static_cast<std::size_t>(models.size)},
            {out->data, static_cast<std::size_t>(out->size)}, state.seed);
        {
            std::lock_guard lock(state.scaleLock);
            if (state.displayCatalogReady && state.replacedDisplays != replaced) {
                throw std::runtime_error("Creature replacement display catalog changed during this run");
            }
            if (!state.displayCatalogReady) {
                state.replacedDisplays = replaced;
                state.displayCatalogReady = true;
            }
        }
        Log(state, "Replaced " + std::to_string(replaced.size()) + " arachnid appearance rows with Vanilla crabs.");
    } catch (const std::exception& error) {
        Stop(state, error.what());
    } catch (...) {
        Stop(state, "Unexpected creature appearance replacement failure");
    }
    return returned;
}

// The game owns one active run. Remember special roles independently of
// display IDs: the same spider appearance can be used by an ordinary elite
// and a lair guardian. Every creation replaces prior ownership for its GUID.
void RememberRole(Runtime& state, void* game, std::uint64_t guid, CreatureRole role) {
    constexpr std::size_t kMaxSpecialRoles = 65536;
    std::lock_guard lock(state.scaleLock);
    if (!state.displayCatalogReady) {
        throw std::runtime_error("Creature creation preceded replacement display catalog initialization");
    }
    if (state.activeGame != game) {
        state.specialRoles.clear();
        state.activeGame = game;
    }
    // Also remove stale special ownership when a new run reuses a GUID for an
    // ordinary enemy or ward. Never keep a native call inside this lock.
    state.specialRoles.erase(guid);
    if (role != CreatureRole::Regular) {
        if (state.specialRoles.size() >= kMaxSpecialRoles) {
            throw std::runtime_error("Creature role tracking exceeded its safety budget");
        }
        state.specialRoles.emplace(guid, role);
    }
}

float ScaledAppearance(Runtime& state, void* game, std::uint64_t guid,
                       std::uint32_t display, float scale) {
    std::lock_guard lock(state.scaleLock);
    auto role = CreatureRole::Regular;
    if (game == state.activeGame) {
        const auto found = state.specialRoles.find(guid);
        if (found != state.specialRoles.end()) role = found->second;
    }
    return ReplacementScale(state.replacedDisplays, display, scale, role);
}

void __fastcall AppearanceHook(void* game, std::uint64_t guid,
                               std::uint32_t display, float scale) noexcept {
    auto& state = *g_runtime;
    try {
        // Native restoration supplies its unmodified actor scale each time.
        // Multiply that input once; never read back an already enlarged model.
        const auto scaled = ScaledAppearance(state, game, guid, display, scale);
        state.originalAppearance(game, guid, display, scaled);
    } catch (const std::exception& error) {
        Stop(state, error.what());
    } catch (...) {
        Stop(state, "Unexpected creature appearance scaling failure");
    }
}

__declspec(noinline) void __fastcall CreateHook(void* game, std::uint64_t guid,
    std::uint32_t entry, std::uint32_t display, float scale, float hp,
    bool rank, const void* position) noexcept {
    auto& state = *g_runtime;
    const auto* caller = _ReturnAddress();
    try {
        auto role = CreatureRole::Regular;
        if (caller == state.bossReturn) role = CreatureRole::Boss;
        else if (caller == state.guardianReturn) role = CreatureRole::Guardian;
        else if (caller != state.ordinaryReturn && caller != state.wardReturn) {
            throw std::runtime_error("An unverified native creature creator reached the scale hook");
        }
        RememberRole(state, game, guid, role);
        // Preserve every original creation argument. In particular, the game
        // derives its bounding radius and combat reach from this raw scale.
        state.originalCreate(game, guid, entry, display, scale, hp, rank, position);
        const auto scaled = ScaledAppearance(state, game, guid, display, scale);
        if (std::isfinite(scale) && scale > 0.0f && scaled != scale) {
            // This native updater emits only display and visual scale fields.
            // Bypass AppearanceHook so this first adjustment is applied once.
            state.originalAppearance(game, guid, display, scaled);
        }
    } catch (const std::exception& error) {
        Stop(state, error.what());
    } catch (...) {
        Stop(state, "Unexpected creature creation scaling failure");
    }
}

bool Matches(std::span<const std::uint8_t> bytes,
             std::span<const std::uint8_t> signature,
             std::span<const std::uint8_t> mask) noexcept {
    if (bytes.size() < signature.size() || mask.size() != signature.size()) return false;
    for (std::size_t i = 0; i < signature.size(); ++i) {
        if ((bytes[i] & mask[i]) != signature[i]) return false;
    }
    return true;
}

class NativeImage {
public:
    NativeImage() {
        base_ = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base_);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 0x100000) {
            throw std::runtime_error("Invalid game PE header");
        }
        nt_ = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base_ + dos->e_lfanew);
        if (nt_->Signature != IMAGE_NT_SIGNATURE || nt_->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt_->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt_->FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) ||
            !nt_->FileHeader.NumberOfSections || nt_->FileHeader.NumberOfSections > 96) {
            throw std::runtime_error("Expected a Windows x64 PE image");
        }
        size_ = nt_->OptionalHeader.SizeOfImage;
        const auto* sections = IMAGE_FIRST_SECTION(nt_);
        if (!Inside(sections, sizeof(IMAGE_SECTION_HEADER) * nt_->FileHeader.NumberOfSections)) {
            throw std::runtime_error("Game PE section table is out of bounds");
        }
        for (unsigned i = 0; i < nt_->FileHeader.NumberOfSections; ++i) {
            if (sections[i].VirtualAddress > size_ ||
                sections[i].Misc.VirtualSize > size_ - sections[i].VirtualAddress) {
                throw std::runtime_error("Game PE section is out of bounds");
            }
        }
    }

    bool Inside(const void* address, std::size_t length) const noexcept {
        const auto value = reinterpret_cast<std::uintptr_t>(address);
        const auto base = reinterpret_cast<std::uintptr_t>(base_);
        return value >= base && value - base <= size_ && length <= size_ - (value - base);
    }

    bool Executable(const void* address, std::size_t length) const noexcept {
        if (!Inside(address, length)) return false;
        const auto rva = reinterpret_cast<const std::uint8_t*>(address) - base_;
        const auto* sections = IMAGE_FIRST_SECTION(nt_);
        for (unsigned i = 0; i < nt_->FileHeader.NumberOfSections; ++i) {
            const auto& section = sections[i];
            if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE) || rva < section.VirtualAddress) continue;
            const auto offset = rva - section.VirtualAddress;
            if (offset <= section.Misc.VirtualSize && length <= section.Misc.VirtualSize - offset) return true;
        }
        return false;
    }

    std::span<const std::uint8_t> Function(std::uint8_t* address, std::size_t minimum) const {
        if (!Executable(address, minimum)) throw std::runtime_error("Native function is outside executable image sections");
        DWORD64 unwindBase = 0;
        const auto* entry = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(address), &unwindBase, nullptr);
        if (!entry || unwindBase != reinterpret_cast<DWORD64>(base_) ||
            entry->BeginAddress != static_cast<DWORD>(address - base_) ||
            entry->EndAddress < entry->BeginAddress ||
            entry->EndAddress - entry->BeginAddress < minimum ||
            !Executable(address, entry->EndAddress - entry->BeginAddress)) {
            throw std::runtime_error("Native function boundary is incompatible after this game update");
        }
        return {address, entry->EndAddress - entry->BeginAddress};
    }

    std::uint8_t* FindCode(std::span<const std::uint8_t> signature,
                       std::span<const std::uint8_t> mask, const char* error) const {
        std::uint8_t* match = nullptr;
        const auto* sections = IMAGE_FIRST_SECTION(nt_);
        for (unsigned i = 0; i < nt_->FileHeader.NumberOfSections; ++i) {
            const auto& section = sections[i];
            if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            const auto length = section.Misc.VirtualSize;
            if (length < signature.size()) continue;
            auto* start = base_ + section.VirtualAddress;
            for (std::size_t at = 0; at <= length - signature.size(); ++at) {
                if (start[at] != signature.front() || !Matches({start + at, signature.size()}, signature, mask)) continue;
                if (match) throw std::runtime_error(error);
                match = start + at;
            }
        }
        if (!match) throw std::runtime_error(error);
        return match;
    }

    std::uint8_t* Find(std::span<const std::uint8_t> signature,
                       std::span<const std::uint8_t> mask, const char* error) const {
        auto* match = FindCode(signature, mask, error);
        Function(match, signature.size());
        return match;
    }

    void ValidateCallSite(std::uint8_t* start, std::size_t size,
                          std::size_t callOffset, std::uint8_t* expected) const {
        if (callOffset + 5 > size || !Executable(start, size) ||
            Relative(start + callOffset, 0xe8) != expected) {
            throw std::runtime_error("Creature role signature no longer calls the verified unit creator");
        }
        DWORD64 unwindBase = 0;
        const auto* entry = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(start), &unwindBase, nullptr);
        if (!entry || unwindBase != reinterpret_cast<DWORD64>(base_)) {
            throw std::runtime_error("Creature role call has no compatible native function boundary");
        }
        auto* begin = base_ + entry->BeginAddress;
        const auto function = Function(begin, 1);
        if (start < begin || static_cast<std::size_t>(start - begin) > function.size() ||
            size > function.size() - static_cast<std::size_t>(start - begin)) {
            throw std::runtime_error("Creature role signature crosses a native function boundary");
        }
    }

    void ValidateCreatorCallers(std::uint8_t* create,
                               const std::array<const void*, 4>& returns) const {
        std::array<bool, 4> seen{};
        const auto* sections = IMAGE_FIRST_SECTION(nt_);
        for (unsigned i = 0; i < nt_->FileHeader.NumberOfSections; ++i) {
            const auto& section = sections[i];
            if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE) || section.Misc.VirtualSize < 5) continue;
            auto* start = base_ + section.VirtualAddress;
            for (std::size_t at = 0; at <= section.Misc.VirtualSize - 5; ++at) {
                if (start[at] != 0xe8) continue;
                std::int32_t displacement;
                std::memcpy(&displacement, start + at + 1, sizeof(displacement));
                const auto next = reinterpret_cast<std::uintptr_t>(start + at + 5);
                if (next + displacement != reinterpret_cast<std::uintptr_t>(create)) continue;
                const auto found = std::find(returns.begin(), returns.end(), start + at + 5);
                if (found == returns.end()) {
                    // Conservatively reject even an ambiguous E8 byte sequence:
                    // a new actor producer must be understood before scaling it.
                    throw std::runtime_error("The native unit creator has an unverified caller after this game update");
                }
                seen[static_cast<std::size_t>(found - returns.begin())] = true;
            }
        }
        if (std::find(seen.begin(), seen.end(), false) != seen.end()) {
            throw std::runtime_error("A verified native creature role call is missing");
        }
    }

    std::uint8_t* Relative(std::uint8_t* instruction, std::uint8_t opcode) const {
        if (!Executable(instruction, 5) || instruction[0] != opcode) {
            throw std::runtime_error("Native relative call/branch is incompatible");
        }
        std::int32_t displacement;
        std::memcpy(&displacement, instruction + 1, sizeof(displacement));
        const auto target = reinterpret_cast<std::uintptr_t>(instruction) + 5 + displacement;
        auto* result = reinterpret_cast<std::uint8_t*>(target);
        if (!Executable(result, 1)) throw std::runtime_error("Native call target is outside executable image sections");
        return result;
    }

    bool Reaches(std::uint8_t* instruction, std::uint8_t* expected) const {
        auto* target = Relative(instruction, 0xe8);
        if (target == expected) return true;
        // Rust emits one JMP thunk for its global allocation entrypoints.
        return target[0] == 0xe9 && Relative(target, 0xe9) == expected;
    }

    void Import(std::uint8_t* instruction, std::size_t displacementOffset,
                std::size_t instructionLength, const char* expected) const {
        if (!Executable(instruction, instructionLength)) throw std::runtime_error("Native allocator import instruction is out of bounds");
        std::int32_t displacement;
        std::memcpy(&displacement, instruction + displacementOffset, sizeof(displacement));
        auto* slot = reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(instruction) + instructionLength + displacement);
        if (!Inside(slot, sizeof(void*))) throw std::runtime_error("Native allocator import slot is out of bounds");
        FARPROC imported;
        std::memcpy(&imported, slot, sizeof(imported));
        const auto system = GetModuleHandleW(L"kernel32.dll");
        if (!system || !imported || imported != GetProcAddress(system, expected)) {
            throw std::runtime_error("Native allocator no longer uses compatible Windows heap imports");
        }
    }

private:
    std::uint8_t* base_{};
    const IMAGE_NT_HEADERS64* nt_{};
    std::size_t size_{};
};

std::uint8_t* FindInside(std::span<const std::uint8_t> function,
                         std::span<const std::uint8_t> signature,
                         std::span<const std::uint8_t> mask) {
    const std::uint8_t* match = nullptr;
    if (function.size() >= signature.size()) {
        for (std::size_t i = 0; i <= function.size() - signature.size(); ++i) {
            if (!Matches(function.subspan(i), signature, mask)) continue;
            if (match) throw std::runtime_error("Native structural signature is ambiguous");
            match = function.data() + i;
        }
    }
    if (!match) throw std::runtime_error("Native structural signature is missing");
    return const_cast<std::uint8_t*>(match);
}

AllocateFunction FindAllocator(const NativeImage& image, std::uint8_t* read) {
    auto* allocate = image.Find(kAllocateSignature, kAllocateSignatureMask,
        "Cannot uniquely locate a compatible native allocator after this game update");
    auto* deallocate = image.Find(kDeallocateSignature, kDeallocateSignatureMask,
        "Cannot uniquely locate a compatible native deallocator after this game update");
    auto* heap = image.Relative(allocate + 30, 0xe9);
    if (image.Relative(allocate + 48, 0xe8) != heap ||
        !Matches(image.Function(heap, kHeapAllocateSignature.size()), kHeapAllocateSignature, kHeapAllocateSignatureMask)) {
        throw std::runtime_error("Native allocator's heap helper is incompatible");
    }
    image.Import(heap + 17, 2, 6, "GetProcessHeap");
    image.Import(heap + 43, 3, 7, "HeapAlloc");
    image.Import(deallocate + 24, 2, 6, "GetProcessHeap");
    image.Import(deallocate + 44, 3, 7, "HeapFree");

    // Finding a System allocator somewhere in the image is insufficient: the
    // active read path must clone/drop byte vectors through these functions.
    auto* cachedRead = image.Relative(read + 53, 0xe8);
    const auto cache = image.Function(cachedRead, kByteCloneSignature.size());
    auto* clone = FindInside(cache, kByteCloneSignature, kByteCloneSignatureMask);
    auto* drop = FindInside(cache, kByteDropSignature, kByteDropSignatureMask);
    if (!image.Reaches(clone + 8, allocate) || !image.Reaches(drop + 10, deallocate)) {
        throw std::runtime_error("The game asset reader no longer uses the verified byte-vector allocator");
    }
    return reinterpret_cast<AllocateFunction>(allocate);
}

void AbsoluteJump(std::uint8_t* buffer, const void* address) {
    constexpr std::array<std::uint8_t, 6> prefix{0xff, 0x25, 0, 0, 0, 0};
    std::memcpy(buffer, prefix.data(), prefix.size());
    std::memcpy(buffer + prefix.size(), &address, sizeof(address));
}

template<std::size_t N>
void* PrepareDetour(const NativeImage& image, std::uint8_t* entry,
                    const std::array<std::uint8_t, N>& prologue) {
    static_assert(N >= 14);
    image.Function(entry, N);
    if (std::memcmp(entry, prologue.data(), N) != 0) {
        throw std::runtime_error("Native detour prologue is incompatible");
    }
    constexpr auto thunkSize = N + 14;
    auto* thunk = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, thunkSize,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!thunk) throw std::runtime_error("Cannot allocate native reader trampoline");
    std::memcpy(thunk, entry, N);
    AbsoluteJump(thunk + N, entry + N);
    DWORD old;
    if (!VirtualProtect(thunk, thunkSize, PAGE_EXECUTE_READ, &old) ||
        !FlushInstructionCache(GetCurrentProcess(), thunk, thunkSize)) {
        VirtualFree(thunk, 0, MEM_RELEASE);
        throw std::runtime_error("Cannot finalize native reader trampoline");
    }
    return thunk;
}

template<std::size_t N>
void WriteDetour(std::uint8_t* entry, const void* hook,
                 const std::array<std::uint8_t, N>&) {
    std::array<std::uint8_t, N> replacement;
    replacement.fill(0x90);
    AbsoluteJump(replacement.data(), hook);
    DWORD old;
    if (!VirtualProtect(entry, N, PAGE_EXECUTE_READWRITE, &old)) {
        throw std::runtime_error("Cannot write native reader detour");
    }
    std::memcpy(entry, replacement.data(), N);
    DWORD ignored;
    const auto restored = VirtualProtect(entry, N, old, &ignored);
    const auto flushed = FlushInstructionCache(GetCurrentProcess(), entry, N);
    if (!restored || !flushed) throw std::runtime_error("Cannot finalize native reader detour");
}

template<std::size_t N>
const void* FindRoleReturn(const NativeImage& image, std::uint8_t* create,
    const std::array<std::uint8_t, N>& signature,
    const std::array<std::uint8_t, N>& mask, std::size_t callOffset) {
    auto* site = image.FindCode(signature, mask,
        "Cannot uniquely locate a compatible creature role call after this game update");
    image.ValidateCallSite(site, N, callOffset, create);
    return site + callOffset + 5;
}

void Install(Runtime& state) {
    const NativeImage image;
    auto* read = image.Find(kReadFileSignature, kReadFileSignatureMask,
        "Cannot uniquely locate a compatible asset reader after this game update");
    auto* contains = image.Find(kContainsSignature, kContainsSignatureMask,
        "Cannot uniquely locate a compatible asset availability check after this game update");
    state.allocate = FindAllocator(image, read);
    auto* create = image.Find(kCreateUnitSignature, kCreateUnitSignatureMask,
        "Cannot uniquely locate a compatible native unit creator after this game update");
    auto* appearance = image.Find(kAppearanceSignature, kAppearanceSignatureMask,
        "Cannot uniquely locate a compatible visual model updater after this game update");
    FindInside(image.Function(create, kCreateUnitSignature.size()),
        kCreateScaleSignature, kCreateScaleSignatureMask);
    FindInside(image.Function(appearance, kAppearanceSignature.size()),
        kAppearanceScaleSignature, kAppearanceScaleSignatureMask);
    state.bossReturn = FindRoleReturn(image, create, kBossCreateCallSignature,
        kBossCreateCallSignatureMask, kBossCreateCallOffset);
    state.guardianReturn = FindRoleReturn(image, create, kGuardianCreateCallSignature,
        kGuardianCreateCallSignatureMask, kGuardianCreateCallOffset);
    state.ordinaryReturn = FindRoleReturn(image, create, kOrdinaryCreateCallSignature,
        kOrdinaryCreateCallSignatureMask, kOrdinaryCreateCallOffset);
    state.wardReturn = FindRoleReturn(image, create, kWardCreateCallSignature,
        kWardCreateCallSignatureMask, kWardCreateCallOffset);
    image.ValidateCreatorCallers(create,
        {state.bossReturn, state.guardianReturn, state.ordinaryReturn, state.wardReturn});
    // Prepare every forwarding trampoline and structural check before changing
    // any entry. Bootstrap completes before any game-owned code executes.
    state.original = reinterpret_cast<ReadFunction>(PrepareDetour(image, read, kReadFilePrologue));
    state.originalContains = reinterpret_cast<ContainsFunction>(PrepareDetour(image, contains, kContainsPrologue));
    state.originalCreate = reinterpret_cast<CreateFunction>(PrepareDetour(image, create, kCreateUnitPrologue));
    state.originalAppearance = reinterpret_cast<AppearanceFunction>(PrepareDetour(image, appearance, kAppearancePrologue));
    WriteDetour(read, reinterpret_cast<const void*>(&ReadHook), kReadFilePrologue);
    WriteDetour(contains, reinterpret_cast<const void*>(&ContainsHook), kContainsPrologue);
    WriteDetour(create, reinterpret_cast<const void*>(&CreateHook), kCreateUnitPrologue);
    WriteDetour(appearance, reinterpret_cast<const void*>(&AppearanceHook), kAppearancePrologue);
    Log(state, "Installed per-creature visual scaling: bosses/guardians 7x, regular enemies 2x; native combat scale is preserved.");
    Log(state, "Installed unique asset read/availability signatures and verified native byte-vector ownership. No game hash/version allowlist is used.");
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
        InitializeEmbeddedAssets();
        Log(*state, "Validated embedded crab mesh, skins and texture dependencies; no MPQ extraction is required.");
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
