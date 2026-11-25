#include "assembly.hpp"
#include "lua_prose.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
namespace {
using namespace gpt;
Bytes read_file(const fs::path &path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        fail("cannot open input file");
    auto size = f.tellg();
    if (size < 0 || size > std::streamoff(2 * lua54::limit + 65536))
        fail("input exceeds the GPT image size limit");
    Bytes bytes(static_cast<size_t>(size));
    f.seekg(0);
    if (!bytes.empty() && !f.read(reinterpret_cast<char *>(bytes.data()), size))
        fail("input read failed");
    return bytes;
}
void save(const fs::path &path, const Bytes &bytes) {
    // Commit only a complete, verified image, on the destination volume.
    auto folder = fs::absolute(path).parent_path();
    wchar_t temporary[MAX_PATH + 1]{};
    if (!GetTempFileNameW(folder.c_str(), L"gpt", 0, temporary))
        fail("cannot create output beside destination");
    struct Cleanup {
        const wchar_t *file;
        ~Cleanup() {
            DeleteFileW(file);
        }
    } cleanup{temporary};
    std::ofstream out(fs::path(temporary), std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out)
        fail("could not finish writing DLL");
    if (!MoveFileExW(temporary, path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        fail("could not replace output DLL (it may be loaded)");
}
std::string ascii(std::wstring_view w) {
    std::string result;
    for (wchar_t c : w) {
        if (c > 127)
            fail("method names and integer arguments must be ASCII");
        result.push_back(static_cast<char>(c));
    }
    return result;
}
U32 integer(std::wstring_view w) {
    auto s = ascii(w);
    U32 v = 0;
    auto [end, err] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (err != std::errc{} || end != s.data() + s.size())
        fail("expected decimal u32");
    return v;
}

void trace_method(const Method &method, std::string_view body, U32 a, U32 b) {
    std::vector<std::string_view> paragraphs;
    while (true) {
        size_t end = body.find("\n\n");
        paragraphs.push_back(body.substr(0, end));
        if (end == std::string_view::npos)
            break;
        body.remove_prefix(end + 2);
    }
    std::cout << method.name << "(" << a << ", " << b << ")\n";
    size_t executed = 0;
    auto report = [&](size_t step, size_t next, std::span<const U32> stack) {
        ++executed;
        std::cout << "\n[" << step + 1 << "] " << paragraphs[step] << "\n    stack [";
        // Keep deep expression stacks readable. The top value is always on the right.
        size_t first = stack.size() > 6 ? stack.size() - 6 : 0;
        if (first)
            std::cout << "..., ";
        for (size_t i = first; i < stack.size(); ++i) {
            if (i != first)
                std::cout << ", ";
            std::cout << stack[i];
        }
        std::cout << "]";
        if (next > step + 1)
            std::cout << " | skipped paragraphs " << step + 2 << ".." << next;
        std::cout << "\n";
    };
    U32 result = evaluate(method, a, b, report);
    std::cout << "\nreturn " << result << " | executed " << executed << "/" << paragraphs.size()
              << " paragraphs\n";
}

int command(int argc, wchar_t **argv) {
    if (argc >= 3 && std::wstring_view(argv[1]) == L"compile" &&
        _wcsicmp(fs::path(argv[2]).extension().c_str(), L".lua") == 0) {
        fs::path output, root;
        std::vector<fs::path> module_roots;
        std::string symbol;
        for (int i = 3; i < argc; i += 2) {
            if (i + 1 >= argc) fail("missing compiler option value");
            std::wstring_view option = argv[i];
            if (option == L"-o") output = argv[i + 1];
            else if (option == L"--root") root = argv[i + 1];
            else if (option == L"--modules") module_roots.emplace_back(argv[i + 1]);
            else if (option == L"--symbol") symbol = ascii(argv[i + 1]);
            else fail("unknown Lua compiler option");
        }
        bool exe = !_wcsicmp(output.extension().c_str(), L".exe");
        if (!exe && _wcsicmp(output.extension().c_str(), L".dll")) fail("expected -o FILE.exe or FILE.dll");
        if (symbol.empty() && exe) symbol = "gpt_main";
        if (symbol.empty()) {
            auto stem = output.stem().wstring();
            stem = stem.substr(0, stem.find(L'-'));
            for (auto &ch : stem) if (ch == L'.') ch = L'_';
            symbol = "luaopen_" + ascii(stem);
        }
        auto image = lua54::compile_project(argv[2], root, module_roots, symbol, exe);
        save(output, image);
        std::cout << "Wrote GPT Lua 5.4 (" << image.size() << " bytes, verified against Lua 5.4.9)\n";
        return 0;
    }
    if (argc == 5 && std::wstring_view(argv[1]) == L"compile" && std::wstring_view(argv[3]) == L"-o") {
        fs::path input = argv[2], output = argv[4];
        if (_wcsicmp(output.extension().c_str(), L".dll") != 0)
            fail("output must use .dll");
        if (fs::exists(output) && fs::equivalent(input, output))
            fail("source and output must be different files");
        auto bytes = read_file(input);
        std::string_view source =
            bytes.empty()
                ? std::string_view{}
                : std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        auto module = compile(source);
        auto image = emit_dll(module);
        read_dll(image, false);
        save(output, image);
        std::cout << "Wrote GPT Assembly v1 (" << module.methods.size() << " methods, " << image.size()
                  << " bytes)\n";
        return 0;
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"inspect") {
        auto image = read_file(argv[2]);
        bool exe;
        auto sections = image_sections(image, false, exe);
        auto &meta = sections.metadata;
        if (meta.size() >= lua54::signature.size() &&
            std::equal(lua54::signature.begin(), lua54::signature.end(), meta.begin())) {
            auto p = lua54::read_image(image, false);
            std::cout << "GPT Lua 5.4 | English grammar v1 | Windows x64 " << (exe ? "EXE" : "DLL")
                      << " | entry " << p.entry << " | export " << p.symbol << "\n";
            for (const auto &c : p.chunks)
                std::cout << c.path << " | " << c.functions << " functions | " << c.paragraphs << " paragraphs\n";
            std::cout << "\n";
            std::cout.write(reinterpret_cast<const char*>(sections.prose.data()),sections.prose.size());
            return 0;
        }
        std::vector<std::string_view> bodies;
        auto module = read_dll(image, false, &bodies);
        std::cout << "GPT Assembly v1 | English grammar v" << module.grammar << " | Windows x64\n";
        for (size_t i = 0; i < module.methods.size(); ++i)
            std::cout << "\n[" << i << "] u32 " << module.methods[i].name << "(u32, u32), "
                      << bodies[i].size() << " UTF-8 bytes\n\n"
                      << bodies[i] << "\n";
        return 0;
    }
    if (argc == 6 && (std::wstring_view(argv[1]) == L"run" || std::wstring_view(argv[1]) == L"trace")) {
        bool tracing = std::wstring_view(argv[1]) == L"trace";
        auto image = read_file(argv[2]);
        std::vector<std::string_view> bodies;
        auto module = read_dll(image, false, tracing ? &bodies : nullptr);
        auto name = ascii(argv[3]);
        for (size_t i = 0; i < module.methods.size(); ++i) {
            const Method &m = module.methods[i];
            if (m.name == name) {
                U32 a = integer(argv[4]), b = integer(argv[5]);
                if (tracing)
                    trace_method(m, bodies[i], a, b);
                else
                    std::cout << evaluate(m, a, b) << "\n";
                return 0;
            }
        }
        fail("unknown method: " + name);
    }
    std::cerr
        << "gptc compile SOURCE -o MODULE.dll\ngptc inspect MODULE.dll\ngptc run MODULE.dll NAME A B\n"
        << "gptc trace MODULE.dll NAME A B\ngptc compile ENTRY.lua [--root PROJECT] [--modules DIR ...] [--symbol luaopen_NAME] -o OUTPUT.dll|OUTPUT.exe\n";
    return 2;
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    SetConsoleOutputCP(CP_UTF8);
    try {
        return command(argc, argv);
    } catch (const std::exception &e) {
        std::cerr << "gptc: " << e.what() << "\n";
        return 1;
    }
}
