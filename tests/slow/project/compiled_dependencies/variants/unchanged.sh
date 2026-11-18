# Fails when a file under the current directory differs from snapshot $1.
find . -type f | LC_ALL=C sort | xargs cksum | diff "$1" -
