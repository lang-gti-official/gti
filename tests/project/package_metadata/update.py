from pathlib import Path

for package, old, new in [("app", "1.2.3-beta.4+build.7", "2.0.0"),
                          ("dep", "4.5.6", "4.5.7")]:
    path = Path(package) / "gti.toml"
    path.write_text(path.read_text().replace('version = "' + old + '"',
                                            'version = "' + new + '"'))
