Debug-info builds (`-g`) on the LLVM backend no longer ask LLVM 23 for the
terminator of an unfinished block. LLVM 23 changed that query to assume a
complete block, so a debug record placed at an unfinished block's end could
land before its last instruction, or read past an empty block.
