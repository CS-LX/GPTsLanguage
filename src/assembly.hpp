#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace gpt {
using U32 = std::uint32_t;
using Bytes = std::vector<std::uint8_t>;
using View = std::span<const std::uint8_t>;

inline constexpr std::string_view magic = "GPTASM\r\n";
inline constexpr size_t section_limit = 16 * 1024 * 1024;
inline constexpr size_t method_limit = 1024;
inline constexpr size_t paragraph_limit = 65535;

[[noreturn]] inline void fail(const std::string &message) {
    throw std::runtime_error(message);
}
bool identifier(std::string_view name);
enum class Op {
    argument,
    literal,
    add,
    subtract,
    equal,
    unequal,
    less,
    greater,
    less_equal,
    greater_equal,
    branch_false,
    jump,
    and_then,
    or_else,
    truth,
    ret
};
inline bool short_circuit(Op op) {
    return op == Op::and_then || op == Op::or_else;
}
inline unsigned grammar_for(Op op) {
    return short_circuit(op) || op == Op::truth ? 2 : 1;
}
struct Instruction {
    Op op;
    U32 operand = 0;
};
struct Method {
    std::string name;
    std::vector<Instruction> code;
};
struct Module {
    std::vector<Method> methods;
    unsigned grammar = 1;
};
struct Payload {
    Bytes metadata;
    Bytes prose;
};
struct Sections {
    View metadata;
    View prose;
};
std::string paragraph(Instruction instruction);
void verify(const Method &method);
// Zero-based executed and next PCs; next == code.size() represents a return.
using TraceSink = std::function<void(size_t, size_t, std::span<const U32>)>;
U32 evaluate(const Method &method, U32 a, U32 b, const TraceSink &trace = {});
Payload encode(const Module &module);
Module decode(Sections sections, std::vector<std::string_view> *bodies = nullptr);
Module read_dll(View image, bool mapped, std::vector<std::string_view> *bodies = nullptr);
Module compile(std::string_view source);
Bytes emit_dll(const Module &module);
enum class PeProfile { integer_dll, lua_dll, lua_exe };
inline std::string_view runtime_library(PeProfile profile) {
    return profile == PeProfile::integer_dll ? "gptrt.dll" : "gptlua54.dll";
}
inline std::string_view runtime_entry(PeProfile profile) {
    if (profile == PeProfile::lua_exe) return "gpt_lua_start";
    if (profile == PeProfile::lua_dll) return "gpt_lua_module";
    return "gpt_dispatch";
}
Bytes emit_image(const Module &exports, Payload payload, PeProfile profile);
Sections image_sections(View image, bool mapped, bool &is_exe);
void verify_image(View image, bool mapped, const Module &exports, PeProfile profile);
} // namespace gpt
