#include "draft.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <shellapi.h>
#include <cstdio>
#include <memory>

extern "C" IMAGE_DOS_HEADER __ImageBase;
namespace {
using namespace gpt;
std::vector<std::string> arguments() {
    int n = 0;
    auto w = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!w)
        fail("cannot read Windows arguments");
    std::unique_ptr<void, decltype(&LocalFree)> release(w, LocalFree);
    std::vector<std::string> args;
    for (int i = 0; i < n; ++i) {
        int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
        if (!len)
            fail("invalid Windows argument");
        std::string s(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w[i], -1, s.data(), len, nullptr, nullptr);
        s.pop_back();
        args.push_back(std::move(s));
    }
    return args;
}
int run(HMODULE image, const char *const *argv) noexcept {
    try {
        MODULEINFO info{};
        if (!GetModuleInformation(GetCurrentProcess(), image, &info, sizeof(info)))
            fail("invalid mapped GPT image");
        auto code =
            draft::load({static_cast<const std::uint8_t *>(info.lpBaseOfDll), info.SizeOfImage}, true);
        std::vector<std::string> args;
        if (argv) {
            while (*argv)
                args.emplace_back(*argv++);
        } else
            args = arguments();
        return draft::run(code, args);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "GPT VM: %s\n", e.what());
        return 1;
    }
}
// Only the historical luaopen_* entry receives a foreign lua_State. It belongs
// to its host. Keep the ABI adapter out of the standalone execution path.
FARPROC adapter() {
    static HMODULE library = []() {
        wchar_t name[32768];
        auto n = GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), name, std::size(name));
        if (!n || n == std::size(name))
            return HMODULE(nullptr);
        auto path = std::filesystem::path(name).parent_path() / L"gptcompat.dll";
        return LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }();
    return library ? GetProcAddress(library, "gpt_lua_module") : nullptr;
}
} // namespace
extern "C" __declspec(dllexport) void gpt_lua_start(HMODULE image) noexcept {
    UINT previous = GetConsoleOutputCP();
    if (previous)
        SetConsoleOutputCP(CP_UTF8);
    auto status = run(image, nullptr);
    std::fflush(nullptr);
    if (previous)
        SetConsoleOutputCP(previous);
    ExitProcess(status);
}
extern "C" __declspec(dllexport) int gpt_lua_module(HMODULE image, unsigned id, void *argument) {
    if (id == 1 || !argument)
        return run(image, static_cast<const char *const *>(argument));
    auto fn = adapter();
    if (fn)
        return reinterpret_cast<int (*)(HMODULE, unsigned, void *)>(fn)(image, 0, argument);
    auto error = GetProcAddress(GetModuleHandleW(L"lua54.dll"), "luaL_error");
    if (error)
        return reinterpret_cast<int (*)(void *, const char *, ...)>(error)(
            argument, "Lua host entry requires gptcompat.dll beside gptlua54.dll");
    std::fputs("GPT: Lua host adapter unavailable\n", stderr);
    return 0;
}
