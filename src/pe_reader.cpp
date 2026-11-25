#include "assembly.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cstring>

namespace gpt {
namespace {
View slice(View s, size_t off, size_t n) {
    if (off > s.size() || n > s.size() - off)
        fail("PE range outside image");
    return s.subspan(off, n);
}
template <typename T> T field(View s, size_t off) {
    T value;
    std::memcpy(&value, slice(s, off, sizeof(T)).data(), sizeof(T));
    return value;
}
struct Region {
    std::string name;
    U32 rva, offset, size, flags;
};
class Image {
    View bytes_;
    std::vector<Region> regions_;

  public:
    IMAGE_OPTIONAL_HEADER64 opt{};
    bool is_exe = false;
    Image(View bytes, bool mapped) : bytes_(bytes) {
        auto dos = field<IMAGE_DOS_HEADER>(bytes, 0);
        if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0)
            fail("not a PE file");
        size_t nt = dos.e_lfanew;
        if (field<DWORD>(bytes, nt) != IMAGE_NT_SIGNATURE)
            fail("not a PE file");
        auto fh = field<IMAGE_FILE_HEADER>(bytes, nt + 4);
        size_t at = nt + 4 + sizeof(fh);
        if (fh.SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER32) &&
            field<WORD>(bytes, at) == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
            auto pe32 = field<IMAGE_OPTIONAL_HEADER32>(bytes, at);
            if (pe32.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR &&
                pe32.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR].VirtualAddress)
                fail("CLR assembly, expected GPT Assembly v1");
        }
        is_exe = !(fh.Characteristics & IMAGE_FILE_DLL);
        if (!(fh.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) || fh.Machine != IMAGE_FILE_MACHINE_AMD64)
            fail("not a Windows x64 GPT image");
        if (fh.SizeOfOptionalHeader < sizeof(opt))
            fail("incomplete PE optional header");
        opt = field<IMAGE_OPTIONAL_HEADER64>(bytes, at);
        if (opt.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || opt.NumberOfRvaAndSizes < 16)
            fail("not a GPT PE32+ module");
        if (opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR].VirtualAddress)
            fail("CLR assembly, expected GPT Assembly v1");
        at += fh.SizeOfOptionalHeader;
        if (!fh.NumberOfSections || fh.NumberOfSections > 96)
            fail("invalid PE section count");
        if (at + fh.NumberOfSections * sizeof(IMAGE_SECTION_HEADER) > opt.SizeOfHeaders ||
            opt.SizeOfHeaders > bytes.size() || opt.SizeOfHeaders > opt.SizeOfImage ||
            (mapped && opt.SizeOfImage > bytes.size()))
            fail("invalid PE header extent");
        std::vector<std::pair<size_t, size_t>> raw_ranges;
        for (unsigned i = 0; i < fh.NumberOfSections; ++i) {
            auto h = field<IMAGE_SECTION_HEADER>(bytes, at);
            at += sizeof(h);
            const char *name = reinterpret_cast<const char *>(h.Name);
            auto len = std::find(name, name + 8, '\0') - name;
            if (h.VirtualAddress < opt.SizeOfHeaders || h.VirtualAddress > opt.SizeOfImage ||
                h.Misc.VirtualSize > opt.SizeOfImage - h.VirtualAddress)
                fail("invalid virtual section range");
            for (const auto &other : regions_)
                if (other.rva < size_t(h.VirtualAddress) + h.Misc.VirtualSize &&
                    h.VirtualAddress < size_t(other.rva) + other.size)
                    fail("overlapping PE sections");
            if (!mapped && h.SizeOfRawData) {
                slice(bytes, h.PointerToRawData, h.SizeOfRawData);
                const size_t begin = h.PointerToRawData, end = begin + h.SizeOfRawData;
                if (begin < opt.SizeOfHeaders)
                    fail("section overlaps PE headers");
                for (auto [start, stop] : raw_ranges)
                    if (begin < stop && start < end)
                        fail("overlapping raw sections");
                raw_ranges.emplace_back(begin, end);
            }
            regions_.push_back(
                {std::string(name, len), h.VirtualAddress,
                 mapped ? h.VirtualAddress : h.PointerToRawData,
                 mapped ? h.Misc.VirtualSize : std::min(h.Misc.VirtualSize, h.SizeOfRawData),
                 h.Characteristics});
            if (!mapped &&
                (std::string_view(name, len) == ".gptil" ||
                 std::string_view(name, len) == ".gptmeta") &&
                h.SizeOfRawData < h.Misc.VirtualSize)
                fail("truncated GPT section");
        }
    }
    const Region &section(std::string_view name) const {
        const Region *found = nullptr;
        for (const auto &s : regions_)
            if (s.name == name) {
                if (found)
                    fail("duplicate PE section " + std::string(name));
                found = &s;
            }
        if (!found)
            fail("missing PE section " + std::string(name));
        return *found;
    }
    View at(U32 rva, size_t n) const {
        for (const auto &s : regions_)
            if (rva >= s.rva && rva - s.rva < s.size) {
                auto delta = rva - s.rva;
                if (!(s.flags & IMAGE_SCN_MEM_READ) || (s.flags & IMAGE_SCN_MEM_DISCARDABLE))
                    fail("reference to unreadable or discardable PE section");
                if (n > s.size - delta)
                    fail("PE reference crosses section boundary");
                return slice(bytes_, size_t(s.offset) + delta, n);
            }
        fail("PE reference outside sections");
    }
    View data(std::string_view name) const {
        const auto &s = section(name);
        if (!(s.flags & IMAGE_SCN_CNT_INITIALIZED_DATA) || !(s.flags & IMAGE_SCN_MEM_READ) ||
            (s.flags & (IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_DISCARDABLE)))
            fail("GPT sections must be read-only data");
        return slice(bytes_, s.offset, s.size);
    }
    void validate_bridges(const Module &m, PeProfile profile) const {
        if (is_exe != (profile == PeProfile::lua_exe) ||
            opt.AddressOfEntryPoint != (is_exe ? section(".text").rva : 0) ||
            opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress)
            fail("unexpected native initialization in GPT module");
        const auto &code = section(".text");
        if (!(code.flags & IMAGE_SCN_MEM_EXECUTE) || (code.flags & IMAGE_SCN_MEM_WRITE) ||
            code.size != m.methods.size() * 24)
            fail("unexpected native bridge section");
        for (const auto &s : regions_)
            if (s.name != ".text" && (s.flags & IMAGE_SCN_MEM_EXECUTE))
                fail("extra executable section");
        auto dir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (dir.Size != 2 * sizeof(IMAGE_IMPORT_DESCRIPTOR))
            fail("expected one runtime import descriptor");
        auto imports = at(dir.VirtualAddress, dir.Size);
        auto imp = field<IMAGE_IMPORT_DESCRIPTOR>(imports, 0);
        if (std::ranges::any_of(imports.subspan(sizeof(imp)), [](auto byte) { return byte != 0; }))
            fail("extra import descriptor");
        auto string_at = [&](U32 address, std::string_view expected) {
            auto b = at(address, expected.size() + 1);
            return b.back() == 0 && !std::memcmp(b.data(), expected.data(), expected.size());
        };
        auto lookup = at(imp.OriginalFirstThunk, 16);
        auto hint = field<std::uint64_t>(lookup, 0);
        if (!string_at(imp.Name, runtime_library(profile)) || hint > UINT32_MAX - 2 ||
            !string_at(U32(hint) + 2, runtime_entry(profile)) || field<std::uint64_t>(lookup, 8) != 0)
            fail("invalid GPT runtime import");
        auto iat = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT];
        if (iat.VirtualAddress != imp.FirstThunk || iat.Size != 16)
            fail("unexpected runtime import slot");

        auto ex = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        auto exports =
            field<IMAGE_EXPORT_DIRECTORY>(at(ex.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY)), 0);
        U32 count = m.methods.size();
        if (exports.NumberOfFunctions != count || exports.NumberOfNames != count)
            fail("export count mismatch");
        auto names = at(exports.AddressOfNames, count * 4),
             ords = at(exports.AddressOfNameOrdinals, count * 2);
        auto entries = at(exports.AddressOfFunctions, count * 4);
        std::string_view previous;
        for (U32 index = 0; index < count; ++index) {
            U32 id = field<std::uint16_t>(ords, 2 * index);
            if (id >= count)
                fail("invalid native export ordinal");
            const auto &name = m.methods[id].name;
            if (name <= previous || !string_at(field<U32>(names, 4 * index), name))
                fail("native/GPT export mismatch");
            previous = name;
            const U32 rva = field<U32>(entries, 4 * id);
            if (rva != code.rva + id * 24)
                fail("native export does not target its bridge");
            auto b = at(rva, 24);
            std::uint8_t registers[]{0x41, 0x89, 0xd1, 0x41, 0x89, 0xc8, 0xba};
            if (profile != PeProfile::integer_dll) registers[3] = 0x49;
            if (std::memcmp(b.data(), registers, sizeof(registers)) || field<U32>(b, 7) != id ||
                b[11] != 0x48 || b[12] != 0x8d || b[13] != 0x0d || field<U32>(b, 14) + rva + 18 != 0 ||
                b[18] != 0xff || b[19] != 0x25 || field<U32>(b, 20) + rva + 24 != imp.FirstThunk)
                fail(name + ": invalid native forwarding bridge");
        }
    }
};
} // namespace
Module read_dll(View bytes, bool mapped, std::vector<std::string_view> *bodies) {
    Image pe(bytes, mapped);
    auto m = decode({pe.data(".gptmeta"), pe.data(".gptil")}, bodies);
    pe.validate_bridges(m, PeProfile::integer_dll);
    return m;
}
Sections image_sections(View image, bool mapped, bool &is_exe) {
    Image pe(image, mapped);
    is_exe = pe.is_exe;
    return {pe.data(".gptmeta"), pe.data(".gptil")};
}
void verify_image(View image, bool mapped, const Module &exports, PeProfile profile) {
    Image(image, mapped).validate_bridges(exports, profile);
}
} // namespace gpt
