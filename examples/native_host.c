#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

typedef uint32_t (*Method)(uint32_t, uint32_t);
typedef const char* (*Diagnostic)(void);

static int number(const wchar_t* text, uint32_t* result)
{
    uint64_t value = 0;
    if (!*text) return 0;
    while (*text) {
        if (*text < L'0' || *text > L'9') return 0;
        value = value * 10 + (*text++ - L'0');
        if (value > UINT32_MAX) return 0;
    }
    *result = (uint32_t)value;
    return 1;
}

int wmain(int argc, wchar_t** argv)
{
    uint32_t a, b;
    if (argc != 5 || !number(argv[3], &a) || !number(argv[4], &b)) {
        fputs("usage: sample-host <module.dll> <export> <first u32> <second u32>\n", stderr);
        return 2;
    }
    char name[65536];
    size_t length = wcslen(argv[2]);
    if (length >= sizeof(name)) return 2;
    for (size_t i = 0; i <= length; ++i) {
        if (argv[2][i] > 127) return 2;
        name[i] = (char)argv[2][i];
    }
    HMODULE library = LoadLibraryW(argv[1]);
    if (!library) {
        fprintf(stderr, "LoadLibraryW failed (%lu)\n", GetLastError());
        return 1;
    }
    Method method = (Method)GetProcAddress(library, name);
    if (!method) {
        fprintf(stderr, "export %s missing (%lu)\n", name, GetLastError());
        FreeLibrary(library);
        return 1;
    }
    SetLastError(ERROR_SUCCESS);
    uint32_t result = method(a, b);
    DWORD error = GetLastError();
    if (error) {
        HMODULE runtime = GetModuleHandleW(L"gptrt.dll");
        Diagnostic diagnostic = runtime ? (Diagnostic)GetProcAddress(runtime, "gpt_last_error") : NULL;
        fprintf(stderr, "call failed (%lu): %s\n", error, diagnostic ? diagnostic() : "no diagnostic");
    } else {
        printf("%s(%u, %u) = %u\n", name, (unsigned)a, (unsigned)b, (unsigned)result);
    }
    FreeLibrary(library);
    return error ? 1 : 0;
}
