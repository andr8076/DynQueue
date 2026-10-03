#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
runtime_dir=$(mktemp -d)
trap 'rm -rf -- "$runtime_dir"' EXIT

set +e
output=$(env \
    XDG_RUNTIME_DIR="$runtime_dir" \
    DYNQUEUE_PARSER="$project_root/src/dynqueue-parser.py" \
    bash --noprofile --norc -i <<EOF
source "$project_root/src/shell/dynqueue.bash"
printf 'normal\n'
echo one && echo two
echo "one && two"
true && echo SHOULD_RUN
cd /tmp && pwd
export TEST=hello && printf 'test=%s\n' "\$TEST"
printf 'after=%s\n' "\$TEST"
false && echo SHOULD_NOT_RUN
EOF
)
status=$?
set -e

if ((status != 0 && status != 130)); then
    printf '%s\n' "$output" >&2
    printf 'interactive Bash integration exited unexpectedly with status %s\n' "$status" >&2
    exit 1
fi

for expected in normal one two 'one && two' SHOULD_RUN /tmp 'test=hello' 'after=hello'; do
    if [[ "$output" != *"$expected"* ]]; then
        printf '%s\n' "$output" >&2
        printf 'missing expected output: %s\n' "$expected" >&2
        exit 1
    fi
done

if [[ "$output" == *SHOULD_NOT_RUN* ]]; then
    printf '%s\n' "$output" >&2
    printf 'a failed queue item incorrectly allowed the next item to run\n' >&2
    exit 1
fi

snapshot=$(find "$runtime_dir" -name snapshot -type f -print -quit)
[[ -n "$snapshot" ]]
grep -F $'item-0\tfailed\t' "$snapshot" >/dev/null
grep -F $'item-1\tstopped\t' "$snapshot" >/dev/null

printf 'Bash integration test passed\n'
