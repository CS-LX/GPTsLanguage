#include "lua_prose.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <psapi.h>
extern "C" {
#include "lauxlib.h"
#include "lualib.h"
}
#include <cstdio>
#include <memory>

namespace {
using namespace gpt;
// A host can already have the unmodified public Lua DLL loaded. In that case
// its standard binary loader supplies the same behavior as stock luac.
int stock_load(lua_State *L) {
    size_t n;
    const char *bytes = luaL_checklstring(L, 1, &n);
    const char *name = luaL_checkstring(L, 2);
    bool env = lua_gettop(L) >= 3;
    if (luaL_loadbufferx(L, bytes, n, name, "b") != LUA_OK)
        return lua_error(L);
    if (env) {
        lua_pushvalue(L, 3);
        if (!lua_setupvalue(L, -2, 1))
            lua_pop(L, 1);
    }
    return 1;
}
lua_CFunction compiled_loader() {
    static auto fn = GetProcAddress(GetModuleHandleW(L"lua54.dll"), "gpt_load_compiled");
    return fn ? reinterpret_cast<lua_CFunction>(fn) : stock_load;
}
// State-local virtual source files. Each load creates a fresh Lua closure and _ENV.
// The normal Lua and native searchers remain available for unbundled dependencies.
constexpr char vfs[] = R"lua(
local files, loadcompiled = ...
local rawfile, rawdo = loadfile, dofile
local type, error, find, gsub = type, error, string.find, string.gsub
local function pathkey(path)
  path = gsub(path, "\\", "/")
  path = gsub(path, "/+", "/")
  path = gsub(path, "^%./", "")
  return (gsub(path, "/%./", "/"))
end
local function bundled(path, mode, ...)
  local chunk = files[pathkey(path)]
  if chunk then
    if mode and not find(mode,"t",1,true) then return nil, "attempt to load a text chunk (mode is '"..mode.."')" end
    return loadcompiled(chunk, "@"..path, ...)
  end
end
function loadfile(path, mode, ...)
  if type(path) == "string" and files[pathkey(path)] then return bundled(path,mode,...) end
  return rawfile(path,mode,...)
end
function dofile(path)
  if type(path) == "string" and files[pathkey(path)] then
    local f,msg = bundled(path)
    if not f then error(msg,2) end
    return f()
  end
  return rawdo(path)
end
local function search(name)
  if type(package.path) ~= "string" then return end
  local module = gsub(name, "%.", "/")
  for pattern in string.gmatch(package.path, "[^;]+") do
    local path = pathkey((gsub(pattern, "%?", function() return module end)))
    if files[path] then return bundled(path), path end
  end
  for _, path in ipairs({module..".lua", module.."/init.lua"}) do
    if files[path] then return bundled(path), path end
  end
end
table.insert(package.searchers, 2, search)
)lua";

int traceback(lua_State *L) {
    const char *msg = lua_tostring(L, 1);
    if (!msg)
        return 1;
    luaL_traceback(L, L, msg, 1);
    return 1;
}
void install(lua_State *L, HMODULE base) {
    MODULEINFO info{};
    if (!base || !GetModuleInformation(GetCurrentProcess(), base, &info, sizeof(info)))
        fail("invalid Lua image");
    auto p =
        lua54::read_image({static_cast<const std::uint8_t *>(info.lpBaseOfDll), info.SizeOfImage}, true);
    bool has_package = lua_getglobal(L, "package") == LUA_TTABLE;
    lua_pop(L, 1);
    if (has_package) {
        static const char registry_key = 0;
        if (lua_rawgetp(L, LUA_REGISTRYINDEX, &registry_key) == LUA_TNIL) {
            lua_pop(L, 1);
            lua_newtable(L);
            if (luaL_loadbufferx(L, vfs, sizeof(vfs) - 1, "=GPT bundled files", "t") != LUA_OK)
                fail(lua_tostring(L, -1));
            lua_pushvalue(L, -2);
            lua_pushcfunction(L, compiled_loader());
            if (lua_pcall(L, 2, 0, 0) != LUA_OK)
                fail(lua_tostring(L, -1));
            lua_pushvalue(L, -1);
            lua_rawsetp(L, LUA_REGISTRYINDEX, &registry_key);
        }
        for (const auto &c : p.chunks) {
            lua_pushlstring(L, reinterpret_cast<const char *>(c.bytecode.data()), c.bytecode.size());
            lua_setfield(L, -2, c.path.c_str());
        }
        lua_pop(L, 1);
    }
    for (const auto &c : p.chunks)
        if (c.path == p.entry) {
            lua_pushcfunction(L, compiled_loader());
            lua_pushlstring(L, reinterpret_cast<const char *>(c.bytecode.data()), c.bytecode.size());
            lua_pushlstring(L, c.path.data(), c.path.size());
            if (lua_pcall(L, 2, 1, 0) != LUA_OK)
                fail(lua_tostring(L, -1));
            return;
        }
    fail("missing Lua entry");
}
std::string utf8(const wchar_t *w) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (!len)
        fail("invalid Windows argument");
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
    s.pop_back();
    return s;
}
int start(HMODULE image) {
    std::unique_ptr<lua_State, decltype(&lua_close)> state(luaL_newstate(), lua_close);
    if (!state)
        fail("cannot allocate Lua state");
    auto L = state.get();
    luaL_openlibs(L);
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || !argc)
        fail("cannot read Windows arguments");
    std::unique_ptr<void, decltype(&LocalFree)> release(argv, LocalFree);
    if (!lua_checkstack(L, argc + 8))
        fail("too many arguments");
    lua_createtable(L, argc, 1);
    for (int i = 0; i < argc; ++i) {
        auto s = utf8(argv[i]);
        lua_pushlstring(L, s.data(), s.size());
        lua_rawseti(L, -2, i);
    }
    lua_setglobal(L, "arg");
    lua_pushcfunction(L, traceback);
    install(L, image);
    lua_getglobal(L, "arg");
    for (int i = 1; i < argc; ++i)
        lua_rawgeti(L, 3, i);
    lua_remove(L, 3);
    if (lua_pcall(L, argc - 1, 0, 1) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        std::fprintf(stderr, "%s\n", msg ? msg : "Lua raised a non-string error");
        return 1;
    }
    return 0;
}
int finish_module(lua_State *L, int, lua_KContext) { return lua_gettop(L); }
} // namespace
extern "C" __declspec(dllexport) void gpt_lua_start(HMODULE image) noexcept {
    const UINT old_cp = GetConsoleOutputCP();
    if (old_cp)
        SetConsoleOutputCP(CP_UTF8);
    int status = 1;
    try {
        status = start(image);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "GPT Lua: %s\n", e.what());
    }
    std::fflush(nullptr);
    if (old_cp)
        SetConsoleOutputCP(old_cp);
    ExitProcess(status);
}
extern "C" __declspec(dllexport) int gpt_lua_module(HMODULE image, unsigned, lua_State *L) {
    int argc = lua_gettop(L);
    bool failed = false;
    try {
        install(L, image);
    } catch (const std::exception &e) {
        lua_pushstring(L, e.what());
        failed = true;
    }
    if (failed)
        return lua_error(L);
    lua_insert(L, 1);
    lua_callk(L, argc, LUA_MULTRET, 0, finish_module);
    return lua_gettop(L);
}
