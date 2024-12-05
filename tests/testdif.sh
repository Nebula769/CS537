#!/bin/bash

# Directory paths
TESTS_DIR="tests"
TESTS_OUT_DIR="tests-out"

# Loop through all test files from 1 to 57
for i in $(seq 1 57); do
    # Construct file paths
    FILE1="$TESTS_DIR/$i.out"
    FILE2="$TESTS_OUT_DIR/$i.out"
    DESC_FILE="$TESTS_DIR/$i.desc"

    echo "----------------------"
    echo "Test $i"

    # Check if the description file exists and display it
    if [[ -f "$DESC_FILE" ]]; then
        echo "Description:"
        cat "$DESC_FILE"
        echo ""
    else
        echo "Description not found for Test $i."
    fi

    # Check if both output files exist
    if [[ -f "$FILE1" && -f "$FILE2" ]]; then
        # Perform the diff
        diff "$FILE1" "$FILE2" > /dev/null
        if [[ $? -eq 0 ]]; then
            echo "Result: No differences"
        else
            echo "Result: Differences found"
            diff "$FILE1" "$FILE2"
        fi
    else
        echo "Result: One or both output files are missing"
    fi

    echo "----------------------"
    echo ""
done
