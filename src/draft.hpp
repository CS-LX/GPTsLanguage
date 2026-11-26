#pragma once
#include "lua_prose.hpp"
#include "lopcodes.h"
#include <map>
#include <variant>

namespace gpt::draft {
using Literal = std::variant<std::monostate, bool, int64_t, double, std::string>;
struct Clause {
    OpCode op{};
    unsigned a = 0, b = 0, c = 0, k = 0;
    int wide = 0; // Signed for displacements and signed immediates.
};
Clause unpack(U32 word);
U32 pack(const Clause &i);
struct Local {
    std::string name;
    size_t first, last;
};
struct Capture {
    unsigned local, index;
};
struct Proto {
    size_t offset = 0;
    unsigned params = 0, stack = 0;
    std::vector<Clause> code;
    std::vector<Literal> constants;
    std::vector<Capture> captures;
    std::vector<Proto> children;
    std::vector<Local> locals;
    std::vector<std::string> capture_names;
};
struct Program {
    std::string entry, symbol;
    U32 grammar = 0;
    std::map<std::string, Proto> files;
};
Program load(View image, bool mapped);
struct Passage {
    std::string text;
    size_t count;
    bool boundary = false;
};
Passage feedback(const Proto &p, size_t pc);
size_t read_feedback(Proto &p, size_t pc, std::string_view &text);
std::string sentence(const Proto &p, size_t pc);
Clause read_sentence(const Proto &p, size_t pc, std::string_view &text);
int run(const Program &program, const std::vector<std::string> &argv);
} // namespace gpt::draft
