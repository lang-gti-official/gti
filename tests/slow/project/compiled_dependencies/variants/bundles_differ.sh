# Fails unless every library bundle differs from its copy in $1.
for name in base left right; do
  if cmp -s "$1/$name.gtilib" "$(find build -name "$name.gtilib")"; then echo "$name unchanged"; exit 1; fi
done
