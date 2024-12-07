#!/bin/bash
cd mnt
for i in {1..10}; do
    touch "file_$i.txt"
done
cd ..
clear