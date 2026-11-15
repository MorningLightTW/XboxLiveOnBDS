#!/usr/bin/env bash
# PostToolUse hook: formats a C++ file Claude Code just wrote or edited with the repo's .clang-format.
# Plain bash, so it runs the same on Windows (Git Bash) and Linux/macOS, without jq. Only touches .cpp/.h under
# src/, src-server/ and tests/, and never fails the tool call.
want=23.1.2 # The version CI checks with (.github/workflows/ci.yml); another version may format differently.

payload=$(cat)
# "file_path":"<JSON string>"; a Windows path arrives with each backslash doubled.
path=$(printf '%s' "$payload" | sed -n 's/.*"file_path"[[:space:]]*:[[:space:]]*"\(\([^"\\]\|\\.\)*\)".*/\1/p' | head -n 1)
[ -n "$path" ] || exit 0
path=${path//\\\\/\\}
path=${path//\\//}

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && { pwd -W 2>/dev/null || pwd; })
case "$root" in
[A-Za-z]:*) # Windows: compare paths case-insensitively.
    relative=$(printf '%s' "$path" | tr '[:upper:]' '[:lower:]')
    prefix=$(printf '%s/' "$root" | tr '[:upper:]' '[:lower:]')
    [ "${relative#"$prefix"}" != "$relative" ] || exit 0
    relative=${path:${#prefix}}
    ;;
*)
    [ "${path#"$root"/}" != "$path" ] || exit 0
    relative=${path#"$root"/}
    ;;
esac
case "$relative" in
src/*.cpp | src/*.h | src-server/*.cpp | src-server/*.h | tests/*.cpp | tests/*.h) ;;
*) exit 0 ;;
esac
[ -f "$path" ] || exit 0

# The pinned version if it is installed, else the same version through pipx, else whatever clang-format there is.
fallback=
for candidate in clang-format "/c/Program Files/LLVM/bin/clang-format.exe"; do
    command -v "$candidate" >/dev/null 2>&1 || continue
    if "$candidate" --version 2>/dev/null | grep -q "version $want"; then
        "$candidate" -i --style=file "$path" 2>/dev/null
        exit 0
    fi
    [ -n "$fallback" ] || fallback=$candidate
done
if command -v pipx >/dev/null 2>&1 && pipx run "clang-format==$want" -i --style=file "$path" 2>/dev/null; then
    exit 0
fi
if [ -n "$fallback" ]; then
    "$fallback" -i --style=file "$path" 2>/dev/null
    printf '{"systemMessage":"Formatted %s with a clang-format other than %s; CI may still flag it (install %s or pipx)."}\n' \
        "$relative" "$want" "$want"
fi
exit 0
