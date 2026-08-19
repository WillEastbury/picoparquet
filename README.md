# PicoParquet

PicoParquet is a dependency-free, read-only C11 Parquet substrate for
constrained systems and selective reads from Cortex chat datasets. It does
not allocate memory: metadata arrays, footer storage, page scratch, and
dictionary storage are supplied by the caller.

## v0.1 status

Implemented:

- `PAR1` footer validation and compact-Thrift `FileMetaData` parsing.
- Schema trees with optional/repeated definition and repetition levels.
- Nested group/LIST metadata and logical annotations.
- Row groups, column chunks, page headers, Data Page v1 and v2.
- Named path lookup, with unambiguous leaf-name fallback.
- PLAIN values for BOOLEAN, INT32, INT64, FLOAT, DOUBLE, BYTE_ARRAY, and
  FIXED_LEN_BYTE_ARRAY.
- Dictionary pages plus PLAIN_DICTIONARY/RLE_DICTIONARY indexes.
- RLE/bit-packed hybrid and pure bit-packed primitives.
- Contiguous input and partial callback-driven input.
- A codec callback boundary suitable for PicoZstd; no codec is bundled.
- Explicit errors for unsupported types, encodings, codecs, encryption, and
  external column files.

Writing, allocation, encryption, page indexes, CRC verification, INT96, delta
encodings, BYTE_STREAM_SPLIT, and general-purpose nested value materialization
are outside v0.1. Page headers are bounded by `PP_PAGE_HEADER_MAX` (1024
bytes); insufficient caller-provided metadata, path, dictionary, or scratch
storage returns `PP_ERR_CAPACITY`.

## API outline

```c
#include <picoparquet.h>

pp_input input;
pp_reader reader;
pp_reader_storage storage = {
    .schema = schema_nodes, .schema_capacity = schema_count,
    .row_groups = row_groups, .row_group_capacity = row_group_count,
    .columns = columns, .column_capacity = column_count,
    .paths = path_components, .path_capacity = path_count,
    .metadata = footer_bytes, .metadata_capacity = sizeof footer_bytes,
    .scratch = page_scratch, .scratch_capacity = sizeof page_scratch,
    .dictionary = dictionary_bytes, .dictionary_capacity = sizeof dictionary_bytes
};

pp_input_from_memory(&input, mapped_bytes, mapped_size);
pp_reader_open(&reader, &input, &storage, NULL);

pp_column_cursor cursor;
pp_column_cursor_init(&cursor, &reader, 0, "conversation.messages");
pp_column_cursor_read(&cursor, on_value, user_context);
```

`pp_input_from_memory` is the mmap-like path. For streaming storage, fill
`pp_input` (zero-initialized) with a `pp_input_read_at_fn`; PicoParquet accepts
short successful reads and repeatedly requests the remaining range, so the
callback can use a bounded transport buffer.
The callback path requires the file size because Parquet metadata is located
from the end of the file.

`pp_column_cursor_read` reports one physical leaf item at a time, including
repetition and definition levels. A null item has `pp_value.is_null` set and
does not consume a value entry; callers can reconstruct the supported nested
LIST/STRUCT shape from those levels.

For callback-backed input, page scratch must hold the compressed page and its
decoded form when a codec is used (`compressed_size + uncompressed_size`).
Contiguous uncompressed pages can be read directly without page scratch.
Dictionary storage holds one decoded dictionary page. All returned spans remain
valid until the next operation that reuses the corresponding caller buffer.
The reader's scratch and dictionary buffers are shared; use one active cursor
at a time unless the caller provides separate reader/storage instances.
Keep metadata storage separate from scratch and dictionary storage because
schema and path spans refer into the copied footer.

Supply a codec without coupling PicoParquet to an implementation:

```c
pp_codec codec = { picozstd_decode, picozstd_context };
pp_reader_open(&reader, &input, &storage, &codec);
```

The callback receives the Parquet compression enum, source bytes, destination
capacity, and an output size. Without it, compressed columns return
`PP_ERR_UNSUPPORTED_CODEC`; uncompressed columns need no callback.

## Build and test

```sh
cmake -S . -B build -DPICOPARQUET_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The deterministic test constructs compact-Thrift metadata and v1 pages in
caller-owned buffers, exercises named lookup, nested schema metadata,
streaming short reads, dictionary values, PLAIN, RLE/bit-pack, codec errors,
and the codec callback boundary.
