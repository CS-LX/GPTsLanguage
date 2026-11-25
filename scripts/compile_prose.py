"""Compile one two-argument unsigned C or C++ arithmetic function to a prose DLL."""

import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path


INTRO = (
    "Context · Inputs · Alignment. Before we go any further, it helps to establish "
    "a shared starting point. For the purposes of this discussion, the running total "
    "starts with argument {arg}."
)
ACTION = (
    "To be clear, acknowledging a second contribution does not invalidate the first. "
    "With that distinction in mind — and this is where the nuance matters — the "
    "practical takeaway is to {verb} the running total by argument {arg}."
)
RETURN = (
    "Ultimately, the caller came here with a specific question. The most helpful "
    "thing we can do at this point is close the loop and return the running total."
)
WORDS = ("one", "two")
RUNTIME = Path(__file__).resolve().with_name("prose_runtime.c")
IDENT = r"[A-Za-z_][A-Za-z_0-9]*"
# A full match keeps unimplemented GIMPLE operations out of the output format.
FUNCTION = re.compile(
    rf"unsigned int (?P<name>{IDENT}) \(unsigned int (?P<a>{IDENT}), unsigned int (?P<b>{IDENT})\)\s*"
    r"gimple_bind <\s*unsigned int (?P<result>D\.\d+);\s*"
    rf"gimple_assign <(?P<op>{IDENT}), (?P=result), (?P<left>{IDENT}|0), (?P<right>{IDENT}|2), NULL>\s*"
    r"gimple_return <(?P=result)>\s*>"
)


def lower(dump):
    fn = FUNCTION.fullmatch(dump.strip())
    if fn is None:
        raise ValueError("expected a single unsigned int function returning the sum or difference of two parameters")
    args = {fn["a"]: "one", fn["b"]: "two"}
    op, left, right = fn["op"], fn["left"], fn["right"]
    # GCC folds x + x and x - x before it emits even unoptimized GIMPLE.
    if op == "mult_expr" and right == "2":
        op, right = "plus_expr", left
    elif (op, left, right) == ("integer_cst", "0", "NULL"):
        op, left, right = "minus_expr", fn["a"], fn["a"]
    if op not in ("plus_expr", "minus_expr") or left not in args or right not in args:
        raise ValueError("supported operations are unsigned parameter addition and subtraction")
    verb = {"plus_expr": "increase", "minus_expr": "decrease"}[op]
    paragraphs = [
        INTRO.format(arg=args[left]),
        ACTION.format(verb=verb, arg=args[right]),
        RETURN,
    ]
    return fn["name"], "\n\n".join(paragraphs)


def write_module(folder, program):
    # Use ASCII escape sequences for UTF-8 bytes so the host locale is irrelevant.
    def literal(s):
        return '"' + ''.join(f"\\{c:03o}" for c in s.encode("utf-8")) + '"'

    module = (
        '#include <stddef.h>\n'
        '__attribute__((section(".gptil"), aligned(1), used))\n'
        f'const char prose_program[] = {literal(program)};\n'
        'const size_t prose_size = sizeof(prose_program) - 1;\n'
    )
    (folder / "module.c").write_text(module, encoding="ascii")
    forms = {
        "start_arg": [INTRO.format(arg=arg) for arg in WORDS],
        "add_arg": [ACTION.format(verb="increase", arg=arg) for arg in WORDS],
        "sub_arg": [ACTION.format(verb="decrease", arg=arg) for arg in WORDS],
    }
    grammar = [f"static const char *const {name}[] = {{{', '.join(map(literal, texts))}}};"
               for name, texts in forms.items()]
    grammar.append(f"static const char finish[] = {literal(RETURN)};")
    (folder / "grammar.h").write_text("\n".join(grammar), encoding="ascii")


def compile_dll(source, output):
    if source.suffix not in (".c", ".cc", ".cpp", ".cxx"):
        raise ValueError("expected a .c, .cc, .cpp or .cxx source")
    frontend = "gcc" if source.suffix == ".c" else "g++"
    if output.suffix.lower() != ".dll":
        raise ValueError("output must use the .dll extension")
    with tempfile.TemporaryDirectory(dir=output.parent) as tmp:
        folder = Path(tmp)
        subprocess.run([frontend, "-O0", "-fdump-tree-gimple-raw=input.ir", "-c",
                        str(source), "-o", "input.o"], cwd=folder, check=True)
        name, program = lower((folder / "input.ir").read_text(encoding="utf-8"))
        write_module(folder, program)
        (folder / "exports.def").write_text(f"EXPORTS\n  {name}=prose_entry\n", encoding="ascii")
        subprocess.run(["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-shared",
                        "-s", "-fno-lto", "-I.", str(RUNTIME), "module.c", "exports.def",
                        "-o", "module.dll"], cwd=folder, check=True)
        os.replace(folder / "module.dll", output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    options = parser.parse_args()
    try:
        compile_dll(options.source.resolve(), options.output.resolve())
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"{exc}\n")
