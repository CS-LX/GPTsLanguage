#include "prose_vm.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <regex>
#include <io.h>

namespace gpt::draft {
namespace {
std::string text(const Value &v) {
    if (v.index() >= 2 && v.index() <= 4)
        return show(v);
    fail("expected text, received " + type_name(v));
}
Table *table(const Value &v) {
    if (auto t = std::get_if<Table *>(&v))
        return *t;
    fail("expected a table");
}
int64_t opt(const Values &args, size_t at_, int64_t fallback) {
    auto v = at(args, at_);
    return v.index() ? integer(v) : fallback;
}
int64_t position(int64_t i, int64_t len) { return i < 0 ? len + i + 1 : i; }
Values find_text(const Values &args, bool positions) {
    auto source = text(at(args, 0)), pattern = text(at(args, 1));
    auto start = std::max<int64_t>(1, position(opt(args, 2, 1), source.size())) - 1;
    if (size_t(start) > source.size())
        return {};
    size_t begin, len;
    Values captures;
    if ((positions && truth(at(args, 3))) || pattern.find_first_of("^$*+?.([%-") == pattern.npos) {
        begin = source.find(pattern, start);
        if (begin == source.npos)
            return {};
        len = pattern.size();
    } else {
        // A deliberate subset of Lua patterns. Reject constructs whose ECMAScript semantics differ.
        std::string rx;
        bool bracket = false;
        for (size_t i = 0; i < pattern.size(); ++i) {
            char c = pattern[i];
            if (c == '%') {
                if (++i == pattern.size())
                    fail("incomplete string pattern");
                c = pattern[i];
                switch (c) {
                case 'd':
                case 'D':
                case 's':
                case 'S':
                case 'w':
                case 'W':
                    if (c == 'w' || c == 'W') {
                        if (bracket)
                            fail("nested alphanumeric class is unsupported");
                        rx += c == 'w' ? "[A-Za-z0-9]" : "[^A-Za-z0-9]";
                    } else {
                        rx += '\\';
                        rx += c;
                    }
                    break;
                case 'a':
                case 'l':
                case 'u': {
                    auto cls = c == 'a' ? "A-Za-z" : c == 'l' ? "a-z" : "A-Z";
                    rx += bracket ? std::string(cls) : "[" + std::string(cls) + "]";
                    break;
                }
                default:
                    if (std::isalnum((unsigned char)c))
                        fail("unsupported Lua pattern escape");
                    rx += '\\';
                    rx += c;
                }
            } else if (c == '-' && !bracket)
                rx += "*?";
            else if (c == '.' && !bracket)
                rx += "[\\s\\S]";
            else {
                if (c == '[')
                    bracket = true;
                else if (c == ']')
                    bracket = false;
                if (c == '\\' || c == '|' || c == '{' || c == '}')
                    rx += '\\';
                rx += c;
            }
        }
        std::smatch m;
        std::regex re(rx);
        if (!std::regex_search(source.cbegin() + start, source.cend(), m, re))
            return {};
        begin = start + m.position();
        len = m.length();
        for (size_t i = 1; i < m.size(); ++i)
            captures.emplace_back(m[i].str());
    }
    if (positions) {
        captures.insert(captures.begin(), int64_t(begin + len));
        captures.insert(captures.begin(), int64_t(begin + 1));
    } else if (captures.empty())
        captures.emplace_back(source.substr(begin, len));
    return captures;
}
} // namespace
void VM::libraries(const std::vector<std::string> &argv) {
    auto group = [&](std::string name) {
        auto t = make<Table>();
        raw_set(globals, name, t);
        raw_set(loaded, name, t);
        return t;
    };
    auto define = [&](Table *t, std::string name, auto fn) {
        auto f = native(fn);
        raw_set(t, name, f);
        return f;
    };
    auto arg = make<Table>();
    for (size_t i = 0; i < argv.size(); ++i)
        raw_set(arg, int64_t(i), argv[i]);
    raw_set(globals, "arg", arg);
    raw_set(globals, "_G", globals);
    raw_set(globals, "_VERSION", std::string("Lua 5.4"));
    auto package = group("package");
    raw_set(package, "loaded", loaded);
    raw_set(package, "config", std::string("\\\n;\n?\n!\n-\n"));
    define(globals, "require", [this](auto &a) { return module(text(at(a, 0))); });
    define(globals, "type", [](auto &a) { return Values{type_name(at(a, 0))}; });
    define(globals, "tostring", [this](auto &a) {
        auto x = at(a, 0), m = metamethod(x, "__tostring");
        return m.index() ? call(m, {x}) : Values{show(x)};
    });
    define(globals, "tonumber", [](auto &a) -> Values {
        auto v = at(a, 0);
        if (v.index() == 2 || v.index() == 3)
            return {v};
        if (v.index() != 4)
            return {};
        auto s = text(v);
        auto first = s.find_first_not_of(" \t\r\n");
        if (first == s.npos)
            return {};
        s = s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
        auto radix = opt(a, 1, 10);
        if (radix < 2 || radix > 36)
            fail("invalid number base");
        int64_t n;
        auto [p, e] = std::from_chars(s.data(), s.data() + s.size(), n, int(radix));
        if (e == std::errc{} && p == s.data() + s.size())
            return {n};
        if (a.size() > 1)
            return {};
        char *end;
        double d = std::strtod(s.c_str(), &end);
        return end == s.data() + s.size() ? Values{d} : Values{};
    });
    define(globals, "assert", [](auto &a) {
        if (!truth(at(a, 0)))
            fail(a.size() > 1 ? show(a[1]) : "assertion failed");
        return a;
    });
    define(globals, "error", [](auto &a) -> Values { fail(show(at(a, 0))); });
    define(globals, "pcall", [this](auto &a) {
        try {
            if (a.empty())
                fail("missing function");
            Values r = call(a[0], {a.begin() + 1, a.end()});
            r.insert(r.begin(), true);
            return r;
        } catch (const std::exception &e) {
            return Values{false, std::string(e.what())};
        }
    });
    define(globals, "setmetatable", [](auto &a) {
        auto t = table(at(a, 0));
        auto m = at(a, 1);
        t->meta = m.index() ? table(m) : nullptr;
        return Values{t};
    });
    define(globals, "getmetatable", [](auto &a) {
        auto t = at(a, 0);
        if (t.index() == 5 && std::get<5>(t)->meta)
            return Values{std::get<5>(t)->meta};
        return Values{};
    });
    define(globals, "rawget", [](auto &a) { return Values{raw_get(table(at(a, 0)), at(a, 1))}; });
    define(globals, "rawset", [](auto &a) {
        auto t = table(at(a, 0));
        raw_set(t, at(a, 1), at(a, 2));
        return Values{t};
    });
    auto next = native([](const Values &a) {
        auto &entries = table(at(a, 0))->entries;
        auto k = at(a, 1);
        auto it = k.index() ? entries.upper_bound(k) : entries.begin();
        if (it == entries.end())
            return Values{};
        return Values{it->first, it->second};
    });
    raw_set(globals, "next", next);
    define(globals, "pairs", [next](auto &a) { return Values{next, at(a, 0), Value{}}; })->references = {
        next};
    auto inext = native([](const Values &a) {
        auto n = integer(at(a, 1)) + 1;
        auto x = raw_get(table(at(a, 0)), n);
        return x.index() ? Values{n, x} : Values{};
    });
    define(globals, "ipairs", [inext](auto &a) {
        return Values{inext, at(a, 0), int64_t(0)};
    })->references = {inext};
    define(globals, "print", [](auto &a) {
        for (size_t i = 0; i < a.size(); ++i) {
            if (i)
                std::putchar('\t');
            auto s = show(a[i]);
            std::fwrite(s.data(), 1, s.size(), stdout);
        }
        std::putchar('\n');
        std::fflush(stdout);
        return Values{};
    });
    auto io = group("io");
    define(io, "write", [io](auto &a) {
        for (auto &x : a) {
            auto s = text(x);
            std::fwrite(s.data(), 1, s.size(), stdout);
        }
        return Values{io};
    })->references = {io};
    define(io, "flush", [](auto &) { return Values{std::fflush(stdout) == 0}; });
    define(io, "read", [](auto &a) {
        if (!a.empty() && text(a[0]) != "*l" && text(a[0]) != "l")
            fail("independent io.read supports lines");
        if (_isatty(_fileno(stdin)))
            std::fflush(stdout);
        std::string line;
        for (;;) {
            int c = std::getchar();
            if (c == '\n')
                break;
            if (c == EOF) {
                if (line.empty())
                    return Values{};
                break;
            }
            line += char(c);
        }
        return Values{line};
    });
    auto os = group("os");
    define(os, "execute", [](auto &a) {
        if (a.empty())
            return Values{std::system(nullptr) != 0};
        auto status = std::system(text(a[0]).c_str());
        return Values{status == 0 ? Value(true) : Value{}, std::string("exit"), int64_t(status)};
    });
    define(os, "exit", [](auto &a) -> Values {
        auto x = at(a, 0);
        throw Exit{x.index() == 1 ? (truth(x) ? 0 : 1) : int(opt(a, 0, 0))};
    });
    raw_set(globals, "string", strings);
    raw_set(loaded, "string", strings);
    define(strings, "sub", [](auto &a) {
        auto s = text(at(a, 0));
        int64_t n = s.size();
        auto from = std::clamp<int64_t>(position(integer(at(a, 1)), n), 1, n + 1),
             to = std::min(n, position(opt(a, 2, -1), n));
        return Values{to >= from ? s.substr(from - 1, to - from + 1) : std::string{}};
    });
    define(strings, "byte", [](auto &a) {
        auto s = text(at(a, 0));
        int64_t n = s.size(), i = opt(a, 1, 1);
        auto j = std::min(n, position(opt(a, 2, i), n));
        Values r;
        for (i = std::max<int64_t>(1, position(i, n)); i <= j; ++i)
            r.emplace_back(int64_t((unsigned char)s[i - 1]));
        return r;
    });
    define(strings, "char", [](auto &a) {
        std::string s;
        for (auto &x : a) {
            auto i = integer(x);
            if (i < 0 || i > 255)
                fail("invalid character");
            s += char(i);
        }
        return Values{s};
    });
    define(strings, "find", [](auto &a) { return find_text(a, true); });
    define(strings, "match", [](auto &a) { return find_text(a, false); });
    define(strings, "rep", [](auto &a) {
        auto s = text(at(a, 0)), sep = a.size() > 2 ? text(a[2]) : "";
        auto n = integer(at(a, 1));
        std::string out;
        if (n > 0 && (s.size() + sep.size()) && uint64_t(n) > lua54::limit / (s.size() + sep.size()))
            fail("repeated text exceeds limit");
        if (!s.empty() || !sep.empty())
            for (int64_t i = 0; i < n; ++i) {
                if (i)
                    out += sep;
                out += s;
            }
        return Values{out};
    });
    auto tbl = group("table");
    define(tbl, "insert", [](auto &a) {
        auto t = table(at(a, 0));
        int64_t n = length(t);
        auto pos = a.size() == 3 ? integer(a[1]) : n + 1;
        if (a.size() < 2 || pos < 1 || pos > n + 1)
            fail("invalid table insertion");
        for (auto j = n; j >= pos; --j)
            raw_set(t, j + 1, raw_get(t, j));
        raw_set(t, pos, a.back());
        return Values{};
    });
    define(tbl, "concat", [](auto &a) {
        auto t = table(at(a, 0));
        auto sep = a.size() > 1 ? text(a[1]) : "";
        std::string s;
        auto begin = opt(a, 2, 1), end = opt(a, 3, length(t));
        for (auto i = begin; i <= end; ++i) {
            if (i > begin)
                s += sep;
            s += text(raw_get(t, i));
        }
        return Values{s};
    });
}
} // namespace gpt::draft
