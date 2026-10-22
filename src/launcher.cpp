#include <windows.h>
#include <tlhelp32.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if !defined(_M_X64)
#error SpiderlessLauncher must be built for Windows x64.
#endif

namespace {

constexpr DWORD kStartupTimeoutMs = 30000;
constexpr DWORD kRemoteCallTimeoutMs = 30000;
constexpr wchar_t kDllName[] = L"SpiderlessAzerothSurvivors.dll";

struct Failure {
    std::wstring message;
};

[[noreturn]] void Fail(std::wstring message) {
    throw Failure{std::move(message)};
}

[[noreturn]] void WinFail(std::wstring_view operation, DWORD error = GetLastError()) {
    wchar_t* systemMessage = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<wchar_t*>(&systemMessage), 0, nullptr);
    std::wstring message(operation);
    message += L" failed (Windows error " + std::to_wstring(error) + L")";
    if (length != 0 && systemMessage != nullptr) {
        message += L": ";
        message.append(systemMessage, length);
    }
    if (systemMessage != nullptr) {
        LocalFree(systemMessage);
    }
    Fail(std::move(message));
}

class Handle {
public:
    explicit Handle(HANDLE value = nullptr) noexcept : value_(value) {}
    ~Handle() {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const noexcept { return value_; }

private:
    HANDLE value_;
};

std::filesystem::path ModulePath(HMODULE module) {
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0) {
        WinFail(L"GetModuleFileNameW");
    }
    if (length >= path.size()) {
        Fail(L"The launcher or system module path is too long.");
    }
    return std::filesystem::path(std::wstring(path.data(), length));
}

std::filesystem::path AbsolutePath(const std::filesystem::path& path) {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetFullPathNameW(
        path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (length == 0) {
        WinFail(L"GetFullPathNameW");
    }
    if (length >= buffer.size()) {
        Fail(L"The game path is too long.");
    }
    return std::filesystem::path(std::wstring(buffer.data(), length));
}

std::wstring CanonicalFilePath(const std::filesystem::path& path) {
    Handle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        WinFail(L"Open module path");
    }
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetFinalPathNameByHandleW(file.get(), buffer.data(),
        static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length == 0) {
        WinFail(L"GetFinalPathNameByHandleW");
    }
    if (length >= buffer.size()) {
        Fail(L"A module path is too long.");
    }
    return std::wstring(buffer.data(), length);
}

// Parse raw PE data without loading the mod into the launcher or running DllMain.
// The read-only file handle denies writers/replacement until injection is complete.
class PeFile {
public:
    explicit PeFile(const std::filesystem::path& path)
        : file_(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {
        if (file_.get() == INVALID_HANDLE_VALUE) {
            WinFail(L"Open EXE/DLL file");
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file_.get(), &size)) {
            WinFail(L"GetFileSizeEx");
        }
        if (size.QuadPart <= 0 || size.QuadPart > 1024LL * 1024 * 1024) {
            Fail(L"The EXE/DLL file has an unsupported size.");
        }
        mapping_ = CreateFileMappingW(file_.get(), nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping_ == nullptr) {
            WinFail(L"CreateFileMappingW");
        }
        const void* view = MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0);
        if (view == nullptr) {
            const DWORD error = GetLastError();
            CloseHandle(mapping_);
            mapping_ = nullptr;
            WinFail(L"MapViewOfFile", error);
        }
        bytes_ = {static_cast<const std::byte*>(view), static_cast<std::size_t>(size.QuadPart)};
        try {
            Parse();
        } catch (...) {
            UnmapViewOfFile(bytes_.data());
            bytes_ = {};
            CloseHandle(mapping_);
            mapping_ = nullptr;
            throw;
        }
    }

    ~PeFile() {
        if (!bytes_.empty()) {
            UnmapViewOfFile(bytes_.data());
        }
        if (mapping_ != nullptr) {
            CloseHandle(mapping_);
        }
    }
    PeFile(const PeFile&) = delete;
    PeFile& operator=(const PeFile&) = delete;

    DWORD entrypoint() const noexcept { return optional_.AddressOfEntryPoint; }
    DWORD image_size() const noexcept { return optional_.SizeOfImage; }
    bool is_dll() const noexcept { return (header_.Characteristics & IMAGE_FILE_DLL) != 0; }

    DWORD InitializeExport() const {
        if (optional_.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) {
            Fail(L"The mod DLL does not have an export table.");
        }
        const auto directory = optional_.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (directory.VirtualAddress == 0 || directory.Size < sizeof(IMAGE_EXPORT_DIRECTORY)) {
            Fail(L"The mod DLL does not have a valid export table.");
        }
        const auto exports = ReadRva<IMAGE_EXPORT_DIRECTORY>(directory.VirtualAddress);
        constexpr std::string_view wanted = "SpiderlessInitialize";
        if (exports.NumberOfNames > bytes_.size() / sizeof(DWORD)) {
            Fail(L"The mod DLL export table is malformed.");
        }
        for (DWORD index = 0; index < exports.NumberOfNames; ++index) {
            const DWORD nameRva = ReadRva<DWORD>(ArrayRva(exports.AddressOfNames, index, sizeof(DWORD)));
            const auto nameOffset = RvaOffset(nameRva, wanted.size() + 1);
            if (std::memcmp(bytes_.data() + nameOffset, wanted.data(), wanted.size()) != 0 ||
                bytes_[nameOffset + wanted.size()] != std::byte{0}) {
                continue;
            }
            const WORD ordinal = ReadRva<WORD>(ArrayRva(exports.AddressOfNameOrdinals, index, sizeof(WORD)));
            if (ordinal >= exports.NumberOfFunctions) {
                Fail(L"The mod DLL export ordinal is malformed.");
            }
            const DWORD function = ReadRva<DWORD>(ArrayRva(exports.AddressOfFunctions, ordinal, sizeof(DWORD)));
            const std::uint64_t exportEnd = static_cast<std::uint64_t>(directory.VirtualAddress) + directory.Size;
            if (function == 0 || function >= optional_.SizeOfImage ||
                (function >= directory.VirtualAddress && function < exportEnd) ||
                !ExecutableRva(function)) {
                Fail(L"The mod DLL initialization export is invalid or forwarded.");
            }
            return function;
        }
        Fail(L"The mod DLL is missing SpiderlessInitialize.");
    }

private:
    template<typename T>
    T Read(std::size_t offset) const {
        if (offset > bytes_.size() || sizeof(T) > bytes_.size() - offset) {
            Fail(L"The EXE/DLL has truncated PE data.");
        }
        T value{};
        std::memcpy(&value, bytes_.data() + offset, sizeof(value));
        return value;
    }

    template<typename T>
    T ReadRva(DWORD rva) const { return Read<T>(RvaOffset(rva, sizeof(T))); }

    static DWORD ArrayRva(DWORD base, DWORD index, std::size_t size) {
        const std::uint64_t rva = static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(index) * size;
        if (rva > std::numeric_limits<DWORD>::max()) {
            Fail(L"The mod DLL export array overflows its image.");
        }
        return static_cast<DWORD>(rva);
    }

    std::size_t RvaOffset(DWORD rva, std::size_t length) const {
        if (rva < optional_.SizeOfHeaders && length <= optional_.SizeOfHeaders - rva &&
            rva <= bytes_.size() && length <= bytes_.size() - rva) {
            return rva;
        }
        for (const auto& section : sections_) {
            if (rva < section.VirtualAddress) {
                continue;
            }
            const std::uint64_t delta = static_cast<std::uint64_t>(rva) - section.VirtualAddress;
            if (delta > section.SizeOfRawData || length > section.SizeOfRawData - delta) {
                continue;
            }
            const std::uint64_t offset = static_cast<std::uint64_t>(section.PointerToRawData) + delta;
            if (offset <= bytes_.size() && length <= bytes_.size() - offset) {
                return static_cast<std::size_t>(offset);
            }
        }
        Fail(L"An EXE/DLL RVA does not refer to valid file data.");
    }

    bool ExecutableRva(DWORD rva) const {
        for (const auto& section : sections_) {
            const std::uint64_t end = static_cast<std::uint64_t>(section.VirtualAddress) + section.Misc.VirtualSize;
            if (rva >= section.VirtualAddress && rva < end &&
                (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) {
                return true;
            }
        }
        return false;
    }

    void Parse() {
        const auto dos = Read<IMAGE_DOS_HEADER>(0);
        if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) {
            Fail(L"The selected file is not a PE executable.");
        }
        const auto ntOffset = static_cast<std::size_t>(dos.e_lfanew);
        if (Read<DWORD>(ntOffset) != IMAGE_NT_SIGNATURE) {
            Fail(L"The selected file has an invalid PE signature.");
        }
        header_ = Read<IMAGE_FILE_HEADER>(ntOffset + sizeof(DWORD));
        if (header_.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            header_.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) ||
            header_.NumberOfSections == 0 || header_.NumberOfSections > 96) {
            Fail(L"Only Windows x64 PE images are supported.");
        }
        const auto optionalOffset = ntOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
        optional_ = Read<IMAGE_OPTIONAL_HEADER64>(optionalOffset);
        if (optional_.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || optional_.SizeOfImage == 0) {
            Fail(L"The selected file has an invalid x64 image header.");
        }
        const auto sectionOffset = optionalOffset + header_.SizeOfOptionalHeader;
        for (WORD index = 0; index < header_.NumberOfSections; ++index) {
            sections_.push_back(Read<IMAGE_SECTION_HEADER>(sectionOffset + index * sizeof(IMAGE_SECTION_HEADER)));
        }
        if (!is_dll() && (entrypoint() == 0 || entrypoint() >= image_size() || !ExecutableRva(entrypoint()))) {
            Fail(L"The game EXE does not have a valid executable entrypoint.");
        }
    }

    Handle file_;
    HANDLE mapping_ = nullptr;
    std::span<const std::byte> bytes_;
    IMAGE_FILE_HEADER header_{};
    IMAGE_OPTIONAL_HEADER64 optional_{};
    std::vector<IMAGE_SECTION_HEADER> sections_;
};

struct Module {
    std::uintptr_t base;
    DWORD size;
};

Module FindRemoteModule(DWORD processId, const std::filesystem::path& expectedPath) {
    const auto expected = CanonicalFilePath(expectedPath);
    for (int attempt = 0; attempt < 50; ++attempt) {
        Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, processId));
        if (snapshot.get() == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error == ERROR_BAD_LENGTH || error == ERROR_PARTIAL_COPY) {
                Sleep(20);
                continue;
            }
            WinFail(L"CreateToolhelp32Snapshot", error);
        }
        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (!Module32FirstW(snapshot.get(), &entry)) {
            const DWORD error = GetLastError();
            if (error == ERROR_NO_MORE_FILES || error == ERROR_BAD_LENGTH) {
                Sleep(20);
                continue;
            }
            WinFail(L"Module32FirstW", error);
        }
        do {
            if (_wcsicmp(entry.szModule, expectedPath.filename().c_str()) == 0 &&
                _wcsicmp(CanonicalFilePath(entry.szExePath).c_str(), expected.c_str()) == 0) {
                return {reinterpret_cast<std::uintptr_t>(entry.modBaseAddr), entry.modBaseSize};
            }
        } while (Module32NextW(snapshot.get(), &entry));
        const DWORD error = GetLastError();
        if (error != ERROR_NO_MORE_FILES) {
            WinFail(L"Module32NextW", error);
        }
        Sleep(20);
    }
    Fail(L"The expected module was not found in the newly launched game: " + expectedPath.filename().wstring());
}

// Own only the child created by this launch. Any error leaves no unpatched game running.
class Child {
public:
    explicit Child(const PROCESS_INFORMATION& info)
        : process(info.hProcess), thread(info.hThread), processId(info.dwProcessId), threadId(info.dwThreadId) {}
    ~Child() {
        if (!started_) {
            TerminateProcess(process.get(), ERROR_PROCESS_ABORTED);
        }
        if (pending_) {
            ContinueDebugEvent(event_.dwProcessId, event_.dwThreadId, DBG_CONTINUE);
        }
        if (debugging_) {
            DebugActiveProcessStop(processId);
        }
        if (!started_) {
            WaitForSingleObject(process.get(), 5000);
        }
    }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;

    DEBUG_EVENT WaitEvent(DWORD timeout) {
        event_ = {};
        if (!WaitForDebugEvent(&event_, timeout)) {
            const DWORD error = GetLastError();
            if (error == ERROR_SEM_TIMEOUT) {
                Fail(L"Timed out waiting for the game to finish loading safely.");
            }
            WinFail(L"WaitForDebugEvent", error);
        }
        pending_ = true;
        return event_;
    }
    void Continue(DWORD status) {
        if (!ContinueDebugEvent(event_.dwProcessId, event_.dwThreadId, status)) {
            WinFail(L"ContinueDebugEvent");
        }
        pending_ = false;
    }
    void Detach() {
        if (!DebugActiveProcessStop(processId)) {
            WinFail(L"DebugActiveProcessStop");
        }
        debugging_ = false;
    }
    void Started() noexcept { started_ = true; }

    Handle process;
    Handle thread;
    DWORD processId;
    DWORD threadId;

private:
    bool debugging_ = true;
    bool pending_ = false;
    bool started_ = false;
    DEBUG_EVENT event_{};
};

void WriteInstruction(HANDLE process, std::uintptr_t address, BYTE value) {
    auto* destination = reinterpret_cast<void*>(address);
    DWORD oldProtection = 0;
    if (!VirtualProtectEx(process, destination, 1, PAGE_EXECUTE_READWRITE, &oldProtection)) {
        WinFail(L"VirtualProtectEx entrypoint");
    }
    SIZE_T written = 0;
    const BOOL wrote = WriteProcessMemory(process, destination, &value, 1, &written);
    const DWORD writeError = wrote ? ERROR_WRITE_FAULT : GetLastError();
    DWORD ignored = 0;
    const BOOL restored = VirtualProtectEx(process, destination, 1, oldProtection, &ignored);
    const DWORD restoreError = GetLastError();
    if (!wrote || written != 1) {
        WinFail(L"WriteProcessMemory entrypoint", writeError);
    }
    if (!restored) {
        WinFail(L"Restore entrypoint protection", restoreError);
    }
    if (!FlushInstructionCache(process, destination, 1)) {
        WinFail(L"FlushInstructionCache entrypoint");
    }
}

void StopBeforeEntrypoint(Child& child, DWORD entryRva) {
    // The loader's initial breakpoint is BEFORE DLL initialization. A remote thread
    // cannot run yet. Let initialization finish and trap the EXE's first instruction.
    // https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/initial-breakpoint
    std::uintptr_t imageBase = 0;
    std::uintptr_t entryAddress = 0;
    BYTE original = 0;
    bool initialBreakpointSeen = false;
    const ULONGLONG deadline = GetTickCount64() + kStartupTimeoutMs;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            Fail(L"Timed out before reaching the game entrypoint.");
        }
        const auto event = child.WaitEvent(static_cast<DWORD>(deadline - now));
        DWORD status = DBG_CONTINUE;
        switch (event.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT:
            imageBase = reinterpret_cast<std::uintptr_t>(event.u.CreateProcessInfo.lpBaseOfImage);
            if (event.u.CreateProcessInfo.hFile != nullptr) {
                CloseHandle(event.u.CreateProcessInfo.hFile);
            }
            break;
        case LOAD_DLL_DEBUG_EVENT:
            if (event.u.LoadDll.hFile != nullptr) {
                CloseHandle(event.u.LoadDll.hFile);
            }
            break;
        case EXIT_PROCESS_DEBUG_EVENT:
            Fail(L"The game exited before the accessibility mod could initialize.");
        case EXCEPTION_DEBUG_EVENT: {
            const auto& exception = event.u.Exception;
            const auto exceptionAddress = reinterpret_cast<std::uintptr_t>(exception.ExceptionRecord.ExceptionAddress);
            if (exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT &&
                exception.dwFirstChance != 0 && event.dwThreadId == child.threadId) {
                if (!initialBreakpointSeen) {
                    if (imageBase == 0 || imageBase > std::numeric_limits<std::uintptr_t>::max() - entryRva) {
                        Fail(L"The game image base is invalid.");
                    }
                    entryAddress = imageBase + entryRva;
                    SIZE_T read = 0;
                    if (!ReadProcessMemory(child.process.get(), reinterpret_cast<void*>(entryAddress), &original, 1, &read) || read != 1) {
                        WinFail(L"ReadProcessMemory entrypoint");
                    }
                    if (original == 0xCC) {
                        Fail(L"The game entrypoint already contains a breakpoint.");
                    }
                    WriteInstruction(child.process.get(), entryAddress, 0xCC);
                    initialBreakpointSeen = true;
                } else if (exceptionAddress == entryAddress) {
                    WriteInstruction(child.process.get(), entryAddress, original);
                    CONTEXT context{};
                    context.ContextFlags = CONTEXT_CONTROL;
                    if (!GetThreadContext(child.thread.get(), &context)) {
                        WinFail(L"GetThreadContext");
                    }
                    if (context.Rip != entryAddress + 1) {
                        Fail(L"The entrypoint breakpoint stopped at an unexpected instruction.");
                    }
                    context.Rip = entryAddress;
                    if (!SetThreadContext(child.thread.get(), &context)) {
                        WinFail(L"SetThreadContext");
                    }
                    const DWORD previous = SuspendThread(child.thread.get());
                    if (previous == static_cast<DWORD>(-1)) {
                        WinFail(L"SuspendThread");
                    }
                    if (previous != 0) {
                        Fail(L"The main game thread has an unexpected suspend count.");
                    }
                    child.Continue(DBG_CONTINUE);
                    child.Detach();
                    return;
                } else {
                    status = DBG_EXCEPTION_NOT_HANDLED;
                }
            } else {
                if (exception.dwFirstChance == 0) {
                    Fail(L"The game encountered an unhandled exception before startup.");
                }
                status = DBG_EXCEPTION_NOT_HANDLED;
            }
            break;
        }
        default:
            break;
        }
        // Process/thread handles supplied by debug events are managed by the
        // debugging API. Close the image/DLL file handles explicitly above.
        child.Continue(status);
    }
}

DWORD RemoteCall(HANDLE process, std::uintptr_t address, void* argument, bool* allocationSafe = nullptr) {
    Handle remote(CreateRemoteThread(process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(address), argument, 0, nullptr));
    if (remote.get() == nullptr) {
        WinFail(L"CreateRemoteThread");
    }
    if (allocationSafe != nullptr) {
        *allocationSafe = false;
    }
    const DWORD wait = WaitForSingleObject(remote.get(), kRemoteCallTimeoutMs);
    if (wait == WAIT_TIMEOUT) {
        Fail(L"Timed out initializing the accessibility mod. The new game process was stopped.");
    }
    if (wait != WAIT_OBJECT_0) {
        WinFail(L"WaitForSingleObject remote initialization");
    }
    if (allocationSafe != nullptr) {
        *allocationSafe = true;
    }
    DWORD result = 0;
    if (!GetExitCodeThread(remote.get(), &result)) {
        WinFail(L"GetExitCodeThread");
    }
    return result;
}

class RemotePath {
public:
    RemotePath(HANDLE process, const std::filesystem::path& path) : process_(process) {
        const SIZE_T length = (path.native().size() + 1) * sizeof(wchar_t);
        memory_ = VirtualAllocEx(process_, nullptr, length, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (memory_ == nullptr) {
            WinFail(L"VirtualAllocEx DLL path");
        }
        SIZE_T written = 0;
        if (!WriteProcessMemory(process_, memory_, path.c_str(), length, &written) || written != length) {
            const DWORD error = GetLastError();
            VirtualFreeEx(process_, memory_, 0, MEM_RELEASE);
            memory_ = nullptr;
            WinFail(L"WriteProcessMemory DLL path", error);
        }
    }
    ~RemotePath() {
        // On timeout the loading thread may still reference this buffer. Leave it
        // allocated until Child terminates the newly launched process instead.
        if (memory_ != nullptr && safeToFree) {
            VirtualFreeEx(process_, memory_, 0, MEM_RELEASE);
        }
    }
    RemotePath(const RemotePath&) = delete;
    RemotePath& operator=(const RemotePath&) = delete;
    void* get() const noexcept { return memory_; }
    bool safeToFree = true;

private:
    HANDLE process_;
    void* memory_ = nullptr;
};

void InjectAndInitialize(Child& child, const std::filesystem::path& dll, const PeFile& image, DWORD exportRva) {
    const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (kernel == nullptr) {
        WinFail(L"GetModuleHandleW kernel32");
    }
    const FARPROC loadLibrary = GetProcAddress(kernel, "LoadLibraryW");
    if (loadLibrary == nullptr) {
        WinFail(L"GetProcAddress LoadLibraryW");
    }
    MEMORY_BASIC_INFORMATION owner{};
    if (VirtualQuery(reinterpret_cast<const void*>(loadLibrary), &owner, sizeof(owner)) == 0) {
        WinFail(L"VirtualQuery LoadLibraryW");
    }
    const auto localBase = reinterpret_cast<std::uintptr_t>(owner.AllocationBase);
    const auto offset = reinterpret_cast<std::uintptr_t>(loadLibrary) - localBase;
    // LoadLibraryW may be forwarded to KernelBase. Resolve the function's actual
    // owning image and apply its RVA to that same image in the child, preserving ASLR.
    const auto remoteOwner = FindRemoteModule(child.processId, ModulePath(static_cast<HMODULE>(owner.AllocationBase)));
    if (offset >= remoteOwner.size || remoteOwner.base > std::numeric_limits<std::uintptr_t>::max() - offset) {
        Fail(L"The remote LoadLibraryW address is outside its module.");
    }
    RemotePath path(child.process.get(), dll);
    // GetExitCodeThread returns only a DWORD, so it cannot carry a 64-bit HMODULE.
    // Confirm the completed load by enumerating the exact DLL path instead.
    RemoteCall(child.process.get(), remoteOwner.base + offset, path.get(), &path.safeToFree);
    const auto remoteDll = FindRemoteModule(child.processId, dll);
    if (remoteDll.size != image.image_size() || exportRva >= remoteDll.size ||
        remoteDll.base > std::numeric_limits<std::uintptr_t>::max() - exportRva) {
        Fail(L"The remote mod DLL does not match its parsed image.");
    }
    const DWORD result = RemoteCall(child.process.get(), remoteDll.base + exportRva, nullptr);
    if (result != ERROR_SUCCESS) {
        Fail(L"SpiderlessInitialize rejected this game or could not install the replacement (code " +
            std::to_wstring(result) + L"). Check SpiderlessAzerothSurvivors.log beside the game EXE.");
    }
}

void Launch(const std::filesystem::path& executable, const std::filesystem::path& dll) {
    PeFile gameImage(executable);
    PeFile modImage(dll);
    if (gameImage.is_dll() || !modImage.is_dll()) {
        Fail(L"Select the game EXE and keep the mod DLL beside SpiderlessLauncher.exe.");
    }
    const DWORD initializeRva = modImage.InitializeExport();
    std::wstring commandLine = L"\"" + executable.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    const auto directory = executable.parent_path();
    if (!CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
            DEBUG_ONLY_THIS_PROCESS, nullptr, directory.c_str(), &startup, &info)) {
        WinFail(L"CreateProcessW game");
    }
    Child child(info);
    StopBeforeEntrypoint(child, gameImage.entrypoint());
    InjectAndInitialize(child, dll, modImage, initializeRva);
    if (WaitForSingleObject(child.process.get(), 0) != WAIT_TIMEOUT) {
        Fail(L"The game exited during accessibility mod initialization.");
    }
    const DWORD previous = ResumeThread(child.thread.get());
    if (previous == static_cast<DWORD>(-1)) {
        WinFail(L"ResumeThread game");
    }
    if (previous != 1) {
        Fail(L"The game main thread has an unexpected resume count.");
    }
    child.Started();
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    try {
        if (argc > 2) {
            Fail(L"Usage: SpiderlessLauncher.exe [path to AzerothSurvivors.exe]\nGame arguments are not supported.");
        }
        const auto launcherDirectory = ModulePath(nullptr).parent_path();
        const auto executable = AbsolutePath(argc == 2 ? std::filesystem::path(argv[1]) :
            launcherDirectory / L"AzerothSurvivors.exe");
        const auto dll = launcherDirectory / kDllName;
        Launch(executable, dll);
        std::wcout << L"Spider replacement initialized. Game started.\n";
        return 0;
    } catch (const Failure& error) {
        const std::wstring message = error.message +
            L"\n\nThe launcher did not start an unpatched game. Any game process created by this launch was stopped.";
        std::wcerr << message << L'\n';
        MessageBoxW(nullptr, message.c_str(), L"Spiderless Azeroth Survivors", MB_OK | MB_ICONERROR);
    } catch (...) {
        constexpr wchar_t message[] = L"An unexpected launcher error occurred. Any game process created by this launch was stopped.";
        std::wcerr << message << L'\n';
        MessageBoxW(nullptr, message, L"Spiderless Azeroth Survivors", MB_OK | MB_ICONERROR);
    }
    return 1;
}
