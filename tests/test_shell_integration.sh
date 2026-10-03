#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
runtime_dir=$(mktemp -d)
trap 'rm -rf -- "$runtime_dir"' EXIT

set +e
output=$(env \
    XDG_RUNTIME_DIR="$runtime_dir" \
    DYNQUEUE_PARSER="$project_root/src/dynqueue-parser.py" \
    DYNQUEUE_BASH_PRECHECKED=1 \
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

set +e
cancel_output=$(env \
    XDG_RUNTIME_DIR="$runtime_dir" \
    DYNQUEUE_PARSER="$project_root/src/dynqueue-parser.py" \
    DYNQUEUE_BASH_PRECHECKED=1 \
    SHELL_SESSION_ID=cancel-test \
    bash --noprofile --norc -i <<EOF
source "$project_root/src/shell/dynqueue.bash"
printf '%s\n' "\$__dynqueue_queue_id" > "\$__dynqueue_session_dir/cancel" && printf 'CANCEL_SHOULD_NOT_RUN\n'
printf 'after-cancel\n'
EOF
)
cancel_status=$?
set -e
if ((cancel_status != 0 && cancel_status != 130)); then
    printf '%s\n' "$cancel_output" >&2
    printf 'cancel test exited unexpectedly with status %s\n' "$cancel_status" >&2
    exit 1
fi
if [[ "$cancel_output" == *CANCEL_SHOULD_NOT_RUN* ]]; then
    printf '%s\n' "$cancel_output" >&2
    printf 'a cancelled queue incorrectly ran a waiting item\n' >&2
    exit 1
fi
cancel_snapshot="$runtime_dir/dynqueue-${UID}/cancel-test/snapshot"
grep -F $'item-0\tcompleted\t' "$cancel_snapshot" >/dev/null
grep -F $'item-1\tstopped\t' "$cancel_snapshot" >/dev/null

set +e
trap_output=$(env \
    XDG_RUNTIME_DIR="$runtime_dir" \
    DYNQUEUE_PARSER="$project_root/src/dynqueue-parser.py" \
    SHELL_SESSION_ID=custom-trap-test \
    bash --noprofile --norc -i <<EOF
trap 'printf "CUSTOM_INT\n"' INT
source "$project_root/src/shell/dynqueue.bash"
true && printf 'ORIGINAL_CHAIN\n'
EOF
)
trap_status=$?
set -e
if ((trap_status != 0)); then
    printf '%s\n' "$trap_output" >&2
    printf 'custom SIGINT trap test exited unexpectedly with status %s\n' "$trap_status" >&2
    exit 1
fi
if [[ "$trap_output" != *ORIGINAL_CHAIN* ]] || [[ "$trap_output" == *CUSTOM_INT* ]]; then
    printf '%s\n' "$trap_output" >&2
    printf 'an existing SIGINT trap was not left untouched\n' >&2
    exit 1
fi

set +e
option_output=$(env \
    XDG_RUNTIME_DIR="$runtime_dir" \
    DYNQUEUE_PARSER="$project_root/src/dynqueue-parser.py" \
    DYNQUEUE_BASH_PRECHECKED=1 \
    SHELL_SESSION_ID=shell-options-test \
    bash --noprofile --norc -i <<EOF
set -eE -T
trap 'echo ERR_HOOK_SHOULD_NOT_RUN' ERR
source "$project_root/src/shell/dynqueue.bash"
true && echo OPTIONS_CHAIN
echo OPTIONS_AFTER
EOF
)
option_status=$?
set -e
if ((option_status != 0)) || [[ "$option_output" != *OPTIONS_CHAIN* ]] || [[ "$option_output" != *OPTIONS_AFTER* ]] \
    || [[ "$option_output" == *ERR_HOOK_SHOULD_NOT_RUN* ]]; then
    printf '%s\n' "$option_output" >&2
    printf 'shell option compatibility test failed with status %s\n' "$option_status" >&2
    exit 1
fi

printf 'Bash integration test passed\n'
