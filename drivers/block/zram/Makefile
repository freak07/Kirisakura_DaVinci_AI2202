# SPDX-License-Identifier: GPL-2.0-only
zram-y	:=	zcomp.o zram_drv.o

obj-$(CONFIG_ZRAM)	+=	zram.o

ifneq (,$(filter AI2202,$(ASUS_BUILD_PROJECT)))
obj-$(CONFIG_EXPANDMEM)	+= expandmem/
endif
