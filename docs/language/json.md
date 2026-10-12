# JSON


Status: Implemented source contract and native C++ execution.

`<std/json>` maps application types to JSON text with no codec, field list or
constructor: every rule is ordinary GTI over `<std/meta>` (static semantics
§3.12), and no JSON type or operation has compiler recognition. A document is
a struct; its fields are the members, and their types are the value kinds.
The library began as `std::json`, the acceptance client of
compile-time reflection, and replaced the earlier owned document model and
its explicit codecs once it covered their uses.

```gti
#include <std/json>

enum class severity { info, warning };

struct report {
  [[std::json::name("msg")]]
  std::string message = std::string("");
  severity level = severity::info;
  std::vector<uint32_t> related = std::vector<uint32_t>();
  std::optional<std::string> fix = std::optional<std::string>();
  std::map<std::string, double> timings = std::map<std::string, double>();
  [[std::json::ignore]]
  uint64_t cache_key = 0;
};

expected<std::string, std::json::error> text =
    std::json::encode(value);
expected<std::string, std::json::error> readable =
    std::json::encode_pretty(value, uint8_t(2));
expected<report, std::json::error> back =
    std::json::decode<report>(text.value().view());
```

`encode<T>(T&)` writes compact text and `encode_pretty<T>(T&, indentation)`
the same text with newlines and indentation; both fail only for invalid UTF-8
in a string or a non-finite floating value. `decode<T>(string_view)` and
`decode<T>(string_view, decode_options&)` scan the text into per-member byte
ranges, check the whole document's grammar and limits, and build `T` from the
inside out. A struct is built through `std::meta::construct`, so no object
exists before every field has a value and constructors are never bypassed.
Member order in the text is free; members that name no field are skipped; a
repeated key is `duplicate_key`.

Types, at the top level and as fields:

- `bool`; the fixed-width integers, which accept only integer lexemes within
  the type's range (`type_mismatch` for a fraction or exponent,
  `number_out_of_range` otherwise); `float` and `double`, written as the
  shortest round-trip decimal (`std::format("{}")`) and read from any JSON
  number lexeme with `std::from_chars`.
- `std::string`, with JSON escapes decoded (including `\uXXXX` and surrogate
  pairs) and the shortest normal escapes written; `std::string_view` encodes
  only.
- Integral enums, by enumerator name (`invalid_enumerator` otherwise).
- `std::optional<T>`: `null` or an absent member decodes as empty, and an
  empty optional encodes as `null`.
- `std::vector<E>`; `std::map<std::string, V>` and
  `std::unordered_map<std::string, V>` as objects with dynamic keys.
- Nested reflectable structs, with `[[std::json::name("key")]]`
  renaming a field, `[[std::json::ignore]]` leaving it out of the text (on
  decoding it takes its declared default, or default construction when it
  declares none), and `[[std::json::defaulted]]` letting an absent member
  take the field's declared default initializer instead of failing with
  `missing_field`. `decode_options::set_missing_members_defaulted(true)`
  applies that rule to every field that declares a default, which is what a
  configuration file usually wants:

  ```gti
  struct editor_ui {
    [[std::json::defaulted]] bool enable_docking = true;
    [[std::json::defaulted]] float font_size = 18.0;
    std::string ini_path = std::string("config://imgui.ini");  // required
  };
  ```
- Any type with a `std::json::codec<T>` specialization:

  ```gti
  class std::json::codec<temperature> {
  public:
    expected<void, std::json::error>
    encode(temperature& value, mut std::string& out);
    expected<temperature, std::json::error>
    decode(std::string_view text);
  };
  ```

  `encode` appends the value's text; `decode` receives exactly the value's
  text and may call the library on it (`decode<double>(text)`), reporting
  offsets relative to it, which the library rebases. The empty primary is
  told from a specialization through reflection (a specialization has
  methods), so no registration is needed.

Any other type is a compile-time error at its use.

The grammar is strict JSON: valid UTF-8 (overlong forms, encoded surrogates
and values above U+10FFFF rejected), no raw control characters in strings,
`\u` escapes with surrogate pairs (`invalid_unicode` for lone or reversed
surrogates), no leading zeros, no comments, no trailing commas, nothing after
the root value. `decode_options` bounds nesting depth (`set_max_depth`,
default 128) and document bytes (`set_max_document_bytes`, default 64 MiB).

Whole documents move to and from disk with `read_file<T>(path[, options])`,
`write_file(path, value)` and `write_file_pretty(path, value, indentation)`
over `<std/cstdio>`; a file that cannot be opened, read or written is
`io_failure`, with no source location.

Decoding reports `std::json::error`: `code` (an `errc`:
`unexpected_end`, `unexpected_character`, `invalid_number`,
`invalid_escape`, `invalid_unicode`, `duplicate_key`, `missing_field`,
`invalid_enumerator`, `type_mismatch`, `number_out_of_range`,
`nesting_limit`, `document_too_large`, `trailing_characters`, `io_failure`),
`description()`, the byte `offset` into the input, one-based `line` and
`column` (`has_source_location()` is false for encoding failures), and `path`,
a JSON Pointer to the value being decoded (`/points/1/x`, with `~` and `/`
escaped as `~0` and `~1`). Codecs may extend a path with `prepend_key` and
`prepend_index`.

Not covered: a dynamic document model for text whose shape is unknown in
advance (decode into `std::map<std::string, ...>` or a struct of optionals
instead), exact preservation of number lexemes through a round trip (numbers
are typed values), and `std::string_view` decoding. Examples 91, 92, 100 and
141 and the fixtures `tests/fixtures/stdlib_surface_json.gti`,
`tests/fixtures/json_parity.gti` and the coverage programs
`tests/coverage/c137_stdlib_json_documents.gti` and
`tests/coverage/c138_stdlib_json_codecs_nested.gti` cover the surface.
