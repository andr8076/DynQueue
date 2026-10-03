# DynQueue

DynQueue is a small Konsole plugin for Bash command chains. It leaves normal
terminal use alone and only opens a right-hand sidebar when a typed command
contains a real top-level `&&` chain.

For example:

```bash
ffmpeg input.mkv output.mkv && rsync output.mkv server:/videos/ && notify-send "Done"
```

becomes an editable queue. The first item runs immediately in the existing
interactive shell. Waiting items can be added, removed, or moved. A failure
stops the remaining items, matching normal `&&` behaviour.

## Implementation

The current Konsole source provides a native plugin interface for widgets and
active-view notifications, but it does not expose a supported pre-execution
hook for shell input. DynQueue therefore uses two small pieces:

- a native Qt/KDE plugin that provides one hidden `QDockWidget` per Konsole
  window;
- a Bash integration loaded from `.bashrc`.

The Bash integration uses a DEBUG trap only to inspect the first command for a
new history entry. It asks `dynqueue-parser.py` to find top-level `&&`
operators while respecting quotes, escapes, comments, pipelines, command
substitutions, subshells, and brace groups. Ordinary commands are not
rewritten. Queue items are then evaluated in the current shell, so `cd`,
`export`, aliases, functions, redirection, and interactive programs keep the
same shell context.

The two sides use a user-owned, per-session directory below
`${XDG_RUNTIME_DIR:-/tmp}/dynqueue-$UID`. The plugin reads atomic snapshots and
writes a small pending-edit file. No network service, daemon, or database is
created.

## Requirements

- Linux
- KDE Plasma 6 / Qt 6 / KDE Frameworks 6
- Konsole with `libkonsoleapp` and `libkonsoleprivate`
- Bash
- Python 3
- CMake, a C++ compiler, ECM, Qt6, KF6 CoreAddons, KF6 I18n, and KF6 XmlGui

Konsole currently does not ship a separate external-plugin SDK. DynQueue keeps
the minimum declarations in `src/plugin/konsole_api_compat.h`. The plugin
locates the active session through Konsole's exported
`SessionDisplayConnection` QObject instead of assuming the private
`SessionController` member layout. It also checks the exact Konsole version
exposed by the host process and disables its UI when the version is missing or
differs from the version used for the build. Rebuild DynQueue after every
Konsole upgrade.

## Install on Arch / EndeavourOS

The user-local installer is the normal route:

```bash
./install.sh
```

It builds the plugin, installs it below `~/.local`, adds an idempotent Bash
source block to `~/.bashrc`, and adds the user plugin directory to the Plasma
session environment. The source block checks for an existing Bash `DEBUG` trap
first. If one exists, DynQueue stays disabled so it cannot replace shell
customizations. DynQueue does not install or synthesize a `SIGINT` trap, so an
existing Ctrl+C handler continues to belong to the user. Start a new
Plasma session, or launch Konsole once with the printed `QT_PLUGIN_PATH`
command.

For a system-wide plugin installation:

```bash
./install.sh --system
```

The installer checks the usual build tools and gives the relevant package
names if something is missing. It does not modify the Konsole source tree.

To remove the user-local installation and only the DynQueue block it added to
`.bashrc`:

```bash
./uninstall.sh
```

## Development build

If the installed libraries are not in a standard search path, pass their
absolute locations:

```bash
cmake -S . -B build \
  -DDYNQUEUE_KONSOLE_VERSION="$(konsole --version | grep -Eo '[0-9]+\\.[0-9]+\\.[0-9]+' | head -n1)" \
  -DKONSOLE_APP_LIBRARY=/usr/lib/libkonsoleapp.so \
  -DKONSOLE_PRIVATE_LIBRARY=/usr/lib/libkonsoleprivate.so
cmake --build build
```

The quick reinstall path is:

```bash
cmake --build build && cmake --install build --prefix "$HOME/.local"
```

## Tests

```bash
python3 -m unittest discover -s tests -v
bash -n src/shell/dynqueue.bash
./tests/test_shell_integration.sh
```

The tests cover real chains, quoted `&&`, pipelines, redirection, command
substitution, brace groups, `[[ ... ]]`, shell control constructs, shell-state
mutations, `cd`, `export`, incomplete chains, normal command handling, failure
stopping the remaining queue, safe queue cancellation, existing SIGINT and
DEBUG traps, and Bash `errexit`/`errtrace`/`functrace` options.
Manual checks should also cover ordinary `ls`, `ssh`, `nano`, `htop`, Python,
and `sudo` input, plus:

```bash
false && echo SHOULD_NOT_RUN
true && echo SHOULD_RUN
cd /tmp && pwd
export TEST=hello && echo "$TEST"
```

The sidebar has a Stop button. It requests a safe stop after the current item
finishes; it does not send a signal into the running program. Ctrl+C remains
the normal way to interrupt the current command immediately.

## MVP boundaries

- Bash is supported first; Zsh is not installed or modified.
- Multiline command editing and here-documents are left on the normal Bash
  path for now.
- Shell control constructs such as `if`, `for`, `case`, and `[[ ... ]]` are
  rejected conservatively when they make queue boundaries ambiguous. Bash
  executes them normally instead of DynQueue rewriting them.
- Queue state is intentionally not persistent.
- The sidebar can be hidden without stopping a queue.
- The current shell bridge uses Bash's DEBUG trap because Konsole does not
  expose a pre-execution plugin hook. Bash's `extdebug` mode skips every
  simple command in the replaced history entry, so DynQueue does not send a
  synthetic signal. The installer refuses to enable DynQueue when a custom
  DEBUG trap is already present; custom SIGINT traps are supported.
