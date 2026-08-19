#include "picoparquet_picocompress.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *pp_pcx_codec_name(pp_compression compression)
{
    switch (compression) {
    case PP_COMPRESSION_SNAPPY: return "snappy";
    case PP_COMPRESSION_GZIP: return "gzip";
    case PP_COMPRESSION_LZO: return "lzo";
    case PP_COMPRESSION_BROTLI: return "brotli";
    case PP_COMPRESSION_LZ4:
    case PP_COMPRESSION_LZ4_RAW: return "lz4";
    case PP_COMPRESSION_ZSTD: return "zstd";
    case PP_COMPRESSION_UNCOMPRESSED:
    default: return NULL;
    }
}

static pp_status pp_pcx_map_result(pcx_result result)
{
    switch (result) {
    case PCX_OK: return PP_OK;
    case PCX_ERR_OUTPUT_TOO_SMALL:
    case PCX_ERR_MEMORY:
        return PP_ERR_CAPACITY;
    case PCX_ERR_UNSUPPORTED:
    case PCX_ERR_NO_CODEC:
    case PCX_ERR_LOAD:
        return PP_ERR_UNSUPPORTED_CODEC;
    case PCX_ERR_INPUT:
    case PCX_ERR_CORRUPT:
    case PCX_ERR_WRITE:
    case PCX_ERR_ABI:
    case PCX_ERR_DUPLICATE:
    default:
        return PP_ERR_CODEC;
    }
}

static void *pp_pcx_state_alloc(size_t size, size_t align)
{
    uintptr_t raw_addr;
    uintptr_t aligned_addr;
    void *raw;
    if (!size || !align || (align & (align - 1u)) != 0) return NULL;
    if (size > SIZE_MAX - align - sizeof(void *)) return NULL;
    raw = malloc(size + align - 1u + sizeof(void *));
    if (!raw) return NULL;
    raw_addr = (uintptr_t)raw + sizeof(void *);
    aligned_addr = (raw_addr + (uintptr_t)align - 1u) & ~((uintptr_t)align - 1u);
    ((void **)aligned_addr)[-1] = raw;
    return (void *)aligned_addr;
}

static void pp_pcx_state_free(void *state)
{
    if (state) free(((void **)state)[-1]);
}

typedef struct pp_pcx_output {
    uint8_t *data;
    size_t capacity;
    size_t size;
    int overflow;
} pp_pcx_output;

static int pp_pcx_write(void *opaque, const uint8_t *data, size_t size)
{
    pp_pcx_output *output = (pp_pcx_output *)opaque;
    if (!output || size > output->capacity - output->size) {
        if (output) output->overflow = 1;
        return 1;
    }
    if (size) memcpy(output->data + output->size, data, size);
    output->size += size;
    return 0;
}

static pcx_result pp_pcx_stream_decode(const pcx_codec_v1 *codec,
                                       const uint8_t *compressed,
                                       size_t compressed_size,
                                       uint8_t *uncompressed,
                                       size_t uncompressed_capacity,
                                       size_t *uncompressed_size)
{
    void *state;
    pcx_options options = { NULL, 0 };
    pp_pcx_output output;
    pcx_result result;
    pcx_result finish_result;

    if (!codec || !(codec->capabilities & PCX_CODEC_CAP_DECOMPRESS) ||
        !codec->decoder_state_size || !codec->decoder_state_align ||
        !codec->decoder_init || !codec->decoder_sink || !codec->decoder_finish)
        return PCX_ERR_UNSUPPORTED;

    state = pp_pcx_state_alloc(codec->decoder_state_size,
                               codec->decoder_state_align);
    if (!state) return PCX_ERR_MEMORY;
    memset(&output, 0, sizeof(output));
    output.data = uncompressed;
    output.capacity = uncompressed_capacity;

    result = codec->decoder_init(state, &options);
    if (result == PCX_OK)
        result = codec->decoder_sink(state, compressed, compressed_size,
                                     pp_pcx_write, &output);
    finish_result = codec->decoder_finish(state);
    if (result == PCX_OK) result = finish_result;
    pp_pcx_state_free(state);

    if (output.overflow) return PCX_ERR_OUTPUT_TOO_SMALL;
    if (result == PCX_OK && uncompressed_size) *uncompressed_size = output.size;
    return result;
}

static pp_status pp_pcx_decode(void *opaque,
                               pp_compression compression,
                               const uint8_t *compressed,
                               size_t compressed_size,
                               uint8_t *uncompressed,
                               size_t uncompressed_capacity,
                               size_t *uncompressed_size)
{
    pp_picocompress_codec *context = (pp_picocompress_codec *)opaque;
    const pcx_codec_v1 *codec;
    const char *name;
    pcx_result result;

    if (!context || !context->registry || !uncompressed_size ||
        (!compressed && compressed_size) ||
        (!uncompressed && uncompressed_capacity))
        return PP_ERR_ARGUMENT;

    name = pp_pcx_codec_name(compression);
    if (!name) return PP_ERR_UNSUPPORTED_CODEC;
    codec = pcx_registry_find(context->registry, name);
    if (!codec || !(codec->capabilities & PCX_CODEC_CAP_DECOMPRESS))
        return PP_ERR_UNSUPPORTED_CODEC;

    if (codec->decompress_buffer) {
        result = codec->decompress_buffer(compressed, compressed_size,
                                          uncompressed, uncompressed_capacity,
                                          uncompressed_size);
    } else {
        result = pp_pcx_stream_decode(codec, compressed, compressed_size,
                                      uncompressed, uncompressed_capacity,
                                      uncompressed_size);
    }
    return pp_pcx_map_result(result);
}

pp_status pp_picocompress_codec_init(pp_picocompress_codec *context,
                                     const pcx_registry *registry,
                                     pp_codec *codec)
{
    if (!context || !registry || !codec) return PP_ERR_ARGUMENT;
    context->registry = registry;
    codec->decode = pp_pcx_decode;
    codec->context = context;
    return PP_OK;
}
