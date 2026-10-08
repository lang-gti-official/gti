# Importing native C enums

Run `gti build` and `gti run` in this directory. The manifest imports
`native/flags.h` automatically; no handwritten or committed generated bindings
are needed. The executable exits successfully only when its C and GTI checks
agree.

This demonstrates unsigned high-bit flags, full-width unsigned and signed enum
values, enum fields in a by-value C record, pointer output and a C-returned
function pointer. C independently supplies record size/alignment and validates
the transmitted values. The native source compiles as C, not generated C++.

Bindings use Clang's target-compatible integer type, not an unconditional
`int32_t`. They are transparent integer aliases rather than GTI nominal enums.
Regenerate bindings for each target, and keep the native compiler's enum ABI
options consistent with the import. The wide enum values in this example use
the C compiler's extended enum range, supported by the hosted Clang/GCC Unix
profiles. On the MSVC-compatible profile, Clang instead applies that target's
32-bit enum rules and evaluates these constants accordingly. Bindings preserve
those native values; they do not promise a portable 64-bit C enum. Use an
explicitly fixed native representation or a C adapter when that is required.
Enums with unsupported integer representations or an independent alignment
requirement are reported and skipped instead of emitting an incorrect alias.
