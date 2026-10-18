# Troubleshooting

This guide covers common setup problems for the examples in this repository.

Commands below assume the default OpenClaw install. If you use a named profile, add `--profile <profile>` to the `openclaw` commands.

## Node Shows Capabilities but `commands: []`

This usually means one of three things:

- The gateway does not allow the example's commands
- The node connected before the allowlist was fixed and has not reconnected since
- You are looking at an older disconnected row instead of the live node session

Make sure the gateway includes the allowlist from the example README, then restart it:

```bash
openclaw config set gateway.nodes.commands.allow '<json-array-from-example>' --strict-json
openclaw gateway restart
```

If the board already has a saved reconnect session, it should reconnect
automatically once Wi-Fi and the gateway are back. Otherwise request another
connection attempt from the board or reboot it.

When you check status, use the live row:

```bash
openclaw nodes status --json
```

Look for:

- `"connected": true`
- The current `remoteIp`
- The current `nodeId`

## Setup Code Is Rejected or Pairing Does Not Complete

Common causes:

- The setup code expired
- The setup code points at the wrong gateway URL
- The board still has saved settings from an older pairing attempt

Use this recovery sequence:

1. Generate a fresh setup code:

```bash
openclaw qr \
  --url ws://<gateway-host-ip>:<gateway-port> \
  --setup-code-only
```

1. Check the board state from the REPL:

```text
status
```

1. If the board may still have an older saved session or Wi-Fi state, erase and
  reflash it:

```bash
. ~/esp-idf/export.sh
cd /path/to/example
idf.py -p <serial-port> erase-flash flash monitor
```

1. Provision again from the REPL:

```text
gateway setup-code <setup-code>
```

`gateway setup-code <setup-code>` already requests the connection attempt. It
does not require a second `gateway connect`. If Wi-Fi is still associating, the
REPL waits for the board to obtain an IP before it submits that attempt.

## Node Does Not Show Up in `openclaw nodes status`

Check both sides:

On the board:

```text
status
wifi set <ssid> <passphrase>
```

Use `wifi set <ssid>` instead if the network is open.

On the gateway host:

```bash
openclaw gateway status --probe --json
openclaw nodes status --json
```

If the board is not associated to Wi-Fi yet, fix that first. If the board is on
Wi-Fi but the gateway is not reachable, verify the `ws://<gateway-host>` URL
embedded in the setup code or passed to `gateway token`, `gateway password`, or
`gateway no-auth`.

## Pending Approvals Still Appear After Setup-Code Pairing

Fresh setup-code pairing should not normally require extra approval commands.
Manual token, password, or no-auth enrollment can leave separate device and
node requests. If either request remains pending, inspect it and approve the
request matching your board:

```bash
openclaw devices list --json
openclaw devices approve <device-request-id> --json
openclaw nodes pending --json
openclaw nodes approve <request-id> --json
```

The node request may appear only after device approval and another connection
attempt. If there is no saved session yet, repeat the original explicit auth
command on the board. Otherwise use `gateway connect`.

A connected node with advertised commands can still need node approval. If
an invocation fails with `PAIRING_CHANGED` or `node pairing changed while
invocation was active`, check `openclaw nodes pending --json`. Approve the
expected command surface before retrying the invocation.

`openclaw devices` and `openclaw nodes` do different jobs:

- `openclaw devices` works with device pairing records
- `openclaw nodes` approves the node command surface and reports live node state

## Reset Saved State on the Board

Useful REPL commands:

- `wifi clear`
- `reboot` 
- `gateway disconnect`

The examples in this repository do not expose a factory-reset or clear-saved-session REPL command.

When you need to forget both Wi-Fi and pairing state, use `idf.py erase-flash` before reflashing the example.
