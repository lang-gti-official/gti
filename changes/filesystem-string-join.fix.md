Allow `std::filesystem::path / std::string_view`, including chained joins such as `root / "bin" / tool`, while preserving the existing lexical path-joining rules.
Also allow direct construction from `std::string`, copying its contents without consuming or borrowing the source.
