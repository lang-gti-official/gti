Fix LLVM compilation of forwarded plain base references and retained owned
receivers invoking readonly or mutable virtual call operators, including
`std::inline_polymorphic::invoke_once`. Preserve the selected base address and
the owner's cleanup on success and failure.
