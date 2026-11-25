"""Run the unmodified 5.4.9 Lua test programs in their official basic mode."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile

bin_dir, suite = (Path(s).resolve() for s in sys.argv[1:3])
if not (suite / "all.lua").is_file():
    raise SystemExit("Pass the extracted lua-5.4.9-tests directory containing all.lua")
with tempfile.TemporaryDirectory() as work:
    base = Path(work)
    source, play = base / "source", base / "run"
    shutil.copytree(suite, source)
    play.mkdir()
    entry = source / "gpt-suite-entry.lua"
    entry.write_text('_U = true\ndofile("all.lua")\n', encoding="utf-8")
    for dll in ("lua54.dll", "gptlua54.dll"):
        shutil.copy2(bin_dir / dll, play)
    exe = play / "suite.exe"
    subprocess.run([bin_dir / "gptc.exe", "compile", entry, "--root", source, "-o", exe], check=True)
    # Remove the input tree so the interpreter cannot reach the original sources.
    shutil.rmtree(source)
    env = {k: v for k, v in os.environ.items() if not k.startswith(("LUA_PATH", "LUA_CPATH", "LUA_INIT"))}
    result = subprocess.run([exe], cwd=play, env=env, capture_output=True, timeout=180)
    if result.returncode or b"final OK !!!" not in result.stdout:
        sys.stderr.buffer.write(result.stdout[-2500:] + result.stderr[-2500:])
        raise SystemExit(result.returncode or 1)
    print("Lua 5.4.9 official basic suite: final OK !!!")
