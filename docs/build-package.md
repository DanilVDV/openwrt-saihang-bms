# Build OpenWrt APK package

Target previously used: MediaTek MT7688AN, MIPS 24KEc, OpenWrt 25.12.5; source directory named `~/RMS7688A/openwrt-25.12.5`. Adjust for your actual SDK/target.

```sh
cd ~/RMS7688A/openwrt-25.12.5
rm -rf package/saihang-bms
cp -a /path/to/openwrt-saihang-bms/saihang-bms package/
make package/saihang-bms/clean
make package/saihang-bms/compile V=s -j1
find bin -type f -name 'saihang-bms*.apk'
```

The package Makefile declares `+bluez-libs +kmod-bluetooth +kmod-btusb` and links with `-lbluetooth -lm`. Its actual output directory depends on the selected target, feed and OpenWrt build configuration. Inspect `package/saihang-bms/Makefile` for `PKG_VERSION:=2.2.0` and `src/saihang-bms.c` for `APP_VERSION "2.2.0"` if an older package appears.

This repository does **not** contain the full OpenWrt source tree or a tested, complete firmware `.config`.
