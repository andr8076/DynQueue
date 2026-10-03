#!/usr/bin/env bash
set -euo pipefail

prefix=${DYNQUEUE_PREFIX:-"$HOME/.local"}
marker_start='# >>> DynQueue shell integration >>>'
marker_end='# <<< DynQueue shell integration <<<'

if [[ -f "$HOME/.bashrc" ]]; then
    temporary=$(mktemp)
    awk -v start="$marker_start" -v end="$marker_end" '
        $0 == start { skipping=1; next }
        $0 == end { skipping=0; next }
        !skipping { print }
    ' "$HOME/.bashrc" >"$temporary"
    mv -- "$temporary" "$HOME/.bashrc"
fi

rm -f -- "$prefix/share/dynqueue/dynqueue.bash" \
       "$prefix/libexec/dynqueue-parser.py" \
       "$HOME/.config/plasma-workspace/env/dynqueue.sh"
rm -f -- "$prefix/lib/qt6/plugins/konsoleplugins/libkonsole_dynqueueplugin.so"

printf 'Removed the user-local DynQueue files and its .bashrc block.\n'
printf 'Restart Konsole; a new Plasma session may be needed to clear its plugin path.\n'
