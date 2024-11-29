#include <errno.h>
#define FUSE_USE_VERSION 30
#include <fuse.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <limits.h>
#include "wfs.h"

#define MAX_DISKS 10


void *maps[MAX_DISKS];
struct wfs_sb *sb_array[MAX_DISKS];
int mounted_disks = 0;

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
        printf("Successfully opened disk file: %s\n", file_name);

        // Map the file into memory
        size_t filesize = st.st_size;
        maps[i] = mmap(NULL, filesize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (maps[i] == MAP_FAILED) {
            perror("Error mapping disk file");
            close(fd);
            return -1;
        }
        printf("Successfully mapped file: %s to memory.\n", file_name);
        close(fd);

        //set the superblock 
        sb_array[i] = (struct wfs_sb *)maps[i];
        printf("Set superblock for disk: %s\n", file_name);
        printf("Superblock details for disk %s:\n", file_name);
        printf("  Number of Inodes: %zu\n", sb_array[i]->num_inodes);
        printf("  Number of Data Blocks: %zu\n", sb_array[i]->num_data_blocks);
        printf("  Inode Bitmap Pointer: %ld\n", sb_array[i]->i_bitmap_ptr);
        printf("  Data Bitmap Pointer: %ld\n", sb_array[i]->d_bitmap_ptr);
        printf("  Inode Blocks Pointer: %ld\n", sb_array[i]->i_blocks_ptr);
        printf("  Data Blocks Pointer: %ld\n", sb_array[i]->d_blocks_ptr);
        printf("  RAID Mode: %d\n", sb_array[i]->raid_mode);
    }

    printf("Successfully mounted %d disks.\n", disk_count);
    return 0;
}

int validate_mount_disks(struct wfs_sb *sb_array[], const char *disk_files[], int disk_count, int expected_disks) 
{
    printf("Validating mount restrictions...\n");



}

static int wfs_mkdir(const char* path, mode_t mode) 
{
    if (!path || strlen(path) == 0 || strcmp(path, "/") == 0) {
        return -EINVAL; 
    }

    // Allocate a copy of the path to manipulate
    char *path_copy = malloc(strlen(path) + 1);
    if (!path_copy) {
        return -ENOMEM; 
    }
    strcpy(path_copy, path);


    // Find parent directory path and directory name
    char *last_slash = strrchr(path_copy, '/');
    if (!last_slash || last_slash == path_copy) {
        free(path_copy);
        return -EINVAL; 
    }

    //sep paretn path and new dir name
    *last_slash = '\0'; 
    char *parent_path = path_copy;
    char *dir_name = last_slash + 1;

    if (strlen(dir_name) == 0) {
        free(path_copy);
        return -EINVAL; 
    }

    //go to the parent directory


}

static int wfs_getattr(const char *path, struct stat *stbuf) {
    // Implementation of getattr function to retrieve file attributes
    // Fill stbuf structure with the attributes of the file/directory indicated
    // by path parse pa
    char *path_copy = malloc(strlen(path) + 1);
    if (!path_copy) {
        return -ENOMEM;  // Return -ENOMEM on memory allocation error
    }
    strcpy(path_copy, path);

    char *token = strtok(path_copy, "/");
    while (token != NULL) {
        // process token

        token = strtok(NULL, "/");
    }

    free(path_copy);

    return 0;  // Return 0 on success
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
                fprintf(stderr, "Too many disk arguments (max %d).\n",
                        MAX_DISKS);
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
    printf("Disks:\n");
    for (int i = 0; i < num_disks; i++) {
        printf("  %s\n", disks[i]);
    }

    printf("FUSE options:\n");
    for (int i = 0; i < fuse_argc; i++) {
        printf("  %s\n", fuse_args[i]);
    }

    printf("Mount point: %s\n", mount_point);

    // Pass FUSE options to fuse_main
    fuse_args[fuse_argc++] = mount_point;  // Add mount point to FUSE args
    fuse_args[fuse_argc] = NULL;           // Null-terminate for FUSE
    mount_disks(disks, num_disks);

    //return fuse_main(argc, argv, &ops, NULL);
}
