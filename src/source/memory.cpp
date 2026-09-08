#include "memory.h"

#include <cstring>

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
