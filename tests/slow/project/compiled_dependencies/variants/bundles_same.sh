# Fails unless every library bundle (and, with a second argument, the
# application) equals its copy in $1.
for name in base left right; do cmp "$1/$name.gtilib" "$(find build -name "$name.gtilib")" || exit 1; done
if [ -n "$2" ]; then cmp "$1/app" "$(find build -type f -name app)" || exit 1; fi
