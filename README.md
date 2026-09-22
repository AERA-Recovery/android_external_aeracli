# AERA CLI

`aera` is the native command-line client for AERA Recovery. It creates typed
AERA RPC requests, streams structured operation events, renders human-readable
status and progress, and supports raw JSON output for automation.

This repository is an original clean-room implementation. It was designed from
the published AERA RPC wire contract and does not contain or derive from
FoxCLI source code.

## Examples

```sh
aera status
aera mount /data
aera flash /sdcard/update.zip
aera backup --parts boot,data --name before-update --compress
aera wifi connect --ssid MyNetwork --password-stdin
aera reboot fastboot
aera --json status
aera --dry-run backup --parts boot,data
```

The following command names are reserved for future native implementations but
are deliberately rejected for now: `reflash`, `addons`, `control`, `input`,
`screencap`, `adbd`, `ota`, `ors`, and `ors-cmd`. The flash options `--verify`,
`--reflash`, `--unmount-system`, and `--unmount-vendor` are likewise reserved.
The client never silently ignores an unsupported operation.

`aera call` exposes new RPC fields without requiring a client rebuild:

```sh
aera call operation --string name value --bool enabled true --int count 2
```

## Clean-room boundary

- Language and implementation: new C++ code.
- Behavioral input: AERA's documented JSON/NDJSON protocol only.
- No FoxCLI source, history, parser, tests, or implementation was imported.
- The recovery server has its own GPL history and remains licensed separately.
