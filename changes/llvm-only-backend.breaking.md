Released toolchains no longer include the frozen C++ backend. LLVM is the
default on every target; `--backend cpp`, `--std`, `--keep-cpp`, `--emit-cpp`
and profiles that select C++ now report an error unless the toolchain was built
with `-DGTI_ENABLE_CPP_BACKEND=ON`.
