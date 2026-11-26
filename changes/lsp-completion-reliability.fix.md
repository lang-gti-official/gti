Fix completion in project roots repeatedly rechecking dependencies because its
entry-point, test-program and imported-module settings differed from background
analysis. Completion now reuses compatible cached dependency semantics while
still checking the current buffer and invalidating edited imports.
