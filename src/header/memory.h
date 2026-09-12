#pragma once

#include <windows.h>

#include <cstddef>

namespace memory {
    void* find(HMODULE module, const unsigned char* pattern, const char* mask);
    void* findString(HMODULE module, const char* text);
    void* findReference(HMODULE module, const void* address);
    void* functionStart(HMODULE module, const void* address);
    void* relativeTarget(const void* instruction);
    bool write(void* address, const void* data, std::size_t size);
    bool call(void* address, const void* target, std::size_t size);
    bool hook(void* target, void* replacement, void** original);
    bool hook_iat(HMODULE module, const char* importedModule, const char* function, void* replacement, void** original);
}
