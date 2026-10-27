#!/usr/bin/env bash
set -euo pipefail
project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# Shared ingress/port registration: richstokes/infra, servers/netsplit.vip.
# This script owns only the DCVMU application and its base systemd unit.
remote=${DCVMU_DEPLOY_HOST:-root@dcvmu.com}
command -v uv >/dev/null || { echo 'Install uv before deploying DCVMU.' >&2; exit 1; }
# Fail before uploading/replacing app files if the shared host needs uv installed.
ssh "$remote" 'command -v uv >/dev/null' || {
  echo 'uv must be in the remote SSH PATH; see infra/servers/netsplit.vip.' >&2
  exit 1
}
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
cd "$project_dir"
if [ ! -x .venv/bin/python ]; then uv venv --python python3 .venv; fi
uv pip install --python .venv/bin/python -r requirements.txt
.venv/bin/python -m unittest -v
# Explicit source manifest prevents uploading credentials, database or local caches.
COPYFILE_DISABLE=1 tar --no-xattrs -czf "$stage/service.tar.gz" app.py play.py netrelay.py vmu_validation.py vmu_tools.py wsgi.py requirements.txt templates static deploy/dcvmu.service deploy/dcvmu-net.service
scp "$stage/service.tar.gz" "$remote:/tmp/dcvmu-service.tar.gz"
ssh "$remote" bash -s <<'REMOTE'
set -euo pipefail
command -v uv >/dev/null
if ! id -u dcvmu >/dev/null 2>&1; then
  useradd --system --home-dir /var/lib/dcvmu --shell /usr/sbin/nologin dcvmu
fi
install -d -o dcvmu -g dcvmu -m 700 /var/lib/dcvmu
install -d -o root -g root -m 755 /opt/dcvmu
if [ -f /opt/dcvmu/app.py ]; then
  backup_files=(app.py wsgi.py requirements.txt templates static)
  if [ -f /opt/dcvmu/vmu_validation.py ]; then backup_files+=(vmu_validation.py); fi
  if [ -f /opt/dcvmu/vmu_tools.py ]; then backup_files+=(vmu_tools.py); fi
  if [ -f /opt/dcvmu/play.py ]; then backup_files+=(play.py); fi
  if [ -f /opt/dcvmu/netrelay.py ]; then backup_files+=(netrelay.py); fi
  tar -czf /opt/dcvmu-previous.tar.gz -C /opt/dcvmu "${backup_files[@]}"
fi
tar --no-same-owner -xzf /tmp/dcvmu-service.tar.gz -C /opt/dcvmu
# Local image tools may write mode 0600. Public assets/templates must remain
# readable by the unprivileged service, regardless of archive permissions.
find /opt/dcvmu/static /opt/dcvmu/templates -type d -exec chmod 755 {} +
find /opt/dcvmu/static /opt/dcvmu/templates -type f -exec chmod 644 {} +
if [ ! -x /opt/dcvmu/.venv/bin/python ]; then uv venv --python /usr/bin/python3 /opt/dcvmu/.venv; fi
uv pip install --python /opt/dcvmu/.venv/bin/python -q -r /opt/dcvmu/requirements.txt
install -m 644 /opt/dcvmu/deploy/dcvmu.service /etc/systemd/system/dcvmu.service
install -m 644 /opt/dcvmu/deploy/dcvmu-net.service /etc/systemd/system/dcvmu-net.service
systemctl daemon-reload
systemctl enable dcvmu.service dcvmu-net.service
systemctl restart dcvmu.service dcvmu-net.service
rm /tmp/dcvmu-service.tar.gz
for attempt in {1..15}; do
  if curl -fsS -H 'X-Forwarded-Proto: https' http://127.0.0.1:8090/healthz; then
    curl -fsS -H 'X-Forwarded-Proto: https' http://127.0.0.1:8090/static/dcvmu-client.png -o /dev/null
    # Caddy sends web player files from the cache (infra's dcvmu.caddy).
    curl -fsS --resolve dcvmu.com:443:127.0.0.1 https://dcvmu.com/play/flycast-wasm.data \
      | cmp - /opt/dcvmu/static/play/flycast-wasm.data \
      || { echo 'Caddy did not send the player core; apply infra servers/netsplit.vip.' >&2; exit 1; }
    # The network relay answers its WebSocket handshake through Caddy. This
    # opens a DCNet session for a moment.
    for relay_attempt in {1..5}; do
      handshake=$(curl -sS --http1.1 --max-time 3 --resolve dcvmu.com:443:127.0.0.1 -o /dev/null -w '%{http_code}' \
        -H 'Connection: Upgrade' -H 'Upgrade: websocket' -H 'Sec-WebSocket-Version: 13' \
        -H 'Sec-WebSocket-Key: ZGN2bXUtbmV0LWNoZWNrIQ==' -H 'Origin: https://dcvmu.com' \
        https://dcvmu.com/play/net 2>/dev/null || true)
      [ "$handshake" = 101 ] && break
      sleep 1
    done
    if [ "$handshake" != 101 ]; then
      echo "Network relay handshake returned $handshake; apply infra servers/netsplit.vip." >&2
      journalctl -u dcvmu-net -n 20 --no-pager
      exit 1
    fi
    exit 0
  fi
  sleep 1
done
journalctl -u dcvmu -u dcvmu-net -n 40 --no-pager
exit 1
REMOTE
