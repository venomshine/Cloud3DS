# FAAD2 (GPL-2.0-o-posterior), el decodificador AAC de la música en streaming (SoundCloud entrega AAC por HLS).
# Descarga la versión fijada (comprobando su commit) y la compila para la 3DS en lib/ con su cabecera en include/.
VER    := 2.11.4
COMMIT := e8e76f0a44db45aed773a3f62fe35a63c867a738
SRC    := src/faad2-$(VER)
ARCH   := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -mword-relocations -ffunction-sections -fdata-sections -D__3DS__
# punto fijo (la 3DS no tiene coma flotante rápida) y solo AAC-LC, que es lo que entrega SoundCloud
DEFS   := -DFIXED_POINT -DLC_ONLY_DECODER -DPACKAGE_VERSION=\"$(VER)\" -DSTDC_HEADERS -DHAVE_STDINT_H -DHAVE_STRING_H \
          -DHAVE_INTTYPES_H -DHAVE_MEMCPY -DHAVE_STRCHR
CFLAGS := $(ARCH) $(DEFS) -O2 -std=gnu17 -w -I$(SRC)/include -I$(SRC)/libfaad
CC     := arm-none-eabi-gcc
AR     := arm-none-eabi-ar
OBJS    = $(patsubst $(SRC)/libfaad/%.c,build/%.o,$(wildcard $(SRC)/libfaad/*.c))

lib/libfaad.a: $(SRC)/.ok
	@$(MAKE) --no-print-directory objs
	@mkdir -p lib include
	$(AR) rcs $@ $(patsubst $(SRC)/libfaad/%.c,build/%.o,$(wildcard $(SRC)/libfaad/*.c))
	cp $(SRC)/include/neaacdec.h include/

objs: $(OBJS)

build/%.o: $(SRC)/libfaad/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

$(SRC)/.ok:
	@mkdir -p src
	rm -rf $(SRC)
	git -c advice.detachedHead=false clone -q --depth 1 --branch $(VER) https://github.com/knik0/faad2.git $(SRC)
	[ "$$(git -C $(SRC) rev-parse HEAD)" = "$(COMMIT)" ]
	touch $@

.PHONY: objs
