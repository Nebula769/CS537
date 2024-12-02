#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#define BLOCK_SIZE (512)
#define MAX_NAME (28)

#define D_BLOCK (6)              // direct data blocks inode can ref
#define IND_BLOCK (D_BLOCK + 1)  // index of indirect block in inode
#define N_BLOCKS (IND_BLOCK + 1) // total number of block pointers in inode

/*
  The fields in the superblock should reflect the structure of the filesystem.
  `mkfs` writes the superblock to offset 0 of the disk image.
  The disk image will have this format:

          d_bitmap_ptr       d_blocks_ptr
               v                  v
+----+---------+---------+--------+--------------------------+
| SB | IBITMAP | DBITMAP | INODES |       DATA BLOCKS        |
+----+---------+---------+--------+--------------------------+
0         ^              ^
i_bitmap_ptr        i_blocks_ptr

*/

// Superblock
struct wfs_sb {
  size_t num_inodes;
  size_t num_data_blocks;
  off_t i_bitmap_ptr;
  off_t d_bitmap_ptr;
  off_t i_blocks_ptr;
  off_t d_blocks_ptr;
  // Extend after this line
  int raid_mode;
  int disk_id;
  int filesystem_id;
  int num_disks;
};

// Inode
struct wfs_inode {
  int num;     /* Inode number */
  mode_t mode; /* File type and mode */
  uid_t uid;   /* User ID of owner */
  gid_t gid;   /* Group ID of owner */
  off_t size;  /* Total size, in bytes */
  int nlinks;  /* Number of links */

  time_t atim; /* Time of last access */
  time_t mtim; /* Time of last modification */
  time_t ctim; /* Time of last status change */

  off_t blocks[N_BLOCKS];
  char padding[392]; /* Padding to make the inode 512 bytes */
};

// Directory entry
struct wfs_dentry {
  char name[MAX_NAME];
  int num;
};
