# JSON

Status: Implemented source contract and native C++ execution.

`<std/json>` provides a strict, owned JSON document model, recoverable parsing
and conversion, deterministic serialization, and explicit codecs for mapping
application types. It is ordinary GTI source-defined policy: no public JSON
type or operation has compiler recognition.

The complete surface and codec fixtures pass semantic analysis, verified MIR,
and native execution. Owning `expected<T, E>` error alternatives retain their
cleanup and move-only behavior through both the C++20 and C++23 representations;
the owned error contract does not require copying or borrowing error strings.

## Model

`std::json::value` is a move-only owner with one `std::json::kind`:

- `null_value`
- `boolean`
- `number`
- `string`
- `array`
- `object`

Parsed values own all descendant strings, keys, number lexemes, arrays, and
objects. Nothing in the document borrows the source text. `clone()` performs
an explicit deep copy.

Objects preserve source or insertion order. `set` replaces an existing key in
place and does not reorder it. The parser rejects duplicate keys, so a parsed
object never loses an earlier member silently. Missing keys and JSON `null`
are distinct: `contains` observes membership while `is_null` observes a stored
value.

`operator[]` is deliberately absent. It cannot insert a missing object member
or convert a value implicitly.

## Parsing

```gti
#include <std/json>
#include <std/string>

mut auto parsed = std::json::parse(response_body.view());
if (!parsed) {
  std::println(parsed.error().description());
  return 1;
}

mut std::json::value document = std::move(parsed).value();
auto login = document.get_string("login");
auto id = document.get_uint64("id");
```

Parsing accepts the JSON grammar only. In particular it:

- requires valid UTF-8 in unescaped string contents;
- decodes JSON escapes and UTF-16 surrogate pairs into UTF-8;
- rejects unescaped control characters, invalid escapes, malformed UTF-8,
  lone surrogates, and out-of-range Unicode scalar values;
- rejects comments, trailing commas, leading-zero numbers, duplicate object
  keys, and characters after the root value; and
- preserves the exact source lexeme for every number.

`parse_options` bounds document bytes, decoded string bytes, node count, and
nesting depth. Defaults are 64 MiB per document, 16 MiB per decoded string,
one million values, and 128 nested values. A limit failure is recoverable and
never returns a partial document.

## Errors

`std::json::error` owns its JSON Pointer path and may outlive both the input
text and document. Syntax errors additionally expose a zero-based byte offset
and one-based line and byte column. `has_source_location()` distinguishes
syntax locations from conversion and member-access errors.

`description()` returns stable explanatory text. Programs should branch on
`code()` rather than parse that text.

The principal error families are malformed input, configured resource limits,
missing keys, kind mismatches, and numeric conversion failures. A successful
parse followed by a failed typed getter leaves the document unchanged.

## Access

The recoverable scalar operations are:

```gti
value.as_boolean();
value.as_string();
value.as_int64();
value.as_uint64();
value.as_double();

object.get("owner");
object.get_string("title");
object.get_boolean("private");
object.get_int64("count");
object.get_uint64("id");
object.get_double("ratio");
```

`get` returns `expected<value&, error>`. Its success state is a checked,
read-only borrow of the stored value rather than a deep clone; its error state
reports a missing key or kind mismatch without retaining a loan. The result
cannot outlive the object and keeps that object read-only until the carrier and
any reference extracted by `value()` reach their last use. After a program has
validated a shape, `at(key)` and `at(index)` return borrowed values directly
without cloning. Their missing-key, wrong-kind, and out-of-range cases use
GTI's defined checked runtime failure; they never insert.

Arrays expose `elements()` for ordinary read-only or mutable range iteration.
Objects expose insertion-ordered `keys()` and `values()` checked spans, and
`members()` supplies a read-only paired span whose reference proxies expose
`first()` (the key) and `second()` (the value), without cloning either.
`values_mut() mut` returns an exclusive mutable span of the existing values;
it permits replacing descendants, not resizing the object or rewriting keys.
These views allocate nothing and retain the whole document, including its
physical storage, through every derived cursor and element borrow. Conflicting
`set`, replacement, movement or destruction is rejected while they are live.
An element borrow must end before its cursor advances. Wrong-kind access and
empty/end element access use the existing defined bounds failure.

`object_size()`, `key_at(index)` and `value_at(index)` remain available.
The tagged document itself is not a homogeneous range: scalar/null alternatives
do not invent an empty collection. Explicit array/object projections select
the intended collection contract. Example 91 demonstrates both member reading
and exclusive value iteration.

## Numbers

JSON numbers are stored as exact decimal lexemes rather than binary floating
point. `as_int64` and `as_uint64` accept only integer-form lexemes and perform
checked range conversion, including the full signed and unsigned 64-bit
ranges. `as_double` performs a checked decimal conversion and reports
`number_out_of_range` instead of producing infinity or NaN.

Builders provide exact signed and unsigned integer factories plus
`number_text`, which validates a caller-supplied JSON number lexeme. Floating
construction can pass the successful result of `std::format("{}", value)` to
`number_text`; its default binary32/binary64 presentation is shortest-round-trip
decimal. Non-finite results remain invalid JSON numbers and are rejected by
`number_text`. The library does not silently narrow `double` or route JSON
through the fixed-display `std::to_string(float)` API.

## Building And Serializing

```gti
mut std::json::value body = std::json::value::object();

auto named = body.set("name", "demo");
if (!named) {
  return 1;
}

auto private_value = body.set("private", true);
if (!private_value) {
  return 2;
}

mut std::json::value labels = std::json::value::array();
auto first = labels.push("bug");
auto second = labels.push("compiler");
if (!first || !second) {
  return 3;
}

auto stored = body.set("labels", std::move(labels));
if (!stored) {
  return 4;
}

mut std::string compact = std::json::stringify(body);
mut std::string readable = std::json::stringify_pretty(body, uint8_t(2));
```

Builders validate UTF-8 for keys and strings. `set` and `push` consume an
owned `value`; overloads construct strings, booleans, and 64-bit integers.

Serialization is deterministic and locale-independent. Compact output uses no
unnecessary whitespace. Pretty output changes whitespace only. Object order
and exact number lexemes are preserved, while strings use the shortest normal
JSON escape for quotes, reverse solidus, control characters, and common
control escapes. Non-ASCII UTF-8 is emitted directly.

## Explicit Codecs

The library performs no reflection or implicit field mapping. An application
may define one exact `std::json::codec<Model>` specialization for a nominal
type it owns:

```gti
class std::json::codec<GiteaUser> {
public:
  expected<GiteaUser, std::json::error>
  decode(std::json::value& input) {
    mut auto id = input.get_uint64("id");
    if (!id) {
      return unexpected(std::move(id).error());
    }
    mut auto login = input.get_string("login");
    if (!login) {
      return unexpected(std::move(login).error());
    }
    mut std::string owned_login = std::move(login).value();
    return GiteaUser(id.value(), std::move(owned_login));
  }

  expected<std::json::value, std::json::error>
  encode(GiteaUser& input) {
    mut std::json::value output = std::json::value::object();
    mut auto id = output.set("id", input.id());
    if (!id) {
      return unexpected(std::move(id).error());
    }
    mut auto login = output.set("login", input.login().view());
    if (!login) {
      return unexpected(std::move(login).error());
    }
    return std::move(output);
  }
};
```

`decode<T>(text)`, `encode<T>(value)`, and `decode_array<T>(text)` invoke that
exact specialization. `decode_array` prefixes a failed element's JSON Pointer
with its array index; codecs may use `error::prepend_path` when propagating
failures through other nested structures. The empty primary codec supplies no
fallback behavior. This keeps key choice, required versus optional fields,
defaulting, and schema evolution in application code. A future reflection or
derive facility may generate the same specialization without changing this
API.

## Experimental Reflected JSON

Status: preview covering `std::json`'s codec uses; the acceptance client of
compile-time reflection.

`<std/experimental/json>` maps application types to JSON text with no codec,
field list or constructor: every rule is ordinary GTI over `<std/meta>`
(static semantics §3.12). It began as the proof that reflection carries a
complete library and now covers what applications used `std::json::codec`
specializations for. `std::json` remains the dynamic document model (a
`value` tree for text whose shape is not known in advance); its explicit
codecs are the part this library replaces.

```gti
#include <std/experimental/json>

enum class severity { info, warning };

struct report {
  [[std::experimental::json::name("msg")]]
  std::string message = std::string("");
  severity level = severity::info;
  std::vector<uint32_t> related = std::vector<uint32_t>();
  std::optional<std::string> fix = std::optional<std::string>();
  std::map<std::string, double> timings = std::map<std::string, double>();
  [[std::experimental::json::ignore]]
  uint64_t cache_key = 0;
};

expected<std::string, std::experimental::json::error> text =
    std::experimental::json::encode(value);
expected<std::string, std::experimental::json::error> readable =
    std::experimental::json::encode_pretty(value, uint8_t(2));
expected<report, std::experimental::json::error> back =
    std::experimental::json::decode<report>(text.value().view());
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
- Nested reflectable structs, with `[[std::experimental::json::name("key")]]`
  renaming a field and `[[std::experimental::json::ignore]]` leaving it out of
  the text and default-constructing it on decoding.
- Any type with a `std::experimental::json::codec<T>` specialization:

  ```gti
  class std::experimental::json::codec<temperature> {
  public:
    expected<void, std::experimental::json::error>
    encode(temperature& value, mut std::string& out);
    expected<temperature, std::experimental::json::error>
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

Decoding reports `std::experimental::json::error`: `code` (an `errc`:
`unexpected_end`, `unexpected_character`, `invalid_number`,
`invalid_escape`, `invalid_unicode`, `duplicate_key`, `missing_field`,
`invalid_enumerator`, `type_mismatch`, `number_out_of_range`,
`nesting_limit`, `document_too_large`, `trailing_characters`),
`description()`, the byte `offset` into the input, one-based `line` and
`column` (`has_source_location()` is false for encoding failures), and `path`,
a JSON Pointer to the value being decoded (`/points/1/x`, with `~` and `/`
escaped as `~0` and `~1`). Codecs may extend a path with `prepend_key` and
`prepend_index`.

Not covered: a dynamic document model (use `std::json::value`), exact
preservation of number lexemes through a round trip (numbers are typed
values here), and `std::string_view` decoding. Example 141 and the fixtures
`tests/fixtures/experimental_json.gti` and
`tests/fixtures/experimental_json_parity.gti` cover the surface.
