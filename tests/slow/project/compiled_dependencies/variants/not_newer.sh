# Fails when a library bundle was rewritten after marker file $1.
newer=$(find build -name '*.gtilib' -newer "$1")
if [ -n "$newer" ]; then echo "rewritten: $newer"; exit 1; fi
