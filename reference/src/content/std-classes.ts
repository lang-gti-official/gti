// Authored prose for standard-library class pages, keyed "unit/class".
//
// Only descriptions live here. Member signatures come from the generated
// extraction of the shipped stdlib source, and the class page THROWS during
// static generation if a member documented here does not exist in that
// extraction — a renamed or removed member fails the build instead of
// leaving a stale table. Members that exist but are not yet described are
// listed on the page as undocumented rather than hidden.

export interface MemberDoc {
  name: string;
  brief: string;
}

export interface MemberCategory {
  title: string;
  members: MemberDoc[];
}

export interface ClassDoc {
  brief: string;
  description?: string[];
  categories: MemberCategory[];
}

export const classDocs: Record<string, ClassDoc> = {
  "filesystem/path": {
    brief:
      "A pathname value: decomposition, joining, comparison, and lexical normalization, with no filesystem access.",
    description: [
      "path stores its pathname exactly as supplied; construction never collapses separators or resolves dot elements. lexically_normal() is the explicit normalizer, matching the C++ rule.",
      "The class is move-only, like the rest of the GTI standard library; a duplicate is an explicit clone(). Comparison is element-wise, which is why \"/a/b\" and \"/a//b\" compare equal while \"/a/b/\" differs from both — a trailing separator contributes an empty final element.",
    ],
    categories: [
      {
        title: "Construction",
        members: [
          { name: "path", brief: "Constructs an empty path, a path from text, or moves another path; copying is deleted." },
          { name: "clone", brief: "Returns an independently owned duplicate of this path." },
        ],
      },
      {
        title: "Assignment and concatenation",
        members: [
          { name: "assign", brief: "Replaces the pathname with the given text." },
          { name: "assign_path", brief: "Replaces the pathname with a copy of another path." },
          { name: "append", brief: "Joins as directories, inserting a separator when needed; an absolute argument replaces the path (C++ operator/=)." },
          { name: "append_path", brief: "append for a path argument." },
          { name: "concat", brief: "Joins text without introducing a separator (C++ operator+=)." },
        ],
      },
      {
        title: "Modifiers",
        members: [
          { name: "clear", brief: "Erases the pathname; empty() becomes true." },
          { name: "make_preferred", brief: "Converts every directory separator to the target's preferred separator." },
          { name: "remove_filename", brief: "Removes the filename component, keeping the trailing separator." },
          { name: "replace_filename", brief: "Replaces the filename component." },
          { name: "replace_extension", brief: "Replaces the extension; an empty replacement removes it, and a missing leading period is supplied." },
          { name: "swap", brief: "Exchanges the contents of two paths." },
        ],
      },
      {
        title: "Observers",
        members: [
          { name: "string", brief: "Returns the pathname as an owned std::string." },
          { name: "size", brief: "The pathname length in bytes." },
          { name: "empty", brief: "Whether the pathname is empty." },
        ],
      },
      {
        title: "Decomposition",
        members: [
          { name: "root_name", brief: "The root name, such as a Windows drive prefix; empty on POSIX." },
          { name: "root_directory", brief: "The root directory, if the path is rooted." },
          { name: "root_path", brief: "root_name() followed by root_directory()." },
          { name: "relative_path", brief: "Everything after the root path." },
          { name: "parent_path", brief: "The path with its filename removed; the root is its own parent." },
          { name: "filename", brief: "Everything after the last separator; empty when the path ends in one." },
          { name: "stem", brief: "The filename without its extension." },
          { name: "extension", brief: "The extension including its period; a leading period names the file and is not an extension." },
        ],
      },
      {
        title: "Queries",
        members: [
          { name: "has_root_name", brief: "Whether root_name() is non-empty." },
          { name: "has_root_directory", brief: "Whether root_directory() is non-empty." },
          { name: "has_root_path", brief: "Whether root_path() is non-empty." },
          { name: "has_relative_path", brief: "Whether relative_path() is non-empty." },
          { name: "has_filename", brief: "Whether filename() is non-empty." },
          { name: "has_stem", brief: "Whether stem() is non-empty." },
          { name: "has_extension", brief: "Whether extension() is non-empty." },
          { name: "has_parent_path", brief: "Whether parent_path() is non-empty." },
          { name: "is_absolute", brief: "Whether the path identifies a location without reference to a current directory." },
          { name: "is_relative", brief: "The negation of is_absolute()." },
        ],
      },
      {
        title: "Comparison",
        members: [
          { name: "compare", brief: "Element-wise three-way comparison: root name, rootedness, then each element." },
          { name: "operator==", brief: "Equality via compare(); also comparable against string text." },
          { name: "operator!=", brief: "Inequality via compare()." },
          { name: "operator<", brief: "Ordering via compare()." },
          { name: "operator<=", brief: "Ordering via compare()." },
          { name: "operator>", brief: "Ordering via compare()." },
          { name: "operator>=", brief: "Ordering via compare()." },
        ],
      },
      {
        title: "Generation and constants",
        members: [
          { name: "lexically_normal", brief: "The normal form: dot elements dropped, dot-dot resolved, separators collapsed; an empty result is spelled \".\"." },
          { name: "is_separator", brief: "Whether a character is a directory separator on this target." },
          { name: "preferred_separator", brief: "The target's preferred separator: '/' on POSIX, '\\\\' on Windows." },
        ],
      },
    ],
  },

  "string/string": {
    brief:
      "A move-only owned byte string with checked access and explicit growth.",
    description: [
      "Copying is explicit through clone(); passing a string transfers or borrows it. Indexed access is bounds-checked: out of range is a defined runtime failure, never undefined behaviour.",
    ],
    categories: [
      {
        title: "Construction",
        members: [
          { name: "string", brief: "Constructs empty, from a string_view, from a fixed character array, or by move; copying is deleted." },
          { name: "clone", brief: "Returns an independently owned duplicate." },
        ],
      },
      {
        title: "Capacity",
        members: [
          { name: "size", brief: "The number of bytes." },
          { name: "empty", brief: "Whether size() is zero." },
          { name: "capacity", brief: "Bytes reserved before reallocation is needed." },
          { name: "reserve", brief: "Ensures capacity for at least the requested size." },
          { name: "resize", brief: "Grows or shrinks the string to the requested size." },
          { name: "shrink_to_fit", brief: "Releases excess capacity." },
        ],
      },
      {
        title: "Element access",
        members: [
          { name: "at", brief: "Checked access by index." },
          { name: "operator[]", brief: "Checked access by index (GTI indexing is always checked)." },
          { name: "front", brief: "The first byte; failing on an empty string is defined." },
          { name: "back", brief: "The last byte; failing on an empty string is defined." },
        ],
      },
      {
        title: "Modifiers",
        members: [
          { name: "push_back", brief: "Appends one byte, growing capacity as needed." },
          { name: "pop_back", brief: "Removes the last byte." },
          { name: "append", brief: "Appends the bytes of a string_view." },
          { name: "clear", brief: "Removes every byte; capacity is retained." },
          { name: "swap", brief: "Declared exchange of contents (definition pending)." },
        ],
      },
      {
        title: "Iteration and comparison",
        members: [
          { name: "begin", brief: "Read-only iterator over the bytes." },
          { name: "end", brief: "The matching sentinel." },
          { name: "operator==", brief: "Byte equality against a string or string_view." },
          { name: "operator!=", brief: "Byte inequality." },
        ],
      },
    ],
  },

  "vector/vector": {
    brief:
      "A move-only dynamic array with checked indexing and explicit copying.",
    description: [
      "The element type must be movable; cloning the vector requires copyable elements. Indexed access is bounds-checked: out of range is a defined runtime failure.",
    ],
    categories: [
      {
        title: "Construction",
        members: [
          { name: "vector", brief: "Constructs empty, from a contextual fixed-array value (copying its elements; requires copyable T), or by move; copying the vector itself is deleted." },
        ],
      },
      {
        title: "Capacity",
        members: [
          { name: "size", brief: "The number of elements." },
          { name: "empty", brief: "Whether size() is zero." },
          { name: "capacity", brief: "Elements reserved before reallocation is needed." },
          { name: "reserve", brief: "Ensures capacity for at least the requested count." },
          { name: "resize", brief: "Grows (default-constructing) or shrinks to the requested count." },
          { name: "shrink_to_fit", brief: "Releases excess capacity." },
        ],
      },
      {
        title: "Element access",
        members: [
          { name: "at", brief: "Checked access by index." },
          { name: "operator[]", brief: "Checked access by index (GTI indexing is always checked)." },
          { name: "front", brief: "The first element; failing on an empty vector is defined." },
          { name: "back", brief: "The last element; failing on an empty vector is defined." },
        ],
      },
      {
        title: "Modifiers",
        members: [
          { name: "push_back", brief: "Appends an element by move." },
          { name: "pop_back", brief: "Removes the last element." },
          { name: "insert", brief: "Inserts at an index, shifting later elements." },
          { name: "erase", brief: "Removes at an index, shifting later elements down." },
          { name: "clear", brief: "Destroys every element; capacity is retained." },
          { name: "swap", brief: "Declared exchange of contents (definition pending)." },
        ],
      },
      {
        title: "Iteration",
        members: [
          { name: "begin", brief: "Read-only iterator over the elements." },
          { name: "end", brief: "The matching sentinel." },
        ],
      },
    ],
  },

  "random/random_engine": {
    brief:
      "A deterministic xoshiro256++ generator seeded from one uint64_t.",
    description: [
      "The same seed always produces the same sequence, which is what makes tests and replays exact. Seeding expands the single seed across the four state words with splitmix64.",
    ],
    categories: [
      {
        title: "Members",
        members: [
          { name: "random_engine", brief: "Constructs the engine from an explicit uint64_t seed." },
          { name: "operator()", brief: "Advances the state and returns the next uint64_t draw." },
        ],
      },
    ],
  },

  "random/random_device": {
    brief: "A fallible source of nondeterministic host entropy.",
    description: [
      "Draw from it to seed an engine, not in a loop. Entropy can fail — a sandbox without a source, an exhausted descriptor — so the call returns expected rather than a bare value.",
    ],
    categories: [
      {
        title: "Members",
        members: [
          { name: "random_device", brief: "Constructs the device; entropy is drawn lazily on first use." },
          { name: "operator()", brief: "Returns expected<uint32_t, int>: a draw from host entropy, or the failure status." },
        ],
      },
    ],
  },

  "tcp/socket": {
    brief:
      "A move-only POSIX IPv4 stream-socket owner with typed creation and close errors.",
    categories: [
      {
        title: "Members",
        members: [
          { name: "open", brief: "Creates a socket, returning expected<socket, errc>." },
          { name: "close", brief: "Closes explicitly, reporting a close error the destructor cannot." },
          { name: "is_open", brief: "Whether the descriptor is live." },
        ],
      },
    ],
  },
};
