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
int num_disks = 0;
int raid = -1;


// helper functions
int compare_disks(const void *a, const void *b) {
  struct wfs_sb *sb_a = *(struct wfs_sb **)a;
  struct wfs_sb *sb_b = *(struct wfs_sb **)b;
  return sb_a->disk_id - sb_b->disk_id;
}

int is_directory(mode_t mode) {
  return (mode & S_IFMT) == S_IFDIR; // Check if it's a directory
}

int is_regular_file(mode_t mode) {
  return (mode & S_IFMT) == S_IFREG; // Check if it's a regular file
}


/*
 * Traverse the path from the root inode to the target inode
 * Return the target inode if it exists, otherwise return -1
 * only iterate up to D_BLOCK (direct pointers)
 */
int traverse_path(const char *path) {
  // printf("traverse_path: %s\n", path);

  if (strcmp(path, "/") == 0) 
  {
    return 0; // Assuming inode 0 is the root inode
  }
  // Traverse the path from the root inode to the target inode
  // Return the target inode in the inode pointer
  char *path_copy = malloc(strlen(path) + 1);
  if (!path_copy) {
    return -ENOMEM; // Return -ENOMEM on memory allocation error
  }

  strcpy(path_copy, path);
  // printf("Copied path: %s\n", path_copy);
  struct wfs_inode *root_inode =
      (struct wfs_inode *)((char *)maps[0] + sb_array[0]->i_blocks_ptr);

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

    printf("Processing token: %s\n", token);
    // process token
    off_t *blocks = current_inode->blocks;

    // printf("Iterating over blocks of current inode...\n");
    // iterate through the blocks ptr array
    for (int i = 0; i < D_BLOCK; i++) {
      if (blocks[i] == 0) {
        continue;
      }
      printf("checking block %i of inode %i\n", i, current_inode->num);

      struct wfs_dentry *entries =
          (struct wfs_dentry *)((char *)maps[0] + sb_array[0]->d_blocks_ptr + blocks[i]);
      // printf("Checking block %d at address %p\n", i, (void *)entries);
      // printf("maps[0]: %p, blocks[%d]: %ld, calculated address: %p\n", (void *)maps[0], i, blocks[i], (void *)((char *)maps[0] + blocks[i]));

      // iterate through the directory entries

      for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
        if (strcmp(entries[j].name, "") != 0) {
          printf("nonempty dentry: %s with token: %s\n", entries[j].name, token);

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
      printf("Traversal: Path not found: %s\n", path);
      free(path_copy);
      return -1;
    }

    // printf("Updating current inode to inode num: %d\n", inode_num);
    // file/dir exists
    current_inode = (struct wfs_inode *)((char *)maps[0] + sb_array[0]->i_blocks_ptr +
                                         inode_num * sizeof(struct wfs_inode));
                                    
    //printf("Current inode updated to : %p\n", (void *)current_inode);

    token = strtok(NULL, "/");
  }

  printf("Traverse: Found inode: %d for path: %s\n", inode_num, path);
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
            (struct wfs_inode *)((char *)maps[0] + sb_array[0]->i_blocks_ptr +
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

        return inode_num;
      }
    }
  }
  return -1;
}

/*
 * Allocate a new free data block in the filesystem
 * Return the off_t of data block if successful, otherwise return -1
 *
 */
off_t allocate_data_block() {
  unsigned char *d_bitmap =
      (unsigned char *)((char *)maps[0] + sb_array[0]->d_bitmap_ptr);
  int num_bytes = sb_array[0]->num_data_blocks / 8;

  for (int i = 0; i < num_bytes; i++) {
    for (int bit_index = 0; bit_index < 8; bit_index++) {
      if (!(d_bitmap[i] & (1 << bit_index))) {
        // Mark the data block as used
        d_bitmap[i] |= (1 << bit_index);
        int block_num = i * 8 + bit_index;

        int target_disk = block_num % num_disks;

        // hard code to disk 0
        target_disk = 0;

        // int block_offset = block_num / num_disks;
        //printf("Allocating data block %d on disk %d\n", block_num, target_disk);

        // Initialize the data block
        char *block_ptr = (char *)maps[target_disk] +
                          sb_array[0]->d_blocks_ptr + block_num * BLOCK_SIZE;

        memset(block_ptr, 0, BLOCK_SIZE);
        off_t offset = (off_t)((char *)block_ptr - (char *)maps[target_disk]);

        return offset;
      }
    }
  }

  return -1; // No free data block available
}

/*
* Allocate a new directory entry in the inode
* gos through data block looking for slots, otherwise allocate new data block
* Returns 1 if successful, otherwise return -1
*/
int allocate_dentry(char *name, int parent_inode_num, int inode_num) {
  // Allocate a new directory entry in the inode
  // Return the index of the new directory entry if successful, otherwise return
  // -1

  if (strlen(name) + 1 > MAX_NAME) {
    printf("Error: Name '%s' exceeds maximum length (%d).\n", name, MAX_NAME);
    return -1; // Return error if name length exceeds MAX_NAME
  }
  
  struct wfs_inode *inode =
      (struct wfs_inode *)((char *)maps[0] + sb_array[0]->i_blocks_ptr +
                           parent_inode_num * sizeof(struct wfs_inode));
  for (int i = 0; i < D_BLOCK; i++) {
    if (inode->blocks[i] == 0 ) {
      // allocate new data block and place dentry
      off_t datablock = allocate_data_block();
      if (datablock < 0) {
        return -1; // No space for new data block
      }
      inode->blocks[i] = datablock;

      
      struct wfs_dentry *dentry =
          (struct wfs_dentry *)((char *)maps[0] + sb_array[0]->d_blocks_ptr +
                                datablock);
      
      dentry[0].num = inode_num;
      strcpy(dentry[0].name, name);

      // increment nlink of parent
      struct wfs_inode *inode =
      (struct wfs_inode *)((char *)maps[0] + sb_array[0]->i_blocks_ptr +
                           inode_num * sizeof(struct wfs_inode));
      inode->nlinks++;


      printf("Allocated dentry %s with new block %d for inode %d in parent inode %d\n",
             name, i, inode_num, parent_inode_num);
                 
     
      return 0;

    } else {
      // iterate over for free slot for dentry, else go to next block
      struct wfs_dentry *dentry =
          (struct wfs_dentry *)((char *)maps[0] + sb_array[0]->d_blocks_ptr +
                                inode->blocks[i]);
     
      for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
        if (dentry[j].num == 0 && dentry[j].name[0] == 0){
          dentry[j].num = inode_num;
          strcpy(dentry[j].name, name);
          // increment nlink of parent
      struct wfs_inode *inode =
      (struct wfs_inode *)((char *)maps[0] + sb_array[0]->i_blocks_ptr +
                           inode_num * sizeof(struct wfs_inode));
      inode->nlinks++;

          printf("Allocated dentry: %s for inode %d in block %d at index %d in inode %d\n",
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
    // printf("Disk %d: filesystem_id = %d\n", i, sb_array[i]->filesystem_id);
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
    // printf("Debug: Disk %d has disk_id %d (expected %d).\n", i,
    //        sb_array[i]->disk_id, i);
    if (sb_array[i]->disk_id != i) {
      printf("Error: Disk %d has an unexpected disk_id %d (expected %d).\n", i,
             sb_array[i]->disk_id, i);
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
    num_disks++;

    // printf("Set superblock for disk: %s\n", dir_name);
    // printf("Superblock details for disk %s:\n", dir_name);
    // printf("  Number of Inodes: %zu\n", sb_array[i]->num_inodes);
    // printf("  Number of Data Blocks: %zu\n", sb_array[i]->num_data_blocks);
    // printf("  Inode Bitmap Pointer: %ld\n", sb_array[i]->i_bitmap_ptr);
    // printf("  Data Bitmap Pointer: %ld\n", sb_array[i]->d_bitmap_ptr);
    // printf("  Inode Blocks Pointer: %ld\n", sb_array[i]->i_blocks_ptr);
    // printf("  Data Blocks Pointer: %ld\n", sb_array[i]->d_blocks_ptr);
    // printf("  RAID Mode: %d\n", sb_array[i]->raid_mode);

    
  }

  int expected_disks = sb_array[0]->num_disks;
  if (validate_disks(sb_array, disk_files, disk_count, expected_disks) != 0) {
    printf("Disk validation failed. Aborting mount.\n");
    return -1;
  }

  return 0;
}

// fuse functions

static int wfs_mknod(const char *path, mode_t mode,  dev_t dev) {
  (void)dev;
  mode_t d_mode = S_IFREG | mode; //change: change the mode
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

  if (last_slash == path_copy) 
  {
    //root case
    parent_path = "/";
    dir_name = ((char*)last_slash + 1);
  }
  else
  {
    *last_slash = '\0';
    parent_path = path_copy;
    dir_name = ((char*)last_slash + 1);
  }

  //printf("parent_path: %s\n", parent_path);
  //printf("dir_name: %s\n", dir_name);

  if (strlen(dir_name) == 0) {
    printf("Error in mkdir: Invalid directory name len is 0.\n");
    free(path_copy);
    return -ENOENT;
  }


  int parent_inode_num = traverse_path(parent_path);
  //printf("parent_inode_num: %d\n", parent_inode_num);
  if (parent_inode_num < 0) {
    printf("Error in mkdir: Parent directory does not exist.\n");
    free(path_copy);
    return parent_inode_num;
  }

  struct wfs_inode *parent_inode =
      (struct wfs_inode *)((char *)maps[0] + sb_array[0]->i_blocks_ptr +
                           parent_inode_num * sizeof(struct wfs_inode));

  // check if dir already exists
  int existing_inode_num = traverse_path(path);
  //printf("existing_inode_num: %d\n", existing_inode_num);
  if (existing_inode_num >= 0) {
    free(path_copy);
    printf("Directory already exists.\n");
    return -EEXIST; // Directory already exists
  }

  // allocate a new inode
  int new_inode_num = allocate_inode(d_mode);
  //printf("new_inode_num: %d\n", new_inode_num);
  if (new_inode_num < 0) {
    printf("Failed to allocate new inode.\n");
    free(path_copy);
    return -ENOSPC;
  }

  struct wfs_inode *new_inode =
      (struct wfs_inode *)((char *)maps[0] + sb_array[0]->i_blocks_ptr +
                           new_inode_num * sizeof(struct wfs_inode));
  new_inode->nlinks = 1; //change: set nlinks to 1


  if (allocate_dentry(dir_name, parent_inode_num, new_inode_num) < 0) 
  {
    printf("Error in alloc dentry: No space in parent directory for new entry.\n");
    free(path_copy);
    return -ENOSPC; // No space in parent directory for new entry
  }
  // Update parent directory's metadata
  // parent_inode->nlinks++;
  parent_inode->mtim = time(NULL);
  //change: dont inc size becase of file

  free(path_copy);
  return 0;
}


static int wfs_mkdir(const char *path, mode_t mode) {
  mode_t d_mode = S_IFDIR | mode;
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

  if (last_slash == path_copy) 
  {
    //root case
    parent_path = "/";
    dir_name = ((char*)last_slash + 1);
  }
  else
  {
    *last_slash = '\0';
    parent_path = path_copy;
    dir_name = ((char*)last_slash + 1);
  }

  //printf("parent_path: %s\n", parent_path);
  //printf("dir_name: %s\n", dir_name);

  if (strlen(dir_name) == 0) {
    printf("Error in mkdir: Invalid directory name len is 0.\n");
    free(path_copy);
    return -ENOENT;
  }


  int parent_inode_num = traverse_path(parent_path);
  //printf("parent_inode_num: %d\n", parent_inode_num);
  if (parent_inode_num < 0) {
    printf("Error in mkdir: Parent directory does not exist.\n");
    free(path_copy);
    return parent_inode_num;
  }

  struct wfs_inode *parent_inode =
      (struct wfs_inode *)((char *)maps[0] + sb_array[0]->i_blocks_ptr +
                           parent_inode_num * sizeof(struct wfs_inode));

  // check if dir already exists
  int existing_inode_num = traverse_path(path);
  //printf("existing_inode_num: %d\n", existing_inode_num);
  if (existing_inode_num >= 0) {
    free(path_copy);
    printf("Directory already exists.\n");
    return -EEXIST; // Directory already exists
  }

  // allocate a new inode
  int new_inode_num = allocate_inode(d_mode);
  //printf("new_inode_num: %d\n", new_inode_num);
  if (new_inode_num < 0) {
    printf("Failed to allocate new inode.\n");
    free(path_copy);
    return -ENOSPC;
  }

  struct wfs_inode *new_inode =
      (struct wfs_inode *)((char *)maps[0] + sb_array[0]->i_blocks_ptr +
                           new_inode_num * sizeof(struct wfs_inode));
  new_inode->nlinks = 1;


  if (allocate_dentry(dir_name, parent_inode_num, new_inode_num) < 0) 
  {
    printf("Error in alloc dentry: No space in parent directory for new entry.\n");
    free(path_copy);
    return -ENOSPC; // No space in parent directory for new entry
  }
  // Update parent directory's metadata
  // parent_inode->nlinks++;
  parent_inode->mtim = time(NULL);
  parent_inode->size += 1;

  free(path_copy);
  return 0;
}

static int wfs_getattr(const char *path, struct stat *stbuf) {
  // Implementation of getattr function to retrieve file attributes
  // Fill stbuf structure with the attributes of the file/directory indicated
  // by path parse pa
  printf("getattr: %s\n", path);
  int inode_num = traverse_path(path);
  if (inode_num < 0) {
    return -ENOENT; // Return -ENOENT if the file does not exist
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

  return 0; // Return 0 on success
}

// FUSE operations
static struct fuse_operations ops = {
    .getattr = wfs_getattr, .mkdir = wfs_mkdir, .mknod = wfs_mknod,
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
  printf("Starting Fuse... \n");
    
  // Pass FUSE options to fuse_main
  fuse_args[fuse_argc++] = mount_point; // Add mount point to FUSE args
  fuse_args[fuse_argc] = NULL;          // Null-terminate for FUSE
  init_disks(disks, num_disks);
  struct wfs_inode *root_inode = (struct wfs_inode *)((char *)maps[0] + sb_array[0]->i_blocks_ptr);
  struct wfs_dentry *dentry = (struct wfs_dentry *)((char *)maps[0] + sb_array[0]->d_blocks_ptr + root_inode->blocks[0]);
  printf("dentry name: %s\n", dentry[0].name);
  printf("dentry num: %d\n", dentry[0].num);
  printf("inode nlink: %d\n", root_inode->nlinks);

  return fuse_main(fuse_argc, fuse_args, &ops, NULL);
}
