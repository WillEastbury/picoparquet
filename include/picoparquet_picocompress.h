#ifndef PICOPARQUET_PICOCOMPRESS_H
#define PICOPARQUET_PICOCOMPRESS_H

#include "picoparquet.h"
#include "picocompress/host.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pp_picocompress_codec {
    const pcx_registry *registry;
} pp_picocompress_codec;

/*
 * Bind PicoParquet's generic codec callback to a PicoCompress registry.
 *
 * The registry remains owned by the caller and must outlive any reader using
 * the returned pp_codec. Compression IDs are mapped to conventional codec
 * names (zstd, brotli, snappy, gzip, lz4, lzo) and resolved at decode time,
 * so installing another PicoCompress module does not require a PicoParquet
 * rebuild.
 */
pp_status pp_picocompress_codec_init(pp_picocompress_codec *context,
                                     const pcx_registry *registry,
                                     pp_codec *codec);

#ifdef __cplusplus
}
#endif

#endif
