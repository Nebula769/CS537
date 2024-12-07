#!/bin/bash

# Access the first argument
number=$1

# Check if a number was provided
if [[ -z "$number" ]]; then
    echo "Usage: $0 <number>"
    exit 1
fi

echo "test $number"
suffix=".desc"
filename="$number$suffix"
echo "desc: $filename"



