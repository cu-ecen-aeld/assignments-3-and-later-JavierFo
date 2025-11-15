#!/bin/bash
set -e
set -u

NUMFILES=10
WRITESTR=AELD_IS_FUN
WRITEDIR=/tmp/aeld-data
username=$(cat conf/username.txt)

if [ $# -lt 3 ]
then
    echo "Using default value ${WRITESTR} for string to write"
    if [ $# -lt 1 ]
    then
        echo "Using default value ${NUMFILES} for number of files to write"
    else
        NUMFILES=$1
    fi
else
    NUMFILES=$1
    WRITESTR=$2
    WRITEDIR=/tmp/aeld-data/$3
fi

MATCHSTR="The number of files are ${NUMFILES} and the number of matching lines are ${NUMFILES}"

echo "Writing ${NUMFILES} files containing string ${WRITESTR} to ${WRITEDIR}"

# -------------------------------------------------------
# 1. Clean previous build artifacts
# -------------------------------------------------------
echo "Cleaning previous build artifacts..."
make clean

# -------------------------------------------------------
# 2. Build writer (native)
# -------------------------------------------------------
echo "Compiling writer application (native)..."
make

# -------------------------------------------------------
# 3. Create the directory BEFORE calling writer
# -------------------------------------------------------
echo "Creating output directory..."
rm -rf "$WRITEDIR"
mkdir -p "$WRITEDIR"
echo "$WRITEDIR created"

# -------------------------------------------------------
# 4. Use writer instead of writer.sh
# -------------------------------------------------------
for i in $(seq 1 $NUMFILES)
do
    ./writer "$WRITEDIR/${username}$i.txt" "$WRITESTR"
done

# -------------------------------------------------------
# 5. Run finder and validate
# -------------------------------------------------------
OUTPUTSTRING=$(./finder.sh "$WRITEDIR" "$WRITESTR")

rm -rf /tmp/aeld-data

set +e
echo ${OUTPUTSTRING} | grep "${MATCHSTR}" >/dev/null

if [ $? -eq 0 ]; then
    echo "success"
    exit 0
else
    echo "failed: expected ${MATCHSTR} but got:"
    echo "${OUTPUTSTRING}"
    exit 1
fi

