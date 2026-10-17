# saihang-bms 2.2.0

Standalone OpenWrt CLI for Saihang Technology SH-series BLE BMS used in Anenji / Aninerel batteries.

## Commands

```sh
saihang-bms scan --timeout 10
saihang-bms -p read B2:60:30:30:02:32
saihang-bms read-all --timeout 8
saihang-bms -r registers B2:60:30:30:02:32 0 72
saihang-bms capabilities
```

`stdout` is JSON. Diagnostics go to `stderr`, making the tool suitable for shell scripts, cron, Python `subprocess`, MQTT bridges, HTTP/LuCI backends, etc.

## JSON groups

`read` returns logical groups:

- `device`: address, address type, optional name
- `pack`: voltage/current/power/SOC/SOH/charging/runtime estimate
- `capacity`: remaining/design Ah, estimated Wh, cycles, SOC consistency
- `status`: problem code/bits, charge/discharge MOSFET, balancer raw state
- `cells`: voltages, sum/average/min/max/delta, cell indexes, stddev, deviations
- `temperatures`: cell sensors, min/max/average/delta, MOSFET and ambient temperatures
- `protocol`: UUIDs, handles, MTU, frame/CRC metadata
- `raw`: optional with `--raw`

## Read-only register exploration

`registers` sends only Modbus function 3 or 4 requests wrapped in the Saihang `A5 A5` framing. It deliberately does not implement arbitrary write functions. The public Saihang implementation used as protocol reference does not publish a verified write register map for protection thresholds/settings.

## Shell example

OpenWrt normally includes `jsonfilter`:

```sh
V=$(saihang-bms read B2:60:30:30:02:32 | jsonfilter -e '@.pack.voltage_v')
D=$(saihang-bms read B2:60:30:30:02:32 | jsonfilter -e '@.cells.delta_mv')
echo "Voltage=$V V delta=$D mV"
```

## Python example

See `examples/read.py`.

## Continuous polling / NDJSON

```sh
saihang-bms watch --interval 5 B2:60:30:30:02:32
saihang-bms watch --interval 10 --count 6 B2:60:30:30:02:32
```

`watch` emits one JSON object per poll (NDJSON), convenient for `while read`, Python streaming parsers, log collectors, or MQTT publisher scripts.

## Adapter recovery (2.2)

The CLI automatically recovers transient MGMT/ATT failures by stopping stale discovery, then (if necessary) re-binding `btusb`, and finally USB deauthorize/reauthorize as a hard fallback.

```sh
saihang-bms adapter status
saihang-bms adapter reset
saihang-bms adapter reset --hard
saihang-bms --no-recover read-all --timeout 8
saihang-bms --recover-retries 1 read-all --timeout 8
```

Recovery discovers the active HCI controller and USB device/interface through MGMT/sysfs instead of hardcoding a USB port or dongle MAC.
