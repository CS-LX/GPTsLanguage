#include "lua_prose.hpp"
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
    void string() {
        auto n = var();
        if (n)
            get(n - 1);
    }
    void finished() {
        if (at != bytes.size())
            fail("unexpected trailing Lua data");
    }
};
struct Code {
    size_t offset, count;
    bool operator==(const Code &) const = default;
};
// Follow ldump.c's prototype layout. Only the actual instruction arrays become holes.
void prototype(Reader &r, std::vector<Code> &holes, unsigned depth) {
    if (depth > 250)
        fail("Lua prototype nesting exceeds reader limit");
    r.string();
    r.var();
    r.var();
    r.get(3);
    auto count = r.var();
    holes.push_back({r.at, count});
    r.get(count * 4);
    for (size_t n = r.var(); n; --n) {
        switch (r.byte()) {
        case 0:
        case 1:
        case 17:
            break; // nil, false, true
        case 3:
        case 19:
            r.get(8);
            break; // integer, float
        case 4:
        case 20:
            r.string();
            break;
        default:
            fail("unknown Lua constant");
        }
    }
    r.get(r.var() * 3); // upvalue descriptors
    for (size_t n = r.var(); n; --n)
        prototype(r, holes, depth + 1);
    r.get(r.var()); // line deltas
    for (size_t n = r.var(); n; --n) {
        r.var();
        r.var();
    }
    for (size_t n = r.var(); n; --n) {
        r.string();
        r.var();
        r.var();
    }
    for (size_t n = r.var(); n; --n)
        r.string();
}
std::vector<Code> code_ranges(View bytes) {
    Reader r{bytes};
    if (!std::ranges::equal(r.get(sizeof header), header))
        fail("unsupported Lua chunk ABI");
    r.byte(); // root closure's upvalue count
    std::vector<Code> ranges;
    prototype(r, ranges, 0);
    r.finished();
    return ranges;
}
} // namespace
Payload encode_package(const Package &p) {
    Payload result;
    auto &meta = result.metadata, &prose = result.prose;
    append(meta, signature);
    put(meta, 1);
    name(meta, p.entry);
    name(meta, p.symbol);
    put(meta, p.chunks.size());
    for (const auto &c : p.chunks) {
        name(meta, c.path);
        auto ranges = code_ranges(c.bytecode);
        put(meta, ranges.size());
        size_t end = sizeof header;
        View bytes = c.bytecode;
        for (auto [start, count] : ranges) {
            blob(meta, bytes.subspan(end, start - end));
            put(meta, count);
            Reader words{bytes.subspan(start, count * 4)};
            for (size_t i = 0; i < count; ++i) {
                append(prose, narrate(words.number()));
                append(prose, "\n\n");
            }
            end = start + count * 4;
        }
        blob(meta, bytes.subspan(end));
    }
    return result;
}
Package decode_package(Sections sections) {
    if (sections.metadata.size() > limit || sections.prose.size() > limit)
        fail("oversized GPT Lua package");
    Reader r{sections.metadata};
    if (chars(r.get(signature.size())) != signature || r.number() != 1)
        fail("unsupported GPT Lua grammar");
    Package p;
    p.entry = chars(r.blob());
    p.symbol = chars(r.blob());
    if (!identifier(p.symbol))
        fail("invalid Lua export symbol");
    std::set<std::string> paths;
    auto prose = chars(sections.prose);
    for (U32 nc = r.number(); nc; --nc) {
        Chunk c;
        c.path = chars(r.blob());
        if (c.path.empty() || c.path.find('\0') != c.path.npos || !paths.insert(c.path).second)
            fail("invalid or duplicate Lua chunk path");
        append(c.bytecode, header);
        std::vector<Code> ranges;
        for (U32 nh = r.number(); nh; --nh) {
            append(c.bytecode, r.blob());
            auto count = r.number();
            ranges.push_back({c.bytecode.size(), count});
            c.paragraphs += count;
            for (U32 i = 0; i < count; ++i) {
                auto end = prose.find("\n\n");
                if (end == prose.npos)
                    fail("missing Lua instruction paragraph");
                put(c.bytecode, assemble(prose.substr(0, end)));
                prose.remove_prefix(end + 2);
            }
        }
        append(c.bytecode, r.blob());
        if (code_ranges(c.bytecode) != ranges)
            fail("Lua prototype and prose ranges disagree");
        c.functions = ranges.size();
        p.chunks.push_back(std::move(c));
    }
    if (!prose.empty() || !paths.contains(p.entry))
        fail("invalid Lua entry or extra prose");
    r.finished();
    return p;
}
Package read_image(View image, bool mapped, Sections *out) {
    bool exe;
    auto sections = image_sections(image, mapped, exe);
    auto p = decode_package(sections);
    verify_image(image, mapped, exports(p.symbol), exe ? PeProfile::lua_exe : PeProfile::lua_dll);
    if (out)
        *out = sections;
    return p;
}
} // namespace gpt::lua54
