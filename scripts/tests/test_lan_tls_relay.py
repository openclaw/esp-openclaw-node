"""Exercise opaque forwarding, admission limits, and LAN identity checks locally."""

import asyncio
from dataclasses import replace
import importlib.util
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest
from unittest.mock import AsyncMock, Mock, patch

SPEC = importlib.util.spec_from_file_location("lan_tls_relay", Path(__file__).resolve().parents[1] / "lan_tls_relay.py")
relay = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = relay
SPEC.loader.exec_module(relay)


def config(**changes):
    base = relay.Config(interface="en0", lan_network="10.1.2.0/24", listen_port=19443,
                        allowed_sources=("127.0.0.1",), upstream_host="127.0.0.1",
                        router_ip="10.1.2.1", router_mac="02:00:00:00:00:01")
    return replace(base, **changes)


class NetworkIdentityTests(unittest.TestCase):
    def address(self, ip="10.1.2.5", router="10.1.2.1", mac="2:0:0:0:0:1", interface="en0"):
        replies = [SimpleNamespace(returncode=0, stdout=ip),
                   SimpleNamespace(returncode=0, stdout=router),
                   SimpleNamespace(returncode=0, stdout=f"? (10.1.2.1) at {mac} on {interface} ifscope [ethernet]")]
        with patch.object(relay.subprocess, "run", side_effect=replies):
            return relay.interface_address(config())

    def test_only_pinned_network_is_admitted(self):
        self.assertEqual(self.address(), "10.1.2.5")
        self.assertIsNone(self.address(ip="10.2.3.4"))
        self.assertIsNone(self.address(ip="0.0.0.0"))
        self.assertIsNone(self.address(ip="10.1.2.255"))
        self.assertIsNone(self.address(router="10.1.2.2"))
        self.assertIsNone(self.address(mac="02:00:00:00:00:02"))
        self.assertIsNone(self.address(mac="incomplete"))
        self.assertIsNone(self.address(interface="en1"))


class ForwardingTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.upstream_count = 0

        async def echo(reader, writer):
            self.upstream_count += 1
            try:
                while data := await reader.read(65536):
                    writer.write(data)
                    await writer.drain()
            finally:
                await relay.close_writer(writer)

        self.upstream = await asyncio.start_server(echo, "127.0.0.1", 0)
        port = self.upstream.sockets[0].getsockname()[1]
        self.relay = relay.Relay(config(listen_port=0, upstream_port=port))
        await self.relay.listen("127.0.0.1")
        self.port = self.relay.server.sockets[0].getsockname()[1]

    async def asyncTearDown(self):
        await self.relay.stop_listener()
        self.upstream.close()
        await self.upstream.wait_closed()

    async def connect(self):
        return await asyncio.open_connection("127.0.0.1", self.port)

    async def test_forwards_opaque_bytes_and_half_close(self):
        reader, writer = await self.connect()
        payload = bytes(range(256)) * 1024
        writer.write(payload)
        await writer.drain()
        writer.write_eof()
        self.assertEqual(await asyncio.wait_for(reader.readexactly(len(payload)), 2), payload)
        self.assertEqual(await asyncio.wait_for(reader.read(), 2), b"")
        await relay.close_writer(writer)

    async def test_source_rejected_before_upstream_connect(self):
        self.relay.config = replace(self.relay.config, allowed_sources=("127.0.0.2",))
        reader, writer = await self.connect()
        self.assertEqual(await asyncio.wait_for(reader.read(), 1), b"")
        self.assertEqual(self.upstream_count, 0)
        await relay.close_writer(writer)

    async def test_old_listener_callback_cannot_open_an_upstream(self):
        old_generation = self.relay.generation
        await asyncio.wait_for(self.relay.listen("127.0.0.1"), 2)
        writer = Mock()
        writer.get_extra_info.return_value = ("127.0.0.1", 1234)
        with patch.object(relay, "close_writer", new_callable=AsyncMock) as close:
            await self.relay.handle_client(Mock(), writer, old_generation)
            close.assert_awaited_once_with(writer)
        self.assertEqual(self.upstream_count, 0)

    async def test_client_limit_preserves_first_connection(self):
        self.relay.config = replace(self.relay.config, max_clients=1)
        first, writer = await self.connect()
        writer.write(b"first")
        await writer.drain()
        self.assertEqual(await first.readexactly(5), b"first")
        denied, rejected = await self.connect()
        self.assertEqual(await asyncio.wait_for(denied.read(), 1), b"")
        self.assertEqual(self.upstream_count, 1)
        writer.write(b"alive")
        await writer.drain()
        self.assertEqual(await first.readexactly(5), b"alive")
        await relay.close_writer(rejected)
        await relay.close_writer(writer)

    async def test_idle_connection_and_network_departure_close(self):
        self.relay.config = replace(self.relay.config, idle_timeout=0.05)
        reader, writer = await self.connect()
        self.assertEqual(await asyncio.wait_for(reader.read(), 1), b"")
        await relay.close_writer(writer)
        self.relay.config = replace(self.relay.config, idle_timeout=120)
        reader, writer = await self.connect()
        writer.write(b"connected")
        await writer.drain()
        self.assertEqual(await reader.readexactly(9), b"connected")
        await asyncio.wait_for(self.relay.listen(None), 2)
        self.assertEqual(await asyncio.wait_for(reader.read(), 1), b"")
        self.assertFalse(self.relay.clients)
        self.assertIsNone(self.relay.server)
        await relay.close_writer(writer)


if __name__ == "__main__":
    unittest.main()
