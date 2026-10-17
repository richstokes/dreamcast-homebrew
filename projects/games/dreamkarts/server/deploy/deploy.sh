#!/bin/bash
# Build, test and deploy the DreamKarts lobby server to the shared host, then
# check from here that it answers with the build in src/net_version.h.
#
#   projects/games/dreamkarts/server/deploy/deploy.sh
#
# The host and its port registry live in richstokes/infra
# (servers/netsplit.vip/applications.json, app id "dreamkarts"). A deploy
# restarts the service, which empties every lobby: consoles notice within a
# second and go back to the lobby browser.
set -euo pipefail

HOST="${DREAMKARTS_DEPLOY_HOST:-root@netsplit.vip}"
ADDR="${DREAMKARTS_ADDR:-dcvmu.com:7535}"
SERVER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${SERVER_DIR}"

echo "== test"
go vet ./...
go test ./...

echo "== build"
stage="$(mktemp -d)"
trap 'rm -rf "${stage}"' EXIT
CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -trimpath -ldflags="-s -w" \
    -o "${stage}/dreamkarts-lobby" ./cmd/dreamkarts-lobby
cp deploy/dreamkarts-lobby.service deploy/lobby.env "${stage}/"

echo "== upload to ${HOST}"
remote_tmp="/tmp/dreamkarts-deploy-$(date +%s)"
ssh "${HOST}" "mkdir -p ${remote_tmp}"
scp -q "${stage}/dreamkarts-lobby" "${stage}/dreamkarts-lobby.service" "${stage}/lobby.env" "${HOST}:${remote_tmp}/"

echo "== install"
ssh "${HOST}" bash -s -- "${remote_tmp}" <<'REMOTE'
set -euo pipefail
tmp="$1"
id dreamkarts >/dev/null 2>&1 || useradd --system --home-dir /var/lib/dreamkarts --shell /usr/sbin/nologin dreamkarts
install -d -m 755 /opt/dreamkarts /opt/dreamkarts/bin /etc/dreamkarts
if [ -f /opt/dreamkarts/bin/dreamkarts-lobby ]; then
    cp /opt/dreamkarts/bin/dreamkarts-lobby /opt/dreamkarts/bin/dreamkarts-lobby.previous
fi
install -m 755 "${tmp}/dreamkarts-lobby" /opt/dreamkarts/bin/dreamkarts-lobby
[ -f /etc/dreamkarts/lobby.env ] || install -m 644 "${tmp}/lobby.env" /etc/dreamkarts/lobby.env
install -m 644 "${tmp}/dreamkarts-lobby.service" /etc/systemd/system/dreamkarts-lobby.service
systemctl daemon-reload
systemctl enable dreamkarts-lobby.service >/dev/null
systemctl restart dreamkarts-lobby.service
rm -rf "${tmp}"
sleep 2
if ! systemctl is-active --quiet dreamkarts-lobby.service; then
    journalctl -u dreamkarts-lobby.service -n 30 --no-pager
    exit 1
fi
journalctl -u dreamkarts-lobby.service -n 3 --no-pager
REMOTE

echo "== smoke test ${ADDR}"
want="$(sed -n 's/^#define DK_NET_BUILD *\([0-9]*\).*/\1/p' ../src/net_version.h)"
out="$(go run ./cmd/dk-bot -addr "${ADDR}" -smoke)"
echo "${out}"
if ! grep -q "latest build ${want}," <<<"${out}"; then
    echo "deployed server does not report build ${want}" >&2
    exit 1
fi
echo "deployed: build ${want}"
