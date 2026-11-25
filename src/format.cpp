#include "assembly.hpp"

#include <algorithm>
#include <cstring>
#include <set>

namespace gpt {
Method read_method(std::string name, std::string_view body, unsigned grammar);

namespace {
class Cursor {
    View rest_;

  public:
    explicit Cursor(View bytes) : rest_(bytes) {}
    View take(size_t n) {
        if (n > rest_.size())
            fail("truncated GPT method table");
        auto v = rest_.first(n);
        rest_ = rest_.subspan(n);
        return v;
    }
    U32 number(size_t n) {
        U32 out = 0;
        auto v = take(n);
        for (size_t i = 0; i < n; ++i)
            out |= U32(v[i]) << (8 * i);
        return out;
    }
    bool empty() const {
        return rest_.empty();
    }
};
void number(Bytes &dest, U32 value, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i)
        dest.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}
std::string_view chars(View v) {
    return {reinterpret_cast<const char *>(v.data()), v.size()};
}
constexpr std::array<std::uint8_t, 4> signature{1, 2, 1, 1};
} // namespace

Payload encode(const Module &module) {
    if (module.methods.empty() || module.methods.size() > method_limit)
        fail("invalid method count");
    Payload p;
    if (module.grammar != 1 && module.grammar != 2)
        fail("unsupported English grammar version");
    p.metadata.assign(magic.begin(), magic.end());
    number(p.metadata, 1, 2);
    number(p.metadata, module.grammar, 2);
    number(p.metadata, module.methods.size(), 4);
    std::set<std::string> names;
    for (size_t id = 0; id < module.methods.size(); ++id) {
        const Method &m = module.methods[id];
        if (!identifier(m.name) || m.name.size() > 65535 || !names.insert(m.name).second)
            fail("invalid or duplicate export name: " + m.name);
        verify(m);
        size_t start = p.prose.size();
        for (auto i : m.code) {
            if (grammar_for(i.op) > module.grammar)
                fail(m.name + ": instruction requires English grammar v2");
            auto s = paragraph(i);
            if (p.prose.size() != start)
                p.prose.insert(p.prose.end(), {'\n', '\n'});
            if (p.prose.size() + s.size() > section_limit)
                fail("GPT body section too large");
            p.prose.insert(p.prose.end(), s.begin(), s.end());
        }
        number(p.metadata, id, 4);
        number(p.metadata, start, 4);
        number(p.metadata, p.prose.size() - start, 4);
        number(p.metadata, m.name.size(), 2);
        p.metadata.insert(p.metadata.end(), signature.begin(), signature.end());
        p.metadata.insert(p.metadata.end(), m.name.begin(), m.name.end());
        if (p.metadata.size() > section_limit)
            fail("GPT metadata section too large");
    }
    return p;
}

Module decode(Sections s, std::vector<std::string_view> *bodies) {
    if (s.metadata.size() > section_limit || s.prose.size() > section_limit)
        fail("oversized GPT section");
    Cursor in(s.metadata);
    if (chars(in.take(magic.size())) != magic)
        fail("invalid GPT assembly marker");
    if (in.number(2) != 1)
        fail("unsupported GPT format version");
    Module module;
    module.grammar = in.number(2);
    if (module.grammar != 1 && module.grammar != 2)
        fail("unsupported English grammar version");
    U32 count = in.number(4);
    if (!count || count > method_limit)
        fail("invalid GPT method count");
    std::set<std::string> names;
    size_t next = 0;
    for (U32 id = 0; id < count; ++id) {
        if (in.number(4) != id)
            fail("invalid GPT method ID");
        U32 start = in.number(4), size = in.number(4), name_size = in.number(2);
        if (!std::ranges::equal(in.take(4), signature))
            fail("unsupported GPT method signature");
        std::string name(chars(in.take(name_size)));
        if (!identifier(name) || !names.insert(name).second)
            fail("invalid or duplicate GPT method name");
        if (start != next || !size || size > s.prose.size() - next)
            fail(name + ": invalid body range");
        auto body = chars(s.prose.subspan(start, size));
        module.methods.push_back(read_method(name, body, module.grammar));
        if (bodies)
            bodies->push_back(body);
        next += size;
    }
    if (!in.empty() || next != s.prose.size())
        fail("unclaimed GPT payload bytes");
    return module;
}
} // namespace gpt
