"""Small source-context vocabulary; providers retain their own observations."""

from dataclasses import asdict, dataclass
import hashlib
from pathlib import PurePosixPath


SCHEMA = "gti.llvm.case-context/v1"


@dataclass(frozen=True)
class CaseContext:
    provider: str
    identity: str
    source: str
    source_sha256: str | None
    execution_profile: str
    loading: str
    observation: str
    unresolved: tuple[str, ...]
    source_kind: str = "repository"
    materialized_name: str | None = None

    def __post_init__(self) -> None:
        if any(not isinstance(value, str) or not value for value in (
                self.provider, self.identity, self.source, self.execution_profile,
                self.loading, self.observation)):
            raise ValueError("case context needs provider-qualified identity")
        path = PurePosixPath(self.source)
        if path.is_absolute() or not path.parts or ".." in path.parts:
            raise ValueError("case context source must be repository-relative")
        if self.source_sha256 is not None and (
                not isinstance(self.source_sha256, str) or len(self.source_sha256) != 64 or
                any(byte not in "0123456789abcdef" for byte in self.source_sha256)):
            raise ValueError("case context source digest must be SHA-256 or unresolved")
        if self.execution_profile not in {"single-threaded", "concurrent"}:
            raise ValueError("unknown case execution profile")
        if self.source_kind not in {"repository", "inline"}:
            raise ValueError("unknown case source kind")
        if self.materialized_name is not None and (
                not isinstance(self.materialized_name, str) or
                PurePosixPath(self.materialized_name).name != self.materialized_name or
                self.materialized_name in {"", ".", ".."}):
            raise ValueError("case materialization needs one safe file name")
        if not self.loading or not self.observation:
            raise ValueError("case context needs loading/observation provenance")
        if (not isinstance(self.unresolved, tuple) or
                any(not isinstance(value, str) or not value for value in self.unresolved) or
                len(set(self.unresolved)) != len(self.unresolved)):
            raise ValueError("case context has malformed unresolved obligations")

    def record(self) -> dict:
        return {"schema": SCHEMA, **asdict(self), "unresolved": list(self.unresolved)}

    @classmethod
    def from_record(cls, record: dict) -> "CaseContext":
        if not isinstance(record, dict) or record.get("schema") != SCHEMA:
            raise ValueError("unknown case-context schema")
        values = {key: value for key, value in record.items() if key != "schema"}
        if not isinstance(values.get("unresolved"), list):
            raise ValueError("case-context unresolved fields must be a list")
        values["unresolved"] = tuple(values["unresolved"])
        try:
            return cls(**values)
        except TypeError as error:
            raise ValueError(f"malformed case-context fields: {error}") from error


def source_digest(path) -> str | None:
    try:
        return hashlib.sha256(path.read_bytes()).hexdigest()
    except OSError:
        # Keep the original source in discovery: the owning harness must still
        # observe/report an unreadable source rather than silently omitting it.
        return None
