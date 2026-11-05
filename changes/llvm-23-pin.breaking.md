GTI now uses one LLVM everywhere: LLVM 23. Release, nightly and beta toolchains
bundle LLVM 23.1.4 (previously 20.1.0). Building GTI from source requires an
installed LLVM 23 (macOS: `brew install llvm@23`; Ubuntu: `llvm-23-dev` and
`libclang-23-dev` from apt.llvm.org), and CMake now refuses other LLVM major
versions instead of accepting LLVM 18 or newer. `-DGTI_ALLOW_UNPINNED_LLVM=ON`
keeps an experimental build on another major, with a warning.
