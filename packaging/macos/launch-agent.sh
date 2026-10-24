#!/bin/bash
# Per-user launchd agent. Uses only macOS system commands at runtime.
# Install after check-config/init. Package and config must remain at their paths.
set -euo pipefail

[[ $(uname -s) == Darwin ]] || { echo 'macOS is required' >&2; exit 2; }
[[ $(id -u) != 0 ]] || { echo 'run as the logged-in user, without sudo' >&2; exit 2; }
label=io.github.ddmoyu.music-service
domain="gui/$(id -u)"
plist="$HOME/Library/LaunchAgents/$label.plist"
here=$(cd "$(dirname "$0")" && pwd -P)
package=$(cd "$here/../../.." && pwd -P)
bin="$package/bin/music-service"
temporary=''
trap 'if [[ -n "$temporary" ]]; then rm -f "$temporary"; fi' EXIT

loaded() { /bin/launchctl print "$domain/$label" >/dev/null 2>&1; }

stop_agent() {
    if loaded; then
        local pid
        pid=$(/bin/launchctl print "$domain/$label" | awk '$1 == "pid" && $2 == "=" {print $3}')
        /bin/launchctl bootout "$domain/$label"
        if [[ -n "$pid" ]]; then
            for ((attempt=0; attempt<60; ++attempt)); do
                kill -0 "$pid" 2>/dev/null || return 0
                sleep 1
            done
            echo 'service did not stop within 60 seconds; data has been preserved' >&2
            return 1
        fi
    fi
}

case "${1:-}" in
    install)
        [[ $# == 2 ]] || { echo "usage: $0 install CONFIG.json" >&2; exit 2; }
        [[ ! -e "$plist" ]] || { echo "agent already installed: $plist; uninstall it first" >&2; exit 1; }
        if loaded; then echo 'agent is already loaded; stop it before installing' >&2; exit 1; fi
        /bin/launchctl print "$domain" >/dev/null
        config="$(cd "$(dirname "$2")" && pwd -P)/$(basename "$2")"
        "$bin" --config "$config" check-config
        # Requires an initialized instance and refuses an already-running owner.
        "$bin" --config "$config" token list --principal prn_local >/dev/null
        [[ $(/usr/bin/plutil -extract logging.file_enabled raw -o - "$config") == true ]] || {
            echo 'enable logging.file_enabled in the configuration before installing the agent' >&2; exit 1;
        }
        mkdir -p "$(dirname "$plist")"
        temporary=$(mktemp "$(dirname "$plist")/.music-service.XXXXXX")
        /usr/bin/plutil -create xml1 "$temporary"
        /usr/bin/plutil -insert Label -string "$label" "$temporary"
        /usr/bin/plutil -insert ProgramArguments -json '[]' "$temporary"
        /usr/bin/plutil -insert ProgramArguments.0 -string "$bin" "$temporary"
        /usr/bin/plutil -insert ProgramArguments.1 -string --config "$temporary"
        /usr/bin/plutil -insert ProgramArguments.2 -string "$config" "$temporary"
        /usr/bin/plutil -insert ProgramArguments.3 -string serve "$temporary"
        /usr/bin/plutil -insert RunAtLoad -bool true "$temporary"
        /usr/bin/plutil -insert KeepAlive -bool true "$temporary"
        /usr/bin/plutil -insert ThrottleInterval -integer 10 "$temporary"
        /usr/bin/plutil -insert ExitTimeOut -integer 60 "$temporary"
        /usr/bin/plutil -insert StandardOutPath -string /dev/null "$temporary"
        /usr/bin/plutil -insert StandardErrorPath -string /dev/null "$temporary"
        /usr/bin/plutil -lint "$temporary"
        chmod 600 "$temporary"
        mv "$temporary" "$plist"
        temporary=''
        if ! /bin/launchctl bootstrap "$domain" "$plist"; then
            echo "bootstrap failed; configuration and agent file preserved at $plist" >&2
            exit 1
        fi
        for ((attempt=0; attempt<60; ++attempt)); do
            if "$bin" --config "$config" healthcheck >/dev/null 2>&1; then
                echo "agent ready: $domain/$label; logs: data_dir/logs/music-service.log"
                exit 0
            fi
            sleep 0.5
        done
        stop_agent
        echo 'agent startup failed; inspect the application log or run serve in the foreground' >&2
        exit 1
        ;;
    start)
        [[ $# == 1 && -f "$plist" ]] || { echo 'install the agent first' >&2; exit 2; }
        loaded || /bin/launchctl bootstrap "$domain" "$plist"
        ;;
    stop)
        [[ $# == 1 ]] || exit 2
        stop_agent
        ;;
    uninstall)
        [[ $# == 1 ]] || exit 2
        stop_agent
        rm -f "$plist"
        echo 'agent removed; package, configuration, data and downloads preserved'
        ;;
    *) echo "usage: $0 install CONFIG.json | start | stop | uninstall" >&2; exit 2 ;;
esac
