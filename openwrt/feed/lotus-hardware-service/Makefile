include $(TOPDIR)/rules.mk
PKG_NAME:=lotus-hardware-service
PKG_VERSION:=0.1.0
PKG_RELEASE:=8
PKG_LICENSE:=GPL-3.0-only
PKG_BUILD_PARALLEL:=1
PKG_FILE_DEPENDS:=$(CURDIR)/src/source.tar.gz
include $(INCLUDE_DIR)/package.mk
define Package/lotus-hardware-service
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Local Lotus Mini hardware control service
  DEPENDS:=+libstdcpp +alsa-lib
endef
define Package/lotus-hardware-service/conffiles
/etc/config/lotus_hardware
/etc/lotus-hardware/
endef
define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)
	tar -xzf $(CURDIR)/src/source.tar.gz -C $(PKG_BUILD_DIR)
endef
define Build/Configure
endef
define Build/Compile
	+$(MAKE) -C $(PKG_BUILD_DIR) build/lotus-hardwared-alsa build/lotus-hwctl CXX="$(TARGET_CXX)" CPPFLAGS="-isystem third_party/picojson -isystem $(STAGING_DIR)/usr/include" LDFLAGS="-L$(STAGING_DIR)/usr/lib"
endef
define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include/lotus-hardware
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/src/protocol.h $(PKG_BUILD_DIR)/src/connection.h $(PKG_BUILD_DIR)/src/application_client.h $(PKG_BUILD_DIR)/src/privacy.h $(PKG_BUILD_DIR)/third_party/picojson/picojson.h $(1)/usr/include/lotus-hardware/
endef
define Package/lotus-hardware-service/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/usr/bin $(1)/etc/config $(1)/etc/init.d $(1)/etc/lotus-hardware $(1)/usr/share/lotus-hardware
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/build/lotus-hardwared-alsa $(1)/usr/sbin/lotus-hardwared
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/build/lotus-hwctl $(1)/usr/bin/lotus-hwctl
	$(INSTALL_BIN) ./files/lotus_hardware.init $(1)/etc/init.d/lotus_hardware
	$(INSTALL_CONF) ./files/lotus_hardware.config $(1)/etc/config/lotus_hardware
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/profiles/lotus-mini-playback.conf $(1)/usr/share/lotus-hardware/playback.conf
	chmod 700 $(1)/etc/lotus-hardware
endef
define Package/lotus-hardware-service/postinst
#!/bin/sh
if [ "$$PKG_UPGRADE" != "1" ]; then
  if [ -n "$$IPKG_INSTROOT" ]; then
    /bin/sh "$$IPKG_INSTROOT/etc/rc.common" "$$IPKG_INSTROOT/etc/init.d/lotus_hardware" disable
  else
    /etc/init.d/lotus_hardware disable
  fi
fi
exit 0
endef
$(eval $(call BuildPackage,lotus-hardware-service))
