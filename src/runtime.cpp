#include "assembly.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <cstdio>

namespace {
thread_local char diagnostic[4096]{};
gpt::U32 invoke(HMODULE base, gpt::U32 method, gpt::U32 a, gpt::U32 b) {
    MODULEINFO info{};
    if (!base || !GetModuleInformation(GetCurrentProcess(), base, &info, sizeof(info)))
        gpt::fail("invalid module handle");
    auto module =
        gpt::read_dll({static_cast<const std::uint8_t *>(info.lpBaseOfDll), info.SizeOfImage}, true);
    if (method >= module.methods.size())
        gpt::fail("unknown method ID");
    return gpt::evaluate(module.methods[method], a, b);
}
} // namespace
extern "C" __declspec(dllexport) const char *gpt_last_error() noexcept {
    return diagnostic;
}

extern "C" __declspec(dllexport) gpt::U32 gpt_dispatch(HMODULE base, gpt::U32 method, gpt::U32 a,
                                                       gpt::U32 b) noexcept {
    try {
        auto result = invoke(base, method, a, b);
        diagnostic[0] = '\0';
        SetLastError(ERROR_SUCCESS);
        return result;
    } catch (const std::exception &e) {
        std::snprintf(diagnostic, sizeof(diagnostic), "%s", e.what());
    } catch (...) {
        std::snprintf(diagnostic, sizeof(diagnostic), "unknown GPT runtime error");
    }
    SetLastError(ERROR_BAD_FORMAT);
    return 0;
}
