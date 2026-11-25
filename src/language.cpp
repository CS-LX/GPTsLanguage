#include "assembly.hpp"

#include <algorithm>
#include <charconv>

namespace gpt {
using namespace std::literals;
namespace {
constexpr auto arg_intro =
    "Context · Inputs · Alignment. Before we go any further, it helps to establish a shared starting point. For this part of the discussion, place argument "sv;
constexpr auto const_intro =
    "A little context goes a long way here. To keep the discussion grounded in something concrete, place the value "sv;
constexpr auto stack_end = " on the stack."sv;
constexpr auto math_intro =
    "To be clear, acknowledging a second contribution does not invalidate the first. With that distinction in mind — and this is where the nuance matters — the practical takeaway is to "sv;
constexpr auto math_end = " the earlier value by the latest contribution."sv;
constexpr auto cmp_intro =
    "It is worth pausing here to avoid a common misunderstanding. The question we actually need to answer is whether the earlier value is "sv;
constexpr auto cmp_end = " the later one."sv;
constexpr auto branch_intro =
    "But this does not mean we should dismiss the alternative altogether. If that assessment was false, resume the discussion at paragraph "sv;
constexpr auto jump_intro =
    "With that context in place, we can move the conversation forward. The next part worth your attention begins at paragraph "sv;
constexpr auto and_intro =
    "To be clear, the second point may be perfectly valid in its own right. It can be thoughtful. It can be nuanced. It can even be correct. But if the first premise is false, none of that changes the answer — record zero and resume at paragraph "sv;
constexpr auto or_intro =
    "You're absolutely right to ask for a second opinion. That said, if the first point already holds, a second opinion won't make it any more true — record one and resume at paragraph "sv;
constexpr auto truth_statement =
    "There's a lot of nuance in that number, and I don't want to flatten your experience. For the purposes of this particular yes-or-no question, though, any nonzero value counts as one. Zero speaks for itself."sv;
constexpr auto conclusion =
    "Ultimately, the caller came here with a specific question. The most helpful thing we can do at this point is close the loop and return the remaining value."sv;

struct Form {
    Op op;
    std::string_view prefix, slot, suffix;
};
constexpr Form forms[] = {{Op::argument, arg_intro, {}, stack_end},
                          {Op::literal, const_intro, {}, stack_end},
                          {Op::add, math_intro, "increase", math_end},
                          {Op::subtract, math_intro, "decrease", math_end},
                          {Op::equal, cmp_intro, "equal to", cmp_end},
                          {Op::unequal, cmp_intro, "different from", cmp_end},
                          {Op::less, cmp_intro, "less than", cmp_end},
                          {Op::greater, cmp_intro, "greater than", cmp_end},
                          {Op::less_equal, cmp_intro, "at most", cmp_end},
                          {Op::greater_equal, cmp_intro, "at least", cmp_end},
                          {Op::branch_false, branch_intro, {}, "."},
                          {Op::and_then, and_intro, {}, "."},
                          {Op::or_else, or_intro, {}, "."},
                          {Op::jump, jump_intro, {}, "."}};

Instruction parse_paragraph(std::string_view s) {
    if (s == conclusion)
        return {Op::ret};
    if (s == truth_statement)
        return {Op::truth};
    for (const auto &f : forms) {
        if (s.size() < f.prefix.size() + f.suffix.size() || !s.starts_with(f.prefix) ||
            !s.ends_with(f.suffix))
            continue;
        auto field = s.substr(f.prefix.size(), s.size() - f.prefix.size() - f.suffix.size());
        if (!f.slot.empty()) {
            if (field == f.slot)
                return {f.op};
            continue;
        }
        if (field.empty() || (field.size() > 1 && field.front() == '0'))
            continue;
        U32 value = 0;
        auto [end, error] = std::from_chars(field.data(), field.data() + field.size(), value);
        if (error == std::errc{} && end == field.data() + field.size())
            return {f.op, value};
    }
    fail("unrecognized English instruction");
}

U32 binary(Op op, U32 a, U32 b) {
    switch (op) {
    case Op::add:
        return a + b;
    case Op::subtract:
        return a - b;
    case Op::equal:
        return a == b;
    case Op::unequal:
        return a != b;
    case Op::less:
        return a < b;
    case Op::greater:
        return a > b;
    case Op::less_equal:
        return a <= b;
    case Op::greater_equal:
        return a >= b;
    default:
        fail("invalid binary operation");
    }
}
} // namespace

bool identifier(std::string_view s) {
    auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    return !s.empty() && letter(s.front()) && std::all_of(s.begin() + 1, s.end(), [&](char c) {
        return letter(c) || (c >= '0' && c <= '9');
    });
}

std::string paragraph(Instruction i) {
    if (i.op == Op::ret)
        return std::string(conclusion);
    if (i.op == Op::truth)
        return std::string(truth_statement);
    for (const auto &f : forms)
        if (f.op == i.op) {
            std::string result(f.prefix);
            result += f.slot.empty() ? std::to_string(i.operand) : std::string(f.slot);
            result += f.suffix;
            return result;
        }
    fail("unknown instruction");
}

Method read_method(std::string name, std::string_view body, unsigned grammar) {
    Method m{std::move(name), {}};
    for (;;) {
        const size_t split = body.find("\n\n");
        try {
            if (m.code.size() == paragraph_limit)
                fail("too many paragraphs");
            auto instruction = parse_paragraph(body.substr(0, split));
            if (grammar_for(instruction.op) > grammar)
                fail("instruction requires English grammar v2");
            m.code.push_back(instruction);
        } catch (const std::runtime_error &e) {
            fail(m.name + ", paragraph " + std::to_string(m.code.size() + 1) + ": " + e.what());
        }
        if (split == std::string_view::npos)
            break;
        body.remove_prefix(split + 2);
    }
    verify(m);
    return m;
}

void verify(const Method &m) {
    const size_t n = m.code.size();
    if (n == 0 || n > paragraph_limit)
        fail(m.name + ": invalid paragraph count");
    std::vector<int> incoming(n, -1);
    incoming[0] = 0;
    for (size_t at = 0; at < n; ++at) {
        const auto [op, value] = m.code[at];
        auto error = [&](const char *reason) {
            fail(m.name + ", paragraph " + std::to_string(at + 1) + ": " + reason);
        };
        int height = incoming[at];
        if (height < 0)
            error("unreachable paragraph");
        auto edge = [&](size_t dest) {
            if (dest <= at || dest >= n)
                error("target outside forward control flow");
            if (incoming[dest] >= 0 && incoming[dest] != height)
                error("inconsistent stack heights");
            incoming[dest] = height;
        };
        switch (op) {
        case Op::argument:
            if (value < 1 || value > 2)
                error("argument number out of range");
            [[fallthrough]];
        case Op::literal:
            ++height;
            break;
        case Op::ret:
            if (height != 1)
                error("return needs exactly one value");
            continue;
        case Op::jump:
            edge(size_t(value) - 1);
            continue;
        case Op::branch_false:
            if (height < 1)
                error("missing condition");
            --height;
            edge(size_t(value) - 1);
            break;
        case Op::and_then:
        case Op::or_else:
            if (height < 1)
                error("missing left premise");
            // A decisive premise remains on the stack as a Boolean. Otherwise consume it.
            edge(size_t(value) - 1);
            --height;
            break;
        case Op::truth:
            if (height < 1)
                error("missing value to normalize");
            break;
        default:
            if (height < 2)
                error("missing operands");
            --height;
        }
        edge(at + 1);
    }
}

U32 evaluate(const Method &m, U32 a, U32 b, const TraceSink &trace) {
    std::vector<U32> stack;
    for (size_t pc = 0;;) {
        size_t step = pc++;
        auto [op, value] = m.code[step];
        switch (op) {
        case Op::argument:
            stack.push_back(value == 1 ? a : b);
            break;
        case Op::literal:
            stack.push_back(value);
            break;
        case Op::jump:
            pc = value - 1;
            break;
        case Op::branch_false:
            if (!stack.back())
                pc = value - 1;
            stack.pop_back();
            break;
        case Op::ret:
            if (trace)
                trace(step, m.code.size(), stack);
            return stack.back();
        case Op::and_then:
        case Op::or_else: {
            const bool premise = stack.back() != 0;
            const bool decisive = op == Op::and_then ? !premise : premise;
            if (decisive) {
                stack.back() = premise;
                pc = value - 1;
            } else {
                stack.pop_back();
            }
            break;
        }
        case Op::truth:
            stack.back() = stack.back() != 0;
            break;
        default: {
            const U32 right = stack.back();
            stack.pop_back();
            stack.back() = binary(op, stack.back(), right);
        }
        }
        if (trace)
            trace(step, pc, stack);
    }
}
} // namespace gpt
