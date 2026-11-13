The language server no longer reports an internal MIR compiler error beside a
syntax error: while a file has a parse error, the editor shows that error and
skips borrow checking until the file parses again.
