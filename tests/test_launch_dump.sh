#!/usr/bin/env bash
# tests/test_launch_dump.sh BB_PROBE: the probe's launch dump (src/app_bundle.c, bb_app_launch_dump),
# which Bloodborne.app asks of run.sh in place of starting the game. No game is needed.
#   - BB_LAUNCH_DUMP set and `-` as the first argument: the arguments, the environment and the
#     directory are written to the file, the exit code is 0 and nothing else happens;
#   - without BB_LAUNCH_DUMP, or with another first argument, no file is written (--print-scaled,
#     which run.sh asks the same program, answers as always).
set -euo pipefail
probe=${1:?usage: test_launch_dump.sh BB_PROBE}
probe=$(cd -- "$(dirname -- "$probe")" && pwd)/$(basename -- "$probe")
work=$(mktemp -d "${TMPDIR:-/tmp}/launch-dump.XXXXXX")
trap 'rm -rf "${work:?}"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

dump=$work/dump
(cd "$work" && BB_LAUNCH_DUMP=$dump BB_DUMP_CHECK='a=b' "$probe" - --flag 'two words' '') || fail "the dump run did not exit 0"
[[ -s $dump ]] || fail "no dump was written"
python3 -I - "$dump" "$probe" "$work" <<'PY'
import os, sys
data = open(sys.argv[1], "rb").read()
records = data.split(b"\0")
assert records.pop() == b"", "the last record must end in NUL"
assert records[0] == b"BBLAUNCH1", records[0]
assert records[-1] == b"Z", "no end record: " + repr(records[-1])
args = [r[1:].decode() for r in records if r[:1] == b"A"]
env = [r[1:].decode() for r in records if r[:1] == b"V"]
cwd = [r[1:].decode() for r in records if r[:1] == b"C"]
assert args[1:] == ["-", "--flag", "two words", ""], args
assert "BB_DUMP_CHECK=a=b" in env, "the environment is missing a variable"
assert any(e.startswith("BB_LAUNCH_DUMP=") for e in env)
assert len(cwd) == 1 and os.path.realpath(cwd[0]) == os.path.realpath(sys.argv[3]), cwd
print("dump: %d arguments, %d variables, directory %s" % (len(args), len(env), cwd[0]))
PY

# Not a dump: another first argument, and --print-scaled (it must still answer).
rm -f "$dump"
BB_LAUNCH_DUMP=$dump "$probe" --print-scaled "$work/no-such-settings.ini" > "$work/scaled.out" || fail "--print-scaled failed"
[[ ! -e $dump ]] || fail "--print-scaled wrote a dump"
BB_LAUNCH_DUMP=$dump "$probe" --vulkan-only-not-an-option > /dev/null 2>&1 && fail "an unknown option was accepted"
[[ ! -e $dump ]] || fail "a first argument other than - wrote a dump"
# No BB_LAUNCH_DUMP: nothing is written (`-` without --app0 is refused, as before).
(cd "$work" && "$probe" - > /dev/null 2>&1) && fail "'-' without --app0 was accepted"
[[ ! -e $dump ]] || fail "a dump was written without BB_LAUNCH_DUMP"
echo "launch dump: ok"
