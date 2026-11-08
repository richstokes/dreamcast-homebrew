#!/bin/bash
# Build, test and deploy the Ridgeline online server to the shared host, then
# check from here that it answers with the build in src/net_version.h.
#
#   projects/games/ridgeline/server/deploy/deploy.sh
#
# The host and its port registry live in richstokes/infra
# (servers/netsplit.vip/applications.json, app id "ridgeline"). A deploy
# restarts the service, which empties every lobby: consoles notice within a
# few seconds and go back to the lobby browser.
set -euo pipefail

HOST="${RIDGELINE_DEPLOY_HOST:-root@dcvmu.com}"
ADDR="${RIDGELINE_ADDR:-dcvmu.com:7536}"
SERVER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${SERVER_DIR}"

echo "== test"
go vet ./...
go test ./...

echo "== build"
stage="$(mktemp -d)"
trap 'rm -rf "${stage}"' EXIT
CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -trimpath -ldflags="-s -w" \
    -o "${stage}/ridgeline-lobby" ./cmd/ridgeline-lobby
cp deploy/ridgeline-lobby.service deploy/lobby.env "${stage}/"

echo "== upload to ${HOST}"
remote_tmp="/tmp/ridgeline-deploy-$(date +%s)"
ssh "${HOST}" "mkdir -p ${remote_tmp}"
scp -q "${stage}/ridgeline-lobby" "${stage}/ridgeline-lobby.service" "${stage}/lobby.env" "${HOST}:${remote_tmp}/"

echo "== install"
ssh "${HOST}" bash -s -- "${remote_tmp}" <<'REMOTE'
set -euo pipefail
tmp="$1"
id ridgeline >/dev/null 2>&1 || useradd --system --home-dir /var/lib/ridgeline --shell /usr/sbin/nologin ridgeline
install -d -m 755 /opt/ridgeline /opt/ridgeline/bin /etc/ridgeline
if [ -f /opt/ridgeline/bin/ridgeline-lobby ]; then
    cp /opt/ridgeline/bin/ridgeline-lobby /opt/ridgeline/bin/ridgeline-lobby.previous
fi
install -m 755 "${tmp}/ridgeline-lobby" /opt/ridgeline/bin/ridgeline-lobby
[ -f /etc/ridgeline/lobby.env ] || install -m 644 "${tmp}/lobby.env" /etc/ridgeline/lobby.env
install -m 644 "${tmp}/ridgeline-lobby.service" /etc/systemd/system/ridgeline-lobby.service
systemctl daemon-reload
systemctl enable ridgeline-lobby.service >/dev/null
systemctl restart ridgeline-lobby.service
rm -rf "${tmp}"
sleep 2
if ! systemctl is-active --quiet ridgeline-lobby.service; then
    journalctl -u ridgeline-lobby.service -n 30 --no-pager
    exit 1
fi
journalctl -u ridgeline-lobby.service -n 3 --no-pager
REMOTE

echo "== smoke test ${ADDR}"
want="$(sed -n 's/^#define RL_NET_BUILD *\([0-9]*\).*/\1/p' ../src/net_version.h)"
# The server downloads its bad word list before it answers: give it a moment.
for try in 1 2 3 4 5 6; do
    if out="$(go run ./cmd/rl-bot -addr "${ADDR}" -smoke 2>&1)"; then break; fi
    sleep 5
done
echo "${out}"
if ! grep -q "latest build ${want}," <<<"${out}"; then
    echo "deployed server does not report build ${want}" >&2
    exit 1
fi
echo "deployed: build ${want}"
