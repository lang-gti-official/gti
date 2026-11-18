# Copies the consumer source and the built bundles, without producer sources,
# into ../distributed, then removes the producer project.
mkdir ../distributed
cp main.gti ../distributed/
for name in base left right; do cp "$(find build -name "$name.gtilib")" ../distributed/; done
cp ../variants/distributed.toml ../distributed/gti.toml
cd .. && rm -rf project
