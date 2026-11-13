/* See gd_iat.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include "gd_iat.h"

void **gd_find_iat_slot(uint8_t *base, const char *dll, const char *func)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;
    IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress) return NULL;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress); d->Name; d++) {
        if (_stricmp((const char *)(base + d->Name), dll)) continue;
        if (!d->OriginalFirstThunk) return NULL;       /* no name table: cannot match by name */
        IMAGE_THUNK_DATA64 *names = (IMAGE_THUNK_DATA64 *)(base + d->OriginalFirstThunk);
        void **iat = (void **)(base + d->FirstThunk);
        for (size_t i = 0; names[i].u1.AddressOfData; i++) {
            if (IMAGE_SNAP_BY_ORDINAL64(names[i].u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + names[i].u1.AddressOfData);
            if (!strcmp((const char *)ibn->Name, func)) return &iat[i];
        }
    }
    return NULL;
}

unsigned gd_device_flags(unsigned engine_flags, int prevent_threading)
{
    unsigned f = engine_flags;
    if (prevent_threading) f |= GD_PREVENT_INTERNAL_THREADING;
    return f;
}
