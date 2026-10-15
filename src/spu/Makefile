PS3DEV ?= /mnt/nvme1n1p1/Build/ps3dev

SPU_CC  := $(PS3DEV)/spu/bin/spu-gcc
SPU_AR  := $(PS3DEV)/spu/bin/spu-ar
BIN2S   := $(PS3DEV)/bin/bin2s
PPU_AS  := $(PS3DEV)/ppu/bin/ppu-as

OPUS_DIR := ../../third_party/opus
BUILD_OPUS := build_opus

ENABLE_PS3PERF ?= 0

COMMON_SPU_CFLAGS := -Os -ffunction-sections -fdata-sections -Wall -I./include -I$(OPUS_DIR)/include -I$(PS3DEV)/spu/include

ifeq ($(ENABLE_PS3PERF),1)
CFLAGS  := $(COMMON_SPU_CFLAGS) -g -finstrument-functions -DENABLE_PS3PERF=1
LDFLAGS := -L$(PS3DEV)/spu/lib -Wl,--gc-sections -lps3perf_spu -lsputhread -lm
else
CFLAGS  := $(COMMON_SPU_CFLAGS)
LDFLAGS := -L$(PS3DEV)/spu/lib -Wl,--gc-sections -lsputhread -lm
endif

TARGET_ELF := spu_opus.elf
TARGET_O   := spu_opus.bin.o
TARGET_LIB := libspu_opus.a

OPUS_SRCS := \
	celt/bands.c celt/celt.c celt/celt_decoder.c celt/cwrs.c celt/entcode.c celt/entdec.c celt/entenc.c \
	celt/kiss_fft.c celt/laplace.c celt/mathops.c celt/mdct.c celt/modes.c celt/pitch.c \
	celt/celt_lpc.c celt/quant_bands.c celt/rate.c celt/vq.c \
	src/opus.c src/opus_decoder.c src/opus_multistream.c src/opus_multistream_decoder.c src/extensions.c \
	silk/CNG.c silk/code_signs.c silk/init_decoder.c silk/decode_core.c silk/decode_frame.c \
	silk/decode_parameters.c silk/decode_indices.c silk/decode_pulses.c silk/decoder_set_fs.c \
	silk/dec_API.c silk/gain_quant.c silk/interpolate.c silk/LP_variable_cutoff.c \
	silk/NLSF_decode.c silk/NSQ.c silk/NSQ_del_dec.c silk/PLC.c silk/shell_coder.c \
	silk/tables_gain.c silk/tables_LTP.c silk/tables_NLSF_CB_NB_MB.c silk/tables_NLSF_CB_WB.c \
	silk/tables_other.c silk/tables_pitch_lag.c silk/tables_pulses_per_block.c silk/VAD.c \
	silk/control_audio_bandwidth.c silk/quant_LTP_gains.c silk/VQ_WMat_EC.c silk/HP_variable_cutoff.c \
	silk/NLSF_unpack.c silk/stereo_LR_to_MS.c silk/stereo_MS_to_LR.c silk/biquad_alt.c \
	silk/bwexpander_32.c silk/bwexpander.c silk/decode_pitch.c silk/inner_prod_aligned.c \
	silk/lin2log.c silk/log2lin.c silk/LPC_analysis_filter.c silk/LPC_inv_pred_gain.c \
	silk/table_LSF_cos.c silk/NLSF2A.c silk/NLSF_stabilize.c silk/pitch_est_tables.c \
	silk/resampler.c silk/resampler_down2_3.c silk/resampler_down2.c silk/resampler_private_AR2.c \
	silk/resampler_private_down_FIR.c silk/resampler_private_IIR_FIR.c silk/resampler_private_up2_HQ.c \
	silk/resampler_rom.c silk/sigm_Q15.c silk/sort.c silk/sum_sqr_shift.c silk/stereo_decode_pred.c \
	silk/LPC_fit.c silk/fixed/apply_sine_window_FIX.c silk/fixed/autocorr_FIX.c silk/fixed/burg_modified_FIX.c \
	silk/fixed/k2a_FIX.c silk/fixed/k2a_Q16_FIX.c silk/fixed/pitch_analysis_core_FIX.c \
	silk/fixed/vector_ops_FIX.c silk/fixed/schur64_FIX.c silk/fixed/schur_FIX.c

OPUS_OBJS := $(patsubst %.c, $(BUILD_OPUS)/%.o, $(OPUS_SRCS))

OPUS_CFLAGS := -Os -ffunction-sections -fdata-sections -DOPUS_BUILD -DUSE_ALLOCA -DHAVE_LRINTF \
	-I$(OPUS_DIR)/include -I$(OPUS_DIR)/celt -I$(OPUS_DIR)/silk -I$(OPUS_DIR)/silk/fixed \
	-DFIXED_POINT=1 -DDISABLE_FLOAT_API

all: $(TARGET_O)

$(BUILD_OPUS)/%.o: $(OPUS_DIR)/%.c
	@mkdir -p $(dir $@)
	$(SPU_CC) $(OPUS_CFLAGS) -c $< -o $@

$(TARGET_LIB): $(OPUS_OBJS)
	$(SPU_AR) rcs $@ $^

$(TARGET_ELF): source/spu_opus.c include/spu_opus.h $(TARGET_LIB)
	$(SPU_CC) $(CFLAGS) source/spu_opus.c $(TARGET_LIB) $(LDFLAGS) -o $@

$(TARGET_O): $(TARGET_ELF)
	$(BIN2S) -a 128 $< | $(PPU_AS) -o $@

clean:
	@mkdir -p ../../.trash
	@mv $(TARGET_ELF) $(TARGET_O) ../../.trash/ 2>/dev/null || true

clean-all:
	@mkdir -p ../../.trash
	@mv $(TARGET_ELF) $(TARGET_O) $(TARGET_LIB) $(BUILD_OPUS) ../../.trash/ 2>/dev/null || true

.PHONY: all clean clean-all
