# MuPDF (AGPL-3.0) para el Lector: PDF, CBZ, EPUB, DOCX y TXT. Ver README.md.
# Descarga la versión fijada (comprobando su SHA-256) y la compila para la 3DS en lib/ con las cabeceras en include/.
VER    := 1.26.9
SHA    := 54ceeaab4a694526d9db7282431e62d3ff9b1ca52da0c5dbf0c82c9a43361a1d
TGZ    := src/mupdf-$(VER)-source.tar.gz
SRC    := src/mupdf-$(VER)-source
P      := $(DEVKITPRO)/portlibs/3ds
ARCH   := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -mword-relocations -ffunction-sections -fdata-sections -D__3DS__
# lo que no hace falta para leer: JavaScript, códigos de barras, OCR, escribir DOCX/ODT, XPS, perfiles de color, brotli y
# las fuentes de otros alfabetos (las CJK básicas sí, para textos en japonés)
OPTS   := -DFZ_ENABLE_JS=0 -DFZ_ENABLE_BARCODE=0 -DFZ_ENABLE_OCR_OUTPUT=0 -DFZ_ENABLE_DOCX_OUTPUT=0 -DFZ_ENABLE_ODT_OUTPUT=0 \
          -DFZ_ENABLE_XPS=0 -DFZ_ENABLE_ICC=0 -DFZ_ENABLE_BROTLI=0 -DFZ_ENABLE_SPOT_RENDERING=0 -DFZ_PLOTTERS_CMYK=0 \
          -DFZ_PLOTTERS_N=0 -DTOFU -DTOFU_CJK_EXT -DTOFU_CJK_LANG -DTOFU_HISTORIC
# GCC 14: C17 (la libjpeg incluida usa prototipos antiguos) y sin convertir en error lo de int/long de ARM
CFLAGS := $(ARCH) $(OPTS) -std=gnu17 -O2 -Wno-incompatible-pointer-types -Wno-format -include $(CURDIR)/fix3ds.h

lib/libmupdf.a: $(SRC)/Makefile fix3ds.h
	@mkdir -p zinc lib
	@cp $(P)/include/zlib.h $(P)/include/zconf.h zinc/
	$(MAKE) -C $(SRC) libs build=release shared=no verbose=no \
	  CC=arm-none-eabi-gcc CXX=arm-none-eabi-g++ AR=arm-none-eabi-ar LD=arm-none-eabi-ld \
	  HAVE_GLUT=no HAVE_X11=no HAVE_OBJCOPY=no HAVE_LIBCRYPTO=no HAVE_PTHREAD=no HAVE_LIBDL=no HAVE_CURL=no \
	  USE_SYSTEM_ZLIB=yes SYS_ZLIB_CFLAGS="-I$(CURDIR)/zinc" SYS_ZLIB_LIBS="-lz" \
	  USE_TESSERACT=no USE_ZXINGCPP=no USE_LEPTONICA=no USE_BROTLI=no \
	  XCFLAGS="$(CFLAGS)" XCXXFLAGS="$(ARCH) -fno-exceptions -fno-rtti"
	cp $(SRC)/build/release/libmupdf.a $(SRC)/build/release/libmupdf-third.a lib/
	rm -rf include && cp -r $(SRC)/include include

$(SRC)/Makefile:
	@mkdir -p src
	[ -f $(TGZ) ] || curl -sSL -o $(TGZ) https://mupdf.com/downloads/archive/mupdf-$(VER)-source.tar.gz
	echo "$(SHA)  $(TGZ)" | sha256sum -c -
	tar xzf $(TGZ) -C src
	touch $@
