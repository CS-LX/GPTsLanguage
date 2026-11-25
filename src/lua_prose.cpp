#include "lua_prose.hpp"
#include "lopcodes.h"
#include <charconv>
#include <unordered_map>

namespace gpt::lua54 {
namespace {
struct Verb {
    OpMode mode;
    std::string_view action;
};
constexpr Verb verbs[] = {
    {iABC, "copy the value between registers"},                               // MOVE
    {iAsBx, "load the signed integer into the destination"},                  // LOADI
    {iAsBx, "represent the signed immediate as a floating-point number"},     // LOADF
    {iABx, "retrieve the indexed constant"},                                  // LOADK
    {iABx, "retrieve the constant indexed by the following paragraph"},       // LOADKX
    {iABC, "record a definite false"},                                        // LOADFALSE
    {iABC, "record false and skip the following paragraph"},                  // LFALSESKIP
    {iABC, "record a definite true"},                                         // LOADTRUE
    {iABC, "clear the specified range of registers to nil"},                  // LOADNIL
    {iABC, "read the captured variable"},                                     // GETUPVAL
    {iABC, "update the captured variable"},                                   // SETUPVAL
    {iABC, "look up a named field in a captured table"},                      // GETTABUP
    {iABC, "look up the table key held in a register"},                       // GETTABLE
    {iABC, "look up the immediate integer key"},                              // GETI
    {iABC, "look up a field by its constant name"},                           // GETFIELD
    {iABC, "assign a field of a captured table"},                             // SETTABUP
    {iABC, "assign a table entry using a register key"},                      // SETTABLE
    {iABC, "assign a table entry using an immediate integer key"},            // SETI
    {iABC, "assign a field by its constant name"},                            // SETFIELD
    {iABC, "allocate a fresh table with the stated size hints"},              // NEWTABLE
    {iABC, "retrieve the method and preserve its receiver"},                  // SELF
    {iABC, "increase the register value by a signed immediate"},              // ADDI
    {iABC, "increase the register value by the constant"},                    // ADDK
    {iABC, "decrease the register value by the constant"},                    // SUBK
    {iABC, "multiply the register value by the constant"},                    // MULK
    {iABC, "take the remainder against the constant"},                        // MODK
    {iABC, "raise the register value to the constant power"},                 // POWK
    {iABC, "divide the register value by the constant"},                      // DIVK
    {iABC, "floor the quotient against the constant"},                        // IDIVK
    {iABC, "take the bitwise intersection with the constant"},                // BANDK
    {iABC, "take the bitwise union with the constant"},                       // BORK
    {iABC, "take the bitwise exclusive union with the constant"},             // BXORK
    {iABC, "shift the register right by the signed immediate"},               // SHRI
    {iABC, "shift the signed immediate left by the register value"},          // SHLI
    {iABC, "increase the left operand by the right operand"},                 // ADD
    {iABC, "decrease the left operand by the right operand"},                 // SUB
    {iABC, "multiply the two register values"},                               // MUL
    {iABC, "take the remainder of the two register values"},                  // MOD
    {iABC, "raise the left operand to the right operand"},                    // POW
    {iABC, "divide the left operand by the right operand"},                   // DIV
    {iABC, "floor the quotient of the two register values"},                  // IDIV
    {iABC, "take the bitwise intersection of the registers"},                 // BAND
    {iABC, "take the bitwise union of the registers"},                        // BOR
    {iABC, "take the bitwise exclusive union of the registers"},              // BXOR
    {iABC, "shift the left operand to the left"},                             // SHL
    {iABC, "shift the left operand to the right"},                            // SHR
    {iABC, "consult the binary metamethod for two registers"},                // MMBIN
    {iABC, "consult the binary metamethod for an immediate"},                 // MMBINI
    {iABC, "consult the binary metamethod for a constant"},                   // MMBINK
    {iABC, "negate the numeric value"},                                       // UNM
    {iABC, "complement every bit in the integer"},                            // BNOT
    {iABC, "invert the truth value"},                                         // NOT
    {iABC, "measure the length according to Lua"},                            // LEN
    {iABC, "concatenate the consecutive register values"},                    // CONCAT
    {iABC, "close the outstanding variables at this level"},                  // CLOSE
    {iABC, "mark the variable for closing when its scope ends"},              // TBC
    {isJ, "continue at the signed displacement"},                             // JMP
    {iABC, "compare the register values for equality"},                       // EQ
    {iABC, "test whether the left register is smaller"},                      // LT
    {iABC, "test whether the left register is at most the right"},            // LE
    {iABC, "compare the register with a constant for equality"},              // EQK
    {iABC, "compare the register with an immediate for equality"},            // EQI
    {iABC, "test whether the register is below the immediate"},               // LTI
    {iABC, "test whether the register is at most the immediate"},             // LEI
    {iABC, "test whether the register exceeds the immediate"},                // GTI
    {iABC, "test whether the register is at least the immediate"},            // GEI
    {iABC, "test the register against the requested truth value"},            // TEST
    {iABC, "copy the register when the truth test agrees"},                   // TESTSET
    {iABC, "call the function with the indicated argument and result spans"}, // CALL
    {iABC, "tail-call the function in the current frame"},                    // TAILCALL
    {iABC, "return the indicated span of results"},                           // RETURN
    {iABC, "return with no results"},                                         // RETURN0
    {iABC, "return exactly one result"},                                      // RETURN1
    {iABx, "advance the numeric loop and test its limit"},                    // FORLOOP
    {iABx, "prepare the numeric loop and check its initial bounds"},          // FORPREP
    {iABx, "prepare the generic iterator and its closing variable"},          // TFORPREP
    {iABC, "call the generic iterator for the next values"},                  // TFORCALL
    {iABx, "continue the generic loop when the iterator returns a key"},      // TFORLOOP
    {iABC, "write consecutive values into the table array"},                  // SETLIST
    {iABx, "create a closure from the indexed child function"},               // CLOSURE
    {iABC, "collect the requested variable arguments"},                       // VARARG
    {iABC, "prepare the frame for variable arguments"},                       // VARARGPREP
    {iAx, "extend the preceding paragraph with a wide operand"},              // EXTRAARG
};
static_assert(std::size(verbs) == NUM_OPCODES);
constexpr std::string_view openings[]{
    "Absolutely. Let's unpack this carefully — we need to ",
    "To be clear, there is a subtle but important distinction here — we should ",
    "You're right to flag the nuance. Before we get ahead of ourselves, let's ",
    "Great question. The most straightforward way to honor that intent is to "};
constexpr std::string_view detail = ". For transparency, the operand ledger reads ";
constexpr std::string_view endings[]{". Taken together, that gives us a clear next step.",
                                     ". In short, the details above are doing the heavy lifting.",
                                     ". And yes — that last detail is intentional.",
                                     ". The takeaway? We can now move forward with confidence."};
constexpr std::string_view dot = " · ";
bool eat(std::string_view &s, std::string_view token) {
    if (!s.starts_with(token))
        return false;
    s.remove_prefix(token.size());
    return true;
}
int take(std::string_view &s, std::string_view label, int lo, int hi) {
    int n;
    if (!eat(s, label))
        fail("missing GPT operand");
    auto [end, err] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (err != std::errc{} || n < lo || n > hi)
        fail("invalid GPT operand");
    s.remove_prefix(end - s.data());
    return n;
}
} // namespace
std::string narrate(U32 w) {
    unsigned op = w & 127;
    if (op >= std::size(verbs))
        fail("unknown Lua opcode");
    auto [mode, verb] = verbs[op];
    unsigned a = (w >> 7) & 255, style = a % 4;
    std::string s(openings[style]);
    s += verb;
    s += detail;
    auto operand = [&](std::string_view label, int value) {
        s += label;
        s += std::to_string(value);
    };
    if (mode == iAx)
        operand("extension ", w >> 7);
    else if (mode == isJ)
        operand("jump ", int(w >> 7) - 16777215);
    else {
        operand("A ", a);
        s += dot;
        if (mode == iABC) {
            operand("B ", (w >> 16) & 255);
            s += dot;
            operand("C ", w >> 24);
            s += dot;
            operand("selector ", (w >> 15) & 1);
        } else if (mode == iAsBx)
            operand("signed extent ", int(w >> 15) - 65535);
        else
            operand("extent ", w >> 15);
    }
    s += endings[style];
    return s;
}
U32 assemble(std::string_view s) {
    size_t tone = 0;
    while (tone < std::size(openings) && !eat(s, openings[tone]))
        ++tone;
    auto split = s.find(detail);
    if (tone == std::size(openings) || split == s.npos)
        fail("unrecognized GPT paragraph");
    static const auto dictionary = [] {
        std::unordered_map<std::string_view, U32> map;
        for (U32 i = 0; i < std::size(verbs); ++i)
            map.emplace(verbs[i].action, i);
        return map;
    }();
    auto it = dictionary.find(s.substr(0, split));
    if (it == dictionary.end())
        fail("unrecognized GPT action: " + std::string(s.substr(0, split)));
    U32 op = it->second, word = op;
    s.remove_prefix(split + detail.size());
    auto mode = verbs[op].mode;
    if (mode == iAx)
        word |= U32(take(s, "extension ", 0, 33554431)) << 7;
    else if (mode == isJ)
        word |= U32(take(s, "jump ", -16777215, 16777216) + 16777215) << 7;
    else {
        word |= U32(take(s, "A ", 0, 255)) << 7;
        if (!eat(s, dot))
            fail("missing GPT middle dot");
        if (mode == iABC) {
            word |= U32(take(s, "B ", 0, 255)) << 16;
            if (!eat(s, dot))
                fail("missing GPT middle dot");
            word |= U32(take(s, "C ", 0, 255)) << 24;
            if (!eat(s, dot))
                fail("missing GPT middle dot");
            word |= U32(take(s, "selector ", 0, 1)) << 15;
        } else if (mode == iAsBx)
            word |= U32(take(s, "signed extent ", -65535, 65536) + 65535) << 15;
        else
            word |= U32(take(s, "extent ", 0, 131071)) << 15;
    }
    if (s != endings[tone])
        fail("unexpected GPT paragraph summary");
    return word;
}
} // namespace gpt::lua54
