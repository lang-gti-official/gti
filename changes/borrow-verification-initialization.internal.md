Reject malformed MIR that forms a local checked reference to a scalar whose
initialization was removed, including when its operand-use indexes are rebuilt.
