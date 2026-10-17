# Changelog (reconstructed from conversation)

## 2.2.0 — source prepared, hardware verification pending
- Adapter status and soft/hard reset commands.
- Automatic recovery for MGMT/ATT operations; configurable retries and delay.
- Dynamic sysfs USB interface/device discovery.

## 2.1.0 — source prepared, hardware verification pending
- Controller index discovery through MGMT rather than assuming index zero.
- Correct decoding of MGMT statuses (`0x11` Invalid Index; `0x0a` Busy).

## 2.0.0 — source prepared, hardware verification pending
- MGMT BLE scanning, scan-all and multi-device read-all.
- JSON logical groups, computed cell statistics, temperatures, raw register reads, NDJSON watch.
- Read-only Modbus register exploration and capability reporting.

## 1.x — experimental prototype
- Raw ATT connection and dynamic FFFA/FFFB/FFFC discovery.
- Successfully queried one SH24100-H BMS from OpenWrt; received a valid 151-byte response with telemetry.

These entries document project milestones, **not Git commits that already exist**. Create Git tags only after confirming the corresponding source and release policy.
