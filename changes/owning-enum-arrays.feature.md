Allow reference-free owning payload enums directly in fixed arrays, including
nested arrays, ordinary class/struct fields, and thread-local storage. Arrays
retain structural move/copy rules, reverse active-payload cleanup, and cleanup
of completed elements when initialization fails. Includes a runnable
`owning-enum-arrays` example and native O0/O3 lifetime regressions.
