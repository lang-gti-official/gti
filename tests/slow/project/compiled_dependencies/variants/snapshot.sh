# Records every file's checksum under the current directory in $1.
find . -type f | LC_ALL=C sort | xargs cksum > "$1"
