# SPDX-License-Identifier: GPL-3.0-only
# Host (Mac/Linux) build of the SSTV receiver core + test bench.
#   make          build sstv_decode, sstv_tx, test_dsp, test_cap
#   make test     run the decoder bench and the capture-drain simulation
CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -g -Wall -Wextra -Wno-unused-parameter -D_DEFAULT_SOURCE
CORE     = ../sstv_dsp.c
BINS     = sstv_decode sstv_tx test_dsp test_cap preview_app

all: $(BINS)

sstv_decode: sstv_decode.c util.c $(CORE) ../sstv_dsp.h
	$(CC) $(CFLAGS) -o $@ sstv_decode.c util.c $(CORE) -lm
sstv_tx: sstv_tx.c enc.c util.c
	$(CC) $(CFLAGS) -o $@ sstv_tx.c enc.c util.c -lm
test_dsp: test_dsp.c enc.c util.c $(CORE) ../sstv_dsp.h
	$(CC) $(CFLAGS) -o $@ test_dsp.c enc.c util.c $(CORE) -lm
test_cap: test_cap.c ../nano_cap.c ../nano_cap.h enc.c util.c $(CORE)
	$(CC) $(CFLAGS) -o $@ test_cap.c ../nano_cap.c enc.c util.c $(CORE) -lm

# the app as on the nano: decoder in the static pool (-DSSTV_POOL, ../sstv_support.c)
preview_app: preview_app.c ../sstv_app.c ../nano_cap.c ../gsf.c ../sstv_support.c ../sstv_font5x7.h font_io_host.c enc.c util.c $(CORE)
	$(CC) $(CFLAGS) -Wno-misleading-indentation -Istub_sdk -I.. -DAPRICOT_NANO -c ../sstv_app.c -o sstv_app_host.o
	$(CC) $(CFLAGS) -I.. -DSSTV_POOL -c $(CORE) -o sstv_dsp_pool.o
	$(CC) $(CFLAGS) -I.. -c ../sstv_support.c -o sstv_support_host.o
	$(CC) $(CFLAGS) -Wno-misleading-indentation -Istub_sdk -I.. -o $@ preview_app.c sstv_app_host.o sstv_dsp_pool.o sstv_support_host.o ../nano_cap.c ../gsf.c font_io_host.c enc.c util.c -lm

# hours of hostile audio into the decoder under ASan/UBSan (not part of `make test`):
#   make fuzz_dsp && ./fuzz_dsp . 3600 1
fuzz_dsp: fuzz_dsp.c enc.c util.c $(CORE) ../sstv_dsp.h
	$(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -Wall -D_DEFAULT_SOURCE -o $@ fuzz_dsp.c enc.c util.c $(CORE) -lm

# test fonts in GSF1 (needs Pillow + DejaVu): ui.gsf 11 px, title.gsf 17 px
fonts/ui.gsf fonts/title.gsf: tools/mkgsf.py
	mkdir -p fonts
	python3 tools/mkgsf.py /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf 11 fonts/ui.gsf --family "DejaVu Sans"
	python3 tools/mkgsf.py /usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf 17 fonts/title.gsf --family "DejaVu Sans Bold"

out:
	mkdir -p out

IMAGES = testcard.ppm photo.ppm testcard_320x256.ppm photo_320x256.ppm \
         testcard_512x400.ppm photo_512x400.ppm testcard_640x496.ppm photo_640x496.ppm \
         testcard_800x616.ppm photo_800x616.ppm
$(IMAGES): tools/make_testcard.py
	python3 tools/make_testcard.py .

test: $(BINS) $(IMAGES) out
	./test_dsp . out
	./test_cap photo.ppm out
	./preview_app photo.ppm out

clean:
	rm -f $(BINS) fuzz_dsp *.o out/*.ppm out/*.BMP
.PHONY: all test clean
