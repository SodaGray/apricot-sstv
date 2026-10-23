# SPDX-License-Identifier: GPL-3.0-only
# Apricot SSTV -- NanoApps app (RAW surface), multi-mode SSTV receiver for the
# iPod nano 7. Put this folder in NanoApps/apps/sstvrx/ and build with ./start.
#
#   SRCS      soft-float app objects: UI, capture drain (integer), libc shims.
#   FPU_SRCS  the decoder, compiled -mfloat-abi=softfp -mfpu=vfpv3-d16 into a
#             static lib. Only integers cross between the two.
# The decoder uses no libm (its own atan2/sin): newlib's libm brings MOVW/MOVT
# address pairs the loader can't relocate. libgcc (integer division) comes from
# the arm/v5te/softfp multilib, and relocheck.py gates the package.

APP_NAME    := sstvrx

SRCS        := sstv_app.c nano_cap.c sstv_support.c gsf.c font_io_nano.c

FPU_SRCS    := sstv_dsp.c
FPU_FLAGS   := -mfloat-abi=softfp -mfpu=vfpv3-d16 -DSSTV_POOL   # decoder memory: sstv_support.c's pool
FPU_LIB     := build/fpu/libsstv.a
LDLIBS_LVGL := $(FPU_LIB)

RAW_SURFACE := 1

# -mword-relocations: never emit MOVW/MOVT address pairs in our own code (the
# packer keeps ABS32 only). -DAPRICOT_NANO: compile the device pokes.
EXTRA_CFLAGS := -DNDEBUG -DAPRICOT_NANO -mword-relocations -I$(CURDIR)

include ../../sdk/hb_app.mk

FPU_OBJS := $(patsubst %.c,build/fpu/%.o,$(FPU_SRCS))
build/fpu/%.o: %.c $(wildcard *.h)
	@mkdir -p build/fpu
	$(CC) $(filter-out -mfpu=% -mfloat-abi=%,$(CFLAGS)) $(FPU_FLAGS) -mword-relocations -c -o $@ $<
$(FPU_LIB): $(FPU_OBJS)
	$(AR) crs $@ $^

# libgcc from the ARMv5TE VFP multilib (literal pools / ABS32 only).
MULTI     := -marm -march=armv5te+fp -mfloat-abi=softfp
LIBGCC_FP := $(shell $(CC) $(MULTI) -print-libgcc-file-name)
LDLIBS    := -Wl,--start-group $(LIBGCC_FP) $(LDLIBS) -Wl,--end-group

# refuse to package an app the loader would relocate wrongly
$(BUILD)/$(APP_NAME).hbapp: $(BUILD)/relocheck.ok
$(BUILD)/relocheck.ok: $(BUILD)/$(APP_NAME).reloc.elf relocheck.py
	python3 relocheck.py $<
	@touch $@
