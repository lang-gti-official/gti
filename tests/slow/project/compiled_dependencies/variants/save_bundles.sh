# Copies the three library bundles and the application into directory $1.
mkdir -p "$1"
for name in base left right; do cp "$(find build -name "$name.gtilib")" "$1/"; done
cp "$(find build -type f -name app)" "$1/app"
