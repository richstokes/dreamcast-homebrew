# DCVMU service

[dcvmu.com](https://dcvmu.com) is a small server-rendered archive for
Dreamcast VMU saves: Flask, SQLite and Gunicorn behind Caddy. Browsing,
accounts, card imports and archives work without JavaScript; the icon studio
needs a modern browser. End-user instructions live in the
[getting started guide](https://dcvmu.com/getting-started).

## What it does

- Accounts (username, password, email) and a plain-text API v1 used by the [Dreamcast client](../client/)
- Save uploads with title, notes and public/private visibility; public saves are browsable and filterable by game and user, 20 per page, with their embedded VMU icons. Re-uploading the same VMU filename offers replace or keep both
- Downloads return the original bytes; private saves 404 for everyone but their owner
- VMU studio (`/studio`) draws 32x32 Dreamcast-menu and VMU-screen icon sets, optionally with the hidden 3D BIOS animation
- Card importer (`/import`) previews `.bin`, `.vmu`, `.dcm` and `.dci` images, then imports the selected saves or icons without replacing existing backups
- Whole-VMU archives (`/archives`) snapshot one 128 KiB card with DCVMU login saves scrubbed out, for one-step restore from the client
- Web player (`/play?rom=continuous/dreamkarts.cdi&name=DreamKarts`) runs this repository's release ELFs and disc images in the browser with Flycast WASM and EmulatorJS. Unlisted like `/stats`; the root README's Play links point here

Limits per account: 200 saves, 20 archives, 60 uploads an hour. Saves are at
most 120.5 KiB (241 blocks) and archives exactly 128 KiB. `DCVMU_AUTH` login
saves are never stored, and `ICONDATA_VMS` only arrives through the studio or
importer.

## Local development

Install [uv](https://docs.astral.sh/uv/getting-started/installation/).
Dependency versions live in `requirements.txt`.

```sh
uv venv --python python3 .venv
uv pip install --python .venv/bin/python -r requirements.txt
.venv/bin/python -m unittest -v
DCVMU_DATABASE=/tmp/dcvmu-dev.sqlite3 .venv/bin/gunicorn --bind 127.0.0.1:8090 wsgi:app
```

The app refuses plain HTTP, locally too, so put a local HTTPS reverse proxy
in front for interactive work. Never expose Gunicorn directly. The web
player's ROM and core routes answer with an empty body and an `X-Dcvmu-File`
header naming a file under `DCVMU_CACHE` (default `instance/cache`); try them
behind a Caddy carrying the `handle_response` block from infra's
`caddy/apps/dcvmu.caddy`.

## Tests

`.venv/bin/python -m unittest -v` runs everything; CI runs it on every push
to `main`, followed by the client's host tests.

| Module | Covers |
| --- | --- |
| `test_service.py` | Web pages, API v1, sessions and CSRF, rate limits, icons, duplicate handling, catalog labels |
| `test_vmu_tools.py` | Studio icon data, card and Nexus import extraction, preview expiry, atomic imports |
| `test_archives.py` | Whole-card login scrubbing, archive pages and the console archive API |
| `test_stats.py` | Unlisted stats page, sortable user table, public-only profiles |
| `test_play.py` | Web player page and headers, release asset parsing, the ROM cache and its GitHub revalidation |

## Security

- Passwords are hashed with Argon2id; session tokens are random 256-bit values stored only as SHA-256 hashes
- Web logins last a year and renew on activity at most once a day. API tokens last 24 hours, or 90 days for a remembered VMU login
- Cookies are Secure, HttpOnly and SameSite=Lax; every web mutation checks the Origin and a per-session CSRF value, while API calls use a bearer token
- Login, registration, uploads and imports are rate limited; request sizes and per-account storage are bounded
- Saves are served only as attachment downloads, and request bodies are not logged

## Deploy

Shared host configuration (Caddy, domains, port allocations, host tooling)
lives in [infra/servers/netsplit.vip](https://github.com/richstokes/infra/tree/main/servers/netsplit.vip);
register new applications and ingress changes there. This repository owns
the deploy script, the base systemd unit, application settings and database
backup/restore.

1. Back up first; schema migrations run at service start: `./deploy/backup.py`
2. Deploy (runs the tests, uploads an explicit source manifest, installs requirements with `uv` into `/opt/dcvmu/.venv`, restarts the unit): `./deploy/deploy.sh`
3. Check: `ssh root@dcvmu.com 'systemctl status dcvmu --no-pager'` and `curl --fail https://dcvmu.com/healthz`
4. On trouble: `journalctl -u dcvmu -u caddy --since '10 minutes ago'`

| | |
| --- | --- |
| SSH target | `root@dcvmu.com` (infra knows the host as `netsplit.vip`); override with `DCVMU_DEPLOY_HOST` |
| From an infra checkout | `python3 servers/netsplit.vip/manage.py deploy dcvmu` runs the same script |
| Prerequisite | `uv` in `PATH` locally and in the remote root SSH session; host tooling is infra's runbook |
| Source | `/opt/dcvmu`; the previous release is kept in `/opt/dcvmu-previous.tar.gz` |
| Database | `/var/lib/dcvmu/archive.sqlite3`, preserved across deploys |
| Player cache | `/var/cache/dcvmu`: release assets fetched from GitHub (rechecked every 5 minutes) and the Flycast core. Caddy sends these files itself; safe to delete |
| Gunicorn | `127.0.0.1:8090` behind the infra-managed Caddy. The host is shared with Valley Rangers and others: restart only `dcvmu` |

Deploy the service before releasing a client that depends on new API
behavior. Client binaries are not hosted here: GitHub Actions publishes
`dcvmu-client.cdi` and `dcvmu-client.elf` to the rolling release and the
website links to them.

## Backups and restore

`./deploy/backup.py` takes an online SQLite snapshot over SSH while the site
keeps running, verifies it (integrity check, foreign keys, every save's
SHA-256) and writes `dcvmu-<UTC timestamp>/` with `archive.sqlite3`, a
manifest and restore notes under `~/Dropbox/DCVMU_Backups`. Older backups are
kept; `--destination` and `--host` override the defaults. Backups hold
accounts, password hashes and private saves, so treat them as private.

Restore:

1. `systemctl stop dcvmu` (only dcvmu; leave Caddy and the other apps running)
2. Move the current `/var/lib/dcvmu/archive.sqlite3` aside for rollback
3. Copy the backup's `archive.sqlite3` into place, owner `dcvmu:dcvmu`, mode `0600`
4. Remove stale `archive.sqlite3-wal` and `archive.sqlite3-shm` files
5. `systemctl start dcvmu`, then `curl --fail https://dcvmu.com/healthz`

## Gotchas

- A deploy can migrate the database at startup, so take a backup before every deploy
- `/stats` and `/users/<username>` are unlisted (`noindex, nofollow`), not access-controlled. Keep them out of site navigation and never show private saves or archives there
- Profiles show public saves only, even to the signed-in owner
- Import previews are per account and expire after 30 minutes; only validated payloads are staged, never the source card or login data
- The web player depends on infra's Caddy rule for `X-Dcvmu-File`; without it ROM downloads come back empty. `deploy.sh` fails if the core does not come through Caddy
- `static/play/flycast-wasm.data` and `flycast.json` are generated: build the core with [tools/flycast-wasm](../../../tools/flycast-wasm/), then run `./package_flycast_core.py`

## Known limitations

- No password-reset email
- Replacing a save discards the previous bytes
- Imports keep bytes and header offsets, not directory timestamps or copy-protection flags
- The studio needs a modern browser
- The web player needs WebGL 2, loads EmulatorJS from its CDN and only plays assets from this repository's releases
