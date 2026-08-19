# PicoParquet

PicoParquet is a dependency-free C11 substrate for read-only, streaming Parquet access on constrained and general-purpose systems. It is designed for selective named-column reads from Cortex chat datasets and other workloads where bounded memory and portability matter.

## v0.1 scope

- mmap or streaming input with bounded caller-provided scratch memory
- Compact Thrift footer and metadata parsing, including schema, row groups, and pages
- Selective named-column reads
- PLAIN, dictionary, and RLE/bit-packed encodings
- The nested LIST/STRUCT subset required by Cortex chat datasets
- A codec callback boundary, initially targeting PicoZstd
- Explicit errors for unsupported features
- Windows, Linux, AArch64, and PicoSuite portability

PicoParquet has zero runtime dependencies. The v0.1 API is read-only; writing and general-purpose nested-type coverage are out of scope.

## Status

The project is at specification stage. See [Issue #1](../../issues/1) for the v0.1 requirements.