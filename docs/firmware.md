# Firmware, kernel and Bluetooth setup

## Known hardware/software observations

- Board/SoC: MediaTek MT7688AN, MIPS 24KEc; OpenWrt 25.12.5 build environment.
- USB adapter `1131:1004` was visible on USB but did **not** yield a working `hci` device in early tests.
- USB adapter `2c4e:0115` (MERCUSYS) created `hci0`; `bluetoothctl show` reported `Powered: yes` and LE support.
- The working adapter used Realtek firmware files reported in kernel logs: `rtl_bt/rtl8761bu_fw.bin` and `rtl_bt/rtl8761bu_config.bin`.
- Installed BlueZ userland packages included `bluez-daemon`, `bluez-libs`, `bluez-utils`, `bluez-utils-btmon`, `bluez-utils-extra`.

## Kernel/firmware checklist (verify in your tree)

Use `make menuconfig` and, if available, `make kernel_menuconfig` to verify USB host/controller support, USB Bluetooth HCI support (`kmod-bluetooth`, `kmod-btusb`), Bluetooth LE and required Realtek firmware. Ensure the USB host controller actually enumerates the dongle. Package names and kernel symbols vary by OpenWrt release; **these are a checklist, not a recovered exact historical .config**.

Capture the actual build configuration for reproducibility:

```sh
cd ~/RMS7688A/openwrt-25.12.5
cp .config /safe/location/openwrt-config-25.12.5.txt
./scripts/diffconfig.sh > /safe/location/openwrt-diffconfig-25.12.5.txt
# Kernel config location varies; inspect build_dir/target-*/linux-*/linux-*/.config
find build_dir -path '*/linux-*/.config' -print
```

Before publishing `.config`, check it for secrets, private feed URLs and custom credentials. Prefer publishing a reviewed `diffconfig` and a hardware/firmware manifest.

## Runtime checks

```sh
lsusb
ls -l /sys/class/bluetooth/
dmesg | grep -Ei 'bluetooth|btusb|rtl|firmware'
bluetoothctl show
```

`btmgmt` failed with `Unable to send index_list cmd` in an early environment despite working `bluetoothctl`; this discrepancy is recorded, not resolved conclusively.
