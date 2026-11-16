The toolchain SDK now installs real LLVM headers when the system LLVM ships
its header directory as a relative symlink (as Ubuntu's packages do), instead
of a link that dangles once the SDK is staged or relocated.
