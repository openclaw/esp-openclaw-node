#!/usr/bin/env python3
"""Forward opaque TLS from explicitly allowed LAN nodes to one fixed upstream."""

import argparse
import asyncio
from dataclasses import dataclass
import ipaddress
import json
import logging
import re
import subprocess
import time


LOG = logging.getLogger("lan_tls_relay")
PRIVATE_LANS = tuple(ipaddress.ip_network(cidr) for cidr in
                     ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16"))


@dataclass(frozen=True)
class Config:
    interface: str
    lan_network: str
    listen_port: int
    allowed_sources: tuple[str, ...]
    upstream_host: str
    router_ip: str
    router_mac: str
    upstream_port: int = 443
    max_clients: int = 4
    connect_timeout: float = 10
    idle_timeout: float = 120

    @classmethod
    def load(cls, path):
        with open(path, encoding="utf-8") as stream:
            config = cls(**json.load(stream))
        network = ipaddress.ip_network(config.lan_network)
        if network.version != 4 or not any(network.subnet_of(lan) for lan in PRIVATE_LANS):
            raise ValueError("lan_network must be an explicit RFC1918 IPv4 subnet")
        if not re.fullmatch(r"en[0-9]+", config.interface):
            raise ValueError("interface must name a macOS Ethernet or Wi-Fi interface")
        if not 1024 <= config.listen_port <= 65535 or not 1 <= config.upstream_port <= 65535:
            raise ValueError("invalid port")
        if not config.allowed_sources or len(config.allowed_sources) > 32:
            raise ValueError("require between 1 and 32 exact source addresses")
        for source in config.allowed_sources:
            address = ipaddress.ip_address(source)
            if address not in network or address in (network.network_address, network.broadcast_address):
                raise ValueError("source must be a host address within lan_network")
        if ipaddress.ip_address(config.router_ip) not in network or normalize_mac(config.router_mac) is None:
            raise ValueError("require a router address and MAC identity for this LAN")
        if not re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9.-]{0,251}[A-Za-z0-9])?", config.upstream_host):
            raise ValueError("invalid upstream hostname")
        if not 1 <= config.max_clients <= 32 or not 0 < config.connect_timeout <= 60:
            raise ValueError("invalid client or connect limit")
        if not 1 <= config.idle_timeout <= 3600:
            raise ValueError("invalid idle timeout")
        return config


def normalize_mac(value):
    if not re.fullmatch(r"(?:[0-9a-fA-F]{1,2}:){5}[0-9a-fA-F]{1,2}", value):
        return None
    return ":".join(f"{int(part, 16):02x}" for part in value.split(":"))


def interface_address(config):
    result = subprocess.run(["/usr/sbin/ipconfig", "getifaddr", config.interface],
                            capture_output=True, text=True, timeout=3, check=False)
    try:
        address = ipaddress.ip_address(result.stdout.strip())
        network = ipaddress.ip_network(config.lan_network)
        if (result.returncode == 0 and address.version == 4 and address in network and
                address not in (network.network_address, network.broadcast_address)):
            router = subprocess.run(["/usr/sbin/ipconfig", "getoption", config.interface, "router"],
                                    capture_output=True, text=True, timeout=3, check=False)
            if router.returncode != 0 or router.stdout.strip() != config.router_ip:
                return None
            arp = subprocess.run(["/usr/sbin/arp", "-n", config.router_ip],
                                 capture_output=True, text=True, timeout=3, check=False)
            match = re.search(r"\) at ([0-9a-fA-F:]+) on (\S+)", arp.stdout)
            if (arp.returncode == 0 and match and match[2] == config.interface and
                    normalize_mac(match[1]) == normalize_mac(config.router_mac)):
                return str(address)
    except ValueError:
        pass
    return None


async def close_writer(writer):
    writer.close()
    try:
        await asyncio.wait_for(writer.wait_closed(), 3)
    except (OSError, asyncio.TimeoutError):
        writer.transport.abort()


class Relay:
    def __init__(self, config):
        self.config = config
        self.clients = set()
        self.server = None
        self.address = None
        self.generation = 0
        self.accepting = False

    async def handle_client(self, reader, writer, generation):
        peer = writer.get_extra_info("peername")
        if (not self.accepting or generation != self.generation or
                not peer or peer[0] not in self.config.allowed_sources or
                len(self.clients) >= self.config.max_clients):
            await close_writer(writer)
            return
        task = asyncio.current_task()
        self.clients.add(task)
        upstream = None
        operations = []
        try:
            upstream_reader, upstream = await asyncio.wait_for(
                asyncio.open_connection(self.config.upstream_host, self.config.upstream_port),
                self.config.connect_timeout)
            last_activity = time.monotonic()

            async def copy(source, target):
                nonlocal last_activity
                while data := await source.read(65536):
                    last_activity = time.monotonic()
                    target.write(data)
                    await asyncio.wait_for(target.drain(), self.config.idle_timeout)
                if target.can_write_eof():
                    target.write_eof()

            async def idle_watchdog():
                while True:
                    remaining = self.config.idle_timeout - (time.monotonic() - last_activity)
                    if remaining <= 0:
                        return
                    await asyncio.sleep(remaining)

            transfers = asyncio.gather(copy(reader, upstream), copy(upstream_reader, writer))
            watchdog = asyncio.create_task(idle_watchdog())
            operations = [transfers, watchdog]
            done, _ = await asyncio.wait(operations, return_when=asyncio.FIRST_COMPLETED)
            for completed in done:
                completed.result()
        except (OSError, asyncio.TimeoutError):
            LOG.info("connection ended or upstream unavailable")
        finally:
            for operation in operations:
                operation.cancel()
            await asyncio.gather(*operations, return_exceptions=True)
            if upstream is not None:
                await close_writer(upstream)
            await close_writer(writer)
            self.clients.discard(task)

    async def stop_listener(self):
        self.accepting = False
        self.generation += 1
        server, self.server = self.server, None
        if server is not None:
            server.close()
        clients = list(self.clients)
        for task in clients:
            task.cancel()
        await asyncio.wait_for(asyncio.gather(*clients, return_exceptions=True), 8)
        if server is not None:
            await asyncio.wait_for(server.wait_closed(), 4)
        self.address = None

    async def listen(self, address):
        await self.stop_listener()
        if address is not None:
            generation = self.generation
            self.server = await asyncio.start_server(
                lambda reader, writer: self.handle_client(reader, writer, generation),
                address, self.config.listen_port, backlog=self.config.max_clients, start_serving=False)
            self.address = address
            self.accepting = True
            await self.server.start_serving()
            LOG.info("LAN listener active")
        else:
            LOG.info("LAN interface unavailable or outside configured subnet")

    async def run(self):
        try:
            while True:
                try:
                    address = await asyncio.to_thread(interface_address, self.config)
                except (OSError, subprocess.TimeoutExpired):
                    address = None
                if address != self.address:
                    await self.listen(address)
                await asyncio.sleep(5)
        finally:
            await self.stop_listener()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, help="private deployment JSON file")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    asyncio.run(Relay(Config.load(args.config)).run())


if __name__ == "__main__":
    main()
