"""Dreambox: a small network drive for Archicast, RISC OS 3.11 on the Dreamcast.

Consoles link with a short code typed on the website, so no password crosses
Archicast's TLS, which cannot check certificates. Files keep RISC OS load and
exec addresses and attributes, with RISC OS names: '.' separates directories
and '/' is the extension dot. The console API answers in plain text with one
object per line, "type load exec size attr name" (hex, hex, hex, decimal, hex),
so the SH-4 side needs no parser beyond strtoul.

Writes arrive in 32 KiB blocks and are staged until a commit, so a console
never needs a whole file in memory. A commit can take unwritten blocks from the
current file (a file opened for update) or leave them zero.
"""
import hashlib
import io
import re
import secrets
import time
from urllib.parse import urlencode

from flask import abort, g, redirect, request, send_file, url_for

SCHEMA = '''
CREATE TABLE IF NOT EXISTS drive_files (
 id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
 parent INTEGER NOT NULL, name TEXT NOT NULL COLLATE NOCASE, is_dir INTEGER NOT NULL,
 load INTEGER NOT NULL, exec INTEGER NOT NULL, attr INTEGER NOT NULL,
 size INTEGER NOT NULL, data BLOB, updated INTEGER NOT NULL,
 UNIQUE(user_id, parent, name));
CREATE TABLE IF NOT EXISTS drive_staging (
 user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE, path TEXT NOT NULL,
 block INTEGER NOT NULL, data BLOB NOT NULL, expires INTEGER NOT NULL,
 PRIMARY KEY(user_id, path, block));
CREATE TABLE IF NOT EXISTS drive_links (
 hash TEXT PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
 created INTEGER NOT NULL, last_used INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS drive_pairing (
 device TEXT PRIMARY KEY, code TEXT NOT NULL UNIQUE, user_id INTEGER REFERENCES users(id) ON DELETE CASCADE,
 expires INTEGER NOT NULL);
'''

BLOCK = 32 * 1024
MAX_FILE = 4 * 1024 * 1024
QUOTA = 16 * 1024 * 1024
MAX_OBJECTS = 2000
MAX_ENTRIES = 255     # per directory; the Filer gets slow well before this
MAX_DEPTH = 12
PAIR_SECONDS = 900
LINK_IDLE_SECONDS = 365 * 86400
STAGING_SECONDS = 3600
CODE_ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'
# Characters FileSwitch and the command line treat specially are left out, and so
# are spaces. Top-bit Latin-1 would be legal on RISC OS but not worth the trouble.
LEAF = re.compile(r"[A-Za-z0-9!'()+,\-/;=\[\]_{}~]{1,32}")
FILE_ATTR = 0x03      # owner read and write
TYPE_FILE, TYPE_DIR = 1, 2
TEXT = {'Content-Type': 'text/plain; charset=utf-8'}

# Extensions for the website: RISC OS type to a download suffix and back.
EXTENSIONS = {0xFFF: 'txt', 0xC85: 'jpg', 0xB60: 'png', 0x695: 'gif', 0xA91: 'zip',
              0xADF: 'pdf', 0xFAF: 'html', 0xDFE: 'csv', 0xFC9: 'sun', 0xFF0: 'tif'}
TYPES = {ext: kind for kind, ext in EXTENSIONS.items()} | {'jpeg': 0xC85, 'htm': 0xFAF, 'text': 0xFFF, 'md': 0xFFF}
TYPE_NAMES = {0xFFF: 'Text', 0xAFF: 'DrawFile', 0xFF9: 'Sprite', 0xFFB: 'BASIC', 0xFF8: 'Absolute',
              0xFEB: 'Obey', 0xFFD: 'Data', 0xC85: 'JPEG', 0xB60: 'PNG', 0x695: 'GIF', 0xA91: 'Archive',
              0xADF: 'PDF', 0xFAF: 'HTML', 0xDFE: 'CSV', 0xFFA: 'Module', 0xFFE: 'Command',
              0xFF6: 'Font', 0xFED: 'Palette', 0xAE6: 'Pen Down'}


def riscos_now():
    """Centiseconds since 1900, the RISC OS clock."""
    return int((time.time() + 2208988800) * 100)


def stamp(filetype, cs=None):
    cs = riscos_now() if cs is None else cs
    return 0xFFF00000 | (filetype & 0xFFF) << 8 | (cs >> 32) & 0xFF, cs & 0xFFFFFFFF


def filetype(load):
    return (load >> 8) & 0xFFF if load & 0xFFF00000 == 0xFFF00000 else None


def unix_time(load, exec_):
    if load & 0xFFF00000 != 0xFFF00000:
        return None
    return ((load & 0xFF) << 32 | exec_) / 100 - 2208988800


def riscos_when(row):
    """The RISC OS stamp as Unix time, or the server's own when there's no plausible one"""
    when = unix_time(row['load'], row['exec'])
    return when if when and when >= 315532800 else row['updated']


def split_path(text):
    """A RISC OS path from the console or a form, without its '$', as a list of leaves."""
    text = text.strip()
    if text.startswith('$'):
        text = text[1:].lstrip('.')
    if not text:
        return []
    leaves = text.split('.')
    if len(leaves) > MAX_DEPTH or not all(LEAF.fullmatch(leaf) for leaf in leaves):
        abort(400, 'Invalid Dreambox path. Names are 1-32 letters, numbers or !\'()+,-/;=[]_{}~')
    return leaves


def web_name(filename):
    """A computer's filename as a RISC OS leaf and type. 'name,xxx' keeps its type."""
    filename = filename.replace('\\', '/').rsplit('/', 1)[-1].strip()
    kind = 0xFFD
    match = re.fullmatch(r'(.+),([0-9A-Fa-f]{3})', filename)
    if match:
        filename, kind = match[1], int(match[2], 16)
    elif '.' in filename.lstrip('.'):
        stem, ext = filename.rsplit('.', 1)
        kind = TYPES.get(ext.lower(), 0xFFD)
    leaf = re.sub(r"[^A-Za-z0-9!'()+,\-/;=\[\]_{}~]", '_', filename.replace('.', '/'))[:32]
    return leaf or 'Upload', kind


def download_name(leaf, load):
    """The other way: '/' back to '.', and a ',xxx' suffix if there's no extension."""
    name = leaf.replace('/', '.')
    kind = filetype(load)
    if '.' not in name.lstrip('.') and kind is not None:
        name += '.' + EXTENSIONS[kind] if kind in EXTENSIONS else f',{kind:03x}'
    return name


def line(row):
    return '%d %08X %08X %d %X %s' % (TYPE_DIR if row['is_dir'] else TYPE_FILE, row['load'] & 0xFFFFFFFF,
                                      row['exec'] & 0xFFFFFFFF, row['size'], row['attr'], row['name'])


def register(app, database, limit, page, require_user):
    """Add the drive's routes. Called from create_app with its helpers; returns the
    lookup identify() uses for console tokens."""
    def uid():
        return g.user['id']

    def lookup(leaves, want_dir=False):
        """The row for a path, None if missing. The root is a pseudo-row with id 0."""
        row = {'id': 0, 'is_dir': 1, 'name': '$', 'load': 0, 'exec': 0, 'attr': 0, 'size': 0}
        for depth, leaf in enumerate(leaves):
            if not row['is_dir']:
                return None
            row = database().execute('SELECT id,parent,name,is_dir,load,exec,attr,size,updated FROM drive_files '
                                     'WHERE user_id=? AND parent=? AND name=?', (uid(), row['id'], leaf)).fetchone()
            if row is None:
                return None
        if want_dir and not row['is_dir']:
            return None
        return row

    def parent_of(leaves):
        if not leaves:
            abort(400, 'The root directory cannot be changed.')
        parent = lookup(leaves[:-1], want_dir=True)
        if parent is None:
            abort(404, 'Directory not found.')
        return parent

    def usage(db):
        return db.execute('SELECT coalesce(sum(size),0),count(*) FROM drive_files WHERE user_id=?', (uid(),)).fetchone()

    def room_for(db, parent_id, adding, replacing=0, new_object=True):
        used, objects = usage(db)
        if adding > MAX_FILE:
            abort(413, 'Dreambox files can be up to 4MB.')
        if used - replacing + adding > QUOTA:
            abort(413, 'Dreambox is full (16MB). Delete something first.')
        if new_object:
            if objects >= MAX_OBJECTS:
                abort(413, f'Dreambox holds up to {MAX_OBJECTS} files and directories.')
            if db.execute('SELECT count(*) FROM drive_files WHERE user_id=? AND parent=?',
                          (uid(), parent_id)).fetchone()[0] >= MAX_ENTRIES:
                abort(413, f'That directory is full ({MAX_ENTRIES} entries).')

    def hex_arg(name, default=None):
        value = request.args.get(name)
        if value is None:
            if default is None:
                abort(400, f'Missing {name}.')
            return default
        if not re.fullmatch(r'[0-9A-Fa-f]{1,8}', value):
            abort(400, f'Invalid {name}.')
        return int(value, 16)

    def int_arg(name, maximum):
        value = request.args.get(name, '')
        if not re.fullmatch(r'\d{1,10}', value) or int(value) > maximum:
            abort(400, f'Invalid {name}.')
        return int(value)

    def put_file(db, leaves, data, load, exec_, attr=FILE_ATTR):
        """Create or replace a file. The caller owns a write transaction."""
        parent = parent_of(leaves)
        existing = lookup(leaves)
        if existing and existing['is_dir']:
            abort(409, 'A directory has that name.')
        room_for(db, parent['id'], len(data), existing['size'] if existing else 0, existing is None)
        if existing:
            db.execute('UPDATE drive_files SET name=?,load=?,exec=?,attr=?,size=?,data=?,updated=? WHERE id=?',
                       (leaves[-1], load, exec_, attr, len(data), data, int(time.time()), existing['id']))
        else:
            db.execute('INSERT INTO drive_files(user_id,parent,name,is_dir,load,exec,attr,size,data,updated) '
                       'VALUES (?,?,?,0,?,?,?,?,?,?)',
                       (uid(), parent['id'], leaves[-1], load, exec_, attr, len(data), data, int(time.time())))

    def make_dir(db, leaves, load=None, exec_=None):
        parent = parent_of(leaves)
        existing = lookup(leaves)
        if existing:
            if not existing['is_dir']:
                abort(409, 'A file has that name.')
            return
        room_for(db, parent['id'], 0)
        if load is None:
            load, exec_ = stamp(0xFFD)
        db.execute('INSERT INTO drive_files(user_id,parent,name,is_dir,load,exec,attr,size,updated) '
                   'VALUES (?,?,?,1,?,?,0,0,?)', (uid(), parent['id'], leaves[-1], load, exec_, int(time.time())))

    def remove(db, row):
        if row['is_dir'] and db.execute('SELECT 1 FROM drive_files WHERE user_id=? AND parent=?',
                                        (uid(), row['id'])).fetchone():
            abort(409, 'Directory not empty.')
        db.execute('DELETE FROM drive_files WHERE id=? AND user_id=?', (row['id'], uid()))

    def move(db, source, target):
        row = lookup(source)
        if row is None:
            abort(404, 'Not found.')
        parent = parent_of(target)
        existing = lookup(target)
        if existing and existing['id'] != row['id']:
            abort(409, 'Something already has that name.')
        # A directory can't go inside itself
        probe = parent
        while probe['id']:
            if probe['id'] == row['id']:
                abort(400, 'A directory cannot move inside itself.')
            probe = db.execute('SELECT id,parent FROM drive_files WHERE id=?', (probe['parent'],)).fetchone() \
                or {'id': 0}
        if parent['id'] != row['parent']:
            room_for(db, parent['id'], 0)
        db.execute('UPDATE drive_files SET parent=?,name=?,updated=? WHERE id=?',
                   (parent['id'], target[-1], int(time.time()), row['id']))

    # --- linking a console ---

    def token_user(token):
        """The drive user for a console's bearer token. Last use is kept to the day."""
        if not token:
            return None
        now = int(time.time())
        db = database()
        row = db.execute('SELECT l.hash,l.last_used,u.id,u.username,u.theme FROM drive_links l '
                         'JOIN users u ON u.id=l.user_id WHERE l.hash=?',
                         (hashlib.sha256(token.encode()).hexdigest(),)).fetchone()
        if row is None or row['last_used'] < now - LINK_IDLE_SECONDS:
            return None
        if row['last_used'] < now - 86400:
            with db:
                db.execute('UPDATE drive_links SET last_used=? WHERE hash=?', (now, row['hash']))
        return row

    @app.post('/api/v1/drive/pair')
    def drive_pair():
        # "device secret\ncode\nseconds\n". The console shows the code and keeps asking
        # pair/claim with the secret until someone types the code on the website.
        limit('drive-pair:' + (request.remote_addr or ''), 20, 3600)
        device = secrets.token_urlsafe(32)
        code = ''.join(secrets.choice(CODE_ALPHABET) for _ in range(8))
        now = int(time.time())
        with database() as db:
            db.execute('DELETE FROM drive_pairing WHERE expires<?', (now,))
            db.execute('INSERT INTO drive_pairing VALUES (?,?,NULL,?)',
                       (hashlib.sha256(device.encode()).hexdigest(), code, now + PAIR_SECONDS))
        return f'{device}\n{code[:4]}-{code[4:]}\n{PAIR_SECONDS}\n', 200, TEXT

    @app.post('/api/v1/drive/pair/claim')
    def drive_claim():
        # 200 and the token once linked, 202 WAIT before then, 404 once the code expired.
        limit('drive-claim:' + (request.remote_addr or ''), 600, 3600)
        device = hashlib.sha256(request.args.get('device', '').encode()).hexdigest()
        now = int(time.time())
        db = database()
        row = db.execute('SELECT * FROM drive_pairing WHERE device=? AND expires>=?', (device, now)).fetchone()
        if row is None:
            abort(404, 'Link code expired.')
        if row['user_id'] is None:
            return 'WAIT\n', 202, TEXT
        token = secrets.token_urlsafe(32)
        with db:
            if not db.execute('DELETE FROM drive_pairing WHERE device=?', (device,)).rowcount:
                abort(404, 'Link code expired.')
            db.execute('INSERT INTO drive_links VALUES (?,?,?,?)',
                       (hashlib.sha256(token.encode()).hexdigest(), row['user_id'], now, now))
        return token + '\n', 200, TEXT

    # --- the console's drive API ---

    def drive_user():
        if not g.user:
            abort(401, 'Dreambox is not linked.')
        limit('drive:' + str(uid()), 3000, 600)

    @app.get('/api/v1/drive/list')
    def drive_list():
        drive_user()
        row = lookup(split_path(request.args.get('path', '')), want_dir=True)
        if row is None:
            abort(404, 'Directory not found.')
        rows = database().execute('SELECT name,is_dir,load,exec,attr,size FROM drive_files WHERE user_id=? '
                                  'AND parent=? ORDER BY name', (uid(), row['id'])).fetchall()
        return ''.join(line(r) + '\n' for r in rows), 200, TEXT

    @app.get('/api/v1/drive/info')
    def drive_info():
        drive_user()
        row = lookup(split_path(request.args.get('path', '')))
        if row is None:
            abort(404, 'Not found.')
        return line(row) + '\n', 200, TEXT

    @app.get('/api/v1/drive/read')
    def drive_read():
        # offset and length in bytes, up to a block. staged=1 reads through blocks
        # written since the file was opened, for a file being updated.
        drive_user()
        path = '.'.join(split_path(request.args.get('path', '')))
        offset = int_arg('offset', MAX_FILE)
        length = int_arg('length', BLOCK)
        db = database()
        row = lookup(path.split('.') if path else [])
        staged = {}
        if request.args.get('staged') == '1':
            first, last = offset // BLOCK, (offset + length - 1) // BLOCK
            staged = {r['block']: r['data'] for r in db.execute(
                'SELECT block,data FROM drive_staging WHERE user_id=? AND path=? AND block BETWEEN ? AND ?',
                (uid(), path.lower(), first, last))}
        if (row is None or row['is_dir']) and not staged:
            abort(404, 'File not found.')
        out = bytearray()
        at, end = offset, offset + length
        while at < end:
            block, skip = divmod(at, BLOCK)
            take = min(BLOCK - skip, end - at)
            if block in staged:
                # A staged block is the whole block as far as the file goes
                piece = staged[block][skip:skip + take]
                piece += bytes(take - len(piece))
            elif row and not row['is_dir']:
                piece = db.execute('SELECT substr(data,?,?) FROM drive_files WHERE id=?',
                                   (at + 1, take, row['id'])).fetchone()[0] or b''
            else:
                piece = b''
            out += piece
            if len(piece) < take:
                break
            at += take
        return send_file(io.BytesIO(bytes(out)), mimetype='application/octet-stream')

    @app.post('/api/v1/drive/write')
    def drive_write():
        # One block of a file being written, held until commit. fresh=1 starts over.
        drive_user()
        leaves = split_path(request.args.get('path', ''))
        if not leaves:
            abort(400, 'Invalid Dreambox path.')
        block = int_arg('block', MAX_FILE // BLOCK - 1)
        data = request.get_data(cache=False)
        if len(data) > BLOCK:
            abort(413, 'Blocks are up to 32KB.')
        key = '.'.join(leaves).lower()
        now = int(time.time())
        with database() as db:
            db.execute('DELETE FROM drive_staging WHERE expires<?', (now,))
            if request.args.get('fresh') == '1':
                db.execute('DELETE FROM drive_staging WHERE user_id=? AND path=?', (uid(), key))
            db.execute('INSERT OR REPLACE INTO drive_staging VALUES (?,?,?,?,?)',
                       (uid(), key, block, data, now + STAGING_SECONDS))
            if db.execute('SELECT coalesce(sum(length(data)),0) FROM drive_staging WHERE user_id=?',
                          (uid(),)).fetchone()[0] > 2 * MAX_FILE:
                abort(413, 'Too much unsaved Dreambox data. Close some files.')
        return 'OK\n', 200, TEXT

    @app.post('/api/v1/drive/commit')
    def drive_commit():
        # Replace the file with its staged blocks. Blocks never written come from the
        # current file with keep=1, else they're zero. staged=0 ignores staging.
        drive_user()
        leaves = split_path(request.args.get('path', ''))
        size = int_arg('size', MAX_FILE)
        load, exec_ = hex_arg('load'), hex_arg('exec')
        attr = hex_arg('attr', FILE_ATTR) & 0xFF
        key = '.'.join(leaves).lower()
        db = database()
        try:
            db.execute('BEGIN IMMEDIATE')
            staged = {}
            if request.args.get('staged') == '1':
                staged = {r['block']: r['data'] for r in db.execute(
                    'SELECT block,data FROM drive_staging WHERE user_id=? AND path=?', (uid(), key))}
            old = b''
            if request.args.get('keep') == '1':
                existing = lookup(leaves)
                if existing and not existing['is_dir']:
                    old = db.execute('SELECT data FROM drive_files WHERE id=?', (existing['id'],)).fetchone()[0] or b''
            data = bytearray(size)
            for block in range((size + BLOCK - 1) // BLOCK):
                piece = staged.get(block, old[block * BLOCK:(block + 1) * BLOCK])
                piece = piece[:size - block * BLOCK]
                data[block * BLOCK:block * BLOCK + len(piece)] = piece
            put_file(db, leaves, bytes(data), load, exec_, attr)
            db.execute('DELETE FROM drive_staging WHERE user_id=? AND path=?', (uid(), key))
            db.commit()
        except BaseException:
            db.rollback()
            raise
        return 'OK\n', 200, TEXT

    @app.post('/api/v1/drive/stamp')
    def drive_stamp():
        drive_user()
        leaves = split_path(request.args.get('path', ''))
        with database() as db:
            row = lookup(leaves) if leaves else None
            if row is None:
                abort(404, 'Not found.')
            db.execute('UPDATE drive_files SET load=?,exec=?,attr=?,updated=? WHERE id=?',
                       (hex_arg('load', row['load'] & 0xFFFFFFFF), hex_arg('exec', row['exec'] & 0xFFFFFFFF),
                        hex_arg('attr', row['attr']) & 0xFF, int(time.time()), row['id']))
        return 'OK\n', 200, TEXT

    @app.post('/api/v1/drive/mkdir')
    def drive_mkdir():
        drive_user()
        leaves = split_path(request.args.get('path', ''))
        with database() as db:
            db.execute('BEGIN IMMEDIATE')
            make_dir(db, leaves, *((hex_arg('load'), hex_arg('exec')) if 'load' in request.args else ()))
        return 'OK\n', 200, TEXT

    @app.post('/api/v1/drive/delete')
    def drive_delete():
        # The deleted object's line, as FileSwitch reports it, or "0" if there was none.
        drive_user()
        leaves = split_path(request.args.get('path', ''))
        with database() as db:
            db.execute('BEGIN IMMEDIATE')
            row = lookup(leaves) if leaves else None
            if row is None:
                return '0\n', 200, TEXT
            remove(db, row)
        return line(row) + '\n', 200, TEXT

    @app.post('/api/v1/drive/rename')
    def drive_rename():
        drive_user()
        source, target = split_path(request.args.get('from', '')), split_path(request.args.get('to', ''))
        if not source or not target:
            abort(400, 'Invalid rename.')
        with database() as db:
            db.execute('BEGIN IMMEDIATE')
            move(db, source, target)
        return 'OK\n', 200, TEXT

    # --- the website: Account, Archicast ---

    def folder_url(leaves, **extra):
        return url_for('archicast', path='.'.join(leaves), **extra) if leaves else url_for('archicast', **extra)

    @app.get('/account/archicast')
    def archicast():
        require_user()
        leaves = split_path(request.args.get('path', ''))
        folder = lookup(leaves, want_dir=True)
        if folder is None:
            abort(404, 'Directory not found.')
        db = database()
        rows = db.execute('SELECT name,is_dir,load,exec,size,updated FROM drive_files WHERE user_id=? AND parent=? '
                          'ORDER BY is_dir DESC,name COLLATE NOCASE', (uid(), folder['id'])).fetchall()
        entries = []
        for row in rows:
            kind = filetype(row['load'])
            entries.append(dict(row, path='.'.join(leaves + [row['name']]),
                                kind='Directory' if row['is_dir'] else
                                TYPE_NAMES.get(kind, f'&{kind:03X}' if kind is not None else 'Untyped'),
                                when=riscos_when(row)))
        used, objects = usage(db)
        links = db.execute('SELECT created,last_used,hash FROM drive_links WHERE user_id=? ORDER BY created',
                           (uid(),)).fetchall()
        crumbs = [('$', folder_url([]))] + [(leaf, folder_url(leaves[:n + 1])) for n, leaf in enumerate(leaves)]
        return page('archicast.html', entries=entries, path='.'.join(leaves), crumbs=crumbs, used=used,
                    quota=QUOTA, objects=objects, links=links, linked=request.args.get('linked'))

    def back_to(leaves, **extra):
        return redirect(folder_url(leaves, **extra), 303)

    @app.post('/account/archicast/link')
    def archicast_link():
        require_user()
        limit('drive-link:' + str(uid()), 10, 600)
        code = re.sub(r'[^A-Z0-9]', '', request.form.get('code', '').upper())
        now = int(time.time())
        with database() as db:
            linked = db.execute('UPDATE drive_pairing SET user_id=? WHERE code=? AND expires>=? AND user_id IS NULL',
                                (uid(), code, now)).rowcount if len(code) == 8 else 0
        if not linked:
            abort(400, 'That code is not valid. Codes last 15 minutes; open Dreambox again for a new one.')
        return back_to([], linked=1)

    @app.post('/account/archicast/unlink')
    def archicast_unlink():
        require_user()
        with database() as db:
            db.execute('DELETE FROM drive_links WHERE user_id=? AND hash=?', (uid(), request.form.get('link', '')))
        return back_to([])

    @app.post('/account/archicast/upload')
    def archicast_upload():
        require_user()
        limit('upload:' + str(uid()), 60, 3600)
        leaves = split_path(request.form.get('path', ''))
        files = request.files.getlist('file')
        if len(files) != 1 or not files[0].filename:
            abort(400, 'Choose a file to upload.')
        data = files[0].read(MAX_FILE + 1)
        leaf, kind = web_name(files[0].filename)
        load, exec_ = stamp(kind)
        with database() as db:
            db.execute('BEGIN IMMEDIATE')
            put_file(db, leaves + [leaf], data, load, exec_)
        return back_to(leaves)

    @app.post('/account/archicast/mkdir')
    def archicast_mkdir():
        require_user()
        leaves = split_path(request.form.get('path', ''))
        name = request.form.get('name', '').strip()
        if not LEAF.fullmatch(name):
            abort(400, "Directory names are 1-32 letters, numbers or !'()+,-/;=[]_{}~")
        with database() as db:
            db.execute('BEGIN IMMEDIATE')
            make_dir(db, leaves + [name])
        return back_to(leaves)

    @app.post('/account/archicast/delete')
    def archicast_delete():
        require_user()
        leaves = split_path(request.form.get('path', ''))
        with database() as db:
            db.execute('BEGIN IMMEDIATE')
            row = lookup(leaves) if leaves else None
            if row is None:
                abort(404, 'Not found.')
            remove(db, row)
        return back_to(leaves[:-1])

    @app.get('/account/archicast/download')
    def archicast_download():
        require_user()
        leaves = split_path(request.args.get('path', ''))
        row = lookup(leaves) if leaves else None
        if row is None or row['is_dir']:
            abort(404, 'File not found.')
        data = database().execute('SELECT data FROM drive_files WHERE id=?', (row['id'],)).fetchone()[0]
        return send_file(io.BytesIO(data or b''), as_attachment=True, mimetype='application/octet-stream',
                         download_name=download_name(row['name'], row['load']))

    return token_user
