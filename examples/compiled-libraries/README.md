# Multiple compiled GTI libraries

Two independent libraries supply functions to one executable through explicit
virtual interfaces. The consumer requires only their `.gtilib` artifacts; their
GTI sources are unnecessary after publication.

From this directory, with an optimized GTI toolchain on `PATH`:

```sh
(cd left && gti build left --release)
(cd right && gti build right --release)
mkdir -p consumer/dependencies
find left/build -name left.gtilib -exec cp {} consumer/dependencies/left.gtilib \;
find right/build -name right.gtilib -exec cp {} consumer/dependencies/right.gtilib \;
(cd consumer && gti check app --release)
(cd consumer && gti run app --release)
```

Success exits with code zero. `left.gti` and `right.gti` inside the consumer are
virtual, validated interfaces and must remain absent on disk. The manifest uses
`[[targets.app.compiled-libraries]]` for two imports; the original singular
`[targets.app.compiled-library]` remains supported for one import. Do not combine
the two forms. Each import needs a distinct package/target owner and interface
path. A target may select up to 64 independent libraries, subject to the combined
artifact and callable limits. Transitive compiled dependencies are not supported
by this profile.

The `compiled_libraries_project` CTest copies this example, builds both bundles,
deletes both producer projects, and then checks, builds, runs and edits the
consumer while preserving its bundled dependencies.
