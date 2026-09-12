#include "memory.h"

#include <cstdint>
#include <cstring>
#include <tlhelp32.h>
#include <vector>

namespace {
    bool getImage(HMODULE module, unsigned char*& base, std::size_t& size) {
        base = reinterpret_cast<unsigned char*>(module);
        if (base == nullptr) {
            return false;
        }

        const IMAGE_DOS_HEADER* const dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
            return false;
        }
        const IMAGE_NT_HEADERS* const ntHeader = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dosHeader->e_lfanew);
        if (ntHeader->Signature != IMAGE_NT_SIGNATURE) {
            return false;
        }

        size = ntHeader->OptionalHeader.SizeOfImage;
        return size != 0;
    }

    bool getCode(HMODULE module, unsigned char*& start, std::size_t& size) {
        unsigned char* base = nullptr;
        std::size_t imageSize = 0;
        if (!getImage(module, base, imageSize)) {
            return false;
        }

        const IMAGE_DOS_HEADER* const dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const IMAGE_NT_HEADERS* const ntHeader = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dosHeader->e_lfanew);
        start = base + ntHeader->OptionalHeader.BaseOfCode;
        size = ntHeader->OptionalHeader.SizeOfCode;
        return size != 0 && start >= base && start + size <= base + imageSize;
    }

    int modRmSize(const unsigned char* code) {
        int size = 1;
        const unsigned char mod = code[0] >> 6;
        const unsigned char rm = code[0] & 7;
        if (mod != 3 && rm == 4) {
            ++size;
            const unsigned char base = code[1] & 7;
            if (mod == 0 && base == 5) {
                size += 4;
            }
        }
        if (mod == 0 && rm == 5) {
            size += 4;
        } else if (mod == 1) {
            ++size;
        } else if (mod == 2) {
            size += 4;
        }
        return size;
    }

    int instructionSize(const unsigned char* code) {
        if (code[0] >= 0x50 && code[0] <= 0x5F) {
            return 1;
        }
        if (code[0] == 0x8B || code[0] == 0x89 || code[0] == 0x8D) {
            return 1 + modRmSize(code + 1);
        }
        if (code[0] == 0x83) {
            return 2 + modRmSize(code + 1);
        }
        if (code[0] == 0x81) {
            return 5 + modRmSize(code + 1);
        }
        if (code[0] == 0x68) {
            return 5;
        }
        if (code[0] == 0x6A) {
            return 2;
        }
        return 0;
    }

    bool writeJump(unsigned char* address, const void* target) {
        const std::intptr_t distance = static_cast<const unsigned char*>(target) - address - 5;
        if (distance < INT32_MIN || distance > INT32_MAX) {
            return false;
        }

        address[0] = 0xE9;
        const std::int32_t offset = static_cast<std::int32_t>(distance);
        std::memcpy(address + 1, &offset, sizeof(offset));
        return true;
    }

    std::vector<HANDLE> pauseThreads() {
        std::vector<HANDLE> threads;
        const DWORD process = GetCurrentProcessId();
        const DWORD current = GetCurrentThreadId();
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            return threads;
        }

        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        if (Thread32First(snapshot, &entry)) {
            do {
                if (entry.th32OwnerProcessID != process || entry.th32ThreadID == current) {
                    continue;
                }
                const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
                if (thread != nullptr) {
                    if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
                        threads.push_back(thread);
                    } else {
                        CloseHandle(thread);
                    }
                }
            } while (Thread32Next(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return threads;
    }

    void resumeThreads(std::vector<HANDLE>& threads) {
        for (HANDLE thread : threads) {
            ResumeThread(thread);
            CloseHandle(thread);
        }
    }
}

void* memory::find(HMODULE module, const unsigned char* pattern, const char* mask) {
    if (pattern == nullptr || mask == nullptr) {
        return nullptr;
    }

    unsigned char* start = nullptr;
    std::size_t size = 0;
    if (!getCode(module, start, size)) {
        return nullptr;
    }

    const std::size_t patternSize = std::strlen(mask);
    if (patternSize == 0 || patternSize > size) {
        return nullptr;
    }

    for (std::size_t offset = 0; offset <= size - patternSize; ++offset) {
        bool matches = true;
        for (std::size_t index = 0; index < patternSize; ++index) {
            if (mask[index] == 'x' && start[offset + index] != pattern[index]) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return start + offset;
        }
    }
    return nullptr;
}

void* memory::findString(HMODULE module, const char* text) {
    if (text == nullptr || text[0] == '\0') {
        return nullptr;
    }

    unsigned char* base = nullptr;
    std::size_t size = 0;
    if (!getImage(module, base, size)) {
        return nullptr;
    }

    const std::size_t textSize = std::strlen(text) + 1;
    if (textSize > size) {
        return nullptr;
    }

    for (std::size_t offset = 0; offset <= size - textSize; ++offset) {
        if (std::memcmp(base + offset, text, textSize) == 0) {
            return base + offset;
        }
    }
    return nullptr;
}

void* memory::findReference(HMODULE module, const void* address) {
    if (address == nullptr) {
        return nullptr;
    }

    const std::uintptr_t value = reinterpret_cast<std::uintptr_t>(address);
    unsigned char pattern[sizeof(value)]{};
    std::memcpy(pattern, &value, sizeof(value));

    char mask[sizeof(value) + 1]{};
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        mask[index] = 'x';
    }
    return find(module, pattern, mask);
}

void* memory::functionStart(HMODULE module, const void* address) {
    unsigned char* code = nullptr;
    std::size_t codeSize = 0;
    if (!getCode(module, code, codeSize) || address == nullptr) {
        return nullptr;
    }

    unsigned char* current = static_cast<unsigned char*>(const_cast<void*>(address));
    if (current < code || current >= code + codeSize) {
        return nullptr;
    }

    unsigned char* limit = code;
    if (current - code > 0x2000) {
        limit = current - 0x2000;
    }
    while (current > limit + 3) {
        if (reinterpret_cast<std::uintptr_t>(current) % 16 == 0) {
            const bool previousEnd = current[-1] == 0xCC || current[-1] == 0xC3 || current[-3] == 0xC2;
            const bool firstPush = current[0] >= 0x50 && current[0] < 0x58;
            const bool secondPush = current[1] >= 0x50 && current[1] < 0x58;
            const bool stackFrame = current[1] == 0x8B && current[2] == 0xEC;
            if (previousEnd && firstPush && (secondPush || stackFrame)) {
                return current;
            }
        }
        --current;
    }
    return nullptr;
}

void* memory::relativeTarget(const void* instruction) {
    if (instruction == nullptr) {
        return nullptr;
    }

    const unsigned char* const bytes = static_cast<const unsigned char*>(instruction);
    if (bytes[0] != 0xE8 && bytes[0] != 0xE9) {
        return nullptr;
    }

    std::int32_t offset = 0;
    std::memcpy(&offset, bytes + 1, sizeof(offset));
    return const_cast<unsigned char*>(bytes + 5 + offset);
}

bool memory::write(void* address, const void* data, std::size_t size) {
    if (address == nullptr || data == nullptr || size == 0) {
        return false;
    }

    DWORD protection = 0;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &protection)) {
        return false;
    }
    std::memcpy(address, data, size);
    FlushInstructionCache(GetCurrentProcess(), address, size);
    DWORD ignored = 0;
    VirtualProtect(address, size, protection, &ignored);
    return true;
}

bool memory::call(void* address, const void* target, std::size_t size) {
    if (address == nullptr || target == nullptr || size < 5) {
        return false;
    }

    const std::intptr_t distance = reinterpret_cast<std::intptr_t>(target) - reinterpret_cast<std::intptr_t>(address) - 5;
    if (distance < INT32_MIN || distance > INT32_MAX) {
        return false;
    }

    std::vector<unsigned char> bytes(size, 0x90);
    bytes[0] = 0xE8;
    const std::int32_t offset = static_cast<std::int32_t>(distance);
    std::memcpy(bytes.data() + 1, &offset, sizeof(offset));
    return write(address, bytes.data(), bytes.size());
}

bool memory::hook(void* target, void* replacement, void** original) {
    if (target == nullptr || replacement == nullptr || original == nullptr) {
        return false;
    }

#if !defined(_M_IX86)
    return false;
#else
    unsigned char* const function = static_cast<unsigned char*>(target);
    int copied = 0;
    while (copied < 5) {
        const int size = instructionSize(function + copied);
        if (size == 0 || copied + size > 16) {
            return false;
        }
        copied += size;
    }

    unsigned char* const trampoline = static_cast<unsigned char*>(VirtualAlloc(nullptr, copied + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (trampoline == nullptr) {
        return false;
    }

    std::memcpy(trampoline, function, copied);
    if (!writeJump(trampoline + copied, function + copied)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }

    std::vector<HANDLE> threads = pauseThreads();
    DWORD protection = 0;
    if (!VirtualProtect(function, copied, PAGE_EXECUTE_READWRITE, &protection)) {
        resumeThreads(threads);
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }

    bool written = writeJump(function, replacement);
    if (written) {
        for (int index = 5; index < copied; ++index) {
            function[index] = 0x90;
        }
        FlushInstructionCache(GetCurrentProcess(), function, copied);
    }

    DWORD ignored = 0;
    VirtualProtect(function, copied, protection, &ignored);
    resumeThreads(threads);
    if (!written) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }

    *original = trampoline;
    return true;
#endif
}

bool memory::hook_iat(HMODULE module, const char* importedModule, const char* function, void* replacement, void** original) {
    unsigned char* const base = reinterpret_cast<unsigned char*>(module);
    if (base == nullptr || importedModule == nullptr || function == nullptr || replacement == nullptr || original == nullptr) {
        return false;
    }

    const IMAGE_DOS_HEADER* const dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    const IMAGE_NT_HEADERS* const ntHeader = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dosHeader->e_lfanew);
    if (ntHeader->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    const IMAGE_DATA_DIRECTORY& imports = ntHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imports.VirtualAddress == 0) {
        return false;
    }

    IMAGE_IMPORT_DESCRIPTOR* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
    for (; descriptor->Name != 0; ++descriptor) {
        const char* const moduleName = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(moduleName, importedModule) != 0 || descriptor->OriginalFirstThunk == 0) {
            continue;
        }

        IMAGE_THUNK_DATA* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
        IMAGE_THUNK_DATA* addresses = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData != 0; ++names, ++addresses) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                continue;
            }

            const IMAGE_IMPORT_BY_NAME* const import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(import->Name), function) != 0) {
                continue;
            }

            void** const address = reinterpret_cast<void**>(&addresses->u1.Function);
            *original = *address;
            DWORD protection = 0;
            if (!VirtualProtect(address, sizeof(*address), PAGE_READWRITE, &protection)) {
                return false;
            }
            InterlockedExchangePointer(reinterpret_cast<void* volatile*>(address), replacement);
            DWORD ignored = 0;
            VirtualProtect(address, sizeof(*address), protection, &ignored);
            return true;
        }
    }
    return false;
}
