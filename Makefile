# Build on aarch64 (or an arm64 container: debian + libsdl2-dev libsdl2-ttf-dev)
CC      ?= gcc
CFLAGS  ?= -O2 -Wall $(shell sdl2-config --cflags)
LDFLAGS ?= $(shell sdl2-config --libs) -lSDL2_ttf
OUT     ?= mspa_reader.aarch64

$(OUT): mspa_reader.c mspa_page.c mspa_page.h
	$(CC) $(CFLAGS) -o $(OUT) mspa_reader.c mspa_page.c $(LDFLAGS)
clean:
	rm -f $(OUT)
