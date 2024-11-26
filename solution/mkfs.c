#include "wfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define bsize 512


int main(int argc, char *argv[])
{

    int r = -1;
    char *df[10];
    int dc = 0;
    int ic = 0;
    int bc = 0;

    // Parse command-line arguments
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-r") == 0)
        {
            if (i + 1 < argc)
            {
                if (strcmp(argv[i + 1], "0") == 0)
                {
                    r = 0;
                }
                else if (strcmp(argv[i + 1], "1") == 0)
                {
                    r = 1;
                }
                else if (strcmp(argv[i + 1], "1v") == 0)
                {
                    r = 2;
                }
                else
                {
                    printf("Invalid RAID mode.\n");
                    return 1;
                }
                i++;
            }
            else
            {
                printf("Missing RAID mode value.\n");
                return 1;
            }
        }
        else if (strcmp(argv[i], "-d") == 0)
        {
            if (i + 1 < argc)
            {
                df[dc++] = argv[i + 1];
                i++;
            }
            else
            {
                printf("Error: Missing disk file.\n");
                return 1;
            }
        }
        else if (strcmp(argv[i], "-i") == 0)
        {
            if (i + 1 < argc)
            {
                ic = atoi(argv[i + 1]);
                i++;
            }
            else
            {
                printf("Error: Missing inode count.\n");
                return 1;
            }
        }
        else if (strcmp(argv[i], "-b") == 0)
        {
            if (i + 1 < argc)
            {
                bc = atoi(argv[i + 1]);
                i++;
            }
            else
            {
                printf("Error: Missing block count.\n");
                return 1;
            }
        }
        else
        {
            printf("Error: Unknown argument %s.\n", argv[i]);
            return 1;
        }
    }

    // Log parsed arguments
    printf("RAID mode: %d\n", r);
    printf("Disk count: %d\n", dc);
    printf("Inode count: %d\n", ic);
    printf("Block count: %d\n", bc);

    for (int i = 0; i < dc; i++)
    {
        printf("Disk file: %s\n", df[i]);
    }

    //If the filesystem was created with n drives, it has to be always mounted with n drves
    if(r == -1)
    {
        printf("No RAID\n");
        return 1;
    }
    if(dc < 2)
    {
        printf("2 disks needed\n");
    }

    //number of blocks should always be rounded up to the nearest multiple of 32
    bc = ((bc + 31) / 32) * 32;
    printf("Updated Block Count: %d\n", bc);


    //mkfs should write the superblock and root inode to the disk image(if small return 1)

    //open disc file
    FILE *disks[10];
    for (int i = 0; i < dc; i++)
    {
        disks[i] = fopen(df[i], "wb+");
        if (!disks[i])
        {
            printf("Error opening disk file\n");
            return 1;
        }
    }

    //bitmaps for inode and data
    //int inode

}
