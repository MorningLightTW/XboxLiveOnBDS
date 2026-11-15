#!/usr/bin/env bash
# Set the mod version everywhere it is written down, and turn CHANGELOG.md's [Unreleased] notes into that version.
#
#   bash scripts/bump-version.sh 0.2.0 [2026-12-01]
#
# Changes xmake.lua (mod_version), lip.toml (version) and CHANGELOG.md. Fails, changing nothing, when the
# version is not x.y.z or [Unreleased] has no notes. The current version (with its CHANGELOG.md section) is
# left as it is. The release workflow runs it.
set -euo pipefail

version="${1:-}"
date="${2:-$(date -u +%F)}"
cd "$(dirname "$0")/.."

fail() { echo "bump-version: $*" >&2; exit 1; }

[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail "version must look like 1.2.3, got '$version'"
prev=$(sed -nE 's/^local mod_version = "([0-9.]+)".*/\1/p' xmake.lua)
[[ -n "$prev" ]] || fail "no mod_version in xmake.lua"
if [[ "$version" == "$prev" ]]; then
    # Releasing the version already written down (e.g. redoing a release): nothing to change.
    grep -qF "## [$version]" CHANGELOG.md || fail "already at $version, but CHANGELOG.md has no [$version] section"
    echo "already at $version"
    exit 0
fi

notes=$(awk '/^## \[Unreleased\]/ {on = 1; next} /^## \[/ {on = 0} on' CHANGELOG.md | tr -d '\r' | grep -v '^[[:space:]]*$' || true)
[[ -n "$notes" ]] || fail "CHANGELOG.md has nothing under [Unreleased]"
repo=$(sed -nE 's#^\[Unreleased\]: (https://github\.com/[^/]+/[^/]+)/compare/.*#\1#p' CHANGELOG.md)
[[ -n "$repo" ]] || fail "no [Unreleased] compare link in CHANGELOG.md"

sed -i -E "s/^local mod_version = \"[0-9.]+\"/local mod_version = \"$version\"/" xmake.lua
sed -i -E "0,/^version = \"[0-9.]+\"/s//version = \"$version\"/" lip.toml
sed -i -E \
    -e "s/^## \[Unreleased\]/## [Unreleased]\n\n## [$version] - $date/" \
    -e "s#^\[Unreleased\]: .*#[Unreleased]: $repo/compare/v$version...HEAD\n[$version]: $repo/compare/v$prev...v$version#" \
    CHANGELOG.md

echo "$prev -> $version"
