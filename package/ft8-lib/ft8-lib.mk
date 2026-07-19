################################################################################
#
# ft8-lib
#
################################################################################

FT8_LIB_VERSION = 9fec6ca39886edbf96f4f5e71edc76da5074e871
FT8_LIB_SITE = $(call github,kgoba,ft8_lib,$(FT8_LIB_VERSION))
FT8_LIB_LICENSE = MIT
FT8_LIB_LICENSE_FILES = LICENSE
FT8_LIB_INSTALL_STAGING = YES

FT8_LIB_CFLAGS = $(TARGET_CFLAGS) -std=c99 -O3 -D_GNU_SOURCE -DHAVE_STPCPY -I$(@D)
FT8_LIB_DECODER_SOURCES = \
	ft8/constants.c \
	ft8/crc.c \
	ft8/decode.c \
	ft8/encode.c \
	ft8/ldpc.c \
	ft8/message.c \
	ft8/text.c \
	common/monitor.c \
	fft/kiss_fft.c \
	fft/kiss_fftr.c

define FT8_LIB_BUILD_CMDS
	rm -rf $(@D)/.build
	mkdir -p $(@D)/.build
	$(foreach source,$(FT8_LIB_DECODER_SOURCES),\
		$(TARGET_CC) $(FT8_LIB_CFLAGS) -c $(@D)/$(source) \
			-o $(@D)/.build/$(notdir $(source:.c=.o));)
	$(TARGET_AR) rcs $(@D)/libft8.a $(@D)/.build/*.o
endef

define FT8_LIB_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(@D)/libft8.a \
		$(STAGING_DIR)/usr/lib/libft8.a
	$(INSTALL) -d $(STAGING_DIR)/usr/include/ft8 \
		$(STAGING_DIR)/usr/include/common $(STAGING_DIR)/usr/include/fft
	$(INSTALL) -m 0644 $(@D)/ft8/*.h $(STAGING_DIR)/usr/include/ft8/
	$(INSTALL) -m 0644 $(@D)/common/common.h $(@D)/common/monitor.h \
		$(STAGING_DIR)/usr/include/common/
	$(INSTALL) -m 0644 $(@D)/fft/*.h $(STAGING_DIR)/usr/include/fft/
endef

$(eval $(generic-package))
