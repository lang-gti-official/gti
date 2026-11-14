A type written where a value is expected (for example `return std::string;`,
`auto p = Point;`, or `return T;` for a type parameter `T`) is now reported as
"names a type, not a value" with a hint to construct a value, instead of a
contradictory type mismatch or "Undefined name".
