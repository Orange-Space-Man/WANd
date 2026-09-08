#pragma once

#include <windows.h>

namespace memory {
    bool hook_iat(HMODULE module, const char* importedModule, const char* function, void* replacement, void** original);
}
