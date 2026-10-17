# OpenWrt Saihang BLE BMS

A standalone C CLI and OpenWrt package for Saihang SH-series BLE BMS (including observed `SH24100-H-001`), using Linux Bluetooth MGMT and raw ATT rather than D-Bus. **Current source: v2.2.0.**

The code and documentation are a reconstruction of a development conversation. The original raw ATT reader was successfully tested on MT7688AN/OpenWrt; **the packaged v2.2 recovery and scanner have not yet been confirmed on the target router**.

## Quick start

See [OpenWrt package build](docs/build-package.md), [firmware and kernel configuration](docs/firmware.md), [installation and usage](docs/installation.md), [troubleshooting](docs/troubleshooting.md), and [project history](CHANGELOG.md).

The OpenWrt package is in [`saihang-bms/`](saihang-bms/). Install it by copying this directory into `<openwrt>/package/saihang-bms`, then running `make package/saihang-bms/compile V=s -j1`.

## Safety and limitations

- Telemetry and arbitrary **read-only** Modbus function 3/4 register queries are supported. Protection thresholds and configuration writes are **not decoded or implemented**.
- `cells.min_v` and `cells.max_v` represent measured cell voltages, not programmed under/over-voltage thresholds.
- Adapter reset commands affect a USB Bluetooth interface/device and require root privileges. Do not use on a shared USB device without reviewing the sysfs target.
- No warranty of correctness of SOC/SOH, computed energy, or fault-bit semantics. Do not use this tool as a safety-critical battery protection system.

## Attribution

Protocol mapping based on the publicly available Saihang driver in [patman15/aiobmsble](https://github.com/patman15/aiobmsble). BlueZ/Linux Bluetooth API documentation and kernel sources are also relevant. See [protocol notes](docs/protocol.md).

## License

License choice is pending. Do not assume that this repository is open-source licensed until a LICENSE file is added and any third-party provenance is reviewed.
