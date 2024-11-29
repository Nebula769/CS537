BINS = wfs mkfs
CC = gcc
CFLAGS = -Wall -Werror -pedantic -std=gnu18 -g
FUSE_CFLAGS = `pkg-config fuse --cflags --libs`
FUSE_OPTIONS = -s -f

.PHONY: all
all: $(BINS)

wfs:
	$(CC) $(CFLAGS) wfs.c $(FUSE_CFLAGS) -o wfs
mkfs:
	$(CC) $(CFLAGS) -o mkfs mkfs.c

.PHONY: clean
clean:
	rm -rf $(BINS)
	clear

runMKFS: mkfs
	./mkfs -r 1 -d disk1 -d disk2 -i 32 -b 200

runWFS: wfs
	./wfs disk1 disk2 $(FUSE_OPTIONS) mnt

createDisks:
	./create_disk.sh

run: createDisks runMKFS runWFS
