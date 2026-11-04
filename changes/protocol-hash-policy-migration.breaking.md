Automatic unordered-container hashing now requires `std::hashing<K>`.
For an existing exact `std::hash<K>` specialization, write
`std::unordered_map<K, V, std::hash<K>>` or
`std::unordered_set<K, std::hash<K>>`; an implicit legacy-only default reports
`GTI-S2029` with this migration. Explicit policies retain their instance state,
including seeds and owned resources, through lookup, moves and destruction.
Pair keys with legacy-only elements require element hashing protocols or an
explicit composite hasher that retains its component policy instances.
