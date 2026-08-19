#ifndef PICOPARQUET_H
#define PICOPARQUET_H

/*
 * PicoParquet: a small, dependency-free, read-only Parquet substrate.
 *
 * The library never allocates memory.  Metadata arrays, metadata storage,
 * page scratch, and dictionary storage are supplied by the caller.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PP_VERSION_MAJOR 0
#define PP_VERSION_MINOR 1
#define PP_VERSION_PATCH 0

#define PP_PAGE_HEADER_MAX 1024u

typedef enum pp_status {
    PP_OK = 0,
    PP_ERR_ARGUMENT,
    PP_ERR_STATE,
    PP_ERR_IO,
    PP_ERR_TRUNCATED,
    PP_ERR_BAD_MAGIC,
    PP_ERR_METADATA,
    PP_ERR_CAPACITY,
    PP_ERR_NOT_FOUND,
    PP_ERR_RANGE,
    PP_ERR_END,
    PP_ERR_UNSUPPORTED,
    PP_ERR_UNSUPPORTED_TYPE,
    PP_ERR_UNSUPPORTED_ENCODING,
    PP_ERR_UNSUPPORTED_CODEC,
    PP_ERR_CODEC,
    PP_ERR_CALLBACK
} pp_status;

const char *pp_status_string(pp_status status);

typedef struct pp_span {
    const uint8_t *data;
    size_t size;
} pp_span;

typedef enum pp_physical_type {
    PP_TYPE_NONE = -1,
    PP_TYPE_BOOLEAN = 0,
    PP_TYPE_INT32 = 1,
    PP_TYPE_INT64 = 2,
    PP_TYPE_INT96 = 3,
    PP_TYPE_FLOAT = 4,
    PP_TYPE_DOUBLE = 5,
    PP_TYPE_BYTE_ARRAY = 6,
    PP_TYPE_FIXED_LEN_BYTE_ARRAY = 7
} pp_physical_type;

typedef enum pp_repetition_type {
    PP_REPETITION_REQUIRED = 0,
    PP_REPETITION_OPTIONAL = 1,
    PP_REPETITION_REPEATED = 2,
    PP_REPETITION_NONE = -1
} pp_repetition_type;

typedef enum pp_converted_type {
    PP_CONVERTED_NONE = -1,
    PP_CONVERTED_UTF8 = 0,
    PP_CONVERTED_MAP = 1,
    PP_CONVERTED_MAP_KEY_VALUE = 2,
    PP_CONVERTED_LIST = 3,
    PP_CONVERTED_ENUM = 4,
    PP_CONVERTED_DECIMAL = 5,
    PP_CONVERTED_DATE = 6,
    PP_CONVERTED_TIME_MILLIS = 7,
    PP_CONVERTED_TIME_MICROS = 8,
    PP_CONVERTED_TIMESTAMP_MILLIS = 9,
    PP_CONVERTED_TIMESTAMP_MICROS = 10,
    PP_CONVERTED_UINT_8 = 11,
    PP_CONVERTED_UINT_16 = 12,
    PP_CONVERTED_UINT_32 = 13,
    PP_CONVERTED_UINT_64 = 14,
    PP_CONVERTED_INT_8 = 15,
    PP_CONVERTED_INT_16 = 16,
    PP_CONVERTED_INT_32 = 17,
    PP_CONVERTED_INT_64 = 18,
    PP_CONVERTED_JSON = 19,
    PP_CONVERTED_BSON = 20,
    PP_CONVERTED_INTERVAL = 21
} pp_converted_type;

typedef enum pp_logical_type {
    PP_LOGICAL_NONE = -1,
    PP_LOGICAL_UTF8 = 0,
    PP_LOGICAL_MAP = 1,
    PP_LOGICAL_LIST = 2,
    PP_LOGICAL_ENUM = 3,
    PP_LOGICAL_DECIMAL = 4,
    PP_LOGICAL_DATE = 5,
    PP_LOGICAL_TIME = 6,
    PP_LOGICAL_INTEGER = 7,
    PP_LOGICAL_JSON = 8,
    PP_LOGICAL_BSON = 9,
    PP_LOGICAL_UUID = 10,
    PP_LOGICAL_TIMESTAMP = 11,
    PP_LOGICAL_UNKNOWN = 12
} pp_logical_type;

typedef enum pp_encoding {
    PP_ENCODING_PLAIN = 0,
    PP_ENCODING_PLAIN_DICTIONARY = 2,
    PP_ENCODING_RLE = 3,
    PP_ENCODING_BIT_PACKED = 4,
    PP_ENCODING_DELTA_BINARY_PACKED = 5,
    PP_ENCODING_DELTA_LENGTH_BYTE_ARRAY = 6,
    PP_ENCODING_DELTA_BYTE_ARRAY = 7,
    PP_ENCODING_RLE_DICTIONARY = 8,
    PP_ENCODING_BYTE_STREAM_SPLIT = 9
} pp_encoding;

typedef enum pp_compression {
    PP_COMPRESSION_UNCOMPRESSED = 0,
    PP_COMPRESSION_SNAPPY = 1,
    PP_COMPRESSION_GZIP = 2,
    PP_COMPRESSION_LZO = 3,
    PP_COMPRESSION_BROTLI = 4,
    PP_COMPRESSION_LZ4 = 5,
    PP_COMPRESSION_ZSTD = 6,
    PP_COMPRESSION_LZ4_RAW = 7
} pp_compression;

typedef enum pp_page_type {
    PP_PAGE_DATA = 0,
    PP_PAGE_INDEX = 1,
    PP_PAGE_DICTIONARY = 2,
    PP_PAGE_DATA_V2 = 3
} pp_page_type;

typedef struct pp_value {
    pp_physical_type type;
    uint8_t is_null;
    union {
        uint8_t boolean;
        int32_t i32;
        int64_t i64;
        float f32;
        double f64;
        pp_span bytes;
    } as;
} pp_value;

typedef struct pp_schema_node {
    int32_t parent;
    pp_span name;
    pp_physical_type type;
    pp_repetition_type repetition;
    pp_converted_type converted;
    pp_logical_type logical;
    int32_t type_length;
    int32_t num_children;
    int32_t field_id;
    int32_t precision;
    int32_t scale;
    uint16_t max_definition_level;
    uint16_t max_repetition_level;
} pp_schema_node;

typedef struct pp_path_component {
    pp_span name;
} pp_path_component;

typedef struct pp_column_chunk {
    pp_physical_type type;
    pp_compression compression;
    uint32_t encoding_mask;
    uint32_t path_offset;
    uint32_t path_count;
    int32_t schema_index;
    uint64_t file_offset;
    uint64_t data_page_offset;
    uint64_t dictionary_page_offset;
    uint64_t num_values;
    uint64_t total_uncompressed_size;
    uint64_t total_compressed_size;
} pp_column_chunk;

typedef struct pp_row_group {
    uint32_t first_column;
    uint32_t column_count;
    uint64_t num_rows;
    uint64_t total_byte_size;
} pp_row_group;

typedef pp_status (*pp_input_read_at_fn)(
    void *context,
    uint64_t offset,
    void *destination,
    size_t bytes,
    size_t *bytes_read);

typedef struct pp_input {
    uint64_t size;
    pp_input_read_at_fn read_at;
    void *context;
    const uint8_t *contiguous; /* non-NULL only for contiguous input */
} pp_input;

pp_status pp_input_from_memory(pp_input *input, const void *data, size_t size);

typedef pp_status (*pp_codec_decode_fn)(
    void *context,
    pp_compression codec,
    const uint8_t *compressed,
    size_t compressed_size,
    uint8_t *uncompressed,
    size_t uncompressed_capacity,
    size_t *uncompressed_size);

typedef struct pp_codec {
    pp_codec_decode_fn decode;
    void *context;
} pp_codec;

typedef struct pp_reader_storage {
    pp_schema_node *schema;
    size_t schema_capacity;
    pp_row_group *row_groups;
    size_t row_group_capacity;
    pp_column_chunk *columns;
    size_t column_capacity;
    pp_path_component *paths;
    size_t path_capacity;
    uint8_t *metadata;
    size_t metadata_capacity;
    uint8_t *scratch;
    size_t scratch_capacity;
    uint8_t *dictionary;
    size_t dictionary_capacity;
} pp_reader_storage;

typedef struct pp_reader {
    pp_input input;
    pp_codec codec;
    pp_reader_storage storage;
    pp_span metadata;
    pp_span created_by;
    uint64_t num_rows;
    size_t schema_count;
    size_t row_group_count;
    size_t column_count;
    size_t path_count;
    uint32_t footer_length;
    uint64_t footer_offset;
    uint8_t opened;
} pp_reader;

pp_status pp_reader_open(
    pp_reader *reader,
    const pp_input *input,
    const pp_reader_storage *storage,
    const pp_codec *codec);

void pp_reader_close(pp_reader *reader);

size_t pp_reader_schema_count(const pp_reader *reader);
size_t pp_reader_row_group_count(const pp_reader *reader);
uint64_t pp_reader_num_rows(const pp_reader *reader);
const pp_schema_node *pp_reader_schema_at(const pp_reader *reader, size_t index);
const pp_row_group *pp_reader_row_group_at(const pp_reader *reader, size_t index);
const pp_column_chunk *pp_reader_column_at(const pp_reader *reader, size_t index);

typedef struct pp_column_ref {
    size_t row_group;
    size_t column;
    int32_t schema_index;
    const pp_column_chunk *chunk;
} pp_column_ref;

pp_status pp_reader_find_column(
    const pp_reader *reader,
    size_t row_group,
    const char *path,
    pp_column_ref *result);

pp_status pp_reader_find_schema(
    const pp_reader *reader,
    const char *path,
    size_t *schema_index);

typedef struct pp_page_header {
    pp_page_type type;
    int32_t uncompressed_page_size;
    int32_t compressed_page_size;
    int32_t crc;
    int32_t num_values;
    int32_t num_nulls;
    int32_t num_rows;
    pp_encoding encoding;
    pp_encoding definition_level_encoding;
    pp_encoding repetition_level_encoding;
    uint32_t definition_levels_byte_length;
    uint32_t repetition_levels_byte_length;
    uint8_t is_compressed;
    uint32_t dictionary_num_values;
} pp_page_header;

typedef struct pp_page {
    pp_page_header header;
    pp_physical_type type;
    int32_t type_length;
    uint16_t max_definition_level;
    uint16_t max_repetition_level;
    pp_span repetition_levels;
    pp_span definition_levels;
    pp_span values;
    pp_span dictionary;
    uint32_t dictionary_count;
} pp_page;

typedef struct pp_column_cursor {
    pp_reader *reader;
    pp_column_ref reference;
    uint64_t offset;
    uint64_t end_offset;
    uint64_t rows_seen;
    uint64_t current_row;
    uint32_t page_number;
    uint32_t dictionary_count;
    size_t dictionary_size;
    uint8_t dictionary_loaded;
    uint8_t row_open;
    uint8_t header_storage[PP_PAGE_HEADER_MAX];
    pp_page_header header;
} pp_column_cursor;

pp_status pp_column_cursor_init(
    pp_column_cursor *cursor,
    pp_reader *reader,
    size_t row_group,
    const char *path);

pp_status pp_column_cursor_next_page(pp_column_cursor *cursor, pp_page *page);

typedef struct pp_plain_decoder {
    pp_physical_type type;
    int32_t type_length;
    const uint8_t *data;
    size_t size;
    size_t offset;
    uint8_t boolean_bit;
    uint8_t boolean_byte;
} pp_plain_decoder;

pp_status pp_plain_decoder_init(
    pp_plain_decoder *decoder,
    pp_physical_type type,
    int32_t type_length,
    const uint8_t *data,
    size_t size);

pp_status pp_plain_decoder_next(pp_plain_decoder *decoder, pp_value *value);

typedef struct pp_rle_decoder {
    const uint8_t *data;
    size_t size;
    size_t offset;
    uint8_t bit_width;
    uint8_t length_prefixed;
    uint32_t run_remaining;
    uint32_t run_value;
    uint32_t bitpack_remaining;
    uint32_t bitpack_index;
    uint64_t bitpack_bits;
    uint8_t bitpack_bits_count;
} pp_rle_decoder;

pp_status pp_rle_decoder_init(
    pp_rle_decoder *decoder,
    const uint8_t *data,
    size_t size,
    uint8_t bit_width,
    uint8_t length_prefixed);

pp_status pp_rle_decoder_next(pp_rle_decoder *decoder, uint32_t *value);

typedef struct pp_bitpack_decoder {
    const uint8_t *data;
    size_t size;
    size_t offset;
    uint8_t bit_width;
    uint32_t remaining;
    uint64_t bits;
    uint8_t bits_count;
} pp_bitpack_decoder;

pp_status pp_bitpack_decoder_init(
    pp_bitpack_decoder *decoder,
    const uint8_t *data,
    size_t size,
    uint8_t bit_width,
    uint32_t value_count);

pp_status pp_bitpack_decoder_next(pp_bitpack_decoder *decoder, uint32_t *value);

typedef struct pp_page_decoder {
    const pp_page *page;
    pp_rle_decoder repetition;
    pp_rle_decoder definition;
    pp_rle_decoder indices;
    pp_bitpack_decoder repetition_bitpack;
    pp_bitpack_decoder definition_bitpack;
    pp_plain_decoder plain;
    uint32_t index;
    uint32_t value_index;
    uint8_t repetition_enabled;
    uint8_t definition_enabled;
    uint8_t dictionary_encoded;
} pp_page_decoder;

pp_status pp_page_decoder_init(pp_page_decoder *decoder, const pp_page *page);
pp_status pp_page_decoder_next(
    pp_page_decoder *decoder,
    int32_t *repetition_level,
    int32_t *definition_level,
    pp_value *value);

typedef pp_status (*pp_value_callback)(
    void *context,
    uint64_t row_index,
    int32_t repetition_level,
    int32_t definition_level,
    const pp_value *value);

pp_status pp_column_cursor_read(
    pp_column_cursor *cursor,
    pp_value_callback callback,
    void *context);

#ifdef __cplusplus
}
#endif

#endif
