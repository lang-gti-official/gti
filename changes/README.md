# Changelog fragments

A pull request that changes shipped toolchain behavior describes it here
instead of editing `VERSION` or `CHANGELOG.md`. Add one file per change:

```text
changes/<name>.<kind>.md
```

- `<name>`: lowercase letters, digits and hyphens, usually the task name.
- `<kind>`: `breaking`, `feature`, `fix` or `internal`.
- Contents: the user-facing text of one CHANGELOG bullet, written for someone
  using the toolchain. The first line starts the bullet; later lines continue it.

`internal` fragments record an invisible change and are not published. A change
with no fragment needs `Release-Impact: internal` and `Release-Reason:` trailers
on each shipped commit instead. `scripts/check_release_version.py` enforces this.

A release cut runs `python3 scripts/cut_release.py --title "<release title>"`.
It computes the next version (a breaking change or feature takes a minor
release before 1.0, otherwise a fix takes a patch), writes one CHANGELOG
section, updates `VERSION` and deletes the consumed fragments. Commit the result
with the `Release-Cut: <version>` trailer it prints; only such a commit may
change `VERSION`.
