# CLI11 binding

This package exposes a bounded CLI11 2.6.2 surface to GTI through a C ABI
adapter. CLI11 stays header-only; the package manifest compiles only
`native/adapter.cpp` and scopes the vendored include directory to that native
group.

Prepare the pinned headers from the repository root:

```sh
python3 packages/prepare.py cli11 --offline  # omit --offline on first use
```

Add the package as a path dependency and use `import cli11.cli11;`.
An app owns every command and option handle returned from it. Arguments are
copied immediately, so owned GTI strings can be passed as temporary dynamic
views:

```gti
import cli11.cli11;
import std.string;
import std.vector;

int main(int argc, std::vector<std::string> argv) {
  unsafe {
    cli11::GtiCli11App* app = cli11::gti_cli11_app_create("example");
    if (app == nullptr) { return 1; }

    cli11::GtiCli11Command* root = cli11::gti_cli11_app_root(app);
    cli11::GtiCli11Option* verbose =
        cli11::gti_cli11_command_add_flag(root, "-v,--verbose", "Verbose output");
    for (mut std::size_t index = 0; index < argv.size(); index++) {
      if (cli11::gti_cli11_app_add_argument(app, argv[index].view()) == 0) {
        cli11::gti_cli11_app_destroy(app);
        return 2;
      }
    }

    int32_t status = cli11::gti_cli11_app_parse(app);
    if (cli11::gti_cli11_app_should_exit(app) != 0) {
      // Read help/error text with the size/byte accessors, then return status.
      cli11::gti_cli11_app_destroy(app);
      return status;
    }
    uint64_t verbosity = cli11::gti_cli11_option_count(verbose);
    cli11::gti_cli11_app_destroy(app);
    if (verbosity > 0) { std::println("verbose mode enabled"); }
    return 0;
  }
}
```

The initial facade supports nested subcommands, flags, positional or named raw
string options, typed `int64_t`/`uint64_t`/`double` options, required and
expected-count modifiers, extras, version/help flags, remaining arguments, and
captured output/errors. Result strings are exposed through checked size/byte
accessors because GTI does not yet provide a general owning conversion from a
borrowed native string.

No C++ exception crosses the C ABI. A parse request that should terminate—help,
version, or an error—sets `gti_cli11_app_should_exit`; use
`gti_cli11_app_exit_code` and the output/error accessors. Destroy the root app
exactly once. Every root/subcommand/option pointer is borrowed from that app and
is invalid afterward. Configuration files, validators, environment-backed
defaults, callbacks and formatter customization are not exposed yet.
