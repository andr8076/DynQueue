#!/usr/bin/env bash
set -euo pipefail

source_dir=${DYNQUEUE_SOURCE_DIR:?DYNQUEUE_SOURCE_DIR is required}
build_dir=${DYNQUEUE_BUILD_DIR:?DYNQUEUE_BUILD_DIR is required}
prefix=${DYNQUEUE_PREFIX:?DYNQUEUE_PREFIX is required}
plugin_rel=${DYNQUEUE_PLUGIN_INSTALL_DIR:-lib/qt6/plugins/konsoleplugins}

if ! command -v konsole >/dev/null 2>&1 || ! command -v cmake >/dev/null 2>&1; then
    exit 1
fi

version=$(konsole --version 2>/dev/null | grep -Eo '[0-9]+\.[0-9]+\.[0-9]+' | head -n1 || true)
if [[ -z "$version" ]] && command -v pacman >/dev/null 2>&1; then
    version=$(pacman -Q konsole 2>/dev/null | grep -Eo '[0-9]+\.[0-9]+\.[0-9]+' | head -n1 || true)
fi
[[ -n "$version" ]] || exit 1

state_dir="$prefix/share/dynqueue"
marker="$state_dir/konsole-build-version"
plugin="$prefix/$plugin_rel/konsole_dynqueueplugin.so"
mkdir -p "$state_dir"

exec 9>"$state_dir/.refresh.lock"
flock -n 9 || exit 0

if [[ -f "$marker" && -f "$plugin" ]] && [[ $(<"$marker") == "$version" ]]; then
    exit 0
fi

[[ -d "$source_dir" ]] || exit 1

cmake_args=(
    -DCMAKE_BUILD_TYPE=Release
    "-DCMAKE_INSTALL_PREFIX=$prefix"
    "-DDYNQUEUE_PLUGIN_INSTALL_DIR=$plugin_rel"
    "-DDYNQUEUE_KONSOLE_VERSION=$version"
)
if [[ -n ${DYNQUEUE_KONSOLE_APP_LIBRARY:-} ]]; then
    cmake_args+=("-DKONSOLE_APP_LIBRARY=$DYNQUEUE_KONSOLE_APP_LIBRARY")
fi
if [[ -n ${DYNQUEUE_KONSOLE_PRIVATE_LIBRARY:-} ]]; then
    cmake_args+=("-DKONSOLE_PRIVATE_LIBRARY=$DYNQUEUE_KONSOLE_PRIVATE_LIBRARY")
fi

cmake -S "$source_dir" -B "$build_dir" "${cmake_args[@]}"
cmake --build "$build_dir" --parallel
cmake --install "$build_dir"

temporary="$marker.tmp.$$"
printf '%s\n' "$version" >"$temporary"
chmod 600 "$temporary" 2>/dev/null || true
mv -f -- "$temporary" "$marker"
