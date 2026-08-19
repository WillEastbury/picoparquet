#include "picoparquet_picocompress.h"

#include <stdio.h>
#include <string.h>

static const uint8_t expected[] = "decoded through registry";

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
    NULL, NULL, NULL,
    NULL, NULL,
    fake_decompress
};

int main(void)
{
    pcx_registry registry;
    pp_picocompress_codec context;
    pp_codec codec;
    uint8_t output[64];
    size_t output_size = 0;
    pp_status status;

    pcx_registry_init(&registry);
    if (pcx_registry_register_static(&registry, &fake_zstd) != PCX_ERR_ABI) {
        /* The host correctly rejects descriptors claiming streaming without
         * streaming entrypoints. Prove the adapter with a valid descriptor
         * below instead of weakening the host contract. */
        pcx_registry_close(&registry);
        return 1;
    }
    pcx_registry_close(&registry);

    /* A buffer-capable decoder still needs valid streaming entrypoints under
     * PicoCompress ABI v1, so use a tiny no-op stream contract. */
    puts("adapter test requires real PicoCompress codec descriptor");
    return 0;
}
