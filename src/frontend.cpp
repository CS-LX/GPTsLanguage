#include "assembly.hpp"

#include <algorithm>
#include <charconv>
#include <optional>
#include <set>

namespace gpt {
namespace {
struct Token {
    std::string_view text;
    size_t line, column;
};
[[noreturn]] void syntax(Token t, std::string_view why) {
    fail("line " + std::to_string(t.line) + ", column " + std::to_string(t.column) + ": " +
         std::string(why));
}
bool letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool digit(char c) {
    return c >= '0' && c <= '9';
}

class Lexer {
    std::string_view src;
    size_t index = 0, line = 1, column = 1;
    void advance() {
        if (src[index++] == '\n') {
            ++line;
            column = 1;
        } else
            ++column;
    }

  public:
    explicit Lexer(std::string_view s) : src(s) {}
    Token next() {
        while (index < src.size()) {
            char c = src[index];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                advance();
                continue;
            }
            if (src.substr(index).starts_with("//")) {
                while (index < src.size() && src[index] != '\n')
                    advance();
                continue;
            }
            if (src.substr(index).starts_with("/*")) {
                Token start{{}, line, column};
                advance();
                advance();
                while (index < src.size() && !src.substr(index).starts_with("*/"))
                    advance();
                if (index == src.size())
                    syntax(start, "unterminated comment");
                advance();
                advance();
                continue;
            }
            break;
        }
        size_t start = index;
        Token t{{}, line, column};
        if (index == src.size())
            return t;
        char c = src[index];
        advance();
        if (letter(c) || digit(c)) {
            while (index < src.size() && (letter(src[index]) || digit(src[index])))
                advance();
        } else if ((c == '<' || c == '>' || c == '=' || c == '!') && index < src.size() &&
                   src[index] == '=')
            advance();
        else if ((c == '&' || c == '|') && index < src.size() && src[index] == c)
            advance();
        else if (c == '"' && src.substr(index).starts_with("C\"")) {
            advance();
            advance();
        }
        t.text = src.substr(start, index - start);
        return t;
    }
};
struct Operator {
    std::string_view text;
    int rank;
    Op op;
};
enum Precedence { logical_or = 1, logical_and, equality, ordering, addition };
constexpr Operator operators[] = {{"||", logical_or, Op::or_else}, {"&&", logical_and, Op::and_then},
                                  {"==", equality, Op::equal}, {"!=", equality, Op::unequal},
                                  {"<", ordering, Op::less}, {">", ordering, Op::greater},
                                  {"<=", ordering, Op::less_equal}, {">=", ordering, Op::greater_equal},
                                  {"+", addition, Op::add}, {"-", addition, Op::subtract}};
class Parser {
    Lexer input;
    Token token;
    Method method;
    std::array<std::string, 2> params;
    int depth = 0;
    unsigned grammar = 1;
    struct Depth {
        int &depth;
        Depth(int &d, Token at) : depth(d) {
            if (++depth > 256)
                syntax(at, "source nesting limit exceeded");
        }
        ~Depth() {
            --depth;
        }
    };
    bool take(std::string_view text) {
        if (token.text != text)
            return false;
        token = input.next();
        return true;
    }
    void want(std::string_view text) {
        if (!take(text))
            syntax(token, "expected '" + std::string(text) + "'");
    }
    std::string name() {
        if (!identifier(token.text))
            syntax(token, "expected an identifier");
        constexpr std::string_view reserved[] = {"if",  "else",   "return",  "unsigned",
                                                 "int", "extern", "uint32_t"};
        if (std::ranges::find(reserved, token.text) != std::end(reserved))
            syntax(token, "reserved identifier");
        std::string n(token.text);
        token = input.next();
        return n;
    }
    void type() {
        if (take("uint32_t"))
            return;
        if (take("unsigned")) {
            want("int");
            return;
        }
        syntax(token, "expected 32-bit unsigned type (uint32_t or unsigned int)");
    }
    size_t emit(Op op, U32 n = 0) {
        if (method.code.size() >= paragraph_limit)
            syntax(token, "too many instructions");
        grammar = std::max(grammar, grammar_for(op));
        method.code.push_back({op, n});
        return method.code.size() - 1;
    }
    bool atom() {
        if (take("!")) {
            Depth guard(depth, token);
            atom();
            emit(Op::literal, 0);
            emit(Op::equal);
            return false;
        }
        if (take("(")) {
            bool u = expression(1);
            want(")");
            return u;
        }
        auto text = token.text;
        if (!text.empty() && digit(text.front())) {
            bool u = text.back() == 'u' || text.back() == 'U';
            if (u)
                text.remove_suffix(1);
            U32 n = 0;
            auto [end, err] = std::from_chars(text.data(), text.data() + text.size(), n);
            if (err != std::errc{} || end != text.data() + text.size() ||
                (text.size() > 1 && text.front() == '0'))
                syntax(token, "expected a decimal u32 literal");
            if (!u && n > INT32_MAX)
                syntax(token, "use the u suffix for a literal above INT32_MAX");
            emit(Op::literal, n);
            token = input.next();
            return u;
        }
        for (U32 i = 0; i < 2; ++i)
            if (take(params[i])) {
                emit(Op::argument, i + 1);
                return true;
            }
        syntax(token, "expected a parameter, decimal literal or parenthesized expression");
    }
    // Result signedness models C integer promotions; comparisons return a signed 0/1.
    bool expression(int minimum) {
        Depth guard(depth, token);
        bool left_unsigned = atom();
        for (;;) {
            const Operator *binary = nullptr;
            for (const auto &op : operators)
                if (op.text == token.text && op.rank >= minimum) {
                    binary = &op;
                    break;
                }
            if (!binary)
                return left_unsigned;
            token = input.next();
            if (short_circuit(binary->op)) {
                const size_t test = emit(binary->op);
                expression(binary->rank + 1);
                emit(Op::truth);
                method.code[test].operand = static_cast<U32>(method.code.size() + 1);
                left_unsigned = false;
                continue;
            }
            size_t right_begin = method.code.size();
            bool right_unsigned = expression(binary->rank + 1);
            bool unsigned_inputs = left_unsigned || right_unsigned;
            if (binary->rank == ordering && !unsigned_inputs) {
                // Bias both signed values before an unsigned ordering comparison.
                // Right-hand gates and their absolute destinations move together.
                for (size_t i = right_begin; i < method.code.size(); ++i)
                    if (short_circuit(method.code[i].op))
                        method.code[i].operand += 2;
                method.code.insert(method.code.begin() + right_begin,
                                   {{Op::literal, 0x80000000u}, {Op::add}});
                emit(Op::literal, 0x80000000u);
                emit(Op::add);
            }
            emit(binary->op);
            left_unsigned = binary->rank == addition && unsigned_inputs;
        }
    }
    // True when control can reach the statement immediately following this one.
    bool statement() {
        Depth guard(depth, token);
        if (take("{")) {
            bool falls = true;
            while (!take("}")) {
                if (token.text.empty())
                    syntax(token, "unclosed block");
                if (!falls)
                    syntax(token, "unreachable statement outside supported subset");
                falls = statement();
            }
            return falls;
        }
        if (take("return")) {
            expression(1);
            want(";");
            emit(Op::ret);
            return false;
        }
        if (take("if")) {
            want("(");
            expression(1);
            want(")");
            size_t alternative = emit(Op::branch_false);
            bool true_falls = statement();
            bool has_else = take("else");
            std::optional<size_t> past_else;
            if (has_else && true_falls)
                past_else = emit(Op::jump);
            method.code[alternative].operand = static_cast<U32>(method.code.size() + 1);
            bool false_falls = !has_else || statement();
            if (past_else)
                method.code[*past_else].operand = static_cast<U32>(method.code.size() + 1);
            return true_falls || false_falls;
        }
        syntax(token, "supported statements are a block, if/else and return");
    }
    void include_directive() {
        size_t line = token.line;
        for (auto word : {"#", "include", "<", "stdint", ".", "h", ">"}) {
            if (token.line != line || !take(word))
                syntax(token, "only #include <stdint.h> is supported");
        }
        if (!token.text.empty() && token.line == line)
            syntax(token, "unexpected text after include");
    }

  public:
    explicit Parser(std::string_view source) : input(source), token(input.next()) {}
    Module parse() {
        Module result;
        std::set<std::string> names;
        while (!token.text.empty()) {
            if (token.text == "#") {
                include_directive();
                continue;
            }
            if (take("extern"))
                want("\"C\"");
            type();
            method = {name(), {}};
            if (!names.insert(method.name).second)
                syntax(token, "duplicate function name");
            want("(");
            type();
            params[0] = name();
            want(",");
            type();
            params[1] = name();
            want(")");
            if (params[0] == params[1])
                syntax(token, "duplicate parameter name");
            if (token.text != "{")
                syntax(token, "expected a function body");
            if (statement())
                syntax(token, "every path must return a value");
            verify(method);
            result.methods.push_back(std::move(method));
            if (result.methods.size() > method_limit)
                syntax(token, "too many methods");
        }
        if (result.methods.empty())
            syntax(token, "expected at least one function");
        result.grammar = grammar;
        return result;
    }
};
} // namespace
Module compile(std::string_view source) {
    if (source.starts_with("\xEF\xBB\xBF"))
        source.remove_prefix(3);
    return Parser(source).parse();
}
} // namespace gpt
