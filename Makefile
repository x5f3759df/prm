CC      ?= cc
CFLAGS  ?= -Os -flto -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables
LDFLAGS ?= -flto -Wl,--gc-sections -s

INCLUDES = \
	-Isrc \
	-Ivendor/yyjson \
	-Ivendor/miniz \
	-Ivendor/xz \
	-Ivendor/mbedtls/include \
	-Ivendor/mbedtls/library

VENDOR_DEFS = \
	-D_DEFAULT_SOURCE \
	-DYYJSON_DISABLE_WRITER=1 \
	-DYYJSON_DISABLE_NON_STANDARD=1 \
	-DYYJSON_DISABLE_UTILS=1 \
	-DYYJSON_DISABLE_FAST_FP_CONV=1 \
	-DMINIZ_NO_STDIO \
	-DMINIZ_NO_TIME \
	-DMINIZ_NO_DEFLATE_APIS \
	-DMINIZ_NO_ARCHIVE_APIS \
	-DMINIZ_NO_MALLOC \
	-DXZ_DEC_X86 \
	-DXZ_USE_CRC64 \
	-DZSTD_NO_UNUSED_FUNCTIONS=1 \
	-DZSTD_LEGACY_SUPPORT=0 \
	-DHUF_FORCE_DECOMPRESS_X1=1 \
	-DZSTD_FORCE_DECOMPRESS_SEQUENCES_SHORT=1 \
	-DZSTD_DISABLE_ASM=1 \
	-DMBEDTLS_CONFIG_FILE=\"$(CURDIR)/vendor/mbedtls/mbedtls_custom_config.h\"

PRM_SRCS = \
	src/main.c \
	src/runner.c \
	src/sources.c \
	src/archive.c \
	src/steam.c

VENDOR_SRCS = \
	vendor/yyjson/yyjson.c \
	vendor/miniz/miniz.c \
	vendor/zstd/zstddeclib.c \
	$(wildcard vendor/xz/*.c) \
	$(wildcard vendor/mbedtls/library/*.c)

BUILD_DIR = build
PRM_OBJS = $(patsubst %.c,$(BUILD_DIR)/%.o,$(PRM_SRCS))
VENDOR_OBJS = $(patsubst %.c,$(BUILD_DIR)/%.o,$(VENDOR_SRCS))

.PHONY: all static test clean

all: prm

$(BUILD_DIR)/src/%.o: src/%.c src/prm.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -std=c11 -Wall -Wextra -Werror $(INCLUDES) $(VENDOR_DEFS) -c $< -o $@

$(BUILD_DIR)/vendor/%.o: vendor/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w $(INCLUDES) $(VENDOR_DEFS) -c $< -o $@

prm: $(PRM_OBJS) $(VENDOR_OBJS)
	$(CC) $(CFLAGS) $(PRM_OBJS) $(VENDOR_OBJS) $(LDFLAGS) -o $@

static:
	$(MAKE) clean
	$(MAKE) CC="$(shell which musl-gcc 2>/dev/null || echo /tmp/musl/bin/musl-gcc)" LDFLAGS="-static -flto -Wl,--gc-sections -s" prm

test: $( filter-out $(BUILD_DIR)/src/main.o, $(PRM_OBJS) ) $(VENDOR_OBJS)
	$(CC) $(CFLAGS) -std=c11 -Wall -Wextra -Werror $(INCLUDES) $(VENDOR_DEFS) \
		tests/test_prm.c $(filter-out $(BUILD_DIR)/src/main.o,$(PRM_OBJS)) $(VENDOR_OBJS) $(LDFLAGS) -o $(BUILD_DIR)/test_prm
	./$(BUILD_DIR)/test_prm

clean:
	rm -rf $(BUILD_DIR) prm
