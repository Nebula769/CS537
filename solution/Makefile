BINS = wfs mkfs
CC = gcc
CFLAGS = -Wall -Werror -pedantic -std=gnu18 -g
FUSE_CFLAGS = `pkg-config fuse --cflags --libs`


.PHONY: all
all: $(BINS)

wfs:
	$(CC) $(CFLAGS) wfs.c $(FUSE_CFLAGS) -o wfs
mkfs:
	$(CC) $(CFLAGS) -o mkfs mkfs.c

.PHONY: clean
clean:
	rm -rf $(BINS)

runMKFS: mkfs
	./mkfs -r 1 -d disk1 -d disk2 -i 32 -b 200

createDisks:
	dd if=/dev/zero of=disk1 bs=1M count=1
	dd if=/dev/zero of=disk2 bs=1M count=1

run: createDisks runMKFS clean
