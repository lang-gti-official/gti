# Binary-to-text encoding

`<std/encoding>` provides portable source-defined conversion between counted
bytes and textual Base64 or hexadecimal representations. The operations use
`std::string_view` as a byte view and return owning `std::string` values, so
embedded zeroes and every other byte value are preserved.

## API

```gti
#include <std/encoding>
#include <std/string>

std::string encoded = std::encoding::base64::encode("hello");
mut auto decoded = std::encoding::base64::decode(encoded.view());
if (!decoded) {
  return 1;
}

std::string digest_text = std::encoding::hex::encode(decoded.value().view());
```

The public operations are:

```gti
std::string std::encoding::base64::encode(std::string_view bytes);
expected<std::string, std::encoding::decode_errc>
std::encoding::base64::decode(std::string_view text);

std::string std::encoding::hex::encode(std::string_view bytes);
expected<std::string, std::encoding::decode_errc>
std::encoding::hex::decode(std::string_view text);
```

`<std/encoding>` includes its implementation dependency on `<std/string>`, but
optional standard units do not re-export declarations. A source file that
names `std::string` directly therefore also includes `<std/string>`.

## Base64 contract

Base64 uses the standard RFC 4648 alphabet
`A-Z a-z 0-9 + /`. Encoding always emits canonical `=` padding and produces an
empty string for empty input.

Decoding is deliberately strict:

- the input length is a multiple of four;
- padding appears only in the final four-character quantum and has the exact
  one- or two-character shape required by the decoded byte count;
- non-alphabet characters, including ASCII whitespace, are rejected; and
- unused low bits in the final quantum are zero, so alternate spellings of the
  same bytes are rejected as non-canonical.

This API does not accept unpadded Base64, MIME line wrapping, or the URL-safe
`-`/`_` alphabet. Those policies require distinct operations rather than flags
that weaken the canonical decoder.

## Hexadecimal contract

Hex encoding emits two lowercase digits per byte. Decoding requires an even
number of digits and accepts both uppercase and lowercase ASCII letters. It
does not accept `0x` prefixes, separators, or whitespace.

## Errors and security boundary

Both decoders return `expected<std::string,
std::encoding::decode_errc>`. The error values are:

- `invalid_length` for an impossible encoded length;
- `invalid_character` for a character outside the selected alphabet;
- `invalid_padding` for structurally misplaced Base64 padding; and
- `non_canonical_input` for nonzero unused Base64 bits.

Encoding is not encryption, authentication, validation of decoded content, or
a Unicode transformation. Applications handling secrets still need an
appropriate cryptographic API and should not assume these ordinary
source-defined loops provide constant-time behavior.
