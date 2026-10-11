#!/usr/bin/env python3
"""Scoped parallel borrowing through production GTI lowering."""

from lambda_pack_invocation_runtime_test import main
from pathlib import Path


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/scoped_parallel_failure.gti"
    def site(marker):
        line = next(index for index, text in enumerate(fixture.read_text().splitlines(), 1)
                    if f"// {marker}" in text)
        return f"{fixture.name}:{line}"
    worker = site("worker-failure")
    cleanup = site("cleanup-failure")
    main(("scoped_parallel_threads", "scoped_parallel_cleanup", "scoped_parallel_failure",
          "scoped_parallel_spawn_failure", "scoped_parallel_allocations"),
         cases={"scoped_parallel_failure": [
             ((), (70, "11\n11\n",
                 f'{worker}: runtime error[GTI-R0017]: panic: "scoped worker failed"\n')),
             (("cleanup",), (70, "11\n11\n",
                 f'{cleanup}: runtime error[GTI-R0017]: panic: "scoped cleanup failed"\n')),
             (("both", "fail"), (70, "",
                 f'{cleanup}: runtime error[GTI-R0014]: failure_during_cleanup; '
                 f'primary [GTI-R0017] at {worker} message="scoped worker failed"; '
                 'secondary [GTI-R0017] message="scoped cleanup failed"\n')),
             (("nested", "worker", "failure"), (70, "11\n11\n",
                 f'{worker}: runtime error[GTI-R0017]: panic: "scoped worker failed"\n'))],
                "scoped_parallel_spawn_failure": [
                    ((), (0, "", "")), (("partial",), (0, "", ""))],
                "scoped_parallel_allocations": [((), (0, "4\n2\n0\n", ""))]},
         native_inputs_by_name={"scoped_parallel_spawn_failure": [
             str(fixture.with_name("scoped_parallel_fault.cpp")),
             f"-I{fixture.parents[2] / 'runtime/src'}"],
             "scoped_parallel_allocations": [
                 str(fixture.with_name("scoped_parallel_allocations.cpp"))]},
         execution_profile="concurrent")
