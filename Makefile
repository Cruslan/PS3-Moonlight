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

SDK_RELEASE	:= nightly-2026-10-12
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
OUT_DEBUG_PKG		:= $(BUILD)/$(TARGET)-debug-$(TIMESTAMP).pkg

CFILES			:= src/main.c src/ui.c src/video.c src/ps3_compat.c src/random.c src/net_logger.c src/openssl_compat.c src/connection.c src/input.c src/audio.c src/handshake.c src/spu_opus_bridge.c
RETAIL_OFILES		:= $(patsubst src/%.c, $(BUILD)/obj/%.o, $(CFILES))
DEBUG_OFILES		:= $(patsubst src/%.c, $(BUILD)/obj_debug/%.o, $(CFILES))

# Retail flags: -O2 -mcpu=cell -mstrict-align -flto
COMMON_INCLUDES		:= -I$(PS3DEV)/ppu/include -I$(PS3DEV)/portlibs/ppu/include -I$(PS3DEV)/portlibs/ppu/include/freetype2 -I./src -I./src/spu/include -I./third_party/moonlight-common-c/src -I./third_party/opus/include -include src/openssl_compat.h
CFLAGS			:= -mcpu=cell -mstrict-align -O2 -flto -Wall -Wextra -Werror=implicit-function-declaration -MMD -MP $(COMMON_INCLUDES)
LDFLAGS			:= -mcpu=cell -mstrict-align -O2 -flto -Wl,--no-undefined -Wl,--as-needed

# Debug flags: -g -finstrument-functions with ps3perf profiler (no -flto to allow function instrumentation)
DEBUG_CFLAGS		:= -mcpu=cell -mstrict-align -O2 -g -finstrument-functions -DENABLE_PS3PERF=1 -Wall -Wextra -Werror=implicit-function-declaration -MMD -MP $(COMMON_INCLUDES) -fno-lto
DEBUG_LDFLAGS		:= -mcpu=cell -mstrict-align -g -fno-lto -Wl,--no-undefined -Wl,--as-needed

RETAIL_LIBS		:= -Wl,--whole-archive -L./third_party/moonlight-common-c -lmoonlight-common-c -Wl,--no-whole-archive -L$(PS3DEV)/ppu/lib -L$(PS3DEV)/portlibs/ppu/lib -L./third_party/opus -lopus -lfreetype -lfont3d -ltiny3d -lvdec -lsysmodule -lsysutil -lio -lrsx -lgcm_sys -lnet -lnetctl -laudio -lcurl -lpolarssl -llv2 -lrt -lm -lz
DEBUG_LIBS		:= -Wl,--whole-archive -L./third_party/moonlight-common-c -lmoonlight-common-c -Wl,--no-whole-archive -L$(PS3DEV)/ppu/lib -L$(PS3DEV)/portlibs/ppu/lib -L./third_party/opus -lopus -lfreetype -lfont3d -ltiny3d -lvdec -lsysmodule -lsysutil -lio -lrsx -lgcm_sys -lnet -lnetctl -laudio -lcurl -lpolarssl -llv2 -lrt -lm -lz -lps3perf

BUILDDIR		:= $(CURDIR)/$(BUILD)

# Default target: builds both retail and debug packages
all: pkg debugpkg

LIBCOMMON		:= third_party/moonlight-common-c/libmoonlight-common-c.a
LIBOPUS			:= third_party/opus/libopus.a
SPU_RETAIL_OBJ		:= $(BUILD)/spu_opus_retail.bin.o
SPU_DEBUG_OBJ		:= $(BUILD)/spu_opus_debug.bin.o

$(SPU_RETAIL_OBJ): FORCE
	@mkdir -p $(BUILD)
	$(MAKE) -C src/spu clean
	$(MAKE) -C src/spu ENABLE_PS3PERF=0 PS3DEV=$(abspath $(PS3DEV))
	cp src/spu/spu_opus.bin.o $(SPU_RETAIL_OBJ)

$(SPU_DEBUG_OBJ): FORCE
	@mkdir -p $(BUILD)
	$(MAKE) -C src/spu clean
	$(MAKE) -C src/spu ENABLE_PS3PERF=1 PS3DEV=$(abspath $(PS3DEV))
	cp src/spu/spu_opus.bin.o $(SPU_DEBUG_OBJ)

$(LIBCOMMON): FORCE
	$(MAKE) -C third_party/moonlight-common-c -f Makefile.ps3 PS3DEV=$(abspath $(PS3DEV)) CC=$(abspath $(PS3DEV))/ppu/bin/ppu-gcc AR=$(abspath $(PS3DEV))/ppu/bin/ppu-ar

$(LIBOPUS): FORCE
	CFLAGS="-mcpu=cell -O2 -Wall -fno-lto -DHAVE_LRINTF -DUSE_ALLOCA" $(MAKE) -C third_party/opus -f Makefile.unix CC=$(abspath $(PS3DEV))/ppu/bin/ppu-gcc AR=$(abspath $(PS3DEV))/ppu/bin/ppu-ar RANLIB=$(abspath $(PS3DEV))/ppu/bin/ppu-ranlib lib

$(TARGET).elf: $(RETAIL_OFILES) $(SPU_RETAIL_OBJ) $(LIBCOMMON) $(LIBOPUS)
	$(CC) $(RETAIL_OFILES) $(SPU_RETAIL_OBJ) $(LDFLAGS) $(RETAIL_LIBS) -o $@

$(TARGET_DEBUG).elf: $(DEBUG_OFILES) $(SPU_DEBUG_OBJ) $(LIBCOMMON) $(LIBOPUS)
	$(CC) $(DEBUG_OFILES) $(SPU_DEBUG_OBJ) $(DEBUG_LDFLAGS) $(DEBUG_LIBS) -o $@

-include $(RETAIL_OFILES:.o=.d)
-include $(DEBUG_OFILES:.o=.d)

$(BUILD)/obj/%.o: src/%.c
	@mkdir -p $(BUILD)/obj
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/obj_debug/%.o: src/%.c
	@mkdir -p $(BUILD)/obj_debug
	$(CC) $(DEBUG_CFLAGS) -c $< -o $@

# Build only Retail Release PKG
pkg: $(OUT_PKG)
	@echo "=========================================================="
	@echo "Retail Package successfully generated:"
	@echo "  [Release Package] $(OUT_PKG)"
	@echo "=========================================================="

# Build only Autonomous Debug & Profiler PKG
debugpkg: $(OUT_DEBUG_PKG)
	@echo "=========================================================="
	@echo "Debug Package successfully generated:"
	@echo "  [Debug Package]   $(OUT_DEBUG_PKG)"
	@echo "=========================================================="

# 1: Standard Retail Release PKG Recipe
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

# 2: Autonomous Debug & Profiler PKG Recipe
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

clean:
	@echo "Cleaning build artifacts and temporary files safely..."
	gio trash src/*.o src/*.d $(TARGET).elf $(TARGET).self $(TARGET).fake.self $(TARGET_DEBUG).elf $(TARGET).pkg 2>/dev/null || true
	gio trash $(BUILD) 2>/dev/null || true
	$(MAKE) -C third_party/moonlight-common-c -f Makefile.ps3 clean CC=$(abspath $(PS3DEV))/ppu/bin/ppu-gcc AR=$(abspath $(PS3DEV))/ppu/bin/ppu-ar 2>/dev/null || true
	$(MAKE) -C third_party/opus -f Makefile.unix clean 2>/dev/null || true
	$(MAKE) -C src/spu clean 2>/dev/null || true

prepare:
	@if [ -L ps3dev ] || [ -h ps3dev ]; then \
		echo "=========================================================="; \
		echo "[prepare] Uyari: './ps3dev' bir sembolik baglantidir (-> $$(readlink ps3dev))."; \
		echo "[prepare] Ozel SDK baglantisi korundu; indirme ve ayiklama islemi atlandi."; \
		echo "=========================================================="; \
		exit 0; \
	elif [ -d ps3dev ]; then \
		echo "=========================================================="; \
		echo "[prepare] Bilgi: './ps3dev' dizini zaten mevcut."; \
		echo "[prepare] SDK hazir durumda; indirme ve ayiklama islemi atlandi."; \
		echo "=========================================================="; \
		exit 0; \
	fi; \
	echo "Downloading PS3 SDK ($(SDK_RELEASE)) from $(SDK_URL)..."; \
	curl --fail --location --progress-bar --output $(SDK_TAR).tmp $(SDK_URL) || { \
		echo "[prepare] Hata: SDK indirilemedi ($(SDK_URL))."; \
		gio trash $(SDK_TAR).tmp 2>/dev/null || true; \
		exit 1; \
	}; \
	mv $(SDK_TAR).tmp $(SDK_TAR); \
	echo "Extracting PS3 SDK to project directory..."; \
	tar -xzf $(SDK_TAR) -C . || { \
		echo "[prepare] Hata: SDK arsivi acilamadi."; \
		gio trash $(SDK_TAR) 2>/dev/null || true; \
		exit 1; \
	}; \
	echo "Cleaning up archive..."; \
	gio trash $(SDK_TAR) 2>/dev/null || true; \
	echo "=========================================================="; \
	echo "PS3 SDK environment successfully set up in ./ps3dev."; \
	echo "Target release: $(SDK_RELEASE)"; \
	echo "Project is ready to build. Run 'make' to compile."; \
	echo "=========================================================="

.PHONY: all clean pkg debugpkg prepare FORCE
