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

runMKFS0: mkfs
	./mkfs -r 0 -d disk1 -d disk2 -i 32 -b 200

runWFS: wfs
	./wfs disk1 disk2 $(FUSE_OPTIONS) mnt

createDisks:
	./create_disk.sh
	clear

run: createDisks runMKFS runWFS


run0: createDisks runMKFS0 runWFS

create_test_disks:
	dd if=/dev/zero of=disk1 bs=512 count=2048
	dd if=/dev/zero of=disk2 bs=512 count=2048

runtest: create_test_disks runtestWFSInvalidID


run1: clean run

run0: clean run0