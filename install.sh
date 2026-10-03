#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
mode=user
if [[ ${1:-} == "--system" ]]; then
    mode=system
elif [[ ${1:-} == "--user" || -z ${1:-} ]]; then
    mode=user
else
    printf 'Usage: %s [--user|--system]\n' "$0" >&2
    exit 2
fi

missing=()
for command_name in bash cmake c++ python3 konsole; do
    command -v "$command_name" >/dev/null 2>&1 || missing+=("$command_name")
done

if ((${#missing[@]})); then
    printf 'Missing required command(s): %s\n' "${missing[*]}" >&2
    printf 'On Arch/EndeavourOS, install the build/runtime dependencies with:\n' >&2
    printf '  sudo pacman -S --needed base-devel cmake extra-cmake-modules qt6-base kcoreaddons ki18n kxmlgui konsole\n' >&2
    exit 1
fi

version=$(konsole --version 2>/dev/null | grep -Eo '[0-9]+\.[0-9]+\.[0-9]+' | head -n1 || true)
if [[ -z "$version" ]]; then
    printf 'Could not determine the installed Konsole version.\n' >&2
    exit 1
fi

if [[ $mode == user ]]; then
    prefix=${DYNQUEUE_PREFIX:-"$HOME/.local"}
    plugin_dir="$prefix/lib/qt6/plugins"
    cmake_prefix_args=(-DCMAKE_INSTALL_PREFIX="$prefix")
    cmake_plugin_args=(-DDYNQUEUE_PLUGIN_INSTALL_DIR=lib/qt6/plugins/konsoleplugins)
    bash_source="$prefix/share/dynqueue/dynqueue.bash"
    parser_path="$prefix/libexec/dynqueue-parser.py"
else
    prefix=${DYNQUEUE_PREFIX:-/usr}
    plugin_dir="$prefix/lib/qt6/plugins"
    cmake_prefix_args=(-DCMAKE_INSTALL_PREFIX="$prefix")
    cmake_plugin_args=(-DDYNQUEUE_PLUGIN_INSTALL_DIR=lib/qt6/plugins/konsoleplugins)
    bash_source="$prefix/share/dynqueue/dynqueue.bash"
    parser_path="$prefix/libexec/dynqueue-parser.py"
fi

cmake_library_args=()
if [[ -n ${KONSOLE_APP_LIBRARY:-} ]]; then
    cmake_library_args+=("-DKONSOLE_APP_LIBRARY=$KONSOLE_APP_LIBRARY")
fi
if [[ -n ${KONSOLE_PRIVATE_LIBRARY:-} ]]; then
    cmake_library_args+=("-DKONSOLE_PRIVATE_LIBRARY=$KONSOLE_PRIVATE_LIBRARY")
fi

build_dir="$project_dir/.build"
cmake -S "$project_dir" -B "$build_dir" \
    "${cmake_prefix_args[@]}" \
    "${cmake_plugin_args[@]}" \
    "${cmake_library_args[@]}" \
    -DCMAKE_BUILD_TYPE=Release \
    "-DDYNQUEUE_KONSOLE_VERSION=$version"
cmake --build "$build_dir" --parallel

if [[ $mode == system && ${EUID:-$(id -u)} -ne 0 ]]; then
    sudo cmake --install "$build_dir"
else
    cmake --install "$build_dir"
fi

marker_start='# >>> DynQueue shell integration >>>'
marker_end='# <<< DynQueue shell integration <<<'
shell_rc="$HOME/.bashrc"
shell_rc_temporary=$(mktemp)
if [[ -f "$shell_rc" ]]; then
    if grep -Fq "$marker_start" "$shell_rc" && ! grep -Fq "$marker_end" "$shell_rc"; then
        rm -f -- "$shell_rc_temporary"
        printf 'Refusing to update %s: DynQueue start marker has no matching end marker.\n' "$shell_rc" >&2
        exit 1
    fi
    awk -v start="$marker_start" -v end="$marker_end" '
        $0 == start { skipping=1; next }
        $0 == end { skipping=0; next }
        !skipping { print }
    ' "$shell_rc" >"$shell_rc_temporary"
    chmod --reference="$shell_rc" "$shell_rc_temporary" 2>/dev/null || true
else
    : >"$shell_rc_temporary"
fi
{
    printf '\n%s\n' "$marker_start"
    printf 'if [[ -f %q ]]; then\n' "$bash_source"
    printf '    if __dynqueue_trap_probe=$(mktemp "${XDG_RUNTIME_DIR:-/tmp}/dynqueue-trap-probe.XXXXXX"); then\n'
    printf '        trap -p DEBUG >"$__dynqueue_trap_probe" 2>/dev/null || true\n'
    printf '        trap -p INT >>"$__dynqueue_trap_probe" 2>/dev/null || true\n'
    printf '        if [[ ! -s "$__dynqueue_trap_probe" ]]; then\n'
    printf '            DYNQUEUE_BASH_PRECHECKED=1 source %q\n' "$bash_source"
    printf '        else\n'
    printf '            printf "[DynQueue] disabled: an existing DEBUG or SIGINT trap was detected; normal shell behavior is unchanged.\\n" >&2\n'
    printf '        fi\n'
    printf '        rm -f -- "$__dynqueue_trap_probe"\n'
    printf '        unset __dynqueue_trap_probe\n'
    printf '    else\n'
    printf '        printf "[DynQueue] disabled: could not create a private shell-trap check file.\\n" >&2\n'
    printf '    fi\n'
    printf 'fi\n'
    printf '%s\n' "$marker_end"
} >>"$shell_rc_temporary"
mv -- "$shell_rc_temporary" "$shell_rc"
printf 'Installed the DynQueue Bash integration block in %s\n' "$shell_rc"

if [[ $mode == user ]]; then
    env_dir="$HOME/.config/plasma-workspace/env"
    env_file="$env_dir/dynqueue.sh"
    mkdir -p "$env_dir"
    cat >"$env_file" <<EOF
#!/usr/bin/env bash
export QT_PLUGIN_PATH="$plugin_dir\${QT_PLUGIN_PATH:+:\$QT_PLUGIN_PATH}"
EOF
    chmod 755 "$env_file"
fi

printf '\nDynQueue was installed.\n'
printf 'Restart Konsole after the plugin path is available.\n'
if [[ $mode == user ]]; then
    printf 'For the current shell, you can test it with:\n'
    printf '  QT_PLUGIN_PATH=%q' "$plugin_dir"
    printf '${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH} konsole\n'
    printf 'A new Plasma session will load %s automatically.\n' "$env_file"
fi
printf 'Installed Bash source: %s\n' "$bash_source"
printf 'Installed parser: %s\n' "$parser_path"
