# Saihang BLE protocol notes

Based on [aiobmsble Saihang implementation](https://github.com/patman15/aiobmsble) and one successful router test:

- BLE advertisement name starts `SH`; service UUID `FFFA`.
- Write characteristic `FFFB`, notification/read characteristic `FFFC`.
- Application frame begins `A5 A5`; payload is Modbus-like, with Modbus CRC16.
- Observed status request: `A5 A5 00 03 00 00 00 48 44 2D` (read 72 registers from zero).
- Working sample GATT service handles `0x0015–0x001b`, TX `0x0017`, RX `0x001a`, CCCD `0x001b`. **These are observations, not fixed constants; the program discovers handles dynamically.**
- Observed ATT MTU: 517; status response length: 151 bytes.

The BMS returned ~26.69 V, 0 A, SOC 100%, SOH 100%, eight cells around 3.33 V and two cell temperature sensors. This confirms telemetry decoding for that device, not all SH-series variants.

SOC is reported charge level; SOH is reported battery health. Both are values returned by the BMS, not independently measured by this utility. Configuration/protection threshold register locations and safe write commands remain unverified.
