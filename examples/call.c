#include <windows.h>
#include <stdint.h>
#include <stdio.h>

typedef uint32_t (*BinaryFunction)(uint32_t, uint32_t);

int wmain(int argc, wchar_t **argv)
{
    if (argc != 2) {
        fputs("usage: call <path-to-add.dll>\n", stderr);
        return 1;
    }
    HMODULE library = LoadLibraryW(argv[1]);
    if (!library) {
        fprintf(stderr, "LoadLibraryW failed (%lu)\n", GetLastError());
        return 1;
    }
    BinaryFunction add = (BinaryFunction)GetProcAddress(library, "add");
    if (!add) {
        fprintf(stderr, "GetProcAddress failed (%lu)\n", GetLastError());
        FreeLibrary(library);
        return 1;
    }
    printf("add(9, 4) = %u\n", (unsigned int)add(9, 4));
    FreeLibrary(library);
    return 0;
}
