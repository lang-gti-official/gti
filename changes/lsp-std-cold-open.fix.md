The language server no longer holds back a file's first diagnostics while it
analyses the whole standard library. When a file's imports have no cached
standard-library analysis, for example after adding `import std;`, the file is
checked the way `gti check` checks it and the complete analysis is built in
the background for later edits.
