#!/bin/sh

# Check for correct number of arguments
if [ $# -ne 2 ]; then
    echo "Error: Two arguments required."
    echo "Usage: $0 <filesdir> <searchstr>"
    exit 1
fi

# Assign arguments to variables
filesdir=$1
searchstr=$2

# Check that filesdir is a valid directory
if [ ! -d "$filesdir" ]; then
    echo "Error: Directory '$filesdir' does not exist."
    exit 1
fi

# Count total number of files under filesdir (recursively)
file_count=$(find "$filesdir" -type f | wc -l)

# Count total number of matching lines containing searchstr
match_count=$(grep -r "$searchstr" "$filesdir" 2>/dev/null | wc -l)

# Print the result
echo "The number of files are $file_count and the number of matching lines are $match_count"

exit 0
