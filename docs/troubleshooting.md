# Troubleshooting

## `cannot start discovery: mgmt status 0x11`

`0x11` is **Invalid Index**, not Resource Busy. An older build mislabeled all start-discovery failures as `Resource busy`. Newer builds query the MGMT controller index list. True MGMT Busy is `0x0a`.

```sh
saihang-bms --version
saihang-bms -v adapter status
saihang-bms -v scan --timeout 8
```

## Adapter becomes unresponsive

In one test, physically unplugging/replugging the USB dongle restored functionality. v2.2 implements recovery stages: stop stale discovery, `btusb` unbind/bind, and USB device deauthorization/reauthorization. **This recovery path needs confirmation on the target hardware.**

```sh
saihang-bms -v adapter status
saihang-bms adapter reset
saihang-bms adapter reset --hard
```

Hard reset is disruptive and requires root. Avoid triggering it while another Bluetooth process is using the dongle.

## `gatttool` segfaults / `btmgmt` errors

Older tests observed a `gatttool` crash and `btmgmt` command failures. The successful implementation uses raw L2CAP ATT sockets with a source bind to the ATT CID, plus MGMT control sockets for scanning, rather than relying on `gatttool` or D-Bus.

## Missing APK / old version

```sh
grep -n 'PKG_VERSION' package/saihang-bms/Makefile
find bin -type f -name 'saihang-bms*.apk'
make package/saihang-bms/clean
make package/saihang-bms/compile V=s -j1
```
