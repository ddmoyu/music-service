#!/usr/bin/env bash
# The online Kuwo loop (packaging/e2e/kuwo_e2e.py) against the container image: the
# three listeners published on host loopback, docker restart (SIGTERM, same volumes)
# as the restart step. Needs network access to Kuwo from the host running Docker.
# Usage: packaging/docker/e2e.sh IMAGE
set -euo pipefail

image="$1"
here="$(cd "$(dirname "$0")" && pwd -P)"
suffix="$$"
data="ms-e2e-data-$suffix"
downloads="ms-e2e-downloads-$suffix"
name="ms-e2e-$suffix"
cleanup() {
  docker logs "$name" > "${TMPDIR:-/tmp}/ms-e2e-$suffix.log" 2>&1 || true
  docker rm -f "$name" >/dev/null 2>&1 || true
  docker volume rm -f "$data" "$downloads" >/dev/null 2>&1 || true
}
trap cleanup EXIT
volumes=(-v "$data:/data" -v "$downloads:/downloads")

docker run --rm "${volumes[@]}" "$image" init
token="$(docker run --rm "${volumes[@]}" --entrypoint head "$image" -n 1 /data/secrets/bootstrap-token.txt)"
# The public URLs in the image name 127.0.0.1:17632-17634, so the same host ports.
docker run -d --name "$name" -p 127.0.0.1:17632:17632 -p 127.0.0.1:17633:17633 -p 127.0.0.1:17634:17634 \
  "${volumes[@]}" "$image" >/dev/null
for _ in $(seq 1 60); do
  [ "$(docker inspect --format '{{.State.Health.Status}}' "$name")" = healthy ] && break
  sleep 1
done

python3 -I "$here/../e2e/kuwo_e2e.py" --api http://127.0.0.1:17632 --token "$token" \
  --restart-command "docker restart --time 30 $name"
