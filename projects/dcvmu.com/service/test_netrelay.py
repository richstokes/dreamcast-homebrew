"""The web player's DCNet relay: framing, limits and refusals, against a fake DCNet."""
import asyncio
import struct
import unittest

from websockets.asyncio.client import connect
from websockets.exceptions import ConnectionClosed, InvalidStatus

import netrelay

ORIGIN = 'https://dcvmu.test'


def frame(size, fill=b'x'):
    return (fill * size)[:size]


class FakeDCNet:
    """Records what the relay sends and lets a test push frames back."""

    def __init__(self):
        self.received = bytearray()
        self.writers = []
        self.data = asyncio.Event()

    async def start(self):
        self.server = await asyncio.start_server(self.client, '127.0.0.1', 0)
        return self.server.sockets[0].getsockname()[:2]

    async def client(self, reader, writer):
        self.writers.append(writer)
        while chunk := await reader.read(65536):
            self.received += chunk
            self.data.set()

    async def wait_for(self, size):
        while len(self.received) < size:
            self.data.clear()
            await asyncio.wait_for(self.data.wait(), 2)

    def send(self, payload):
        self.writers[-1].write(payload)


class RelayTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.dcnet = FakeDCNet()
        self.relay = netrelay.Relay(await self.dcnet.start())
        self.server = await self.relay.serve('127.0.0.1', 0, [ORIGIN]).__aenter__()
        self.url = f'ws://127.0.0.1:{self.server.sockets[0].getsockname()[1]}/play/net'

    async def asyncTearDown(self):
        self.server.close()
        await self.server.wait_closed()
        self.dcnet.server.close()
        for writer in self.dcnet.writers:
            writer.close()
        await asyncio.gather(*(w.wait_closed() for w in self.dcnet.writers), return_exceptions=True)

    def open(self, ip='192.0.2.1', url=None, origin=ORIGIN):
        return connect(url or self.url, origin=origin, additional_headers={'X-Real-IP': ip})

    async def test_frames_both_ways(self):
        async with self.open() as ws:
            await ws.send(frame(60))
            await self.dcnet.wait_for(8 + 62)
            self.assertEqual(bytes(self.dcnet.received[:8]), netrelay.PROLOG)
            self.assertEqual(bytes(self.dcnet.received[8:]), struct.pack('<H', 60) + frame(60))
            # Two frames in one TCP segment still arrive as two messages.
            self.dcnet.send(struct.pack('<H', 42) + frame(42, b'a') + struct.pack('<H', 1514) + frame(1514, b'b'))
            self.assertEqual(await ws.recv(), frame(42, b'a'))
            self.assertEqual(await ws.recv(), frame(1514, b'b'))

    async def test_bad_frames_end_the_session(self):
        for bad in (frame(netrelay.MAX_FRAME + 1), frame(4), 'text'):
            async with self.open() as ws:
                await ws.send(bad)
                with self.assertRaises(ConnectionClosed):
                    await asyncio.wait_for(ws.recv(), 2)
        # The relay finishes its own cleanup just after the client sees the close.
        for _ in range(100):
            if not self.relay.sessions:
                break
            await asyncio.sleep(0.01)
        self.assertEqual(self.relay.sessions, {})

    async def test_refuses_other_paths_and_origins(self):
        with self.assertRaises(InvalidStatus) as cm:
            async with self.open(url=self.url.replace('/play/net', '/')):
                pass
        self.assertEqual(cm.exception.response.status_code, 404)
        with self.assertRaises(InvalidStatus) as cm:
            async with self.open(origin='https://elsewhere.test'):
                pass
        self.assertEqual(cm.exception.response.status_code, 403)

    async def test_session_limit_per_ip(self):
        sockets = [await self.open() for _ in range(netrelay.MAX_PER_IP)]
        try:
            await sockets[-1].send(frame(60))
            await self.dcnet.wait_for(len(netrelay.PROLOG) * netrelay.MAX_PER_IP)
            async with self.open() as extra:
                with self.assertRaises(ConnectionClosed):
                    await asyncio.wait_for(extra.recv(), 2)
                self.assertEqual(extra.close_code, 1013)
            async with self.open(ip='192.0.2.2') as other:
                await other.send(frame(60))
        finally:
            for ws in sockets:
                await ws.close()

    async def test_dcnet_unreachable(self):
        self.dcnet.server.close()
        await self.dcnet.server.wait_closed()
        async with self.open() as ws:
            with self.assertRaises(ConnectionClosed):
                await asyncio.wait_for(ws.recv(), 12)
            self.assertEqual(ws.close_code, 1011)


if __name__ == '__main__':
    unittest.main()
