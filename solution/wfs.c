#define FUSE_USE_VERSION 30
#include "wfs.h"
#include <errno.h>
#include <fuse.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define MAX_DISKS 10

void *maps[MAX_DISKS];
struct wfs_sb *sb_array[MAX_DISKS];
int mounted_disks = 0;
int raid = -1;

int validate_mount_disks(struct wfs_sb *sb_array[], char *disk_files[],
                         int disk_count, int expected_disks);

int mount_disks(char *disk_files[], int disk_count) {
  int fd;
  for (int i = 0; i < disk_count; i++) {
    char *file_name = disk_files[i];
    fd = open(file_name, O_RDWR);
    if (fd == -1) {
      perror("Error opening disk file");
      return -1;
    }

    // Get disk file size before mapping
    struct stat st;
    if (fstat(fd, &st) == -1) {
      perror("Error getting disk file size");
      close(fd);
      return -1;
    }

    // Map the file into memory
    size_t filesize = st.st_size;
    maps[i] = mmap(NULL, filesize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (maps[i] == MAP_FAILED) {
      perror("Error mapping disk file");
      close(fd);
      return -1;
    }

    close(fd);

    // set the superblock
    sb_array[i] = (struct wfs_sb *)maps[i];

    if (raid == -1) {
      raid = sb_array[i]->raid_mode;
    } else if (raid != sb_array[i]->raid_mode) {
      printf("Error: RAID mode mismatch between disks.\n");
      return -1;
    }

    // printf("Set superblock for disk: %s\n", file_name);
    // printf("Superblock details for disk %s:\n", file_name);
    // printf("  Number of Inodes: %zu\n", sb_array[i]->num_inodes);
    // printf("  Number of Data Blocks: %zu\n", sb_array[i]->num_data_blocks);
    // printf("  Inode Bitmap Pointer: %ld\n", sb_array[i]->i_bitmap_ptr);
    // printf("  Data Bitmap Pointer: %ld\n", sb_array[i]->d_bitmap_ptr);
    // printf("  Inode Blocks Pointer: %ld\n", sb_array[i]->i_blocks_ptr);
    // printf("  Data Blocks Pointer: %ld\n", sb_array[i]->d_blocks_ptr);
    // printf("  RAID Mode: %d\n", sb_array[i]->raid_mode);
  }

  int expected_disks = sb_array[0]->num_disks;
  if (validate_mount_disks(sb_array, disk_files, disk_count, expected_disks) !=
      0) {
    printf("Disk validation failed. Aborting mount.\n");
    return -1;
  }

  return 0;
}

int compare_disks(const void *a, const void *b) {
  struct wfs_sb *sb_a = *(struct wfs_sb **)a;
  struct wfs_sb *sb_b = *(struct wfs_sb **)b;
  return sb_a->disk_id - sb_b->disk_id;
}
// cound the filesystem id of the 2nd 3rd 4th be differnt
int validate_mount_disks(struct wfs_sb *sb_array[], char *disk_files[],
                         int disk_count, int expected_disks) {

  // If the filesystem was created with n drives, it has to be always mounted
  // with n drives
  if (disk_count != expected_disks) {
    printf("Error: Incorrect number of disks. Expected %d, got %d.\n",
           expected_disks, disk_count);
    return -1;
  }

  // check if in same file system
  int expected_filesystem_id = sb_array[0]->filesystem_id;
  for (int i = 0; i < disk_count; i++) {
    printf("Disk %d: filesystem_id = %d\n", i, sb_array[i]->filesystem_id);
    if (sb_array[i]->filesystem_id != expected_filesystem_id) {
      printf(
          "Error: Disk %d does not belong to the same filesystem as Disk 0.\n",
          i);
      return -1;
    }
  }

  // Make sure that order or name change doesnt affect anything
  qsort(sb_array, disk_count, sizeof(struct wfs_sb *), compare_disks);

  // Validate disk_id sequence
  for (int i = 0; i < disk_count; i++) {
    printf("Debug: Disk %d has disk_id %d (expected %d).\n", i,
           sb_array[i]->disk_id, i);
    if (sb_array[i]->disk_id != i) {
      printf("Error: Disk %d has an unexpected disk_id %d (expected %d).\n", i,
             sb_array[i]->disk_id, i);
      return -1;
    }
  }

  return 0;
}

// static int wfs_mkdir(const char *path, mode_t mode) {
//   if (!path || strlen(path) == 0 || strcmp(path, "/") == 0) {
//     return -EINVAL;
//   }

//   // Allocate a copy of the path to manipulate
//   char *path_copy = malloc(strlen(path) + 1);
//   if (!path_copy) {
//     return -ENOMEM;
//   }
//   strcpy(path_copy, path);

//   // Find parent directory path and directory name
//   char *last_slash = strrchr(path_copy, '/');
//   if (!last_slash || last_slash == path_copy) {
//     free(path_copy);
//     return -EINVAL;
//   }

//   // sep paretn path and new dir name
//   *last_slash = '\0';
//   char *parent_path = path_copy;
//   char *dir_name = last_slash + 1;

//   if (strlen(dir_name) == 0) {
//     free(path_copy);
//     return -EINVAL;
//   }

//   // Traverse from root to locate parent directory
//   struct wfs_sb *root_inode =
//       (struct wfs_inode *)(maps[0] + sb_array[0]->i_blocks_ptr);
//   if (!root_inode) {
//     free(path_copy);
//     return -EIO;
//   }

//   // go to the parent path
//   struct wfs_inode *current_inode = root_inode;
//   char *token = strtok(parent_path, "/");

//   while (token) {
//     int found = 0;

//     // traverse thru the current inode
//     for (int i = 0; i < D_BLOCK; i++) {
//       if (current_inode->blocks[i] == 0)
//         continue;
//     }
//   }
// }

// static int wfs_getattr(const char *path, struct stat *stbuf) {
//   // Implementation of getattr function to retrieve file attributes
//   // Fill stbuf structure with the attributes of the file/directory indicated
//   // by path parse pa
//   char *path_copy = malloc(strlen(path) + 1);
//   if (!path_copy) {
//     return -ENOMEM; // Return -ENOMEM on memory allocation error
//   }
//   strcpy(path_copy, path);

//   char *token = strtok(path_copy, "/");
//   while (token != NULL) {
//     // process token

//     token = strtok(NULL, "/");
//   }

//   free(path_copy);

//   return 0; // Return 0 on success
// }

// helper functions
int is_directory(mode_t mode) {
  return (mode & S_IFMT) == S_IFDIR; // Check if it's a directory
}

int is_regular_file(mode_t mode) {
  return (mode & S_IFMT) == S_IFREG; // Check if it's a regular file
}

int main(int argc, char *argv[]) {
  // Initialize FUSE with specified operations
  // Filter argc and argv here and then pass it to fuse_main
  if (argc < 3) {
    fprintf(stderr, "Usage: %s disk1 disk2 [FUSE options] mount_point\n",
            argv[0]);
    return 1;
  }

  char *disks[MAX_DISKS];
  int num_disks = 0;
  char *fuse_args[argc];
  int fuse_argc = 0;
  char *mount_point = NULL;

  // Parse arguments
  for (int i = 1; i < argc; i++) {
    if (argv[i][0] == '-') {
      // FUSE option
      fuse_args[fuse_argc++] = argv[i];
    } else if (i == argc - 1) {
      // Last argument is the mount point
      mount_point = argv[i];
    } else {
      // Disk argument
      if (num_disks >= MAX_DISKS) {
        fprintf(stderr, "Too many disk arguments (max %d).\n", MAX_DISKS);
        return 1;
      }
      disks[num_disks++] = argv[i];
    }
  }


  if (!mount_point) {
    fprintf(stderr, "Mount point not specified.\n");
    return 1;
  }

  // Print results for debugging
  // printf("Disks:\n");
  for (int i = 0; i < num_disks; i++) {
    printf("  %s\n", disks[i]);
  }

  // printf("FUSE options:\n");
  for (int i = 0; i < fuse_argc; i++) {
    printf("  %s\n", fuse_args[i]);
  }

  // printf("Mount point: %s\n", mount_point);

  // Pass FUSE options to fuse_main
  fuse_args[fuse_argc++] = mount_point; // Add mount point to FUSE args
  fuse_args[fuse_argc] = NULL;          // Null-terminate for FUSE
  mount_disks(disks, num_disks);

  // return fuse_main(argc, argv, &ops, NULL);
}
