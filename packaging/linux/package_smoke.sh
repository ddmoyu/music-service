#!/usr/bin/env bash
# Checks a Linux release package: unpacks it under a Chinese, deep path; checks the
# documented layout; prints the glibc the program needs and fails if it links libstdc++
# or libgcc dynamically; runs smoke.sh on the unpacked program. With --service (root and
# systemd required, as on CI runners) it also installs the package with install.sh, checks
# the running unit, its user, a token request, a clean SIGTERM stop (exit status 0, the
# shutdown in the journal), a restart, and removes everything again.
# Usage: packaging/linux/package_smoke.sh PACKAGE.tar.gz [--service]
set -euo pipefail

package=$(realpath "$1")
service=${2:-}
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
deep="$work/很长的目录名-$(printf 'x%.0s' {1..40})/很长的目录名-$(printf 'x%.0s' {1..40})"
mkdir -p "$deep"
cleanup() { rm -rf "$work"; }
trap cleanup EXIT

tar -xzf "$package" -C "$deep"
root=$(find "$deep" -mindepth 1 -maxdepth 1 -type d | head -n 1)
echo "== unpacked to $root"
for file in bin/music-service share/music-service/README.md share/music-service/config.example.json \
    share/music-service/licenses/THIRD-PARTY.md share/music-service/service/music-service.service \
    share/music-service/service/install.sh share/music-service/integration/docs/api/openapi.json \
    share/music-service/integration/examples/frontend/client.mjs; do
    [[ -e "$root/$file" ]] || { echo "missing $file" >&2; exit 1; }
done
exe="$root/bin/music-service"
echo "== dependencies"
ldd "$exe"
if ldd "$exe" | grep -E 'libstdc\+\+|libgcc_s'; then
    echo "the C++ runtime is linked dynamically" >&2
    exit 1
fi
echo "needs glibc symbols up to: $(objdump -T "$exe" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -n 1)"
bash "$here/smoke.sh" "$exe"

[[ "$service" == "--service" ]] || exit 0
echo "== systemd service"
downloads=/srv/music-service-smoke
mkdir -p "$downloads"
id music-service >/dev/null 2>&1 || useradd --system --no-create-home --home-dir /nonexistent \
    --shell /usr/sbin/nologin music-service
chown music-service "$downloads"
remove() {
    systemctl disable --now music-service.service >/dev/null 2>&1 || true
    rm -rf /etc/systemd/system/music-service.service /etc/systemd/system/music-service.service.d
    systemctl daemon-reload
    rm -rf /opt/music-service /etc/music-service /var/lib/music-service "$downloads"
    userdel music-service >/dev/null 2>&1 || true
    cleanup
}
trap remove EXIT
bash "$root/share/music-service/service/install.sh" "$downloads"
systemctl is-active --quiet music-service.service
[[ $(systemctl show -p User --value music-service.service) == music-service ]]
[[ $(stat -c %U /var/lib/music-service/secrets) == music-service ]]
token=$(head -n 1 /var/lib/music-service/secrets/bootstrap-token.txt)
curl -fsS -H "Authorization: Bearer $token" http://127.0.0.1:17632/api/v1/info | grep -q '"service":"music-service"'
since=$(date '+%Y-%m-%d %H:%M:%S')
systemctl stop music-service.service
[[ $(systemctl show -p ExecMainStatus --value music-service.service) == 0 ]]
journalctl -u music-service --since "$since" --no-pager | grep -q 'shutdown.started'
systemctl start music-service.service
for _ in $(seq 1 50); do
    curl -fsS -H "Authorization: Bearer $token" http://127.0.0.1:17632/api/v1/info >/dev/null 2>&1 && break
    sleep 0.2
done
curl -fsS -H "Authorization: Bearer $token" http://127.0.0.1:17632/api/v1/info >/dev/null
echo "service smoke test passed"
