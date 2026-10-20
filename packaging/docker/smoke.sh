#!/usr/bin/env bash
# Runtime smoke test of the container image: init on volumes, serve as a non-root
# user, health check, an authenticated request through the published port,
# management refused while running, docker stop (SIGTERM) with a clean exit and the
# shutdown order, then a new container on the same volumes keeps the identities.
# Usage: packaging/docker/smoke.sh IMAGE [host-port]
set -euo pipefail

image="$1"
port="${2:-17632}"
suffix="$$"
data="ms-smoke-data-$suffix"
downloads="ms-smoke-downloads-$suffix"
name="ms-smoke-$suffix"
cleanup() {
  docker rm -f "$name" >/dev/null 2>&1 || true
  docker volume rm -f "$data" "$downloads" >/dev/null 2>&1 || true
}
trap cleanup EXIT
volumes=(-v "$data:/data" -v "$downloads:/downloads")

echo "== image"
docker image inspect "$image" --format 'size={{.Size}} os={{.Os}} arch={{.Architecture}} user={{.Config.User}}'

echo "== init"
docker run --rm "${volumes[@]}" "$image" init
token="$(docker run --rm "${volumes[@]}" --entrypoint head "$image" -n 1 /data/secrets/bootstrap-token.txt)"

start() {
  docker run -d --name "$name" -p "127.0.0.1:$port:17632" "${volumes[@]}" "$image" >/dev/null
  for _ in $(seq 1 60); do
    health="$(docker inspect --format '{{.State.Health.Status}}' "$name")"
    [ "$health" = healthy ] && break
    sleep 1
  done
  echo "health: $health"
  [ "$health" = healthy ]
}

echo "== serve"
start
[ "$(docker exec "$name" id -u)" = 10001 ]
curl -fsS -H "Authorization: Bearer $token" "http://127.0.0.1:$port/api/v1/info"; echo
if docker exec "$name" /usr/local/bin/music-service --config /etc/music-service/config.json token list --principal prn_local; then
  echo "management must be refused while the service runs"; exit 1
fi

echo "== docker stop"
docker stop --time 30 "$name" >/dev/null
code="$(docker inspect --format '{{.State.ExitCode}}' "$name")"
order="$(docker logs "$name" 2>&1 | grep -o '"event":"shutdown\.[a-z_]*"' | tr '\n' ' ')"
echo "exit code: $code; shutdown order: $order"
[ "$code" = 0 ]
[[ "$order" == *listeners_stopped*database_closed*lock_released* ]]

echo "== new container, same volumes"
docker rm "$name" >/dev/null
start
curl -fsS -H "Authorization: Bearer $token" "http://127.0.0.1:$port/api/v1/info" >/dev/null
docker stop --time 30 "$name" >/dev/null
echo "container smoke test passed"
