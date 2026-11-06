Compiled libraries can retain private zero-initialized namespace globals and
passive namespace/class static `constexpr` data without producer sources at
consumer build time. Dependency diamonds share one storage instance per library.
Executable initializers, thread-local storage and global cleanup remain unsupported.
