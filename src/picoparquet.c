#include "picoparquet.h"

#include <limits.h>
#include <string.h>

#define PP_T_STOP 0u
#define PP_T_BOOL_TRUE 1u
#define PP_T_BOOL_FALSE 2u
#define PP_T_BYTE 3u
#define PP_T_I16 4u
#define PP_T_I32 5u
#define PP_T_I64 6u
#define PP_T_DOUBLE 7u
#define PP_T_BINARY 8u
#define PP_T_LIST 9u
#define PP_T_SET 10u
#define PP_T_MAP 11u
#define PP_T_STRUCT 12u

#define PP_MAX_THRIFT_DEPTH 64u
#define PP_MAX_SCHEMA_DEPTH 64u

typedef struct pp_thrift {
    const uint8_t *data;
    const uint8_t *end;
} pp_thrift;

typedef struct pp_parse_context {
    pp_reader *reader;
    pp_thrift *thrift;
    int32_t schema_stack[PP_MAX_SCHEMA_DEPTH];
    uint32_t schema_remaining[PP_MAX_SCHEMA_DEPTH];
    size_t schema_depth;
} pp_parse_context;

static pp_status pp_t_u8(pp_thrift *t, uint8_t *value)
{
    if (t == NULL || value == NULL) {
        return PP_ERR_ARGUMENT;
    }
    if (t->data >= t->end) {
        return PP_ERR_TRUNCATED;
    }
    *value = *t->data++;
    return PP_OK;
}

static pp_status pp_t_bytes(pp_thrift *t, const uint8_t **data, size_t size)
{
    if (t == NULL || data == NULL) {
        return PP_ERR_ARGUMENT;
    }
    if ((size_t)(t->end - t->data) < size) {
        return PP_ERR_TRUNCATED;
    }
    *data = t->data;
    t->data += size;
    return PP_OK;
}

static pp_status pp_t_var_u64(pp_thrift *t, uint64_t *value)
{
    uint64_t result = 0;
    unsigned shift = 0;
    unsigned i;

    if (t == NULL || value == NULL) {
        return PP_ERR_ARGUMENT;
    }
    for (i = 0; i < 10; ++i) {
        uint8_t byte;
        pp_status status = pp_t_u8(t, &byte);
        if (status != PP_OK) {
            return status;
        }
        if (shift == 63 && (byte & 0x7fu) > 1u) {
            return PP_ERR_RANGE;
        }
        result |= ((uint64_t)(byte & 0x7fu)) << shift;
        if ((byte & 0x80u) == 0) {
            *value = result;
            return PP_OK;
        }
        shift += 7;
    }
    return PP_ERR_RANGE;
}

static int64_t pp_unzigzag_u64(uint64_t value)
{
    return (int64_t)((value >> 1) ^ (uint64_t)-(int64_t)(value & 1u));
}

static pp_status pp_t_i16(pp_thrift *t, int16_t *value)
{
    uint64_t encoded;
    int64_t decoded;
    pp_status status = pp_t_var_u64(t, &encoded);
    if (status != PP_OK) {
        return status;
    }
    decoded = pp_unzigzag_u64(encoded);
    if (decoded < INT16_MIN || decoded > INT16_MAX) {
        return PP_ERR_RANGE;
    }
    *value = (int16_t)decoded;
    return PP_OK;
}

static pp_status pp_t_i32(pp_thrift *t, int32_t *value)
{
    uint64_t encoded;
    int64_t decoded;
    pp_status status = pp_t_var_u64(t, &encoded);
    if (status != PP_OK) {
        return status;
    }
    decoded = pp_unzigzag_u64(encoded);
    if (decoded < INT32_MIN || decoded > INT32_MAX) {
        return PP_ERR_RANGE;
    }
    *value = (int32_t)decoded;
    return PP_OK;
}

static pp_status pp_t_i64(pp_thrift *t, int64_t *value)
{
    uint64_t encoded;
    pp_status status = pp_t_var_u64(t, &encoded);
    if (status != PP_OK) {
        return status;
    }
    *value = pp_unzigzag_u64(encoded);
    return PP_OK;
}

static pp_status pp_t_binary(pp_thrift *t, pp_span *span)
{
    uint64_t length;
    pp_status status = pp_t_var_u64(t, &length);
    if (status != PP_OK) {
        return status;
    }
    if (length > SIZE_MAX) {
        return PP_ERR_RANGE;
    }
    status = pp_t_bytes(t, &span->data, (size_t)length);
    if (status != PP_OK) {
        return status;
    }
    span->size = (size_t)length;
    return PP_OK;
}

static pp_status pp_t_field(
    pp_thrift *t,
    int16_t *last_field,
    int16_t *field_id,
    uint8_t *field_type,
    uint8_t *bool_value,
    uint8_t *is_stop)
{
    uint8_t header;
    uint8_t compact_id;
    int16_t delta;
    pp_status status;

    if (last_field == NULL || field_id == NULL || field_type == NULL ||
        bool_value == NULL || is_stop == NULL) {
        return PP_ERR_ARGUMENT;
    }
    status = pp_t_u8(t, &header);
    if (status != PP_OK) {
        return status;
    }
    *field_type = (uint8_t)(header >> 4);
    compact_id = (uint8_t)(header & 0x0fu);
    if (*field_type == PP_T_STOP) {
        *is_stop = 1;
        *field_id = 0;
        *bool_value = 0;
        return PP_OK;
    }
    *is_stop = 0;
    if (*field_type == PP_T_BOOL_TRUE || *field_type == PP_T_BOOL_FALSE) {
        *bool_value = (uint8_t)(*field_type == PP_T_BOOL_TRUE);
    } else {
        *bool_value = 0;
    }
    if (compact_id == 15u) {
        status = pp_t_i16(t, &delta);
        if (status != PP_OK) {
            return status;
        }
        *field_id = delta;
    } else {
        if ((int32_t)*last_field + compact_id > INT16_MAX) {
            return PP_ERR_RANGE;
        }
        *field_id = (int16_t)((int32_t)*last_field + compact_id);
    }
    *last_field = *field_id;
    return PP_OK;
}

static pp_status pp_t_list_header(pp_thrift *t, uint64_t *count, uint8_t *element_type)
{
    uint8_t header;
    pp_status status = pp_t_u8(t, &header);
    if (status != PP_OK) {
        return status;
    }
    *element_type = (uint8_t)(header & 0x0fu);
    if ((header >> 4) == 15u) {
        return pp_t_var_u64(t, count);
    }
    *count = (uint64_t)(header >> 4);
    return PP_OK;
}

static pp_status pp_t_map_header(
    pp_thrift *t,
    uint64_t *count,
    uint8_t *key_type,
    uint8_t *value_type)
{
    uint64_t size;
    uint8_t types;
    pp_status status = pp_t_var_u64(t, &size);
    if (status != PP_OK) {
        return status;
    }
    if (size == 0) {
        *count = 0;
        *key_type = 0;
        *value_type = 0;
        return PP_OK;
    }
    status = pp_t_u8(t, &types);
    if (status != PP_OK) {
        return status;
    }
    *count = size;
    *key_type = (uint8_t)(types >> 4);
    *value_type = (uint8_t)(types & 0x0fu);
    return PP_OK;
}

static pp_status pp_t_skip(pp_thrift *t, uint8_t type, unsigned depth);

static pp_status pp_t_skip_struct(pp_thrift *t, unsigned depth)
{
    int16_t last_field = 0;
    if (depth > PP_MAX_THRIFT_DEPTH) {
        return PP_ERR_UNSUPPORTED;
    }
    for (;;) {
        int16_t field_id;
        uint8_t field_type;
        uint8_t bool_value;
        uint8_t is_stop;
        pp_status status = pp_t_field(
            t, &last_field, &field_id, &field_type, &bool_value, &is_stop);
        (void)field_id;
        (void)bool_value;
        if (status != PP_OK) {
            return status;
        }
        if (is_stop) {
            return PP_OK;
        }
        status = pp_t_skip(t, field_type, depth + 1);
        if (status != PP_OK) {
            return status;
        }
    }
}

static pp_status pp_t_skip(pp_thrift *t, uint8_t type, unsigned depth)
{
    uint8_t byte;
    const uint8_t *ignored_bytes;
    uint64_t count;
    uint8_t element_type;
    uint8_t key_type;
    uint8_t value_type;
    size_t i;
    pp_status status;

    if (depth > PP_MAX_THRIFT_DEPTH) {
        return PP_ERR_UNSUPPORTED;
    }
    switch (type) {
    case PP_T_BOOL_TRUE:
    case PP_T_BOOL_FALSE:
        return PP_OK;
    case PP_T_BYTE:
        return pp_t_u8(t, &byte);
    case PP_T_I16:
    case PP_T_I32:
    case PP_T_I64:
        return pp_t_var_u64(t, &count);
    case PP_T_DOUBLE:
        return pp_t_bytes(t, &ignored_bytes, 8);
    case PP_T_BINARY: {
        pp_span span;
        return pp_t_binary(t, &span);
    }
    case PP_T_STRUCT:
        return pp_t_skip_struct(t, depth + 1);
    case PP_T_LIST:
    case PP_T_SET:
        status = pp_t_list_header(t, &count, &element_type);
        if (status != PP_OK) {
            return status;
        }
        if (count > SIZE_MAX) {
            return PP_ERR_RANGE;
        }
        for (i = 0; i < (size_t)count; ++i) {
            if (element_type == PP_T_BOOL_TRUE || element_type == PP_T_BOOL_FALSE) {
                status = pp_t_u8(t, &byte);
            } else {
                status = pp_t_skip(t, element_type, depth + 1);
            }
            if (status != PP_OK) {
                return status;
            }
        }
        return PP_OK;
    case PP_T_MAP:
        status = pp_t_map_header(t, &count, &key_type, &value_type);
        if (status != PP_OK) {
            return status;
        }
        if (count > SIZE_MAX) {
            return PP_ERR_RANGE;
        }
        for (i = 0; i < (size_t)count; ++i) {
            if (key_type == PP_T_BOOL_TRUE || key_type == PP_T_BOOL_FALSE) {
                status = pp_t_u8(t, &byte);
            } else {
                status = pp_t_skip(t, key_type, depth + 1);
            }
            if (status != PP_OK) {
                return status;
            }
            if (value_type == PP_T_BOOL_TRUE || value_type == PP_T_BOOL_FALSE) {
                status = pp_t_u8(t, &byte);
            } else {
                status = pp_t_skip(t, value_type, depth + 1);
            }
            if (status != PP_OK) {
                return status;
            }
        }
        return PP_OK;
    default:
        return PP_ERR_METADATA;
    }
}

static int pp_valid_physical_type(int32_t value)
{
    return value >= PP_TYPE_BOOLEAN && value <= PP_TYPE_FIXED_LEN_BYTE_ARRAY;
}

static int pp_valid_repetition_type(int32_t value)
{
    return value >= PP_REPETITION_REQUIRED && value <= PP_REPETITION_REPEATED;
}

static int pp_valid_converted_type(int32_t value)
{
    return value >= PP_CONVERTED_UTF8 && value <= PP_CONVERTED_INTERVAL;
}

static int pp_valid_encoding(int32_t value)
{
    return value == PP_ENCODING_PLAIN ||
           value == PP_ENCODING_PLAIN_DICTIONARY ||
           value == PP_ENCODING_RLE ||
           value == PP_ENCODING_BIT_PACKED ||
           value == PP_ENCODING_DELTA_BINARY_PACKED ||
           value == PP_ENCODING_DELTA_LENGTH_BYTE_ARRAY ||
           value == PP_ENCODING_DELTA_BYTE_ARRAY ||
           value == PP_ENCODING_RLE_DICTIONARY ||
           value == PP_ENCODING_BYTE_STREAM_SPLIT;
}

static int pp_valid_compression(int32_t value)
{
    return value >= PP_COMPRESSION_UNCOMPRESSED &&
           value <= PP_COMPRESSION_LZ4_RAW;
}

static int pp_valid_page_type(int32_t value)
{
    return value >= PP_PAGE_DATA && value <= PP_PAGE_DATA_V2;
}

static pp_status pp_parse_logical_type(pp_thrift *t, pp_logical_type *logical)
{
    int16_t last_field = 0;
    uint8_t seen = 0;

    for (;;) {
        int16_t field_id;
        uint8_t field_type;
        uint8_t bool_value;
        uint8_t is_stop;
        pp_status status = pp_t_field(
            t, &last_field, &field_id, &field_type, &bool_value, &is_stop);
        (void)bool_value;
        if (status != PP_OK) {
            return status;
        }
        if (is_stop) {
            break;
        }
        if (field_id >= 1 && field_id <= 13) {
            if (field_type != PP_T_STRUCT) {
                return PP_ERR_METADATA;
            }
            if (seen) {
                return PP_ERR_METADATA;
            }
            switch (field_id) {
            case 1: *logical = PP_LOGICAL_UTF8; break;
            case 2: *logical = PP_LOGICAL_MAP; break;
            case 3: *logical = PP_LOGICAL_LIST; break;
            case 4: *logical = PP_LOGICAL_ENUM; break;
            case 5: *logical = PP_LOGICAL_DECIMAL; break;
            case 6: *logical = PP_LOGICAL_DATE; break;
            case 7: *logical = PP_LOGICAL_TIME; break;
            case 8: *logical = PP_LOGICAL_INTEGER; break;
            case 9: *logical = PP_LOGICAL_UNKNOWN; break;
            case 10: *logical = PP_LOGICAL_JSON; break;
            case 11: *logical = PP_LOGICAL_BSON; break;
            case 12: *logical = PP_LOGICAL_UUID; break;
            case 13: *logical = PP_LOGICAL_TIMESTAMP; break;
            default: *logical = PP_LOGICAL_UNKNOWN; break;
            }
            seen = 1;
            status = pp_t_skip(t, field_type, 0);
        } else {
            status = pp_t_skip(t, field_type, 0);
        }
        if (status != PP_OK) {
            return status;
        }
    }
    return PP_OK;
}

static pp_status pp_parse_schema_element(pp_parse_context *context)
{
    pp_reader *reader = context->reader;
    pp_thrift *t = context->thrift;
    pp_schema_node node;
    int16_t last_field = 0;
    uint8_t name_seen = 0;
    int32_t parent = -1;
    size_t index;
    pp_status status;

    while (context->schema_depth > 0 &&
           context->schema_remaining[context->schema_depth - 1] == 0) {
        context->schema_depth--;
    }
    if (reader->schema_count >= reader->storage.schema_capacity ||
        reader->storage.schema == NULL) {
        return PP_ERR_CAPACITY;
    }
    if (context->schema_depth > 0) {
        parent = context->schema_stack[context->schema_depth - 1];
    }
    memset(&node, 0, sizeof(node));
    node.parent = parent;
    node.type = PP_TYPE_NONE;
    node.repetition = PP_REPETITION_NONE;
    node.converted = PP_CONVERTED_NONE;
    node.logical = PP_LOGICAL_NONE;
    node.type_length = -1;
    node.num_children = 0;
    node.field_id = -1;
    node.precision = -1;
    node.scale = -1;
    index = reader->schema_count;

    for (;;) {
        int16_t field_id;
        uint8_t field_type;
        uint8_t bool_value;
        uint8_t is_stop;
        status = pp_t_field(
            t, &last_field, &field_id, &field_type, &bool_value, &is_stop);
        if (status != PP_OK) {
            return status;
        }
        if (is_stop) {
            break;
        }
        switch (field_id) {
        case 1: {
            int32_t value;
            if (field_type != PP_T_I32) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i32(t, &value);
            if (status != PP_OK) {
                return status;
            }
            if (!pp_valid_physical_type(value)) {
                return PP_ERR_UNSUPPORTED_TYPE;
            }
            node.type = (pp_physical_type)value;
            break;
        }
        case 2:
            if (field_type != PP_T_I32) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i32(t, &node.type_length);
            if (status != PP_OK) {
                return status;
            }
            break;
        case 3: {
            int32_t value;
            if (field_type != PP_T_I32) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i32(t, &value);
            if (status != PP_OK) {
                return status;
            }
            if (!pp_valid_repetition_type(value)) {
                return PP_ERR_METADATA;
            }
            node.repetition = (pp_repetition_type)value;
            break;
        }
        case 4:
            if (field_type != PP_T_BINARY) {
                return PP_ERR_METADATA;
            }
            status = pp_t_binary(t, &node.name);
            if (status != PP_OK) {
                return status;
            }
            name_seen = 1;
            break;
        case 5:
            if (field_type != PP_T_I32) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i32(t, &node.num_children);
            if (status != PP_OK) {
                return status;
            }
            if (node.num_children < 0) {
                return PP_ERR_METADATA;
            }
            break;
        case 6: {
            int32_t value;
            if (field_type != PP_T_I32) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i32(t, &value);
            if (status != PP_OK) {
                return status;
            }
            if (!pp_valid_converted_type(value)) {
                return PP_ERR_UNSUPPORTED;
            }
            node.converted = (pp_converted_type)value;
            break;
        }
        case 7:
            if (field_type != PP_T_I32) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i32(t, &node.scale);
            if (status != PP_OK) {
                return status;
            }
            break;
        case 8:
            if (field_type != PP_T_I32) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i32(t, &node.precision);
            if (status != PP_OK) {
                return status;
            }
            break;
        case 9:
            if (field_type != PP_T_I32) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i32(t, &node.field_id);
            if (status != PP_OK) {
                return status;
            }
            break;
        case 10:
            if (field_type != PP_T_STRUCT) {
                return PP_ERR_METADATA;
            }
            status = pp_parse_logical_type(t, &node.logical);
            if (status != PP_OK) {
                return status;
            }
            break;
        default:
            status = pp_t_skip(t, field_type, 0);
            if (status != PP_OK) {
                return status;
            }
            break;
        }
    }
    if (!name_seen) {
        return PP_ERR_METADATA;
    }
    if (node.type != PP_TYPE_NONE && node.num_children != 0) {
        return PP_ERR_METADATA;
    }
    if (node.type == PP_TYPE_FIXED_LEN_BYTE_ARRAY && node.type_length <= 0) {
        return PP_ERR_METADATA;
    }
    if (parent >= 0) {
        if (context->schema_remaining[context->schema_depth - 1] == 0) {
            return PP_ERR_METADATA;
        }
        context->schema_remaining[context->schema_depth - 1]--;
    }
    reader->storage.schema[index] = node;
    reader->schema_count++;
    if (node.num_children > 0) {
        if (context->schema_depth >= PP_MAX_SCHEMA_DEPTH) {
            return PP_ERR_UNSUPPORTED;
        }
        context->schema_stack[context->schema_depth] = (int32_t)index;
        context->schema_remaining[context->schema_depth] =
            (uint32_t)node.num_children;
        context->schema_depth++;
    }
    return PP_OK;
}

static pp_status pp_parse_schema_list(pp_parse_context *context, uint8_t field_type)
{
    uint64_t count;
    uint8_t element_type;
    size_t i;
    pp_status status;

    if (field_type != PP_T_LIST && field_type != PP_T_SET) {
        return PP_ERR_METADATA;
    }
    status = pp_t_list_header(context->thrift, &count, &element_type);
    if (status != PP_OK) {
        return status;
    }
    if (element_type != PP_T_STRUCT || count > SIZE_MAX) {
        return PP_ERR_METADATA;
    }
    if (count > context->reader->storage.schema_capacity ||
        context->reader->schema_count > context->reader->storage.schema_capacity - (size_t)count) {
        return PP_ERR_CAPACITY;
    }
    for (i = 0; i < (size_t)count; ++i) {
        status = pp_parse_schema_element(context);
        if (status != PP_OK) {
            return status;
        }
    }
    while (context->schema_depth > 0 &&
           context->schema_remaining[context->schema_depth - 1] == 0) {
        context->schema_depth--;
    }
    if (context->schema_depth != 0) {
        return PP_ERR_METADATA;
    }
    return PP_OK;
}

static pp_status pp_parse_encoding_list(
    pp_thrift *t,
    uint8_t field_type,
    pp_column_chunk *column)
{
    uint64_t count;
    uint8_t element_type;
    size_t i;
    pp_status status;

    if (field_type != PP_T_LIST && field_type != PP_T_SET) {
        return PP_ERR_METADATA;
    }
    status = pp_t_list_header(t, &count, &element_type);
    if (status != PP_OK) {
        return status;
    }
    if (element_type != PP_T_I32 || count > SIZE_MAX) {
        return PP_ERR_METADATA;
    }
    for (i = 0; i < (size_t)count; ++i) {
        int32_t encoding;
        status = pp_t_i32(t, &encoding);
        if (status != PP_OK) {
            return status;
        }
        if (!pp_valid_encoding(encoding)) {
            return PP_ERR_UNSUPPORTED_ENCODING;
        }
        if (encoding < 32) {
            column->encoding_mask |= (uint32_t)1u << (unsigned)encoding;
        }
    }
    return PP_OK;
}

static pp_status pp_parse_path_list(
    pp_parse_context *context,
    uint8_t field_type,
    pp_column_chunk *column)
{
    pp_reader *reader = context->reader;
    uint64_t count;
    uint8_t element_type;
    size_t i;
    pp_status status;

    if (field_type != PP_T_LIST && field_type != PP_T_SET) {
        return PP_ERR_METADATA;
    }
    status = pp_t_list_header(context->thrift, &count, &element_type);
    if (status != PP_OK) {
        return status;
    }
    if (element_type != PP_T_BINARY || count > UINT32_MAX) {
        return PP_ERR_METADATA;
    }
    column->path_offset = (uint32_t)reader->path_count;
    column->path_count = (uint32_t)count;
    if (count > reader->storage.path_capacity ||
        reader->path_count > reader->storage.path_capacity - (size_t)count ||
        reader->storage.paths == NULL) {
        return PP_ERR_CAPACITY;
    }
    for (i = 0; i < (size_t)count; ++i) {
        pp_span span;
        status = pp_t_binary(context->thrift, &span);
        if (status != PP_OK) {
            return status;
        }
        reader->storage.paths[reader->path_count++].name = span;
    }
    return PP_OK;
}

static pp_status pp_parse_column_metadata(pp_parse_context *context, pp_column_chunk *column)
{
    pp_thrift *t = context->thrift;
    int16_t last_field = 0;
    uint8_t type_seen = 0;
    uint8_t encodings_seen = 0;
    uint8_t path_seen = 0;
    uint8_t codec_seen = 0;
    uint8_t values_seen = 0;
    uint8_t uncompressed_seen = 0;
    uint8_t compressed_seen = 0;
    uint8_t data_offset_seen = 0;

    memset(column, 0, sizeof(*column));
    column->type = PP_TYPE_NONE;
    column->schema_index = -1;
    column->dictionary_page_offset = UINT64_MAX;
    for (;;) {
        int16_t field_id;
        uint8_t field_type;
        uint8_t bool_value;
        uint8_t is_stop;
        pp_status status = pp_t_field(
            t, &last_field, &field_id, &field_type, &bool_value, &is_stop);
        (void)bool_value;
        if (status != PP_OK) {
            return status;
        }
        if (is_stop) {
            break;
        }
        switch (field_id) {
        case 1: {
            int32_t value;
            if (field_type != PP_T_I32) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i32(t, &value);
            if (status != PP_OK) {
                return status;
            }
            if (!pp_valid_physical_type(value)) {
                return PP_ERR_UNSUPPORTED_TYPE;
            }
            column->type = (pp_physical_type)value;
            type_seen = 1;
            break;
        }
        case 2:
            status = pp_parse_encoding_list(t, field_type, column);
            if (status != PP_OK) {
                return status;
            }
            encodings_seen = 1;
            break;
        case 3:
            status = pp_parse_path_list(context, field_type, column);
            if (status != PP_OK) {
                return status;
            }
            path_seen = 1;
            break;
        case 4: {
            int32_t value;
            if (field_type != PP_T_I32) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i32(t, &value);
            if (status != PP_OK) {
                return status;
            }
            if (!pp_valid_compression(value)) {
                return PP_ERR_UNSUPPORTED_CODEC;
            }
            column->compression = (pp_compression)value;
            codec_seen = 1;
            break;
        }
        case 5: {
            int64_t value;
            if (field_type != PP_T_I64) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i64(t, &value);
            if (status != PP_OK) {
                return status;
            }
            if (value < 0) {
                return PP_ERR_METADATA;
            }
            column->num_values = (uint64_t)value;
            values_seen = 1;
            break;
        }
        case 6: {
            int64_t value;
            if (field_type != PP_T_I64) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i64(t, &value);
            if (status != PP_OK) {
                return status;
            }
            if (value < 0) {
                return PP_ERR_METADATA;
            }
            column->total_uncompressed_size = (uint64_t)value;
            uncompressed_seen = 1;
            break;
        }
        case 7: {
            int64_t value;
            if (field_type != PP_T_I64) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i64(t, &value);
            if (status != PP_OK) {
                return status;
            }
            if (value < 0) {
                return PP_ERR_METADATA;
            }
            column->total_compressed_size = (uint64_t)value;
            compressed_seen = 1;
            break;
        }
        case 8: {
            int64_t value;
            if (field_type != PP_T_I64) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i64(t, &value);
            if (status != PP_OK) {
                return status;
            }
            if (value < 0) {
                return PP_ERR_METADATA;
            }
            column->data_page_offset = (uint64_t)value;
            data_offset_seen = 1;
            break;
        }
        case 9: {
            int64_t value;
            if (field_type != PP_T_I64) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i64(t, &value);
            if (status != PP_OK) {
                return status;
            }
            if (value < 0) {
                return PP_ERR_METADATA;
            }
            (void)value;
            break;
        }
        case 10: {
            int64_t value;
            if (field_type != PP_T_I64) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i64(t, &value);
            if (status != PP_OK) {
                return status;
            }
            if (value < 0) {
                return PP_ERR_METADATA;
            }
            column->dictionary_page_offset = (uint64_t)value;
            break;
        }
        default:
            status = pp_t_skip(t, field_type, 0);
            if (status != PP_OK) {
                return status;
            }
            break;
        }
    }
    if (!type_seen || !encodings_seen || !path_seen || !codec_seen ||
        !values_seen || !uncompressed_seen || !compressed_seen || !data_offset_seen) {
        return PP_ERR_METADATA;
    }
    return PP_OK;
}

static pp_status pp_parse_column_chunk(pp_parse_context *context, pp_column_chunk *column)
{
    pp_thrift *t = context->thrift;
    int16_t last_field = 0;
    uint8_t metadata_seen = 0;

    memset(column, 0, sizeof(*column));
    column->type = PP_TYPE_NONE;
    column->schema_index = -1;
    column->dictionary_page_offset = UINT64_MAX;
    for (;;) {
        int16_t field_id;
        uint8_t field_type;
        uint8_t bool_value;
        uint8_t is_stop;
        pp_status status = pp_t_field(
            t, &last_field, &field_id, &field_type, &bool_value, &is_stop);
        (void)bool_value;
        if (status != PP_OK) {
            return status;
        }
        if (is_stop) {
            break;
        }
        switch (field_id) {
        case 1: {
            pp_span file_path;
            if (field_type != PP_T_BINARY) {
                return PP_ERR_METADATA;
            }
            status = pp_t_binary(t, &file_path);
            if (status != PP_OK) {
                return status;
            }
            if (file_path.size != 0) {
                return PP_ERR_UNSUPPORTED;
            }
            break;
        }
        case 2: {
            int64_t value;
            if (field_type != PP_T_I64) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i64(t, &value);
            if (status != PP_OK) {
                return status;
            }
            if (value < 0) {
                return PP_ERR_METADATA;
            }
            column->file_offset = (uint64_t)value;
            break;
        }
        case 3:
            if (field_type != PP_T_STRUCT) {
                return PP_ERR_METADATA;
            }
            status = pp_parse_column_metadata(context, column);
            if (status != PP_OK) {
                return status;
            }
            metadata_seen = 1;
            break;
        case 4:
        case 6: {
            int64_t value;
            if (field_type != PP_T_I64) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i64(t, &value);
            if (status != PP_OK) {
                return status;
            }
            if (value < 0) {
                return PP_ERR_METADATA;
            }
            break;
        }
        case 5:
        case 7: {
            int32_t value;
            if (field_type != PP_T_I32) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i32(t, &value);
            if (status != PP_OK) {
                return status;
            }
            if (value < 0) {
                return PP_ERR_METADATA;
            }
            break;
        }
        case 8:
            if (field_type != PP_T_STRUCT) {
                return PP_ERR_METADATA;
            }
            status = pp_t_skip(t, field_type, 0);
            if (status != PP_OK) {
                return status;
            }
            return PP_ERR_UNSUPPORTED;
        case 9:
            if (field_type != PP_T_BINARY) {
                return PP_ERR_METADATA;
            }
            status = pp_t_skip(t, field_type, 0);
            if (status != PP_OK) {
                return status;
            }
            return PP_ERR_UNSUPPORTED;
        default:
            status = pp_t_skip(t, field_type, 0);
            if (status != PP_OK) {
                return status;
            }
            break;
        }
    }
    if (!metadata_seen) {
        return PP_ERR_METADATA;
    }
    return PP_OK;
}

static pp_status pp_parse_row_group(pp_parse_context *context)
{
    pp_reader *reader = context->reader;
    pp_thrift *t = context->thrift;
    pp_row_group group;
    int16_t last_field = 0;
    uint8_t columns_seen = 0;
    uint8_t rows_seen = 0;
    uint8_t size_seen = 0;
    size_t group_index;

    if (reader->row_group_count >= reader->storage.row_group_capacity ||
        reader->storage.row_groups == NULL) {
        return PP_ERR_CAPACITY;
    }
    group_index = reader->row_group_count;
    memset(&group, 0, sizeof(group));
    group.first_column = (uint32_t)reader->column_count;
    for (;;) {
        int16_t field_id;
        uint8_t field_type;
        uint8_t bool_value;
        uint8_t is_stop;
        pp_status status = pp_t_field(
            t, &last_field, &field_id, &field_type, &bool_value, &is_stop);
        (void)bool_value;
        if (status != PP_OK) {
            return status;
        }
        if (is_stop) {
            break;
        }
        switch (field_id) {
        case 1: {
            uint64_t count;
            uint8_t element_type;
            size_t i;
            if (field_type != PP_T_LIST && field_type != PP_T_SET) {
                return PP_ERR_METADATA;
            }
            status = pp_t_list_header(t, &count, &element_type);
            if (status != PP_OK) {
                return status;
            }
            if (element_type != PP_T_STRUCT || count > UINT32_MAX) {
                return PP_ERR_METADATA;
            }
            if (count > reader->storage.column_capacity ||
                reader->column_count > reader->storage.column_capacity - (size_t)count ||
                reader->storage.columns == NULL) {
                return PP_ERR_CAPACITY;
            }
            group.column_count = (uint32_t)count;
            for (i = 0; i < (size_t)count; ++i) {
                status = pp_parse_column_chunk(
                    context, &reader->storage.columns[reader->column_count]);
                if (status != PP_OK) {
                    return status;
                }
                reader->column_count++;
            }
            columns_seen = 1;
            break;
        }
        case 2: {
            int64_t value;
            if (field_type != PP_T_I64) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i64(t, &value);
            if (status != PP_OK) {
                return status;
            }
            if (value < 0) {
                return PP_ERR_METADATA;
            }
            group.total_byte_size = (uint64_t)value;
            size_seen = 1;
            break;
        }
        case 3: {
            int64_t value;
            if (field_type != PP_T_I64) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i64(t, &value);
            if (status != PP_OK) {
                return status;
            }
            if (value < 0) {
                return PP_ERR_METADATA;
            }
            group.num_rows = (uint64_t)value;
            rows_seen = 1;
            break;
        }
        default:
            status = pp_t_skip(t, field_type, 0);
            if (status != PP_OK) {
                return status;
            }
            break;
        }
    }
    if (!columns_seen || !rows_seen || !size_seen) {
        return PP_ERR_METADATA;
    }
    reader->storage.row_groups[group_index] = group;
    reader->row_group_count++;
    return PP_OK;
}

static pp_status pp_parse_row_group_list(pp_parse_context *context, uint8_t field_type)
{
    pp_thrift *t = context->thrift;
    pp_reader *reader = context->reader;
    uint64_t count;
    uint8_t element_type;
    size_t i;
    pp_status status;

    if (field_type != PP_T_LIST && field_type != PP_T_SET) {
        return PP_ERR_METADATA;
    }
    status = pp_t_list_header(t, &count, &element_type);
    if (status != PP_OK) {
        return status;
    }
    if (element_type != PP_T_STRUCT || count > SIZE_MAX) {
        return PP_ERR_METADATA;
    }
    if (count > reader->storage.row_group_capacity ||
        reader->row_group_count > reader->storage.row_group_capacity - (size_t)count) {
        return PP_ERR_CAPACITY;
    }
    for (i = 0; i < (size_t)count; ++i) {
        status = pp_parse_row_group(context);
        if (status != PP_OK) {
            return status;
        }
    }
    return PP_OK;
}

static pp_status pp_parse_file_metadata(pp_parse_context *context)
{
    pp_reader *reader = context->reader;
    pp_thrift *t = context->thrift;
    int16_t last_field = 0;
    uint8_t version_seen = 0;
    uint8_t schema_seen = 0;
    uint8_t rows_seen = 0;
    uint8_t groups_seen = 0;

    for (;;) {
        int16_t field_id;
        uint8_t field_type;
        uint8_t bool_value;
        uint8_t is_stop;
        pp_status status = pp_t_field(
            t, &last_field, &field_id, &field_type, &bool_value, &is_stop);
        (void)bool_value;
        if (status != PP_OK) {
            return status;
        }
        if (is_stop) {
            break;
        }
        switch (field_id) {
        case 1: {
            int32_t version;
            if (field_type != PP_T_I32) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i32(t, &version);
            if (status != PP_OK) {
                return status;
            }
            if (version <= 0) {
                return PP_ERR_METADATA;
            }
            version_seen = 1;
            break;
        }
        case 2:
            status = pp_parse_schema_list(context, field_type);
            if (status != PP_OK) {
                return status;
            }
            schema_seen = 1;
            break;
        case 3: {
            int64_t value;
            if (field_type != PP_T_I64) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i64(t, &value);
            if (status != PP_OK) {
                return status;
            }
            if (value < 0) {
                return PP_ERR_METADATA;
            }
            reader->num_rows = (uint64_t)value;
            rows_seen = 1;
            break;
        }
        case 4:
            status = pp_parse_row_group_list(context, field_type);
            if (status != PP_OK) {
                return status;
            }
            groups_seen = 1;
            break;
        case 5:
            status = pp_t_skip(t, field_type, 0);
            if (status != PP_OK) {
                return status;
            }
            break;
        case 6:
            if (field_type != PP_T_BINARY) {
                return PP_ERR_METADATA;
            }
            status = pp_t_binary(t, &reader->created_by);
            if (status != PP_OK) {
                return status;
            }
            break;
        default:
            status = pp_t_skip(t, field_type, 0);
            if (status != PP_OK) {
                return status;
            }
            break;
        }
    }
    if (!version_seen || !schema_seen || !rows_seen || !groups_seen ||
        reader->schema_count == 0) {
        return PP_ERR_METADATA;
    }
    if (t->data != t->end) {
        return PP_ERR_METADATA;
    }
    return PP_OK;
}

static pp_status pp_memory_read_at(
    void *context,
    uint64_t offset,
    void *destination,
    size_t bytes,
    size_t *bytes_read)
{
    const uint8_t *data = (const uint8_t *)context;
    size_t available;

    if (bytes_read == NULL || (bytes != 0 && destination == NULL)) {
        return PP_ERR_ARGUMENT;
    }
    *bytes_read = 0;
    if (bytes == 0) {
        return PP_OK;
    }
    if (data == NULL || offset > SIZE_MAX) {
        return PP_ERR_RANGE;
    }
    available = SIZE_MAX - (size_t)offset;
    if (available < bytes) {
        bytes = available;
    }
    if (bytes != 0) {
        memmove(destination, data + (size_t)offset, bytes);
    }
    *bytes_read = bytes;
    return PP_OK;
}

pp_status pp_input_from_memory(pp_input *input, const void *data, size_t size)
{
    if (input == NULL || (data == NULL && size != 0)) {
        return PP_ERR_ARGUMENT;
    }
    input->size = (uint64_t)size;
    input->read_at = pp_memory_read_at;
    input->context = (void *)data;
    input->contiguous = (const uint8_t *)data;
    return PP_OK;
}

static pp_status pp_reader_read_exact(
    const pp_reader *reader,
    uint64_t offset,
    void *destination,
    size_t bytes)
{
    size_t done = 0;
    uint8_t *out = (uint8_t *)destination;

    if (reader == NULL || (bytes != 0 && destination == NULL)) {
        return PP_ERR_ARGUMENT;
    }
    if (offset > reader->input.size ||
        (uint64_t)bytes > reader->input.size - offset) {
        return PP_ERR_TRUNCATED;
    }
    while (done < bytes) {
        size_t got = 0;
        pp_status status = reader->input.read_at(
            reader->input.context, offset + (uint64_t)done,
            out + done, bytes - done, &got);
        if (status != PP_OK) {
            return status;
        }
        if (got == 0 || got > bytes - done) {
            return PP_ERR_TRUNCATED;
        }
        done += got;
    }
    return PP_OK;
}

static uint32_t pp_load_u32(const uint8_t *data)
{
    return ((uint32_t)data[0]) |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static int pp_span_equal(pp_span left, pp_span right)
{
    return left.size == right.size &&
           (left.size == 0 || memcmp(left.data, right.data, left.size) == 0);
}

static int pp_span_equal_cstr(pp_span span, const char *text)
{
    size_t length;
    if (text == NULL) {
        return 0;
    }
    length = strlen(text);
    return span.size == length &&
           (length == 0 || memcmp(span.data, text, length) == 0);
}

static int pp_query_segment(
    const char *query,
    size_t *position,
    pp_span *segment)
{
    const char *start;
    size_t length;

    if (query == NULL || position == NULL || segment == NULL) {
        return 0;
    }
    start = query + *position;
    if (*start == '\0') {
        return 0;
    }
    length = 0;
    while (start[length] != '\0' && start[length] != '.') {
        length++;
    }
    segment->data = (const uint8_t *)start;
    segment->size = length;
    *position += length;
    if (query[*position] == '.') {
        (*position)++;
    }
    return 1;
}

static size_t pp_schema_chain(
    const pp_reader *reader,
    int32_t schema_index,
    int32_t *chain,
    size_t capacity)
{
    size_t count = 0;
    int32_t current = schema_index;
    while (current >= 0 && count < capacity) {
        if ((size_t)current >= reader->schema_count) {
            return 0;
        }
        chain[count++] = current;
        current = reader->storage.schema[current].parent;
    }
    if (current >= 0) {
        return 0;
    }
    return count;
}

static int pp_query_matches_chain(
    const pp_reader *reader,
    const char *query,
    int32_t schema_index)
{
    int32_t chain[PP_MAX_SCHEMA_DEPTH];
    pp_span segments[PP_MAX_SCHEMA_DEPTH];
    size_t chain_count;
    size_t segment_count = 0;
    size_t position = 0;
    size_t i;

    if (query == NULL) {
        return 0;
    }
    chain_count = pp_schema_chain(
        reader, schema_index, chain, PP_MAX_SCHEMA_DEPTH);
    if (chain_count == 0) {
        return 0;
    }
    while (segment_count < PP_MAX_SCHEMA_DEPTH &&
           pp_query_segment(query, &position, &segments[segment_count])) {
        segment_count++;
    }
    if (query[position] != '\0') {
        return 0;
    }
    if (segment_count == chain_count) {
        for (i = 0; i < segment_count; ++i) {
            if (!pp_span_equal(
                    segments[segment_count - 1 - i],
                    reader->storage.schema[chain[i]].name)) {
                return 0;
            }
        }
        return 1;
    }
    if (segment_count == chain_count - 1) {
        for (i = 0; i < segment_count; ++i) {
            if (!pp_span_equal(
                    segments[segment_count - 1 - i],
                    reader->storage.schema[chain[i]].name)) {
                return 0;
            }
        }
        return 1;
    }
    return 0;
}

static int pp_column_path_matches(
    const pp_reader *reader,
    const pp_column_chunk *column,
    const char *query,
    int leaf_only)
{
    size_t position = 0;
    size_t i;
    pp_span segment;
    pp_span last = { NULL, 0 };
    size_t segment_count = 0;

    if (query == NULL) {
        return 0;
    }
    while (pp_query_segment(query, &position, &segment)) {
        last = segment;
        segment_count++;
    }
    if (query[position] != '\0') {
        return 0;
    }
    if (leaf_only && segment_count != 1) {
        return 0;
    }
    if (leaf_only) {
        if (column->path_count == 0) {
            return 0;
        }
        return pp_span_equal(
            last,
            reader->storage.paths[column->path_offset + column->path_count - 1].name);
    }
    position = 0;
    for (i = 0; i < column->path_count; ++i) {
        pp_span expected;
        if (!pp_query_segment(query, &position, &expected) ||
            !pp_span_equal(
                expected,
                reader->storage.paths[column->path_offset + i].name)) {
            return 0;
        }
    }
    return query[position] == '\0';
}

static int32_t pp_find_schema_for_column(
    const pp_reader *reader,
    const pp_column_chunk *column)
{
    size_t i;
    for (i = 0; i < reader->schema_count; ++i) {
        const pp_schema_node *node = &reader->storage.schema[i];
        if (node->type == PP_TYPE_NONE) {
            continue;
        }
        {
            int32_t chain[PP_MAX_SCHEMA_DEPTH];
            size_t count = pp_schema_chain(
                reader, (int32_t)i, chain, PP_MAX_SCHEMA_DEPTH);
            size_t j;
            if (count == 0) {
                continue;
            }
            if (count == column->path_count) {
                for (j = 0; j < count; ++j) {
                    if (!pp_span_equal(
                            reader->storage.paths[
                                column->path_offset + column->path_count - 1 - j].name,
                            reader->storage.schema[chain[j]].name)) {
                        break;
                    }
                }
                if (j == count) {
                    return (int32_t)i;
                }
            }
            if (count == column->path_count + 1) {
                for (j = 0; j < column->path_count; ++j) {
                    if (!pp_span_equal(
                            reader->storage.paths[
                                column->path_offset + column->path_count - 1 - j].name,
                            reader->storage.schema[chain[j]].name)) {
                        break;
                    }
                }
                if (j == column->path_count) {
                    return (int32_t)i;
                }
            }
        }
    }
    return -1;
}

static pp_status pp_finalize_metadata(pp_reader *reader)
{
    size_t i;
    size_t j;

    if (reader->schema_count == 0 ||
        reader->storage.schema[0].parent != -1) {
        return PP_ERR_METADATA;
    }
    for (i = 1; i < reader->schema_count; ++i) {
        if (reader->storage.schema[i].parent < 0) {
            return PP_ERR_METADATA;
        }
    }
    for (i = 0; i < reader->schema_count; ++i) {
        uint32_t definition = 0;
        uint32_t repetition = 0;
        int32_t current = (int32_t)i;
        while (current >= 0) {
            const pp_schema_node *node = &reader->storage.schema[current];
            if (node->repetition == PP_REPETITION_OPTIONAL ||
                node->repetition == PP_REPETITION_REPEATED) {
                definition++;
            }
            if (node->repetition == PP_REPETITION_REPEATED) {
                repetition++;
            }
            current = node->parent;
            if (definition > UINT16_MAX || repetition > UINT16_MAX) {
                return PP_ERR_UNSUPPORTED;
            }
        }
        reader->storage.schema[i].max_definition_level = (uint16_t)definition;
        reader->storage.schema[i].max_repetition_level = (uint16_t)repetition;
    }
    for (i = 0; i < reader->column_count; ++i) {
        pp_column_chunk *column = &reader->storage.columns[i];
        int32_t schema_index = pp_find_schema_for_column(reader, column);
        column->schema_index = schema_index;
        if (schema_index < 0) {
            return PP_ERR_METADATA;
        }
        if (column->type != reader->storage.schema[schema_index].type) {
            return PP_ERR_METADATA;
        }
    }
    for (i = 0; i < reader->row_group_count; ++i) {
        pp_row_group *group = &reader->storage.row_groups[i];
        if ((uint64_t)group->first_column + group->column_count >
            reader->column_count) {
            return PP_ERR_METADATA;
        }
        for (j = 0; j < group->column_count; ++j) {
            pp_column_chunk *column =
                &reader->storage.columns[group->first_column + j];
            uint64_t start = column->data_page_offset;
            if (column->dictionary_page_offset != UINT64_MAX &&
                column->dictionary_page_offset < start) {
                start = column->dictionary_page_offset;
            }
            if (start >= reader->footer_offset ||
                column->total_compressed_size >
                    reader->footer_offset - start) {
                return PP_ERR_METADATA;
            }
            if (column->dictionary_page_offset != UINT64_MAX &&
                column->dictionary_page_offset >= reader->footer_offset) {
                return PP_ERR_METADATA;
            }
        }
    }
    return PP_OK;
}

const char *pp_status_string(pp_status status)
{
    switch (status) {
    case PP_OK: return "ok";
    case PP_ERR_ARGUMENT: return "argument error";
    case PP_ERR_STATE: return "invalid state";
    case PP_ERR_IO: return "input error";
    case PP_ERR_TRUNCATED: return "truncated input";
    case PP_ERR_BAD_MAGIC: return "bad parquet magic";
    case PP_ERR_METADATA: return "invalid metadata";
    case PP_ERR_CAPACITY: return "caller buffer capacity exceeded";
    case PP_ERR_NOT_FOUND: return "not found";
    case PP_ERR_RANGE: return "numeric range error";
    case PP_ERR_END: return "end of input";
    case PP_ERR_UNSUPPORTED: return "unsupported feature";
    case PP_ERR_UNSUPPORTED_TYPE: return "unsupported physical type";
    case PP_ERR_UNSUPPORTED_ENCODING: return "unsupported encoding";
    case PP_ERR_UNSUPPORTED_CODEC: return "unsupported codec";
    case PP_ERR_CODEC: return "codec failure";
    case PP_ERR_CALLBACK: return "callback failure";
    default: return "unknown status";
    }
}

pp_status pp_reader_open(
    pp_reader *reader,
    const pp_input *input,
    const pp_reader_storage *storage,
    const pp_codec *codec)
{
    uint8_t magic[4];
    uint8_t footer_length_bytes[4];
    uint8_t tail_magic[4];
    uint32_t footer_length;
    uint64_t footer_offset;
    pp_thrift thrift;
    pp_parse_context context;
    pp_status status;

    if (reader == NULL || input == NULL || storage == NULL ||
        input->read_at == NULL) {
        return PP_ERR_ARGUMENT;
    }
    if ((storage->metadata_capacity != 0 && storage->metadata == NULL) ||
        (storage->schema_capacity != 0 && storage->schema == NULL) ||
        (storage->row_group_capacity != 0 && storage->row_groups == NULL) ||
        (storage->column_capacity != 0 && storage->columns == NULL) ||
        (storage->path_capacity != 0 && storage->paths == NULL) ||
        (storage->scratch_capacity != 0 && storage->scratch == NULL) ||
        (storage->dictionary_capacity != 0 && storage->dictionary == NULL)) {
        return PP_ERR_ARGUMENT;
    }
    if (input->size < 12) {
        return PP_ERR_TRUNCATED;
    }
    memset(reader, 0, sizeof(*reader));
    reader->input = *input;
    if (input->read_at != pp_memory_read_at) {
        reader->input.contiguous = NULL;
    }
    reader->storage = *storage;
    if (codec != NULL) {
        reader->codec = *codec;
    }

    status = pp_reader_read_exact(reader, 0, magic, sizeof(magic));
    if (status != PP_OK) {
        return status;
    }
    if (memcmp(magic, "PAR1", 4) != 0) {
        return PP_ERR_BAD_MAGIC;
    }
    status = pp_reader_read_exact(
        reader, reader->input.size - 8, footer_length_bytes, 4);
    if (status != PP_OK) {
        return status;
    }
    status = pp_reader_read_exact(
        reader, reader->input.size - 4, tail_magic, sizeof(tail_magic));
    if (status != PP_OK) {
        return status;
    }
    if (memcmp(tail_magic, "PAR1", 4) != 0) {
        return PP_ERR_BAD_MAGIC;
    }
    footer_length = pp_load_u32(footer_length_bytes);
    if (footer_length == 0) {
        return PP_ERR_METADATA;
    }
    if ((uint64_t)footer_length > (uint64_t)SIZE_MAX) {
        return PP_ERR_RANGE;
    }
    if ((uint64_t)footer_length > reader->input.size - 8) {
        return PP_ERR_TRUNCATED;
    }
    footer_offset = reader->input.size - 8 - (uint64_t)footer_length;
    if (footer_offset < 4) {
        return PP_ERR_METADATA;
    }
    reader->footer_offset = footer_offset;
    if ((size_t)footer_length > reader->storage.metadata_capacity ||
        (footer_length != 0 && reader->storage.metadata == NULL)) {
        return PP_ERR_CAPACITY;
    }
    status = pp_reader_read_exact(
        reader, footer_offset, reader->storage.metadata, (size_t)footer_length);
    if (status != PP_OK) {
        return status;
    }
    reader->metadata.data = reader->storage.metadata;
    reader->metadata.size = (size_t)footer_length;
    reader->footer_length = footer_length;

    thrift.data = reader->metadata.data;
    thrift.end = reader->metadata.data + reader->metadata.size;
    memset(&context, 0, sizeof(context));
    context.reader = reader;
    context.thrift = &thrift;
    status = pp_parse_file_metadata(&context);
    if (status != PP_OK) {
        return status;
    }
    status = pp_finalize_metadata(reader);
    if (status != PP_OK) {
        return status;
    }
    reader->opened = 1;
    return PP_OK;
}

void pp_reader_close(pp_reader *reader)
{
    if (reader != NULL) {
        memset(reader, 0, sizeof(*reader));
    }
}

size_t pp_reader_schema_count(const pp_reader *reader)
{
    return reader != NULL && reader->opened ? reader->schema_count : 0;
}

size_t pp_reader_row_group_count(const pp_reader *reader)
{
    return reader != NULL && reader->opened ? reader->row_group_count : 0;
}

uint64_t pp_reader_num_rows(const pp_reader *reader)
{
    return reader != NULL && reader->opened ? reader->num_rows : 0;
}

const pp_schema_node *pp_reader_schema_at(const pp_reader *reader, size_t index)
{
    if (reader == NULL || !reader->opened || index >= reader->schema_count) {
        return NULL;
    }
    return &reader->storage.schema[index];
}

const pp_row_group *pp_reader_row_group_at(const pp_reader *reader, size_t index)
{
    if (reader == NULL || !reader->opened || index >= reader->row_group_count) {
        return NULL;
    }
    return &reader->storage.row_groups[index];
}

const pp_column_chunk *pp_reader_column_at(const pp_reader *reader, size_t index)
{
    if (reader == NULL || !reader->opened || index >= reader->column_count) {
        return NULL;
    }
    return &reader->storage.columns[index];
}

pp_status pp_reader_find_schema(
    const pp_reader *reader,
    const char *path,
    size_t *schema_index)
{
    size_t i;
    size_t fallback = 0;
    size_t fallback_count = 0;

    if (reader == NULL || path == NULL || schema_index == NULL) {
        return PP_ERR_ARGUMENT;
    }
    if (!reader->opened) {
        return PP_ERR_STATE;
    }
    for (i = 0; i < reader->schema_count; ++i) {
        if (pp_query_matches_chain(reader, path, (int32_t)i)) {
            *schema_index = i;
            return PP_OK;
        }
    }
    for (i = 0; i < reader->schema_count; ++i) {
        const pp_schema_node *node = &reader->storage.schema[i];
        if (node->type != PP_TYPE_NONE &&
            pp_span_equal_cstr(node->name, path)) {
            fallback = i;
            fallback_count++;
        }
    }
    if (fallback_count == 1) {
        *schema_index = fallback;
        return PP_OK;
    }
    return PP_ERR_NOT_FOUND;
}

pp_status pp_reader_find_column(
    const pp_reader *reader,
    size_t row_group,
    const char *path,
    pp_column_ref *result)
{
    const pp_row_group *group;
    size_t i;
    size_t fallback = SIZE_MAX;
    size_t exact_count = 0;

    if (reader == NULL || path == NULL || result == NULL) {
        return PP_ERR_ARGUMENT;
    }
    if (!reader->opened) {
        return PP_ERR_STATE;
    }
    if (row_group >= reader->row_group_count) {
        return PP_ERR_RANGE;
    }
    group = &reader->storage.row_groups[row_group];
    for (i = 0; i < group->column_count; ++i) {
        pp_column_chunk *column =
            &reader->storage.columns[group->first_column + i];
        if (pp_column_path_matches(reader, column, path, 0)) {
            fallback = group->first_column + i;
            exact_count++;
        }
    }
    if (exact_count == 0) {
        for (i = 0; i < group->column_count; ++i) {
            pp_column_chunk *column =
                &reader->storage.columns[group->first_column + i];
            if (pp_column_path_matches(reader, column, path, 1)) {
                fallback = group->first_column + i;
                exact_count++;
            }
        }
    }
    if (exact_count != 1) {
        return exact_count == 0 ? PP_ERR_NOT_FOUND : PP_ERR_METADATA;
    }
    result->row_group = row_group;
    result->column = fallback;
    result->schema_index = reader->storage.columns[fallback].schema_index;
    result->chunk = &reader->storage.columns[fallback];
    return PP_OK;
}

static uint32_t pp_bit_mask(uint8_t bit_width)
{
    if (bit_width == 0) {
        return 0;
    }
    if (bit_width >= 32) {
        return UINT32_MAX;
    }
    return ((uint32_t)1u << bit_width) - 1u;
}

pp_status pp_plain_decoder_init(
    pp_plain_decoder *decoder,
    pp_physical_type type,
    int32_t type_length,
    const uint8_t *data,
    size_t size)
{
    if (decoder == NULL || (size != 0 && data == NULL)) {
        return PP_ERR_ARGUMENT;
    }
    if (type == PP_TYPE_INT96) {
        return PP_ERR_UNSUPPORTED_TYPE;
    }
    if (type < PP_TYPE_BOOLEAN || type > PP_TYPE_FIXED_LEN_BYTE_ARRAY) {
        return PP_ERR_UNSUPPORTED_TYPE;
    }
    if (type == PP_TYPE_FIXED_LEN_BYTE_ARRAY && type_length <= 0) {
        return PP_ERR_ARGUMENT;
    }
    memset(decoder, 0, sizeof(*decoder));
    decoder->type = type;
    decoder->type_length = type_length;
    decoder->data = data;
    decoder->size = size;
    decoder->boolean_bit = 8;
    return PP_OK;
}

static pp_status pp_plain_take(
    pp_plain_decoder *decoder,
    size_t bytes,
    const uint8_t **data)
{
    if (bytes > decoder->size - decoder->offset) {
        return PP_ERR_TRUNCATED;
    }
    *data = decoder->data + decoder->offset;
    decoder->offset += bytes;
    return PP_OK;
}

pp_status pp_plain_decoder_next(pp_plain_decoder *decoder, pp_value *value)
{
    const uint8_t *data;
    pp_status status;

    if (decoder == NULL || value == NULL) {
        return PP_ERR_ARGUMENT;
    }
    memset(value, 0, sizeof(*value));
    value->type = decoder->type;
    switch (decoder->type) {
    case PP_TYPE_BOOLEAN:
        if (decoder->boolean_bit >= 8) {
            status = pp_plain_take(decoder, 1, &data);
            if (status != PP_OK) {
                return status;
            }
            decoder->boolean_byte = *data;
            decoder->boolean_bit = 0;
        }
        value->as.boolean = (uint8_t)(
            (decoder->boolean_byte >> decoder->boolean_bit) & 1u);
        decoder->boolean_bit++;
        return PP_OK;
    case PP_TYPE_INT32:
        status = pp_plain_take(decoder, 4, &data);
        if (status != PP_OK) {
            return status;
        }
        value->as.i32 = (int32_t)pp_load_u32(data);
        return PP_OK;
    case PP_TYPE_INT64: {
        uint64_t raw;
        status = pp_plain_take(decoder, 8, &data);
        if (status != PP_OK) {
            return status;
        }
        raw = ((uint64_t)data[0]) |
              ((uint64_t)data[1] << 8) |
              ((uint64_t)data[2] << 16) |
              ((uint64_t)data[3] << 24) |
              ((uint64_t)data[4] << 32) |
              ((uint64_t)data[5] << 40) |
              ((uint64_t)data[6] << 48) |
              ((uint64_t)data[7] << 56);
        memcpy(&value->as.i64, &raw, sizeof(raw));
        return PP_OK;
    }
    case PP_TYPE_FLOAT: {
        uint32_t raw;
        status = pp_plain_take(decoder, 4, &data);
        if (status != PP_OK) {
            return status;
        }
        raw = pp_load_u32(data);
        memcpy(&value->as.f32, &raw, sizeof(raw));
        return PP_OK;
    }
    case PP_TYPE_DOUBLE: {
        uint64_t raw;
        status = pp_plain_take(decoder, 8, &data);
        if (status != PP_OK) {
            return status;
        }
        raw = ((uint64_t)data[0]) |
              ((uint64_t)data[1] << 8) |
              ((uint64_t)data[2] << 16) |
              ((uint64_t)data[3] << 24) |
              ((uint64_t)data[4] << 32) |
              ((uint64_t)data[5] << 40) |
              ((uint64_t)data[6] << 48) |
              ((uint64_t)data[7] << 56);
        memcpy(&value->as.f64, &raw, sizeof(raw));
        return PP_OK;
    }
    case PP_TYPE_BYTE_ARRAY: {
        uint32_t length;
        status = pp_plain_take(decoder, 4, &data);
        if (status != PP_OK) {
            return status;
        }
        length = pp_load_u32(data);
        status = pp_plain_take(decoder, (size_t)length, &value->as.bytes.data);
        if (status != PP_OK) {
            return status;
        }
        value->as.bytes.size = length;
        return PP_OK;
    }
    case PP_TYPE_FIXED_LEN_BYTE_ARRAY:
        status = pp_plain_take(
            decoder, (size_t)decoder->type_length, &value->as.bytes.data);
        if (status != PP_OK) {
            return status;
        }
        value->as.bytes.size = (size_t)decoder->type_length;
        return PP_OK;
    default:
        return PP_ERR_UNSUPPORTED_TYPE;
    }
}

pp_status pp_rle_decoder_init(
    pp_rle_decoder *decoder,
    const uint8_t *data,
    size_t size,
    uint8_t bit_width,
    uint8_t length_prefixed)
{
    uint32_t length;

    if (decoder == NULL || (size != 0 && data == NULL) || bit_width > 32) {
        return PP_ERR_ARGUMENT;
    }
    memset(decoder, 0, sizeof(*decoder));
    decoder->bit_width = bit_width;
    decoder->length_prefixed = length_prefixed;
    if (length_prefixed) {
        if (size < 4) {
            return PP_ERR_TRUNCATED;
        }
        length = pp_load_u32(data);
        if ((size_t)length > size - 4) {
            return PP_ERR_TRUNCATED;
        }
        decoder->data = data + 4;
        decoder->size = (size_t)length;
    } else {
        decoder->data = data;
        decoder->size = size;
    }
    return PP_OK;
}

static pp_status pp_rle_var_u32(pp_rle_decoder *decoder, uint32_t *value)
{
    uint32_t result = 0;
    unsigned shift = 0;
    unsigned i;
    for (i = 0; i < 5; ++i) {
        uint8_t byte;
        if (decoder->offset >= decoder->size) {
            return PP_ERR_TRUNCATED;
        }
        byte = decoder->data[decoder->offset++];
        if (shift == 28 && (byte & 0x7fu) > 0x0fu) {
            return PP_ERR_RANGE;
        }
        result |= ((uint32_t)(byte & 0x7fu)) << shift;
        if ((byte & 0x80u) == 0) {
            *value = result;
            return PP_OK;
        }
        shift += 7;
    }
    return PP_ERR_RANGE;
}

pp_status pp_rle_decoder_next(pp_rle_decoder *decoder, uint32_t *value)
{
    pp_status status;
    uint32_t header;
    uint32_t bytes;
    uint32_t i;

    if (decoder == NULL || value == NULL) {
        return PP_ERR_ARGUMENT;
    }
    if (decoder->run_remaining != 0) {
        *value = decoder->run_value;
        decoder->run_remaining--;
        return PP_OK;
    }
    if (decoder->bitpack_remaining != 0) {
        uint32_t mask = pp_bit_mask(decoder->bit_width);
        while (decoder->bitpack_bits_count < decoder->bit_width) {
            if (decoder->offset >= decoder->size) {
                return PP_ERR_TRUNCATED;
            }
            decoder->bitpack_bits |=
                ((uint64_t)decoder->data[decoder->offset++]) <<
                decoder->bitpack_bits_count;
            decoder->bitpack_bits_count = (uint8_t)(
                decoder->bitpack_bits_count + 8);
        }
        *value = (uint32_t)(decoder->bitpack_bits & mask);
        if (decoder->bit_width != 0) {
            decoder->bitpack_bits >>= decoder->bit_width;
            decoder->bitpack_bits_count = (uint8_t)(
                decoder->bitpack_bits_count - decoder->bit_width);
        }
        decoder->bitpack_remaining--;
        return PP_OK;
    }
    if (decoder->offset >= decoder->size) {
        return PP_ERR_END;
    }
    status = pp_rle_var_u32(decoder, &header);
    if (status != PP_OK) {
        return status;
    }
    if ((header & 1u) == 0) {
        uint32_t run_length = header >> 1;
        if (run_length == 0) {
            return PP_ERR_METADATA;
        }
        bytes = (decoder->bit_width + 7u) / 8u;
        if ((size_t)bytes > decoder->size - decoder->offset) {
            return PP_ERR_TRUNCATED;
        }
        decoder->run_value = 0;
        for (i = 0; i < bytes; ++i) {
            decoder->run_value |=
                ((uint32_t)decoder->data[decoder->offset++]) << (8u * i);
        }
        decoder->run_value &= pp_bit_mask(decoder->bit_width);
        decoder->run_remaining = run_length;
        *value = decoder->run_value;
        decoder->run_remaining--;
        return PP_OK;
    } else {
        uint32_t groups = header >> 1;
        if (groups == 0 || groups > UINT32_MAX / 8u) {
            return PP_ERR_RANGE;
        }
        decoder->bitpack_remaining = groups * 8u;
        decoder->bitpack_bits = 0;
        decoder->bitpack_bits_count = 0;
        return pp_rle_decoder_next(decoder, value);
    }
}

pp_status pp_bitpack_decoder_init(
    pp_bitpack_decoder *decoder,
    const uint8_t *data,
    size_t size,
    uint8_t bit_width,
    uint32_t value_count)
{
    if (decoder == NULL || (size != 0 && data == NULL) || bit_width > 32) {
        return PP_ERR_ARGUMENT;
    }
    memset(decoder, 0, sizeof(*decoder));
    decoder->data = data;
    decoder->size = size;
    decoder->bit_width = bit_width;
    decoder->remaining = value_count;
    return PP_OK;
}

pp_status pp_bitpack_decoder_next(pp_bitpack_decoder *decoder, uint32_t *value)
{
    uint32_t mask;
    if (decoder == NULL || value == NULL) {
        return PP_ERR_ARGUMENT;
    }
    if (decoder->remaining == 0) {
        return PP_ERR_END;
    }
    mask = pp_bit_mask(decoder->bit_width);
    while (decoder->bits_count < decoder->bit_width) {
        if (decoder->offset >= decoder->size) {
            return PP_ERR_TRUNCATED;
        }
        decoder->bits |=
            ((uint64_t)decoder->data[decoder->offset++]) <<
            decoder->bits_count;
        decoder->bits_count = (uint8_t)(decoder->bits_count + 8);
    }
    *value = (uint32_t)(decoder->bits & mask);
    if (decoder->bit_width != 0) {
        decoder->bits >>= decoder->bit_width;
        decoder->bits_count = (uint8_t)(
            decoder->bits_count - decoder->bit_width);
    }
    decoder->remaining--;
    return PP_OK;
}

static uint8_t pp_level_bit_width(uint32_t maximum)
{
    uint8_t width = 0;
    uint32_t value = maximum;
    while (value != 0) {
        width++;
        value >>= 1;
    }
    return width;
}

static pp_status pp_parse_data_page_header(
    pp_thrift *t,
    pp_page_header *header)
{
    int16_t last_field = 0;
    uint8_t values_seen = 0;
    uint8_t encoding_seen = 0;
    uint8_t definition_seen = 0;
    uint8_t repetition_seen = 0;
    for (;;) {
        int16_t field_id;
        uint8_t field_type;
        uint8_t bool_value;
        uint8_t is_stop;
        pp_status status = pp_t_field(
            t, &last_field, &field_id, &field_type, &bool_value, &is_stop);
        (void)bool_value;
        if (status != PP_OK) {
            return status;
        }
        if (is_stop) {
            break;
        }
        switch (field_id) {
        case 1:
            if (field_type != PP_T_I32) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i32(t, &header->num_values);
            if (status != PP_OK) {
                return status;
            }
            values_seen = 1;
            break;
        case 2: {
            int32_t value;
            if (field_type != PP_T_I32) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i32(t, &value);
            if (status != PP_OK) {
                return status;
            }
            if (!pp_valid_encoding(value)) {
                return PP_ERR_UNSUPPORTED_ENCODING;
            }
            header->encoding = (pp_encoding)value;
            encoding_seen = 1;
            break;
        }
        case 3: {
            int32_t value;
            if (field_type != PP_T_I32) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i32(t, &value);
            if (status != PP_OK) {
                return status;
            }
            if (!pp_valid_encoding(value)) {
                return PP_ERR_UNSUPPORTED_ENCODING;
            }
            header->definition_level_encoding = (pp_encoding)value;
            definition_seen = 1;
            break;
        }
        case 4: {
            int32_t value;
            if (field_type != PP_T_I32) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i32(t, &value);
            if (status != PP_OK) {
                return status;
            }
            if (!pp_valid_encoding(value)) {
                return PP_ERR_UNSUPPORTED_ENCODING;
            }
            header->repetition_level_encoding = (pp_encoding)value;
            repetition_seen = 1;
            break;
        }
        case 5:
            status = pp_t_skip(t, field_type, 0);
            if (status != PP_OK) {
                return status;
            }
            break;
        default:
            status = pp_t_skip(t, field_type, 0);
            if (status != PP_OK) {
                return status;
            }
            break;
        }
    }
    if (!values_seen || !encoding_seen || !definition_seen || !repetition_seen) {
        return PP_ERR_METADATA;
    }
    if (header->num_values < 0) {
        return PP_ERR_METADATA;
    }
    return PP_OK;
}

static pp_status pp_parse_data_page_v2_header(
    pp_thrift *t,
    pp_page_header *header)
{
    int16_t last_field = 0;
    uint8_t values_seen = 0;
    uint8_t nulls_seen = 0;
    uint8_t rows_seen = 0;
    uint8_t encoding_seen = 0;
    uint8_t definition_length_seen = 0;
    uint8_t repetition_length_seen = 0;
    uint8_t compressed_seen = 0;
    for (;;) {
        int16_t field_id;
        uint8_t field_type;
        uint8_t bool_value;
        uint8_t is_stop;
        pp_status status = pp_t_field(
            t, &last_field, &field_id, &field_type, &bool_value, &is_stop);
        if (status != PP_OK) {
            return status;
        }
        if (is_stop) {
            break;
        }
        switch (field_id) {
        case 1:
            if (field_type != PP_T_I32) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i32(t, &header->num_values);
            if (status != PP_OK) {
                return status;
            }
            values_seen = 1;
            break;
        case 2:
            if (field_type != PP_T_I32) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i32(t, &header->num_nulls);
            if (status != PP_OK) {
                return status;
            }
            nulls_seen = 1;
            break;
        case 3:
            if (field_type != PP_T_I32) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i32(t, &header->num_rows);
            if (status != PP_OK) {
                return status;
            }
            rows_seen = 1;
            break;
        case 4: {
            int32_t value;
            if (field_type != PP_T_I32) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i32(t, &value);
            if (status != PP_OK) {
                return status;
            }
            if (!pp_valid_encoding(value)) {
                return PP_ERR_UNSUPPORTED_ENCODING;
            }
            header->encoding = (pp_encoding)value;
            encoding_seen = 1;
            break;
        }
        case 5: {
            int32_t value;
            if (field_type != PP_T_I32) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i32(t, &value);
            if (status != PP_OK) {
                return status;
            }
            if (value < 0) {
                return PP_ERR_METADATA;
            }
            header->definition_levels_byte_length = (uint32_t)value;
            definition_length_seen = 1;
            break;
        }
        case 6: {
            int32_t value;
            if (field_type != PP_T_I32) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i32(t, &value);
            if (status != PP_OK) {
                return status;
            }
            if (value < 0) {
                return PP_ERR_METADATA;
            }
            header->repetition_levels_byte_length = (uint32_t)value;
            repetition_length_seen = 1;
            break;
        }
        case 7:
            if (field_type != PP_T_BOOL_TRUE && field_type != PP_T_BOOL_FALSE) {
                return PP_ERR_METADATA;
            }
            header->is_compressed = bool_value;
            compressed_seen = 1;
            break;
        default:
            status = pp_t_skip(t, field_type, 0);
            if (status != PP_OK) {
                return status;
            }
            break;
        }
    }
    if (!values_seen || !nulls_seen || !rows_seen || !encoding_seen ||
        !definition_length_seen || !repetition_length_seen || !compressed_seen) {
        return PP_ERR_METADATA;
    }
    if (header->num_values < 0 || header->num_nulls < 0 || header->num_rows < 0) {
        return PP_ERR_METADATA;
    }
    return PP_OK;
}

static pp_status pp_parse_dictionary_page_header(
    pp_thrift *t,
    pp_page_header *header)
{
    int16_t last_field = 0;
    uint8_t values_seen = 0;
    uint8_t encoding_seen = 0;
    for (;;) {
        int16_t field_id;
        uint8_t field_type;
        uint8_t bool_value;
        uint8_t is_stop;
        pp_status status = pp_t_field(
            t, &last_field, &field_id, &field_type, &bool_value, &is_stop);
        (void)bool_value;
        if (status != PP_OK) {
            return status;
        }
        if (is_stop) {
            break;
        }
        switch (field_id) {
        case 1:
        {
            int32_t value;
            if (field_type != PP_T_I32) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i32(t, &value);
            if (status != PP_OK) {
                return status;
            }
            if (value < 0) {
                return PP_ERR_METADATA;
            }
            header->dictionary_num_values = (uint32_t)value;
            values_seen = 1;
            break;
        }
        case 2: {
            int32_t value;
            if (field_type != PP_T_I32) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i32(t, &value);
            if (status != PP_OK) {
                return status;
            }
            if (!pp_valid_encoding(value)) {
                return PP_ERR_UNSUPPORTED_ENCODING;
            }
            header->encoding = (pp_encoding)value;
            encoding_seen = 1;
            break;
        }
        default:
            status = pp_t_skip(t, field_type, 0);
            if (status != PP_OK) {
                return status;
            }
            break;
        }
    }
    if (!values_seen || !encoding_seen) {
        return PP_ERR_METADATA;
    }
    return PP_OK;
}

static pp_status pp_parse_page_header_bytes(
    const uint8_t *data,
    size_t size,
    pp_page_header *header)
{
    pp_thrift t;
    int16_t last_field = 0;
    uint8_t type_seen = 0;
    uint8_t uncompressed_seen = 0;
    uint8_t compressed_seen = 0;
    uint8_t data_header_seen = 0;
    uint8_t dictionary_header_seen = 0;
    uint8_t data_v2_header_seen = 0;
    pp_status status;

    if (data == NULL || header == NULL) {
        return PP_ERR_ARGUMENT;
    }
    memset(header, 0, sizeof(*header));
    header->type = PP_PAGE_DATA;
    header->encoding = PP_ENCODING_PLAIN;
    header->definition_level_encoding = PP_ENCODING_RLE;
    header->repetition_level_encoding = PP_ENCODING_RLE;
    t.data = data;
    t.end = data + size;
    for (;;) {
        int16_t field_id;
        uint8_t field_type;
        uint8_t bool_value;
        uint8_t is_stop;
        status = pp_t_field(
            &t, &last_field, &field_id, &field_type, &bool_value, &is_stop);
        if (status != PP_OK) {
            return status;
        }
        if (is_stop) {
            break;
        }
        switch (field_id) {
        case 1: {
            int32_t value;
            if (field_type != PP_T_I32) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i32(&t, &value);
            if (status != PP_OK) {
                return status;
            }
            if (!pp_valid_page_type(value)) {
                return PP_ERR_UNSUPPORTED;
            }
            header->type = (pp_page_type)value;
            type_seen = 1;
            break;
        }
        case 2:
            if (field_type != PP_T_I32) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i32(&t, &header->uncompressed_page_size);
            if (status != PP_OK) {
                return status;
            }
            uncompressed_seen = 1;
            break;
        case 3:
            if (field_type != PP_T_I32) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i32(&t, &header->compressed_page_size);
            if (status != PP_OK) {
                return status;
            }
            compressed_seen = 1;
            break;
        case 4:
            if (field_type != PP_T_I32) {
                return PP_ERR_METADATA;
            }
            status = pp_t_i32(&t, &header->crc);
            if (status != PP_OK) {
                return status;
            }
            break;
        case 5:
            if (field_type != PP_T_STRUCT) {
                return PP_ERR_METADATA;
            }
            status = pp_parse_data_page_header(&t, header);
            if (status != PP_OK) {
                return status;
            }
            data_header_seen = 1;
            break;
        case 6:
            status = pp_t_skip(&t, field_type, 0);
            if (status != PP_OK) {
                return status;
            }
            break;
        case 7:
            if (field_type != PP_T_STRUCT) {
                return PP_ERR_METADATA;
            }
            status = pp_parse_dictionary_page_header(&t, header);
            if (status != PP_OK) {
                return status;
            }
            dictionary_header_seen = 1;
            break;
        case 8:
            if (field_type != PP_T_STRUCT) {
                return PP_ERR_METADATA;
            }
            status = pp_parse_data_page_v2_header(&t, header);
            if (status != PP_OK) {
                return status;
            }
            data_v2_header_seen = 1;
            break;
        default:
            status = pp_t_skip(&t, field_type, 0);
            if (status != PP_OK) {
                return status;
            }
            break;
        }
    }
    if (!type_seen || !uncompressed_seen || !compressed_seen ||
        t.data != t.end) {
        return PP_ERR_METADATA;
    }
    if (header->uncompressed_page_size < 0 ||
        header->compressed_page_size < 0) {
        return PP_ERR_METADATA;
    }
    if ((header->type == PP_PAGE_DATA && !data_header_seen) ||
        (header->type == PP_PAGE_DICTIONARY && !dictionary_header_seen) ||
        (header->type == PP_PAGE_DATA_V2 && !data_v2_header_seen)) {
        return PP_ERR_METADATA;
    }
    return PP_OK;
}

static pp_status pp_read_page_header(
    pp_column_cursor *cursor,
    uint64_t offset,
    size_t *header_size)
{
    size_t size = 0;
    while (size < PP_PAGE_HEADER_MAX) {
        pp_status status;
        status = pp_reader_read_exact(
            cursor->reader, offset + (uint64_t)size,
            cursor->header_storage + size, 1);
        if (status != PP_OK) {
            return status;
        }
        size++;
        status = pp_parse_page_header_bytes(
            cursor->header_storage, size, &cursor->header);
        if (status == PP_OK) {
            *header_size = size;
            return PP_OK;
        }
        if (status != PP_ERR_TRUNCATED) {
            return status;
        }
    }
    return PP_ERR_CAPACITY;
}

static pp_status pp_read_payload(
    pp_column_cursor *cursor,
    uint64_t offset,
    const pp_page_header *header,
    pp_span *payload)
{
    pp_reader *reader = cursor->reader;
    size_t compressed_size = (size_t)header->compressed_page_size;
    size_t uncompressed_size = (size_t)header->uncompressed_page_size;
    uint8_t *scratch = reader->storage.scratch;
    pp_status status;
    size_t decoded_size;
    size_t prefix_size;

    if (header->type == PP_PAGE_DATA_V2 && !header->is_compressed) {
        if (compressed_size != uncompressed_size) {
            return PP_ERR_METADATA;
        }
        if (reader->input.contiguous != NULL) {
            payload->data = reader->input.contiguous + (size_t)offset;
            payload->size = compressed_size;
            return PP_OK;
        }
        if (compressed_size > reader->storage.scratch_capacity ||
            (compressed_size != 0 && scratch == NULL)) {
            return PP_ERR_CAPACITY;
        }
        status = pp_reader_read_exact(
            reader, offset, scratch, compressed_size);
        if (status != PP_OK) {
            return status;
        }
        payload->data = scratch;
        payload->size = compressed_size;
        return PP_OK;
    }
    if (header->type == PP_PAGE_DATA_V2 && header->is_compressed) {
        prefix_size = (size_t)header->repetition_levels_byte_length +
            (size_t)header->definition_levels_byte_length;
        if (prefix_size > compressed_size || prefix_size > uncompressed_size) {
            return PP_ERR_METADATA;
        }
        if (compressed_size > reader->storage.scratch_capacity ||
            (compressed_size != 0 && scratch == NULL)) {
            return PP_ERR_CAPACITY;
        }
        status = pp_reader_read_exact(
            reader, offset, scratch, compressed_size);
        if (status != PP_OK) {
            return status;
        }
        if (reader->storage.columns[cursor->reference.column].compression ==
            PP_COMPRESSION_UNCOMPRESSED) {
            return PP_ERR_METADATA;
        }
        if (reader->codec.decode == NULL) {
            return PP_ERR_UNSUPPORTED_CODEC;
        }
        if (uncompressed_size > SIZE_MAX - compressed_size ||
            compressed_size + uncompressed_size > reader->storage.scratch_capacity) {
            return PP_ERR_CAPACITY;
        }
        memcpy(
            scratch + compressed_size,
            scratch,
            prefix_size);
        decoded_size = 0;
        status = reader->codec.decode(
            reader->codec.context,
            reader->storage.columns[cursor->reference.column].compression,
            scratch + prefix_size,
            compressed_size - prefix_size,
            scratch + compressed_size + prefix_size,
            uncompressed_size - prefix_size,
            &decoded_size);
        if (status != PP_OK) {
            return status == PP_ERR_UNSUPPORTED_CODEC ? status : PP_ERR_CODEC;
        }
        if (decoded_size != uncompressed_size - prefix_size) {
            return PP_ERR_CODEC;
        }
        payload->data = scratch + compressed_size;
        payload->size = uncompressed_size;
        return PP_OK;
    }
    if (reader->storage.columns[cursor->reference.column].compression ==
        PP_COMPRESSION_UNCOMPRESSED) {
        if (compressed_size != uncompressed_size) {
            return PP_ERR_METADATA;
        }
        if (reader->input.contiguous != NULL) {
            payload->data = reader->input.contiguous + (size_t)offset;
            payload->size = compressed_size;
            return PP_OK;
        }
        if (compressed_size > reader->storage.scratch_capacity ||
            (compressed_size != 0 && scratch == NULL)) {
            return PP_ERR_CAPACITY;
        }
        status = pp_reader_read_exact(
            reader, offset, scratch, compressed_size);
        if (status != PP_OK) {
            return status;
        }
        payload->data = scratch;
        payload->size = compressed_size;
        return PP_OK;
    }
    if (compressed_size > reader->storage.scratch_capacity ||
        (compressed_size != 0 && scratch == NULL)) {
        return PP_ERR_CAPACITY;
    }
    status = pp_reader_read_exact(
        reader, offset, scratch, compressed_size);
    if (status != PP_OK) {
        return status;
    }
    if (reader->codec.decode == NULL) {
        return PP_ERR_UNSUPPORTED_CODEC;
    }
    if (uncompressed_size > SIZE_MAX - compressed_size ||
        compressed_size + uncompressed_size > reader->storage.scratch_capacity ||
        (uncompressed_size != 0 && scratch == NULL)) {
        return PP_ERR_CAPACITY;
    }
    decoded_size = 0;
    status = reader->codec.decode(
        reader->codec.context,
        reader->storage.columns[cursor->reference.column].compression,
        scratch,
        compressed_size,
        scratch + compressed_size,
        uncompressed_size,
        &decoded_size);
    if (status != PP_OK) {
        return status == PP_ERR_UNSUPPORTED_CODEC ? status : PP_ERR_CODEC;
    }
    if (decoded_size != uncompressed_size) {
        return PP_ERR_CODEC;
    }
    payload->data = scratch + compressed_size;
    payload->size = decoded_size;
    return PP_OK;
}

static pp_status pp_read_dictionary(
    pp_column_cursor *cursor,
    uint64_t offset,
    const pp_page_header *header)
{
    pp_reader *reader = cursor->reader;
    pp_column_chunk *column =
        &reader->storage.columns[cursor->reference.column];
    size_t compressed_size = (size_t)header->compressed_page_size;
    size_t uncompressed_size = (size_t)header->uncompressed_page_size;
    uint8_t *scratch = reader->storage.scratch;
    uint8_t *dictionary = reader->storage.dictionary;
    pp_status status;
    size_t decoded_size;

    if (header->encoding != PP_ENCODING_PLAIN) {
        return PP_ERR_UNSUPPORTED_ENCODING;
    }
    if (uncompressed_size > reader->storage.dictionary_capacity ||
        (uncompressed_size != 0 && dictionary == NULL)) {
        return PP_ERR_CAPACITY;
    }
    if (column->compression == PP_COMPRESSION_UNCOMPRESSED) {
        if (compressed_size != uncompressed_size) {
            return PP_ERR_METADATA;
        }
        if (uncompressed_size != 0) {
            if (reader->input.contiguous != NULL) {
                memcpy(
                    dictionary,
                    reader->input.contiguous + (size_t)offset,
                    uncompressed_size);
            } else {
                if (compressed_size > reader->storage.scratch_capacity ||
                    scratch == NULL) {
                    return PP_ERR_CAPACITY;
                }
                status = pp_reader_read_exact(
                    reader, offset, scratch, compressed_size);
                if (status != PP_OK) {
                    return status;
                }
                memcpy(dictionary, scratch, uncompressed_size);
            }
        }
    } else {
        if (reader->codec.decode == NULL) {
            return PP_ERR_UNSUPPORTED_CODEC;
        }
        if (compressed_size > reader->storage.scratch_capacity ||
            (compressed_size != 0 && scratch == NULL)) {
            return PP_ERR_CAPACITY;
        }
        status = pp_reader_read_exact(reader, offset, scratch, compressed_size);
        if (status != PP_OK) {
            return status;
        }
        decoded_size = 0;
        status = reader->codec.decode(
            reader->codec.context,
            column->compression,
            scratch,
            compressed_size,
            dictionary,
            uncompressed_size,
            &decoded_size);
        if (status != PP_OK) {
            return status == PP_ERR_UNSUPPORTED_CODEC ? status : PP_ERR_CODEC;
        }
        if (decoded_size != uncompressed_size) {
            return PP_ERR_CODEC;
        }
    }
    cursor->dictionary_count = (uint32_t)header->dictionary_num_values;
    cursor->dictionary_size = uncompressed_size;
    cursor->dictionary_loaded = 1;
    return PP_OK;
}

static pp_status pp_make_page(
    pp_column_cursor *cursor,
    const pp_page_header *header,
    pp_span payload,
    pp_page *page)
{
    pp_reader *reader = cursor->reader;
    pp_column_chunk *column =
        &reader->storage.columns[cursor->reference.column];
    const pp_schema_node *schema;
    size_t offset;
    uint32_t repetition_size;
    uint32_t definition_size;

    if (column->schema_index < 0 ||
        (size_t)column->schema_index >= reader->schema_count) {
        return PP_ERR_METADATA;
    }
    schema = &reader->storage.schema[column->schema_index];
    if (header->type != PP_PAGE_DATA && header->type != PP_PAGE_DATA_V2) {
        return PP_ERR_ARGUMENT;
    }
    memset(page, 0, sizeof(*page));
    page->header = *header;
    page->type = column->type;
    page->type_length = schema->type_length;
    page->max_definition_level = schema->max_definition_level;
    page->max_repetition_level = schema->max_repetition_level;
    if (header->type == PP_PAGE_DATA) {
        if (payload.size < 8) {
            return PP_ERR_TRUNCATED;
        }
        repetition_size = pp_load_u32(payload.data);
        definition_size = pp_load_u32(payload.data + 4);
        offset = 8;
        if ((size_t)repetition_size > payload.size - offset) {
            return PP_ERR_TRUNCATED;
        }
        page->repetition_levels.data = payload.data + offset;
        page->repetition_levels.size = repetition_size;
        offset += repetition_size;
        if ((size_t)definition_size > payload.size - offset) {
            return PP_ERR_TRUNCATED;
        }
        page->definition_levels.data = payload.data + offset;
        page->definition_levels.size = definition_size;
        offset += definition_size;
        page->values.data = payload.data + offset;
        page->values.size = payload.size - offset;
    } else {
        repetition_size = header->repetition_levels_byte_length;
        definition_size = header->definition_levels_byte_length;
        offset = 0;
        if ((size_t)repetition_size > payload.size) {
            return PP_ERR_TRUNCATED;
        }
        page->repetition_levels.data = payload.data;
        page->repetition_levels.size = repetition_size;
        offset += repetition_size;
        if ((size_t)definition_size > payload.size - offset) {
            return PP_ERR_TRUNCATED;
        }
        page->definition_levels.data = payload.data + offset;
        page->definition_levels.size = definition_size;
        offset += definition_size;
        page->values.data = payload.data + offset;
        page->values.size = payload.size - offset;
    }
    if (cursor->dictionary_loaded) {
        page->dictionary.data = reader->storage.dictionary;
        page->dictionary.size = cursor->dictionary_size;
        page->dictionary_count = cursor->dictionary_count;
    }
    return PP_OK;
}

pp_status pp_column_cursor_init(
    pp_column_cursor *cursor,
    pp_reader *reader,
    size_t row_group,
    const char *path)
{
    pp_status status;
    pp_column_chunk *column;
    uint64_t start;

    if (cursor == NULL || reader == NULL || path == NULL) {
        return PP_ERR_ARGUMENT;
    }
    if (!reader->opened) {
        return PP_ERR_STATE;
    }
    memset(cursor, 0, sizeof(*cursor));
    status = pp_reader_find_column(
        reader, row_group, path, &cursor->reference);
    if (status != PP_OK) {
        return status;
    }
    column = &reader->storage.columns[cursor->reference.column];
    start = column->data_page_offset;
    if (column->dictionary_page_offset != UINT64_MAX &&
        column->dictionary_page_offset < start) {
        start = column->dictionary_page_offset;
    }
    if (start >= reader->footer_offset ||
        column->total_compressed_size > reader->footer_offset - start) {
        return PP_ERR_METADATA;
    }
    if (column->total_compressed_size > UINT64_MAX - start) {
        return PP_ERR_RANGE;
    }
    cursor->reader = reader;
    cursor->offset = start;
    cursor->end_offset = start + column->total_compressed_size;
    return PP_OK;
}

pp_status pp_column_cursor_next_page(pp_column_cursor *cursor, pp_page *page)
{
    pp_reader *reader;
    size_t header_size;
    uint64_t payload_offset;
    pp_span payload;
    pp_status status;

    if (cursor == NULL || page == NULL) {
        return PP_ERR_ARGUMENT;
    }
    reader = cursor->reader;
    if (reader == NULL || !reader->opened) {
        return PP_ERR_STATE;
    }
    for (;;) {
        if (cursor->offset >= cursor->end_offset) {
            return PP_ERR_END;
        }
        status = pp_read_page_header(cursor, cursor->offset, &header_size);
        if (status != PP_OK) {
            return status;
        }
        if ((uint64_t)header_size > cursor->end_offset - cursor->offset) {
            return PP_ERR_TRUNCATED;
        }
        payload_offset = cursor->offset + (uint64_t)header_size;
        if ((uint64_t)cursor->header.compressed_page_size >
            cursor->end_offset - payload_offset) {
            return PP_ERR_TRUNCATED;
        }
        cursor->offset = payload_offset +
            (uint64_t)cursor->header.compressed_page_size;
        if (cursor->header.type == PP_PAGE_DICTIONARY) {
            status = pp_read_dictionary(
                cursor, payload_offset, &cursor->header);
            if (status != PP_OK) {
                return status;
            }
            continue;
        }
        if (cursor->header.type == PP_PAGE_INDEX) {
            continue;
        }
        status = pp_read_payload(
            cursor, payload_offset, &cursor->header, &payload);
        if (status != PP_OK) {
            return status;
        }
        status = pp_make_page(cursor, &cursor->header, payload, page);
        if (status != PP_OK) {
            return status;
        }
        cursor->page_number++;
        return PP_OK;
    }
}

static pp_status pp_page_level_next(
    pp_rle_decoder *rle,
    pp_bitpack_decoder *bitpack,
    uint8_t use_bitpack,
    uint32_t *value)
{
    pp_status status;
    if (use_bitpack) {
        status = pp_bitpack_decoder_next(bitpack, value);
    } else {
        status = pp_rle_decoder_next(rle, value);
    }
    return status == PP_ERR_END ? PP_ERR_TRUNCATED : status;
}

static pp_status pp_dictionary_value(
    const pp_page *page,
    uint32_t index,
    pp_value *value)
{
    pp_plain_decoder decoder;
    uint32_t i;
    pp_status status;

    if (page->dictionary.data == NULL ||
        index >= page->dictionary_count) {
        return PP_ERR_RANGE;
    }
    status = pp_plain_decoder_init(
        &decoder,
        page->type,
        page->type_length,
        page->dictionary.data,
        page->dictionary.size);
    if (status != PP_OK) {
        return status;
    }
    for (i = 0; i <= index; ++i) {
        status = pp_plain_decoder_next(&decoder, value);
        if (status != PP_OK) {
            return status;
        }
    }
    return PP_OK;
}

pp_status pp_page_decoder_init(pp_page_decoder *decoder, const pp_page *page)
{
    pp_status status;
    uint8_t bit_width;

    if (decoder == NULL || page == NULL) {
        return PP_ERR_ARGUMENT;
    }
    if (page->header.type != PP_PAGE_DATA &&
        page->header.type != PP_PAGE_DATA_V2) {
        return PP_ERR_ARGUMENT;
    }
    if (page->header.num_values < 0) {
        return PP_ERR_METADATA;
    }
    memset(decoder, 0, sizeof(*decoder));
    decoder->page = page;
    decoder->repetition_enabled = page->max_repetition_level != 0;
    decoder->definition_enabled = page->max_definition_level != 0;
    bit_width = pp_level_bit_width(page->max_repetition_level);
    if (decoder->repetition_enabled) {
        if (page->header.repetition_level_encoding == PP_ENCODING_RLE) {
            status = pp_rle_decoder_init(
                &decoder->repetition,
                page->repetition_levels.data,
                page->repetition_levels.size,
                bit_width,
                0);
        } else if (page->header.repetition_level_encoding ==
                   PP_ENCODING_BIT_PACKED) {
            status = pp_bitpack_decoder_init(
                &decoder->repetition_bitpack,
                page->repetition_levels.data,
                page->repetition_levels.size,
                bit_width,
                (uint32_t)page->header.num_values);
        } else {
            return PP_ERR_UNSUPPORTED_ENCODING;
        }
        if (status != PP_OK) {
            return status;
        }
    }
    bit_width = pp_level_bit_width(page->max_definition_level);
    if (decoder->definition_enabled) {
        if (page->header.definition_level_encoding == PP_ENCODING_RLE) {
            status = pp_rle_decoder_init(
                &decoder->definition,
                page->definition_levels.data,
                page->definition_levels.size,
                bit_width,
                0);
        } else if (page->header.definition_level_encoding ==
                   PP_ENCODING_BIT_PACKED) {
            status = pp_bitpack_decoder_init(
                &decoder->definition_bitpack,
                page->definition_levels.data,
                page->definition_levels.size,
                bit_width,
                (uint32_t)page->header.num_values);
        } else {
            return PP_ERR_UNSUPPORTED_ENCODING;
        }
        if (status != PP_OK) {
            return status;
        }
    }
    switch (page->header.encoding) {
    case PP_ENCODING_PLAIN:
        return pp_plain_decoder_init(
            &decoder->plain,
            page->type,
            page->type_length,
            page->values.data,
            page->values.size);
    case PP_ENCODING_PLAIN_DICTIONARY:
    case PP_ENCODING_RLE_DICTIONARY:
    {
        uint8_t encoded_bit_width;
        if (page->dictionary.data == NULL) {
            return PP_ERR_STATE;
        }
        if (page->values.size < 1) {
            return PP_ERR_TRUNCATED;
        }
        encoded_bit_width = page->values.data[0];
        bit_width = pp_level_bit_width(
            page->dictionary_count == 0 ? 0 : page->dictionary_count - 1);
        if (encoded_bit_width != bit_width) {
            return PP_ERR_METADATA;
        }
        status = pp_rle_decoder_init(
            &decoder->indices,
            page->values.data + 1,
            page->values.size - 1,
            bit_width,
            1);
        if (status != PP_OK) {
            return status;
        }
        decoder->dictionary_encoded = 1;
        return PP_OK;
    }
    default:
        return PP_ERR_UNSUPPORTED_ENCODING;
    }
}

pp_status pp_page_decoder_next(
    pp_page_decoder *decoder,
    int32_t *repetition_level,
    int32_t *definition_level,
    pp_value *value)
{
    const pp_page *page;
    uint32_t level;
    pp_status status;

    if (decoder == NULL || repetition_level == NULL ||
        definition_level == NULL || value == NULL) {
        return PP_ERR_ARGUMENT;
    }
    page = decoder->page;
    if (page == NULL) {
        return PP_ERR_STATE;
    }
    if (decoder->index >= (uint32_t)page->header.num_values) {
        return PP_ERR_END;
    }
    *repetition_level = 0;
    *definition_level = 0;
    memset(value, 0, sizeof(*value));
    value->type = page->type;
    if (decoder->repetition_enabled) {
        status = pp_page_level_next(
            &decoder->repetition,
            &decoder->repetition_bitpack,
            page->header.repetition_level_encoding == PP_ENCODING_BIT_PACKED,
            &level);
        if (status != PP_OK) {
            return status;
        }
        if (level > page->max_repetition_level) {
            return PP_ERR_METADATA;
        }
        *repetition_level = (int32_t)level;
    }
    if (decoder->definition_enabled) {
        status = pp_page_level_next(
            &decoder->definition,
            &decoder->definition_bitpack,
            page->header.definition_level_encoding == PP_ENCODING_BIT_PACKED,
            &level);
        if (status != PP_OK) {
            return status;
        }
        if (level > page->max_definition_level) {
            return PP_ERR_METADATA;
        }
        *definition_level = (int32_t)level;
    }
    decoder->index++;
    if ((uint32_t)*definition_level < page->max_definition_level) {
        value->is_null = 1;
        return PP_OK;
    }
    if (decoder->dictionary_encoded) {
        uint32_t dictionary_index;
        status = pp_rle_decoder_next(&decoder->indices, &dictionary_index);
        if (status == PP_ERR_END) {
            status = PP_ERR_TRUNCATED;
        }
        if (status != PP_OK) {
            return status;
        }
        status = pp_dictionary_value(page, dictionary_index, value);
    } else {
        status = pp_plain_decoder_next(&decoder->plain, value);
    }
    if (status != PP_OK) {
        return status;
    }
    value->is_null = 0;
    decoder->value_index++;
    return PP_OK;
}

pp_status pp_column_cursor_read(
    pp_column_cursor *cursor,
    pp_value_callback callback,
    void *context)
{
    pp_page page;
    pp_page_decoder decoder;
    pp_status status;

    if (cursor == NULL || callback == NULL) {
        return PP_ERR_ARGUMENT;
    }
    for (;;) {
        int32_t repetition_level;
        int32_t definition_level;
        pp_value value;
        status = pp_column_cursor_next_page(cursor, &page);
        if (status == PP_ERR_END) {
            return PP_OK;
        }
        if (status != PP_OK) {
            return status;
        }
        status = pp_page_decoder_init(&decoder, &page);
        if (status != PP_OK) {
            return status;
        }
        for (;;) {
            status = pp_page_decoder_next(
                &decoder,
                &repetition_level,
                &definition_level,
                &value);
            if (status == PP_ERR_END) {
                break;
            }
            if (status != PP_OK) {
                return status;
            }
            if (repetition_level == 0) {
                if (cursor->row_open) {
                    cursor->rows_seen++;
                }
                cursor->current_row = cursor->rows_seen;
                cursor->row_open = 1;
            } else if (!cursor->row_open) {
                cursor->current_row = cursor->rows_seen;
                cursor->row_open = 1;
            }
            status = callback(
                context,
                cursor->current_row,
                repetition_level,
                definition_level,
                &value);
            if (status != PP_OK) {
                return status == PP_ERR_CALLBACK ? status : PP_ERR_CALLBACK;
            }
        }
    }
}
