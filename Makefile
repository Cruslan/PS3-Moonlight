ifneq ($(wildcard $(CURDIR)/ps3dev),)
export PS3DEV	?= $(CURDIR)/ps3dev
else ifneq ($(wildcard /home/cruslan/Templates/Projects/upstream-sdk/ps3dev),)
export PS3DEV	?= /home/cruslan/Templates/Projects/upstream-sdk/ps3dev
else ifneq ($(wildcard /home/cruslan/Templates/Projects/ps3dev),)
export PS3DEV	?= /home/cruslan/Templates/Projects/ps3dev
else
export PS3DEV	?= /mnt/nvme1n1p1/Build/ps3dev
endif
export PSL1GHT	?= $(PS3DEV)

TITLE		:= Moonlight PS3
APPID		:= MNLT00001
CONTENTID	:= UP0001-$(APPID)_00-0000000000000000
ICON0		:= ICON0.PNG
SFOXML		:= sfo.xml
SFOXML_DEBUG	:= sfo_debug.xml

SDK_RELEASE	:= nightly-2026-07-26
HOST_OS		:= $(shell uname -s)
HOST_ARCH	:= $(shell uname -m)

ifeq ($(HOST_OS)-$(HOST_ARCH),Darwin-arm64)
SDK_ASSET	:= ps3dev-macos-ARM64.tar.gz
else ifeq ($(HOST_OS)-$(HOST_ARCH),Darwin-x86_64)
SDK_ASSET	:= ps3dev-macos-X64.tar.gz
else ifeq ($(HOST_OS)-$(HOST_ARCH),Linux-x86_64)
SDK_ASSET	:= ps3dev-linux-X64.tar.gz
else
$(error Unsupported build host: $(HOST_OS)-$(HOST_ARCH))
endif

SDK_URL		:= https://github.com/ps3dev/ps3dev/releases/download/$(SDK_RELEASE)/$(SDK_ASSET)
SDK_TAR		:= $(SDK_ASSET)

-include $(PS3DEV)/ppu_rules

CC		:= $(PS3DEV)/ppu/bin/ppu-gcc

TARGET			:= moonlight-ps3
TARGET_DEBUG		:= $(TARGET)-debug
BUILD			:= build
TIMESTAMP		:= $(shell date +%Y%m%d_%H%M%S)
OUT_PKG			:= $(BUILD)/$(TARGET)-$(TIMESTAMP).pkg
OUT_GNPDRM_PKG		:= $(BUILD)/$(TARGET)-$(TIMESTAMP).gnpdrm.pkg
OUT_DEBUG_PKG		:= $(BUILD)/$(TARGET)-debug-$(TIMESTAMP).pkg
OUT_DEBUG_GNPDRM_PKG	:= $(BUILD)/$(TARGET)-debug-$(TIMESTAMP).gnpdrm.pkg

CFILES			:= src/main.c src/ui.c src/video.c src/ps3_compat.c src/random.c src/net_logger.c src/openssl_compat.c src/connection.c src/input.c src/audio.c src/handshake.c
OFILES			:= $(CFILES:.c=.o)
DEBUG_OFILES		:= $(patsubst src/%.c, $(BUILD)/obj_debug/%.o, $(CFILES))

CFLAGS			+= -mcpu=cell -O2 -Wall -Wextra -Werror=implicit-function-declaration -MMD -MP -I$(PS3DEV)/ppu/include -I$(PS3DEV)/portlibs/ppu/include -I$(PS3DEV)/portlibs/ppu/include/freetype2 -I./src -I./third_party/moonlight-common-c/src -I./third_party/opus/include -include src/openssl_compat.h -fno-lto
LDFLAGS     	+= -fno-lto -Wl,--no-undefined -Wl,--as-needed

# Minimal & clean: Debug build instruments all functions and enables ps3perf
DEBUG_CFLAGS		:= $(CFLAGS) -g -finstrument-functions -DENABLE_PS3PERF=1 -DENABLE_NETWORK_LOGGING=1

LIBS			:= -L$(PS3DEV)/ppu/lib -L$(PS3DEV)/portlibs/ppu/lib -L./third_party/moonlight-common-c -lmoonlight-common-c -L./third_party/opus -lopus -lfreetype -lfont3d -ltiny3d -lvdec -lsysmodule -lsysutil -lio -lrsx -lgcm_sys -lnet -lnetctl -laudio -lcurl -lpolarssl -lrt -lm -lz
DEBUG_LIBS		:= $(LIBS) -lps3perf

BUILDDIR		:= $(CURDIR)/$(BUILD)

all: pkg

LIBCOMMON		:= third_party/moonlight-common-c/libmoonlight-common-c.a
LIBOPUS			:= third_party/opus/libopus.a

$(LIBCOMMON): FORCE
	$(MAKE) -C third_party/moonlight-common-c -f Makefile.ps3 CC=$(abspath $(PS3DEV))/ppu/bin/ppu-gcc AR=$(abspath $(PS3DEV))/ppu/bin/ppu-ar

$(LIBOPUS): FORCE
	CFLAGS="-mcpu=cell -O2 -Wall -fno-lto -DHAVE_LRINTF -DUSE_ALLOCA" $(MAKE) -C third_party/opus -f Makefile.unix CC=$(abspath $(PS3DEV))/ppu/bin/ppu-gcc AR=$(abspath $(PS3DEV))/ppu/bin/ppu-ar RANLIB=$(abspath $(PS3DEV))/ppu/bin/ppu-ranlib lib

$(TARGET).elf: $(OFILES) $(LIBCOMMON) $(LIBOPUS)
	$(CC) $(OFILES) $(LDFLAGS) $(LIBS) -o $@

$(TARGET_DEBUG).elf: $(DEBUG_OFILES) $(LIBCOMMON) $(LIBOPUS)
	$(CC) $(DEBUG_OFILES) $(LDFLAGS) $(DEBUG_LIBS) -o $@

-include $(OFILES:.o=.d)
-include $(DEBUG_OFILES:.o=.d)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/obj_debug/%.o: src/%.c
	@mkdir -p $(BUILD)/obj_debug
	$(CC) $(DEBUG_CFLAGS) -c $< -o $@

# Build all 3 packages: Standard Release, GNPDRM Release, and Autonomous Debug & Profiler PKG
pkg: $(OUT_PKG) $(OUT_DEBUG_PKG)
	@echo "=========================================================="
	@echo "Packages successfully generated:"
	@echo "  [Package 1] Standard Release: $(OUT_PKG)"
	@echo "  [Package 2] GNPDRM Release:   $(OUT_GNPDRM_PKG)"
	@echo "  [Package 3] Debug & Profile:  $(OUT_DEBUG_PKG)"
	@echo "=========================================================="

# 1 & 2: Standard Release PKG and GNPDRM Release PKG
$(OUT_PKG): $(TARGET).elf
	mkdir -p $(BUILD)/pkg/USRDIR
	$(PS3DEV)/ppu/bin/ppu-strip $(TARGET).elf -o $(BUILD)/$(TARGET).elf
	$(PS3DEV)/bin/sprxlinker $(BUILD)/$(TARGET).elf
	@echo "Signing Release EBOOT.BIN with Retail NPDRM..."
	$(PS3DEV)/bin/make_self_npdrm $(BUILD)/$(TARGET).elf $(BUILD)/pkg/USRDIR/EBOOT.BIN $(CONTENTID)
	$(PS3DEV)/bin/fself $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).fake.self
	$(PS3DEV)/bin/sfo --fromxml $(SFOXML) $(BUILD)/pkg/PARAM.SFO
	cp $(ICON0) $(BUILD)/pkg/ICON0.PNG || true
	$(PS3DEV)/bin/pkg --contentid $(CONTENTID) $(BUILD)/pkg/ $(OUT_PKG)
	cp $(OUT_PKG) $(OUT_GNPDRM_PKG)
	@echo "Finalizing Release PKG..."
	$(PS3DEV)/bin/package_finalize $(OUT_GNPDRM_PKG) 2>/dev/null || true

# 3: Autonomous Debug & Profiler PKG
$(OUT_DEBUG_PKG): $(TARGET_DEBUG).elf
	mkdir -p $(BUILD)/pkg_debug/USRDIR
	cp $(TARGET_DEBUG).elf $(BUILD)/$(TARGET_DEBUG).unstripped.elf
	$(PS3DEV)/ppu/bin/ppu-strip $(TARGET_DEBUG).elf -o $(BUILD)/$(TARGET_DEBUG).elf
	$(PS3DEV)/bin/sprxlinker $(BUILD)/$(TARGET_DEBUG).elf
	@echo "Signing Debug EBOOT.BIN with Retail NPDRM..."
	$(PS3DEV)/bin/make_self_npdrm $(BUILD)/$(TARGET_DEBUG).elf $(BUILD)/pkg_debug/USRDIR/EBOOT.BIN $(CONTENTID)
	$(PS3DEV)/bin/fself $(BUILD)/$(TARGET_DEBUG).elf $(BUILD)/$(TARGET_DEBUG).fake.self
	$(PS3DEV)/bin/sfo --fromxml $(SFOXML_DEBUG) $(BUILD)/pkg_debug/PARAM.SFO
	cp $(ICON0) $(BUILD)/pkg_debug/ICON0.PNG || true
	$(PS3DEV)/bin/pkg --contentid $(CONTENTID) $(BUILD)/pkg_debug/ $(OUT_DEBUG_PKG)
	cp $(OUT_DEBUG_PKG) $(OUT_DEBUG_GNPDRM_PKG)
	@echo "Finalizing Debug PKG..."
	$(PS3DEV)/bin/package_finalize $(OUT_DEBUG_GNPDRM_PKG) 2>/dev/null || true

clean:
	@echo "Cleaning build artifacts and temporary files..."
	rm -f $(OFILES) $(OFILES:.o=.d) $(TARGET).elf $(TARGET).self $(TARGET).fake.self $(TARGET_DEBUG).elf $(TARGET).pkg $(TARGET).gnpdrm.pkg
	rm -rf $(BUILD)
	$(MAKE) -C third_party/moonlight-common-c -f Makefile.ps3 clean CC=$(abspath $(PS3DEV))/ppu/bin/ppu-gcc AR=$(abspath $(PS3DEV))/ppu/bin/ppu-ar 2>/dev/null || true
	$(MAKE) -C third_party/opus -f Makefile.unix clean 2>/dev/null || true

prepare:
	@echo "Downloading PS3 SDK from $(SDK_URL)..."
	curl --fail --location --output $(SDK_TAR).tmp $(SDK_URL)
	mv $(SDK_TAR).tmp $(SDK_TAR)
	@echo "Extracting PS3 SDK to project directory..."
	tar -xzf $(SDK_TAR) -C .
	@echo "Cleaning up archive..."
	gio trash $(SDK_TAR) 2>/dev/null || rm -f $(SDK_TAR)
	@echo "PS3 SDK environment successfully set up in ./ps3dev."
	@echo "Project is ready to build. Run 'make' to compile."

.PHONY: all clean pkg prepare FORCE
