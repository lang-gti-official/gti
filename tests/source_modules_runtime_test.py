#!/usr/bin/env python3
"""Source module interfaces execute through MIR and invalidate project caches."""

import json
import pathlib
import subprocess
import sys
import tempfile

from project_cli_smoke_test import executable_named, run


def main():
    compiler = str(pathlib.Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="gti-modules-runtime-") as temporary:
        root = pathlib.Path(temporary)
        library, app = root / "library", root / "app"
        library.mkdir()
        app.mkdir()
        api = library / "api.gti"
        api.write_text('''export module core;
namespace sample {
  int seed() { return 40; }
  int initialized = seed();
  export int answer() { return initialized + 2; }
  export struct Item {
    int value;
    Item(int initial) : value(initial) {}
    int read() { return value; }
  };
  export concept readable<T> = requires(T& item) { { item.read() } -> int; };
  export int read<T>(T& item) requires readable<T> { return item.read() + seed(); }
}
''')
        facade = library / "facade.gti"
        facade.write_text('''export module main;
export import "api.gti";
namespace sample {
  int from_dependency = answer();
  export int startup() { return from_dependency; }
}
''')
        direct = library / "consumer.gti"
        body = '''int main() {
  sample::Item item{2};
  return sample::answer() == 42 && sample::read(item) == 42 &&
         sample::startup() == 42 ? 0 : 1;
}
'''
        direct.write_text('import "facade.gti";\n' + body)
        for standard in ("c++20", "c++23"):
            for optimization in ("-O0", "-O1", "-O3"):
                binary = root / f"direct-{standard}-{optimization}"
                run([compiler, str(direct), "--std", standard, optimization,
                     "--keep-cpp", "-o", str(binary)])
                run([str(binary)])
                assert "// GTI verified-MIR body:" in binary.with_suffix(".gti.cpp").read_text()

        (library / "gti.toml").write_text('manifest-version = 1\n[package]\nname = "library"\n'
            'version = "0.1.0"\nsource-root = "."\n[modules]\nmain = "facade.gti"\ncore = "api.gti"\n')
        (app / "main.gti").write_text("import lib;\n" + body)
        (app / "gti.toml").write_text('manifest-version = 1\n[package]\nname = "app"\n'
            'version = "0.1.0"\nsource-root = "."\n[dependencies]\nlib = { path = "../library" }\n'
            '[targets.app]\nkind = "executable"\nroot = "main.gti"\n')
        report = root / "report.json"
        run([compiler, "build", "--build-report", str(report)], cwd=app)
        binary = executable_named(app / "build/gti/dev", "app")
        run([str(binary)])
        recorded = json.loads(report.read_text())
        # Provenance retains both edge kinds; capture deliberately bypasses caches.
        encoded = json.dumps(recorded)
        assert '"export-import"' in encoded and '"import"' in encoded, recorded
        run([compiler, "build"], cwd=app)
        cached = run([compiler, "build", "--verbose"], cwd=app)
        assert "gti: cache hit " in cached.stderr, cached.stderr
        before = api.read_text()
        api.write_text(before.replace("export int answer", "int answer"))
        invalid = subprocess.run([compiler, "build"], cwd=app, text=True, capture_output=True)
        assert invalid.returncode != 0 and "not exported" in invalid.stderr, invalid.stderr
        api.write_text(before)
        run([compiler, "build"], cwd=app)
        run([str(binary)])
        # A module-map edit must select the new interface, not stale cached code.
        replacement = library / "replacement.gti"
        replacement.write_text('export module main; export int replacement() { return 0; }\n')
        manifest = library / "gti.toml"
        manifest.write_text(manifest.read_text().replace('main = "facade.gti"', 'main = "replacement.gti"'))
        (app / "main.gti").write_text("import lib; int main() { return replacement(); }\n")
        rebuilt = run([compiler, "build", "--verbose"], cwd=app)
        assert "gti: cache hit " not in rebuilt.stderr, rebuilt.stderr
        run([str(binary)])


if __name__ == "__main__":
    main()
