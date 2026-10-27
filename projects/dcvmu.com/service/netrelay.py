"""Network relay for the web player: browser WebSocket to DCNet's Ethernet tunnel.

The web player's Flycast core hands its emulated broadband adapter's Ethernet
frames to the page (tools/flycast-wasm), which sends each one as a binary
WebSocket message to /play/net. This relays them to DCNet, flyinghead's
Dreamcast network (dcnet.flyca.st), so browser players get the internet
access Flycast's DCNet mode gives. The destination is fixed; this is not a
general proxy. It listens on loopback behind Caddy, which sets X-Real-IP.
"""
import asyncio
import logging
import os
import struct
from http import HTTPStatus

from websockets.asyncio.server import serve

PATH = '/play/net'
# DCNet length-prefixes every frame, its DCNET\x01 prolog included.
PROLOG = struct.pack('<H', 6) + b'DCNET\x01'
MIN_FRAME, MAX_FRAME = 14, 1518
MAX_SESSIONS = 32
MAX_PER_IP = 4
IDLE_SECONDS = 600
MAX_BYTES = 256 * 1024 * 1024  # Per session, both directions.

log = logging.getLogger('dcvmu-net')


class Relay:
    def __init__(self, dcnet=('dcnet.flyca.st', 7655)):
        self.dcnet = dcnet
        self.sessions = {}  # Client IP: open sessions.

    def check_path(self, connection, request):
        if request.path != PATH:
            return connection.respond(HTTPStatus.NOT_FOUND, 'Not found\n')

    async def handle(self, websocket):
        ip = websocket.request.headers.get('X-Real-IP') or websocket.remote_address[0]
        if sum(self.sessions.values()) >= MAX_SESSIONS or self.sessions.get(ip, 0) >= MAX_PER_IP:
            await websocket.close(1013, 'Too many network sessions')
            return
        self.sessions[ip] = self.sessions.get(ip, 0) + 1
        try:
            await self.bridge(websocket)
        finally:
            self.sessions[ip] -= 1
            if not self.sessions[ip]:
                del self.sessions[ip]

    async def bridge(self, websocket):
        try:
            reader, writer = await asyncio.wait_for(asyncio.open_connection(*self.dcnet), 10)
        except (OSError, asyncio.TimeoutError):
            log.warning('DCNet is unreachable')
            await websocket.close(1011, 'DCNet is unreachable')
            return
        writer.write(PROLOG)
        moved = 0

        async def upstream():
            nonlocal moved
            while True:
                frame = await asyncio.wait_for(websocket.recv(), IDLE_SECONDS)
                if not isinstance(frame, bytes) or not MIN_FRAME <= len(frame) <= MAX_FRAME:
                    return
                moved += len(frame)
                if moved > MAX_BYTES:
                    return
                writer.write(struct.pack('<H', len(frame)) + frame)
                await writer.drain()

        async def downstream():
            nonlocal moved
            while True:
                size, = struct.unpack('<H', await reader.readexactly(2))
                frame = await reader.readexactly(size)
                if size < MIN_FRAME or size > MAX_FRAME:
                    continue
                moved += size
                if moved > MAX_BYTES:
                    return
                await websocket.send(frame)

        log.info('Session opened (%d active)', sum(self.sessions.values()))
        tasks = [asyncio.create_task(upstream()), asyncio.create_task(downstream())]
        try:
            await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        finally:
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            writer.close()
            await websocket.close()
            log.info('Session closed after %d bytes', moved)

    def serve(self, host, port, origins):
        # Frames are small and mostly compressed already; skip deflate's CPU.
        return serve(self.handle, host, port, origins=origins, process_request=self.check_path,
                     max_size=MAX_FRAME, compression=None)


async def main():
    host, port = os.environ.get('DCVMU_NET_LISTEN', '127.0.0.1:8092').rsplit(':', 1)
    origins = os.environ.get('DCVMU_NET_ORIGINS', 'https://dcvmu.com').split(',')
    async with Relay().serve(host, int(port), origins) as server:
        await server.serve_forever()


if __name__ == '__main__':
    logging.basicConfig(level=logging.INFO, format='%(message)s')
    # websockets logs every refused handshake (bots, other origins) as an
    # error with a traceback; that is noise in the journal.
    logging.getLogger('websockets').setLevel(logging.CRITICAL)
    asyncio.run(main())
