#!/usr/bin/env bash
# tests/test_compile_status.sh BB_PROBE: `bb-probe --compile-status <data dir>` (src/compile_status.c),
# whether Bloodborne's shader compile step is due. Temp folders only, no game data.
#   - a missing cache: due, never_built;
#   - finished files with this version and this system, no notes: not due, up_to_date;
#   - another system: due, other_system; another version: due, other_version;
#   - the tool creates and changes nothing (a listing with sizes and times before and after);
#   - BB_USER_DIR moves the cache; a missing argument is an error.
set -euo pipefail
probe=${1:?usage: test_compile_status.sh BB_PROBE}
probe=$(cd -- "$(dirname -- "$probe")" && pwd)/$(basename -- "$probe")
work=$(mktemp -d "${TMPDIR:-/tmp}/compile-status.XXXXXX")
trap 'rm -rf "${work:?}"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
unset BB_USER_DIR BB_GPU_USER_DIR

listing() { (cd "$1" && find . -exec stat -f '%N %z %m' {} + 2>/dev/null | sort); }
# status DIR: the JSON line (the tool must not change DIR).
status() {
    local before after out
    before=$(listing "$1")
    out=$("$probe" --compile-status "$1") || fail "--compile-status exited non-zero for $1"
    after=$(listing "$1")
    [[ $before == "$after" ]] || fail "the tool changed $1"
    [[ $(printf '%s\n' "$out" | wc -l) -eq 1 ]] || fail "not one line: $out"
    printf '%s\n' "$out"
}
# field JSON NAME: one top-level value.
field() { python3 -I -c 'import json,sys; v=json.loads(sys.argv[1])[sys.argv[2]]; print(json.dumps(v) if isinstance(v,bool) else v)' "$1" "$2"; }
expect() { # JSON NAME VALUE
    [[ $(field "$1" "$2") == "$3" ]] || fail "$2: wanted $3, got $(field "$1" "$2") in $1"
}

# A data folder that does not exist: due, never built; nothing is created.
empty=$work/empty
mkdir "$empty"
out=$(status "$empty")
expect "$out" applicable true
expect "$out" due true
expect "$out" reason never_built
expect "$out" built_states 0
expect "$out" seen_states 0
expect "$out" new_states_for_a_run 4
version=$(field "$out" version)
system=$(field "$out" system)
[[ $version =~ ^[0-9]+$ && $version -gt 0 ]] || fail "no version in $out"
[[ -z $(ls -A "$empty") ]] || fail "the tool created something in $empty"
missing=$work/does-not-exist
out=$("$probe" --compile-status "$missing") || fail "a missing data folder failed"
[[ ! -e $missing ]] || fail "the tool created $missing"
expect "$out" reason never_built

# Finished with this version and system, no notes: up to date.
good=$work/good
mkdir -p "$good/user/cache"
echo "$version" > "$good/user/cache/shaders-ahead.txt.complete"
echo "5 $system" > "$good/user/cache/pipelines-ahead.complete"
out=$(status "$good")
expect "$out" due false
expect "$out" reason up_to_date
expect "$out" built_states 5
expect "$out" built_for "$system"
expect "$out" built_version "$version"

# Another system.
other=$work/other-system
mkdir -p "$other/user/cache"
echo "$version" > "$other/user/cache/shaders-ahead.txt.complete"
echo '5 24A000/Other"Mac\1' > "$other/user/cache/pipelines-ahead.complete"
out=$(status "$other")
expect "$out" due true
expect "$out" reason other_system
expect "$out" built_for '24A000/Other"Mac\1' # the JSON is escaped and parses back

# Another version.
old=$work/other-version
mkdir -p "$old/user/cache"
echo "$((version + 1))" > "$old/user/cache/shaders-ahead.txt.complete"
echo "5 $system" > "$old/user/cache/pipelines-ahead.complete"
out=$(status "$old")
expect "$out" due true
expect "$out" reason other_version
expect "$out" version "$version"
expect "$out" built_version "$((version + 1))"

# A 0.9.5 record (states, no system) is this system: not due; and the tool does not rewrite it.
legacy=$work/legacy
mkdir -p "$legacy/user/cache"
echo "$version" > "$legacy/user/cache/shaders-ahead.txt.complete"
echo "5" > "$legacy/user/cache/pipelines-ahead.complete"
out=$(status "$legacy")
expect "$out" due false
expect "$out" reason up_to_date
[[ $(cat "$legacy/user/cache/pipelines-ahead.complete") == 5 ]] || fail "the legacy record was rewritten"

# BB_USER_DIR: the cache is under it, not under the data folder.
redirect=$work/redirect
mkdir -p "$redirect/cache"
echo "$version" > "$redirect/cache/shaders-ahead.txt.complete"
echo "5 $system" > "$redirect/cache/pipelines-ahead.complete"
out=$(BB_USER_DIR=$redirect "$probe" --compile-status "$work/elsewhere")
expect "$out" reason up_to_date

# A missing argument is an error, with nothing on stdout.
[[ -z $("$probe" --compile-status 2>/dev/null || true) ]] || fail "output without an argument"
"$probe" --compile-status >/dev/null 2>&1 && fail "a missing argument was accepted"
echo "compile-status: ok (version $version, system $system)"
