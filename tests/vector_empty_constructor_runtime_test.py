#!/usr/bin/env python3
"""Empty vectors acquire no region; later growth retains ownership and cleanup."""
from pathlib import Path

from lambda_pack_invocation_runtime_test import main as runtime_matrix


if __name__ == "__main__":
    fixtures = Path(__file__).parent / "fixtures"
    runtime_matrix(
        names=("vector_empty_constructor",),
        cases={"vector_empty_constructor": [((), (0, "8\n7\n99\nlive=0\n", ""))]},
        native_inputs_by_name={"vector_empty_constructor": [
            str(fixtures / "stdlib_adoption_allocation_counts.cpp")]},
        llvm_modes=True,
    )
