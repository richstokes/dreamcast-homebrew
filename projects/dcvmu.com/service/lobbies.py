"""Online lobbies (/lobbies): who is playing DreamKarts and Ridgeline online.

Each game server runs on this host and publishes its lobbies as JSON on a
loopback port (projects/common/gameserver/status). The page reads them on
request, with a short per-process cache so a busy page costs the game
servers at most one read every few seconds each.
"""
import json
import os
import time
import urllib.request

# Game name, the status JSON on this host, and its links.
SOURCES = [
    {'game': 'DreamKarts', 'url': 'http://127.0.0.1:8093/lobbies.json',
     'about': 'https://github.com/richstokes/dreamcast-homebrew/tree/main/projects/games/dreamkarts',
     'play': '/play?rom=continuous%2Fdreamkarts.cdi&name=DreamKarts'},
    {'game': 'Ridgeline', 'url': 'http://127.0.0.1:8094/lobbies.json',
     'about': 'https://github.com/richstokes/dreamcast-homebrew/tree/main/projects/games/ridgeline',
     'play': '/play?rom=continuous%2Fridgeline.elf&name=Ridgeline'},
]
FRESH_SECONDS = 5
TIMEOUT_SECONDS = 1.0
MAX_BYTES = 64 * 1024

_cache = {}


def fetch(url):
    """The server's snapshot, or None when it does not answer."""
    try:
        with urllib.request.urlopen(url, timeout=TIMEOUT_SECONDS) as response:
            data = json.loads(response.read(MAX_BYTES))
    except (OSError, ValueError):
        return None
    return data if isinstance(data, dict) and isinstance(data.get('lobbies'), list) else None


def snapshot(url, now=None):
    now = time.time() if now is None else now
    hit = _cache.get(url)
    if hit and now - hit[0] < FRESH_SECONDS:
        return hit[1]
    data = fetch(url)
    _cache[url] = (now, data)
    return data


def _lobby(raw):
    players = [{'name': str(p.get('name', ''))[:16], 'trophy': bool(p.get('trophy'))}
               for p in raw.get('players') or [] if isinstance(p, dict)]
    return {'id': int(raw.get('id', 0)), 'max': int(raw.get('max', 0)),
            'status': str(raw.get('status', '')), 'detail': str(raw.get('detail', '')),
            'demo': bool(raw.get('demo')), 'watchers': int(raw.get('watchers', 0)), 'players': players}


def ranges(ids):
    """[1, 3, 4, 5, 9] as '1, 3-5, 9' (with an en dash)."""
    parts, start = [], None
    for i, n in enumerate(ids):
        if start is None:
            start = n
        if i + 1 == len(ids) or ids[i + 1] != n + 1:
            parts.append(str(start) if start == n else f'{start}\u2013{n}')
            start = None
    return ', '.join(parts)


def games(sources=None):
    """Each game's lobbies for the page, in lobby order."""
    out = []
    for source in sources or SOURCES:
        data = snapshot(source['url'])
        game = dict(source, up=data is not None, online=0, playing=0, lobbies=[], empty='')
        try:
            lobbies = [_lobby(lobby) for lobby in data['lobbies'] if isinstance(lobby, dict)] if data else []
            online = int(data.get('online', 0)) if data else 0
        except (TypeError, ValueError):
            data = None
        game['up'] = data is not None
        if data is not None:
            game['online'] = online
            game['playing'] = sum(len(lobby['players']) for lobby in lobbies if not lobby['demo'])
            # Lobbies with someone in them (or the demo's bots) get a row;
            # the empty ones share a line.
            game['lobbies'] = [lobby for lobby in lobbies if lobby['players'] or lobby['demo']]
            game['empty'] = ranges([lobby['id'] for lobby in lobbies if not lobby['players'] and not lobby['demo']])
        out.append(game)
    return out


def sources_from_env():
    """DCVMU_LOBBY_SOURCES='Game=url,Game=url' points the page elsewhere (local testing)."""
    value = os.environ.get('DCVMU_LOBBY_SOURCES', '')
    if not value:
        return None
    by_name = {s['game']: s for s in SOURCES}
    out = []
    for item in value.split(','):
        name, _, url = item.partition('=')
        out.append(dict(by_name.get(name.strip(), {'game': name.strip(), 'about': '', 'play': ''}), url=url.strip()))
    return out
