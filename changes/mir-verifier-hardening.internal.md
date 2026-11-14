MIR verification rejects malformed programs that no lowering produces but that
a backend would otherwise have admitted: out-of-range integer literals, class
traits without matching special members, deleted destructors on live classes,
mistyped place projections, untagged constructor field writes and evaluated
provenance-only carrier owner places.
