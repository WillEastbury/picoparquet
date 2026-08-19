# AGENTS.md

## Project constraints

- Keep the implementation dependency-free and conform to C11.
- Treat caller-owned memory as the default: no unbounded allocations and bounded scratch usage.
- Preserve both mmap and incremental streaming input paths.
- Keep the codec boundary explicit so PicoZstd can be supplied without coupling the parser to a codec implementation.
- Unsupported Parquet features must return an explicit error; do not silently skip or reinterpret data.
- Maintain portability across Windows, Linux, AArch64, and PicoSuite. Portable scalar code is authoritative.
- Keep the public API small, documented, and suitable for read-only selective named-column access.

Issue #1 is the source of truth for the v0.1 scope. Update documentation when behavior or supported Parquet features change.