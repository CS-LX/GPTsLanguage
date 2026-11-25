"""Exercise the shipped PE entry points in a source-free Windows directory."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
from acceptance import pe_layout

BIN = Path(sys.argv[1]).resolve()
CC = sys.argv[2]
IMPORT = Path(sys.argv[3]).resolve()
ROOT = Path(__file__).resolve().parents[1]
C = BIN / "gptc.exe"
HOST = BIN / "lua-host.exe"
WIN = "\n1,1\n2,1\n1,2\n2,2\n1,3\n"
DRAW = "\n1,1\n1,2\n1,3\n2,2\n2,1\n2,3\n3,2\n3,1\n3,3\n"


def run(*args, cwd, stdin="", ok=True):
    env = {k: v for k, v in os.environ.items() if not k.startswith(("LUA_PATH", "LUA_CPATH", "LUA_INIT"))}
    p = subprocess.run(args, input=stdin.encode(), capture_output=True, timeout=45, cwd=cwd, env=env)
    if (p.returncode == 0) != ok:
        raise AssertionError((args, p.returncode, p.stdout[-500:], p.stderr[-1000:]))
    return (p.stdout if ok else p.stderr).decode("utf8").replace("\r\n", "\n")


def game(play):
    files = {}
    for ext in ("dll", "exe"):
        file = play / ("TicTacToing." + ext)
        shutil.copy2(BIN / file.name, file)
        data = file.read_bytes()
        _, parts = pe_layout(data)
        a, n = parts[".gptil"]
        prose = data[a:a+n]
        assert parts[".text"][1] == 24 and n / len(data) > .94
        assert b"\x1bLua" not in data and b"local function game()" not in data
        assert " · ".encode() in prose and " — ".encode() in prose
        listing = run(C, "inspect", file, cwd=play)
        assert "luaopen_TicTacToing" in listing if ext == "dll" else "gpt_main" in listing
        assert prose.decode() in listing and "require/utils.lua" in listing
        files[ext] = (file, data, prose)
    exe, data, prose = files["exe"]
    assert prose == files["dll"][2]
    inputs = WIN.replace("2,1\n", "abc\n4,1\n1,1\n2,1\n", 1) + DRAW
    out = run(exe, "-p1=Ada", cwd=play, stdin=inputs)
    for phrase in ("invalid input", "invalid indexing", "position is taken", "Ada WON!", "it's a tie!"):
        assert phrase in out
    assert out.count("play again?") == 2
    # The real Lua module goes through Windows' normal native DLL searcher.
    lua = 'package.cpath="./?.dll"; arg={"-p1=Ada"}; require("TicTacToing")'
    from_dll = run(HOST, "-e", lua, cwd=play, stdin=inputs)
    assert from_dll == out
    source = ROOT / "examples" / "tic-tac-toing"
    stock = run(HOST, "tic-tac-toing.lua", "-p1=Ada", cwd=source, stdin=inputs)
    assert stock == out
    # Stop counting moves by changing only the immediate operand of the
    # increment. The five moves are accepted, but never reach the win check.
    counter = "increase the register value by a signed immediate. For transparency, the operand ledger reads A 3 · B 3 · C 128".encode()
    assert prose.count(counter) == 1
    changed = play / "alternate.exe"
    changed.write_bytes(data.replace(counter, counter[:-3] + b"127"))
    out = run(changed, cwd=play, stdin=WIN)
    assert "WON!" not in out and "player2 move: end" in out
    assert "player1 WON!" in run(exe, cwd=play, stdin=WIN)
    instructions = prose.count(b"\n\n")
    print(f"Game verified: {len(prose):,} prose bytes / {len(data):,} EXE bytes; {instructions} instructions.")


def libraries(base, play):
    src = base / "source"
    mods = base / "installed" / "share" / "lua" / "5.4"
    (mods / "helpers").mkdir(parents=True)
    src.mkdir()
    (mods / "helpers" / "init.lua").write_text("return {name=..., source=debug.getinfo(1).source}")
    (src / "env.lua").write_text("return value")
    main = src / "calc.lua"
    main.write_text("""#!/usr/bin/env lua
local m = require("helpers")
assert(m.name == "helpers" and m.source == "@helpers/init.lua")
assert(assert(loadfile("env.lua","t",{value=42}))() == 42)
assert(assert(loadfile("env.lua","t",nil)))
assert(not loadfile("env.lua","b"))
local make = coroutine.wrap(function(x)
  local done <close> = setmetatable({}, {__close=function() _G.closed=true end})
  coroutine.yield(x)
  return x+1
end)
assert(make(40)==40 and make()==41 and closed)
assert(require("native").answer()==42)
return function(a,b) return a+b end
""", encoding="utf-8-sig")
    ext = src / "native.c"
    ext.write_text("""#include "lauxlib.h"
static int answer(lua_State *L) { lua_pushinteger(L,42); return 1; }
__declspec(dllexport) int luaopen_native(lua_State *L) {
    lua_newtable(L); lua_pushcfunction(L,answer); lua_setfield(L,-2,"answer"); return 1;
}
""")
    run(CC, "-shared", "-DLUA_BUILD_AS_DLL", "-I" + str(ROOT / "third_party/lua54"),
        ext, IMPORT, "-o", play / "native.dll", cwd=play)
    dll = play / "calc.dll"
    args = [C, "compile", main, "--root", src, "--modules", mods, "-o", dll]
    run(*args, cwd=play)
    original = dll.read_bytes()
    run(*args, cwd=play)
    assert dll.read_bytes() == original
    (src / "bad.lua").write_text("local =")
    assert "bad.lua" in run(*args, cwd=play, ok=False) and dll.read_bytes() == original
    (src / "bad.lua").unlink()
    (mods / "env.lua").write_text("return nil")
    assert "duplicate bundled" in run(*args, cwd=play, ok=False) and dll.read_bytes() == original
    call = 'package.path="external-only/?.lua"; print(require("calc")(9,4))'
    assert run(HOST, "-e", call, cwd=play) == "13\n"
    verb = b"increase the left operand by the right operand"
    assert original.count(verb) == 1
    dll.write_bytes(original.replace(verb, verb.replace(b"increase", b"decrease")))
    assert run(HOST, "-e", call, cwd=play) == "5\n"
    dll.write_bytes(original.replace(verb, verb.replace(b"increase", b"xxxxxxxx")))
    assert "unrecognized GPT action" in run(HOST, "-e", call, cwd=play, ok=False)
    probe = src / "argv.lua"
    probe.write_text("local a,b=...; assert(arg[1]==a and arg[2]==b); print(a,b); os.exit(0,true)")
    run(C, "compile", probe, "-o", play/"argv.exe", cwd=play)
    assert run(play/"argv.exe", "hello world", "围棋", cwd=play) == "hello world\t围棋\n"
    assert not list(play.rglob("*.lua"))


with tempfile.TemporaryDirectory() as tmp:
    base = Path(tmp)
    play = base / "bin"
    play.mkdir()
    for runtime in ("gptlua54.dll", "lua54.dll"):
        shutil.copy2(BIN / runtime, play)
    game(play)
    libraries(base, play)
