CC      = gcc
CFLAGS  = -std=gnu11 -g -O0 -Wall -Wno-unused-function -Wno-builtin-declaration-mismatch -fcommon -I. -Iinclude
SRCS    = $(wildcard src/*.c) sys/disk/disk.c sys/mem/mem.c \
          util/list.c util/spinlock.c util/string.c
TEST    = bin/fs_test

all: $(TEST)

$(TEST): $(SRCS) test/fs_test.c $(wildcard include/*.h util/*.h sys/*/*.h)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(SRCS) test/fs_test.c -o $@

empty-iso:
	dd if=/dev/zero bs=4096 count=161 > sys/empty-fs.iso

fs: empty-iso
	python3 fs-maker.py

test: fs $(TEST)
	$(TEST)

clean:
	rm -f bin/*