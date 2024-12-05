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
	clear

run: createDisks runMKFS runWFS

create_test_disks:
	dd if=/dev/zero of=disk1 bs=512 count=2048
	dd if=/dev/zero of=disk2 bs=512 count=2048
	dd if=/dev/zero of=disk3 bs=512 count=2048
	dd if=/dev/zero of=disk4 bs=512 count=2048

runtestMKFS: mkfs create_test_disks
	./mkfs -r 1 -d disk1 -d disk2 -i 32 -b 200

# runtestWFS: wfs
# 	./wfs disk1 disk2 $(FUSE_OPTIONS) mnt

# runtestWFSInvalidCount: wfs
# 	./wfs disk1 $(FUSE_OPTIONS) mnt

runtestWFSInvalidID: create_test_disks mkfs wfs 
	./mkfs -r 1 -d disk1 -d disk2 -i 32 -b 200
	./mkfs -r 1 -d disk3 -d disk4 -i 32 -b 200
	./wfs disk1 disk3 $(FUSE_OPTIONS) mnt

runtest: create_test_disks runtestWFSInvalidID


run1: clean run