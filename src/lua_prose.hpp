#pragma once
#include "assembly.hpp"
#include <filesystem>
namespace gpt::lua54 {
inline constexpr std::string_view signature = "GPTLUA54";
inline constexpr size_t limit = 256u << 20;
struct Chunk {
    std::string path;
    Bytes bytecode; // Exists only in memory, for the stock VM.
    size_t paragraphs = 0;
    size_t functions = 0;
};
struct Package {
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
                      bool exe);
inline Module exports(const std::string &symbol) { return {{{symbol, {}}}, 1}; }
} // namespace gpt::lua54
