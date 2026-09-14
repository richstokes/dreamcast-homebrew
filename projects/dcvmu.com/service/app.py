"""DCVMU: server-rendered archive and bounded, HTTPS-only VMU upload API."""
import hashlib
from datetime import datetime, timezone
import io
import os
import re
import secrets
import sqlite3
import time
from pathlib import Path
from urllib.parse import quote

from argon2 import PasswordHasher
from argon2.exceptions import VerificationError, InvalidHashError
from flask import Flask, abort, g, redirect, render_template, request, send_file, url_for
from werkzeug.middleware.proxy_fix import ProxyFix
from vmu_validation import MAX_SAVE, validate_vms, header_metadata

PASSWORDS = PasswordHasher(time_cost=3, memory_cost=65536, parallelism=2)
CLIENT_RELEASE_BASE = 'https://github.com/richstokes/dreamcast-homebrew/releases/latest/download'
DUMMY_HASH = PASSWORDS.hash(secrets.token_urlsafe(32))
SCHEMA = '''
CREATE TABLE IF NOT EXISTS users (
 id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE COLLATE NOCASE,
 email TEXT NOT NULL, password_hash TEXT NOT NULL, created INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS sessions (
 hash TEXT PRIMARY KEY, user_id INTEGER REFERENCES users(id) ON DELETE CASCADE,
 csrf TEXT NOT NULL, expires INTEGER NOT NULL, kind TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS saves (
 id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),
 name TEXT NOT NULL, filename TEXT NOT NULL, game TEXT NOT NULL, notes TEXT NOT NULL,
 private INTEGER NOT NULL, data BLOB NOT NULL, sha256 TEXT NOT NULL,
 revision INTEGER NOT NULL DEFAULT 1, created INTEGER NOT NULL, updated INTEGER NOT NULL,
 UNIQUE(user_id, name));
CREATE INDEX IF NOT EXISTS saves_browse ON saves(private, updated DESC);
CREATE TABLE IF NOT EXISTS limits (key TEXT PRIMARY KEY, count INTEGER NOT NULL, expires INTEGER NOT NULL);
'''


def create_app(config=None):
    app = Flask(__name__)
    app.config.update(DATABASE=os.environ.get('DCVMU_DATABASE', 'instance/dcvmu.sqlite3'),
                      MAX_CONTENT_LENGTH=192 * 1024, MAX_FORM_MEMORY_SIZE=192 * 1024,
                      MAX_FORM_PARTS=20, PUBLIC_ORIGIN='https://dcvmu.com')
    if config:
        app.config.update(config)
    # Only the loopback Caddy upstream may reach gunicorn.
    app.wsgi_app = ProxyFix(app.wsgi_app, x_for=1, x_proto=1, x_host=0)
    Path(app.config['DATABASE']).parent.mkdir(parents=True, exist_ok=True)
    with sqlite3.connect(app.config['DATABASE']) as db:
        db.executescript(SCHEMA)
        db.execute('BEGIN IMMEDIATE')
        if 'header_offset' not in {r[1] for r in db.execute('PRAGMA table_info(saves)')}:
            db.execute('ALTER TABLE saves ADD COLUMN header_offset INTEGER NOT NULL DEFAULT 0')
            for sid, data in db.execute('SELECT id,data FROM saves').fetchall():
                try:
                    offset = validate_vms(data)
                except ValueError:
                    continue
                db.execute('UPDATE saves SET header_offset=? WHERE id=?', (offset, sid))
        if 'uploaded_at' not in {r[1] for r in db.execute('PRAGMA table_info(saves)')}:
            db.execute('ALTER TABLE saves ADD COLUMN uploaded_at INTEGER')
            # Later revisions may be metadata edits: their last upload is unknown.
            db.execute('UPDATE saves SET uploaded_at=created WHERE revision=1')
        db.commit()
        db.execute('PRAGMA journal_mode=WAL')

    @app.template_filter('utc_iso')
    def utc_iso(value):
        return datetime.fromtimestamp(value, timezone.utc).isoformat().replace('+00:00', 'Z')

    @app.template_filter('utc_display')
    def utc_display(value):
        return datetime.fromtimestamp(value, timezone.utc).strftime('%Y-%m-%d %H:%M:%S UTC')

    def database():
        if 'db' not in g:
            g.db = sqlite3.connect(app.config['DATABASE'], timeout=10)
            g.db.row_factory = sqlite3.Row
            g.db.execute('PRAGMA foreign_keys=ON')
        return g.db

    @app.teardown_appcontext
    def close_db(error):
        if 'db' in g:
            g.db.close()

    def digest(token):
        return hashlib.sha256(token.encode()).hexdigest()

    def limit(key, maximum, seconds):
        db = database()
        now = int(time.time())
        with db:
            db.execute('DELETE FROM limits WHERE expires < ?', (now,))
            db.execute('INSERT INTO limits VALUES (?,1,?) ON CONFLICT(key) DO UPDATE SET count=count+1',
                       (key, now + seconds))
            count = db.execute('SELECT count FROM limits WHERE key=?', (key,)).fetchone()[0]
        if count > maximum:
            abort(429, 'Too many attempts. Please try again later.')

    def new_session(user_id=None, kind='web', remembered=False):
        token = secrets.token_urlsafe(32)
        csrf = secrets.token_urlsafe(32)
        with database() as db:
            db.execute('DELETE FROM sessions WHERE expires < ?', (int(time.time()),))
            db.execute('INSERT INTO sessions VALUES (?,?,?,?,?)',
                       (digest(token), user_id, csrf, int(time.time()) + (90 * 86400 if remembered else 86400 if user_id else 3600), kind))
        return token, csrf

    def web_response(response, token=None):
        if token:
            response.set_cookie('dcvmu_session', token, max_age=86400, secure=True,
                                httponly=True, samesite='Lax', path='/')
        return response

    @app.before_request
    def identify():
        if not request.is_secure and not app.testing:
            abort(400, 'HTTPS is required.')
        token = request.cookies.get('dcvmu_session', '')
        kind = 'web'
        if request.path.startswith('/api/'):
            kind = 'api'
            token = request.headers.get('Authorization', '').removeprefix('Bearer ')
        g.session = database().execute(
            'SELECT * FROM sessions WHERE hash=? AND kind=? AND expires>?',
            (digest(token), kind, int(time.time()))).fetchone() if token else None
        g.user = database().execute('SELECT id,username FROM users WHERE id=?',
                                  (g.session['user_id'],)).fetchone() if g.session else None
        if request.method == 'POST':
            limit('post:' + (request.remote_addr or ''), 180, 600)
            if not request.path.startswith('/api/'):
                if request.headers.get('Origin') not in (None, app.config['PUBLIC_ORIGIN']):
                    abort(403, 'Invalid request origin.')
                csrf = request.form.get('csrf', '')
                if not g.session or not re.fullmatch(r'[A-Za-z0-9_-]{43}', csrf) or not secrets.compare_digest(csrf, g.session['csrf']):
                    abort(403, 'This form expired. Reload the page and try again.')

    @app.after_request
    def headers(response):
        response.headers['X-Content-Type-Options'] = 'nosniff'
        response.headers['Referrer-Policy'] = 'same-origin'
        response.headers['Content-Security-Policy'] = "default-src 'self'; style-src 'self'; script-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'; form-action 'self'"
        if request.path == '/support':
            response.headers['Content-Security-Policy'] += "; img-src 'self' https://cdn.buymeacoffee.com"
        response.headers['Cache-Control'] = 'no-store'
        return response

    def page(template, **context):
        token = None
        if not g.session:
            limit('guest:' + (request.remote_addr or ''), 120, 600)
            token, csrf = new_session()
        else:
            csrf = g.session['csrf']
        response = app.make_response(render_template(template, user=g.user, csrf=csrf, client_release_base=CLIENT_RELEASE_BASE, console_browser=any(name in request.user_agent.string.lower() for name in ('dreamcast', 'dreamkey', 'dreampassport')), **context))
        return web_response(response, token)

    @app.errorhandler(400)
    @app.errorhandler(401)
    @app.errorhandler(403)
    @app.errorhandler(404)
    @app.errorhandler(409)
    @app.errorhandler(413)
    @app.errorhandler(429)
    def error(e):
        if request.path.startswith('/api/'):
            return str(e.description) + '\n', e.code, {'Content-Type': 'text/plain; charset=utf-8'}
        return render_template('error.html', message=e.description, user=getattr(g, 'user', None)), e.code

    def require_user():
        if not g.user:
            abort(401, 'Please log in to continue.')

    def text_field(name, maximum, required=False):
        value = request.form.get(name, '').strip()
        if len(value) > maximum or any(ord(c) < 32 and c not in '\n\t' for c in value):
            abort(400, f'Invalid {name}. Maximum {maximum} characters.')
        if required and not value:
            abort(400, f'{name.capitalize()} is required.')
        return value

    @app.get('/healthz')
    def health():
        database().execute('SELECT 1')
        return 'ok\n', 200, {'Content-Type': 'text/plain'}

    @app.route('/register', methods=['GET', 'POST'])
    def register():
        if request.method == 'GET':
            return page('auth.html', register=True)
        limit('register:' + (request.remote_addr or ''), 5, 3600)
        username = text_field('username', 24, True)
        email = text_field('email', 254, True)
        password = request.form.get('password', '')
        if not re.fullmatch(r'[A-Za-z0-9_]{3,24}', username):
            abort(400, 'Username must be 3-24 letters, numbers or underscores.')
        if not re.fullmatch(r'[^\s@]+@[^\s@]+\.[^\s@]+', email):
            abort(400, 'Enter a valid email address.')
        if not 5 <= len(password) <= 128 or not password.isascii():
            abort(400, 'Use a password of 5-128 ASCII characters for Dreamcast compatibility.')
        hashed = PASSWORDS.hash(password)
        try:
            with database() as db:
                uid = db.execute('INSERT INTO users(username,email,password_hash,created) VALUES (?,?,?,?)',
                                 (username, email, hashed, int(time.time()))).lastrowid
        except sqlite3.IntegrityError:
            abort(409, 'That username is already taken.')
        return logged_in(uid)

    def logged_in(uid):
        with database() as db:
            if g.session:
                db.execute('DELETE FROM sessions WHERE hash=?', (g.session['hash'],))
        token, _ = new_session(uid)
        return web_response(redirect('/account', 303), token)

    def credentials():
        username = text_field('username', 24, True)
        password = request.form.get('password', '')
        limit('login-ip:' + (request.remote_addr or ''), 30, 900)
        limit('login-user:' + username.lower(), 15, 900)
        if len(password) > 128:
            abort(401, 'Incorrect username or password.')
        row = database().execute('SELECT * FROM users WHERE username=?', (username,)).fetchone()
        try:
            PASSWORDS.verify(row['password_hash'] if row else DUMMY_HASH, password)
        except (VerificationError, InvalidHashError):
            abort(401, 'Incorrect username or password.')
        if row is None:
            abort(401, 'Incorrect username or password.')
        return row['id']

    @app.route('/login', methods=['GET', 'POST'])
    def login():
        if request.method == 'GET':
            return page('auth.html', register=False)
        return logged_in(credentials())

    @app.post('/logout')
    def logout():
        with database() as db:
            db.execute('DELETE FROM sessions WHERE hash=?', (g.session['hash'],))
        response = redirect('/', 303)
        response.delete_cookie('dcvmu_session', secure=True, httponly=True, samesite='Lax')
        return response

    @app.post('/api/v1/login')
    def api_login():
        token, _ = new_session(credentials(), 'api', request.form.get('remember') == '1')
        return token + '\n', 200, {'Content-Type': 'text/plain'}

    @app.get('/api/v1/me')
    def api_me():
        require_user()
        return g.user['username'] + '\n', 200, {'Content-Type': 'text/plain'}

    @app.post('/api/v1/logout')
    def api_logout():
        require_user()
        with database() as db:
            db.execute('DELETE FROM sessions WHERE hash=?', (g.session['hash'],))
        return 'OK\n', 200, {'Content-Type': 'text/plain'}

    @app.get('/')
    def browse():
        game = request.args.get('game', '')[:80]
        username = request.args.get('user', '')[:24]
        try:
            offset = max(0, min(int(request.args.get('page', 1)), 100000) - 1) * 20
        except ValueError:
            abort(400, 'Invalid page.')
        rows = database().execute('''SELECT s.id,s.name,s.game,s.notes,s.filename,s.updated,s.created,s.uploaded_at,
            length(s.data) AS size,substr(s.data,s.header_offset*512+1,128) AS vms_header,u.username FROM saves s JOIN users u ON u.id=s.user_id
            WHERE private=0 AND (?='' OR s.game=? COLLATE NOCASE)
            AND (?='' OR u.username=? COLLATE NOCASE) ORDER BY updated DESC,s.id DESC LIMIT 21 OFFSET ?''',
            (game, game, username, username, offset)).fetchall()
        return page('browse.html', saves=[dict(row, metadata=header_metadata(row['vms_header'])) for row in rows[:20]], more=len(rows)>20,
                    page_num=offset//20+1, game=game, username=username)

    @app.get('/getting-started')
    def getting_started():
        return page('getting_started.html')

    @app.get('/support')
    def support():
        return page('support.html')

    @app.get('/downloads/dcvmu-client.elf')
    def client_download():
        return redirect(CLIENT_RELEASE_BASE + '/dcvmu-client.elf', 302)

    @app.get('/downloads/dcvmu-client.cdi')
    def client_disc_download():
        return redirect(CLIENT_RELEASE_BASE + '/dcvmu-client.cdi', 302)

    @app.get('/account')
    def account():
        require_user()
        try:
            page_num = max(1, min(int(request.args.get('page', 1)), 10))
        except ValueError:
            abort(400, 'Invalid page.')
        rows = database().execute('SELECT id,name,game,private,revision,created,uploaded_at FROM saves WHERE user_id=? ORDER BY updated DESC,id DESC LIMIT 21 OFFSET ?',
                                  (g.user['id'], (page_num-1)*20)).fetchall()
        return page('account.html', saves=rows[:20], more=len(rows)>20, page_num=page_num)

    def visible_save(sid):
        row = database().execute('SELECT s.*,u.username FROM saves s JOIN users u ON u.id=s.user_id WHERE s.id=?',
                                 (sid,)).fetchone()
        if row is None or (row['private'] and (not g.user or g.user['id'] != row['user_id'])):
            abort(404)
        return row

    @app.get('/saves/<int:sid>')
    def detail(sid):
        row = visible_save(sid)
        offset = row['header_offset'] * 512
        return page('save.html', save=dict(row, metadata=header_metadata(row['data'][offset:offset+128])))

    @app.get('/saves/<int:sid>/download')
    def download(sid):
        row = visible_save(sid)
        return send_file(io.BytesIO(row['data']), as_attachment=True,
                         download_name=row['filename'] + '.vms', mimetype='application/octet-stream')

    @app.post('/saves/<int:sid>/edit')
    def edit(sid):
        require_user()
        row = visible_save(sid)
        if row['user_id'] != g.user['id']:
            abort(404)
        game = text_field('game', 80, True)
        notes = text_field('notes', 500)
        if request.form.get('private', '0') not in ('0', '1'):
            abort(400, 'Invalid visibility.')
        private = 1 if request.form.get('private') == '1' else 0
        revision = request.form.get('revision', '')
        with database() as db:
            changed = db.execute('UPDATE saves SET game=?,notes=?,private=?,revision=revision+1,updated=? WHERE id=? AND revision=? AND user_id=?',
                                 (game, notes, private, int(time.time()), sid, revision, g.user['id'])).rowcount
        if not changed:
            abort(409, 'This save changed. Reload before editing.')
        return redirect(url_for('detail', sid=sid), 303)

    @app.get('/api/v1/saves')
    def api_saves():
        require_user()
        scope = request.args.get('scope', 'mine')
        if scope not in ('mine', 'public'):
            abort(400, 'Invalid scope.')
        try:
            page_num = int(request.args.get('page', '0'))
            if not 0 <= page_num <= 100000:
                raise ValueError()
        except ValueError:
            abort(400, 'Invalid page.')
        where, params = ('s.user_id=?', [g.user['id']]) if scope == 'mine' else ('s.private=0', [])
        rows = database().execute(
            'SELECT s.id,s.revision,s.filename,s.name,s.game,u.username,length(s.data) AS size,s.sha256,s.header_offset '
            'FROM saves s JOIN users u ON u.id=s.user_id WHERE ' + where +
            ' ORDER BY s.updated DESC,s.id DESC LIMIT 8 OFFSET ?', params + [page_num * 7]).fetchall()
        lines = ['MORE\t' + ('1' if len(rows) > 7 else '0')]
        for row in rows[:7]:
            lines.append('\t'.join(quote(str(row[key]), safe='') for key in
                ('id','revision','filename','name','game','username','size','sha256','header_offset')))
        return '\n'.join(lines) + '\n', 200, {'Content-Type': 'text/plain; charset=utf-8'}

    @app.get('/api/v1/saves/<int:sid>/download')
    def api_download(sid):
        require_user()
        row = visible_save(sid)
        if request.args.get('revision') != str(row['revision']):
            abort(409, 'Save changed. Refresh the list before downloading.')
        return send_file(io.BytesIO(row['data']), mimetype='application/octet-stream')

    @app.post('/api/v1/saves')
    def upload():
        require_user()
        limit('upload:' + str(g.user['id']), 60, 3600)
        name = text_field('name', 64, True)
        filename = text_field('filename', 12, True)
        game = text_field('game', 80, True)
        notes = text_field('notes', 500)
        if not re.fullmatch(r'[A-Za-z0-9_.! -]{1,12}', filename) or filename in ('.', '..'):
            abort(400, 'Invalid VMU filename.')
        if request.form.get('private', '0') not in ('0', '1'):
            abort(400, 'Invalid visibility.')
        private = int(request.form.get('private', '0'))
        mode = request.form.get('mode', 'ask')
        if mode not in ('ask', 'replace', 'keep'):
            abort(400, 'Invalid duplicate action.')
        files = request.files.getlist('save')
        if len(files) != 1 or set(request.files) != {'save'}:
            abort(400, 'Upload exactly one VMS save file.')
        file = files[0]
        if not (file.filename or '').lower().endswith('.vms'):
            abort(400, 'Upload a .vms save file, not an archive or whole VMU image.')
        if file.mimetype not in ('application/octet-stream', 'application/x-dreamcast-vms',
                                  'application/x-sega-dreamcast-vms'):
            abort(400, 'Unsupported upload content type. Send a binary VMS save.')
        data = file.read(MAX_SAVE + 1)
        try:
            detected_header_offset = validate_vms(data)
        except ValueError as error:
            abort(400, str(error))
        if filename.upper() == 'DCVMU_AUTH' or data[48:64].rstrip(b'\0') == b'DCVMU_AUTH' or b'DCVMU-AUTH-V1' in data:
            abort(400, 'DCVMU login saves cannot be uploaded.')
        try:
            header_offset = int(request.form.get('header_offset', str(detected_header_offset)))
            if header_offset != detected_header_offset:
                raise ValueError()
        except ValueError:
            abort(400, 'Invalid VMU header offset.')
        sha = hashlib.sha256(data).hexdigest()
        now = int(time.time())
        db = database()
        try:
            db.execute('BEGIN IMMEDIATE')
            existing = db.execute('SELECT id,revision FROM saves WHERE user_id=? AND name=?',
                                  (g.user['id'], name)).fetchone()
            if existing and mode == 'ask':
                db.rollback()
                return f"CONFLICT\n{existing['revision']}\n", 409, {'Content-Type': 'text/plain'}
            if mode == 'replace':
                if not existing or str(existing['revision']) != request.form.get('revision'):
                    db.rollback()
                    return 'Save changed. Upload again to review the current version.\n', 409
                sid = existing['id']
                db.execute('''UPDATE saves SET filename=?,game=?,notes=?,private=?,data=?,sha256=?,
                    revision=revision+1,updated=? WHERE id=? AND user_id=?''', (filename, game, notes, private, data, sha, now, sid, g.user['id']))
            else:
                if db.execute('SELECT count(*) FROM saves WHERE user_id=?', (g.user['id'],)).fetchone()[0] >= 200:
                    abort(400, 'Account limit reached (200 saves).')
                if existing:
                    base = name[:56]
                    for number in range(2, 203):
                        name = f'{base} ({number})'
                        if not db.execute('SELECT 1 FROM saves WHERE user_id=? AND name=?', (g.user['id'], name)).fetchone():
                            break
                sid = db.execute('''INSERT INTO saves(user_id,name,filename,game,notes,private,data,sha256,created,updated)
                    VALUES (?,?,?,?,?,?,?,?,?,?)''', (g.user['id'], name, filename, game, notes, private, data, sha, now, now)).lastrowid
            db.execute('UPDATE saves SET header_offset=?,uploaded_at=? WHERE id=? AND user_id=?', (header_offset, now, sid, g.user['id']))
            db.commit()
        except Exception:
            db.rollback()
            raise
        return f'OK\n{sid}\n{name}\n', 201, {'Content-Type': 'text/plain; charset=utf-8'}

    return app
