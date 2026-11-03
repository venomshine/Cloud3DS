# QuickJS-ng (MIT): el motor de JavaScript del navegador. Se compila tal cual en lib/libqjs.a. Ver README.md.
# Sin NaN-boxing (JS_NAN_BOXING=0, igual en el Makefile principal): con él, en la 3DS, algunos
# iteradores no avanzaban (for…of sobre texto no acababa nunca).
include $(DEVKITARM)/3ds_rules
CFLAGS := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -O2 -fno-strict-aliasing -ffunction-sections -fdata-sections -std=gnu11 -D__3DS__ -DNDEBUG -DJS_NAN_BOXING=0 -w
SRCS := $(wildcard src/*.c)
OBJS := $(patsubst src/%.c,build/%.o,$(SRCS))

lib/libqjs.a: $(OBJS)
	@mkdir -p lib
	$(AR) rcs $@ $^

build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -I$(DEVKITPRO)/libctru/include -Isrc -c $< -o $@

clean:
	rm -rf build lib
