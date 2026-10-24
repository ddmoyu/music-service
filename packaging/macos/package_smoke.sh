#!/bin/bash
# Validate the extracted archive rather than a binary left in the build directory.
# Usage: bash packaging/macos/package_smoke.sh PACKAGE.tar.gz
set -euo pipefail
[[ $(uname -s) == Darwin ]] || { echo 'macOS is required' >&2; exit 2; }
[[ $# == 1 ]] || { echo "usage: $0 PACKAGE.tar.gz" >&2; exit 2; }
package="$(cd "$(dirname "$1")" && pwd -P)/$(basename "$1")"
here=$(cd "$(dirname "$0")" && pwd -P)
work=$(mktemp -d)
work=$(cd "$work" && pwd -P)
agent=''
cleanup() {
    local result=$?
    trap - EXIT
    if [[ -n "$agent" ]] && ! bash "$agent" uninstall; then
        echo "agent cleanup failed; preserving its configuration and data at $work" >&2
        exit 1
    fi
    rm -rf "$work"
    exit "$result"
}
trap cleanup EXIT
deep="$work/中文目录 with spaces/独立发行包"
mkdir -p "$deep"
tar -xzf "$package" -C "$deep"
roots=("$deep"/*)
[[ ${#roots[@]} == 1 && -d "${roots[0]}" ]] || { echo 'expected one archive root' >&2; exit 1; }
root=${roots[0]}
for file in bin/music-service share/music-service/README.md share/music-service/MACOS.md \
    share/music-service/config.macos.example.json share/music-service/macos-package.json \
    share/music-service/licenses/THIRD-PARTY.md share/music-service/licenses/libcxx-LICENSE.txt \
    share/music-service/licenses/libcxxabi-LICENSE.txt share/music-service/licenses/libunwind-LICENSE.txt \
    share/music-service/service/launch-agent.sh; do
    [[ -f "$root/$file" ]] || { echo "missing $file" >&2; exit 1; }
done
[[ ! -e "$root/share/music-service/service/music-service.service" ]]
[[ ! -e "$root/share/music-service/service/install.sh" ]]
exe="$root/bin/music-service"
[[ $(/usr/bin/lipo -archs "$exe") == "$(uname -m)" ]]

# No Homebrew PATH or DYLD fallback is available to the extracted program.
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
unset DYLD_LIBRARY_PATH DYLD_FALLBACK_LIBRARY_PATH DYLD_FRAMEWORK_PATH DYLD_FALLBACK_FRAMEWORK_PATH DYLD_INSERT_LIBRARIES
images=("$root"/lib/music-service/*.dylib "$exe")
for image in "${images[@]}"; do
    [[ -f "$image" ]]
    /usr/bin/codesign --verify --strict --verbose=2 "$image"
    /usr/bin/otool -L "$image"
    /usr/bin/otool -l "$image" | awk '
      $1 == "cmd" && $2 == "LC_RPATH" {bad=1}
      END {exit bad ? 1 : 0}'
    while IFS= read -r dependency; do
        case "$dependency" in
            /usr/lib/*|/System/Library/*|@rpath/$(basename "$image")) ;;
            @loader_path/*)
                target="$(dirname "$image")/${dependency#@loader_path/}"
                [[ -f "$target" ]]
                resolved_dir=$(cd "$(dirname "$target")" && pwd -P)
                [[ "$resolved_dir" == "$root/lib/music-service" ]]
                ;;
            *) echo "non-relocatable dependency: $dependency" >&2; exit 1 ;;
        esac
    done < <(/usr/bin/otool -L "$image" | sed -n '2,$s/^[[:space:]]*\(.*\) (compatibility version.*/\1/p')
done
cat "$root/share/music-service/macos-package.json"
echo "archive bytes: $(stat -f %z "$package")"
du -sk "$root"
bash "$here/../linux/smoke.sh" "$exe"

echo '== launchd agent'
# Never replace a user agent already present on a developer's machine.
[[ ! -e "$HOME/Library/LaunchAgents/io.github.ddmoyu.music-service.plist" ]]
if /bin/launchctl print "gui/$(id -u)/io.github.ddmoyu.music-service" >/dev/null 2>&1; then
    echo 'refusing to replace an already-loaded music-service agent' >&2
    exit 1
fi
state="$work/用户配置 with spaces"
mkdir -p "$state/downloads"
cp "$root/share/music-service/config.macos.example.json" "$state/config.json"
"$exe" --config "$state/config.json" check-config
"$exe" --config "$state/config.json" init
token=$(head -n 1 "$state/data/secrets/bootstrap-token.txt")
agent="$root/share/music-service/service/launch-agent.sh"
bash "$agent" install "$state/config.json"
curl --max-time 5 -fsS -H "Authorization: Bearer $token" http://127.0.0.1:17632/api/v1/info >/dev/null
[[ $(stat -f %u "$state/data/secrets") == "$(id -u)" ]]
bash "$agent" stop
grep -q '"event":"shutdown.lock_released"' "$state/data/logs/music-service.log"
"$exe" --config "$state/config.json" token list --principal prn_local >/dev/null
bash "$agent" start
for ((attempt=0; attempt<60; ++attempt)); do
    "$exe" --config "$state/config.json" healthcheck >/dev/null 2>&1 && break
    sleep 0.5
done
curl --max-time 5 -fsS -H "Authorization: Bearer $token" http://127.0.0.1:17632/api/v1/info >/dev/null
bash "$agent" uninstall
agent=''
[[ -f "$state/data/service.sqlite3" && -d "$state/downloads" ]]
"$exe" --config "$state/config.json" token list --principal prn_local >/dev/null
echo 'macOS package and launchd smoke test passed'
