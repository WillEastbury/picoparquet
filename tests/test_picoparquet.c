#include "picoparquet.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

typedef struct test_buffer {
    uint8_t *data;
    size_t size;
    size_t capacity;
} test_buffer;

static void tb_put(test_buffer *buffer, uint8_t byte)
{
    assert(buffer->size < buffer->capacity);
    buffer->data[buffer->size++] = byte;
}

static void tb_bytes(test_buffer *buffer, const void *data, size_t size)
{
    assert(size <= buffer->capacity - buffer->size);
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
}

static void tb_var_u64(test_buffer *buffer, uint64_t value)
{
    while (value >= 0x80u) {
        tb_put(buffer, (uint8_t)(value | 0x80u));
        value >>= 7;
    }
    tb_put(buffer, (uint8_t)value);
}

static uint64_t tb_zigzag_i64(int64_t value)
{
    return ((uint64_t)value << 1) ^ (uint64_t)-(value < 0);
}

static void tb_i32(test_buffer *buffer, int32_t value)
{
    tb_var_u64(buffer, tb_zigzag_i64(value));
}

static void tb_i64(test_buffer *buffer, int64_t value)
{
    tb_var_u64(buffer, tb_zigzag_i64(value));
}

static void tb_fixed_u32(test_buffer *buffer, uint32_t value)
{
    tb_put(buffer, (uint8_t)value);
    tb_put(buffer, (uint8_t)(value >> 8));
    tb_put(buffer, (uint8_t)(value >> 16));
    tb_put(buffer, (uint8_t)(value >> 24));
}

static void tb_binary(test_buffer *buffer, const char *text)
{
    size_t size = strlen(text);
    tb_var_u64(buffer, size);
    tb_bytes(buffer, text, size);
}

static void tb_field(
    test_buffer *buffer,
    uint8_t type,
    int field_id,
    int *last_field)
{
    int delta = field_id - *last_field;
    if (delta > 0 && delta < 15) {
        tb_put(buffer, (uint8_t)((type << 4) | (uint8_t)delta));
    } else {
        assert(delta >= -32768 && delta <= 32767);
        tb_put(buffer, (uint8_t)((type << 4) | 15u));
        tb_var_u64(buffer, tb_zigzag_i64(field_id));
    }
    *last_field = field_id;
}

static void tb_stop(test_buffer *buffer)
{
    tb_put(buffer, 0);
}

static void tb_list_header(test_buffer *buffer, uint64_t count, uint8_t type)
{
    if (count < 15) {
        tb_put(buffer, (uint8_t)((uint8_t)count << 4 | type));
    } else {
        tb_put(buffer, (uint8_t)(0xf0u | type));
        tb_var_u64(buffer, count);
    }
}

static void tb_logical_list(test_buffer *buffer)
{
    int last = 0;
    tb_field(buffer, 12, 3, &last);
    tb_stop(buffer);
    tb_stop(buffer);
}

static void tb_schema_element(
    test_buffer *buffer,
    const char *name,
    int type,
    int repetition,
    int children,
    int converted,
    int logical)
{
    int last = 0;
    if (type >= 0) {
        tb_field(buffer, 5, 1, &last);
        tb_i32(buffer, type);
    }
    if (type == PP_TYPE_FIXED_LEN_BYTE_ARRAY) {
        tb_field(buffer, 5, 2, &last);
        tb_i32(buffer, 4);
    }
    if (repetition >= 0) {
        tb_field(buffer, 5, 3, &last);
        tb_i32(buffer, repetition);
    }
    tb_field(buffer, 8, 4, &last);
    tb_binary(buffer, name);
    if (children > 0) {
        tb_field(buffer, 5, 5, &last);
        tb_i32(buffer, children);
    }
    if (converted >= 0) {
        tb_field(buffer, 5, 6, &last);
        tb_i32(buffer, converted);
    }
    if (logical >= 0) {
        tb_field(buffer, 12, 10, &last);
        if (logical == PP_LOGICAL_LIST) {
            tb_logical_list(buffer);
        } else {
            tb_stop(buffer);
        }
    }
    tb_stop(buffer);
}

static void tb_data_page_header(
    test_buffer *buffer,
    int num_values,
    int encoding)
{
    int last = 0;
    tb_field(buffer, 5, 1, &last);
    tb_i32(buffer, num_values);
    tb_field(buffer, 5, 2, &last);
    tb_i32(buffer, encoding);
    tb_field(buffer, 5, 3, &last);
    tb_i32(buffer, PP_ENCODING_RLE);
    tb_field(buffer, 5, 4, &last);
    tb_i32(buffer, PP_ENCODING_RLE);
    tb_stop(buffer);
}

static void tb_dictionary_page_header(test_buffer *buffer, int count)
{
    int last = 0;
    tb_field(buffer, 5, 1, &last);
    tb_i32(buffer, count);
    tb_field(buffer, 5, 2, &last);
    tb_i32(buffer, PP_ENCODING_PLAIN);
    tb_stop(buffer);
}

static size_t tb_page_v1(
    uint8_t *file,
    size_t offset,
    int num_values,
    int encoding,
    const uint8_t *payload,
    size_t payload_size,
    int dictionary)
{
    uint8_t header_bytes[128];
    test_buffer header = { header_bytes, 0, sizeof(header_bytes) };
    int last = 0;

    tb_field(&header, 5, 1, &last);
    tb_i32(&header, dictionary ? PP_PAGE_DICTIONARY : PP_PAGE_DATA);
    tb_field(&header, 5, 2, &last);
    tb_i32(&header, (int32_t)payload_size);
    tb_field(&header, 5, 3, &last);
    tb_i32(&header, (int32_t)payload_size);
    if (dictionary) {
        tb_field(&header, 12, 7, &last);
        tb_dictionary_page_header(&header, num_values);
    } else {
        tb_field(&header, 12, 5, &last);
        tb_data_page_header(&header, num_values, encoding);
    }
    tb_stop(&header);
    memcpy(file + offset, header.data, header.size);
    offset += header.size;
    memcpy(file + offset, payload, payload_size);
    return offset + payload_size;
}

static void tb_column_chunk(
    test_buffer *footer,
    const char *path,
    int type,
    int encoding,
    int64_t num_values,
    int64_t uncompressed_size,
    int64_t compressed_size,
    int64_t data_offset,
    int64_t dictionary_offset,
    int compression)
{
    int last = 0;
    int metadata_last = 0;
    tb_field(footer, 12, 3, &last);
    tb_field(footer, 5, 1, &metadata_last);
    tb_i32(footer, type);
    tb_field(footer, 9, 2, &metadata_last);
    tb_list_header(footer, 1, 5);
    tb_i32(footer, encoding);
    tb_field(footer, 9, 3, &metadata_last);
    tb_list_header(footer, 1, 8);
    tb_binary(footer, path);
    tb_field(footer, 5, 4, &metadata_last);
    tb_i32(footer, compression);
    tb_field(footer, 6, 5, &metadata_last);
    tb_i64(footer, num_values);
    tb_field(footer, 6, 6, &metadata_last);
    tb_i64(footer, uncompressed_size);
    tb_field(footer, 6, 7, &metadata_last);
    tb_i64(footer, compressed_size);
    tb_field(footer, 6, 8, &metadata_last);
    tb_i64(footer, data_offset);
    if (dictionary_offset >= 0) {
        tb_field(footer, 6, 10, &metadata_last);
        tb_i64(footer, dictionary_offset);
    }
    tb_stop(footer);
    tb_stop(footer);
}

static size_t make_fixture(
    uint8_t *file,
    size_t capacity,
    int id_compression)
{
    test_buffer output = { file, 0, capacity };
    uint8_t id_payload[20];
    uint8_t dictionary_payload[10];
    uint8_t name_payload[17];
    uint8_t footer_bytes[2048];
    test_buffer footer = { footer_bytes, 0, sizeof(footer_bytes) };
    size_t id_start;
    size_t id_end;
    size_t dictionary_start;
    size_t dictionary_end;
    size_t name_start;
    size_t name_end;
    size_t footer_start;
    int last;

    tb_bytes(&output, "PAR1", 4);
    memset(id_payload, 0, sizeof(id_payload));
    id_payload[8] = 10;
    id_payload[9] = 0;
    id_payload[10] = 0;
    id_payload[11] = 0;
    id_payload[12] = 20;
    id_payload[13] = 0;
    id_payload[14] = 0;
    id_payload[15] = 0;
    id_payload[16] = 30;
    id_payload[17] = 0;
    id_payload[18] = 0;
    id_payload[19] = 0;
    id_start = output.size;
    id_end = tb_page_v1(
        file, id_start, 3, PP_ENCODING_PLAIN,
        id_payload, sizeof(id_payload), 0);
    output.size = id_end;

    dictionary_payload[0] = 1;
    dictionary_payload[1] = 0;
    dictionary_payload[2] = 0;
    dictionary_payload[3] = 0;
    dictionary_payload[4] = 'a';
    dictionary_payload[5] = 1;
    dictionary_payload[6] = 0;
    dictionary_payload[7] = 0;
    dictionary_payload[8] = 0;
    dictionary_payload[9] = 'b';
    dictionary_start = output.size;
    dictionary_end = tb_page_v1(
        file, dictionary_start, 2, PP_ENCODING_PLAIN,
        dictionary_payload, sizeof(dictionary_payload), 1);
    output.size = dictionary_end;

    memset(name_payload, 0, sizeof(name_payload));
    name_payload[0] = 0;
    name_payload[1] = 0;
    name_payload[2] = 0;
    name_payload[3] = 0;
    name_payload[4] = 2;
    name_payload[5] = 0;
    name_payload[6] = 0;
    name_payload[7] = 0;
    name_payload[8] = 3;
    name_payload[9] = 5;
    name_payload[10] = 1;
    name_payload[11] = 2;
    name_payload[12] = 0;
    name_payload[13] = 0;
    name_payload[14] = 0;
    name_payload[15] = 3;
    name_payload[16] = 2;
    name_start = output.size;
    name_end = tb_page_v1(
        file, name_start, 3, PP_ENCODING_PLAIN_DICTIONARY,
        name_payload, sizeof(name_payload), 0);
    output.size = name_end;

    last = 0;
    tb_field(&footer, 5, 1, &last);
    tb_i32(&footer, 1);
    tb_field(&footer, 9, 2, &last);
    tb_list_header(&footer, 6, 12);
    tb_schema_element(&footer, "schema", -1, -1, 3, -1, -1);
    tb_schema_element(&footer, "id", PP_TYPE_INT32,
                      PP_REPETITION_REQUIRED, 0, -1, -1);
    tb_schema_element(&footer, "name", PP_TYPE_BYTE_ARRAY,
                      PP_REPETITION_OPTIONAL, 0, PP_CONVERTED_UTF8, -1);
    tb_schema_element(&footer, "messages", -1,
                      PP_REPETITION_OPTIONAL, 1, PP_CONVERTED_LIST,
                      PP_LOGICAL_LIST);
    tb_schema_element(&footer, "list", -1,
                      PP_REPETITION_REPEATED, 1, -1, -1);
    tb_schema_element(&footer, "element", PP_TYPE_BYTE_ARRAY,
                      PP_REPETITION_OPTIONAL, 0, PP_CONVERTED_UTF8, -1);
    tb_field(&footer, 6, 3, &last);
    tb_i64(&footer, 3);
    tb_field(&footer, 9, 4, &last);
    tb_list_header(&footer, 1, 12);
    {
        int group_last = 0;
        tb_field(&footer, 9, 1, &group_last);
        tb_list_header(&footer, 2, 12);
        tb_column_chunk(
            &footer, "id", PP_TYPE_INT32, PP_ENCODING_PLAIN, 3,
            (int64_t)(id_end - id_start), (int64_t)(id_end - id_start),
            (int64_t)id_start, -1, id_compression);
        tb_column_chunk(
            &footer, "name", PP_TYPE_BYTE_ARRAY,
            PP_ENCODING_PLAIN_DICTIONARY, 3,
            (int64_t)(name_end - dictionary_start),
            (int64_t)(name_end - dictionary_start),
            (int64_t)name_start, (int64_t)dictionary_start,
            PP_COMPRESSION_UNCOMPRESSED);
        tb_field(&footer, 6, 2, &group_last);
        tb_i64(&footer, (int64_t)(
            (id_end - id_start) + (name_end - dictionary_start)));
        tb_field(&footer, 6, 3, &group_last);
        tb_i64(&footer, 3);
        tb_stop(&footer);
    }
    tb_stop(&footer);

    footer_start = output.size;
    tb_bytes(&output, footer.data, footer.size);
    tb_fixed_u32(&output, (uint32_t)footer.size);
    tb_bytes(&output, "PAR1", 4);
    (void)footer_start;
    return output.size;
}

typedef struct chunk_reader {
    const uint8_t *data;
    size_t size;
    size_t maximum;
} chunk_reader;

static pp_status chunk_read_at(
    void *context,
    uint64_t offset,
    void *destination,
    size_t bytes,
    size_t *bytes_read)
{
    chunk_reader *reader = (chunk_reader *)context;
    size_t available;
    size_t count;
    if (offset >= reader->size) {
        *bytes_read = 0;
        return PP_OK;
    }
    available = reader->size - (size_t)offset;
    count = bytes < available ? bytes : available;
    if (count > reader->maximum) {
        count = reader->maximum;
    }
    memcpy(destination, reader->data + (size_t)offset, count);
    *bytes_read = count;
    return PP_OK;
}

typedef struct values {
    int count;
    int value_count;
    int nulls;
    int32_t ints[4];
    char strings[4][8];
} values;

static pp_status collect_values(
    void *context,
    uint64_t row_index,
    int32_t repetition_level,
    int32_t definition_level,
    const pp_value *value)
{
    values *collected = (values *)context;
    (void)row_index;
    (void)repetition_level;
    (void)definition_level;
    if (value->is_null) {
        collected->nulls++;
    } else if (value->type == PP_TYPE_INT32) {
        collected->ints[collected->value_count] = value->as.i32;
    } else if (value->type == PP_TYPE_BYTE_ARRAY) {
        assert(value->as.bytes.size < sizeof(collected->strings[0]));
        memcpy(
            collected->strings[collected->value_count],
            value->as.bytes.data,
            value->as.bytes.size);
        collected->strings[collected->value_count][value->as.bytes.size] = '\0';
    }
    if (!value->is_null) {
        collected->value_count++;
    }
    collected->count++;
    return PP_OK;
}

static pp_status identity_codec(
    void *context,
    pp_compression codec,
    const uint8_t *compressed,
    size_t compressed_size,
    uint8_t *uncompressed,
    size_t uncompressed_capacity,
    size_t *uncompressed_size)
{
    int *calls = (int *)context;
    assert(codec == PP_COMPRESSION_ZSTD);
    assert(uncompressed_capacity >= compressed_size);
    memcpy(uncompressed, compressed, compressed_size);
    *uncompressed_size = compressed_size;
    (*calls)++;
    return PP_OK;
}

int main(void)
{
    uint8_t file[8192];
    uint8_t metadata[2048];
    uint8_t scratch[512];
    uint8_t dictionary[128];
    pp_schema_node schema[8];
    pp_row_group groups[2];
    pp_column_chunk columns[4];
    pp_path_component paths[8];
    pp_reader_storage storage;
    pp_input input;
    pp_reader reader;
    pp_column_ref reference;
    pp_column_cursor cursor;
    values collected;
    size_t file_size;
    int codec_calls = 0;
    pp_codec codec;
    chunk_reader chunk;
    pp_input streaming_input;
    pp_plain_decoder plain;
    pp_rle_decoder rle;
    pp_bitpack_decoder bitpack;
    pp_value value;
    uint32_t decoded;
    size_t schema_index;

    file_size = make_fixture(file, sizeof(file), PP_COMPRESSION_UNCOMPRESSED);
    memset(&storage, 0, sizeof(storage));
    storage.schema = schema;
    storage.schema_capacity = 8;
    storage.row_groups = groups;
    storage.row_group_capacity = 2;
    storage.columns = columns;
    storage.column_capacity = 4;
    storage.paths = paths;
    storage.path_capacity = 8;
    storage.metadata = metadata;
    storage.metadata_capacity = sizeof(metadata);
    storage.scratch = scratch;
    storage.scratch_capacity = sizeof(scratch);
    storage.dictionary = dictionary;
    storage.dictionary_capacity = sizeof(dictionary);

    assert(pp_input_from_memory(&input, file, file_size) == PP_OK);
    assert(pp_reader_open(&reader, &input, &storage, NULL) == PP_OK);
    assert(pp_reader_num_rows(&reader) == 3);
    assert(pp_reader_schema_count(&reader) == 6);
    assert(pp_reader_row_group_count(&reader) == 1);
    assert(pp_reader_find_schema(
               &reader, "messages.list.element", &schema_index) == PP_OK);
    assert(schema[schema_index].max_definition_level == 3);
    assert(schema[schema_index].max_repetition_level == 1);
    assert(pp_reader_find_column(&reader, 0, "name", &reference) == PP_OK);
    assert(reference.chunk->type == PP_TYPE_BYTE_ARRAY);

    memset(&collected, 0, sizeof(collected));
    assert(pp_column_cursor_init(&cursor, &reader, 0, "id") == PP_OK);
    assert(pp_column_cursor_read(&cursor, collect_values, &collected) == PP_OK);
    assert(collected.count == 3);
    assert(collected.ints[0] == 10);
    assert(collected.ints[1] == 20);
    assert(collected.ints[2] == 30);

    memset(&collected, 0, sizeof(collected));
    assert(pp_column_cursor_init(&cursor, &reader, 0, "name") == PP_OK);
    assert(pp_column_cursor_read(&cursor, collect_values, &collected) == PP_OK);
    assert(collected.count == 3);
    assert(collected.nulls == 1);
    assert(strcmp(collected.strings[0], "a") == 0);
    assert(strcmp(collected.strings[1], "b") == 0);

    chunk.data = file;
    chunk.size = file_size;
    chunk.maximum = 2;
    memset(&streaming_input, 0, sizeof(streaming_input));
    streaming_input.size = file_size;
    streaming_input.read_at = chunk_read_at;
    streaming_input.context = &chunk;
    assert(pp_reader_open(&reader, &streaming_input, &storage, NULL) == PP_OK);
    memset(&collected, 0, sizeof(collected));
    assert(pp_column_cursor_init(&cursor, &reader, 0, "id") == PP_OK);
    assert(pp_column_cursor_read(&cursor, collect_values, &collected) == PP_OK);
    assert(collected.count == 3 && collected.ints[2] == 30);

    {
        pp_reader_storage mmap_storage = storage;
        mmap_storage.scratch = NULL;
        mmap_storage.scratch_capacity = 0;
        assert(pp_reader_open(&reader, &input, &mmap_storage, NULL) == PP_OK);
        memset(&collected, 0, sizeof(collected));
        assert(pp_column_cursor_init(&cursor, &reader, 0, "name") == PP_OK);
        assert(pp_column_cursor_read(&cursor, collect_values, &collected) == PP_OK);
        assert(collected.count == 3 && collected.nulls == 1);
    }

    {
        pp_reader_storage small_storage = storage;
        small_storage.metadata_capacity = 1;
        assert(pp_reader_open(&reader, &input, &small_storage, NULL) ==
               PP_ERR_CAPACITY);
    }

    file_size = make_fixture(file, sizeof(file), PP_COMPRESSION_ZSTD);
    assert(pp_input_from_memory(&input, file, file_size) == PP_OK);
    assert(pp_reader_open(&reader, &input, &storage, NULL) == PP_OK);
    assert(pp_column_cursor_init(&cursor, &reader, 0, "id") == PP_OK);
    memset(&collected, 0, sizeof(collected));
    assert(pp_column_cursor_read(&cursor, collect_values, &collected) ==
           PP_ERR_UNSUPPORTED_CODEC);
    codec.decode = identity_codec;
    codec.context = &codec_calls;
    assert(pp_reader_open(&reader, &input, &storage, &codec) == PP_OK);
    assert(pp_column_cursor_init(&cursor, &reader, 0, "id") == PP_OK);
    memset(&collected, 0, sizeof(collected));
    assert(pp_column_cursor_read(&cursor, collect_values, &collected) == PP_OK);
    assert(codec_calls == 1);

    {
        uint8_t plain_bytes[] = { 1, 0, 0, 0, 0xFE, 0xFF, 0xFF, 0xFF };
        assert(pp_plain_decoder_init(
                   &plain, PP_TYPE_INT32, 0,
                   plain_bytes, sizeof(plain_bytes)) == PP_OK);
        assert(pp_plain_decoder_next(&plain, &value) == PP_OK);
        assert(value.as.i32 == 1);
        assert(pp_plain_decoder_next(&plain, &value) == PP_OK);
        assert(value.as.i32 == -2);
    }
    {
        uint8_t boolean_bytes[] = { 0x05 };
        assert(pp_plain_decoder_init(
                   &plain, PP_TYPE_BOOLEAN, 0,
                   boolean_bytes, sizeof(boolean_bytes)) == PP_OK);
        assert(pp_plain_decoder_next(&plain, &value) == PP_OK &&
               value.as.boolean == 1);
        assert(pp_plain_decoder_next(&plain, &value) == PP_OK &&
               value.as.boolean == 0);
        assert(pp_plain_decoder_next(&plain, &value) == PP_OK &&
               value.as.boolean == 1);
    }
    assert(pp_plain_decoder_init(
               &plain, PP_TYPE_INT96, 0, NULL, 0) ==
           PP_ERR_UNSUPPORTED_TYPE);
    {
        uint8_t rle_bytes[] = { 3, 5 };
        assert(pp_rle_decoder_init(
                   &rle, rle_bytes, sizeof(rle_bytes), 1, 0) == PP_OK);
        assert(pp_rle_decoder_next(&rle, &decoded) == PP_OK && decoded == 1);
        assert(pp_rle_decoder_next(&rle, &decoded) == PP_OK && decoded == 0);
        assert(pp_rle_decoder_next(&rle, &decoded) == PP_OK && decoded == 1);
    }
    {
        uint8_t run_bytes[] = { 6, 3 };
        assert(pp_rle_decoder_init(
                   &rle, run_bytes, sizeof(run_bytes), 2, 0) == PP_OK);
        assert(pp_rle_decoder_next(&rle, &decoded) == PP_OK && decoded == 3);
        assert(pp_rle_decoder_next(&rle, &decoded) == PP_OK && decoded == 3);
        assert(pp_rle_decoder_next(&rle, &decoded) == PP_OK && decoded == 3);
    }
    {
        uint8_t bitpack_bytes[] = { 0x05 };
        assert(pp_bitpack_decoder_init(
                   &bitpack, bitpack_bytes, sizeof(bitpack_bytes), 1, 3) == PP_OK);
        assert(pp_bitpack_decoder_next(&bitpack, &decoded) == PP_OK && decoded == 1);
        assert(pp_bitpack_decoder_next(&bitpack, &decoded) == PP_OK && decoded == 0);
        assert(pp_bitpack_decoder_next(&bitpack, &decoded) == PP_OK && decoded == 1);
    }
    {
        uint8_t v2_values[] = {
            7, 0, 0, 0,
            9, 0, 0, 0
        };
        pp_page v2_page;
        pp_page_decoder v2_decoder;
        memset(&v2_page, 0, sizeof(v2_page));
        v2_page.header.type = PP_PAGE_DATA_V2;
        v2_page.header.num_values = 2;
        v2_page.header.uncompressed_page_size = (int32_t)sizeof(v2_values);
        v2_page.header.compressed_page_size = (int32_t)sizeof(v2_values);
        v2_page.header.encoding = PP_ENCODING_PLAIN;
        v2_page.header.is_compressed = 0;
        v2_page.type = PP_TYPE_INT32;
        v2_page.values.data = v2_values;
        v2_page.values.size = sizeof(v2_values);
        assert(pp_page_decoder_init(&v2_decoder, &v2_page) == PP_OK);
        assert(pp_page_decoder_next(
                   &v2_decoder, &(int32_t){ 0 }, &(int32_t){ 0 }, &value) == PP_OK);
        assert(value.as.i32 == 7);
        assert(pp_page_decoder_next(
                   &v2_decoder, &(int32_t){ 0 }, &(int32_t){ 0 }, &value) == PP_OK);
        assert(value.as.i32 == 9);
    }

    file[0] = 'X';
    assert(pp_reader_open(&reader, &input, &storage, NULL) == PP_ERR_BAD_MAGIC);
    return 0;
}
