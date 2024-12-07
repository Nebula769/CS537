#define FUSE_USE_VERSION 30
#include "wfs.h"

#include <errno.h>
#include <fuse.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define MAX_DISKS 10

void *maps[MAX_DISKS];
struct wfs_sb *sb_array[MAX_DISKS];
int num_disks = 0;
int raid = -1;

// helper functions
int compare_disks(const void *a, const void *b) {
    struct wfs_sb *sb_a = *(struct wfs_sb **)a;
    struct wfs_sb *sb_b = *(struct wfs_sb **)b;
    return sb_a->disk_id - sb_b->disk_id;
}

int compare_addresses(const void *a, const void *b) {
    void *addr1 = *(void **)a;
    void *addr2 = *(void **)b;

    if (addr1 < addr2) return -1;
    if (addr1 > addr2) return 1;
    return 0;
}

// grabs next logical block by looping through all the bitmaps
int next_logical_block() {
    for (int disk_index = 0; disk_index < num_disks; disk_index++) {
        unsigned char *d_bitmap = (unsigned char *)((char *)maps[disk_index] +
                                                    sb_array[0]->d_bitmap_ptr);
        int num_bytes = sb_array[0]->num_data_blocks / 8;

        for (int i = 0; i < num_bytes; i++) {
            for (int bit_index = 0; bit_index < 8; bit_index++) {
                if (!(d_bitmap[i] & (1 << bit_index))) {
                    d_bitmap[i] |= (1 << bit_index);
                    return i * 8 + bit_index;
                }
            }
        }
    }

    // no free block
    return -1;
}

int print_i_bitmap(int disk_index) {
    unsigned char *i_bitmap =
        (unsigned char *)((char *)maps[disk_index] + sb_array[0]->i_bitmap_ptr);
    int num_bytes = sb_array[0]->num_inodes / 8;

    for (int i = 0; i < num_bytes; i++) {
        for (int bit_index = 0; bit_index < 8; bit_index++) {
            if (i_bitmap[i] & (1 << bit_index)) {
                int inode_num = i * 8 + bit_index;
                printf("Allocated inode: %d\n", inode_num);
            }
        }
    }
    return 0;
}

int print_d_bitmap(int disk_index) {
    unsigned char *d_bitmap =
        (unsigned char *)((char *)maps[disk_index] + sb_array[0]->d_bitmap_ptr);
    int num_bytes = sb_array[0]->num_data_blocks / 8;

    for (int i = 0; i < num_bytes; i++) {
        for (int bit_index = 0; bit_index < 8; bit_index++) {
            if (d_bitmap[i] & (1 << bit_index)) {
                int block_index = i * 8 + bit_index;
                printf("Allocated block: %d\n", block_index);
            }
        }
    }
    return 0;
}

int sync_disks() {
    // copy disk 1 to all other disks
    void *end_address = (char *)maps[0] + sb_array[0]->d_blocks_ptr +
                        (sb_array[0]->num_data_blocks * BLOCK_SIZE);

    size_t disk_size = (char *)end_address - (char *)maps[0];

    for (int i = 1; i < num_disks; i++) {
        memcpy(maps[i], maps[0], disk_size);
    }

    return 0;
}

int sync_meta_data() {
    // // copy i_bitmap and inode blocks section to every disk

    for (int i = 1; i < num_disks; i++) {
        // copy i_bitmap
        memcpy((char *)maps[i] + sb_array[i]->i_bitmap_ptr,
               (char *)maps[0] + sb_array[0]->i_bitmap_ptr,
               sb_array[0]->num_inodes / 8);

        memcpy((char *)maps[i] + sb_array[i]->i_blocks_ptr,
               (char *)maps[0] + sb_array[0]->i_blocks_ptr,
               sb_array[0]->num_inodes * BLOCK_SIZE);
    }

    return 0;
}

int is_directory(mode_t mode) {
    return (mode & S_IFMT) == S_IFDIR;  // Check if it's a directory
}

int is_regular_file(mode_t mode) {
    return (mode & S_IFMT) == S_IFREG;  // Check if it's a regular file
}

// R0
//  Global helper to map a global block number to disk and block index in RAID 0
void global_to_disk_block(off_t g, int *disk_idx, int *block_idx) {
    *disk_idx = g % num_disks;
    *block_idx = (int)(g / num_disks);
}

// Get total global blocks in RAID 0 mode
// Assuming sb_array[0]->num_data_blocks is per disk:
off_t get_total_global_blocks() {
    return sb_array[0]->num_data_blocks * num_disks;
}
// R0

/*
 * Traverse the path from the root inode to the target inode
 * Return the target inode if it exists, otherwise return -1
 * only iterate up to D_BLOCK (direct pointers)
 */
int traverse_path(const char *path, int disk_index) {
    // printf("traverse_path: %s\n", path);

    if (strcmp(path, "/") == 0) {
        return 0;  // Assuming inode 0 is the root inode
    }
    // Traverse the path from the root inode to the target inode
    // Return the target inode in the inode pointer
    char *path_copy = malloc(strlen(path) + 1);
    if (!path_copy) {
        return -ENOMEM;  // Return -ENOMEM on memory allocation error
    }

    strcpy(path_copy, path);
    // printf("Copied path: %s\n", path_copy);
    struct wfs_inode *root_inode =
        (struct wfs_inode *)((char *)maps[disk_index] +
                             sb_array[0]->i_blocks_ptr);

    // printf("Root inode address: %p\n", (void *)root_inode);

    struct wfs_inode *current_inode = root_inode;

    int inode_num = -1;
    char *token = strtok(path_copy, "/");

    while (token != NULL) {
        // reset inode_num
        inode_num = -1;
        if (strcmp(token, "") == 0) {
            printf("token is empty string\n");
            token = strtok(NULL, "/");
        }

        // printf("Processing token: %s\n", token);
        //  process token
        off_t *blocks = current_inode->blocks;

        // printf("Iterating over blocks of current inode...\n");
        // iterate through the blocks ptr array
        for (int i = 0; i < D_BLOCK; i++) {
            if (blocks[i] == -1) {
                continue;
            }
            // printf("checking block %i of inode %i\n", i, current_inode->num);

            struct wfs_dentry *entries =
                (struct wfs_dentry *)((char *)maps[disk_index] +
                                      sb_array[0]->d_blocks_ptr + blocks[i]);
            // printf("Checking block %d at address %p\n", i, (void *)entries);
            // printf("maps[0]: %p, blocks[%d]: %ld, calculated address: %p\n",
            // (void *)maps[0], i, blocks[i], (void *)((char *)maps[0] +
            // blocks[i]));

            // iterate through the directory entries

            for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
                if (strcmp(entries[j].name, "") != 0) {
                    // printf("nonempty dentry: %s with token: %s\n",
                    // entries[j].name, token);
                }

                if (strcmp(entries[j].name, token) == 0) {
                    inode_num = entries[j].num;
                    break;
                }
            }

            if (inode_num != -1) {
                break;
            }
        }

        if (inode_num == -1) {
            // printf("Traversal: Path not found: %s\n", path);
            free(path_copy);
            return -1;
        }

        // printf("Updating current inode to inode num: %d\n", inode_num);
        // file/dir exists
        current_inode =
            (struct wfs_inode *)((char *)maps[disk_index] +
                                 sb_array[0]->i_blocks_ptr +
                                 inode_num * sizeof(struct wfs_inode));

        // printf("Current inode updated to : %p\n", (void *)current_inode);

        token = strtok(NULL, "/");
    }

    // printf("Traverse: Found inode: %d for path: %s\n", inode_num, path);
    free(path_copy);
    return inode_num;
}
/*
 * Allocate a new inode in the filesystem
 * Return the inode number if successful, otherwise return -1
 *
 */
int allocate_inode(mode_t mode) {
    unsigned char *i_bitmap =
        (unsigned char *)((char *)maps[0] + sb_array[0]->i_bitmap_ptr);
    int num_bytes = sb_array[0]->num_inodes / 8;

    for (int i = 0; i < num_bytes; i++) {
        for (int bit_index = 0; bit_index < 8; bit_index++) {
            if (!(i_bitmap[i] & (1 << bit_index))) {
                // set inode as used
                i_bitmap[i] |= (1 << bit_index);
                // init new inode
                int inode_num = i * 8 + bit_index;
                struct wfs_inode *new_inode =
                    (struct wfs_inode *)((char *)maps[0] +
                                         sb_array[0]->i_blocks_ptr +
                                         inode_num * sizeof(struct wfs_inode));

                memset(new_inode, 0, sizeof(struct wfs_inode));
                new_inode->num = inode_num;
                new_inode->mode = mode;
                new_inode->uid = getuid();
                new_inode->gid = getgid();
                new_inode->size = 0;
                new_inode->nlinks = 0;
                time_t current_time = time(NULL);
                new_inode->atim = current_time;
                new_inode->mtim = current_time;
                new_inode->ctim = current_time;

                for (int i = 0; i < N_BLOCKS; i++) {
                    new_inode->blocks[i] = -1;
                }

                return inode_num;
            }
        }
    }
    return -1;
}

int free_inode(int inode_num) {
    unsigned char *i_bitmap =
        (unsigned char *)((char *)maps[0] + sb_array[0]->i_bitmap_ptr);

    // Ensure the inode number is within the valid range
    if (inode_num < 0 || inode_num >= sb_array[0]->num_inodes) {
        printf("free_inode: Inode %d is out of range.\n", inode_num);
        return -EINVAL;  // Invalid argument
    }

    // Calculate the byte index using /4 logic
    int byte_index = inode_num / 8;
    int bit_offset =
        inode_num % 8;  // index of the byte in the bitmap that has the bit
                        // coorepsonding to a specific inode

    // Debugging print to ensure calculations are correct
    printf(
        "free_inode: Calculated byte_index = %d, bit_offset = %d for inode "
        "%d\n",
        byte_index, bit_offset, inode_num);

    // Check if the inode is already free
    if (!(i_bitmap[byte_index] & (1 << bit_offset))) {
        printf("free_inode: Inode %d is already free.\n", inode_num);
        return -EINVAL;  // Invalid argument
    }

    // Mark the inode as free in the bitmap
    // printf("Before: 0x%x\n", i_bitmap[byte_index]);
    i_bitmap[byte_index] &= ~(1 << bit_offset);
    // printf("After: 0x%x\n", i_bitmap[byte_index]);

    // Clear the inode structure
    struct wfs_inode *inode =
        (struct wfs_inode *)((char *)maps[0] + sb_array[0]->i_blocks_ptr +
                             inode_num * sizeof(struct wfs_inode));
    memset(inode, 0,
           sizeof(struct wfs_inode));  // Reset all inode fields to zero

    printf("free_inode: Successfully freed inode %d.\n", inode_num);
    return 0;  // Success
}

/*
 * Allocate a new free data block in the filesystem
 * Return the off_t of data block if successful, otherwise return -1
 *
 */
off_t allocate_data_block() {
    if (raid == 0) {
        printf("Just entered allocate_data_block()\n");
        print_d_bitmap(0);

        int logical_block = next_logical_block();
        printf("Allocating logical block: %d\n", logical_block);

        if (logical_block == -1) {
            printf("Error: No logical blocks could be found!.\n");
            return -1;
        }
        int disk_index = logical_block % num_disks;
        int block_index = logical_block / num_disks;
        printf("logical block: %d, disk index: %d, block index: %d\n",
               logical_block, disk_index, block_index);

        // // go through bitmap and set logical block as used
        // unsigned char *d_bitmap = (unsigned char *)((char *)maps[disk_index] +
        //                                             sb_array[0]->d_bitmap_ptr);
        // int byte_index = block_index / 8;
        // int bit_offset = block_index % 8;
        // if (d_bitmap[byte_index] & (1 << bit_offset)) {
        //     printf(
        //         "next_logical_block returns block number %d that is already
        //         in " "used.\n", block_index);
        //     return -1;
        // }

        printf("Allocating data block %d on disk %d\n", block_index,
               disk_index);

        // Initialize the data block
        char *block_ptr = (char *)maps[disk_index] + sb_array[0]->d_blocks_ptr +
                          block_index * BLOCK_SIZE;
        memset(block_ptr, 0, BLOCK_SIZE);
        off_t offset = block_index * BLOCK_SIZE;
        printf("offset: %ld\n", offset);

        printf("Data block %d allocated at offset %ld for disk %d\n",
               block_index, offset, disk_index);

        printf("Just leaving allocate_data_block()\n");
        print_d_bitmap(0);
        printf("map[0]: %p\n", maps[0]);
        printf("map[1]: %p\n", maps[1]);

        return (off_t)((char *)maps[disk_index] - (char *)maps[0] + offset);

    } else if (raid == 1) {
        printf("Just entered allocate_data_block()\n");
        print_d_bitmap(0);
        unsigned char *d_bitmap =
            (unsigned char *)((char *)maps[0] + sb_array[0]->d_bitmap_ptr);

        int num_bytes = sb_array[0]->num_data_blocks / 8;

        for (int i = 0; i < num_bytes; i++) {
            for (int bit_index = 0; bit_index < 8; bit_index++) {
                if (!(d_bitmap[i] & (1 << bit_index))) {
                    // Mark the data block as used
                    d_bitmap[i] |= (1 << bit_index);
                    int block_num = i * 8 + bit_index;

                    printf("Allocating data block %d on disk %d\n", block_num,
                           0);

                    // Initialize the data block
                    char *block_ptr = (char *)maps[0] +
                                      sb_array[0]->d_blocks_ptr +
                                      block_num * BLOCK_SIZE;

                    memset(block_ptr, 0, BLOCK_SIZE);

                    off_t offset = block_num * BLOCK_SIZE;

                    printf("Data block %d allocated at offset %ld\n", block_num,
                           offset);
                    printf("Just leaving allocate_data_block()\n");
                    print_d_bitmap(0);

                    return offset;
                }
            }
        }
    }

    return -1;  // No free data block available
}

int free_data_block(off_t block_offset, int disk_index) {
    // Calculate block number from the offset
    int block_num = block_offset / BLOCK_SIZE;

    unsigned char *d_bitmap =
        (unsigned char *)((char *)maps[disk_index] + sb_array[0]->d_bitmap_ptr);

    int byte_index = block_num / 8;
    int bit_offset = block_num % 8;

    // Check if the block is already free
    if (!(d_bitmap[byte_index] & (1 << bit_offset))) {
        printf("Data block %d is already free.\n", block_num);
        return -1;
    }

    // Mark the block as free
    d_bitmap[byte_index] &= ~(1 << bit_offset);
    printf("Freed data block %d\n", block_num);
    return 0;
}

/*
 * Allocate a new directory entry in the inode
 * gos through data block looking for slots, otherwise allocate new data block
 * Returns 1 if successful, otherwise return -1
 */
int allocate_dentry(char *name, int parent_inode_num, int inode_num,
                    int disk_index) {
    // Allocate a new directory entry in the inode
    // Return the index of the new directory entry if successful, otherwise
    // return -1
    printf("startingallocate_dentry: %s\n", name);
    if (strlen(name) + 1 > MAX_NAME) {
        printf("Error: Name '%s' exceeds maximum length (%d).\n", name,
               MAX_NAME);
        return -1;  // Return error if name length exceeds MAX_NAME
    }

    struct wfs_inode *inode =
        (struct wfs_inode *)((char *)maps[disk_index] +
                             sb_array[0]->i_blocks_ptr +
                             parent_inode_num * sizeof(struct wfs_inode));
    for (int i = 0; i < IND_BLOCK; i++) {
        if (inode->blocks[i] == -1) {
            // allocate new data block and place dentry
            off_t datablock = allocate_data_block(disk_index);
            if (datablock < 0) {
                printf("allocdentry: dataoffset is negative.\n");
                return -1;  // No space for new data block
            }
            inode->blocks[i] = datablock;

            struct wfs_dentry *dentry =
                (struct wfs_dentry *)((char *)maps[disk_index] +
                                      sb_array[0]->d_blocks_ptr + datablock);

            dentry[0].num = inode_num;
            strcpy(dentry[0].name, name);

            // increment nlink of parent
            struct wfs_inode *inode =
                (struct wfs_inode *)((char *)maps[disk_index] +
                                     sb_array[0]->i_blocks_ptr +
                                     inode_num * sizeof(struct wfs_inode));
            inode->nlinks++;

            printf(
                "Allocated dentry %s with new block %d for inode %d in parent "
                "inode %d\n",
                name, i, inode_num, parent_inode_num);

            return 0;

        } else {
            // iterate over for free slot for dentry, else go to next block
            struct wfs_dentry *dentry =
                (struct wfs_dentry *)((char *)maps[disk_index] +
                                      sb_array[0]->d_blocks_ptr +
                                      inode->blocks[i]);

            for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
                if (dentry[j].num == 0 && dentry[j].name[0] == 0) {
                    dentry[j].num = inode_num;
                    strcpy(dentry[j].name, name);
                    // increment nlink of parent
                    struct wfs_inode *inode =
                        (struct wfs_inode *)((char *)maps[disk_index] +
                                             sb_array[0]->i_blocks_ptr +
                                             inode_num *
                                                 sizeof(struct wfs_inode));
                    inode->nlinks++;

                    printf(
                        "Allocated dentry: %s for inode %d in block %d at "
                        "index %d in inode %d\n",
                        dentry[j].name, inode_num, i, j, parent_inode_num);
                    return 0;
                }
            }
        }
    }

    printf("Error: No space for new directory entry.\n");
    return -1;
}

// count the filesystem id of the 2nd 3rd 4th be differnt
int validate_disks(struct wfs_sb *sb_array[], char *disk_files[],
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
        // printf("Disk %d: filesystem_id = %d\n", i,
        // sb_array[i]->filesystem_id);
        if (sb_array[i]->filesystem_id != expected_filesystem_id) {
            printf(
                "Error: Disk %d does not belong to the same filesystem as Disk "
                "0.\n",
                i);
            return -1;
        }
    }

    // Make sure that order or name change doesnt affect anything
    qsort(sb_array, disk_count, sizeof(struct wfs_sb *), compare_disks);

    // Validate disk_id sequence
    for (int i = 0; i < disk_count; i++) {
        // printf("Debug: Disk %d has disk_id %d (expected %d).\n", i,
        //        sb_array[i]->disk_id, i);
        if (sb_array[i]->disk_id != i) {
            printf(
                "Error: Disk %d has an unexpected disk_id %d (expected %d).\n",
                i, sb_array[i]->disk_id, i);
            return -1;
        }
    }

    return 0;
}

int init_disks(char *disk_files[], int disk_count) {
    int fd;
    for (int i = 0; i < disk_count; i++) {
        char *dir_name = disk_files[i];
        fd = open(dir_name, O_RDWR);
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
        maps[i] =
            mmap(NULL, filesize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
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
        num_disks++;
    }

    int expected_disks = sb_array[0]->num_disks;
    if (validate_disks(sb_array, disk_files, disk_count, expected_disks) != 0) {
        printf("Disk validation failed. Aborting mount.\n");
        return -1;
    }

    qsort(maps, disk_count, sizeof(void *), compare_addresses);

    // Print sorted addresses for verification
    for (int i = 0; i < disk_count; i++) {
        printf("maps[%d]: %p\n", i, maps[i]);
    }

    return 0;
}

int unlink_helper(const char *path, int disk_index) {
    printf("unlink: %s\n", path);

    // Validate the path
    if (!path || strlen(path) == 0 || strcmp(path, "/") == 0) {
        printf("Error in unlink: Invalid path.\n");
        return -EINVAL;  // Invalid argument
    }

    // Make a modifiable copy of the path
    char *path_copy = malloc(strlen(path) + 1);
    if (!path_copy) {
        printf("Error in unlink: No space for path copy.\n");
        return -ENOMEM;  // Out of memory
    }
    strcpy(path_copy, path);

    // Find parent directory path and file name
    char *last_slash = strrchr(path_copy, '/');
    if (!last_slash) {
        printf("Error in unlink: Invalid path.\n");
        free(path_copy);
        return -ENOENT;  // No such file or directory
    }

    // Initialize parent_path and file_name
    char *parent_path = NULL;
    char *file_name = NULL;

    // Handle root directory case
    if (last_slash == path_copy) {
        parent_path = "/";
        file_name = last_slash + 1;
    } else {
        *last_slash = '\0';  // Split the path into parent and file name
        parent_path = path_copy;
        file_name = last_slash + 1;
    }

    // Traverse to the parent directory
    int parent_inode_num = traverse_path(parent_path, 0);
    if (parent_inode_num < 0) {
        printf("Error in unlink: Parent directory not found.\n");
        free(path_copy);
        return parent_inode_num;
    }

    // Get the parent inode
    struct wfs_inode *parent_inode =
        (struct wfs_inode *)((char *)maps[0] + sb_array[0]->i_blocks_ptr +
                             parent_inode_num * sizeof(struct wfs_inode));

    // Traverse to the file to be unlinked
    int file_inode_num = traverse_path(path, disk_index);
    if (file_inode_num < 0) {
        printf("Error in unlink: File not found: %s\n", path);
        free(path_copy);
        return -ENOENT;
    }

    // Get the file's inode
    struct wfs_inode *file_inode =
        (struct wfs_inode *)((char *)maps[0] + sb_array[0]->i_blocks_ptr +
                             file_inode_num * sizeof(struct wfs_inode));

    // Check if the target is a regular file
    if (!is_regular_file(file_inode->mode)) {
        printf("Error in unlink: Path is not a file: %s\n", path);
        free(path_copy);
        return -EISDIR;  // Is a directory
    }

    // clear inode datablock and its entry(thats all)
    //  Remove the file's directory entry from the parent directory
    for (int i = 0; i < IND_BLOCK; i++) {
        if (parent_inode->blocks[i] != -1) {
            struct wfs_dentry *entries =
                (struct wfs_dentry *)((char *)maps[0] +
                                      sb_array[0]->d_blocks_ptr +
                                      parent_inode->blocks[i]);
            // int num_entries = 0;
            for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
                // if (entries[j].num != 0) {
                //     num_entries++;
                // }

                if (entries[j].num == file_inode_num) {
                    memset(
                        &entries[j], 0,
                        sizeof(
                            struct wfs_dentry));  // Clear the directory entry
                    printf("unlink: Removed directory entry for %s\n",
                           file_name);
                    break;
                }
            }
            // if (num_entries == 0) {
            //     free_data_block(parent_inode->blocks[i], disk_index);
            //     parent_inode->blocks[i] = -1;
            // }
        }
    }
    // free direct blocks of the file
    for (int i = 0; i < IND_BLOCK; i++) {
        off_t block_offset = file_inode->blocks[i];
        printf("unlink: blocks[%d] = %ld for inode %d\n", i, block_offset,
               file_inode_num);

        if (file_inode->blocks[i] != -1) {
            free_data_block(file_inode->blocks[i], disk_index);
            file_inode->blocks[i] = -1;
        }
    }

    // free indirect if needed
    if (file_inode->blocks[IND_BLOCK] != -1) {
        off_t *indirect_blocks =
            (off_t *)((char *)maps[disk_index] + sb_array[0]->d_blocks_ptr +
                      file_inode->blocks[IND_BLOCK]);

        for (int i = 0; i < N_POINTERS; i++) {
            if (indirect_blocks[i] != 0) {
                free_data_block(indirect_blocks[i], disk_index);
                indirect_blocks[i] = 0;
            }
        }
        // Free the indirect block itself
        free_data_block(file_inode->blocks[IND_BLOCK], disk_index);
        file_inode->blocks[IND_BLOCK] = -1;
    }

    printf("unlink: No more hard links, freeing inode %d.\n", file_inode_num);
    int free_result = free_inode(file_inode_num);
    if (free_result < 0) {
        printf("Error in unlink: Failed to free inode for %s\n", path);
        free(path_copy);
        return free_result;
    }

    // Update parent directory metadata
    // parent_inode->mtim = time(NULL); do we need to update time?

    // decremnt the size of the parent directory

    free(path_copy);
    printf("unlink: Successfully unlinked file: %s\n", path);
    return 0;
}

static int wfs_unlink(const char *path) {
    printf("wfs_unlink: %s\n", path);
    int disk = 0;
    if (raid < 3) {
        int result = unlink_helper(path, disk);
        sync_disks();
        // print_i_bitmap(disk);
        print_d_bitmap(disk);

        return result;
    } else if (raid == 0) {
        // Implement RAID 0 logic if applicable
    } else if (raid == 2) {
        // Implement RAID 2 logic if applicable
    }

    return -1;
}

int rmdir_helper(const char *path, int disk_index) {
    printf("rmdir: %s\n", path);

    // Validate the path(path(null), path empty, path if root which cant be
    // removed
    if (!path || strlen(path) == 0 || strcmp(path, "/") == 0) {
        printf("Error in rmdir: Invalid path.\n");
        return -EINVAL;  // Invalid argument
    }

    // Make a modifiable copy of the path
    char *path_copy = malloc(strlen(path) + 1);
    if (!path_copy) {
        printf("Error in rmdir: No space for path copy.\n");
        return -ENOMEM;  // Out of memory
    }
    strcpy(path_copy, path);

    // Find parent directory path and directory name
    char *last_slash = strrchr(path_copy, '/');
    if (!last_slash) {
        printf("Error in rmdir: Invalid path.\n");
        free(path_copy);
        return -ENOENT;  // No such file or directory
    }

    // Initialize parent_path and dir_name
    char *parent_path = NULL;
    char *dir_name = NULL;

    // if the last / is the first char /dir the parent path is root /
    if (last_slash == path_copy) {
        // Root case
        parent_path = "/";
        dir_name = last_slash + 1;
        // othereise split path copt into parent path and dir name
    } else {
        *last_slash = '\0';  // Split the path into parent and directory name
        parent_path = path_copy;
        dir_name = last_slash + 1;
    }

    // Traverse to the parent directory-find inode numver of parent director
    int parent_inode_num = traverse_path(parent_path, 0);
    if (parent_inode_num < 0) {
        printf("Error in rmdir: Parent directory not found.\n");
        free(path_copy);
        return parent_inode_num;
    }

    // get the parent inode
    struct wfs_inode *parent_inode =
        (struct wfs_inode *)((char *)maps[0] + sb_array[0]->i_blocks_ptr +
                             parent_inode_num * sizeof(struct wfs_inode));

    // Traverse to the directory to be removed
    int dir_inode_num = traverse_path(path, 0);
    if (dir_inode_num < 0) {
        printf("Error in rmdir: Directory not found: %s\n", path);
        free(path_copy);
        return -ENOENT;
    }

    // get the inode of the directory
    struct wfs_inode *dir_inode =
        (struct wfs_inode *)((char *)maps[0] + sb_array[0]->i_blocks_ptr +
                             dir_inode_num * sizeof(struct wfs_inode));

    // Check if the target is a directory
    if (!is_directory(dir_inode->mode)) {
        printf("Error in rmdir: Path is not a directory: %s\n", path);
        free(path_copy);
        return -ENOTDIR;  // Not a directory
    }

    // Check if the directory is empty (apart from "." and "..")
    // loop thru the directory data blocks
    for (int i = 0; i < D_BLOCK; i++) {
        if (dir_inode->blocks[i] != -1) {
            struct wfs_dentry *entries =
                (struct wfs_dentry *)((char *)maps[0] +
                                      sb_array[0]->d_blocks_ptr +
                                      dir_inode->blocks[i]);
            for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
                if (entries[j].name[0] != '\0' &&
                    strcmp(entries[j].name, ".") != 0 &&
                    strcmp(entries[j].name, "..") != 0) {
                    printf("Error in rmdir: Directory is not empty: %s\n",
                           path);
                    free(path_copy);
                    return -ENOTEMPTY;  // Directory not empty
                }
            }
        }
    }

    // Remove the directory entry from the parent directory
    for (int i = 0; i < D_BLOCK; i++) {
        if (parent_inode->blocks[i] != -1) {
            struct wfs_dentry *entries =
                (struct wfs_dentry *)((char *)maps[0] +
                                      sb_array[0]->d_blocks_ptr +
                                      parent_inode->blocks[i]);
            for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
                if (entries[j].num == dir_inode_num) {
                    memset(
                        &entries[j], 0,
                        sizeof(
                            struct wfs_dentry));  // Clear the directory entry
                    printf("rmdir: Removed directory entry for %s\n", dir_name);
                    break;
                }
            }
        }
    }

    // Decrement the nlinks and free the inode if necessary
    dir_inode->nlinks--;
    printf("rmdir: Decremented nlinks for inode %d, remaining links: %d\n",
           dir_inode_num, dir_inode->nlinks);
    if (dir_inode->nlinks >= 0) {
        printf("rmdir: No more hard links, freeing inode %d.\n", dir_inode_num);
        int free_result = free_inode(dir_inode_num);
        if (free_result < 0) {
            printf("Error in rmdir: Failed to free inode for %s\n", path);
            free(path_copy);
            return free_result;
        }
    }

    parent_inode->size--;
    // should we do something with time here?

    printf("rmdir: Successfully removed directory: %s\n", path);
    free(path_copy);
    return 0;
}
static int wfs_rmdir(const char *path) {
    printf("rmdir: %s\n", path);
    int disk = 0;
    if (raid < 3) {
        int result = rmdir_helper(path, disk);
        sync_disks();
        return result;
    } else if (raid == 0) {
        // Implement RAID 0 logic if applicable
    } else if (raid == 2) {
        // Implement RAID 2 logic if applicable
    }

    return -1;
}

// fuse functions
static int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi) {
    (void)offset;
    (void)fi;

    int dir_inode_num = traverse_path(path, 0);
    if (dir_inode_num < 0) {
        printf("Error in readdir: Directory not found: %s\n", path);
        return -ENOENT;
    }

    struct wfs_inode *dir_inode =
        (struct wfs_inode *)((char *)maps[0] + sb_array[0]->i_blocks_ptr +
                             dir_inode_num * sizeof(struct wfs_inode));
    if (!is_directory(dir_inode->mode)) {
        printf("Error in readdir: Path is not a directory: %s\n", path);
        return -ENOTDIR;
    }

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    for (int i = 0; i < D_BLOCK; i++) {
        if (dir_inode->blocks[i] == -1) {
            // skip unused blocks
            continue;
        }

        // get directories in this block
        struct wfs_dentry *entries =
            (struct wfs_dentry *)((char *)maps[0] + sb_array[0]->d_blocks_ptr +
                                  dir_inode->blocks[i]);

        for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
            // Non-empty entry
            if (entries[j].name[0] != '\0') {
                filler(buf, entries[j].name, NULL,
                       0);  // Add entry to the result buffer
            }
        }
    }
    return 0;
}
static int mknod_helper(const char *path, mode_t mode, int disk_index) {
    mode_t d_mode = S_IFREG | mode;  // change: change the mode
    printf("mkdir: %s\n", path);
    if (!path || strlen(path) == 0 || strcmp(path, "/") == 0) {
        printf("Error in mkdir: Invalid path.\n");
        return -ENOENT;
    }

    // Allocate a copy of the path to manipulate
    char *path_copy = malloc(strlen(path) + 1);
    if (!path_copy) {
        printf("Error in mkdir: No space for path copy.\n");
        return -ENOSPC;
    }

    strcpy(path_copy, path);

    // Find parent directory path and directory name
    char *last_slash = strrchr(path_copy, '/');

    if (!last_slash) {
        printf("Error in mkdir: Invalid path.\n");
        free(path_copy);
        return -ENOENT;
    }

    // Initialize parent_path and dir_name
    char *parent_path = NULL;
    char *dir_name = NULL;

    if (last_slash == path_copy) {
        // root case
        parent_path = "/";
        dir_name = ((char *)last_slash + 1);
    } else {
        *last_slash = '\0';
        parent_path = path_copy;
        dir_name = ((char *)last_slash + 1);
    }

    // printf("parent_path: %s\n", parent_path);
    // printf("dir_name: %s\n", dir_name);

    if (strlen(dir_name) == 0) {
        printf("Error in mkdir: Invalid directory name len is 0.\n");
        free(path_copy);
        return -ENOENT;
    }

    int parent_inode_num = traverse_path(parent_path, 0);
    // printf("parent_inode_num: %d\n", parent_inode_num);
    if (parent_inode_num < 0) {
        printf("Error in mkdir: Parent directory does not exist.\n");
        free(path_copy);
        return parent_inode_num;
    }

    struct wfs_inode *parent_inode =
        (struct wfs_inode *)((char *)maps[0] + sb_array[0]->i_blocks_ptr +
                             parent_inode_num * sizeof(struct wfs_inode));

    // check if dir already exists
    int existing_inode_num = traverse_path(path, disk_index);
    // printf("existing_inode_num: %d\n", existing_inode_num);
    if (existing_inode_num >= 0) {
        free(path_copy);
        printf("Directory already exists.\n");
        return -EEXIST;  // Directory already exists
    }

    // allocate a new inode
    int new_inode_num = allocate_inode(d_mode);
    // printf("new_inode_num: %d\n", new_inode_num);
    if (new_inode_num < 0) {
        printf("Failed to allocate new inode.\n");
        free(path_copy);
        return -ENOSPC;
    }

    struct wfs_inode *new_inode =
        (struct wfs_inode *)((char *)maps[0] + sb_array[0]->i_blocks_ptr +
                             new_inode_num * sizeof(struct wfs_inode));
    new_inode->nlinks = 1;  // change: set nlinks to 1

    if (raid == 0) {
        sync_meta_data(disk_index);
    }

    if (allocate_dentry(dir_name, parent_inode_num, new_inode_num, disk_index) <
        0) {
        printf(
            "Error in alloc dentry: No space in parent directory for new "
            "entry.\n");
        free(path_copy);
        return -ENOSPC;  // No space in parent directory for new entry
    }
    // Update parent directory's metadata
    // parent_inode->nlinks++;
    parent_inode->mtim = time(NULL);
    // change: dont inc size becase of file

    free(path_copy);
    printf("Successfully created file: %s with inode %d\n", path,
           new_inode_num);
    return 0;
}

static int wfs_mknod(const char *path, mode_t mode, dev_t dev) {
    (void)dev;
    printf("mknod: %s\n", path);
    int disk = 0;
    if (raid == 1) {
        int i = mknod_helper(path, mode, disk);
        sync_disks();
        // print_i_bitmap(disk);
        print_d_bitmap(disk);
        return i;
    } else if (raid == 0) {
        int i = mknod_helper(path, mode, disk);
        sync_meta_data();
        print_i_bitmap(0);
        print_d_bitmap(0);
        // printf("printing out inode of new directory \n");
        // struct wfs_inode *inode =
        //     (struct wfs_inode *)((char *)maps[0] + sb_array[0]->i_blocks_ptr
        //     +
        //                          i * sizeof(struct wfs_inode));
        // printf("inode->mode: %d\n", inode->mode);
        // printf("inode->uid: %d\n", inode->uid);
        // printf("inode->gid: %d\n", inode->gid);
        // printf("inode->size: %ld\n", inode->size);
        // printf("inode->nlinks: %d\n", inode->nlinks);
        // printf("inode->atim: %ld\n", inode->atim);

        return i;

    } else if (raid == 2) {
    }

    return -1;
}

int mkdir_helper(const char *path, mode_t mode, int disk_index) {
    mode_t d_mode = S_IFDIR | mode;
    if (strlen(path) == 0 || strcmp(path, "/") == 0) {
        printf("Error in mkdir: Invalid path.\n");
        return -ENOENT;
    }

    // Allocate a copy of the path to manipulate
    char *path_copy = malloc(strlen(path) + 1);
    if (!path_copy) {
        printf("Error in mkdir: No space for path copy.\n");
        return -ENOSPC;
    }

    strcpy(path_copy, path);

    // Find parent directory path and directory name
    char *last_slash = strrchr(path_copy, '/');

    if (!last_slash) {
        printf("Error in mkdir: Invalid path.\n");
        free(path_copy);
        return -ENOENT;
    }

    // Initialize parent_path and dir_name
    char *parent_path = NULL;
    char *dir_name = NULL;

    if (last_slash == path_copy) {
        // root case
        parent_path = "/";
        dir_name = ((char *)last_slash + 1);
    } else {
        *last_slash = '\0';
        parent_path = path_copy;
        dir_name = ((char *)last_slash + 1);
    }

    if (strlen(dir_name) == 0) {
        printf("Error in mkdir: Invalid directory name len is 0.\n");
        free(path_copy);
        return -ENOENT;
    }

    int parent_inode_num = traverse_path(parent_path, 0);
    if (parent_inode_num < 0) {
        printf("Error in mkdir: Parent directory does not exist.\n");
        free(path_copy);
        return parent_inode_num;
    }

    struct wfs_inode *parent_inode =
        (struct wfs_inode *)((char *)maps[0] + sb_array[0]->i_blocks_ptr +
                             parent_inode_num * sizeof(struct wfs_inode));

    // check if dir already exists
    int existing_inode_num = traverse_path(path, disk_index);
    // printf("existing_inode_num: %d for disk %d\n", existing_inode_num,
    if (existing_inode_num >= 0) {
        free(path_copy);
        printf("Directory already exists.\n");
        return -EEXIST;  // Directory already exists
    }

    // allocate a new inode
    int new_inode_num = allocate_inode(d_mode);
    // printf("new_inode_num: %d\n", new_inode_num);
    if (new_inode_num < 0) {
        printf("Failed to allocate new inode.\n");
        free(path_copy);
        return -ENOSPC;
    }

    struct wfs_inode *new_inode =
        (struct wfs_inode *)((char *)maps[0] + sb_array[0]->i_blocks_ptr +
                             new_inode_num * sizeof(struct wfs_inode));
    new_inode->nlinks = 1;

    if (raid == 0) {
        sync_meta_data(disk_index);
    }

    if (allocate_dentry(dir_name, parent_inode_num, new_inode_num, disk_index) <
        0) {
        printf(
            "Error in alloc dentry: No space in parent directory for "
            "new "
            "entry.\n");
        free(path_copy);
        return -ENOSPC;  // No space in parent directory for new entry
    }
    // Update parent directory's metadata
    // parent_inode->nlinks++;
    parent_inode->mtim = time(NULL);
    parent_inode->size += 1;

    free(path_copy);
    return 0;
}

static int wfs_mkdir(const char *path, mode_t mode) {
    printf("raid: %d\n", raid);
    printf("mkdir: %s\n", path);
    int disk = 0;
    if (raid == 1) {
        print_i_bitmap(0);
        int i = mkdir_helper(path, mode, disk);
        sync_disks();
        print_i_bitmap(0);
        return i;
    } else if (raid == 0) {
        int i = mkdir_helper(path, mode, disk);
        sync_meta_data();
        print_i_bitmap(0);
        print_d_bitmap(0);
        // printf("printing out inode of new directory \n");
        // struct wfs_inode *inode =
        //     (struct wfs_inode *)((char *)maps[0] + sb_array[0]->i_blocks_ptr
        //     +
        //                          i * sizeof(struct wfs_inode));
        // printf("inode->mode: %d\n", inode->mode);
        // printf("inode->uid: %d\n", inode->uid);
        // printf("inode->gid: %d\n", inode->gid);
        // printf("inode->size: %ld\n", inode->size);
        // printf("inode->nlinks: %d\n", inode->nlinks);
        // printf("inode->atim: %ld\n", inode->atim);

        return i;

    } else if (raid == 2) {
    }

    return -1;
}

static int wfs_getattr(const char *path, struct stat *stbuf) {
    // Implementation of getattr function to retrieve file attributes
    // Fill stbuf structure with the attributes of the file/directory
    // indicated by path parse pa
    // printf("getattr: %s\n", path);
    int inode_num = traverse_path(path, 0);
    if (inode_num < 0) {
        return -ENOENT;  // Return -ENOENT if the file does not exist
    }

    struct wfs_inode *inode =
        (struct wfs_inode *)((char *)maps[0] + sb_array[0]->i_blocks_ptr +
                             inode_num * sizeof(struct wfs_inode));

    stbuf->st_mode = inode->mode;
    stbuf->st_uid = inode->uid;
    stbuf->st_gid = inode->gid;
    stbuf->st_size = inode->size;
    stbuf->st_nlink = inode->nlinks;
    stbuf->st_atime = inode->atim;
    stbuf->st_mtime = inode->mtim;
    stbuf->st_ctime = inode->ctim;

    return 0;  // Return 0 on success
}
static int read_helper(const char *path, char *buf, size_t size, off_t offset,
                       struct fuse_file_info *fi) {
    (void)fi;
    printf("read: %s\n", path);
    printf("size: %zu, offset: %ld\n", size, offset);
    // Find the inode number of the file specified by the path
    int file_inode_num = traverse_path(path, 0);
    if (file_inode_num < 0) {
        printf("Error in read: File not found: %s\n", path);
        return -ENOENT;  // File not found
    }

    struct wfs_inode *file_inode =
        (struct wfs_inode *)((char *)maps[0] + sb_array[0]->i_blocks_ptr +
                             file_inode_num * sizeof(struct wfs_inode));
    int direct_bytes = (D_BLOCK + 1) * BLOCK_SIZE;  // 3584

    // Ensure the read does not go beyond the file's size
    if (offset >= file_inode->size) {
        printf("Error in read: Offset is beyond file size.\n");
        return 0;  // EOF
    }

    if (offset + size > file_inode->size) {
        size = file_inode->size -
               offset;  // Adjust size to read up to the file's end
    }

    // size_t original_size = size;
    size_t total_read = 0;

    // Case 1: Read from direct blocks
    if (offset < direct_bytes) {
        // int num_blocks = (size + (BLOCK_SIZE - 1)) / BLOCK_SIZE;
        int starting_block = offset / BLOCK_SIZE;
        int byte_offset = offset % BLOCK_SIZE;

        for (int i = starting_block; i < IND_BLOCK && size > 0; i++) {
            if (file_inode->blocks[i] == -1) {
                printf(
                    "read: Attempted to read from an unallocated block "
                    "%d.\n",
                    i);
                break;  // Stop if the block is not allocated
            }

            char *block_ptr = (char *)maps[0] + sb_array[0]->d_blocks_ptr +
                              file_inode->blocks[i];
            size_t read_size = BLOCK_SIZE - byte_offset;
            if (read_size > size) {
                read_size = size;
            }

            memcpy(buf, block_ptr + byte_offset, read_size);
            buf += read_size;
            size -= read_size;
            total_read += read_size;
            byte_offset = 0;  // Reset offset for the next block
        }

        offset = direct_bytes;  // adjust ofset to transiton to indirect blocks
    }

    // Case 2: Read from indirect blocks if needed
    if (size > 0 && offset >= direct_bytes) {
        if (file_inode->blocks[IND_BLOCK] == -1) {
            printf(
                "read: Attempted to read from an unallocated indirect "
                "block. IND_BLOCK"
                "Total read: %zu\n",
                total_read);
            return total_read;  // Stop if the indirect block is not
                                // allocated
        }
        off_t *indirect_blocks =
            (off_t *)((char *)maps[0] + sb_array[0]->d_blocks_ptr +
                      file_inode->blocks[IND_BLOCK]);
        int starting_indirect_block = (offset - direct_bytes) / BLOCK_SIZE;
        int byte_offset = (offset - direct_bytes) % BLOCK_SIZE;

        printf("read: Starting indirect block: %d\n", starting_indirect_block);
        printf("read: Byte offset: %d\n", byte_offset);

        for (int i = starting_indirect_block; i < N_POINTERS && size > 0; i++) {
            if (indirect_blocks[i] == 0) {
                printf(
                    "read: Attempted to read from an unallocated indirect "
                    "block %d %p.\n",
                    i, (void *)indirect_blocks);
                break;  // Stop if the indirect block is not allocated
            }

            char *block_ptr = (char *)maps[0] + sb_array[0]->d_blocks_ptr +
                              indirect_blocks[i];
            size_t read_size = BLOCK_SIZE - byte_offset;
            printf("read: Read size: %zu\n", read_size);
            if (read_size > size) {
                read_size = size;
            }
            printf("read: Read size after adjustment: %zu\n", read_size);

            memcpy(buf, block_ptr + byte_offset, read_size);
            buf += read_size;
            size -= read_size;
            total_read += read_size;
            printf("read: Total read so far: %zu\n", total_read);
            byte_offset = 0;  // Reset offset for the next block
        }
    }

    printf("read: Successfully read %zu bytes from %s\n", total_read, path);
    return total_read;  // Return the total number of bytes read
}
static int wfs_read(const char *path, char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi) {
    (void)fi;
    if (raid < 3) {
        int i = read_helper(path, buf, size, offset, fi);
        sync_disks();
        return i;

    } else if (raid == 0) {
    } else if (raid == 2) {
    }

    return -1;
}
static int write_helper(const char *path, const char *buf, size_t size,
                        off_t offset, struct fuse_file_info *fi) {
    (void)fi;
    printf("write: %s\n", path);
    printf("size: %zu\n", size);
    // finds the file inode
    int file_inode_num = traverse_path(path, 0);
    if (file_inode_num < 0) {
        printf("Error in write: File not found: %s\n", path);
        return -ENOENT;
    }

    // saves original size of file
    int original_size = size;
    struct wfs_inode *file_inode =
        (struct wfs_inode *)((char *)maps[0] + sb_array[0]->i_blocks_ptr +
                             file_inode_num * sizeof(struct wfs_inode));
    // total size that can be acessed by the data blocks
    int direct_bytes = (D_BLOCK + 1) * BLOCK_SIZE;

    // direct block case
    if (offset + size <= direct_bytes) {
        // write data into block
        int byte_offset = offset % BLOCK_SIZE;
        int num_blocks = ((size + byte_offset) + (BLOCK_SIZE - 1)) / BLOCK_SIZE;
        int starting_block = offset / BLOCK_SIZE;

        for (int i = starting_block; i < starting_block + num_blocks; i++) {
            if (file_inode->blocks[i] == -1) {
                // allocate new data block
                off_t datablock = allocate_data_block();
                if (datablock < 0) {
                    return -ENOSPC;  // No space for new data block
                }
                file_inode->blocks[i] = datablock;
            }

            char *block_ptr = (char *)maps[0] + sb_array[0]->d_blocks_ptr +
                              file_inode->blocks[i];
            int writeSize = BLOCK_SIZE - byte_offset;
            if (i == starting_block + num_blocks - 1) {
                writeSize = size;
            }
            memcpy(block_ptr + byte_offset, buf, writeSize);
            size -= writeSize;
            byte_offset = 0;
            buf += writeSize;
        }

        if (size != 0) {
            printf("Error in write: Incomplete write.\n");
            return -1;
        }

        if (offset + original_size > file_inode->size) {
            off_t curr_size = file_inode->size;
            file_inode->size += (offset + original_size) - curr_size;
        } else {
            file_inode->size = original_size;
        }
        time_t current_time = time(NULL);
        file_inode->atim = current_time;
        file_inode->mtim = current_time;

        return original_size;

    } else {
        // write into indirect block
        // write into direct blocks first then handle remaining size with
        // indirect block

        // write into both direct and indirect blocks
        if (offset < direct_bytes) {
            // write into direct blocks first
            int direct_bytes_size = direct_bytes - offset;
            int remaining_size = size - direct_bytes_size;

            int byte_offset = offset % BLOCK_SIZE;
            int num_blocks =
                (byte_offset + direct_bytes_size + (BLOCK_SIZE - 1)) /
                BLOCK_SIZE;
            int starting_block = offset / BLOCK_SIZE;

            for (int i = starting_block; i < starting_block + num_blocks; i++) {
                if (file_inode->blocks[i] == -1) {
                    // allocate new data block
                    off_t datablock = allocate_data_block();
                    if (datablock < 0) {
                        return -ENOSPC;  // No space for new data block
                    }
                    file_inode->blocks[i] = datablock;
                }

                char *block_ptr = (char *)maps[0] + sb_array[0]->d_blocks_ptr +
                                  file_inode->blocks[i];
                int writeSize = BLOCK_SIZE - byte_offset;  // 512
                memcpy(block_ptr + byte_offset, buf, writeSize);
                size -= writeSize;  // 4416
                byte_offset = 0;
                buf += writeSize;
            }
            // write into indirect block
            if (file_inode->blocks[IND_BLOCK] == -1) {
                // allocate new data block
                off_t datablock = allocate_data_block();
                if (datablock < 0) {
                    return -ENOSPC;  // No space for new data block
                }
                file_inode->blocks[IND_BLOCK] = datablock;
            }
            off_t *indirect_blocks =
                (off_t *)((char *)maps[0] + sb_array[0]->d_blocks_ptr +
                          file_inode->blocks[IND_BLOCK]);
            int num_indirect_blocks =
                (remaining_size + (BLOCK_SIZE - 1)) / BLOCK_SIZE;  // 9 PAGES

            if (num_indirect_blocks > N_POINTERS) {
                printf("Error in write: File too large.\n");
                return -ENOSPC;
            }
            printf("remaining_size: %d\n", remaining_size);

            for (int i = 0; i < num_indirect_blocks; i++) {  // 9 TIMES
                if (indirect_blocks[i] == 0) {
                    // allocate new data block
                    off_t datablock = allocate_data_block();
                    if (datablock < 0) {
                        return -ENOSPC;  // No space for new data block
                    }
                    indirect_blocks[i] = datablock;
                }

                char *block_ptr = (char *)maps[0] + sb_array[0]->d_blocks_ptr +
                                  indirect_blocks[i];
                int writeSize = BLOCK_SIZE;  // 512
                if (i == num_indirect_blocks - 1) {
                    writeSize = size;
                }
                printf("writeSize: %d for %i\n", writeSize, i);
                memcpy(block_ptr, buf, writeSize);
                size -= writeSize;
                buf += writeSize;
            }

            if (size != 0) {
                printf("Error in write: Incomplete write.\n");
                return -1;
            }

            if (offset + original_size > file_inode->size) {
                off_t curr_size = file_inode->size;
                file_inode->size += (offset + original_size) - curr_size;
            } else {
                file_inode->size = original_size;
            }
            printf("file_inode->size: %ld\n", file_inode->size);
            time_t current_time = time(NULL);
            file_inode->atim = current_time;
            file_inode->mtim = current_time;

            return original_size;

        } else {
            // write starting only from indirect block
            if (file_inode->blocks[IND_BLOCK] == -1) {
                // allocate new data block
                off_t datablock = allocate_data_block();
                if (datablock < 0) {
                    return -ENOSPC;  // No space for new data block
                }
                file_inode->blocks[IND_BLOCK] = datablock;
            }
            off_t *indirect_blocks =
                (off_t *)((char *)maps[0] + sb_array[0]->d_blocks_ptr +
                          file_inode->blocks[IND_BLOCK]);
            int num_indirect_blocks = (size + (BLOCK_SIZE - 1)) / BLOCK_SIZE;

            int starting_block = offset / BLOCK_SIZE - IND_BLOCK;
            int byte_offset = offset % BLOCK_SIZE;

            printf(
                "write (indirect only): size = %zu, offset = %ld, "
                "starting_block = %d, byte_offset = %d\n",
                size, offset, starting_block, byte_offset);
            printf("Number of indirect blocks to write: %d\n",
                   num_indirect_blocks);

            for (int i = starting_block;
                 i < starting_block + num_indirect_blocks; i++) {
                if (indirect_blocks[i] == 0) {
                    // allocate new data block
                    off_t datablock = allocate_data_block();
                    if (datablock < 0) {
                        return -ENOSPC;  // No space for new data block
                    }
                    indirect_blocks[i] = datablock;
                    printf("writing indirect_blocks[%d] = %ld %p\n", i,
                           indirect_blocks[i], (void *)indirect_blocks);
                }

                char *block_ptr = (char *)maps[0] + sb_array[0]->d_blocks_ptr +
                                  indirect_blocks[i];
                int writeSize = BLOCK_SIZE - byte_offset;
                if (i == starting_block + num_indirect_blocks - 1) {
                    writeSize = size;
                }
                printf("Writing %d bytes to block %d at byte_offset %d\n",
                       writeSize, i, byte_offset);
                printf("block ptr address: %p for block %d\n", block_ptr, i);
                memcpy(block_ptr + byte_offset, buf, writeSize);
                size -= writeSize;
                byte_offset = 0;
                buf += writeSize;
                printf("Remaining size to write: %zu\n", size);
            }

            if (size != 0) {
                printf("Error in write: Incomplete write.\n");
                return -1;
            }

            if (offset + original_size > file_inode->size) {
                off_t curr_size = file_inode->size;
                file_inode->size += (offset + original_size) - curr_size;
            } else {
                file_inode->size = original_size;
            }
            printf("file_inode->size: %ld\n", file_inode->size);
            time_t current_time = time(NULL);
            file_inode->atim = current_time;
            file_inode->mtim = current_time;

            return original_size;
        }
    }
}

/*
 * Write data to a file
 * Return the number of bytes written if successful, otherwise return error
 * code
 */
static int wfs_write(const char *path, const char *buf, size_t size,
                     off_t offset, struct fuse_file_info *fi) {
    (void)fi;
    if (raid == 1) {
        int i = write_helper(path, buf, size, offset, fi);
        sync_disks();
        return i;
    } else if (raid == 0) {
        int i = write_helper(path, buf, size, offset, fi);
        sync_meta_data();
        return i;
    } else if (raid == 2) {
    }

    return -1;
}

// FUSE operations
static struct fuse_operations ops = {
    .getattr = wfs_getattr,
    .mkdir = wfs_mkdir,
    .mknod = wfs_mknod,
    .readdir = wfs_readdir,
    .write = wfs_write,
    .rmdir = wfs_rmdir,
    .unlink = wfs_unlink,
    .read = wfs_read
    // Add other functions (read, write, mkdir, etc.) here as needed
};

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
    // printf("Disks:\n");
    for (int i = 0; i < num_disks; i++) {
        printf("  %s\n", disks[i]);
    }

    // Pass FUSE options to fuse_main
    fuse_args[fuse_argc++] = mount_point;  // Add mount point to FUSE args
    fuse_args[fuse_argc] = NULL;           // Null-terminate for FUSE
    init_disks(disks, num_disks);
    printf("Starting Fuse in raid %d... \n", raid);
    print_d_bitmap(0);
    print_i_bitmap(0);

    return fuse_main(fuse_argc, fuse_args, &ops, NULL);
}