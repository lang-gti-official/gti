A local copy of a by-value view or carrier formal that is reassigned inside an
`if`, `else`, nested `if` or loop-body `if` to a view relayed from a formal
(`value = entry.substr(1)`) no longer fails with an internal compiler error
("failed to construct valid MIR"); after the branch the local holds both owners.
