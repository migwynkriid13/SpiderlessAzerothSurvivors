#include <windows.h>

#include "proxy.hpp"

#include <cstddef>
#include <cstdint>

#if !defined(_M_X64)
#error Spiderless requires Windows x64.
#endif

extern "C" DWORD WINAPI SpiderlessInitialize(void*) noexcept;

namespace {

struct Startup {
    HMODULE self{};
    std::uint8_t* entry{};
    std::uint8_t original{};
    DWORD thread{};
    PVOID handler{};
    bool armed{};
    alignas(16) CONTEXT resume{};
};

constinit Startup g_startup{};

[[noreturn]] void WINAPI Bootstrap() noexcept;

// This handler only redirects the saved context. Initialization happens after
// exception dispatch returns, outside both the handler and the loader lock.
LONG CALLBACK EntryTrap(EXCEPTION_POINTERS* exception) noexcept {
    if (!exception || !exception->ExceptionRecord || !exception->ContextRecord ||
        exception->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT ||
        exception->ExceptionRecord->ExceptionAddress != g_startup.entry ||
        GetCurrentThreadId() != g_startup.thread || !g_startup.armed ||
        (exception->ContextRecord->Rsp & 0xf) != 8 ||
        (exception->ContextRecord->Rip != reinterpret_cast<DWORD64>(g_startup.entry) &&
         exception->ContextRecord->Rip != reinterpret_cast<DWORD64>(g_startup.entry) + 1)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    g_startup.resume = *exception->ContextRecord;
    // This fixed-size buffer has no variable XSTATE tail. Keep the original
    // legacy register flags, including AMD64, without claiming an absent tail.
    // The Windows process entrypoint has no incoming AVX vector arguments.
    g_startup.resume.ContextFlags &= CONTEXT_ALL;
    g_startup.resume.Rip = reinterpret_cast<DWORD64>(g_startup.entry);
    g_startup.armed = false;
    exception->ContextRecord->Rip = reinterpret_cast<DWORD64>(&Bootstrap);
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Mapped input is read behind an SEH boundary so malformed headers fail DLL
// attachment instead of turning an unchecked header offset into a memory read.
std::uint8_t* FindEntry() noexcept {
    __try {
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        if (!base) return nullptr;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
            dos->e_lfanew > 0x100000) return nullptr;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            (nt->FileHeader.Characteristics & IMAGE_FILE_DLL) ||
            nt->FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) ||
            nt->FileHeader.NumberOfSections == 0 || nt->FileHeader.NumberOfSections > 96 ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return nullptr;
        const auto headersSize = nt->OptionalHeader.SizeOfHeaders;
        const auto imageSize = nt->OptionalHeader.SizeOfImage;
        const auto entryRva = nt->OptionalHeader.AddressOfEntryPoint;
        const auto sectionsEnd = static_cast<std::size_t>(dos->e_lfanew) + sizeof(*nt) +
            nt->FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER);
        if (headersSize > imageSize || sectionsEnd > headersSize ||
            !entryRva || entryRva >= imageSize) return nullptr;

        // Game TLS callbacks execute before its entrypoint. A future update
        // adding one could cache models before bootstrap, so reject that case.
        if (nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_TLS) {
            const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
            if (directory.VirtualAddress || directory.Size) {
                if (!directory.VirtualAddress || directory.Size < sizeof(IMAGE_TLS_DIRECTORY64) ||
                    directory.VirtualAddress > imageSize ||
                    directory.Size > imageSize - directory.VirtualAddress) return nullptr;
                const auto* tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(
                    base + directory.VirtualAddress);
                if (tls->AddressOfCallBacks) {
                    const auto baseAddress = reinterpret_cast<std::uintptr_t>(base);
                    if (imageSize < sizeof(std::uint64_t) || tls->AddressOfCallBacks < baseAddress ||
                        tls->AddressOfCallBacks - baseAddress > imageSize - sizeof(std::uint64_t) ||
                        *reinterpret_cast<const std::uint64_t*>(tls->AddressOfCallBacks) != 0) {
                        return nullptr;
                    }
                }
            }
        }
        const auto* sections = IMAGE_FIRST_SECTION(nt);
        bool executable = false;
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            const auto& section = sections[i];
            if (section.VirtualAddress > imageSize ||
                section.Misc.VirtualSize > imageSize - section.VirtualAddress) return nullptr;
            if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) &&
                entryRva >= section.VirtualAddress &&
                entryRva - section.VirtualAddress < section.Misc.VirtualSize) executable = true;
        }
        if (!executable) return nullptr;
        auto* entry = base + entryRva;
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(entry, &memory, sizeof(memory)) != sizeof(memory) ||
            memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE || memory.AllocationBase != base ||
            (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ||
            *entry == 0xcc) return nullptr;
        return entry;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool ArmEntryTrap(HMODULE self) noexcept {
    auto* entry = FindEntry();
    if (!entry) return false;
    g_startup.self = self;
    g_startup.entry = entry;
    g_startup.original = *entry;
    g_startup.thread = GetCurrentThreadId();
    g_startup.handler = AddVectoredExceptionHandler(1, &EntryTrap);
    if (!g_startup.handler) return false;
    DWORD protection{};
    if (!VirtualProtect(entry, 1, PAGE_EXECUTE_READWRITE, &protection)) {
        RemoveVectoredExceptionHandler(g_startup.handler);
        g_startup.handler = nullptr;
        return false;
    }
    g_startup.armed = true;
    *entry = 0xcc;
    DWORD ignored{};
    const auto restored = VirtualProtect(entry, 1, protection, &ignored);
    const auto flushed = FlushInstructionCache(GetCurrentProcess(), entry, 1);
    if (restored && flushed) return true;

    // A failed static attachment stops process startup. Roll back the trap and
    // handler as well, so loader error cleanup cannot leave a dangling callback.
    if (VirtualProtect(entry, 1, PAGE_EXECUTE_READWRITE, &ignored)) {
        *entry = g_startup.original;
        VirtualProtect(entry, 1, protection, &ignored);
        FlushInstructionCache(GetCurrentProcess(), entry, 1);
    }
    g_startup.armed = false;
    RemoveVectoredExceptionHandler(g_startup.handler);
    g_startup.handler = nullptr;
    return false;
}

[[noreturn]] void Stop(const wchar_t* reason, DWORD error) noexcept {
    MessageBoxW(nullptr, reason, L"Spiderless Azeroth Survivors: startup stopped",
                MB_OK | MB_ICONERROR);
    ExitProcess(error ? error : ERROR_DLL_INIT_FAILED);
}

[[noreturn]] void WINAPI Bootstrap() noexcept {
    DWORD protection{};
    if (*g_startup.entry != 0xcc ||
        !VirtualProtect(g_startup.entry, 1, PAGE_EXECUTE_READWRITE, &protection)) {
        Stop(L"Cannot restore the game entrypoint. Spiderless stopped startup safely.",
             ERROR_INVALID_DATA);
    }
    *g_startup.entry = g_startup.original;
    DWORD ignored{};
    const auto restored = VirtualProtect(g_startup.entry, 1, protection, &ignored);
    const auto flushed = FlushInstructionCache(GetCurrentProcess(), g_startup.entry, 1);
    if (!restored || !flushed || !RemoveVectoredExceptionHandler(g_startup.handler)) {
        Stop(L"Cannot finish the Spiderless startup handoff.", ERROR_INVALID_DATA);
    }
    g_startup.handler = nullptr;

    if (!spiderless::proxy::LoadSystemAvrt(g_startup.self)) {
        Stop(L"Cannot load the Windows system AVRT functions. Spiderless stopped startup safely.",
             GetLastError());
    }
    const auto result = SpiderlessInitialize(nullptr);
    if (result != ERROR_SUCCESS) {
        Stop(L"Spiderless could not initialize. Check SpiderlessAzerothSurvivors.log beside the game.",
             result);
    }

    // Restore the original entrypoint context, including its incoming registers,
    // stack and return address. No game entrypoint instructions were displaced.
    RtlRestoreContext(&g_startup.resume, nullptr);
    Stop(L"Windows could not resume the game entrypoint.", ERROR_INVALID_DATA);
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    // An implicit startup import is required. A late LoadLibrary cannot safely
    // intercept an entrypoint that may already have run.
    return reserved != nullptr && ArmEntryTrap(self);
}
