# libgit2 for GTI

A focused GTI binding for libgit2 1.9.6. It currently covers library lifecycle,
version and error access, repository creation/opening and common repository and
commit queries. The package vendors libgit2 and pinned zlib sources and builds a
static library with CMake. macOS and Linux are supported; Windows is not yet
supported.

```toml
[dependencies]
libgit2 = { git = "https://git.internal.howetotech.dev/gti-lang/libgit2.git", rev = "<full-commit>" }

[build]
allow-build-scripts = ["gti-libgit2@0.1.0"]
```

After `gti fetch`:

```gti
import libgit2.libgit2;

int main() {
  unsafe {
    if (git::gti_git_init() < 1) { return 1; }
    if (git::gti_git_shutdown() != 0) { return 2; }
  }
  return 0;
}
```

Repository and commit handles are owned and must be freed before shutdown;
returned path, message, author, and error strings are borrowed. Use
`gti build --jobs 2` to cap CMake parallelism. Approving the script permits host
code from the pinned dependency revision; GTI does not sandbox build scripts
yet.
