- Support shared borrowed elements in source-defined owned storage,
  `std::vector`, and local fixed-array literals, including dynamic text views
  and records containing shared references to different owners. Checked
  construction, movement, and direct indexed copies preserve those owners' lifetimes. Adapter-derived copies
  conservatively retain the container borrow; dynamic text-field construction
  in classes with user-defined destructors remains unsupported through generic
  owner-aware construction. Ordinary literal-backed construction remains valid.
- Keep consuming vector iteration restricted to borrow-free relocatable elements;
  shared borrowed elements use owner-aware storage and ordinary checked reads.
