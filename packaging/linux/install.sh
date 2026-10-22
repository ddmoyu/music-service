#!/usr/bin/env bash
# Installs music-service as a systemd service from an unpacked release package. Run as root:
#   sudo ./share/music-service/service/install.sh /srv/music
# What it does:
#   - copies the package to /opt/music-service (the program never writes there);
#   - creates the system user music-service (no login, no home);
#   - writes /etc/music-service/config.json (kept if it exists) with data_dir
#     /var/lib/music-service and the download directory as destination dst_default
#     (which music-service must already be allowed to write: nothing is chowned);
#   - runs "init" once as music-service and prints where the bootstrap token is;
#   - installs the unit plus a drop-in that allows writing the download directory, then
#     enables and starts the service and waits for it to be ready.
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 DOWNLOAD_DIR" >&2
    exit 2
fi
if [[ $(id -u) -ne 0 ]]; then
    echo "run as root" >&2
    exit 1
fi
downloads=$(realpath "$1")
[[ -d "$downloads" ]] || { echo "download directory $downloads does not exist" >&2; exit 1; }
package=$(realpath "$(dirname "$0")/../../..")
prefix=/opt/music-service
config=/etc/music-service/config.json
data=/var/lib/music-service

if ! id music-service >/dev/null 2>&1; then
    useradd --system --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin music-service
fi
if [[ "$package" != "$prefix" ]]; then
    rm -rf "$prefix"
    mkdir -p "$prefix"
    cp -a "$package"/. "$prefix"/
fi
chown -R root:root "$prefix"
chmod -R go-w "$prefix"

install -d -m 0750 -o root -g music-service /etc/music-service
if [[ ! -f "$config" ]]; then
    cat >"$config" <<JSON
{
  "config_version": 1,
  "data_dir": "$data",
  "control": {"host": "127.0.0.1", "port": 17632},
  "features": {"file_access": true},
  "destinations": [
    {"id": "dst_default", "name": "downloads", "root": "$downloads", "principals": ["prn_local"]}
  ]
}
JSON
    chown root:music-service "$config"
    chmod 0640 "$config"
fi
install -d -m 0750 -o music-service -g music-service "$data"
if ! runuser -u music-service -- test -w "$downloads"; then
    echo "music-service cannot write $downloads; grant it, for example:" >&2
    echo "  setfacl -m u:music-service:rwx,d:u:music-service:rwx $downloads" >&2
    exit 1
fi

run_as_service() { runuser -u music-service -- "$prefix/bin/music-service" --config "$config" "$@"; }
run_as_service check-config
if [[ ! -f "$data/service.sqlite3" ]]; then
    run_as_service init
    echo "bootstrap token: $data/secrets/bootstrap-token.txt (move it to the client, then delete it)"
fi

install -m 0644 "$prefix/share/music-service/service/music-service.service" /etc/systemd/system/music-service.service
install -d /etc/systemd/system/music-service.service.d
cat >/etc/systemd/system/music-service.service.d/downloads.conf <<UNIT
[Service]
ReadWritePaths=$downloads
UNIT
systemctl daemon-reload
systemctl enable --now music-service.service
for _ in $(seq 1 50); do
    if run_as_service healthcheck >/dev/null 2>&1; then
        echo "music-service is running; logs: journalctl -u music-service"
        exit 0
    fi
    sleep 0.2
done
echo "music-service started but is not ready; see journalctl -u music-service" >&2
exit 1
