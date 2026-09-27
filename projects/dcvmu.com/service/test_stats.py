"""Unlisted statistics: accurate aggregates without exposing account or save data."""
import re
import sqlite3
import tempfile
import unittest
from html import unescape
from unittest.mock import patch

from app import create_app


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
                     bytes(512), 'fixture', cutoff - 1, self.NOW, self.NOW, 'data'),
                    (2, 1, 'Private fixture', 'SECRET', 'Secret game', 'Secret notes', 1,
                     bytes(1024), 'fixture', cutoff, self.NOW, cutoff, 'data'),
                    (3, 2, 'Hidden icon', 'ICONDATA_VMS', 'Customisation', '', 1,
                     bytes(1024), 'fixture', self.NOW, self.NOW, self.NOW, 'icon'),
                    (4, 2, 'Public icon', 'ICONDATA_VMS', 'Customisation', '', 0,
                     bytes(1024), 'fixture', cutoff - 1, self.NOW, None, 'icon'),
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
            'Registered users': '0', 'Total saves': '0', 'VMU archives': '0',
            'Stored data': '0 Bytes', 'Public saves': '0', 'Private saves': '0',
            'Game saves': '0', 'Custom VMU icons': '0', 'New users': '0',
            'New saves': '0', 'New VMU archives': '0',
        })

    def test_totals_recent_boundary_and_private_data(self):
        self.seed()
        with patch('app.time.time', return_value=self.NOW):
            response = self.client.get('/stats')
        self.assertEqual(self.metrics(response), {
            'Registered users': '3', 'Total saves': '4', 'VMU archives': '2',
            'Stored data': '259.5 KiB', 'Public saves': '2', 'Private saves': '2',
            'Game saves': '2', 'Custom VMU icons': '2', 'New users': '2',
            'New saves': '2', 'New VMU archives': '1',
        })
        for private_value in ('old_owner', 'new_owner', 'empty_account', '@example.com',
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
        self.assertEqual(changed['VMU archives'], '0')
        self.assertEqual(changed['Stored data'], '3.5 KiB')

    def test_unlisted_and_excluded_from_indexing(self):
        for method in (self.client.get, self.client.head):
            response = method('/stats')
            self.assertEqual(response.status_code, 200)
            self.assertEqual(response.headers['X-Robots-Tag'], 'noindex, nofollow')
            self.assertEqual(response.headers['Cache-Control'], 'no-store')
        for path in ('/', '/getting-started', '/support', '/login', '/register', '/stats'):
            response = self.client.get(path)
            self.assertEqual(response.status_code, 200)
            links = re.findall(r'href=[\"\']([^\"\']+)', unescape(response.text))
            self.assertFalse(any('/stats' in link for link in links), path)


if __name__ == '__main__':
    unittest.main()
