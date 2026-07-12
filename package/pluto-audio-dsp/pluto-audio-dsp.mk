################################################################################
#
# pluto-audio-dsp
#
################################################################################

PLUTO_AUDIO_DSP_SITE = $(TOPDIR)/board/pluto/pluto-audio-dsp
PLUTO_AUDIO_DSP_SITE_METHOD = local
PLUTO_AUDIO_DSP_DEPENDENCIES = libiio liquid-dsp
PLUTO_AUDIO_DSP_LICENSE = MIT

define PLUTO_AUDIO_DSP_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -std=c99 -Wall -Wextra -Os \
		-o $(@D)/pluto-audio-backend $(@D)/pluto-audio-backend.c \
		$(TARGET_LDFLAGS) -liio -lliquid -lm
	$(TARGET_CC) $(TARGET_CFLAGS) -std=c99 -Wall -Wextra -Os \
		-o $(@D)/pluto-loopback-backend $(@D)/pluto-loopback-backend.c \
		$(TARGET_LDFLAGS) -liio -lm
	$(TARGET_CC) $(TARGET_CFLAGS) -std=c99 -Wall -Wextra -Os \
		-o $(@D)/pluto-spectrum-backend $(@D)/pluto-spectrum-backend.c \
		$(TARGET_LDFLAGS) -liio -lm
endef

define PLUTO_AUDIO_DSP_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/pluto-audio-backend \
		$(TARGET_DIR)/usr/sbin/pluto-audio-backend
	$(INSTALL) -D -m 0755 $(@D)/pluto-loopback-backend \
		$(TARGET_DIR)/usr/sbin/pluto-loopback-backend
	$(INSTALL) -D -m 0755 $(@D)/pluto-spectrum-backend \
		$(TARGET_DIR)/usr/sbin/pluto-spectrum-backend
	ln -sf pluto-loopback-backend \
		$(TARGET_DIR)/usr/sbin/pluto-tx-backend
endef

$(eval $(generic-package))
