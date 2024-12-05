#!/bin/bash

# Directory paths
TESTS_DIR="tests"
TESTS_OUT_DIR="tests-out"

# Loop through all test files from 1 to 57
for i in $(seq 1 57); do
    # Construct file paths
    FILE1="$TESTS_DIR/$i.out"
    FILE2="$TESTS_OUT_DIR/$i.out"

    # Check if both files exist
    if [[ -f "$FILE1" && -f "$FILE2" ]]; then
        # Perform the diff
        diff "$FILE1" "$FILE2" > /dev/null
        if [[ $? -eq 0 ]]; then
            echo "Test $i: No differences"
        else
            echo "Test $i: Differences found"
            diff "$FILE1" "$FILE2"
        fi
    else
        echo "Test $i: One or both files are missing"
    fi
done
