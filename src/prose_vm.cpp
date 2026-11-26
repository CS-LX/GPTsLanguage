#include "prose_vm.hpp"
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>

namespace gpt::draft {
namespace {
bool number(const Value &v) { return v.index() == 2 || v.index() == 3; }
bool integral(double d) { return std::isfinite(d) && d >= -0x1p63 && d < 0x1p63 && std::trunc(d) == d; }
Value key(Value v) {
    if (auto d = std::get_if<double>(&v); d && integral(*d))
        return int64_t(*d);
    return v;
}
Value literal(const Literal &v) {
    return std::visit([](auto &x) -> Value { return x; }, v);
}
Value first(Values v) { return v.empty() ? Value{} : std::move(v.front()); }
int64_t bits(uint64_t v) { return std::bit_cast<int64_t>(v); }
int64_t shift(int64_t n, int64_t by, bool left) {
    if (by >= 64 || by <= -64)
        return 0;
    if (by < 0) {
        by = -by;
        left = !left;
    }
    return bits(left ? uint64_t(n) << by : uint64_t(n) >> by);
}
int64_t floor_div(int64_t a, int64_t b) {
    if (!b)
        fail("attempt to divide by zero");
    if (b == -1)
        return bits(0 - uint64_t(a));
    auto q = a / b;
    return q - ((a % b) != 0 && ((a < 0) != (b < 0)));
}
int64_t mod(int64_t a, int64_t b) {
    if (!b)
        fail("attempt to perform modulo by zero");
    if (b == -1)
        return 0;
    auto r = a % b;
    return r && ((r < 0) != (b < 0)) ? r + b : r;
}
Value arithmetic(OpCode op, const Value &v, const Value &w) {
    bool ints = v.index() == 2 && w.index() == 2;
    auto a = ints ? std::get<int64_t>(v) : 0, b = ints ? std::get<int64_t>(w) : 0;
    switch (op) {
    case OP_ADD:
        return ints ? Value(bits(uint64_t(a) + uint64_t(b))) : Value(real(v) + real(w));
    case OP_SUB:
        return ints ? Value(bits(uint64_t(a) - uint64_t(b))) : Value(real(v) - real(w));
    case OP_MUL:
        return ints ? Value(bits(uint64_t(a) * uint64_t(b))) : Value(real(v) * real(w));
    case OP_IDIV:
        return ints ? Value(floor_div(a, b)) : Value(std::floor(real(v) / real(w)));
    case OP_MOD: {
        if (ints)
            return mod(a, b);
        double x = std::fmod(real(v), real(w));
        return x != 0 && (std::signbit(x) != std::signbit(real(w))) ? x + real(w) : x;
    }
    case OP_DIV:
        return real(v) / real(w);
    case OP_POW:
        return std::pow(real(v), real(w));
    case OP_BAND:
        return integer(v) & integer(w);
    case OP_BOR:
        return integer(v) | integer(w);
    case OP_BXOR:
        return integer(v) ^ integer(w);
    case OP_SHL:
    case OP_SHR:
        return shift(integer(v), integer(w), op == OP_SHL);
    default:
        fail("unsupported arithmetic");
    }
}
bool less_numeric(Value a, Value b) {
    a = key(a);
    b = key(b);
    if (a.index() == 2 && b.index() == 2)
        return std::get<int64_t>(a) < std::get<int64_t>(b);
    // Exactly representable integral floats have already become integers.
    // Comparing as doubles would round INT64_MAX up to 2^63.
    if (a.index() == 2) {
        double d = real(b);
        if (std::isnan(d) || d < -0x1p63)
            return false;
        return d >= 0x1p63 || std::get<int64_t>(a) <= int64_t(std::floor(d));
    }
    if (b.index() == 2) {
        double d = real(a);
        if (std::isnan(d) || d >= 0x1p63)
            return false;
        return d < -0x1p63 || int64_t(std::floor(d)) < std::get<int64_t>(b);
    }
    return real(a) < real(b);
}
struct Tail {
    Value fn;
    Values args;
};
} // namespace
Value at(const Values &v, size_t i) { return i < v.size() ? v[i] : Value{}; }
bool truth(const Value &v) { return v.index() != 0 && (v.index() != 1 || std::get<bool>(v)); }
bool equal(const Value &a, const Value &b) { return number(a) && number(b) ? key(a) == key(b) : a == b; }
std::string type_name(const Value &v) {
    constexpr const char *names[]{"nil", "boolean", "number", "number", "string", "table", "function"};
    return names[v.index()];
}
double real(const Value &v) {
    if (auto i = std::get_if<int64_t>(&v))
        return double(*i);
    if (auto d = std::get_if<double>(&v))
        return *d;
    fail("expected number, received " + type_name(v));
}
int64_t integer(const Value &v) {
    if (auto i = std::get_if<int64_t>(&v))
        return *i;
    if (auto d = std::get_if<double>(&v); d && integral(*d))
        return int64_t(*d);
    fail("expected a representable integer");
}
std::string show(const Value &v) {
    if (auto s = std::get_if<std::string>(&v))
        return *s;
    if (auto n = std::get_if<int64_t>(&v))
        return std::to_string(*n);
    if (auto n = std::get_if<double>(&v)) {
        char b[64];
        auto [p, e] = std::to_chars(b, b + sizeof(b), *n, std::chars_format::general, 14);
        if (e != std::errc{})
            fail("number formatting failed");
        std::string s(b, p);
        if (integral(*n) && s.find_first_of(".eE") == s.npos)
            s += ".0";
        return s;
    }
    if (auto b = std::get_if<bool>(&v))
        return *b ? "true" : "false";
    return type_name(v);
}
Value raw_get(Table *t, Value k) {
    if (k.index() == 3 && std::isnan(real(k)))
        return {};
    auto it = t->entries.find(key(std::move(k)));
    return it == t->entries.end() ? Value{} : it->second;
}
void raw_set(Table *t, Value k, Value v) {
    k = key(std::move(k));
    if (!k.index() || (k.index() == 3 && std::isnan(real(k))))
        fail("invalid table key");
    if (!v.index())
        t->entries.erase(k);
    else
        t->entries[std::move(k)] = std::move(v);
}
int64_t length(Table *t) {
    int64_t n = 0;
    while (raw_get(t, n + 1).index())
        ++n;
    return n;
}
Value &Frame::slot(unsigned n) {
    if (n >= 1000000)
        fail("frame exceeds value limit");
    auto it = open.find(n);
    if (it != open.end())
        return it->second->value;
    if (regs.size() <= n)
        regs.resize(n + 1);
    return regs[n];
}
void Frame::close(unsigned first) {
    for (auto it = open.lower_bound(first); it != open.end();) {
        regs.at(it->first) = it->second->value;
        it = open.erase(it);
    }
}
VM::VM(const Program &p, const std::vector<std::string> &args) : program(p) {
    globals = make<Table>();
    loaded = make<Table>();
    strings = make<Table>();
    libraries(args);
}
Function *VM::native(std::function<Values(const Values &)> fn) {
    auto f = make<Function>();
    f->builtin = std::move(fn);
    return f;
}
Function *VM::root(const Proto &p) {
    auto f = make<Function>();
    f->body = &p;
    for (size_t n = 0; n < p.captures.size(); ++n) {
        auto cell = make<Cell>();
        if (!n)
            cell->value = globals;
        f->captured.push_back(cell);
    }
    return f;
}
void VM::collect() {
    std::vector<Object *> pending;
    auto hold = [&](Object *o) {
        if (o && !o->marked) {
            o->marked = true;
            pending.push_back(o);
        }
    };
    auto mark = [&](const Value &v) {
        if (auto t = std::get_if<Table *>(&v))
            hold(*t);
        if (auto f = std::get_if<Function *>(&v))
            hold(*f);
    };
    hold(globals);
    hold(loaded);
    hold(strings);
    for (auto args : call_arguments)
        for (const auto &v : *args)
            mark(v);
    for (auto f : frames) {
        hold(f->fn);
        for (auto &v : f->regs)
            mark(v);
        for (auto &v : f->varargs)
            mark(v);
        for (auto &entry : f->open)
            hold(entry.second);
    }
    while (!pending.empty()) {
        auto o = pending.back();
        pending.pop_back();
        if (auto t = dynamic_cast<Table *>(o)) {
            hold(t->meta);
            for (auto &[k, v] : t->entries) {
                mark(k);
                mark(v);
            }
        } else if (auto f = dynamic_cast<Function *>(o)) {
            for (auto c : f->captured)
                hold(c);
            for (auto &v : f->references)
                mark(v);
        } else
            mark(static_cast<Cell *>(o)->value);
    }
    std::erase_if(heap, [](auto &o) {
        if (!o->marked)
            return true;
        o->marked = false;
        return false;
    });
    next_collection = heap.size() * 2 + 1024;
}
Value VM::metamethod(const Value &v, const std::string &name) {
    if (auto t = std::get_if<Table *>(&v); t && (*t)->meta)
        return raw_get((*t)->meta, name);
    return {};
}
Value VM::get(Value t, Value k) {
    if (t.index() == 4)
        return raw_get(strings, k);
    for (int depth = 0; depth < 100; ++depth) {
        if (auto obj = std::get_if<Table *>(&t)) {
            auto v = raw_get(*obj, k);
            if (v.index())
                return v;
            auto index = metamethod(t, "__index");
            if (!index.index())
                return {};
            if (index.index() == 6)
                return first(call(index, {t, k}));
            t = index;
        } else
            fail("cannot index " + type_name(t));
    }
    fail("cyclic __index");
}
void VM::set(Value t, Value k, Value v) {
    for (int depth = 0; depth < 100; ++depth) {
        auto obj = std::get_if<Table *>(&t);
        if (!obj)
            fail("assignment requires a table");
        auto index = metamethod(t, "__newindex");
        if (!index.index() || raw_get(*obj, k).index()) {
            raw_set(*obj, k, v);
            return;
        }
        if (index.index() == 6) {
            call(index, {t, k, v});
            return;
        }
        t = index;
    }
    fail("cyclic __newindex");
}
Values VM::module(const std::string &name) {
    auto existing = raw_get(loaded, name);
    if (truth(existing))
        return {existing};
    auto path = name;
    std::replace(path.begin(), path.end(), '.', '/');
    for (auto suffix : {".lua", "/init.lua"}) {
        auto it = program.files.find(path + suffix);
        if (it == program.files.end())
            continue;
        auto v = first(call(root(it->second), {name, it->first}));
        if (!v.index())
            v = raw_get(loaded, name);
        if (!v.index())
            v = true;
        raw_set(loaded, name, v);
        return {v, it->first};
    }
    fail("module '" + name + "' is not bundled in this GPT assembly");
}
void VM::start(const Values &args) { call(root(program.files.at(program.entry)), args); }
Values VM::call(Value target, const Values &arguments) {
    Values args = arguments;
    call_arguments.push_back(&args);
    struct ReleaseArguments {
        std::vector<const Values *> &roots;
        ~ReleaseArguments() { roots.pop_back(); }
    } release{call_arguments};
    for (;;) {
        if (target.index() != 6) {
            auto method = metamethod(target, "__call");
            if (!method.index())
                fail("attempt to call " + type_name(target));
            args.insert(args.begin(), target);
            target = method;
        }
        auto fn = std::get<Function *>(target);
        if (fn->builtin) {
            struct Scope {
                unsigned &n;
                ~Scope() { --n; }
            } scope{native_depth};
            ++native_depth;
            return fn->builtin(args);
        }
        if (frames.size() >= 250)
            fail("GPT call stack exhausted");
        try {
            return execute(fn, args);
        } catch (Tail &tail) {
            target = tail.fn;
            args = std::move(tail.args);
        }
    }
}
Values VM::execute(Function *fn, const Values &args) {
    auto &p = *fn->body;
    Frame frame{fn, Values(p.stack), {}, {}, {}, 0};
    for (unsigned n = 0; n < p.params; ++n)
        frame.slot(n) = at(args, n);
    if (args.size() > p.params)
        frame.varargs.assign(args.begin() + p.params, args.end());
    frames.push_back(&frame);
    struct Pop {
        std::vector<Frame *> &f;
        ~Pop() { f.pop_back(); }
    } pop{frames};
    auto R = [&](unsigned n) -> Value & { return frame.slot(n); };
    auto K = [&](unsigned n) { return literal(p.constants.at(n)); };
    auto U = [&](unsigned n) -> Value & { return fn->captured.at(n)->value; };
    auto span = [&](unsigned a, unsigned count) {
        Values list;
        for (unsigned n = 0; n < count; ++n)
            list.push_back(R(a + n));
        return list;
    };
    auto open_count = [&](unsigned a) {
        if (frame.top < a)
            fail("invalid open argument span");
        return unsigned(frame.top - a);
    };
    auto results = [&](unsigned a, unsigned c, const Values &v) {
        unsigned n = c ? c - 1 : v.size();
        for (unsigned x = 0; x < n; ++x)
            R(a + x) = at(v, x);
        frame.top = a + n;
    };
    auto close = [&](unsigned a, Value error = Value{}) {
        while (!frame.closing.empty() && frame.closing.back() >= a) {
            auto v = R(frame.closing.back());
            frame.closing.pop_back();
            call(metamethod(v, "__close"), {v, error});
        }
        frame.close(a);
    };
    auto ordering = [&](Value a, Value b, bool inclusive) {
        if (number(a) && number(b))
            return less_numeric(a, b) || (inclusive && equal(a, b));
        if (a.index() == 4 && b.index() == 4)
            return inclusive ? std::get<4>(a) <= std::get<4>(b) : std::get<4>(a) < std::get<4>(b);
        auto method = metamethod(a, inclusive ? "__le" : "__lt");
        if (!method.index())
            method = metamethod(b, inclusive ? "__le" : "__lt");
        return truth(first(call(method, {a, b})));
    };
    int pc = 0;
    try {
        while (pc >= 0 && size_t(pc) < p.code.size()) {
            if (!native_depth && heap.size() > next_collection)
                collect();
            const auto i = p.code[pc++];
            auto op = i.op;
            unsigned a = i.a, b = i.b, c = i.c;
            auto mixed = [&]() { return i.k ? K(c) : R(c); };
            auto extra = [&]() {
                auto j = p.code.at(pc++);
                if (j.op != OP_EXTRAARG)
                    fail("missing supporting detail");
                return unsigned(j.wide);
            };
            auto test = [&](bool yes) {
                if (yes != bool(i.k))
                    ++pc;
            };
            if (op >= OP_ADDI && op <= OP_SHR) {
                OpCode operation = op;
                Value x = R(b), y;
                if (op == OP_ADDI) {
                    operation = OP_ADD;
                    y = int64_t(c) - 127;
                } else if (op >= OP_ADDK && op <= OP_BXORK) {
                    operation = OpCode(OP_ADD + op - OP_ADDK);
                    y = K(c);
                } else if (op == OP_SHRI || op == OP_SHLI) {
                    operation = op == OP_SHRI ? OP_SHR : OP_SHL;
                    y = int64_t(c) - 127;
                    if (op == OP_SHLI)
                        std::swap(x, y);
                } else
                    y = R(c);
                if (number(x) && number(y)) {
                    auto validint = [](const Value &v) { return v.index() == 2 || integral(real(v)); };
                    if (operation < OP_BAND || (validint(x) && validint(y))) {
                        R(a) = arithmetic(operation, x, y);
                        ++pc;
                    }
                }
                continue;
            }
            switch (op) {
            case OP_MOVE:
                R(a) = R(b);
                break;
            case OP_LOADI:
                R(a) = int64_t(i.wide);
                break;
            case OP_LOADF:
                R(a) = double(i.wide);
                break;
            case OP_LOADK:
                R(a) = K(i.wide);
                break;
            case OP_LOADKX:
                R(a) = K(extra());
                break;
            case OP_LFALSESKIP:
                ++pc;
                [[fallthrough]];
            case OP_LOADFALSE:
                R(a) = false;
                break;
            case OP_LOADTRUE:
                R(a) = true;
                break;
            case OP_LOADNIL:
                for (unsigned n = 0; n <= b; ++n)
                    R(a + n) = {};
                break;
            case OP_GETUPVAL:
                R(a) = U(b);
                break;
            case OP_SETUPVAL:
                U(b) = R(a);
                break;
            case OP_GETTABUP:
                R(a) = get(U(b), K(c));
                break;
            case OP_GETTABLE:
                R(a) = get(R(b), R(c));
                break;
            case OP_GETI:
                R(a) = get(R(b), int64_t(c));
                break;
            case OP_GETFIELD:
                R(a) = get(R(b), K(c));
                break;
            case OP_SETTABUP:
                set(U(a), K(b), mixed());
                break;
            case OP_SETTABLE:
                set(R(a), R(b), mixed());
                break;
            case OP_SETI:
                set(R(a), int64_t(b), mixed());
                break;
            case OP_SETFIELD:
                set(R(a), K(b), mixed());
                break;
            case OP_NEWTABLE:
                extra();
                R(a) = make<Table>();
                break;
            case OP_SELF: {
                auto receiver = R(b), method = get(receiver, mixed());
                R(a + 1) = receiver;
                R(a) = method;
                break;
            }
            case OP_MMBIN:
            case OP_MMBINI:
            case OP_MMBINK: {
                static const char *events[]{"__add",  "__sub",  "__mul", "__mod",  "__pow", "__div",
                                            "__idiv", "__band", "__bor", "__bxor", "__shl", "__shr"};
                if (c < 6 || c >= 6 + std::size(events) || pc < 2)
                    fail("invalid arithmetic qualification");
                Value x = R(a), y = op == OP_MMBIN    ? R(b)
                                    : op == OP_MMBINI ? Value(int64_t(b) - 127)
                                                      : K(b);
                if (i.k)
                    std::swap(x, y);
                auto method = metamethod(x, events[c - 6]);
                if (!method.index())
                    method = metamethod(y, events[c - 6]);
                R(p.code[pc - 2].a) = first(call(method, {x, y}));
                break;
            }
            case OP_UNM: {
                auto v = R(b);
                R(a) = number(v) ? arithmetic(OP_SUB, int64_t(0), v)
                                 : first(call(metamethod(v, "__unm"), {v, v}));
                break;
            }
            case OP_BNOT:
                R(a) = ~integer(R(b));
                break;
            case OP_NOT:
                R(a) = !truth(R(b));
                break;
            case OP_LEN: {
                auto v = R(b), m = metamethod(v, "__len");
                if (m.index())
                    R(a) = first(call(m, {v, v}));
                else if (v.index() == 4)
                    R(a) = int64_t(std::get<4>(v).size());
                else if (v.index() == 5)
                    R(a) = length(std::get<5>(v));
                else
                    fail("length requires text or table");
                break;
            }
            case OP_CONCAT: {
                if (!b)
                    fail("empty concatenation");
                Value v = R(a + b - 1);
                for (unsigned n = b - 1; n; --n) {
                    auto x = R(a + n - 1);
                    if ((number(x) || x.index() == 4) && (number(v) || v.index() == 4))
                        v = show(x) + show(v);
                    else {
                        auto m = metamethod(x, "__concat");
                        if (!m.index())
                            m = metamethod(v, "__concat");
                        v = first(call(m, {x, v}));
                    }
                }
                R(a) = v;
                break;
            }
            case OP_CLOSE:
                close(a);
                break;
            case OP_TBC:
                if (truth(R(a))) {
                    if (!metamethod(R(a), "__close").index())
                        fail("value cannot be closed");
                    frame.closing.push_back(a);
                }
                break;
            case OP_JMP:
                pc += i.wide;
                break;
            case OP_EQ:
            case OP_EQK:
            case OP_EQI: {
                Value x = R(a), y = op == OP_EQ ? R(b) : op == OP_EQK ? K(b) : Value(int64_t(b) - 127);
                bool same = equal(x, y);
                if (!same && x.index() == 5 && y.index() == 5) {
                    auto m = metamethod(x, "__eq");
                    if (!m.index())
                        m = metamethod(y, "__eq");
                    if (m.index())
                        same = truth(first(call(m, {x, y})));
                }
                test(same);
                break;
            }
            case OP_LT:
            case OP_LE:
                test(ordering(R(a), R(b), op == OP_LE));
                break;
            case OP_LTI:
            case OP_LEI:
            case OP_GTI:
            case OP_GEI: {
                Value x = R(a), y = int64_t(b) - 127;
                if (op == OP_GTI || op == OP_GEI)
                    std::swap(x, y);
                test(ordering(x, y, op == OP_GEI || op == OP_LEI));
                break;
            }
            case OP_TEST:
                test(truth(R(a)));
                break;
            case OP_TESTSET:
                if (truth(R(b)) == bool(i.k))
                    R(a) = R(b);
                else
                    ++pc;
                break;
            case OP_CALL:
            case OP_TAILCALL: {
                auto target = R(a);
                auto av = span(a + 1, b ? b - 1 : open_count(a + 1));
                if (op == OP_TAILCALL) {
                    close(0);
                    throw Tail{target, std::move(av)};
                }
                results(a, c, call(target, av));
                break;
            }
            case OP_RETURN:
            case OP_RETURN0:
            case OP_RETURN1: {
                auto v = span(a, op == OP_RETURN0   ? 0
                                 : op == OP_RETURN1 ? 1
                                 : b                ? b - 1
                                                    : open_count(a));
                close(0);
                return v;
            }
            case OP_FORPREP: {
                auto initial = R(a), limit = R(a + 1), step = R(a + 2);
                if (real(step) == 0)
                    fail("zero loop step");
                if (real(step) > 0 ? ordering(limit, initial, false) : ordering(initial, limit, false)) {
                    pc += i.wide + 1;
                    break;
                }
                if (initial.index() == 2 && limit.index() == 2 && step.index() == 2) {
                    auto x = integer(initial), y = integer(limit), d = integer(step);
                    uint64_t count = d > 0 ? (uint64_t(y) - uint64_t(x)) / uint64_t(d)
                                           : (uint64_t(x) - uint64_t(y)) / (0 - uint64_t(d));
                    R(a + 1) = bits(count);
                } else {
                    R(a) = real(initial);
                    R(a + 1) = real(limit);
                    R(a + 2) = real(step);
                }
                R(a + 3) = R(a);
                break;
            }
            case OP_FORLOOP: {
                bool more = false;
                auto next = arithmetic(OP_ADD, R(a), R(a + 2));
                if (R(a).index() == 2) {
                    auto left = uint64_t(integer(R(a + 1)));
                    if (left) {
                        R(a + 1) = bits(left - 1);
                        more = true;
                    }
                } else
                    more =
                        real(R(a + 2)) > 0 ? real(next) <= real(R(a + 1)) : real(next) >= real(R(a + 1));
                if (more) {
                    R(a) = R(a + 3) = next;
                    pc -= i.wide;
                }
                break;
            }
            case OP_TFORPREP:
                if (truth(R(a + 3)))
                    frame.closing.push_back(a + 3);
                pc += i.wide;
                break;
            case OP_TFORCALL:
                results(a + 4, c + 1, call(R(a), {R(a + 1), R(a + 2)}));
                break;
            case OP_TFORLOOP:
                if (R(a + 4).index()) {
                    R(a + 2) = R(a + 4);
                    pc -= i.wide;
                }
                break;
            case OP_SETLIST: {
                auto t = std::get<Table *>(R(a));
                unsigned n = b ? b : open_count(a + 1);
                int64_t offset = c;
                if (i.k)
                    offset += int64_t(extra()) * 256;
                for (unsigned j = 1; j <= n; ++j)
                    raw_set(t, offset + j, R(a + j));
                break;
            }
            case OP_CLOSURE: {
                auto &child = p.children.at(i.wide);
                auto f = make<Function>();
                f->body = &child;
                for (auto cap : child.captures) {
                    if (!cap.local)
                        f->captured.push_back(fn->captured.at(cap.index));
                    else {
                        if (!frame.open.contains(cap.index)) {
                            auto cell = make<Cell>();
                            cell->value = R(cap.index);
                            frame.open.emplace(cap.index, cell);
                        }
                        f->captured.push_back(frame.open.at(cap.index));
                    }
                }
                R(a) = f;
                break;
            }
            case OP_VARARG:
                results(a, c, frame.varargs);
                break;
            case OP_VARARGPREP:
                break;
            default:
                fail("unexpected independent GPT instruction");
            }
        }
        fail("control left the feedback before a conclusion");
    } catch (const std::exception &e) {
        close(0, std::string(e.what()));
        fail("at observation " + std::to_string(pc) + ": " + e.what());
    }
}
int run(const Program &p, const std::vector<std::string> &args) {
    VM vm(p, args);
    Values rest;
    for (size_t i = 1; i < args.size(); ++i)
        rest.push_back(args[i]);
    try {
        vm.start(rest);
        return 0;
    } catch (const Exit &exit) {
        return exit.status;
    }
}
} // namespace gpt::draft
