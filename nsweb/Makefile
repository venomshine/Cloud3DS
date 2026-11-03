# Librerías de NetSurf (MIT): leer HTML (hubbub), montar el DOM (libdom) y el CSS (libcss), con sus utilidades
# (libparserutils, libwapcaplet). Se compilan tal cual en lib/libnsweb.a. Ver README.md.
include $(DEVKITARM)/3ds_rules
ARCH   := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
BASE   := $(ARCH) -O2 -ffunction-sections -fdata-sections -std=gnu99 -D__3DS__ -DWITHOUT_ICONV_FILTER -w -Iinclude
SRC_wapcaplet := $(wildcard libwapcaplet/src/*.c)
SRC_parserutils := $(shell find libparserutils/src -name '*.c')
SRC_hubbub := $(shell find libhubbub/src -name '*.c')
SRC_dom := $(shell find libdom/src -name '*.c') libdom/bindings/hubbub/parser.c
SRC_css := $(shell find libcss/src -name '*.c')
OBJS := $(patsubst %.c,build/%.o,$(SRC_wapcaplet) $(SRC_parserutils) $(SRC_hubbub) $(SRC_dom) $(SRC_css))

lib/libnsweb.a: $(OBJS)
	@mkdir -p lib
	$(AR) rcs $@ $^

build/libwapcaplet/%.o: libwapcaplet/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BASE) -Ilibwapcaplet/src -c $< -o $@
build/libparserutils/%.o: libparserutils/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BASE) -Ilibparserutils/src -c $< -o $@
build/libhubbub/%.o: libhubbub/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BASE) -Ilibhubbub/src -c $< -o $@
build/libcss/%.o: libcss/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BASE) -Wno-error=incompatible-pointer-types -Wno-error=int-conversion -Ilibcss/src -c $< -o $@
build/libdom/%.o: libdom/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BASE) -Ilibdom/src -Ilibdom/bindings -c $< -o $@

clean:
	rm -rf build lib
