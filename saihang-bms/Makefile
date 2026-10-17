include $(TOPDIR)/rules.mk

PKG_NAME:=saihang-bms
PKG_VERSION:=2.2.0
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define Package/saihang-bms
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Saihang SH-series BLE BMS CLI
  DEPENDS:=+bluez-libs +kmod-bluetooth +kmod-btusb
endef

define Package/saihang-bms/description
 Standalone raw ATT/MGMT CLI for Saihang/Anenji/Aninerel SH-series BLE BMS.
 Supports discovery, status reading, derived cell/temperature metrics, JSON output,
 multi-BMS polling, adapter self-recovery, and read-only Modbus register exploration.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
		-o $(PKG_BUILD_DIR)/saihang-bms $(PKG_BUILD_DIR)/saihang-bms.c \
		$(TARGET_LDFLAGS) -lbluetooth -lm
endef

define Package/saihang-bms/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/saihang-bms $(1)/usr/bin/
endef

$(eval $(call BuildPackage,saihang-bms))
