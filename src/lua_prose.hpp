#pragma once
#include "assembly.hpp"
#include <filesystem>
namespace gpt::lua54 {
inline constexpr std::string_view signature = "GPTLUA54";
inline constexpr size_t limit = 256u << 20;
struct Chunk {
    std::string path;
    Bytes bytecode; // Exists only in memory, for the stock VM.
    size_t operations = 0;
    size_t functions = 0;
};
struct Package {
    U32 grammar = 2;
    bool host = false;
    std::string entry, symbol;
    std::vector<Chunk> chunks;
};
std::string narrate(U32 word);
U32 assemble(std::string_view paragraph);
Payload encode_package(const Package &package);
Package decode_package(Sections sections);
Package read_image(View image, bool mapped, Sections *sections = nullptr);
Bytes compile_project(const std::filesystem::path &entry, const std::filesystem::path &root,
                      const std::vector<std::filesystem::path> &module_roots, std::string symbol,
                      bool exe, bool host = false);
inline Module exports(const Package &p, bool exe) {
    Module m{{{p.symbol, {}}}, 1};
    if (p.grammar >= 2 && !exe && !p.host) {
        if (p.symbol == "gpt_run")
            fail("gpt_run is reserved for the native VM entry");
        m.methods.push_back({"gpt_run", {}});
    }
    return m;
}
} // namespace gpt::lua54
