# LAN TLS passthrough for compact nodes

An ESP32 on Wi-Fi cannot directly reach a Gateway available only through a
private VPN. `scripts/lan_tls_relay.py` can run on a macOS host that can reach
both networks. It forwards raw TCP bytes to one fixed upstream. TLS remains
between the board and the Gateway; the relay does not decrypt traffic or
receive Gateway credentials.

The relay binds only the current IPv4 address of one configured `en` interface.
It checks the configured private subnet, DHCP router address, and router MAC
address before listening. It polls every five seconds and closes existing
forwards when that network identity changes. The router must be present in the
host's ARP cache; an absent or mismatched entry leaves the listener closed.
These checks limit accidental exposure on another network. They do not replace
TLS verification and Gateway authentication.

Create a private JSON configuration outside the repository. Replace all sample
network values with verified deployment values, and restrict the file to its
owner:

```json
{
  "interface": "en0",
  "lan_network": "10.42.0.0/24",
  "listen_port": 19443,
  "allowed_sources": ["10.42.0.20", "10.42.0.21"],
  "upstream_host": "gateway.example.test",
  "upstream_port": 443,
  "router_ip": "10.42.0.1",
  "router_mac": "02:00:00:00:00:01",
  "max_clients": 4,
  "connect_timeout": 10,
  "idle_timeout": 120
}
```

Run with Python 3.10 or later:

```sh
python3 scripts/lan_tls_relay.py --config /path/to/private/config.json
```

For a persistent deployment, install the helper in a dedicated user directory
and create a separate user LaunchAgent. Use absolute paths for Python, the
helper, and its configuration. Set `RunAtLoad`, `KeepAlive`, and a restart
throttle; do not replace an existing OpenClaw node or Gateway service. Stop the
exact LaunchAgent before changing its configuration. Keep the relay host awake
while the boards need it; sleep disconnects them.

The board's setup URL can use the relay host's stable `.local` name and listener
port. In the board's untracked local `sdkconfig`, set
`CONFIG_EXAMPLE_GATEWAY_TLS_SERVER_NAME` to the upstream certificate's DNS name.
Keep certificate bundle, date, and name verification enabled. Wait for the
board's clock-synchronized log before its first setup attempt. Use a node-only
bootstrap profile when the Gateway supports it.

Source addresses are exact and are never learned automatically. If a board's
DHCP lease changes, identify that board again and update the private allowlist.
The relay host may change address within the pinned LAN; it closes the old
listener before rebinding. Boards with a saved session retry after the relay
returns. Gateway authentication and command policy still control each node.

Local tests exercise opaque byte forwarding, half-close behavior, source
rejection, connection limits, idle expiry, listener shutdown, stale callbacks,
and network identity mismatches:

```sh
python3 -B -m unittest discover -s scripts/tests -p test_lan_tls_relay.py -v
```
