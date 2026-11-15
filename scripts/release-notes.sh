#!/usr/bin/env bash
# Print one version's notes from CHANGELOG.md (its section, without the heading), for the release page.
#
#   bash scripts/release-notes.sh 0.2.0
#
# The release workflow runs it after bump-version.sh.
set -euo pipefail

version="${1:?usage: release-notes.sh <version>}"
cd "$(dirname "$0")/.."

awk -v v="$version" '
    index($0, "## [" v "]") == 1 {on = 1; next}
    /^## \[/ || /^\[[^]]+\]: / {on = 0}
    on
' CHANGELOG.md
