#!/usr/bin/env python3
"""Selected configuration flags across CLI plans, cache and example builds."""

import json
import pathlib
import shutil
import sys
import tempfile

from project_cli_smoke_test import (
    DEPENDENCY_INJECTING_NATIVE_ENVIRONMENT,
    GIT_REPOSITORY_ENVIRONMENT,
    executable_named,
    run,
)
import os


def main():
    gti = str(pathlib.Path(sys.argv[1]).resolve())
    example = pathlib.Path(sys.argv[2])
    for variable in (*DEPENDENCY_INJECTING_NATIVE_ENVIRONMENT,
                     *GIT_REPOSITORY_ENVIRONMENT):
        os.environ.pop(variable, None)
    with tempfile.TemporaryDirectory(prefix="gti-scoped-defines-") as temporary:
        project = pathlib.Path(temporary) / "project"
        shutil.copytree(example, project)
        manifest = project / "gti.toml"
        original = manifest.read_text(encoding="utf-8")
        metadata = json.loads(run([gti, "metadata"], cwd=project).stdout)
        expected = {
            "game": {"dev": ["PSYCH_ASSERTIONS", "PSYCH_GAME", "PSYCH_PROJECT"],
                     "release": ["NDEBUG", "PSYCH_GAME", "PSYCH_PROJECT"]},
            "unit": {"dev": ["PSYCH_ASSERTIONS", "PSYCH_PROJECT", "PSYCH_TESTS"],
                     "release": ["NDEBUG", "PSYCH_PROJECT", "PSYCH_TESTS"]},
        }
        for target in metadata["targets"]:
            for output in target["outputs"]:
                assert output["defines"] == expected[target["name"]][output["profile"]], output

        for options, observation in (([], "enabled"), (["--release"], "disabled"),
                                     (["--release", "-D", "PSYCH_ASSERTIONS"], "enabled")):
            checked = run([gti, "check", "game", *options], cwd=project)
            assert "GTI-S2075" not in checked.stderr, checked.stderr
            result = run([gti, "run", "game", *options], cwd=project)
            assert "GTI-S2075" not in result.stderr, result.stderr
            assert f"game: assertions {observation}" in result.stdout, result.stdout
        run([gti, "test", "unit"], cwd=project)
        run([gti, "build", "--all", "--jobs", "2"], cwd=project)
        for name, text in (("game", "game: assertions enabled"),
                           ("unit", "unit: test target selected")):
            binary = executable_named(project / "build/gti/dev", name)
            assert text in run([str(binary)]).stdout

        # Changing selected profile flags must invalidate an existing artifact.
        manifest.write_text(original.replace('defines = ["PSYCH_ASSERTIONS"]',
                                              'defines = []'), encoding="utf-8")
        result = run([gti, "run", "game"], cwd=project)
        assert "game: assertions disabled" in result.stdout, result.stdout

        # Target flags select source before both check and cached builds.
        manifest.write_text(original.replace('defines = ["PSYCH_GAME"]',
                                              'defines = []'), encoding="utf-8")
        for command in ("check", "build"):
            result = run([gti, command, "game"], expected=65, cwd=project)
            assert "game target configuration is missing" in result.stderr
        manifest.write_text(original, encoding="utf-8")
        result = run([gti, "run", "game"], cwd=project)
        assert "game: assertions enabled" in result.stdout

        # Selected flags seed source loading; source-order undef still wins.
        source = project / "src/main.gti"
        source.write_text("#undef PSYCH_ASSERTIONS\n" + source.read_text(encoding="utf-8"),
                          encoding="utf-8")
        result = run([gti, "run", "game"], cwd=project)
        assert "game: assertions disabled" in result.stdout

        # Direct mode reads no project manifest and still accepts explicit -D.
        direct = pathlib.Path(temporary) / "direct"
        run([gti, str(source), "-D", "PSYCH_PROJECT", "-D", "PSYCH_GAME",
             "-o", str(direct)])
        assert "game: assertions disabled" in run([str(direct)]).stdout


if __name__ == "__main__":
    main()
