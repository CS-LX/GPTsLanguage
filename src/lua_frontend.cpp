#include "lua_prose.hpp"
extern "C" {
#include "lauxlib.h"
}
#include <fstream>
#include <map>
#include <memory>

namespace gpt::lua54 {
namespace fs = std::filesystem;
namespace {
int dump_writer(lua_State *, const void *data, size_t n, void *context) noexcept {
    auto &out = *static_cast<Bytes *>(context);
    if (n > limit - out.size())
        return 1;
    try {
        auto bytes = static_cast<const std::uint8_t *>(data);
        out.insert(out.end(), bytes, bytes + n);
        return 0;
    } catch (...) {
        return 1;
    }
}
std::string utf8(const fs::path &p) {
    auto s = p.generic_u8string();
    return {s.begin(), s.end()};
}
Chunk compile_chunk(lua_State *L, const std::string &name, const fs::path &path) {
    Chunk c;
    c.path = name;
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    auto length = input.tellg();
    if (length < 0 || length > std::streamoff(limit))
        fail("cannot read Lua source: " + c.path);
    std::string data(static_cast<size_t>(length), '\0');
    input.seekg(0);
    if (!input.read(data.data(), length))
        fail("incomplete Lua source: " + c.path);
    std::string_view source(data);
    if (source.starts_with("\xef\xbb\xbf"))
        source.remove_prefix(3);
    if (source.starts_with('#')) {
        auto lf = source.find('\n');
        source = lf == source.npos ? std::string_view("\n") : source.substr(lf);
    }
    if (luaL_loadbufferx(L, source.data(), source.size(), ("@" + c.path).c_str(), "t") != LUA_OK)
        fail(lua_tostring(L, -1));
    if (lua_dump(L, dump_writer, &c.bytecode, 0) != 0)
        fail("could not serialize Lua prototype: " + c.path);
    lua_pop(L, 1);
    return c;
}
} // namespace
Bytes compile_project(const fs::path &entry, const fs::path &source_root,
                      const std::vector<fs::path> &module_roots, std::string symbol, bool exe, bool host) {
    auto main = fs::canonical(entry);
    auto root = source_root.empty() ? main.parent_path() : fs::canonical(source_root);
    auto relative = fs::relative(main, root);
    if (relative.empty() || *relative.begin() == ".." || relative.is_absolute())
        fail("Lua entry must be inside the project root");
    Package p;
    p.host=host;
    p.entry = utf8(relative);
    p.symbol = std::move(symbol);
    if (!identifier(p.symbol))
        fail("use an ASCII module name or pass --symbol luaopen_NAME");
    std::map<std::string, fs::path> files{{p.entry, main}};
    auto add_tree = [&](const fs::path &tree) {
        for (const auto &f : fs::recursive_directory_iterator(tree)) {
            if (f.is_symlink() || !f.is_regular_file() ||
                _wcsicmp(f.path().extension().c_str(), L".lua"))
                continue;
            auto name = utf8(fs::relative(f.path(), tree));
            auto [it, inserted] = files.emplace(name, f.path());
            if (!inserted && !fs::equivalent(it->second, f.path()))
                fail("duplicate bundled Lua path: " + name);
        }
    };
    if (!source_root.empty())
        add_tree(root);
    for (const auto &tree : module_roots)
        add_tree(fs::canonical(tree));
    std::unique_ptr<lua_State, decltype(&lua_close)> state(luaL_newstate(), lua_close);
    if (!state)
        fail("could not initialize Lua compiler");
    p.chunks.push_back(compile_chunk(state.get(),p.entry,main));
    for (const auto &[name, path] : files)
        if(name!=p.entry)p.chunks.push_back(compile_chunk(state.get(), name, path));
    auto image =
        emit_image(exports(p,exe), encode_package(p), lua_profile(exe, host));
    auto restored = read_image(image, false);
    if (restored.chunks.size() != p.chunks.size() || restored.entry != p.entry ||
        restored.symbol != p.symbol)
        fail("compiled project manifest did not round-trip");
    for (size_t i = 0; i < p.chunks.size(); ++i)
        if (restored.chunks[i].path != p.chunks[i].path ||
            restored.chunks[i].bytecode != p.chunks[i].bytecode)
            fail("Lua instructions did not round-trip: " + p.chunks[i].path);
    return image;
}
} // namespace gpt::lua54
