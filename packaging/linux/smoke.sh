#!/usr/bin/env bash
# Runtime smoke test of a built music-service on Linux or macOS:
# init, serve, readiness, an authenticated request, offline management refused while
# running, SIGTERM, a clean exit, the shutdown order, and the lock released.
# Usage: packaging/linux/smoke.sh path/to/music-service [port]
set -euo pipefail

# Resolve the executable without requiring GNU realpath on macOS.
bin="$(cd "$(dirname "$1")" && pwd -P)/$(basename "$1")"
port="${2:-17642}"
work="$(mktemp -d)"
pid=""
# Only ever signal the service itself: "kill 0" would hit the whole process group.
cleanup() {
  if [ -n "$pid" ]; then kill "$pid" 2>/dev/null || true; fi
  rm -rf "$work"
}
trap cleanup EXIT
mkdir -p "$work/downloads"
cat > "$work/config.json" <<JSON
{
  "config_version": 1,
  "data_dir": "$work/data",
  "control": {"host": "127.0.0.1", "port": $port},
  "destinations": [{"id": "dst_default", "name": "downloads", "root": "$work/downloads", "principals": ["prn_local"]}]
}
JSON
run() { "$bin" --config "$work/config.json" "$@"; }

echo "== system"
uname -srm
case "$(uname -s)" in
  Darwin) sw_vers; otool -L "$bin" ;;
  *) ldd --version | head -n 1; ldd "$bin" | sed 's/ (0x[0-9a-f]*)//' ;;
esac

echo "== version, check-config, init"
run version
run check-config
run init
token="$(head -n 1 "$work/data/secrets/bootstrap-token.txt")"

echo "== serve"
# The binary itself in the background (not the run function, which would be a
# subshell), so $! is the service and receives the SIGTERM below.
"$bin" --config "$work/config.json" serve > "$work/stdout.txt" 2> "$work/stderr.txt" &
pid=$!
for _ in $(seq 1 100); do
  grep -q '"event":"ready"' "$work/stdout.txt" 2>/dev/null && break
  sleep 0.1
done
cat "$work/stdout.txt"
grep -q '"event":"ready"' "$work/stdout.txt"

run healthcheck
curl -fsS -H "Authorization: Bearer $token" "http://127.0.0.1:$port/api/v1/info"; echo
status="$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$port/api/v1/info")"
[ "$status" = 401 ] || { echo "expected 401 without a token, got $status"; exit 1; }
if run token list --principal prn_local; then
  echo "management must be refused while the service runs"; exit 1
fi

echo "== SIGTERM"
kill -TERM "$pid"
code=0; wait "$pid" || code=$?
pid=""
[ "$code" = 0 ] || { echo "service exited with $code"; cat "$work/stderr.txt"; exit 1; }
order="$(grep -o '"event":"shutdown\.[a-z_]*"' "$work/stderr.txt" | tr '\n' ' ')"
echo "shutdown order: $order"
[[ "$order" == *listeners_stopped*database_closed*lock_released* ]]
run token list --principal prn_local
echo "smoke test passed"
