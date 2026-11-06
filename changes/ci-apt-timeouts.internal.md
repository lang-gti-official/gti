Bound and retry the Linux CI toolchain's `apt-get` steps, so a stalled package mirror fails an attempt after five minutes instead of holding a job until the six-hour limit.
