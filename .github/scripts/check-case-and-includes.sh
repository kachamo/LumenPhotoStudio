#!/usr/bin/env bash
# ==============================================================================
# check-case-and-includes.sh
#
# Filesystem case-correctness check for Lumen Photo Studio.
#
# The project is developed primarily on Windows (a case-INsensitive
# filesystem). A quoted #include, or a <file> path inside a .qrc, that has
# the wrong case (e.g. #include "mainwindow.h" when the real file is
# MainWindow.h) compiles silently on Windows/NTFS but fails to resolve on
# Linux and macOS (both case-sensitive by default). This script catches
# that class of bug in CI.
#
# It works by STRING comparison against the real on-disk name returned by a
# case-insensitive `find`, rather than relying on the host filesystem's own
# case sensitivity. That means it gives correct results whether it happens
# to run on a case-sensitive runner (Linux CI) or a case-insensitive one
# (e.g. if a developer runs it locally on Windows or macOS).
#
# What it checks:
#   1. Every local, double-quoted #include "..." found under src/ and ui/
#      resolves, case-exact, against a real file on disk.
#   2. Every <file>...</file> path inside every .qrc resource file resolves,
#      case-exact, relative to that .qrc file's own directory.
#
# What it deliberately does NOT do (to avoid false positives):
#   - It never looks at <angle-bracket> includes (system/Qt/third-party
#     headers) -- only project-local "quoted" includes are in scope.
#   - If an include can't be resolved anywhere in the tree at all, that is
#     reported as a WARNING, not a failure. A missing file is a "the
#     compiler will catch it" problem, not a cross-platform case-sensitivity
#     problem, and is out of scope for this script.
#   - It only FAILS the build when a path exists on disk under a DIFFERENT
#     case than what the source references -- that is the exact bug
#     signature that works on Windows and silently breaks Linux/macOS.
#
# Known stale/ignored paths (never scanned, see repo root notes):
#   - ./LumenPhotoStudio/  (stale nested duplicate clone, untracked)
#   - ./build/             (stale local build directory, gitignored)
#
# Usage (from repo root, or anywhere -- it cds to the repo root itself):
#   bash .github/scripts/check-case-and-includes.sh
# ==============================================================================
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT" || exit 1

FAIL=0
WARN=0
CHECKED_INCLUDES=0
CHECKED_QRC=0

# ------------------------------------------------------------------------
# resolve_case_exact BASE_DIR REL_PATH
#
# Walks REL_PATH component by component starting at BASE_DIR, using a
# case-INSENSITIVE directory listing at each step but comparing the found
# entry's exact on-disk name against the expected component with a
# case-SENSITIVE string comparison.
#
# Return codes (checked via $?, output captured via stdout):
#   0 -> resolves, and every path component matches on-disk case exactly.
#   1 -> does not resolve at all under BASE_DIR (no case-insensitive match
#        for some component) -- not a case bug, just "not found here".
#   2 -> resolves case-insensitively, but at least one path component has
#        the wrong case -- the real bug this script exists to catch.
# ------------------------------------------------------------------------
resolve_case_exact() {
    local base="$1" relpath="$2"
    local cur="$base"
    local part match

    [ -d "$cur" ] || return 1

    IFS='/' read -r -a parts <<< "$relpath"
    for part in "${parts[@]}"; do
        [ -z "$part" ] && continue
        match=$(find "$cur" -maxdepth 1 -mindepth 1 -iname "$part" -printf '%f\n' 2>/dev/null | head -n1)
        if [ -z "$match" ]; then
            return 1
        fi
        if [ "$match" != "$part" ]; then
            echo "      on-disk entry is '$match', source references '$part'"
            return 2
        fi
        cur="$cur/$match"
    done
    return 0
}

echo "== Checking case-correctness of quoted #include paths under src/ and ui/ =="

while IFS=: read -r src_file _line content; do
    [ -z "${src_file:-}" ] && continue
    inc_path=$(printf '%s' "$content" | sed -E 's/^[[:space:]]*#[[:space:]]*include[[:space:]]*"([^"]*)".*/\1/')
    [ -z "$inc_path" ] && continue
    CHECKED_INCLUDES=$((CHECKED_INCLUDES + 1))
    src_dir=$(dirname "$src_file")

    resolved=1
    mismatch_reason=""
    # Search order approximates real compiler quoted-include resolution:
    # 1) same directory as the including file, 2) the engine's public
    # include root (src/), 3) the ui/ tree, 4) the repo root.
    for base in "$src_dir" "src" "ui" "."; do
        out=$(resolve_case_exact "$base" "$inc_path")
        rc=$?
        if [ "$rc" -eq 0 ]; then
            resolved=0
            break
        elif [ "$rc" -eq 2 ] && [ -z "$mismatch_reason" ]; then
            mismatch_reason="$out"
        fi
    done

    if [ "$resolved" -eq 0 ]; then
        continue
    fi

    if [ -n "$mismatch_reason" ]; then
        echo "  [CASE MISMATCH] \"$inc_path\" (referenced from $src_file)"
        echo "$mismatch_reason"
        FAIL=$((FAIL + 1))
    else
        echo "  [WARN] \"$inc_path\" (referenced from $src_file) not found under src/, ui/, or repo root -- not a case issue, skipping."
        WARN=$((WARN + 1))
    fi
done < <(grep -rn '^[[:space:]]*#[[:space:]]*include[[:space:]]*"[^"]\+"' src ui 2>/dev/null)

echo
echo "== Checking case-correctness of .qrc <file> resource paths =="

while IFS= read -r qrc_file; do
    [ -z "$qrc_file" ] && continue
    qrc_dir=$(dirname "$qrc_file")

    while IFS= read -r file_path; do
        [ -z "$file_path" ] && continue
        CHECKED_QRC=$((CHECKED_QRC + 1))

        out=$(resolve_case_exact "$qrc_dir" "$file_path")
        rc=$?
        if [ "$rc" -eq 0 ]; then
            continue
        elif [ "$rc" -eq 2 ]; then
            echo "  [CASE MISMATCH] \"$file_path\" (referenced from $qrc_file)"
            echo "$out"
            FAIL=$((FAIL + 1))
        else
            echo "  [WARN] \"$file_path\" (referenced from $qrc_file) not found relative to $qrc_dir"
            WARN=$((WARN + 1))
        fi
    done < <(grep -o '<file[^>]*>[^<]*</file>' "$qrc_file" | sed -E 's/<file[^>]*>([^<]*)<\/file>/\1/')
done < <(find . \
    -path './LumenPhotoStudio' -prune -o \
    -path './build' -prune -o \
    -path './.git' -prune -o \
    -name '*.qrc' -print 2>/dev/null)

echo
echo "== (informational, non-blocking) scanning for windows.h / Q_OS_WIN usage =="
HITS=$(grep -rn 'windows\.h\|Q_OS_WIN' src ui 2>/dev/null || true)
if [ -n "$HITS" ]; then
    echo "  Found the following occurrences -- review that they are properly platform-guarded"
    echo "  (this is informational only and does not fail the job):"
    echo "$HITS" | sed 's/^/    /'
else
    echo "  none found."
fi

echo
echo "=============================================================="
echo "Summary: $CHECKED_INCLUDES include reference(s) checked, $CHECKED_QRC qrc resource path(s) checked."
echo "  Case mismatches (fail):                  $FAIL"
echo "  Unresolved / non-project refs (warn only): $WARN"
echo "=============================================================="

if [ "$FAIL" -gt 0 ]; then
    echo "FAILED: one or more filesystem case-sensitivity bugs found."
    echo "These work by accident on Windows/NTFS and will break the build on Linux/macOS."
    exit 1
fi

echo "PASSED: filesystem case-correctness check is clean."
exit 0
