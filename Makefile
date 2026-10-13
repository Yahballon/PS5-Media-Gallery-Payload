# Small standalone PS5 ELF, compatible with ps5-payload-dev/sdk.
PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
ifneq ($(filter host test clean,$(MAKECMDGOALS)),)
else
$(error Set PS5_PAYLOAD_SDK to the extracted ps5-payload-sdk directory)
endif
endif

ELF := ps5-media-gallery.elf
CFLAGS := -O2 -Wall -Wextra -Werror -pthread

all: $(ELF)

src/web_asset.h: web/index.html tools/embed_html.py
	python3 tools/embed_html.py

$(ELF): src/gallery.c src/web_asset.h
	$(CC) $(CFLAGS) -DMEDIA_PS5=1 -o $@ src/gallery.c -lpthread

host: src/gallery.c src/web_asset.h
	gcc -std=c11 -O2 -Wall -Wextra -Werror -pthread -o gallery-host src/gallery.c

test: host
	python3 tests/test_gallery.py

deploy: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $(ELF)

clean:
	rm -f $(ELF) gallery-host

.PHONY: all host test deploy clean
