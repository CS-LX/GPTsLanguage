"""End-to-end Windows acceptance checks, using a native compiler as an independent oracle."""
import argparse
import contextlib
import ctypes
import os
from pathlib import Path
import random
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
U32_MAX = 2**32 - 1


def run(*args, success=True, **kwargs):
    result = subprocess.run(args, capture_output=True, encoding="utf-8", **kwargs)
    if (result.returncode == 0) != success:
        raise AssertionError(f"{args}: exit {result.returncode}\n{result.stdout}{result.stderr}")
    return result.stdout if success else result.stderr


def pe_layout(data):
    nt, = struct.unpack_from("<I", data, 60)
    assert data[nt:nt+4] == b"PE\0\0"
    count, = struct.unpack_from("<H", data, nt+6)
    opt_size, = struct.unpack_from("<H", data, nt+20)
    sections = {}
    for i in range(count):
        at = nt + 24 + opt_size + i * 40
        name = data[at:at+8].rstrip(b"\0").decode("ascii")
        virtual_size, _, _, raw_offset = struct.unpack_from("<4I", data, at+8)
        sections[name] = (raw_offset, virtual_size)
    return nt+24, sections


def inspect_payload(file, compiler):
    data = file.read_bytes()
    opt, sections = pe_layout(data)
    assert struct.unpack_from("<H", data, opt)[0] == 0x20B
    assert struct.unpack_from("<II", data, opt+112+14*8) == (0, 0)
    assert sections[".text"][1] == 2*24
    meta, meta_length = sections[".gptmeta"]
    text, text_length = sections[".gptil"]
    assert struct.unpack_from("<8sHHI", data, meta) == (b"GPTASM\r\n", 1, 1, 2)
    cursor = meta + 16
    end_body = 0
    methods = {}
    for index in range(2):
        method_id, start, size, name_len, *types = struct.unpack_from("<IIIH4B", data, cursor)
        cursor += 18
        assert (method_id, start, types) == (index, end_body, [1, 2, 1, 1])
        name = data[cursor:cursor+name_len].decode("ascii")
        cursor += name_len
        methods[name] = data[text+start:text+start+size]
        end_body += size
    assert cursor == meta+meta_length and end_body == text_length
    assert list(methods) == ["distance_u32", "add_u32"]
    listing = run(compiler, "inspect", file)
    for body in methods.values():
        assert body.decode("utf-8") in listing
    assert "·" in listing and "—" in listing and "paragraph 9." in listing
    return data, text, methods["distance_u32"]


@contextlib.contextmanager
def library(path):
    # Own exactly one Windows loader reference, even when loading two GPT images at the same base preference.
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.LoadLibraryW.argtypes = [ctypes.c_wchar_p]
    kernel.LoadLibraryW.restype = ctypes.c_void_p
    kernel.GetProcAddress.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    kernel.GetProcAddress.restype = ctypes.c_void_p
    kernel.FreeLibrary.argtypes = [ctypes.c_void_p]
    handle = kernel.LoadLibraryW(str(path))
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        def symbol(name):
            address = kernel.GetProcAddress(handle, name.encode("ascii"))
            assert address, name
            return ctypes.CFUNCTYPE(ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32,
                                    use_last_error=True)(address)
        yield symbol
    finally:
        assert kernel.FreeLibrary(handle)


VARIANT = """#include <stdint.h>
extern "C" uint32_t adjusted(uint32_t a, uint32_t b) {
    if (a != b) {
        if (a < b) return (a + 7u) - b;
        else return a + (b - 2u);
    }
    return 19u;
}
extern "C" uint32_t ordered(uint32_t a, uint32_t b) {
    if (a > b) {}
    else if (a <= 123u) return a == b;
    return a - b + 2u;
}
extern "C" uint32_t signed_order(uint32_t a, uint32_t b) {
    return ((a == b) - 1) < 0;
}
"""


def compare(cc, source, path, module, symbols, inputs):
    run(cc, "-shared", "-Wl,--export-all-symbols", source, "-o", path)
    with library(path) as native, library(module) as gpt:
        for name in symbols:
            oracle, actual = native(name), gpt(name)
            for a, b in inputs:
                expected = oracle(a, b)
                ctypes.set_last_error(123)
                result = actual(a, b)
                assert (result, ctypes.get_last_error()) == (expected, 0), (name, a, b, result, expected)


def check_short_circuits(dist, temp, cc, inputs):
    compiler, host, dll = dist/"gptc.exe", dist/"sample-host.exe", dist/"caveats.dll"
    raw = dll.read_bytes()
    _, sections = pe_layout(raw)
    meta, _ = sections[".gptmeta"]
    assert struct.unpack_from("<HH", raw, meta+8) == (1, 2)
    assert "English grammar v2" in run(compiler, "inspect", dll)
    assert "English grammar v1" in run(compiler, "inspect", dist/"sample.dll")
    for name, a, b, decisive, expected in [
        ("worth_a_followup", 0, 99, True, 0), ("worth_a_followup", 1, 99, False, 1),
        ("worth_a_followup", 7, 1, False, 0), ("enough_to_publish", 7, 0, True, 1),
        ("enough_to_publish", 0, 99, False, 1), ("enough_to_publish", 0, 1, False, 0),
    ]:
        trace = run(compiler, "trace", dll, name, str(a), str(b))
        steps = re.findall(r"^\[(\d+)\] (.+)$", trace, re.M)
        assert [int(n) for n, _ in steps] == ([1, 2, 7] if decisive else list(range(1, 8)))
        assert all(prose.encode("utf-8") in raw for _, prose in steps)
        assert ("place argument 2 on the stack" in trace) != decisive
        assert ("skipped paragraphs 3..6" in trace) == decisive
        assert f"return {expected} | executed {len(steps)}/7 paragraphs" in trace
        assert run(host, dll, name, str(a), str(b)).strip().endswith(f" = {expected}")
    print("AND and OR trace only the evaluated operands; each displayed paragraph is present in the DLL.")

    bad = temp/"wrong-grammar.dll"
    wrong = bytearray(raw)
    struct.pack_into("<H", wrong, meta+10, 1)
    bad.write_bytes(wrong)
    for command in [(compiler, "inspect", bad), (host, bad, "worth_a_followup", "0", "0")]:
        assert "worth_a_followup, paragraph 2: instruction requires English grammar v2" in run(*command, success=False)

    source, module = temp/"conditions.c", temp/"conditions.dll"
    expressions = {
        "and_raw": "a && b", "or_raw": "a || b", "not_raw": "!!a + !b",
        "precedence": "a == 9 || b == 2 && a + 1u > b",
        "nested": "(b && (a || b)) + (a || (b && a))",
        "signed_order": "((a == b) - 1) < ((a || b) && (b || a))",
        "left_gate_order": "(a && b) > ((b == 0) - 1)",
        "both_sides": "(a && b) <= (a || b)",
        "negated": "!(a && !b) && !(b || !a)",
    }
    source.write_text("".join(f"unsigned int {name}(unsigned int a, unsigned int b) {{ return {expr}; }}\n"
                              for name, expr in expressions.items()), encoding="utf-8")
    run(compiler, "compile", source, "-o", module)
    with library(dist/"gptrt.dll"):
        compare(cc, source, temp/"logical-oracle.dll", module, expressions, inputs)
    print(f"Matched native short-circuit, signed ordering and precedence on {len(expressions)*len(inputs)} calls.")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("bin", type=Path)
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    dist = args.bin.resolve()
    compiler, host, sample = dist/"gptc.exe", dist/"sample-host.exe", dist/"sample.dll"
    source = ROOT/"examples/sample.c"
    data, start, body = inspect_payload(sample, compiler)

    def call(file, a, b):
        return run(host, file, "distance_u32", str(a), str(b)).strip()

    with tempfile.TemporaryDirectory(prefix="GPT Assembly ") as folder:
        temp = Path(folder)
        rebuilt = temp/"sample rebuilt.dll"
        run(compiler, "compile", source, "-o", rebuilt, cwd=temp,
            env={**os.environ, "PATH": str(Path(os.environ["SystemRoot"])/"System32")})
        assert rebuilt.read_bytes() == data
        print("Standalone compiler: rebuilding the C source reproduced every DLL byte.")

        def edit(name, old, new):
            assert len(old) == len(new)
            offset = start + body.index(old)
            copy = temp/name
            copy.write_bytes(data[:offset]+new+data[offset+len(old):])
            changed = copy.read_bytes()
            assert changed[:offset] == data[:offset] and changed[offset+len(old):] == data[offset+len(old):]
            return copy

        edited = edit("changed.dll", b"decrease", b"increase")
        for a, b, before, after in [(9, 4, 5, 13), (4, 9, 5, 5), (4, 4, 0, 8)]:
            assert call(sample, a, b) == f"distance_u32({a}, {b}) = {before}"
            assert call(edited, a, b) == f"distance_u32({a}, {b}) = {after}"
            assert int(run(compiler, "run", edited, "distance_u32", str(a), str(b))) == after
            print(f"Same Windows host, ({a}, {b}): original {before}, prose-edited {after}.")

        for bad, paragraph in [(edit("word.dll", b"decrease", b"sideways"), 7),
                               (edit("target.dll", b"paragraph 9.", b"paragraph 0."), 4)]:
            expected = f"distance_u32, paragraph {paragraph}:"
            assert expected in run(compiler, "inspect", bad, success=False)
            assert expected in run(host, bad, "distance_u32", "9", "4", success=False)
        print("Inspector and loaded runtime rejected bad grammar and branch targets at paragraphs 7 and 4.")

        normal = dist/"gptrt.dll"
        managed = Path(os.environ["SystemRoot"])/"Microsoft.NET/Framework64/v4.0.30319/System.dll"
        assert managed.exists()
        clr = managed.read_bytes()
        opt, _ = pe_layout(clr)
        directories = opt + (112 if struct.unpack_from("<H", clr, opt)[0] == 0x20b else 96)
        assert all(struct.unpack_from("<II", clr, directories+14*8))
        assert "CLR assembly" in run(compiler, "inspect", managed, success=False)
        for other in [normal, ROOT/"examples/add.dll"]:
            run(compiler, "inspect", other, success=False)
        print("Rejected native DLL, actual .NET Framework assembly, and historical prose-in-a-native-DLL prototype.")

        _, sections = pe_layout(data)
        meta, _ = sections[".gptmeta"]
        for label, offset, replacement, diagnostic in [
            ("marker", meta, b"PRETND", "marker"),
            ("version", meta + 8, b"\x02", "version"),
            ("range", meta + 20, b"\x01", "body range"),
            ("signature", meta + 30, b"\x03", "signature"),
            ("bridge", sections[".text"][0] + 7, b"\x01", "forwarding bridge"),
        ]:
            bad = temp/(label+".dll")
            copy = bytearray(data)
            copy[offset:offset+len(replacement)] = replacement
            bad.write_bytes(copy)
            assert diagnostic in run(compiler, "inspect", bad, success=False)
        print("Rejected invalid format marker, version, body range, signature and method bridge.")

        native_c, native_cpp = temp/"native-c.dll", temp/"native-cpp.dll"
        new_source, new_module = temp/"renamed.cpp", temp/"renamed.dll"
        new_source.write_text(VARIANT, encoding="utf-8")
        run(compiler, "compile", new_source, "-o", new_module)
        rng = random.Random(127)
        bounds = [0, 1, 4, 9, 123, 0x7fffffff, 0x80000000, U32_MAX]
        inputs = [(a, b) for a in bounds for b in bounds] + [
            (rng.getrandbits(32), rng.getrandbits(32)) for _ in range(100)]
        with library(normal), library(sample) as orig, library(edited) as changed:
            assert orig("distance_u32")(9, 4) == 5 and changed("distance_u32")(9, 4) == 13
            compare(args.cc, source, native_c, sample, ["distance_u32", "add_u32"], inputs)
            compare(args.cxx, new_source, native_cpp, new_module, ["adjusted", "ordered", "signed_order"], inputs)
        print(f"Matched native C/C++ on {len(inputs)*5} calls to five functions, including nested branches and wraparound.")
        check_short_circuits(dist, temp, args.cc, inputs)

        new_source.write_text("unsigned int wrong(unsigned int a, unsigned int b) { return a * b; }",
                              encoding="utf-8")
        previous = new_module.read_bytes()
        assert "line 1, column" in run(compiler, "compile", new_source, "-o", new_module, success=False)
        assert previous == new_module.read_bytes()
        print("Unsupported source reports a location and preserves the prior output.")


if __name__ == "__main__":
    main()
