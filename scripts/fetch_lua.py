"""
PlatformIO pre-build hook: populate lib/Lua/src/ from lua.org before each
`pio run` so contributors don't need the vendor tarball committed. Delegates
to scripts/update_lua_source.sh for fetch + SHA256-verify + extract.
"""
import shutil
import subprocess
import sys
from pathlib import Path

LUA_VERSION = "5.4.7"
# Mirror the constant in scripts/update_lua_source.sh — bump both together.
LUA_TARBALL_SHA256 = "9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30"


def main(env=None) -> int:
    # SCons's exec wrapper doesn't bind `__file__`, so Path(__file__) would
    # crash when this script is run as a pre: build hook. Use PIO's
    # PROJECT_DIR when invoked by SCons, cwd otherwise.
    if env is not None and "PROJECT_DIR" in env:
        repo_root = Path(env["PROJECT_DIR"])
    else:
        repo_root = Path.cwd()

    source_sh = repo_root / "scripts" / "update_lua_source.sh"
    lib_lua = repo_root / "lib" / "Lua"
    lib_lua_src = lib_lua / "src"

    # Mirror the .sh script's cache guard. Both must hold for a fetch to skip.
    marker = lib_lua / ".VERSION"
    if marker.exists() and marker.read_text().strip() == LUA_VERSION:
        if any(lib_lua_src.glob("*.c")):
            print(f"[fetch_lua] lib/Lua/.VERSION={LUA_VERSION} with src/*.c present — skipping fetch")
            return 0

    if not shutil.which("bash"):
        print("[fetch_lua] bash not found on PATH; install bash or run scripts/update_lua_source.sh manually",
              file=sys.stderr)
        return 1
    if not source_sh.exists():
        print(f"[fetch_lua] missing companion script {source_sh}", file=sys.stderr)
        return 1

    # SCons treats a pre: script that exits non-zero as success — a SHA
    # mismatch or curl 404 would otherwise surface only as a downstream
    # "lua.h not found" compile error, not as a build failure.
    result = subprocess.run(["bash", str(source_sh)], capture_output=True, text=True)
    if result.returncode != 0:
        # `print(file=…)` guarantees a trailing newline; `sys.stderr.write`
        # would let the .sh's last line concatenate with the RuntimeError
        # traceback. The conditional `end=` keeps `echo`-terminated output
        # byte-stable.
        if result.stdout:
            print(result.stdout, file=sys.stderr, end="" if result.stdout.endswith("\n") else "\n")
        if result.stderr:
            print(result.stderr, file=sys.stderr, end="" if result.stderr.endswith("\n") else "\n")
        sys.stderr.flush()
        raise RuntimeError(
            f"[fetch_lua] update_lua_source.sh exited {result.returncode} — "
            f"see scripts/update_lua_source.sh"
        )
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
else:
    try:
        Import("env")  # noqa: F821
        main(env)
    except NameError:
        pass
