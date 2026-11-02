# Build on aarch64 (GitHub Actions ubuntu-22.04-arm does this).
# Needs: libsdl2-dev libsdl2-ttf-dev libstb-dev zlib1g-dev
CC      ?= gcc
CFLAGS  ?= -O2 -Wall $(shell sdl2-config --cflags)
LDFLAGS ?= $(shell sdl2-config --libs) -lSDL2_ttf -lz
OUT     ?= mspa_reader.aarch64

$(OUT): mspa_reader.c mjson.c mjson.h gifdec.c gifdec.h
	$(CC) $(CFLAGS) -o $(OUT) mspa_reader.c mjson.c gifdec.c $(LDFLAGS)
clean:
	rm -f $(OUT)
