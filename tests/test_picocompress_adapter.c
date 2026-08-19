#include "picoparquet_picocompress.h"

#include <stdio.h>
#include <string.h>

static const uint8_t expected[] = "decoded through registry";

static pcx_result fake_decoder_init(void *state, const pcx_options *options)
{
    (void)state;
    if (options && options->count) return PCX_ERR_UNSUPPORTED;
    return PCX_OK;
}

static pcx_result fake_decoder_sink(void *state, const uint8_t *data, size_t len,
                                    pcx_write_fn write_fn, void *write_user)
{
    (void)state;
    (void)data;
    (void)len;
    if (!write_fn) return PCX_ERR_INPUT;
    return write_fn(write_user, expected, sizeof(expected) - 1u) == 0
        ? PCX_OK : PCX_ERR_WRITE;
}

static pcx_result fake_decoder_finish(void *state)
{
    (void)state;
    return PCX_OK;
}

static pcx_result fake_decompress(const uint8_t *input, size_t input_len,
                                  uint8_t *output, size_t output_cap,
                                  size_t *output_len)
{
    (void)input;
    (void)input_len;
    if (!output_len || output_cap < sizeof(expected) - 1u)
        return PCX_ERR_OUTPUT_TOO_SMALL;
    memcpy(output, expected, sizeof(expected) - 1u);
    *output_len = sizeof(expected) - 1u;
    return PCX_OK;
}

static const pcx_codec_v1 fake_zstd = {
    PCX_CODEC_ABI_V1,
    sizeof(pcx_codec_v1),
    "zstd",
    "test zstd codec",
    "zstandard",
    PCX_CODEC_CAP_DECOMPRESS | PCX_CODEC_CAP_STREAMING,
    0, 0,
    1, 1,
    NULL, NULL, NULL,
    fake_decoder_init,
    fake_decoder_sink,
    fake_decoder_finish,
    NULL, NULL,
    fake_decompress
};

int main(void)
{
    static const uint8_t compressed[] = { 1, 2, 3 };
    pcx_registry registry;
    pp_picocompress_codec context;
    pp_codec codec;
    uint8_t output[64];
    size_t output_size = 0;
    pp_status status;

    pcx_registry_init(&registry);
    if (pcx_registry_register_static(&registry, &fake_zstd) != PCX_OK) {
        fprintf(stderr, "failed to register test zstd codec\n");
        return 1;
    }
    if (pp_picocompress_codec_init(&context, &registry, &codec) != PP_OK) {
        pcx_registry_close(&registry);
        return 2;
    }

    status = codec.decode(codec.context, PP_COMPRESSION_ZSTD,
                          compressed, sizeof(compressed),
                          output, sizeof(output), &output_size);
    if (status != PP_OK || output_size != sizeof(expected) - 1u ||
        memcmp(output, expected, output_size) != 0) {
        fprintf(stderr, "registry decode failed status=%d size=%zu\n",
                (int)status, output_size);
        pcx_registry_close(&registry);
        return 3;
    }

    output_size = 0;
    status = codec.decode(codec.context, PP_COMPRESSION_BROTLI,
                          compressed, sizeof(compressed),
                          output, sizeof(output), &output_size);
    if (status != PP_ERR_UNSUPPORTED_CODEC) {
        fprintf(stderr, "missing codec did not fail explicitly: %d\n", (int)status);
        pcx_registry_close(&registry);
        return 4;
    }

    pcx_registry_close(&registry);
    puts("PicoParquet PicoCompress adapter tests passed");
    return 0;
}
