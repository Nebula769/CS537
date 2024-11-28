
#include "wfs.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MAX_DISKS 10

int main(int argc, char *argv[]) {
  int raid_mode = -1;
  char *disk_files[MAX_DISKS];
  int disk_count = 0;
  int inode_count = 0;
  int block_count = 0;

  // Parse command-line arguments
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-r") == 0) {
      if (i + 1 < argc) {
        // Set RAID mode based on input value
        if (strcmp(argv[i + 1], "0") == 0) {
          raid_mode = 0;
        } else if (strcmp(argv[i + 1], "1") == 0) {
          raid_mode = 1;
        } else if (strcmp(argv[i + 1], "1v") == 0) {
          raid_mode = 2;
        } else {
          printf("Error: Invalid RAID mode. Valid options are '0', '1', or "
                 "'1v'.\n");
          return 1;
        }
        i++; // Skip the next argument as it is processed
      } else {
        printf("Error: Missing RAID mode value.\n");
        return 1;
      }
    } else if (strcmp(argv[i], "-d") == 0) {
      if (i + 1 < argc && disk_count < MAX_DISKS) {
        disk_files[disk_count++] = argv[i + 1];
        i++; // Skip next argument as it is processed
      } else {
        printf("Error: Missing or too many disk files. Maximum %d disks "
               "allowed.\n",
               MAX_DISKS);
        return 1;
      }
    } else if (strcmp(argv[i], "-i") == 0) {
      if (i + 1 < argc) {
        inode_count = atoi(argv[i + 1]);
        if (inode_count <= 0) {
          printf("Error: Inode count must be a positive integer.\n");
          return 1;
        }
        i++; // Skip next argument as it is processed
      } else {
        printf("Error: Missing inode count.\n");
        return 1;
      }
    } else if (strcmp(argv[i], "-b") == 0) {
      if (i + 1 < argc) {
        block_count = atoi(argv[i + 1]);
        if (block_count <= 0) {
          printf("Error: Block count must be a positive integer.\n");
          return 1;
        }
        i++; // Skip next argument as it is processed
      } else {
        printf("Error: Missing block count.\n");
        return 1;
      }
    } else {
      printf("Error: Unknown argument '%s'.\n", argv[i]);
      return 1;
    }
  }

  // Validate required arguments
  if (raid_mode == -1) {
    printf("Error: RAID mode not specified. Use '-r <0|1|1v>'.\n");
    return 1;
  }

  if (disk_count < 2) {
    printf("Error: At least 2 disk files are required.\n");
    return 1;
  }

  if (inode_count == 0) {
    printf("Error: Inode count not specified or invalid.\n");
    return 1;
  }

  if (block_count == 0) {
    printf("Error: Block count not specified or invalid.\n");
    return 1;
  }

  // Round up block count to nearest multiple of 32
  block_count = ((block_count + 31) / 32) * 32;
  printf("Updated Block Count: %d\n", block_count);

  // check if files exist
  for (int i = 0; i < disk_count; i++) {
    if (access(disk_files[i], F_OK) != 0) {
      printf("%s does not exist\n", disk_files[i]);
      return ENOENT;
    }
  }

  // round inode count to multiple of 32
  inode_count = ((inode_count + 31) / 32) * 32;

  // Log parsed arguments (for debugging)
  //   printf("RAID mode: %d\n", raid_mode);
  //   printf("Disk count: %d\n", disk_count);
  //   for (int i = 0; i < disk_count; i++) {
  //     printf("Disk file: %s\n", disk_files[i]);
  //   }
  //   printf("Inode count: %d\n", inode_count);
  //   printf("Block count: %d\n", block_count);

  // Example logic for setting up the filesystem (superblock, root inode, etc.)
  // Here you would proceed with creating the filesystem based on the input
  // parameters

  // write to disks
  for (int i = 0; i < disk_count; i++) {
    const char *file_name = disk_files[i];
    int fd = open(file_name, O_RDWR);
    if (fd == -1) {
      perror("Error  opening file");
      return 1;
    }

    struct stat st;
    if (fstat(fd, &st) == -1) {
      perror("fstat");
      close(fd);
      return 1;
    }

    // get the size of the file
    size_t filesize = st.st_size;

    // map the file into mem
    void *map = mmap(NULL, filesize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (map == MAP_FAILED) {
      perror("mmap");
      return 1;
    }

    // init and write the superblock
    struct wfs_sb sb;
    sb.num_data_blocks = block_count;
    sb.num_inodes = inode_count;
    sb.i_bitmap_ptr = sizeof(struct wfs_sb);
    sb.d_bitmap_ptr = sb.i_bitmap_ptr + (inode_count + 7) / 8;
    sb.i_blocks_ptr =
        (((sb.d_bitmap_ptr + (block_count + 7) / 8) + 511) / BLOCK_SIZE) *
        BLOCK_SIZE;
    sb.d_blocks_ptr = sb.i_blocks_ptr + (inode_count * BLOCK_SIZE);
    sb.raid_mode = raid_mode;

    // printf("\ni_bitmap_ptr: %ld\n", sb.i_bitmap_ptr);
    // printf("d_bitmap_ptr: %ld\n", sb.d_bitmap_ptr);
    // printf("i_blocks_ptr: %ld\n", sb.i_blocks_ptr);
    // printf("d_blocks_ptr: %ld\n", sb.d_blocks_ptr);

    // write superblock to disk
    memcpy(map, &sb, sizeof(sb));

    // bitmaps
    unsigned char i_bitmap[(inode_count + 7) / 8];
    memset(i_bitmap, 0, sizeof(i_bitmap));
    // set the first inode as used
    i_bitmap[0] = 1;
    memcpy((char *)map + sb.i_bitmap_ptr, i_bitmap, sizeof(i_bitmap));

    unsigned char d_bitmap[(block_count + 7) / 8];
    memset(d_bitmap, 0, sizeof(d_bitmap));
    memcpy((char *)map + sb.d_bitmap_ptr, d_bitmap, sizeof(d_bitmap));

    // init data blocks
    char block[BLOCK_SIZE];
    memset(block, 0, BLOCK_SIZE);

     size_t required_data_size = sb.d_blocks_ptr + (block_count * BLOCK_SIZE);

       if (required_data_size > filesize) {
        printf("Error: Not enough space for data blocks. Disk size insufficient.\n");
        return 255; // Return 0 as per test requirements
    }

    // initialize inodes

    struct wfs_inode root_inode;
    root_inode.num = 0;
    root_inode.uid = getuid();
    root_inode.gid = getgid();
    root_inode.mode = S_IFDIR | S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
    root_inode.nlinks = 1;
    root_inode.size = 0;

    // bitmap rounding divide block size by something make a function for that
    // for test case

    // time
    time_t current_time = time(NULL);
    root_inode.atim = current_time;
    root_inode.mtim = current_time;
    root_inode.ctim = current_time;

    memcpy((char *)map + sb.i_blocks_ptr, &root_inode, sizeof(root_inode));

    // unmap files from mem and close them
    if (munmap(map, filesize) == -1) {
      perror("munmap");
      close(fd);
      return 1;
    }
    close(fd);
  }

  return 0;
}
