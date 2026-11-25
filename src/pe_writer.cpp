#include "assembly.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <numeric>

namespace gpt {
namespace {
constexpr U32 page = 0x1000, disk_align = 0x200, bridge_size = 24;
constexpr U32 read_data = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
U32 align_up(U32 n, U32 by) {
    return (n + by - 1) & ~(by - 1);
}
template <typename T> void put(Bytes &out, size_t at, const T &value) {
    if (at > out.size() || sizeof(T) > out.size() - at)
        fail("internal PE layout error");
    std::memcpy(out.data() + at, &value, sizeof(value));
}
U32 append_name(Bytes &bytes, std::string_view s) {
    U32 at = static_cast<U32>(bytes.size());
    bytes.insert(bytes.end(), s.begin(), s.end());
    bytes.push_back(0);
    return at;
}
struct Section {
    const char *name;
    U32 flags;
    U32 rva;
    Bytes bytes;
};
struct Layout {
    std::vector<Section> sections;
    U32 next = page;
    U32 add(const char *name, U32 flags, Bytes bytes) {
        U32 rva = next;
        next += align_up(static_cast<U32>(bytes.size()), page);
        sections.push_back({name, flags, rva, std::move(bytes)});
        return rva;
    }
};

Bytes make_imports(U32 rva, U32 &iat_rva, PeProfile profile) {
    Bytes data(2 * sizeof(IMAGE_IMPORT_DESCRIPTOR));
    U32 dll = append_name(data, runtime_library(profile));
    data.resize(align_up(data.size(), 2));
    U32 hint = data.size();
    data.resize(data.size() + 2);
    append_name(data, runtime_entry(profile));
    U32 lookup = align_up(data.size(), 8), addresses = lookup + 16;
    data.resize(addresses + 16);
    put(data, lookup, std::uint64_t(rva + hint));
    put(data, addresses, std::uint64_t(rva + hint));
    IMAGE_IMPORT_DESCRIPTOR descriptor{};
    descriptor.OriginalFirstThunk = rva + lookup;
    descriptor.Name = rva + dll;
    descriptor.FirstThunk = iat_rva = rva + addresses;
    put(data, 0, descriptor);
    return data;
}

Bytes make_exports(const Module &m, U32 rva, U32 code) {
    const U32 n = m.methods.size();
    U32 addresses = sizeof(IMAGE_EXPORT_DIRECTORY), names = addresses + 4 * n, ordinals = names + 4 * n;
    Bytes bytes(ordinals + 2 * n);
    IMAGE_EXPORT_DIRECTORY dir{};
    dir.Name = rva + append_name(bytes, "gptassembly.dll");
    dir.Base = 1;
    dir.NumberOfFunctions = dir.NumberOfNames = n;
    dir.AddressOfFunctions = rva + addresses;
    dir.AddressOfNames = rva + names;
    dir.AddressOfNameOrdinals = rva + ordinals;
    std::vector<U32> order(n);
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(),
              [&](U32 a, U32 b) { return m.methods[a].name < m.methods[b].name; });
    for (U32 i = 0; i < n; ++i) {
        U32 id = order[i];
        put(bytes, addresses + 4 * id, code + id * bridge_size);
        U32 text_rva = rva + append_name(bytes, m.methods[id].name);
        put(bytes, names + 4 * i, text_rva);
        put(bytes, ordinals + 2 * i, static_cast<std::uint16_t>(id));
    }
    put(bytes, 0, dir);
    return bytes;
}

void write_bridges(Section &text, U32 iat, PeProfile profile) {
    // RCX and RDX hold the caller's u32 arguments; the dispatcher needs them in R8 and R9.
    constexpr std::uint8_t bridge[]{0x41, 0x89, 0xd1, 0x41, 0x89, 0xc8, 0xba, 0,    0, 0, 0, 0x48,
                                    0x8d, 0x0d, 0,    0,    0,    0,    0xff, 0x25, 0, 0, 0, 0};
    static_assert(sizeof bridge == bridge_size);
    for (U32 at = 0; at < text.bytes.size(); at += bridge_size) {
        U32 rva = text.rva + at;
        std::memcpy(text.bytes.data() + at, bridge, bridge_size);
        if (profile != PeProfile::integer_dll)
            text.bytes[at + 3] = 0x49; // Preserve the caller's full lua_State pointer in R8.
        put(text.bytes, at + 7, at / bridge_size);
        put(text.bytes, at + 14, U32(0) - (rva + 18)); // lea rcx, [rip + image_base]
        put(text.bytes, at + 20, iat - (rva + 24));    // jmp [rip + import slot]
    }
}
} // namespace

Bytes emit_dll(const Module &m) {
    return emit_image(m, encode(m), PeProfile::integer_dll);
}
Bytes emit_image(const Module &m, Payload payload, PeProfile profile) {
    Layout layout;
    U32 code = layout.add(".text", IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ,
                          Bytes(m.methods.size() * bridge_size));
    if (profile == PeProfile::integer_dll) {
        layout.add(".gptmeta", read_data, std::move(payload.metadata));
        layout.add(".gptil", read_data, std::move(payload.prose));
    } else {
        // Put the executable English immediately after the Windows bridge.
        layout.add(".gptil", read_data, std::move(payload.prose));
        layout.add(".gptmeta", read_data, std::move(payload.metadata));
    }
    U32 iat = 0;
    auto imports = make_imports(layout.next, iat, profile);
    U32 import_rva = layout.add(".idata", read_data | IMAGE_SCN_MEM_WRITE, std::move(imports));
    auto exports = make_exports(m, layout.next, code);
    U32 export_length = exports.size();
    U32 export_rva = layout.add(".edata", read_data, std::move(exports));
    // All executable references are RIP-relative. The relocation block has only ABSOLUTE padding.
    Bytes relocs(12);
    put(relocs, 0, code);
    put(relocs, 4, U32(12));
    U32 reloc_rva = layout.add(".reloc", read_data | IMAGE_SCN_MEM_DISCARDABLE, std::move(relocs));
    write_bridges(layout.sections.front(), iat, profile);

    IMAGE_DOS_HEADER dos{};
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfanew = sizeof(dos);
    IMAGE_NT_HEADERS64 nt{};
    nt.Signature = IMAGE_NT_SIGNATURE;
    auto &fh = nt.FileHeader;
    fh.Machine = IMAGE_FILE_MACHINE_AMD64;
    fh.NumberOfSections = static_cast<WORD>(layout.sections.size());
    fh.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    fh.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE;
    if (profile != PeProfile::lua_exe) fh.Characteristics |= IMAGE_FILE_DLL;
    auto &opt = nt.OptionalHeader;
    opt.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    opt.MajorLinkerVersion = 1;
    opt.ImageBase = 0x180000000ull;
    opt.SectionAlignment = page;
    opt.FileAlignment = disk_align;
    opt.MajorOperatingSystemVersion = 6;
    opt.MajorSubsystemVersion = 6;
    opt.SizeOfImage = layout.next;
    opt.BaseOfCode = code;
    if (profile == PeProfile::lua_exe) opt.AddressOfEntryPoint = code;
    opt.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;
    opt.DllCharacteristics = IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE |
                             IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA |
                             IMAGE_DLLCHARACTERISTICS_NX_COMPAT;
    opt.SizeOfStackReserve = opt.SizeOfHeapReserve = 0x100000;
    opt.SizeOfStackCommit = opt.SizeOfHeapCommit = page;
    opt.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT] = {export_rva, export_length};
    opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT] = {import_rva, 2 * sizeof(IMAGE_IMPORT_DESCRIPTOR)};
    opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT] = {iat, 16};
    opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC] = {reloc_rva, 12};

    U32 table = sizeof(dos) + sizeof(nt);
    opt.SizeOfHeaders =
        align_up(table + fh.NumberOfSections * sizeof(IMAGE_SECTION_HEADER), disk_align);
    Bytes file(opt.SizeOfHeaders);
    for (const auto &s : layout.sections) {
        IMAGE_SECTION_HEADER h{};
        std::memcpy(h.Name, s.name, std::strlen(s.name));
        h.Misc.VirtualSize = s.bytes.size();
        h.VirtualAddress = s.rva;
        h.PointerToRawData = file.size();
        h.SizeOfRawData = align_up(h.Misc.VirtualSize, disk_align);
        h.Characteristics = s.flags;
        if (s.flags & IMAGE_SCN_CNT_CODE)
            opt.SizeOfCode += h.SizeOfRawData;
        else
            opt.SizeOfInitializedData += h.SizeOfRawData;
        put(file, table, h);
        table += sizeof(h);
        file.insert(file.end(), s.bytes.begin(), s.bytes.end());
        file.resize(h.PointerToRawData + h.SizeOfRawData);
    }
    put(file, 0, dos);
    put(file, sizeof(dos), nt);
    return file;
}
} // namespace gpt
