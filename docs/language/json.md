# JSON


Status: Implemented source contract, with LLVM and C++ execution coverage.

`std.json` maps application types to JSON text with no codec, field list or
constructor: every rule is ordinary GTI over `std.meta` (static semantics
§3.12), and no JSON type or operation has compiler recognition. A document is
a struct when its schema is known; its fields are the members, and their types
are the value kinds. `std::json::value` and `std::json::object` additionally
represent documents with runtime keys and heterogeneous values.

The earlier reflection migration replaced the original owned document model.
Dynamic values now complement that typed interface: unknown schemas are a
separate use case, not something a struct or a homogeneous map can always
represent.

```gti
import std.json;

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
- `std::json::value` and `std::json::object`, including as fields inside
  reflected structs or elements of ordinary containers.
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
over `std.cstdio`; a file that cannot be opened, read or written is
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

Not covered: exact preservation of number lexemes through a round trip
(numbers are typed values), and `std::string_view` decoding. Examples 91, 92, 100 and
141 and the fixtures `tests/fixtures/stdlib_surface_json.gti`,
`tests/fixtures/json_parity.gti` and the coverage programs
`tests/coverage/c137_stdlib_json_documents.gti` and
`tests/coverage/c138_stdlib_json_codecs_nested.gti` cover the surface.


## Dynamic documents

Import `std.json`. `std::json::value` owns one of null, bool, signed 64-bit
integer, unsigned 64-bit integer, double, string, array or object. It is
move-only; `clone()` explicitly duplicates its text and descendants. It never
borrows the JSON input or shares mutable subtrees.

```gti
mut auto rows = std::json::value::array();
mut auto row = std::json::value::object();
row.set("country", std::json::value("Australia"));
row.set("population", std::json::value(uint64_t(27000000)));
row.set("active", std::json::value(true));
row.set("note", std::json::value());
rows.push_back(std::move(row));
auto text = std::json::encode(rows);
auto parsed = std::json::decode<std::json::value>("[{\"country\":\"Australia\"}]");
```

`value()` constructs null. Constructors accept bool, int32/int64, uint32/uint64,
float/double, owned string, string view, `std::vector<value>` and `json::object`.
A string-view constructor copies the bytes; owned strings and containers move.
`value::array()` and `value::object()` create empty containers. Alternatively,
instantiate `mut std::json::object row{};` and use `encode(row)` or
`decode<std::json::object>(text)`. The latter rejects a non-object root with
`type_mismatch`. `std::move(value).take_object()` transfers object storage out
of a value; the receiver is consumed.

`type()` returns `value_kind` (`null_value`, `boolean`, `signed_integer`,
`unsigned_integer`, `floating`, `string`, `array`, `object`). Predicates are
`is_null`, `is_bool`, `is_number`, `is_string`, `is_array` and `is_object`.
`as_bool`, `as_int64`, `as_uint64` and `as_double` require that exact alternative;
they do not perform numeric conversions. `as_string`, `as_array`, `as_object`
return shared references. Their `_mut` counterparts return exclusive references.

For arrays, `push_back(value)` transfers an element; `at(size_t)` and
`at_mut(size_t)` access an existing element. For objects, `set(key, value)`
inserts or replaces a member; the key may be an owned string or a borrowed
string view. `contains(key)`, `at(key)` and `at_mut(key)` accept either a
string reference or a view. Borrowed-key lookup does not allocate. A `json::object`
also exposes `size`, `empty`, `reserve`, `erase`, shared `begin`/`end`, and the
same view-key lookup and insertion operations. Iteration yields shared `member`
references with `first` (key) and `second` (value); keys cannot be mutated
through the object. Check the kind/containment before access when absence is
expected. Wrong-kind access or a missing object key panics; an invalid array
index raises the vector's checked bounds failure. A live child borrow prevents
structural mutation or replacement of its owning document.

Parsing uses the existing string, UTF-8 and number grammar and builds the tree
with one scanner. Duplicate keys, including differently escaped spellings of
the same key, are rejected. Child errors carry JSON Pointer paths plus source
locations. `decode_options` bounds document bytes and container nesting just
as for typed decoding. Dynamic encoding limits container nesting to 128 and
reports `nesting_limit` rather than recursing without a bound. Encoding still
rejects invalid UTF-8 and non-finite numbers. Compact and pretty encoding,
`read_file`, `write_file`, and dynamic fields within reflected structs all use
the existing entry points.

Negative integer tokens decode to int64; non-negative tokens use int64 when
they fit and uint64 otherwise. Tokens with a fraction or exponent decode to
double. Integers outside [-2^63, 2^64-1] and out-of-range doubles are errors;
large integers are never silently rounded through double. Numeric spelling,
negative integer zero and the original numeric alternative are not
preserved by a text round trip. JSON itself has no signedness tag.

Objects store sorted contiguous members. Lookup is O(log n) comparisons;
insertion and erasure move up to O(n) members. Arrays grow geometrically and
store values contiguously. Scalars need no payload allocation; strings own
text buffers; containers own their backing buffers. Cloning reserves each
container once. This representation avoids separate object-member node
allocations, but constructing very wide objects in reverse key order can take
quadratic movement. It is an explicit cost of compact sorted storage, not a
claim of constant-time insertion. Object serialization uses lexical key order;
input member order is not preserved.

See [example 186](../../examples/186-dynamic-json.gti) for CSV-style construction,
iteration, independent cloning and a round trip. `json_dynamic_runtime` covers
LLVM O0/O3, C++20/C++23, sanitizer cleanup, checked failures and borrow rejection;
`json_runtime` retains the typed-interface regressions.
