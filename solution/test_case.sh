#!/bin/bash
cd mnt
cat ../three_blocks.txt > foo.c
rm -f foo.c
cat ../three_blocks.txt > foo.c
cd ..
