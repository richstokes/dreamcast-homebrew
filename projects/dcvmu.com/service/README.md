# DCVMU service

[dcvmu.com](https://dcvmu.com) hosts a small server-rendered VMU archive. Flask,
SQLite and Gunicorn run behind Caddy on the dedicated shared game server.
No JavaScript is required. Dreamcast browsers get the same semantic HTML forms;
modern browsers additionally render the responsive stylesheet.

## Local development and verification

```sh
cd /Users/rich/Dropbox/code/dreamcast-dev/projects/dcvmu.com/service
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/python -m unittest -v
DCVMU_DATABASE=/tmp/dcvmu-dev.sqlite3 .venv/bin/gunicorn --bind 127.0.0.1:8090 wsgi:app
```

The application requires HTTPS, including locally. Use a trusted local HTTPS
reverse proxy for interactive development. A loopback diagnostic request may
supply `X-Forwarded-Proto: https`; never expose Gunicorn directly to the network.
Tests run in temporary databases and cover authentication, CSRF, session
separation, privacy, ownership, download integrity, duplicates, revisions,
validation and security headers.

## Product behavior

Registration asks for username, password and email. Usernames are case-insensitive,
3–24 ASCII letters/numbers/underscores. Passwords are 5–128 ASCII characters for
console keyboard compatibility. Emails remain private; this version does not
send verification or password-reset email. Protect your password accordingly.

Owners can delete saves from the website details page after confirming. Modern
browsers show a confirmation dialog; browsers without JavaScript use a confirmation
page. Deletion is permanent and rejects stale revisions so a replaced or edited
save must be reviewed again before deletion.

The Dreamcast client uploads a selected data file, original filename, archive
name, game title, optional notes and public/private choice. Game descriptions
are derived by the service from the validated VMS header (filename fallback),
using the client's printable ASCII display rules. Submitted game fields are ignored.
Only notes and visibility can be edited on the save's website page. Browse filters
match game/user names case-insensitively, with 20 saves per page. Private saves
appear only in their owner's account and return 404 to everyone else, including
on the download URL. VMS downloads preserve the raw file byte-for-byte; restoring
a save to a VMU requires a compatible import tool. The client does not restore
saves or write VMUs.

Browse cards, account cards and save details show the save's embedded 32×32
color icon at 64×64 with crisp pixel scaling. Animated icons use the first frame;
saves without icons keep a text-only heading. PNG previews are generated from
the stored VMS palette and pixels, including transparency and block-offset
headers, without changing the download. The `/saves/<id>/icon.png` endpoint
enforces save visibility and uses `no-store`, including for public icons, so
changing a save to private cannot leave a reusable cached preview. No JavaScript,
database migration or extra image library is required.

Duplicate identity is **account + archive name**. The server returns a conflict
without changing anything. The client offers replacement, a numbered second
copy, or returning to change the name. Replacement requires the conflict's
revision number; concurrent changes cause another conflict. Replacement adopts
the upload's notes and privacy setting. Replaced bytes are not retained as an
in-product version history. The maximum is 200 entries per account, each at most
120.5 KiB (241 complete 512-byte VMU blocks). Identical data under an intentionally
different archive name is allowed.

## API v1

Responses are UTF-8 plain text so the SH-4 client needs no JSON parser. All
endpoints require HTTPS. Passwords/tokens must never be put into URLs or logs.

| Request | Body / result |
| --- | --- |
| `POST /api/v1/login` | Form URL encoded `username`, `password`. HTTP 200 contains one bearer token line. |
| `GET /api/v1/me` | Bearer token validation; returns username plus newline or 401. |
| `POST /api/v1/logout` | Bearer authorization header; revokes that token. |
| `POST /api/v1/saves` | Bearer authorization, multipart fields below. HTTP 201: `OK`, numeric save ID, final archive name on separate lines. |

Upload fields: `save` binary file, `filename` (original VMU name, max 12), `name`
(max 64), `game` (legacy, ignored), `notes` (max 500), `private` (`0` or `1`), and `mode`
(`ask`, `replace`, or `keep`). `replace` also requires `revision`. A normal
conflict is HTTP 409 with `CONFLICT` and the revision on separate lines. Other
400/401/409/413/429 responses contain a readable error. The client deliberately
does not automatically replay mutations after a timeout; inspect your account
before retrying because a response can be lost after a successful write.

## Security and operations

- Argon2id hashes with individual salts (64 MiB, three iterations, two lanes);
  plaintext passwords are never stored. Passwords are hashed, not reversibly encrypted.
- Random 256-bit session tokens, stored only as SHA-256 hashes server-side.
  API and web sessions are separate and normally expire after 24 hours.
  API login with `remember=1` creates a 90-day VMU session. Browser cookies
  are Secure, HttpOnly and SameSite=Lax; every web mutation checks a session-bound
  CSRF value and same-origin header when present. Sessions rotate on login.
- Rate limits persist across workers for login, registration, session creation
  and uploads. Request bodies, field lengths and per-account storage are bounded.
- SQLite transactions serialize duplicate decisions and revision checks. Save
  blobs and metadata commit atomically. SQL parameters and autoescaped HTML are
  used throughout; uploads are served only as attachment downloads.
- The proxy supplies trusted forwarding headers, and both upstream services bind
  only loopback. The unprivileged `dcvmu` system user can write only its data area.
- Application/Caddy access logging of request bodies is disabled. Runtime data,
  local environments and temporary test credentials are excluded from git.

Deploy from a tested local virtualenv:

```sh
./deploy/deploy.sh
ssh root@dcvmu.com 'systemctl status dcvmu --no-pager'
curl --fail https://dcvmu.com/healthz
```

Ubuntu needs `python3-venv`. Source is installed under `/opt/dcvmu`, the virtualenv
under `/opt/dcvmu/.venv`, and the live database under
`/var/lib/dcvmu/archive.sqlite3`. `DCVMU_DATABASE`
is the optional database path setting. No production credential belongs
in this repository. Deployment uses an explicit source manifest, preserves the
database, and keeps the previous service source in `/opt/dcvmu-previous.tar.gz`.

Client releases are built by GitHub Actions from `.github/console-projects.txt` and
published as `dcvmu-client.cdi` and `dcvmu-client.elf` in the rolling GitHub
release. The website prefers CDI and links directly to GitHub. Legacy
`/downloads/dcvmu-client.elf` and `/downloads/dcvmu-client.cdi` URLs redirect to
those assets. No client binaries need uploading to the web server. Wait for a
successful release before deploying changes that introduce new asset links.

Caddy ingress and the preserved Valley Rangers service are documented in
`/Users/rich/Dropbox/code/voxel-game-xp/docs/SHARED_HOSTING.md`. Normal site
updates restart only `dcvmu`; normal game updates preserve the proxy drop-in.
For initial ingress rollback, follow that document rather than stopping unrelated
services. Inspect errors with `journalctl -u dcvmu -u caddy --since '10 minutes ago'`.

## Back up accounts and uploaded saves

Run from anywhere (requires Python 3 and your existing SSH access):

```sh
/Users/rich/Dropbox/code/dreamcast-dev/projects/dcvmu.com/service/deploy/backup.py
```

The default destination is `~/Dropbox/DCVMU_Backups`. Each run creates a new
`dcvmu-<UTC timestamp>/` directory containing `archive.sqlite3`, `manifest.json`
(checksum and record counts), and restore instructions. Older backups are kept.
The script uses SQLite's online backup API over SSH while the website stays
running, exports a standalone database without WAL sidecars, checks SQLite
integrity/foreign keys and every save's SHA-256, then publishes the local folder.
Failed downloads or verification do not become completed backups. Temporary
server files are cleaned up; new local directories/files use private permissions.

The database contains accounts, password hashes, session hashes, all metadata,
and public/private save bytes. Treat backups as private account data. Source
code, release ELFs, and server configuration are not included. Dropbox syncing
is handled by your installed Dropbox application; the script verifies the local
copy, not remote Dropbox sync completion. No scheduled task is installed.

Optional overrides:

```sh
./projects/dcvmu.com/service/deploy/backup.py --destination /path/to/backups --host root@dcvmu.com
```

To restore, stop only `dcvmu`, preserve the current database for rollback, restore
`archive.sqlite3` to `/var/lib/dcvmu/archive.sqlite3` with owner `dcvmu:dcvmu` and
mode `0600`, remove only stale WAL/SHM files for that stopped database, then start
and verify `/healthz`. Use service code compatible with the backup's schema.
Do not stop Caddy or the game server.

Login saves (`DCVMU_AUTH`, its VMS app ID, or its payload signature) are rejected
by the upload endpoint, even with a different submitted filename.

### Upload sanity checks

Only one `.vms` binary file per request is accepted. Its actual contents must
have a supported VMS data header, 0–3 icon frames, a known eyecatch layout,
a nonempty payload fitting the file, and a valid CRC-16/CCITT checksum. Final
block padding is allowed; appended whole blocks, truncated files, whole-card
images, archives, and arbitrary renamed blobs are rejected. Header offsets on
512-byte boundaries are supported. The VMU system icon file `ICONDATA_VMS` is
not a VMS game save and is not accepted. Existing stored saves are untouched.

The 192 KiB request cap, 60 upload attempts/hour/account, 200-save account cap,
authentication, and login-save exclusion also remain enforced. These checks
validate format and size, not the origin or authenticity of game progress.

The 241-block per-file limit includes
[KOS-supported expanded VMUs](https://kos-docs.dreamcast.wiki/group__vmu__settings.html).

## Dreamcast download API

`GET /api/v1/saves?scope=mine|public&page=0` requires bearer authentication.
It returns `MORE<TAB>0|1`, followed by up to seven tab-separated rows of
percent-encoded fields: id, revision, original filename, archive name, game,
username, byte size, SHA-256, and VMS header offset in blocks. My saves includes
private entries owned by the caller; public excludes every private entry.

New Dreamcast clients use
`GET /api/v1/saves?scope=public&user=<username>&game=<title-substring>&page=0`.
`user` is an exact, case-insensitive username (3-24 ASCII letters, digits or
underscores); an explicitly empty or invalid username returns 400. It is only
accepted with `scope=public`. Unknown and private-only accounts return an empty
list, and public mode excludes even the caller's own private saves. Optional
`game` is a literal, case-insensitive substring of at most 80 characters; `%` and
`_` are literal characters. URL-encode query values.

Filtering happens before pagination. Results sort by `updated DESC, id DESC`
and fetch only eight rows to determine whether the next seven-row page exists.
The indexed owner lookup limits console searches to one account's maximum 200
saves (29 pages); it does not scan every user's archive or count all results.
Offset pagination is bounded by that account limit; concurrent edits can shift
page boundaries, so the client's refresh action restarts at page zero. Revisit
cursor pagination if the account cap is substantially increased. The unfiltered
public endpoint remains available for older clients. Deploy these service changes
before releasing the new client; the new index is created at service startup.

`GET /api/v1/saves/<id>/download?revision=<revision>` requires bearer
authentication, enforces the same privacy rules as the website, and returns exact
binary bytes. A changed revision returns 409; clients must refresh before trying
again. Uploads may send `header_offset`; it must match the validated VMS header.
Older clients can omit it and the server detects it. Existing rows are migrated
with their validated offsets when the service starts. Back up before deployment.

Upload timestamps are server-generated UTC Unix seconds. A replacement updates
`uploaded_at`; editing notes or visibility does not. Modern browsers display
the viewer’s local timezone; no-JavaScript and console pages retain UTC. Legacy
entries with an uncertain replacement history show their known first upload.
