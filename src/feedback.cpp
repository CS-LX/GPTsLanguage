#include "draft.hpp"
#include <algorithm>
#include <charconv>
#include <array>

namespace gpt::draft {
namespace {
struct Rule {
    OpCode op;
    std::string_view text;
};
constexpr Rule rules[]{
#include "feedback_rules.inc"
};
static_assert(std::size(rules) == NUM_OPCODES);
static_assert([] {
    for (size_t i = 0; i < std::size(rules); ++i)
        if (size_t(rules[i].op) != i)
            return false;
    return true;
}());
constexpr Rule idioms[]{
    {OP_ADDI, "I'd count that as one more for %s. It may feel like a small change, but it belongs in "
              "the total all the same."},
    {OP_ADDI, "I'd count that as one less for %s. It may feel like a small change, but it belongs in "
              "the total all the same."},
    {OP_ADDI, "I'd count that as no gain for %s. It may feel like a small change, but it belongs in the "
              "total all the same."},
    {OP_EXTRAARG, "That should leave us just enough room without overthinking the layout."},
    {OP_VARARGPREP, "Absolutely. Let's take your question on its own terms first; we can bring in the "
                    "wider context as it becomes relevant."},
    {OP_GETTABUP, "On %D, the earlier brief already gives us a starting point. I'd use that for %s."},
    {OP_CALL, "I'd hear %a out on this one and let its answer speak for itself."},
};
OpMode mode(OpCode op) {
    switch (op) {
    case OP_LOADI:
    case OP_LOADF:
        return iAsBx;
    case OP_LOADK:
    case OP_LOADKX:
    case OP_FORLOOP:
    case OP_FORPREP:
    case OP_TFORPREP:
    case OP_TFORLOOP:
    case OP_CLOSURE:
        return iABx;
    case OP_EXTRAARG:
        return iAx;
    case OP_JMP:
        return isJ;
    default:
        return iABC;
    }
}
constexpr std::string_view low[]{"zero",     "one",      "two",      "three",   "four",    "five",
                                 "six",      "seven",    "eight",    "nine",    "ten",     "eleven",
                                 "twelve",   "thirteen", "fourteen", "fifteen", "sixteen", "seventeen",
                                 "eighteen", "nineteen", "twenty"};
constexpr std::string_view roles[]{
    "the working answer",   "your main point",        "my first impression",    "the running total",
    "the next question",    "the proposed reading",   "the supporting detail",  "the example at hand",
    "the revised estimate", "the remaining question", "the wider context",      "the second opinion",
    "the current proposal", "the earlier suggestion", "the provisional answer", "the last detail"};
constexpr std::string_view events[]{"the lookup",         "the revision",         "the collection",
                                    "the mode",           "the length",           "agreement",
                                    "addition",           "subtraction",          "multiplication",
                                    "the remainder",      "the exponent",         "the quotient",
                                    "floor division",     "the shared marks",     "the combined marks",
                                    "the disputed marks", "the left shift",       "the right shift",
                                    "negation",           "the complement",       "ordering",
                                    "the upper bound",    "the combined wording", "the invitation",
                                    "the conclusion"};
std::string numeral(int n) {
    if (n < 0)
        return "minus " + numeral(-n);
    return n < int(std::size(low)) ? std::string(low[n]) : std::to_string(n);
}
bool eat(std::string_view &s, std::string_view prefix) {
    if (!s.starts_with(prefix))
        return false;
    s.remove_prefix(prefix.size());
    return true;
}
bool number(std::string_view &s, int &n, int lo, int hi) {
    bool neg = eat(s, "minus ");
    if (!s.empty() && s.front() >= '0' && s.front() <= '9') {
        auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
        if (ec != std::errc{})
            return false;
        s.remove_prefix(p - s.data());
    } else {
        int i = int(std::size(low));
        while (--i >= 0 && !eat(s, low[i])) {
        }
        if (i < 0)
            return false;
        n = i;
    }
    if (neg)
        n = -n;
    return n >= lo && n <= hi;
}
std::string quote(std::string_view t) {
    std::string s = "“";
    for (unsigned char c : t) {
        if (c < 32 || c == '\\' || c >= 127) {
            constexpr char digits[] = "0123456789abcdef";
            s += "\\x";
            s += digits[c >> 4];
            s += digits[c & 15];
        } else
            s += char(c);
    }
    return s + "”";
}
bool writes(OpCode o) {
    return (o <= OP_GETFIELD && o != OP_SETUPVAL) || (o >= OP_NEWTABLE && o <= OP_SHR) ||
           (o >= OP_UNM && o <= OP_CONCAT) || o == OP_CLOSURE;
}
struct Context {
    const Proto &p;
    size_t pc;
    std::vector<const std::string *> locals;
    Context(const Proto &fn, size_t at) : p(fn), pc(at) {
        for (auto &l : p.locals)
            if (l.first <= pc && pc < l.last)
                locals.push_back(&l.name);
    }
    std::string slot(unsigned n, bool pronoun) const {
        if (pronoun && pc && writes(p.code[pc - 1].op) && p.code[pc - 1].a == n)
            return "that last point";
        if (n < locals.size() && !locals[n]->empty() && locals[n]->front() != '(' &&
            std::count_if(locals.begin(), locals.end(), [&](auto l) { return *l == *locals[n]; }) == 1) {
            const auto &name = *locals[n];
            if (name.starts_with("n_") && name.size() > 2)
                return "your count of " + quote(name.substr(2));
            return "your " + quote(name);
        }
        if (n < std::size(roles))
            return std::string(roles[n]);
        return "working note " + numeral(n + 1);
    }
    std::string constant(unsigned n) const {
        if (n < 64 && n < p.constants.size() &&
            std::find(p.constants.begin(), p.constants.begin() + n, p.constants[n]) ==
                p.constants.begin() + n) {
            const auto &v = p.constants[n];
            if (auto s = std::get_if<std::string>(&v); s && s->size() <= 64)
                return quote(*s);
            if (auto i = std::get_if<int64_t>(&v); i && *i >= -1000000 && *i <= 1000000)
                return "exactly " + numeral(int(*i));
        }
        return "the excerpt numbered " + numeral(n + 1);
    }
    std::string capture(unsigned n) const {
        if (n < p.capture_names.size() && p.capture_names[n] == "_ENV")
            return "our shared brief";
        return "earlier point " + numeral(n + 1);
    }
};
std::string token(char f, const Clause &i, const Context &ctx) {
    auto n = [&](int v) { return numeral(v); };
    switch (f) {
    case 'a':
        return ctx.slot(i.a, true);
    case 's':
        return ctx.slot(i.a, false);
    case 'b':
        return ctx.slot(i.b, true);
    case 'c':
        return ctx.slot(i.c, true);
    case 'U':
        return ctx.capture(i.b);
    case 'V':
        return ctx.capture(i.a);
    case 'D':
        return ctx.constant(i.c);
    case 'E':
        return ctx.constant(i.b);
    case 'W':
        return ctx.constant(i.wide);
    case 'M':
        return i.k ? ctx.constant(i.c) : ctx.slot(i.c, true);
    case 'q':
        return i.k ? "holds" : "falls apart";
    case 'P':
        return i.k ? "in reverse order" : "in that order";
    case 'f':
        return i.k ? ", with the outstanding details taken into account" : "";
    case 'h':
        return i.c ? "fractional" : "whole";
    case 'H':
        return i.c ? " on the fractional reading" : "";
    case 'G':
        return i.c < std::size(events) ? std::string(events[i.c]) : "reading " + n(i.c);
    case 'A':
        return n(i.a);
    case 'B':
        return n(i.b);
    case 'C':
        return n(i.c);
    case 'N':
        return !i.b       ? ""
               : i.b == 1 ? ", along with the point immediately after it"
                          : ", along with the next " + n(i.b) + " points";
    case 'i':
        return n(int(i.b) - 127);
    case 'j':
        return n(int(i.c) - 127);
    case 'X':
        return n(i.wide);
    case 'T':
        return n(int(ctx.pc) + 2 + i.wide);
    case 'o':
    case 'r': {
        unsigned v = f == 'o' ? i.b : i.c;
        if (!v)
            return "everything that's available";
        return v == 1 ? "nothing further" : v == 2 ? "one contribution" : n(v - 1) + " contributions";
    }
    default:
        fail("invalid feedback template");
    }
}
bool parse_token(char f, Clause &i, const Context &ctx, std::string_view &text) {
    int v = 0;
    auto natural = [&](int lo, int hi) { return number(text, v, lo, hi); };
    auto slot = [&](unsigned &out, bool pronoun) {
        for (unsigned n = 0; n < std::max<unsigned>(ctx.p.stack, 1); ++n) {
            auto word = ctx.slot(n, pronoun);
            if (text.starts_with(word) &&
                (text.size() == word.size() || !std::isalnum((unsigned char)text[word.size()]))) {
                text.remove_prefix(word.size());
                out = n;
                return true;
            }
        }
        // The compiler can refer to the position just after the last live value on return.
        if (eat(text, "working note ") && natural(1, 256)) {
            out = v - 1;
            return true;
        }
        for (unsigned n = ctx.p.stack; n < std::size(roles); ++n)
            if (eat(text, roles[n])) {
                out = n;
                return true;
            }
        return false;
    };
    auto constant = [&](unsigned &out) {
        if (eat(text, "the excerpt numbered ") && natural(1, 131072)) {
            out = v - 1;
            return true;
        }
        for (unsigned n = 0; n < std::min<size_t>(64, ctx.p.constants.size()); ++n) {
            auto name = ctx.constant(n);
            // Integers written in prose can share a prefix (e.g. six/sixteen).
            if (text.starts_with(name) &&
                (text.size() == name.size() || !std::isalnum((unsigned char)text[name.size()]))) {
                text.remove_prefix(name.size());
                out = n;
                return true;
            }
        }
        return false;
    };
    switch (f) {
    case 'a':
    case 's':
        return slot(i.a, f == 'a');
    case 'b':
        return slot(i.b, true);
    case 'c':
        return slot(i.c, true);
    case 'D':
        return constant(i.c);
    case 'E':
        return constant(i.b);
    case 'W': {
        unsigned x;
        if (!constant(x))
            return false;
        i.wide = x;
        return true;
    }
    case 'M':
        if (constant(i.c)) {
            i.k = 1;
            return true;
        }
        return slot(i.c, true);
    case 'U':
    case 'V': {
        unsigned &out = f == 'U' ? i.b : i.a;
        if (eat(text, "our shared brief")) {
            auto it = std::find(ctx.p.capture_names.begin(), ctx.p.capture_names.end(), "_ENV");
            if (it == ctx.p.capture_names.end())
                return false;
            out = it - ctx.p.capture_names.begin();
            return true;
        }
        if (!eat(text, "earlier point ") || !natural(1, 256))
            return false;
        out = v - 1;
        return true;
    }
    case 'P':
    case 'q':
    case 'h': {
        auto &out = f == 'h' ? i.c : i.k;
        if (eat(text, f == 'P' ? "in reverse order" : f == 'q' ? "holds" : "fractional")) {
            out = 1;
            return true;
        }
        return eat(text, f == 'P' ? "in that order" : f == 'q' ? "falls apart" : "whole");
    }
    case 'f':
        i.k = eat(text, ", with the outstanding details taken into account");
        return true;
    case 'H':
        i.c = eat(text, " on the fractional reading");
        return true;
    case 'N':
        if (eat(text, ", along with the point immediately after it"))
            i.b = 1;
        else if (eat(text, ", along with the next ")) {
            if (!natural(2, 255) || !eat(text, " points"))
                return false;
            i.b = v;
        }
        return true;
    case 'G':
        for (unsigned e = 0; e < std::size(events); ++e)
            if (eat(text, events[e])) {
                i.c = e;
                return true;
            }
        if (eat(text, "reading ") && natural(0, 255)) {
            i.c = v;
            return true;
        }
        return false;
    case 'o':
    case 'r': {
        unsigned &field = f == 'o' ? i.b : i.c;
        if (eat(text, "everything that's available")) {
            field = 0;
            return true;
        }
        if (eat(text, "nothing further")) {
            field = 1;
            return true;
        }
        if (eat(text, "one contribution")) {
            field = 2;
            return true;
        }
        if (!natural(2, 254) || !eat(text, " contributions"))
            return false;
        field = v + 1;
        return true;
    }
    case 'X':
    case 'T': {
        bool jump = i.op == OP_JMP;
        if (!natural(-16777215, 33554431))
            return false;
        i.wide = f == 'T' ? v - int(ctx.pc) - 2 : v;
        if (jump)
            return i.wide >= -16777215 && i.wide <= 16777216;
        if (mode(i.op) == iAsBx)
            return i.wide >= -65535 && i.wide <= 65536;
        return i.wide >= 0 && i.wide <= (mode(i.op) == iAx ? 33554431 : 131071);
    }
    default:
        if (!natural((f == 'i' || f == 'j') ? -127 : 0, (f == 'i' || f == 'j') ? 128 : 255))
            return false;
        if (f == 'A')
            i.a = v;
        else if (f == 'B')
            i.b = v;
        else if (f == 'C')
            i.c = v;
        else if (f == 'i')
            i.b = v + 127;
        else if (f == 'j')
            i.c = v + 127;
        else
            return false;
        return true;
    }
}
template <class Text, class Field> bool walk(std::string_view s, Text literal, Field field) {
    while (!s.empty()) {
        auto cut = s.find('%');
        if (!literal(s.substr(0, cut)))
            return false;
        if (cut == s.npos)
            break;
        if (!field(s[cut + 1]))
            return false;
        s.remove_prefix(cut + 2);
    }
    return true;
}
int idiom(const Clause &i, const Context &ctx) {
    if (i.op == OP_ADDI && i.a == i.b && i.c >= 126 && i.c <= 128)
        return i.c == 128 ? 0 : i.c == 126 ? 1 : 2;
    if (i.op == OP_EXTRAARG && !i.wide)
        return 3;
    if (i.op == OP_VARARGPREP && !i.a)
        return 4;
    if (i.op == OP_GETTABUP && i.b == 0 && ctx.capture(0) == "our shared brief")
        return 5;
    if (i.op == OP_CALL && i.b == 1 && i.c == 1)
        return 6;
    return -1;
}
} // namespace
Clause unpack(U32 w) {
    if ((w & 127) >= NUM_OPCODES)
        fail("unknown operation in GPT program");
    Clause i{OpCode(w & 127)};
    auto m = mode(i.op);
    if (m == iAx || m == isJ)
        i.wide = int(w >> 7) - (m == isJ ? 16777215 : 0);
    else {
        i.a = w >> 7 & 255;
        if (m == iABC) {
            i.b = w >> 16 & 255;
            i.c = w >> 24;
            i.k = w >> 15 & 1;
        } else
            i.wide = int(w >> 15) - (m == iAsBx ? 65535 : 0);
    }
    return i;
}
U32 pack(const Clause &i) {
    auto m = mode(i.op);
    U32 w = i.op;
    if (m == iAx || m == isJ)
        return w | (U32(i.wide + (m == isJ ? 16777215 : 0)) << 7);
    w |= i.a << 7;
    if (m == iABC)
        return w | (i.k << 15) | (i.b << 16) | (i.c << 24);
    return w | (U32(i.wide + (m == iAsBx ? 65535 : 0)) << 15);
}
std::string sentence(const Proto &p, size_t pc) {
    Context ctx(p, pc);
    auto &i = p.code[pc];
    int variant = idiom(i, ctx);
    auto &rule = variant >= 0 ? idioms[variant] : rules[i.op];
    std::string out;
    walk(
        rule.text,
        [&](auto s) {
            out += s;
            return true;
        },
        [&](char f) {
            out += token(f, i, ctx);
            return true;
        });
    return out;
}
Clause read_sentence(const Proto &p, size_t pc, std::string_view &text) {
    Context ctx(p, pc);
    auto match = [&](const Rule &r, int variant, Clause &i) {
        auto s = text;
        i = Clause{r.op};
        if (r.op == OP_RETURN0)
            i.b = 1;
        if (r.op == OP_RETURN1)
            i.b = 2;
        if (!walk(
                r.text, [&](auto literal) { return eat(s, literal); },
                [&](char f) { return parse_token(f, i, ctx, s); }))
            return false;
        if (variant >= 0 && variant <= 2) {
            i.b = i.a;
            i.c = variant == 0 ? 128 : variant == 1 ? 126 : 127;
        }
        if (variant == 6)
            i.b = i.c = 1;
        if (s.empty() || (s.front() != ' ' && s.front() != '\n'))
            return false;
        size_t spaces = s.find_first_not_of(" \n");
        s.remove_prefix(spaces == s.npos ? s.size() : spaces);
        text = s;
        return true;
    };
    Clause result;
    for (int n = 0; n < int(std::size(idioms)); ++n)
        if (match(idioms[n], n, result))
            return result;
    for (const auto &r : rules)
        if (match(r, -1, result))
            return result;
    fail("unrecognized feedback at observation " + std::to_string(pc + 1) + ": " +
         std::string(text.substr(0, 150)));
}

namespace {
std::string readable(std::string_view name) {
    std::string s;
    for (unsigned char ch : name) {
        if (ch == '_' || ch == '-') {
            if (!s.empty() && s.back() != ' ')
                s += ' ';
            continue;
        }
        if (std::isupper(ch) && !s.empty() && std::islower((unsigned char)s.back()))
            s += ' ';
        s += char(std::tolower(ch));
    }
    return s;
}
const std::string *string_at(const Proto &p, unsigned n) {
    return n < p.constants.size() ? std::get_if<std::string>(&p.constants[n]) : nullptr;
}
int index_of(const Proto &p, std::string_view name) {
    for (unsigned i = 0; i < p.constants.size(); ++i)
        if (auto s = string_at(p, i); s && *s == name)
            return int(i);
    return -1;
}
std::string topic(const Proto &p, unsigned n) {
    auto s = string_at(p, n);
    if (!s)
        return {};
    auto leaf = s->substr(s->find_last_of("./") + 1);
    if (leaf.ends_with("_functions"))
        leaf.resize(leaf.size() - 10);
    if (leaf.empty() || leaf.size() > 60 || !std::all_of(leaf.begin(), leaf.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '_' || c == '-';
        }))
        return {};
    return leaf == "utils" ? "the shared utilities" : "the " + readable(leaf) + " notes";
}
std::string subject(const Context &ctx, unsigned n, bool condition) {
    auto raw = string_at(ctx.p, n);
    if (!raw || !identifier(*raw))
        return ctx.constant(n);
    auto label = readable(*raw);
    for (unsigned j = 0; j < n; ++j) {
        auto prior = string_at(ctx.p, j);
        if (prior && readable(*prior) == label)
            return ctx.constant(n);
    }
    if (!condition)
        return "the " + label;
    if (label.starts_with("is "))
        label.erase(0, 3);
    if (label.starts_with("windows "))
        label.front() = 'W';
    return "the " + label + " check comes back positive";
}
int topic_at(const Proto &p, std::string_view &s) {
    int best = -1;
    std::string longest;
    for (unsigned i = 0; i < p.constants.size(); ++i) {
        auto label = topic(p, i);
        if (!label.empty() && s.starts_with(label) && label.size() > longest.size()) {
            longest = std::move(label);
            best = int(i);
        }
    }
    if (best >= 0)
        s.remove_prefix(longest.size());
    return best;
}
constexpr std::string_view introduce = "Absolutely. Before I give you a firm answer, I'd want us to "
                                       "work from the same material. I'd start with ";
constexpr std::string_view more = ", then bring in ";
constexpr std::string_view last_topic = ", and round that out with ";
constexpr std::string_view imports_end =
    ". With that context in place, we can move through the rest in order — evidence · caveats · "
    "recommendation — and still answer the question you actually asked.";
constexpr std::string_view baseline = "My starting recommendation for ";
constexpr std::string_view exception = ". There is one caveat worth spelling out. If ";
constexpr std::string_view revised = ", I'd use ";
constexpr std::string_view policy_end = " instead. That keeps the general advice useful while giving "
                                        "the exception somewhere sensible to live.";
constexpr std::string_view increment_start = "I'd put that down as ";
constexpr std::string_view increment_end = ". Even a small change belongs in the total; if you count "
                                           "things differently, I'd defer to your convention here.";
constexpr std::string_view qualify = "There is one qualification worth keeping in view. ";
constexpr std::string_view followups =
    "I'd give each of these follow-ups its own answer. First there's ";
constexpr std::string_view another = ", followed by ";
constexpr std::string_view shared_context = ". Each can refer back to what we've already established, "
                                            "so there's no need to repeat the entire brief.";
unsigned earlier_replies(const Proto &p, size_t pc) {
    return std::count_if(p.code.begin(), p.code.begin() + pc,
                         [](const Clause &c) { return c.op == OP_CLOSURE; });
}
constexpr std::string_view redirect = ", I'd take this up at observation ";
constexpr std::string_view continue_reading = ". Otherwise we can carry on from here.";
Clause ins(OpCode op, unsigned a = 0, unsigned b = 0, unsigned c = 0, unsigned k = 0) {
    return {op, a, b, c, k, 0};
}
Clause wide(OpCode op, unsigned a, int w) { return {op, a, 0, 0, 0, w}; }
bool same(const Clause &a, const Clause &b) { return pack(a) == pack(b); }
void space(std::string_view &s) {
    if (s.empty() || (s.front() != ' ' && s.front() != '\n'))
        fail("expected a break between thoughts");
    auto end = s.find_first_not_of(" \n");
    s.remove_prefix(end == s.npos ? s.size() : end);
}
std::string_view condition(OpCode op) {
    if (op < OP_EQ || op > OP_TESTSET)
        return {};
    auto pattern = rules[op].text;
    auto n = pattern.find(", I'd follow");
    if (n == pattern.npos)
        return {};
    return pattern.substr(0, n);
}
} // namespace
Passage feedback(const Proto &p, size_t pc) {
    Context ctx(p, pc);
    auto &i = p.code[pc];
    auto remaining = p.code.size() - pc;
    if (i.op == OP_CLOSURE && remaining >= 2 && p.code[pc + 1].op == OP_CLOSURE) {
        unsigned reply = earlier_replies(p, pc);
        size_t n = 0;
        std::string out(followups);
        while (n < remaining) {
            auto &next = p.code[pc + n];
            if (next.op != OP_CLOSURE || next.wide != int(reply + n))
                break;
            Context binding(p, pc + n + 1);
            if (next.a >= binding.locals.size())
                break;
            if (std::any_of(binding.locals.begin(), binding.locals.begin() + next.a,
                            [&](auto name) { return *name == *binding.locals[next.a]; }))
                break;
            if (n)
                out += another;
            out += quote(*binding.locals[next.a]);
            ++n;
        }
        if (n >= 2) {
            out += shared_context;
            return {out, n, true};
        }
    }
    if (pc == 0 && same(i, ins(OP_VARARGPREP)) && !p.capture_names.empty() &&
        p.capture_names[0] == "_ENV") {
        int req = index_of(p, "require");
        size_t n = 1;
        std::string out(introduce);
        while (req >= 0 && n + 2 < remaining) {
            auto &ref = p.code[pc + n + 1];
            if (!same(p.code[pc + n], ins(OP_GETTABUP, 0, 0, req)) || ref.op != OP_LOADK || ref.a != 1 ||
                !same(p.code[pc + n + 2], ins(OP_CALL, 0, 2, 1)))
                break;
            auto label = topic(p, ref.wide);
            std::string_view check = label;
            if (label.empty() || topic_at(p, check) != ref.wide)
                break;
            if (n > 1)
                out += more;
            out += label;
            n += 3;
        }
        if (n > 1) {
            if (n > 4) {
                auto last = out.rfind(more);
                out.replace(last, more.size(), last_topic);
            }
            out += imports_end;
            return {out, n, true};
        }
    }
    if (remaining >= 6 && i.op == OP_SETTABUP && i.k && i.a == 0 &&
        ctx.capture(0) == "our shared brief") {
        unsigned tmp = ctx.locals.size();
        auto pred = p.code[pc + 1], last = p.code[pc + 5];
        if (same(pred, ins(OP_GETTABUP, tmp, 0, pred.c)) &&
            same(p.code[pc + 2], ins(OP_CALL, tmp, 1, 2)) && same(p.code[pc + 3], ins(OP_TEST, tmp)) &&
            same(p.code[pc + 4], wide(OP_JMP, 0, 1)) &&
            same(last, ins(OP_SETTABUP, 0, i.b, last.c, 1))) {
            return {std::string(baseline) + subject(ctx, i.b, false) + " would be " + ctx.constant(i.c) +
                        std::string(exception) + subject(ctx, pred.c, true) + std::string(revised) +
                        ctx.constant(last.c) + std::string(policy_end),
                    6, true};
        }
    }
    int delta = idiom(i, ctx);
    if (delta >= 0 && delta <= 2 && remaining >= 2) {
        unsigned expectation = delta == 2 ? 127 : 128;
        if (same(p.code[pc + 1], ins(OP_MMBINI, i.a, expectation, delta == 1 ? 7 : 6)))
            return {std::string(increment_start) +
                        (delta == 0   ? "one more"
                         : delta == 1 ? "one less"
                                      : "no gain") +
                        " for " + ctx.slot(i.a, false) + std::string(increment_end),
                    2, false};
    }
    if (remaining >= 2 && p.code[pc + 1].op == OP_JMP) {
        auto clause = condition(i.op);
        if (!clause.empty()) {
            std::string out(qualify);
            walk(
                clause,
                [&](auto s) {
                    out += s;
                    return true;
                },
                [&](char f) {
                    out += token(f, i, ctx);
                    return true;
                });
            out += redirect;
            out += numeral(int(pc) + 3 + p.code[pc + 1].wide);
            out += continue_reading;
            return {out, 2, true};
        }
    }
    return {sentence(p, pc), 1, i.op == OP_JMP || (i.op >= OP_TAILCALL && i.op <= OP_RETURN1)};
}
size_t read_feedback(Proto &p, size_t pc, std::string_view &text) {
    auto s = text;
    Context ctx(p, pc);
    size_t n = 0;
    auto put = [&](Clause i) {
        if (pc + n >= p.code.size())
            fail("feedback exceeds its function");
        p.code[pc + n++] = i;
    };
    auto need = [&](std::string_view part) {
        if (!eat(s, part))
            fail("incomplete feedback passage");
    };
    auto constant = [&]() {
        Clause i;
        if (!parse_token('D', i, ctx, s) || i.c >= p.constants.size() || i.c > 255)
            fail("unknown reference in recommendation");
        return i.c;
    };
    if (eat(s, followups)) {
        auto child = earlier_replies(p, pc);
        do {
            if (pc + n >= p.code.size() || child >= p.children.size())
                fail("too many follow-up answers");
            Context binding(p, pc + n + 1);
            unsigned dst = 0;
            while (dst < binding.locals.size() && !eat(s, quote(*binding.locals[dst])))
                ++dst;
            if (dst >= binding.locals.size())
                fail("unknown follow-up in this context");
            put(wide(OP_CLOSURE, dst, child++));
        } while (eat(s, another));
        need(shared_context);
    } else if (eat(s, introduce)) {
        if (pc != 0 || ctx.capture(0) != "our shared brief")
            fail("opening outside its brief");
        int req = index_of(p, "require");
        if (req < 0 || req > 255)
            fail("missing module lookup");
        put(ins(OP_VARARGPREP));
        do {
            int index = topic_at(p, s);
            if (index < 0)
                fail("unknown supporting material");
            put(ins(OP_GETTABUP, 0, 0, req));
            put(wide(OP_LOADK, 1, index));
            put(ins(OP_CALL, 0, 2, 1));
        } while (eat(s, more) || eat(s, last_topic));
        need(imports_end);
    } else if (eat(s, baseline)) {
        auto named = [&](std::string_view boundary, bool predicate) {
            for (unsigned idx = 0; idx < std::min<size_t>(p.constants.size(), 256); ++idx) {
                if (eat(s, subject(ctx, idx, predicate) + std::string(boundary)))
                    return idx;
            }
            fail("unknown subject in the recommendation");
        };
        auto field = named(" would be ", false);
        auto initial = constant();
        need(exception);
        auto pred = named(revised, true);
        auto other = constant();
        need(policy_end);
        unsigned tmp = ctx.locals.size();
        put(ins(OP_SETTABUP, 0, field, initial, 1));
        put(ins(OP_GETTABUP, tmp, 0, pred));
        put(ins(OP_CALL, tmp, 1, 2));
        put(ins(OP_TEST, tmp));
        put(wide(OP_JMP, 0, 1));
        put(ins(OP_SETTABUP, 0, field, other, 1));
    } else if (eat(s, increment_start)) {
        int d = eat(s, "one more") ? 1 : eat(s, "one less") ? -1 : eat(s, "no gain") ? 0 : 99;
        if (d == 99)
            fail("unknown change in the total");
        need(" for ");
        Clause i;
        if (!parse_token('s', i, ctx, s))
            fail("unknown running total");
        need(increment_end);
        put(ins(OP_ADDI, i.a, i.a, 127 + d));
        put(ins(OP_MMBINI, i.a, d ? 128 : 127, d < 0 ? 7 : 6));
    } else if (eat(s, qualify)) {
        bool found = false;
        for (int op = OP_EQ; op <= OP_TESTSET; ++op) {
            auto pattern = condition(OpCode(op));
            if (pattern.empty())
                continue;
            auto copy = s;
            Clause i{OpCode(op)};
            if (walk(
                    pattern, [&](auto part) { return eat(copy, part); },
                    [&](char f) { return parse_token(f, i, ctx, copy); }) &&
                eat(copy, redirect)) {
                int target;
                if (!number(copy, target, 1, int(p.code.size())))
                    fail("qualification outside its discussion");
                s = copy;
                need(continue_reading);
                put(i);
                put(wide(OP_JMP, 0, target - int(pc) - 3));
                found = true;
                break;
            }
        }
        if (!found)
            fail("unrecognized qualification");
    } else {
        p.code[pc] = read_sentence(p, pc, text);
        return 1;
    }
    space(s);
    text = s;
    return n;
}

} // namespace gpt::draft
