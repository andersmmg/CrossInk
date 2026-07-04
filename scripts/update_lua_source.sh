#!/usr/bin/env bash
# Fetches PUC-Rio Lua 5.4.7 from lua.org into lib/Lua/src/.
#
# Idempotent: when lib/Lua/.VERSION records 5.4.7 AND src/ has at least one
# *.c file, the download is skipped. Invoked automatically by
# scripts/fetch_lua.py before each `pio run`; also runnable manually:
# ./scripts/update_lua_source.sh
#
# Bump LUA_VERSION, LUA_TARBALL_URL, and LUA_SHA256 together with the
# matching constants in scripts/fetch_lua.py.

set -euo pipefail

LUA_VERSION=5.4.7
LUA_TARBALL_URL="https://www.lua.org/ftp/lua-${LUA_VERSION}.tar.gz"
# lua.org does not publish a sidecar .sha256 file, so this is the canonical reference.
LUA_SHA256="9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30"

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd -P)
LIB_LUA="$REPO_ROOT/lib/Lua"
LIB_LUA_SRC="$LIB_LUA/src"
LIB_LUA_VERSION_MARKER="$LIB_LUA/.VERSION"

# Both guards must hold for a fetch to skip; partial state still re-fetches.
if [ -f "$LIB_LUA_VERSION_MARKER" ] && [ "$(cat "$LIB_LUA_VERSION_MARKER")" = "$LUA_VERSION" ]; then
  if compgen -G "$LIB_LUA_SRC/*.c" > /dev/null; then
    echo "[update_lua_source] lib/Lua/.VERSION=$LUA_VERSION with src/*.c present — skipping fetch"
    exit 0
  fi
fi

command -v curl          >/dev/null 2>&1 || { echo "curl not found on PATH"          >&2; exit 1; }
command -v tar           >/dev/null 2>&1 || { echo "tar not found on PATH"           >&2; exit 1; }
if ! command -v sha256sum >/dev/null 2>&1 && ! command -v shasum >/dev/null 2>&1; then
  echo "neither sha256sum nor shasum found on PATH" >&2; exit 1
fi

TMPDIR=$(mktemp -d -t crossink-lua.XXXXXX)
trap 'rm -rf "$TMPDIR"' EXIT

# Stream into a tmpdir so the SHA is verified before extracting.
echo "[update_lua_source] downloading $LUA_TARBALL_URL"
curl -fsSL "$LUA_TARBALL_URL" -o "$TMPDIR/lua.tar.gz"

if command -v sha256sum >/dev/null 2>&1; then
  ACTUAL=$(sha256sum "$TMPDIR/lua.tar.gz" | awk '{print $1}')
else
  ACTUAL=$(shasum -a 256 "$TMPDIR/lua.tar.gz" | awk '{print $1}')
fi
if [ "$ACTUAL" != "$LUA_SHA256" ]; then
  echo "SHA256 mismatch: expected $LUA_SHA256, got $ACTUAL" >&2
  exit 1
fi
echo "[update_lua_source] sha256 verified"

# --strip-components=2 drops `lua-<version>/` and `src/`, so each member
# lands directly under lib/Lua/src/.
mkdir -p "$LIB_LUA_SRC"
rm -rf -- "$LIB_LUA_SRC"/*

tar -xzf "$TMPDIR/lua.tar.gz" --strip-components=2 -C "$LIB_LUA_SRC" "lua-${LUA_VERSION}/src"

# Wrap LUA_32BITS in `#if !defined(...)` so -DLUA_32BITS=1 (from
# platformio.ini [base] build_flags) is respected. PUC-Rio 5.4.7's
# stock luaconf.h sets `LUA_32BITS` unguarded, silently overriding
# any external -D flag. Idempotent: skipped if upstream already has
# the guard, and the WARN branch surfaces any future layout divergence.
python3 - "$LIB_LUA_SRC/luaconf.h" <<'LUAPATCH'
import sys, pathlib
p = pathlib.Path(sys.argv[1])
text = p.read_text()
old = "#define LUA_32BITS\t0\n"
new = "#if !defined(LUA_32BITS)\n#define LUA_32BITS\t0\n#endif\n"
if old in text and "#if !defined(LUA_32BITS)" not in text:
    p.write_text(text.replace(old, new, 1))
    print("[update_lua_source] patched luaconf.h: LUA_32BITS now wrapped in #if !defined")
elif "#if !defined(LUA_32BITS)" in text:
    print("[update_lua_source] luaconf.h already has #if !defined guard — no patch needed")
else:
    print(f"[update_lua_source] WARN: unexpected luaconf.h layout in {p}; please review", file=sys.stderr)
    sys.exit(1)
LUAPATCH

echo "$LUA_VERSION" > "$LIB_LUA_VERSION_MARKER"

FILE_COUNT=$(find "$LIB_LUA_SRC" -type f | wc -l | tr -d ' ')
echo "[update_lua_source] done: $FILE_COUNT files extracted into lib/Lua/src/"
