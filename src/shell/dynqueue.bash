# DynQueue Bash integration.
#
# This file is sourced from an interactive Bash session.  It leaves ordinary
# input alone.  A DEBUG trap only inspects the first command associated with a
# new history entry; when the entry contains a real top-level && chain, the
# chain is parsed by dynqueue-parser.py and executed item-by-item in this same
# shell.  A SIGINT then cancels Bash's original, still-pending compound line.

if [[ $- == *i* ]]; then
    if [[ -z ${__DYNQUEUE_BASH_LOADED:-} ]]; then
        if [[ ${DYNQUEUE_BASH_PRECHECKED:-0} != 1 ]]; then
            printf '[DynQueue] not loaded: source it through the installed .bashrc integration block so existing shell traps can be checked safely.\n' >&2
        else
        __DYNQUEUE_BASH_LOADED=1

        __dynqueue_parser="${DYNQUEUE_PARSER:-}"
        if [[ -z "$__dynqueue_parser" ]]; then
            __dynqueue_script_dir=${BASH_SOURCE[0]%/*}
            __dynqueue_parser="${__dynqueue_script_dir}/../../libexec/dynqueue-parser.py"
        fi
        if [[ ! -x "$__dynqueue_parser" ]]; then
            __dynqueue_parser=$(command -v dynqueue-parser.py 2>/dev/null || true)
        fi

        __dynqueue_uid=${UID:-$(id -u)}
        __dynqueue_runtime_base="${XDG_RUNTIME_DIR:-/tmp}/dynqueue-${__dynqueue_uid}"
        __dynqueue_session_raw="${SHELL_SESSION_ID:-}"
        if [[ -z "$__dynqueue_session_raw" ]]; then
            __dynqueue_session_raw="terminal-$$-$(tty 2>/dev/null || printf 'notty')"
        fi
        __dynqueue_session_id=${__dynqueue_session_raw//[^[:alnum:]._-]/_}
        __dynqueue_session_dir="${__dynqueue_runtime_base}/${__dynqueue_session_id}"

        # DynQueue's final safety step uses SIGINT to prevent Bash from
        # continuing the original compound command after the queue has run.
        # An existing SIGINT trap could swallow that signal and allow the
        # original command to run a second time, so leave the shell entirely
        # untouched when one is already installed.
        if [[ -n "$__dynqueue_parser" ]]; then
            mkdir -p "$__dynqueue_session_dir" 2>/dev/null || true
        fi
        __dynqueue_trap_probe="${__dynqueue_session_dir}/trap-probe.$$"
        trap -p INT >"$__dynqueue_trap_probe" 2>/dev/null || true
        IFS= read -r __dynqueue_existing_int_trap <"$__dynqueue_trap_probe" || __dynqueue_existing_int_trap=
        rm -f -- "$__dynqueue_trap_probe"
        if [[ -n "$__dynqueue_parser" ]] && [[ -n "$__dynqueue_existing_int_trap" ]]; then
            printf '[DynQueue] disabled: an existing SIGINT trap was detected; normal shell behavior is unchanged.\n' >&2
        fi

        if [[ -n "$__dynqueue_parser" ]] && [[ -z "$__dynqueue_existing_int_trap" ]] \
            && mkdir -p "$__dynqueue_session_dir" 2>/dev/null; then
            chmod 700 "$__dynqueue_runtime_base" "$__dynqueue_session_dir" 2>/dev/null || true
            printf '%s\n' "$$" >"${__dynqueue_session_dir}/shell.pid"
            chmod 600 "${__dynqueue_session_dir}/shell.pid" 2>/dev/null || true

            __dynqueue_queue_id=
            __dynqueue_current_index=-1
            __dynqueue_last_histcmd=
            __dynqueue_running=0
            declare -ga __dynqueue_item_ids=()
            declare -ga __dynqueue_item_commands=()
            declare -ga __dynqueue_item_states=()

            __dynqueue_debug_log() {
                [[ ${DYNQUEUE_DEBUG:-0} == 1 ]] || return 0
                printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*" >>"${__dynqueue_session_dir}/debug.log"
            }

            __dynqueue_b64_encode() {
                printf '%s' "$1" | base64 | tr -d '\n'
            }

            __dynqueue_b64_decode() {
                printf '%s' "$1" | base64 --decode 2>/dev/null
            }

            __dynqueue_write_snapshot() {
                local temporary="${__dynqueue_session_dir}/snapshot.tmp.$$.$RANDOM"
                {
                    printf 'DYNQUEUE\t1\t%s\n' "$__dynqueue_queue_id"
                    local index encoded
                    for ((index = 0; index < ${#__dynqueue_item_ids[@]}; index++)); do
                        encoded=$(__dynqueue_b64_encode "${__dynqueue_item_commands[index]}")
                        printf '%s\t%s\t%s\n' \
                            "${__dynqueue_item_ids[index]}" \
                            "${__dynqueue_item_states[index]}" \
                            "$encoded"
                    done
                } >"$temporary" || return 1
                chmod 600 "$temporary" 2>/dev/null || true
                mv -f "$temporary" "${__dynqueue_session_dir}/snapshot"
            }

            __dynqueue_cancel_requested() {
                local requested=
                [[ -f "${__dynqueue_session_dir}/cancel" ]] || return 1
                IFS= read -r requested <"${__dynqueue_session_dir}/cancel" || requested=
                if [[ "$requested" == "$__dynqueue_queue_id" ]]; then
                    rm -f -- "${__dynqueue_session_dir}/cancel"
                    return 0
                fi
                # A request for an older queue must never affect a new queue.
                rm -f -- "${__dynqueue_session_dir}/cancel"
                return 1
            }

            __dynqueue_stop_remaining() {
                local from_index=$1 remaining
                for ((remaining = from_index; remaining < ${#__dynqueue_item_states[@]}; remaining++)); do
                    __dynqueue_item_states[remaining]=stopped
                done
                __dynqueue_write_snapshot
                __dynqueue_running=0
                __dynqueue_debug_log "Queue stopped by user request"
                return 130
            }

            __dynqueue_apply_pending() {
                local pending="${__dynqueue_session_dir}/pending"
                [[ -e "$pending" ]] || return 0

                local -a new_ids=()
                local -a new_commands=()
                local -a pending_lines=()
                local pending_header pending_line item_id encoded extra command invalid=0
                mapfile -t pending_lines <"$pending" || {
                    rm -f -- "$pending"
                    return 0
                }
                if ((${#pending_lines[@]} == 0)); then
                    rm -f -- "$pending"
                    return 0
                fi
                pending_header=${pending_lines[0]}
                if [[ "$pending_header" != $'DYNQUEUE_PENDING\t1\t'"$__dynqueue_queue_id" ]]; then
                    rm -f -- "$pending"
                    __dynqueue_debug_log "Ignored stale pending queue update"
                    return 0
                fi
                for pending_line in "${pending_lines[@]:1}"; do
                    IFS=$'\t' read -r item_id encoded extra <<<"$pending_line"
                    [[ -z "$item_id" && -z "$encoded" ]] && continue
                    if [[ ! "$item_id" =~ ^[A-Za-z0-9._-]+$ || -z "$encoded" || -n "$extra" ]]; then
                        invalid=1
                        break
                    fi
                    command=$(__dynqueue_b64_decode "$encoded") || {
                        invalid=1
                        break
                    }
                    new_ids+=("$item_id")
                    new_commands+=("$command")
                done

                if ((invalid)); then
                    rm -f -- "$pending"
                    __dynqueue_debug_log "Ignored malformed pending queue update"
                    return 0
                fi

                local completed_count=$((__dynqueue_current_index + 1))
                __dynqueue_item_ids=("${__dynqueue_item_ids[@]:0:completed_count}" "${new_ids[@]}")
                __dynqueue_item_commands=("${__dynqueue_item_commands[@]:0:completed_count}" "${new_commands[@]}")
                __dynqueue_item_states=()
                local index
                for ((index = 0; index < completed_count; index++)); do
                    __dynqueue_item_states+=("completed")
                done
                for ((index = completed_count; index < ${#__dynqueue_item_ids[@]}; index++)); do
                    __dynqueue_item_states+=("waiting")
                done

                rm -f -- "$pending"
                __dynqueue_write_snapshot
                __dynqueue_debug_log "Applied pending queue edit"
            }

            __dynqueue_run_queue() {
                local encoded command
                __dynqueue_queue_id="q-${HISTCMD:-0}-${BASHPID}-${RANDOM}"
                __dynqueue_current_index=-1
                __dynqueue_item_ids=()
                __dynqueue_item_commands=()
                __dynqueue_item_states=()

                local item_number=0
                for encoded in "$@"; do
                    command=$(__dynqueue_b64_decode "$encoded") || return 2
                    __dynqueue_item_ids+=("item-${item_number}")
                    __dynqueue_item_commands+=("$command")
                    __dynqueue_item_states+=("waiting")
                    item_number=$((item_number + 1))
                done

                __dynqueue_running=1
                __dynqueue_write_snapshot
                __dynqueue_debug_log "Detected && chain with ${#@} items"

                local index=0 status
                while ((index < ${#__dynqueue_item_ids[@]})); do
                    if __dynqueue_cancel_requested; then
                        __dynqueue_stop_remaining "$index"
                        return $?
                    fi

                    __dynqueue_current_index=$index
                    __dynqueue_item_states[index]=running
                    __dynqueue_write_snapshot
                    __dynqueue_debug_log "Started item $index"

                    # The command runs in the current interactive shell.  This
                    # preserves cd, export, aliases, functions, redirection,
                    # interactive programs, and the user's normal environment.
                    if eval "${__dynqueue_item_commands[index]}"; then
                        status=0
                    else
                        status=$?
                    fi
                    if ((status == 0)); then
                        __dynqueue_item_states[index]=completed
                        __dynqueue_write_snapshot
                        __dynqueue_debug_log "Item $index exited 0"
                        __dynqueue_apply_pending
                        index=$((index + 1))
                    else
                        __dynqueue_item_states[index]=failed
                        local remaining
                        for ((remaining = index + 1; remaining < ${#__dynqueue_item_states[@]}; remaining++)); do
                            __dynqueue_item_states[remaining]=stopped
                        done
                        rm -f -- "${__dynqueue_session_dir}/cancel"
                        __dynqueue_write_snapshot
                        __dynqueue_debug_log "Item $index failed with status $status"
                        __dynqueue_running=0
                        return "$status"
                    fi
                done

                __dynqueue_running=0
                rm -f -- "${__dynqueue_session_dir}/pending" "${__dynqueue_session_dir}/cancel"
                __dynqueue_write_snapshot
                __dynqueue_debug_log "Queue finished"
                return 0
            }

            __dynqueue_debug_trap() {
                local saved_status=$?
                [[ $__dynqueue_running == 0 ]] || return "$saved_status"
                [[ $BASH_SUBSHELL == 0 ]] || return "$saved_status"

                # The abort path is only safe while SIGINT retains Bash's
                # normal behavior.  If a user installs a SIGINT trap after
                # DynQueue was sourced, leave this command on the ordinary
                # Bash path instead of risking duplicate execution.
                local current_int_trap
                __dynqueue_trap_probe="${__dynqueue_session_dir}/int-trap-probe.$$.$RANDOM"
                trap -p INT >"$__dynqueue_trap_probe" 2>/dev/null || true
                IFS= read -r current_int_trap <"$__dynqueue_trap_probe" || current_int_trap=
                rm -f -- "$__dynqueue_trap_probe"
                [[ -z "$current_int_trap" ]] || return "$saved_status"

                local current_history="${HISTCMD:-}"
                [[ -n "$current_history" && "$current_history" != "$__dynqueue_last_histcmd" ]] || return "$saved_status"
                __dynqueue_last_histcmd=$current_history

                local old_history_time_format=${HISTTIMEFORMAT:-}
                HISTTIMEFORMAT=
                local history_line
                history_line=$(history 1)
                HISTTIMEFORMAT=$old_history_time_format

                local entered_line
                if [[ "$history_line" =~ ^[[:space:]]*([0-9]+)[[:space:]](.*)$ ]] \
                    && [[ ${BASH_REMATCH[1]} == "$current_history" ]]; then
                    entered_line=${BASH_REMATCH[2]}
                else
                    return "$saved_status"
                fi

                # Avoid changing multiline editing and here-document input in
                # this MVP.  A future shell adapter can handle those forms.
                [[ "$entered_line" != *$'\n'* && "$entered_line" != *$'\r'* ]] || return "$saved_status"
                [[ "$entered_line" == *'&&'* ]] || return "$saved_status"
                bash -n -c "$entered_line" 2>/dev/null || return "$saved_status"

                local -a encoded_items=()
                mapfile -t encoded_items < <(printf '%s' "$entered_line" | "$__dynqueue_parser" 2>/dev/null)
                (( ${#encoded_items[@]} >= 2 )) || return "$saved_status"

                local queue_status
                if __dynqueue_run_queue "${encoded_items[@]}"; then
                    queue_status=0
                else
                    queue_status=$?
                fi
                # Bash has no supported DEBUG-trap return value that cancels
                # the current compound command.  SIGINT is the same abort path
                # Bash uses for Ctrl+C and leaves the just-typed line out of
                # the shell's execution stream after the queue has run.
                kill -INT "$$" 2>/dev/null
                return "$queue_status"
            }

            __dynqueue_install_debug_trap() {
                trap '__dynqueue_debug_trap' DEBUG
            }

            __dynqueue_install_debug_trap
        fi
        fi
    fi
fi
