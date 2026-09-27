"""Unlisted statistics, sortable users and profiles that only show public saves."""
import hashlib
import re
import sqlite3
import tempfile
import time
import unittest
from html import unescape
from unittest.mock import patch
from urllib.parse import parse_qs, urlsplit

from app import create_app
from test_service import vms
from test_vmu_tools import icon_data


class StatsTest(unittest.TestCase):
    NOW = 1801000000

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = self.temp.name + '/stats.sqlite3'
        self.app = create_app({'TESTING': True, 'DATABASE': self.path})
        self.client = self.app.test_client()

    def metrics(self, response):
        self.assertEqual(response.status_code, 200)
        return dict(re.findall(r'<dt>([^<]+)</dt><dd>([^<]+)</dd>', response.text))

    def seed(self):
        cutoff = self.NOW - 7 * 86400
        with sqlite3.connect(self.path) as db:
            db.executemany('''INSERT INTO users(id,username,email,password_hash,created)
                VALUES (?,?,?,?,?)''', [
                    (1, 'old_owner', 'old@example.com', 'unused', cutoff - 1),
                    (2, 'new_owner', 'new@example.com', 'unused', cutoff),
                    (3, 'empty_account', 'empty@example.com', 'unused', self.NOW),
                ])
            # Multiple saves per owner, both visibility settings and both kinds.
            # Recent edits/replacements must not count as newly created saves.
            db.executemany('''INSERT INTO saves(id,user_id,name,filename,game,notes,private,
                data,sha256,created,updated,uploaded_at,kind) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)''', [
                    (1, 1, 'Public fixture', 'PUBLIC', 'Public game', '', 0,
                     vms(), 'fixture', cutoff - 1, self.NOW, self.NOW, 'data'),
                    (2, 1, 'Private fixture', 'SECRET', 'Secret game', 'Secret notes', 1,
                     vms(payload=bytes(700)), 'fixture', cutoff, self.NOW, cutoff, 'data'),
                    (3, 2, 'Hidden icon', 'ICONDATA_VMS', 'Customisation', '', 1,
                     icon_data(), 'fixture', self.NOW, self.NOW, self.NOW, 'icon'),
                    (4, 2, 'Public icon', 'ICONDATA_VMS', 'Customisation', '', 0,
                     icon_data(), 'fixture', cutoff - 1, self.NOW, None, 'icon'),
                ])
            db.executemany('''INSERT INTO archives(user_id,name,source,files,free_blocks,
                data,sha256,created,updated) VALUES (?,?,?,?,?,?,?,?,?)''', [
                    (1, 'Secret archive', 'Private card', 3, 180,
                     bytes(131072), 'fixture', cutoff - 1, self.NOW),
                    (2, 'Recent archive', 'VMU A1', 7, 160,
                     bytes(131072), 'fixture', cutoff, self.NOW),
                ])

    def test_empty_database_is_readable_without_login(self):
        response = self.client.get('/stats')
        self.assertEqual(self.metrics(response), {
            'Registered users': '0', 'Total saves': '0', 'Whole-card archives': '0',
            'Stored data': '0 Bytes', 'Public saves': '0', 'Private saves': '0',
            'Game saves': '0', 'Custom VMU icons': '0', 'New users': '0',
            'New saves': '0', 'New whole-card archives': '0',
        })

    def test_totals_recent_boundary_and_private_data(self):
        self.seed()
        with patch('app.time.time', return_value=self.NOW):
            response = self.client.get('/stats')
        self.assertEqual(self.metrics(response), {
            'Registered users': '3', 'Total saves': '4', 'Whole-card archives': '2',
            'Stored data': '259.5 KiB', 'Public saves': '2', 'Private saves': '2',
            'Game saves': '2', 'Custom VMU icons': '2', 'New users': '2',
            'New saves': '2', 'New whole-card archives': '1',
        })
        for private_value in ('@example.com',
                              'Private fixture', 'SECRET', 'Secret game', 'Secret notes',
                              'Hidden icon', 'Secret archive', 'Private card', '/saves/', '/archives/'):
            self.assertNotIn(private_value, response.text)

        # Stats describe the current archive, not lifetime upload events.
        with sqlite3.connect(self.path) as db:
            db.execute('DELETE FROM saves WHERE id=2')
            db.execute('UPDATE saves SET private=1 WHERE id=1')
            db.execute('UPDATE saves SET data=? WHERE id=3', (bytes(2048),))
            db.execute('DELETE FROM archives')
        with patch('app.time.time', return_value=self.NOW):
            changed = self.metrics(self.client.get('/stats'))
        self.assertEqual(changed['Total saves'], '3')
        self.assertEqual(changed['Public saves'], '1')
        self.assertEqual(changed['Private saves'], '2')
        self.assertEqual(changed['New saves'], '1')
        self.assertEqual(changed['Whole-card archives'], '0')
        self.assertEqual(changed['Stored data'], '3.5 KiB')

    def test_unlisted_and_excluded_from_indexing(self):
        self.seed()
        for method in (self.client.get, self.client.head):
            for path in ('/stats', '/users/old_owner'):
                response = method(path)
                self.assertEqual(response.status_code, 200)
                self.assertEqual(response.headers['X-Robots-Tag'], 'noindex, nofollow')
                self.assertEqual(response.headers['Cache-Control'], 'no-store')
        for path in ('/', '/getting-started', '/support', '/login', '/register', '/users/old_owner'):
            response = self.client.get(path)
            self.assertEqual(response.status_code, 200)
            links = re.findall(r'href=[\"\']([^\"\']+)', unescape(response.text))
            self.assertFalse(any('/stats' in link for link in links), path)

    def members(self, response):
        self.assertEqual(response.status_code, 200)
        rows = re.findall(r'<td><a href="(/users/[^\"]+)">([^<]+)</a></td>\s*'
                          r'<td>.*?</td>\s*<td>([\d,]+)</td>', response.text, re.S)
        for url, username, _ in rows:
            self.assertEqual(url, '/users/' + username)
        return [(username, int(count.replace(',', ''))) for _, username, count in rows]

    def test_user_sorting_public_counts_and_pagination(self):
        self.seed()
        cutoff = self.NOW - 7 * 86400
        records = [('old_owner', cutoff - 1, 1), ('new_owner', cutoff, 1),
                   ('empty_account', self.NOW, 0)]
        with sqlite3.connect(self.path) as db:
            for number in range(25):
                name = ('Alpha' if number % 2 else 'beta') + f'{number:02}'
                joined = cutoff + number % 5
                count = number % 4
                uid = db.execute('INSERT INTO users(username,email,password_hash,created) VALUES (?,?,?,?)',
                                 (name, name + '@example.com', 'unused', joined)).lastrowid
                records.append((name, joined, count))
                # Every user has a private save, including some with no public saves.
                for index in range(count + 1):
                    db.execute('''INSERT INTO saves(user_id,name,filename,game,notes,private,data,
                        sha256,created,updated) VALUES (?,?,?,?,?,?,?,?,?,?)''',
                        (uid, f'Save {index}', 'TEST_SAVE', 'Test game', '', int(index == count),
                         vms(), 'fixture', joined, joined))
        expected = {
            'username_asc': sorted(records, key=lambda row: row[0].lower()),
            'username_desc': sorted(records, key=lambda row: row[0].lower(), reverse=True),
            'joined_asc': sorted(records, key=lambda row: (row[1], row[0].lower())),
            'joined_desc': sorted(records, key=lambda row: (-row[1], row[0].lower())),
            'saves_asc': sorted(records, key=lambda row: (row[2], row[0].lower())),
            'saves_desc': sorted(records, key=lambda row: (-row[2], row[0].lower())),
        }
        for sort, ordered in expected.items():
            with self.subTest(sort=sort):
                responses = [self.client.get('/stats', query_string={'sort': sort, 'page': page})
                             for page in (1, 2)]
                self.assertEqual([row for response in responses for row in self.members(response)],
                                 [(name, count) for name, _, count in ordered])
                self.assertEqual(len(self.members(responses[0])), 20)
                direction = 'ascending' if sort.endswith('_asc') else 'descending'
                self.assertIn(f'aria-sort="{direction}"', responses[0].text)
                toggle = sort.rsplit('_', 1)[0] + ('_desc' if sort.endswith('_asc') else '_asc')
                self.assertIn(f'href="/stats?sort={toggle}#users"', responses[0].text)
                for response in responses:
                    for link in re.findall(r'href="([^\"]+)">(?:Previous|Next) page', response.text):
                        parsed = urlsplit(unescape(link))
                        self.assertEqual(parsed.path, '/stats')
                        self.assertEqual(parsed.fragment, 'users')
                        self.assertEqual(parse_qs(parsed.query)['sort'], [sort])
        for sort in ('invalid', 'u.username; DROP TABLE users'):
            response = self.client.get('/stats', query_string={'sort': sort})
            self.assertEqual(self.members(response), [(name, count) for name, _, count in expected['username_asc'][:20]])
        self.assertEqual(self.client.get('/stats?page=bad').status_code, 400)
        self.assertEqual(self.members(self.client.get('/stats?page=-1')),
                         self.members(self.client.get('/stats')))
        self.assertIn('Page 2 of 2', self.client.get('/stats?page=999999').text)

    def profile_save_ids(self, response):
        self.assertEqual(response.status_code, 200)
        return [int(sid) for sid in re.findall(r'<h2><a href="/saves/(\d+)"', response.text)]

    def test_profile_matches_exact_user_and_never_shows_private_saves(self):
        self.seed()
        with sqlite3.connect(self.path) as db:
            db.execute('INSERT INTO users(id,username,email,password_hash,created) VALUES (4,?,?,?,?)',
                       ('old_owner_extra', 'extra@example.com', 'unused', self.NOW))
            db.execute('''INSERT INTO saves(user_id,name,filename,game,notes,private,data,sha256,created,updated)
                VALUES (4,'Other account save','OTHER','Other game','',0,?,'fixture',?,?)''',
                (vms(), self.NOW, self.NOW))
        response = self.client.get('/users/OLD_OWNER')
        self.assertEqual(self.profile_save_ids(response), [1])
        self.assertIn('<h1 class="profile-name">old_owner</h1>', response.text)
        self.assertIn('1 public save,', response.text)
        for secret in ('Private fixture', 'Secret game', 'Secret notes', 'Secret archive',
                       'Other account save', '@example.com', '/saves/2', '/archives/'):
            self.assertNotIn(secret, response.text)
        self.assertEqual(self.profile_save_ids(self.client.get('/users/new_owner')), [4])
        self.assertIn('src="/saves/4/icon.png"', self.client.get('/users/new_owner').text)
        self.assertEqual(self.client.get('/saves/4/icon.png').status_code, 200)
        self.assertEqual(self.client.get('/saves/1').status_code, 200)
        self.assertEqual(self.client.get('/saves/1/download').data, vms())
        self.assertEqual(self.client.get('/saves/2').status_code, 404)
        self.assertEqual(self.client.get('/saves/2/download').status_code, 404)
        for name in ('old', 'missing', "old_owner' OR 1=1--"):
            self.assertEqual(self.client.get('/users/' + name).status_code, 404)

        token = 'profile-owner-session'
        with sqlite3.connect(self.path) as db:
            db.execute('INSERT INTO sessions VALUES (?,?,?,?,?)',
                       (hashlib.sha256(token.encode()).hexdigest(), 1, 'c' * 43,
                        int(time.time()) + 86400, 'web'))
        self.client.set_cookie('dcvmu_session', token)
        owner = self.client.get('/users/old_owner')
        self.assertIn("old_owner's account", owner.text)
        self.assertEqual(self.profile_save_ids(owner), [1])
        self.assertNotIn('Private fixture', owner.text)

    def test_empty_and_private_only_profiles(self):
        self.seed()
        with sqlite3.connect(self.path) as db:
            db.execute('UPDATE saves SET private=1 WHERE user_id=2')
        for name in ('empty_account', 'new_owner'):
            response = self.client.get('/users/' + name)
            self.assertEqual(self.profile_save_ids(response), [])
            self.assertIn('No public saves yet', response.text)
            self.assertIn('0 public saves,', response.text)
            self.assertNotIn('Hidden icon', response.text)

    def test_profile_pagination_uses_public_upload_order(self):
        self.seed()
        records = [(1, self.NOW)]
        with sqlite3.connect(self.path) as db:
            for number in range(25):
                created = 100 + number
                uploaded = None if number % 3 == 0 else 200 + number % 4
                sid = db.execute('''INSERT INTO saves(user_id,name,filename,game,notes,private,data,
                    sha256,created,updated,uploaded_at) VALUES (1,?,?,?,'',0,?,'fixture',?,?,?)''',
                    (f'Public save {number}', 'TEST_SAVE', 'Test game', vms(), created, self.NOW, uploaded)).lastrowid
                records.append((sid, uploaded or created))
        first = self.client.get('/users/old_owner')
        next_url = unescape(re.search(r'href="([^\"]+)">Next page', first.text)[1])
        self.assertEqual(next_url, '/users/old_owner?page=2')
        second = self.client.get(next_url)
        expected = [sid for sid, _ in sorted(records, key=lambda row: (row[1], row[0]), reverse=True)]
        self.assertEqual(self.profile_save_ids(first), expected[:20])
        self.assertEqual(self.profile_save_ids(second), expected[20:])
        self.assertIn('Page 2 of 2', second.text)
        self.assertIn('26 public saves,', second.text)
        self.assertEqual(self.profile_save_ids(self.client.get('/users/old_owner?page=999999')), expected[20:])
        self.assertEqual(self.profile_save_ids(self.client.get('/users/old_owner?page=-1')), expected[:20])
        self.assertEqual(self.client.get('/users/old_owner?page=bad').status_code, 400)


if __name__ == '__main__':
    unittest.main()
