- `std::copy` returns its advanced output cursor, allowing successive copies to
  continue writing while preserving checked destination ownership. Existing
  calls that discard the result remain valid.
- Fix an internal compiler error when discarding a returned borrowed carrier
  after moving a named carrier into a forwarding function.
