#include "draft.hpp"
#include <algorithm>
#include <cstring>
#include <set>

namespace gpt::lua54 {
namespace {
// luaU_dump, Windows x64, stock 64-bit lua_Integer and binary64 lua_Number.
constexpr std::uint8_t header[]{0x1b, 'L', 'u', 'a', 0x54, 0,    0x19, 0x93, 13,  10, 0x1a,
                                10,   4,   8,   8,   0x78, 0x56, 0,    0,    0,   0,  0,
                                0,    0,   0,   0,   0,    0,    0x28, 0x77, 0x40};
std::string_view chars(View v) { return {reinterpret_cast<const char *>(v.data()), v.size()}; }
void append(Bytes &out, View bytes) {
    if (bytes.size() > limit - out.size())
        fail("Lua package exceeds the 256 MiB section limit");
    out.insert(out.end(), bytes.begin(), bytes.end());
}
void append(Bytes &out, std::string_view s) {
    append(out, {reinterpret_cast<const std::uint8_t *>(s.data()), s.size()});
}
void put(Bytes &out, U32 n) { append(out, {reinterpret_cast<const std::uint8_t *>(&n), 4}); }
void blob(Bytes &out, View v) {
    put(out, v.size());
    append(out, v);
}
void name(Bytes &out, const std::string &s) {
    put(out, s.size());
    append(out, s);
}
struct Reader {
    View bytes;
    size_t at = 0;
    View get(size_t n) {
        if (n > bytes.size() - at)
            fail("truncated Lua metadata");
        auto v = bytes.subspan(at, n);
        at += n;
        return v;
    }
    unsigned byte() { return get(1).front(); }
    U32 number() {
        U32 n;
        std::memcpy(&n, get(4).data(), 4);
        return n;
    }
    View blob() { return get(number()); }
    size_t var() {
        size_t n = 0;
        for (;;) {
            auto b = byte();
            if (n > (limit - (b & 127)) / 128)
                fail("oversized Lua metadata field");
            n = n * 128 + (b & 127);
            if (b & 128)
                return n;
        }
    }
    std::string string() {
        auto n = var();
        if (n)
            return std::string(chars(get(n - 1)));
        return {};
    }
    void finished() {
        if (at != bytes.size())
            fail("unexpected trailing Lua data");
    }
};
struct Hole {
    size_t offset, count;
};
using draft::Proto;
Proto prototype(Reader &r, bool dumped, unsigned depth, std::vector<Hole> &layout,
                const std::vector<Hole> *expected) {
    if (depth > 250)
        fail("Lua prototype nesting exceeds reader limit");
    Proto p;
    r.string();
    r.var();
    r.var();
    p.params = r.byte();
    r.byte();
    p.stack = r.byte();
    auto count = r.var();
    p.offset = r.at;
    if (expected && (layout.size() >= expected->size() || (*expected)[layout.size()].offset != r.at ||
                     (*expected)[layout.size()].count != count))
        fail("incorrect paragraph span");
    layout.push_back({r.at, count});
    if (count > limit / sizeof(draft::Clause))
        fail("oversized function");
    if (dumped && count > (r.bytes.size() - r.at) / 4)
        fail("truncated instructions");
    p.code.resize(count);
    if (dumped)
        for (auto &i : p.code)
            i = draft::unpack(r.number());
    for (size_t n = r.var(); n; --n) {
        switch (r.byte()) {
        case 0:
            p.constants.emplace_back(std::monostate{});
            break;
        case 1:
            p.constants.emplace_back(false);
            break;
        case 17:
            p.constants.emplace_back(true);
            break;
        case 3: {
            int64_t v;
            std::memcpy(&v, r.get(8).data(), 8);
            p.constants.emplace_back(v);
            break;
        }
        case 19: {
            double v;
            std::memcpy(&v, r.get(8).data(), 8);
            p.constants.emplace_back(v);
            break;
        }
        case 4:
        case 20:
            p.constants.emplace_back(r.string());
            break;
        default:
            fail("unknown Lua constant");
        }
    }
    for (size_t n = r.var(); n; --n) {
        p.captures.push_back({r.byte(), r.byte()});
        r.byte();
    }
    for (size_t n = r.var(); n; --n)
        p.children.push_back(prototype(r, dumped, depth + 1, layout, expected));
    r.get(r.var());
    for (size_t n = r.var(); n; --n) {
        r.var();
        r.var();
    }
    for (size_t n = r.var(); n; --n)
        p.locals.push_back({r.string(), r.var(), r.var()});
    for (size_t n = r.var(); n; --n)
        p.capture_names.push_back(r.string());
    return p;
}
template <class P, class F> void preorder(P &p, const F &visit) {
    visit(p);
    for (auto &child : p.children)
        preorder(child, visit);
}
Proto from_dump(View bytes) {
    Reader r{bytes};
    if (!std::ranges::equal(r.get(sizeof header), header))
        fail("unsupported Lua chunk ABI");
    r.byte();
    std::vector<Hole> layout;
    auto p = prototype(r, true, 0, layout, nullptr);
    r.finished();
    return p;
}
} // namespace
Payload encode_package(const Package &p) {
    Payload result;
    auto &meta = result.metadata, &prose = result.prose;
    append(meta, signature);
    put(meta, 2);
    put(meta, p.host);
    name(meta, p.entry);
    name(meta, p.symbol);
    put(meta, p.chunks.size());
    for (const auto &c : p.chunks) {
        auto root = from_dump(c.bytecode);
        U32 count = 0;
        preorder(root, [&](const Proto &) { ++count; });
        name(meta, c.path);
        put(meta, count);
        size_t end = sizeof header;
        View bytes = c.bytecode;
        preorder(root, [&](const Proto &fn) {
            blob(meta, bytes.subspan(end, fn.offset - end));
            put(meta, fn.code.size());
            end = fn.offset + 4 * fn.code.size();
            unsigned thoughts = 0;
            for (size_t pc = 0; pc < fn.code.size();) {
                auto group = draft::feedback(fn, pc);
                pc += group.count;
                ++thoughts;
                append(prose, group.text);
                bool end = group.boundary || pc == fn.code.size() || thoughts >= 4;
                append(prose, end ? "\n\n" : " ");
                if (end)
                    thoughts = 0;
            }
        });
        blob(meta, bytes.subspan(end));
    }
    return result;
}
namespace {
Package parse(Sections sections, draft::Program *program) {
    if (sections.metadata.size() > limit || sections.prose.size() > limit)
        fail("oversized GPT package");
    Reader r{sections.metadata};
    if (chars(r.get(signature.size())) != signature)
        fail("expected GPT prose");
    Package p;
    p.grammar = r.number();
    if (p.grammar != 1 && p.grammar != 2)
        fail("unsupported GPT grammar");
    if (p.grammar == 2) {
        auto backend = r.number();
        if (backend > 1)
            fail("unknown GPT backend");
        p.host = backend;
    }
    p.entry = chars(r.blob());
    p.symbol = chars(r.blob());
    if (!identifier(p.symbol))
        fail("invalid export");
    std::set<std::string> paths;
    auto prose = chars(sections.prose);
    for (U32 files = r.number(); files; --files) {
        Chunk c;
        c.path = chars(r.blob());
        if (c.path.empty() || c.path.find('\0') != c.path.npos || !paths.insert(c.path).second)
            fail("invalid module name");
        Bytes metadata;
        std::vector<Hole> holes;
        for (U32 n = r.number(); n; --n) {
            append(metadata, r.blob());
            auto count = r.number();
            if (count > prose.size() / 4 || c.operations > prose.size() / 4 - count)
                fail("instruction count exceeds text");
            holes.push_back({metadata.size(), count});
            c.operations += count;
        }
        append(metadata, r.blob());
        Reader m{metadata};
        m.byte();
        std::vector<Hole> actual;
        auto root = prototype(m, false, 0, actual, &holes);
        m.finished();
        if (actual.size() != holes.size())
            fail("unused paragraph ranges");
        size_t end = 0;
        if (!program)
            append(c.bytecode, header);
        preorder(root, [&](Proto &fn) {
            if (!program)
                append(c.bytecode, View(metadata).subspan(end, fn.offset - end));
            end = fn.offset;
            for (size_t pc = 0; pc < fn.code.size();) {
                size_t used = 1;
                if (p.grammar == 1) {
                    auto split = prose.find("\n\n");
                    if (split == prose.npos)
                        fail("missing legacy paragraph");
                    fn.code[pc] = draft::unpack(assemble(prose.substr(0, split)));
                    prose.remove_prefix(split + 2);
                } else
                    used = draft::read_feedback(fn, pc, prose);
                if (!program)
                    for (size_t n = 0; n < used; ++n)
                        put(c.bytecode, draft::pack(fn.code[pc + n]));
                pc += used;
            }
        });
        if (!program)
            append(c.bytecode, View(metadata).subspan(end));
        else
            program->files.emplace(c.path, std::move(root));
        c.functions = holes.size();
        p.chunks.push_back(std::move(c));
    }
    if (!prose.empty() || !paths.contains(p.entry))
        fail("incorrect GPT text length or missing entry");
    r.finished();
    return p;
}
} // namespace
Package decode_package(Sections s) { return parse(s, nullptr); }
Package read_dll_content(View image, bool mapped, Sections *out, draft::Program *program) {
    bool exe;
    auto s = image_sections(image, mapped, exe);
    auto p = parse(s, program);
    verify_image(image, mapped, exports(p, exe), lua_profile(exe, p.host));
    if (out)
        *out = s;
    return p;
}
Package read_image(View image, bool mapped, Sections *out) {
    return read_dll_content(image, mapped, out, nullptr);
}
} // namespace gpt::lua54
namespace gpt::draft {
Program load(View image, bool mapped) {
    Program program;
    auto p = lua54::read_dll_content(image, mapped, nullptr, &program);
    program.entry = p.entry;
    program.symbol = p.symbol;
    program.grammar = p.grammar;
    return program;
}
} // namespace gpt::draft
