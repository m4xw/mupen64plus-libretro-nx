CC ?= gcc
CFLAGS += -g -O0 -I../../src -I../../src/device/r4300 -I../../src/device/r4300/new_dynarec -I../../src/api -I../../..//libretro-common/include -DNEW_DYNAREC=4
LDFLAGS += -lcheck -lsubunit -lm

all: test_wasm_dynarec

clean:
	$(RM) test_wasm_dynarec

.PHONY: all clean

test_wasm_dynarec: test_wasm_dynarec.c ../../src/device/r4300/wasm_dynarec/wasm_dynarec.c
	$(CC) $(CFLAGS) -Werror -o $@ $^ $(LDFLAGS)
