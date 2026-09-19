# DCVMU service

[dcvmu.com](https://dcvmu.com) hosts a small server-rendered VMU save archive.
Flask, SQLite and Gunicorn run behind Caddy. Browsing, account management and
card imports work without JavaScript. The drawing studio needs a modern browser.

## What it does

- Accounts register with a username, password and email. Usernames are 3–24
  ASCII letters, numbers or underscores; passwords are 5–128 ASCII characters
  for console keyboard compatibility. There is no password-reset email yet.
- The [Dreamcast client](../client/) uploads VMS data saves with a title, notes
  and a public/private choice. Owners can edit those on the website, and delete
  saves after confirming.
- Public saves can be browsed, filtered by game and user (case-insensitive
  substrings), sorted, and viewed as cards or a list, 20 per page. Private saves
  are visible only to their owner and return 404 to everyone else.
- Saves show their embedded 32×32 VMU icon, and downloads preserve the raw file
  byte-for-byte.
- Re-uploading a save with the same VMU filename offers to replace an existing
  backup or keep both. Replaced bytes are not retained.
- Dark mode and the Cards/List preference work without JavaScript.
- Browse pagination shows the current page, total pages and filtered save count.
- The VMU studio creates custom Dreamcast-menu and VMU-screen icons.
- The card importer previews `.bin`, `.vmu`, `.dcm` and `.dci` files, then imports
  selected saves or custom icons without replacing existing cloud backups.

The studio (`/studio`) draws or converts images to 32×32, 16-colour Dreamcast
menu icons and monochrome VMU icons, with live previews and undo. Each set takes
two blocks and can optionally enable the hidden 3D BIOS animation. Save it to
your account, then install from **Download a save** in the updated client.

The importer (`/import`) accepts one standard 128 KiB card bank or one Nexus
DCI file. File bytes and VMS header offsets are preserved; original directory
timestamps and copy-protection flags are not restored by the client. Mini-games,
unsupported filenames, damaged/cross-linked files and login saves are excluded
with reasons shown. It never restores a whole card or changes the source image.

Both tools default to private entries. Import previews belong to their owner
and expire after 30 minutes. Only validated file payloads are staged, never the
original card or excluded login data. A new preview replaces the previous one;
completion or **Discard preview** removes it. Expired previews are purged on
subsequent import requests. A selection imports in one transaction: reaching
the account limit leaves both the archive and preview unchanged.

Limits: 200 saves per account, each at most 120.5 KiB (241 VMU blocks), and 60
upload attempts per hour. Uploads must be a single valid VMS data save with a
correct CRC; whole-card images, archives, `ICONDATA_VMS`, and the client's
`DCVMU_AUTH` login save are rejected by the normal upload endpoint. Use the
separate card importer or studio for custom icons. Login saves remain excluded.

## Local development

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/python -m unittest -v
DCVMU_DATABASE=/tmp/dcvmu-dev.sqlite3 .venv/bin/gunicorn --bind 127.0.0.1:8090 wsgi:app
```

The application requires HTTPS, including locally, so use a local HTTPS reverse
proxy for interactive development. Never expose Gunicorn directly to the
network.

## API v1

Responses are UTF-8 plain text so the SH-4 client needs no JSON parser. All
endpoints require HTTPS, and everything except login uses a bearer token.

| Request | Body / result |
| --- | --- |
| `POST /api/v1/login` | Form fields `username`, `password`, optional `remember=1` for a 90-day session. Returns one bearer token line. |
| `GET /api/v1/me` | Returns the username, or 401. |
| `POST /api/v1/logout` | Revokes the token. |
| `POST /api/v1/saves` | Multipart upload (fields below). HTTP 201: `OK`, save ID, and final title on separate lines. |
| `POST /api/v1/saves/<id>/rename` | Form fields `name` and `revision`. Returns `OK`. |
| `GET /api/v1/saves` | Lists saves (parameters below). |
| `GET /api/v1/saves/<id>/download?revision=<revision>` | Returns the exact save bytes; 409 if the revision changed. |

Upload fields: `save` (binary file), `filename` (original VMU name, max 12),
`name` (title, max 64), `notes` (max 500), `private` (`0` or `1`),
`match=filename`, and `mode` (`ask`, `replace`, or `keep`). With `ask`, an
existing backup of the same filename returns `409 MATCHES`; `replace` then
requires the chosen `save_id` and `revision`, while `keep` creates another
entry. Requests without `match` use the legacy name-matching behavior.

List parameters: `scope=mine|public` and `page`. Public scope takes an exact
`user` and an optional `game` substring; `scope=mine` takes an optional exact
`filename`. The response is `MORE<TAB>0|1` followed by up to seven
tab-separated rows of percent-encoded fields: id, revision, original filename,
title, game, username, byte size, SHA-256, and VMS header offset in blocks.

Icon-aware clients send `include_icons=1` when listing or downloading saves.
`GET /api/v1/saves/<id>/icon-header?revision=<revision>` provides the custom
icon preview in the client's VMS header format. Older clients only see data saves.
Icon entries have filename `ICONDATA_VMS` and header offset zero. They use a
special icon format, not a VMS header or data CRC. The preview response is
640 bytes and applies normal access/revision checks; downloads always return
the original file bytes. Older clients cannot download icons without opting in.

Errors are 400/401/409/413/429 with a readable message. A response can be lost
after a successful write, so clients should not replay mutations automatically.

## Security

- Passwords are hashed with Argon2id; plaintext passwords are never stored.
- Session tokens are random 256-bit values stored only as SHA-256 hashes. Website logins last
  one year and renew on activity at most once per day. API sessions are separate
  and expire after 24 hours, or 90 days for a remembered VMU login.
- Cookies are Secure, HttpOnly and SameSite=Lax, and every web mutation checks a
  CSRF value.
- Login, registration and uploads are rate limited, and request sizes and
  per-account storage are bounded.
- Uploads are served only as attachment downloads, and request bodies are not
  logged.

## Deploy

Shared server configuration is owned by
[infra/servers/netsplit.vip](https://github.com/richstokes/infra/tree/main/servers/netsplit.vip).
That is the source of truth for Caddy, domains, backend port allocations and
host operations. This project owns the service build/deploy script, base
systemd unit, application settings and SQLite backup/restore.

```sh
./deploy/deploy.sh
ssh root@netsplit.vip 'systemctl status dcvmu --no-pager'
curl --fail https://dcvmu.com/healthz
```

The SSH default is `root@netsplit.vip` (the same host as `dcvmu.com`); override
it with `DCVMU_DEPLOY_HOST`. The public URL stays `https://dcvmu.com`.
From a sibling infra checkout, `python3 servers/netsplit.vip/manage.py deploy dcvmu`
dispatches this same script. Register new applications and ingress changes in
infra; app deployment must preserve its shared configuration.

Source is installed under `/opt/dcvmu` and the live database is
`/var/lib/dcvmu/archive.sqlite3`. Deployment preserves the database and keeps
the previous source in `/opt/dcvmu-previous.tar.gz`. Schema migrations run at
service startup, so back up first, and deploy the service before releasing a
client that depends on new API behavior.

The server is shared with Valley Rangers and other applications. Keep Gunicorn
on `127.0.0.1:8090` behind the infra-managed Caddy ingress and restart only
`dcvmu` for application changes. Inspect errors with
`journalctl -u dcvmu -u caddy --since '10 minutes ago'`.

Client binaries are not hosted here: GitHub Actions publishes
`dcvmu-client.cdi` and `dcvmu-client.elf` to the rolling GitHub release, and
the website links to them.

## Backups

```sh
./deploy/backup.py
```

Each run creates a verified `dcvmu-<UTC timestamp>/` directory under
`~/Dropbox/DCVMU_Backups` containing `archive.sqlite3`, a manifest, and restore
instructions, while the website stays running. Older backups are kept. Use
`--destination` and `--host` to override the defaults.

The database contains accounts, password hashes, and all public and private
saves, so treat backups as private data.

To restore, stop only `dcvmu`, keep the current database for rollback, restore
`archive.sqlite3` to `/var/lib/dcvmu/archive.sqlite3` with owner `dcvmu:dcvmu`
and mode `0600`, remove stale WAL/SHM files, then start the service and check
`/healthz`.
