// Authored briefs for namespace-level standard-library functions, keyed by
// unit. Same contract as the class docs: signatures come from the generated
// extraction, and the unit page THROWS during static generation if a
// function documented here does not exist in the shipped source. Functions
// with no entry are listed with their signatures, never hidden.

export interface FunctionDoc {
  name: string;
  brief: string;
}

export const functionDocs: Record<string, FunctionDoc[]> = {
  algorithm: [
    { name: "min", brief: "The smaller of two values." },
    { name: "max", brief: "The larger of two values." },
    { name: "clamp", brief: "A value constrained to a closed range." },
    { name: "all_of", brief: "Whether a predicate holds for every element." },
    { name: "any_of", brief: "Whether a predicate holds for at least one element." },
    { name: "none_of", brief: "Whether a predicate holds for no element." },
    { name: "find_if", brief: "The first position where a predicate holds." },
    { name: "find_if_not", brief: "The first position where a predicate does not hold." },
    { name: "count_if", brief: "How many elements satisfy a predicate." },
  ],
  cmath: [
    { name: "abs", brief: "Absolute value of a binary32." },
    { name: "isfinite", brief: "Whether a binary32 is neither infinite nor NaN." },
    { name: "isinf", brief: "Whether a binary32 is an infinity." },
    { name: "isnan", brief: "Whether a binary32 is NaN." },
  ],
  cstdio: [
    { name: "getchar", brief: "Reads one byte from stdin; negative on end of input." },
    { name: "fopen", brief: "Opens a file for reading, returning a move-only FILE owner." },
    { name: "fgetc", brief: "Reads one byte from an open file." },
    { name: "fclose", brief: "Closes the file explicitly." },
  ],
  format: [
    { name: "format", brief: "Formats {} placeholders into an owned string; pattern errors return expected." },
    { name: "try_print", brief: "format followed by print, reporting pattern errors before any output." },
    { name: "try_println", brief: "try_print with a trailing newline." },
  ],
  numeric: [
    { name: "accumulate", brief: "Left-to-right reduction over a range (GTI does not inherit C++'s reorder permission)." },
    { name: "inner_product", brief: "Left-to-right pairwise product-and-accumulate over two ranges." },
    { name: "transform_reduce", brief: "Unary transform followed by left-to-right reduction." },
    { name: "wrapping_add", brief: "Addition with explicit two's-complement wraparound." },
    { name: "wrapping_sub", brief: "Subtraction with explicit wraparound." },
    { name: "wrapping_mul", brief: "Multiplication with explicit wraparound." },
    { name: "saturating_add", brief: "Addition clamped to the type's range." },
    { name: "saturating_sub", brief: "Subtraction clamped to the type's range." },
    { name: "saturating_mul", brief: "Multiplication clamped to the type's range." },
    { name: "checked_add", brief: "Addition returning expected; overflow is a value, not a failure." },
    { name: "checked_sub", brief: "Subtraction returning expected." },
    { name: "checked_mul", brief: "Multiplication returning expected." },
    { name: "gcd", brief: "Greatest common divisor." },
    { name: "lcm", brief: "Least common multiple." },
    { name: "midpoint", brief: "The midpoint of two values without intermediate overflow." },
  ],
  iterator: [
    { name: "advance", brief: "Moves an iterator forward by a count." },
    { name: "distance", brief: "The number of steps between two positions." },
    { name: "next", brief: "A copy of an iterator advanced by a count." },
  ],
  string: [
    { name: "to_string", brief: "Base-10 text of any fixed-width integer, as an owned string." },
    { name: "print", brief: "Writes a string to stdout." },
    { name: "println", brief: "print with a trailing newline." },
  ],
  utility: [
    { name: "make_pair", brief: "Deduces and constructs a pair." },
    { name: "swap", brief: "Exchanges two values." },
    { name: "exchange", brief: "Replaces a value, returning the old one." },
  ],
  memory: [
    { name: "make_shared", brief: "Allocates and constructs a shared_ptr-owned value." },
  ],
};
