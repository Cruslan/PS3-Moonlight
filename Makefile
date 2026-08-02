export PS3DEV	?= $(CURDIR)/ps3dev
export PSL1GHT	?= $(PS3DEV)

TITLE		:= Moonlight PS3
APPID		:= MNLT00001
CONTENTID	:= UP0001-$(APPID)_00-0000000000000000
ICON0		:= ICON0.PNG
SFOXML		:= sfo.xml


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

# Use absolute tool paths so local SDK builds do not depend on the caller's PATH.
CC		:= $(PS3DEV)/ppu/bin/ppu-gcc

TARGET		:= moonlight-ps3
BUILD		:= build
OFILES		:= src/main.o src/ui.o src/video/ps3.o src/ps3_compat.o src/random.o src/net_logger.o src/openssl_compat.o src/connection.o src/input/ps3.o src/audio/ps3.o src/handshake.o
# Enable Cell Broadband Engine CPU optimizations for the PowerPC Processing Unit (PPU)
CFLAGS		+= -mcpu=cell -O2 -Wall -Wextra -Werror=implicit-function-declaration -MMD -MP -I$(PS3DEV)/ppu/include -I$(PS3DEV)/portlibs/ppu/include -I./src -I./src/video -I./third_party/moonlight-common-c/src -I./third_party/opus/include -include src/openssl_compat.h -fno-lto
LDFLAGS     += -fno-lto -Wl,--no-undefined -Wl,--as-needed
# Link with polarssl for client-side cryptography. The moonlight-common-c
# submodule expects mbedtls, but we emulate it via src/openssl_compat.c
# mapping to PolarSSL to avoid conflicts with the portlib mbedtls library.
LIBS		:= -L$(PS3DEV)/ppu/lib -L$(PS3DEV)/portlibs/ppu/lib -L./third_party/moonlight-common-c -lmoonlight-common-c -L./third_party/opus -lopus -lfont3d -ltiny3d -lvdec -lsysmodule -lsysutil -lio -lrsx -lgcm_sys -lnet -lnetctl -laudio -lcurl -lpolarssl -lrt -lm -lz

BUILDDIR	:= $(CURDIR)/$(BUILD)

all: pkg

LIBCOMMON	:= third_party/moonlight-common-c/libmoonlight-common-c.a
LIBOPUS		:= third_party/opus/libopus.a

# We resolve the absolute path of PS3DEV because make invokes the sub-makefile
# from a different subdirectory, which would invalidate relative paths.
$(LIBCOMMON): FORCE
	$(MAKE) -C third_party/moonlight-common-c -f Makefile.ps3 CC=$(abspath $(PS3DEV))/ppu/bin/ppu-gcc AR=$(abspath $(PS3DEV))/ppu/bin/ppu-ar

$(LIBOPUS): FORCE
	CFLAGS="-mcpu=cell -O2 -Wall -fno-lto" $(MAKE) -C third_party/opus -f Makefile.unix CC=$(abspath $(PS3DEV))/ppu/bin/ppu-gcc AR=$(abspath $(PS3DEV))/ppu/bin/ppu-ar RANLIB=$(abspath $(PS3DEV))/ppu/bin/ppu-ranlib lib

$(TARGET).elf: $(OFILES) $(LIBCOMMON) $(LIBOPUS)
	$(CC) $(OFILES) $(LDFLAGS) $(LIBS) -o $@

-include $(OFILES:.o=.d)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Sign EBOOT.BIN with Retail NPDRM after stripping ELF (compatible with both RPCS3 and real hardware)
pkg: $(TARGET).elf
	mkdir -p $(BUILD)/pkg/USRDIR
	$(PS3DEV)/ppu/bin/ppu-strip $(TARGET).elf -o $(BUILD)/$(TARGET).elf
	$(PS3DEV)/bin/sprxlinker $(BUILD)/$(TARGET).elf
	@echo "Signing EBOOT.BIN with Retail NPDRM..."
	$(PS3DEV)/bin/make_self_npdrm $(BUILD)/$(TARGET).elf $(BUILD)/pkg/USRDIR/EBOOT.BIN $(CONTENTID)
	$(PS3DEV)/bin/fself $(BUILD)/$(TARGET).elf $(TARGET).fake.self
	$(PS3DEV)/bin/sfo --fromxml $(SFOXML) $(BUILD)/pkg/PARAM.SFO
	cp $(ICON0) $(BUILD)/pkg/ICON0.PNG || true
	$(PS3DEV)/bin/pkg --contentid $(CONTENTID) $(BUILD)/pkg/ $(TARGET).pkg
	cp $(TARGET).pkg $(TARGET).gnpdrm.pkg
	@echo "Finalizing PKG..."
	$(PS3DEV)/bin/package_finalize $(TARGET).gnpdrm.pkg 2>/dev/null || true

clean:
	@echo "Cleaning build artifacts and temporary files..."
	rm -f $(OFILES) $(OFILES:.o=.d) $(TARGET).elf $(TARGET).self $(TARGET).fake.self $(TARGET).pkg $(TARGET).gnpdrm.pkg
	rm -rf $(BUILD)
	# Clean third_party submodules using absolute paths
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
