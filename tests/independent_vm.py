"""Replay the real published EXE after deleting Lua, and inspect the running process."""
import ctypes as ct
from ctypes import wintypes as win
import gzip
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BIN = Path(sys.argv[1]).resolve()
K = ct.WinDLL("kernel32", use_last_error=True)
K.OpenProcess.argtypes = [win.DWORD, win.BOOL, win.DWORD]
K.OpenProcess.restype = win.HANDLE
K.CloseHandle.argtypes = [win.HANDLE]
K.K32EnumProcessModules.argtypes = [win.HANDLE, ct.POINTER(win.HMODULE), win.DWORD, ct.POINTER(win.DWORD)]
K.K32GetModuleFileNameExW.argtypes = [win.HANDLE, win.HMODULE, win.LPWSTR, win.DWORD]


def modules(pid):
    handle = K.OpenProcess(0x0410, False, pid)
    assert handle, ct.get_last_error()
    try:
        mods = (win.HMODULE * 512)()
        used = win.DWORD()
        assert K.K32EnumProcessModules(handle, mods, ct.sizeof(mods), ct.byref(used))
        assert used.value <= ct.sizeof(mods)
        paths = set()
        for module in mods[:used.value // ct.sizeof(win.HMODULE)]:
            name = ct.create_unicode_buffer(32768)
            assert K.K32GetModuleFileNameExW(handle, module, name, len(name))
            paths.add(Path(name.value).name.lower())
        return paths
    finally:
        K.CloseHandle(handle)


def play(exe, env):
    inputs = "\n1,1\njunk\n1,1\n2,1\n1,2\n2,2\n1,3\n\n1,1\n1,2\n1,3\n2,2\n2,1\n2,3\n3,2\n3,1\n3,3\n"
    with subprocess.Popen([exe, "-p1=Ada"], cwd=exe.parent, env=env, stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE) as process:
        while True:
            line = process.stdout.readline()
            assert line, process.stderr.read()
            if b"EOF to quit" in line:
                break
        present = modules(process.pid)
        assert "gptlua54.dll" in present
        assert present.isdisjoint({"lua54.dll", "gptcompat.dll"}), present
        output, err = process.communicate(inputs.encode(), timeout=15)
        assert process.returncode == 0, err
        for message in (b"Ada WON!", b"it's a tie!", b"invalid input", b"position is taken"):
            assert message in output, message
        return output


with tempfile.TemporaryDirectory() as tmp:
    work = Path(tmp)
    env = dict(os.environ, PATH=str(Path(os.environ["SystemRoot"])/"System32"))
    shutil.copy2(BIN/"gptlua54.dll", work)
    shutil.copy2(BIN/"lua54.dll", work)
    (work/"lua54.dll").unlink()
    old = gzip.decompress((ROOT/"tests/fixtures/tictactoing-v1.exe.gz").read_bytes())
    assert hashlib.sha256(old).hexdigest() == "fc3abbb3f106e169644ee834608404811eec858cd5f58652705999825e1d92cb"
    released = work/"released.exe"
    released.write_bytes(old)
    baseline = play(released, env)
    exe = work/"TicTacToing.exe"
    shutil.copy2(BIN/exe.name, exe)
    assert play(exe, env) == baseline
    counting="one more for your count of “moves”".encode()
    image=exe.read_bytes()
    assert image.count(counting)==1
    changed=work/"changed.exe"
    changed.write_bytes(image.replace(counting,counting.replace(b"one more",b"one less")))
    changed_output=subprocess.run([changed],input=b"\n1,1\n2,1\n1,2\n2,2\n1,3\n",
                                  cwd=work,env=env,capture_output=True,check=True,timeout=15).stdout
    assert b"WON!" not in changed_output and b"player2 move: end" in changed_output
    dll = work/"TicTacToing.dll"
    shutil.copy2(BIN/dll.name, dll)
    host = (
        "import ctypes as c,sys; m=c.CDLL(sys.argv[1]); f=m.gpt_run;"
        "f.argtypes=[c.POINTER(c.c_char_p)]; "
        "sys.exit(f((c.c_char_p*3)(b'game',b'-p1=Ada',None)))"
    )
    play_dll = subprocess.run([sys.executable,"-c",host,str(dll)], cwd=work,env=env,
                              input=b"\n1,1\n2,1\n1,2\n2,2\n1,3\n",capture_output=True,timeout=15)
    assert play_dll.returncode == 0 and b"Ada WON!" in play_dll.stdout, play_dll.stderr
    # An unrelated program exercises frames, signed arithmetic, open captures,
    # GC cycles, metamethods and tail calls independently of any game functions.
    probe = work/"semantics.lua"
    probe.write_text("""local function tail(n,x)
      if n==0 then return x end
      return tail(n-1,x+1)
    end
    local closed = 0
    do local x <close> = setmetatable({}, {__close=function() closed=closed+1 end}) end
    local function make(x)
      return function(y) x=x+y; return x end
    end
    local a,b=make(10),make(20)
    local total=0
    for i=-2,3 do total=total+i end
    for k,v in ipairs({3,5,7}) do total=total+k*v end
    local m={__add=function(x,y) return x.n+y.n end}
    local sum=setmetatable({n=3},m)+setmetatable({n=4},m)
    local t={}
    for i=1,5000 do t={previous=i}; t.self=t end
    local function many(...) return ... end
    local x,y=many(7,13)
    print(tail(800,2),a(1),b(4),a(2),closed,total,sum,x,y,t.previous,
          -7//3,-7%3,0x7fffffffffffffff+1,(3<<4) ~ 2,0x7fffffffffffffff < (2.0 ^ 63))
    """,encoding="utf-8")
    expected=subprocess.check_output([BIN/"lua-host.exe",probe],env=env,cwd=work)
    subprocess.run([BIN/"gptc.exe","compile",probe,"-o",work/"semantics.exe"],check=True)
    probe.unlink()
    assert subprocess.check_output([work/"semantics.exe"],env=env,cwd=work)==expected
    print("Released EXE, feedback EXE and native DLL ran on the independent C++ VM.")
