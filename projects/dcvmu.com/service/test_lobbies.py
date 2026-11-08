"""The online lobbies page, from the game servers' status JSON."""
import json
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, HTTPServer
from unittest.mock import patch

import lobbies
from app import create_app

DREAMKARTS = {'game': 'DreamKarts', 'build': 35, 'online': 3, 'lobbies': [
    {'id': 1, 'max': 6, 'status': 'Racing', 'detail': 'Emerald Beach, 3 laps', 'players': [{'name': 'RICH'}, {'name': '<B>'}]},
    {'id': 2, 'max': 6, 'status': 'Empty', 'players': []},
    {'id': 10, 'max': 6, 'status': 'Racing', 'detail': 'Coral Crossing, 3 laps', 'demo': True, 'watchers': 2,
     'players': [{'name': 'BREEZE'}]},
]}
RIDGELINE = {'game': 'Ridgeline', 'build': 1, 'online': 1, 'lobbies': [
    {'id': 1, 'max': 4, 'status': 'Exploring', 'detail': '09:30 on the mountain', 'players': [{'name': 'PAL', 'trophy': True}]},
]}


class Handler(BaseHTTPRequestHandler):
    body = b''

    def do_GET(self):
        self.send_response(200)
        self.end_headers()
        self.wfile.write(self.body)

    def log_message(self, *args):
        pass


class LobbiesTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        lobbies._cache.clear()
        self.sources = [dict(lobbies.SOURCES[0], url='dk'), dict(lobbies.SOURCES[1], url='rl')]
        self.app = create_app({'TESTING': True, 'DATABASE': self.temp.name + '/l.sqlite3', 'LOBBY_SOURCES': self.sources})
        self.client = self.app.test_client()

    def test_page(self):
        with patch.object(lobbies, 'fetch', side_effect=lambda url: {'dk': DREAMKARTS, 'rl': RIDGELINE}[url]):
            response = self.client.get('/lobbies')
        self.assertEqual(response.status_code, 200)
        text = response.text
        self.assertIn('<meta http-equiv="refresh" content="20">', text)
        self.assertIn('3 online, 2 in a lobby', text)   # the demo lobby's bots are not people
        self.assertIn('Emerald Beach, 3 laps', text)
        self.assertIn('&lt;B&gt;', text)                 # names are escaped
        self.assertNotIn('<B>', text)
        self.assertIn('+2 watching', text)
        self.assertIn('PAL <span title="Reached the summit"', text)
        self.assertIn('09:30 on the mountain', text)
        self.assertIn('Empty: lobby 2', text)

    def test_ranges(self):
        self.assertEqual(lobbies.ranges([1, 3, 4, 5, 9]), '1, 3\u20135, 9')
        self.assertEqual(lobbies.ranges([]), '')

    def test_server_down(self):
        with patch.object(lobbies, 'fetch', side_effect=lambda url: RIDGELINE if url == 'rl' else None):
            text = self.client.get('/lobbies').text
        self.assertIn('The DreamKarts server is not answering', text)
        self.assertIn('1 online, 1 in a lobby', text)

    def test_cache(self):
        calls = []
        with patch.object(lobbies, 'fetch', side_effect=lambda url: calls.append(url) or RIDGELINE):
            lobbies.snapshot('rl', now=100)
            lobbies.snapshot('rl', now=103)
            lobbies.snapshot('rl', now=106)
        self.assertEqual(calls, ['rl', 'rl'])

    def test_fetch_real_http(self):
        Handler.body = json.dumps(RIDGELINE).encode()
        server = HTTPServer(('127.0.0.1', 0), Handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.shutdown)
        self.assertEqual(lobbies.fetch(f'http://127.0.0.1:{server.server_port}/lobbies.json'), RIDGELINE)
        Handler.body = b'not json'
        self.assertIsNone(lobbies.fetch(f'http://127.0.0.1:{server.server_port}/lobbies.json'))
        self.assertIsNone(lobbies.fetch('http://127.0.0.1:1/lobbies.json'))
