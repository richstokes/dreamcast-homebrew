#!/usr/bin/env bash
set -euo pipefail
project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
remote=${DCVMU_DEPLOY_HOST:-root@dcvmu.com}
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
cd "$project_dir"
.venv/bin/python -m unittest -v
# Explicit source manifest prevents uploading credentials, database or local caches.
COPYFILE_DISABLE=1 tar --no-xattrs -czf "$stage/service.tar.gz" app.py vmu_validation.py wsgi.py requirements.txt templates static deploy/dcvmu.service
scp "$stage/service.tar.gz" "$remote:/tmp/dcvmu-service.tar.gz"
ssh "$remote" bash -s <<'REMOTE'
set -euo pipefail
if ! id -u dcvmu >/dev/null 2>&1; then
  useradd --system --home-dir /var/lib/dcvmu --shell /usr/sbin/nologin dcvmu
fi
install -d -o dcvmu -g dcvmu -m 700 /var/lib/dcvmu
install -d -o root -g root -m 755 /opt/dcvmu
if [ -f /opt/dcvmu/app.py ]; then
  backup_files=(app.py wsgi.py requirements.txt templates static)
  if [ -f /opt/dcvmu/vmu_validation.py ]; then backup_files+=(vmu_validation.py); fi
  tar -czf /opt/dcvmu-previous.tar.gz -C /opt/dcvmu "${backup_files[@]}"
fi
tar -xzf /tmp/dcvmu-service.tar.gz -C /opt/dcvmu
if [ ! -x /opt/dcvmu/.venv/bin/pip ]; then python3 -m venv /opt/dcvmu/.venv; fi
/opt/dcvmu/.venv/bin/pip --disable-pip-version-check install -q -r /opt/dcvmu/requirements.txt
install -m 644 /opt/dcvmu/deploy/dcvmu.service /etc/systemd/system/dcvmu.service
systemctl daemon-reload
systemctl enable dcvmu.service
systemctl restart dcvmu.service
rm /tmp/dcvmu-service.tar.gz
for attempt in {1..15}; do
  if curl -fsS -H 'X-Forwarded-Proto: https' http://127.0.0.1:8090/healthz; then exit 0; fi
  sleep 1
done
journalctl -u dcvmu -n 40 --no-pager
exit 1
REMOTE
