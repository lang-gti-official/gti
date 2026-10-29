#!/usr/bin/env python3
"""Public standard APIs work while private implementation interfaces stay hidden."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    compiler = str(Path(sys.argv[1]).resolve())
    repository = Path(__file__).resolve().parent.parent
    environment = dict(os.environ)
    environment.setdefault("GTI_STDLIB_PATH", str(repository / "stdlib"))
    environment.setdefault("GTI_RUNTIME_INCLUDE", str(repository / "runtime/include"))
    with tempfile.TemporaryDirectory(prefix="gti-stdlib-modules-") as temporary:
        root = Path(temporary)
        public = root / "public.gti"
        public.write_text('''import std.set;
import std.unordered_set;
import std.charconv;
import std.string;
import std.resource_vector;
import std.resource;
import std.memory;
mut int drops = 0;
class Payload {
public:
  int value;
  Payload(int value) : value(value) {}
  ~Payload() { drops += 1; }
};
int main() {
  mut std::set<int> ordered{};
  ordered.insert(7);
  mut std::unordered_set<int> hashed{};
  hashed.insert(9);
  auto first = ordered.begin();
  auto second = hashed.begin();
  mut int value = 0;
  auto parsed = std::from_chars("26", value);
  if (parsed.ec != std::parse_errc::none) { return 1; }
  if (*first + *second + value != 42) { return 2; }
  {
    mut auto owner = std::make_shared<Payload>(42);
    auto observer = owner.downgrade();
    { auto locked = observer.lock(); if (!locked || locked->value != 42) { return 3; } }
    owner.reset();
    if (!observer.expired() || drops != 1) { return 4; }
  }
  std::raw_allocation vacant{};
  if (vacant.address != nullptr) { return 5; }
  std::process_resource process{};
  unsafe {
    auto resource = std::bind_resource(process);
    mut std::resource_vector<int, std::process_resource> items{resource};
    items.push_back(42);
    if (items[std::size_t(0)] != 42) { return 6; }
  }
  return 0;
}
''', encoding="utf-8")
        executable = root / ("public.exe" if os.name == "nt" else "public")
        built = subprocess.run([compiler, str(public), "--backend", "llvm", "-O2", "-o", str(executable)],
                               env=environment, text=True, capture_output=True)
        assert built.returncode == 0, built.stdout + built.stderr
        executed = subprocess.run([str(executable)], text=True, capture_output=True)
        assert executed.returncode == 0, executed.stdout + executed.stderr
        cases = [
            ("utility", "map_entry", "std::detail::map_entry<int, int> hidden{1, 2}; return 0;"),
            ("map", "map_sentinel", "std::detail::map_sentinel hidden{0}; return 0;"),
            ("charconv", "numeric_format", "return int(std::detail::numeric_format(std::chars_format::general));"),
            ("vector", "vector_sentinel", "std::detail::vector_sentinel hidden{0}; return 0;"),
            ("memory", "prefix_owner", "std::detail::prefix_owner<int> hidden{uint64_t(1)}; return 0;"),
            ("resource", "raw_allocation", "std::detail::raw_allocation hidden{}; return 0;"),
            ("resource", "resource_lease", "std::process_resource provider{}; unsafe { auto binding = std::bind_resource(provider); std::detail::resource_lease<std::process_resource> hidden{binding}; } return 0;"),
        ]
        for module, name, body in cases:
            source = root / f"private_{name}.gti"
            source.write_text(f"import std.{module};\nint main() {{ {body} }}\n", encoding="utf-8")
            result = subprocess.run([compiler, str(source), "--emit-mir", "-o", str(root / "rejected.mir")],
                                    env=environment, text=True, capture_output=True)
            diagnostic = result.stdout + result.stderr
            code = "GTI-S2000" if name == "map_entry" else "GTI-S2024"
            assert result.returncode != 0 and f"error[{code}]" in diagnostic, diagnostic
            assert f"std::detail::{name}'" in diagnostic, diagnostic
            assert "GTI-I0012" not in diagnostic, diagnostic
        hidden = root / "hidden_interface.gti"
        hidden.write_text("import std.detail.map_entry;\nint main() { return 0; }\n", encoding="utf-8")
        result = subprocess.run([compiler, str(hidden), "--emit-mir", "-o", str(root / "hidden.mir")],
                                env=environment, text=True, capture_output=True)
        assert result.returncode != 0 and "GTI-I0012" in result.stderr, result.stdout + result.stderr
        # Application source outside the standard-library roots cannot claim
        # the reserved name, as an interface or an implementation, even though
        # doing so would grant no compiler trust.
        reserved = "The module name 'std' is reserved for the standard library."
        for header in ("export module std;", "export module std.memory;",
                       "export module std.custom.tools;", "module std.memory;"):
            claimed = root / "claimed_std.gti"
            claimed.write_text(f"{header}\nexport int value() {{ return 0; }}\n", encoding="utf-8")
            importer = root / "claimed_std_importer.gti"
            importer.write_text('import "claimed_std.gti";\nint main() { return value(); }\n',
                                encoding="utf-8")
            result = subprocess.run([compiler, str(importer), "--emit-mir", "-o",
                                     str(root / "claimed.mir")],
                                    env=environment, text=True, capture_output=True)
            diagnostic = result.stdout + result.stderr
            assert result.returncode != 0 and "error[GTI-I0012]" in diagnostic, (header, diagnostic)
            assert reserved in diagnostic and "claimed_std.gti:1:" in diagnostic, (header, diagnostic)
        # A name that merely starts with the letters "std" is not reserved.
        lookalike = root / "lookalike.gti"
        lookalike.write_text("export module stdx;\nexport int value() { return 0; }\n",
                             encoding="utf-8")
        importer = root / "lookalike_importer.gti"
        importer.write_text('import "lookalike.gti";\nint main() { return value(); }\n',
                            encoding="utf-8")
        result = subprocess.run([compiler, str(importer), "--emit-mir", "-o", str(root / "lookalike.mir")],
                                env=environment, text=True, capture_output=True)
        assert result.returncode == 0, result.stdout + result.stderr
    print("standard module public/private boundary checks passed")


if __name__ == "__main__":
    main()
