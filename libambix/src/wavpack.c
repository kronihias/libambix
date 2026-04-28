/* wavpack.c - WavPack backend for libambix              -*- c -*-

   Copyright © 2026 Matthias Kronlachner.

   This file is part of libambix

   libambix is free software; you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as
   published by the Free Software Foundation; either version 2.1 of
   the License, or (at your option) any later version.

   libambix is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this program; if not, see <http://www.gnu.org/licenses/>.
*/

/*
   This backend stores ambix files as WavPack-compressed streams whose
   wrapper carries a faithful Apple CAF byte sequence (file header +
   'desc' + 'chan' + 'uuid' + marker/region chunks + 'data' chunk header
   minus payload). The audio payload is encoded by WavPack and decoded
   transparently. WavPack 5 explicitly supports CAF wrapper preservation
   (WP_FORMAT_CAF + WavpackAddWrapper), which is the mechanism the ambix
   spec endorses (Section 2.3 of "ambix - a suggested ambisonics format").
*/

#include "private.h"

#ifdef HAVE_WAVPACK

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__has_include)
# if __has_include(<wavpack/wavpack.h>)
#  include <wavpack/wavpack.h>
# else
#  include <wavpack.h>
# endif
#else
# include <wavpack.h>
#endif

/* ---------- CAF wrapper synthesis ---------- */

static void wpk_be_u16(uint8_t *p, uint16_t v) { p[0]=(v>>8)&0xff; p[1]=v&0xff; }
static void wpk_be_u32(uint8_t *p, uint32_t v) { p[0]=(v>>24)&0xff; p[1]=(v>>16)&0xff; p[2]=(v>>8)&0xff; p[3]=v&0xff; }
static void wpk_be_u64(uint8_t *p, uint64_t v) {
  p[0]=(v>>56)&0xff; p[1]=(v>>48)&0xff; p[2]=(v>>40)&0xff; p[3]=(v>>32)&0xff;
  p[4]=(v>>24)&0xff; p[5]=(v>>16)&0xff; p[6]=(v>>8)&0xff;  p[7]=v&0xff;
}
static uint16_t wpk_rd_u16(const uint8_t *p) { return ((uint16_t)p[0]<<8) | p[1]; }
static uint32_t wpk_rd_u32(const uint8_t *p) {
  return ((uint32_t)p[0]<<24) | ((uint32_t)p[1]<<16) | ((uint32_t)p[2]<<8) | p[3];
}
static uint64_t wpk_rd_u64(const uint8_t *p) {
  return ((uint64_t)p[0]<<56) | ((uint64_t)p[1]<<48) | ((uint64_t)p[2]<<40) | ((uint64_t)p[3]<<32)
       | ((uint64_t)p[4]<<24) | ((uint64_t)p[5]<<16) | ((uint64_t)p[6]<<8)  | p[7];
}
static void wpk_be_f64(uint8_t *p, double d) {
  uint64_t bits;
  memcpy(&bits, &d, 8);
  wpk_be_u64(p, bits);
}
static double wpk_rd_f64(const uint8_t *p) {
  uint64_t bits = wpk_rd_u64(p);
  double d;
  memcpy(&d, &bits, 8);
  return d;
}

/* ---------- buffered chunk list (write side) ---------- */

typedef struct wpk_chunk_s {
  uint32_t id;        /* 4cc, host byte order */
  uint8_t *data;
  int64_t  size;
} wpk_chunk_t;

/* ---------- private state ---------- */

typedef struct wpk_private_s {
  /* common */
  WavpackContext *wpc;
  FILE           *fp;       /* write side: WavPack writes through callback */
  int             writing;  /* 1 = encoder, 0 = decoder */
  int             initialized; /* writer: 1 once WavpackPackInit called */
  /* sample format */
  uint32_t        sample_rate;
  uint32_t        num_channels;
  int             bits_per_sample;   /* 16/24/32 */
  int             bytes_per_sample;  /* 2/3/4 */
  int             is_float;
  /* write side: buffered chunks */
  uint8_t        *pending_uuid;
  int64_t         pending_uuid_size;
  wpk_chunk_t    *chunks;
  uint32_t        num_chunks;
  /* int32 conversion scratch */
  int32_t        *xfer_buf;
  size_t          xfer_buf_samples;
  /* read side: parsed wrapper */
  uint8_t        *wrap;       /* not owned; lifetime tied to wpc */
  uint32_t        wrap_size;
  uint8_t        *uuid_data;  /* points into wrap */
  int64_t         uuid_size;
} wpk_private_t;

static inline wpk_private_t *PWP(ambix_t *ax) { return (wpk_private_t*)(ax->private_data); }

/* ---------- sample format glue ---------- */

static void
ambixsamp_to_wavpack(ambix_sampleformat_t fmt, int *bits, int *bytes, int *is_float) {
  switch (fmt) {
  case AMBIX_SAMPLEFORMAT_PCM16:   *bits=16; *bytes=2; *is_float=0; break;
  case AMBIX_SAMPLEFORMAT_PCM24:   *bits=24; *bytes=3; *is_float=0; break;
  case AMBIX_SAMPLEFORMAT_PCM32:   *bits=32; *bytes=4; *is_float=0; break;
  case AMBIX_SAMPLEFORMAT_FLOAT32: *bits=32; *bytes=4; *is_float=1; break;
  case AMBIX_SAMPLEFORMAT_FLOAT64: /* WavPack has no float64 — quietly downcast to float32 */
                                   *bits=32; *bytes=4; *is_float=1; break;
  default:                         *bits=24; *bytes=3; *is_float=0; break;
  }
}

static ambix_sampleformat_t
wavpack_to_ambixsamp(int bits, int is_float) {
  if (is_float) return AMBIX_SAMPLEFORMAT_FLOAT32;
  switch (bits) {
  case 16: return AMBIX_SAMPLEFORMAT_PCM16;
  case 24: return AMBIX_SAMPLEFORMAT_PCM24;
  case 32: return AMBIX_SAMPLEFORMAT_PCM32;
  }
  return AMBIX_SAMPLEFORMAT_NONE;
}

/* Ensure xfer_buf can hold (frames * channels) int32_t samples. */
static int
wpk_xfer_resize(wpk_private_t *p, size_t frames) {
  size_t need = frames * (size_t)p->num_channels;
  if (need <= p->xfer_buf_samples) return 1;
  int32_t *nb = (int32_t*)realloc(p->xfer_buf, need * sizeof(int32_t));
  if (!nb) return 0;
  p->xfer_buf      = nb;
  p->xfer_buf_samples = need;
  return 1;
}

/* ---------- CAF wrapper builder (write) ---------- */

/*
 * Build a CAF byte stream for the wrapper. Layout:
 *   caff header (8)
 *   'desc' chunk (12 + 32) -- mandatory
 *   'uuid' chunk (12 + uuid_size) -- if uuid_data != NULL
 *   for each buffered chunk: (12 + chunk size)
 *   'data' chunk header (12) + mEditCount(4) -- audio bytes are NOT in wrapper
 *
 * Returns malloc()d buffer on success, sets *out_size; caller frees.
 */
static uint8_t *
wpk_build_caf_wrapper(const wpk_private_t *p, uint32_t *out_size) {
  /* Compute total size. */
  size_t total = 8 /* file header */;
  total += 12 + 32; /* desc */
  if (p->pending_uuid && p->pending_uuid_size > 0)
    total += 12 + (size_t)p->pending_uuid_size;
  for (uint32_t i = 0; i < p->num_chunks; ++i)
    total += 12 + (size_t)p->chunks[i].size;
  total += 12 + 4; /* data chunk header + mEditCount */

  uint8_t *buf = (uint8_t*)malloc(total);
  if (!buf) { *out_size = 0; return NULL; }
  uint8_t *q = buf;

  /* CAF file header: "caff" + version=1 + flags=0 */
  memcpy(q, "caff", 4); q += 4;
  wpk_be_u16(q, 1); q += 2;
  wpk_be_u16(q, 0); q += 2;

  /* desc chunk */
  memcpy(q, "desc", 4); q += 4;
  wpk_be_u64(q, 32); q += 8;
  wpk_be_f64(q, (double)p->sample_rate); q += 8;
  memcpy(q, "lpcm", 4); q += 4;
  /* mFormatFlags: bit 0 = kCAFLinearPCMFormatFlagIsFloat, bit 1 = kCAFLinearPCMFormatFlagIsLittleEndian
     For ambix CAF we use big-endian (bit 1 = 0). */
  wpk_be_u32(q, p->is_float ? 0x01 : 0x00); q += 4;
  wpk_be_u32(q, (uint32_t)(p->bytes_per_sample * p->num_channels)); q += 4; /* mBytesPerPacket */
  wpk_be_u32(q, 1); q += 4; /* mFramesPerPacket */
  wpk_be_u32(q, p->num_channels); q += 4;
  wpk_be_u32(q, (uint32_t)p->bits_per_sample); q += 4;

  /* uuid chunk (if present) */
  if (p->pending_uuid && p->pending_uuid_size > 0) {
    memcpy(q, "uuid", 4); q += 4;
    wpk_be_u64(q, (uint64_t)p->pending_uuid_size); q += 8;
    memcpy(q, p->pending_uuid, (size_t)p->pending_uuid_size); q += p->pending_uuid_size;
  }

  /* buffered chunks */
  for (uint32_t i = 0; i < p->num_chunks; ++i) {
    memcpy(q, &p->chunks[i].id, 4); q += 4; /* id is already in CAF byte order */
    wpk_be_u64(q, (uint64_t)p->chunks[i].size); q += 8;
    if (p->chunks[i].size > 0)
      memcpy(q, p->chunks[i].data, (size_t)p->chunks[i].size);
    q += p->chunks[i].size;
  }

  /* data chunk: header + mEditCount=0; audio sample bytes are not in the wrapper */
  memcpy(q, "data", 4); q += 4;
  /* size = -1 (sentinel meaning "to end of file" per CAF spec; common in streaming) */
  wpk_be_u64(q, (uint64_t)(int64_t)-1); q += 8;
  wpk_be_u32(q, 0); q += 4; /* mEditCount */

  *out_size = (uint32_t)(q - buf);
  return buf;
}

/* ---------- WavPack write callback ---------- */

static int
wpk_write_block(void *id, void *data, int32_t bcount) {
  FILE *fp = (FILE*)id;
  if (!fp || bcount <= 0) return 1;
  size_t wrote = fwrite(data, 1, (size_t)bcount, fp);
  return (wrote == (size_t)bcount) ? 1 : 0;
}

/* ---------- lazy WavPack writer init ---------- */

static ambix_err_t
wpk_lazy_init_writer(ambix_t *ax) {
  wpk_private_t *p = PWP(ax);
  if (p->initialized) return AMBIX_ERR_SUCCESS;
  if (!p->writing) return AMBIX_ERR_INVALID_FILE;
  if (!p->fp || p->num_channels == 0) return AMBIX_ERR_INVALID_FORMAT;

  /* Open the WavPack writer using our FILE* and write callback. */
  p->wpc = WavpackOpenFileOutput(wpk_write_block, p->fp, NULL);
  if (!p->wpc) return AMBIX_ERR_UNKNOWN;
  WavpackSetFileInformation(p->wpc, "caf", WP_FORMAT_CAF);

  /* Build and attach the CAF wrapper. */
  uint32_t wrap_size = 0;
  uint8_t *wrap = wpk_build_caf_wrapper(p, &wrap_size);
  if (!wrap) {
    WavpackCloseFile(p->wpc); p->wpc = NULL;
    return AMBIX_ERR_UNKNOWN;
  }
  if (!WavpackAddWrapper(p->wpc, wrap, wrap_size)) {
    free(wrap);
    WavpackCloseFile(p->wpc); p->wpc = NULL;
    return AMBIX_ERR_UNKNOWN;
  }
  free(wrap); /* WavPack copies the bytes internally */

  /* Configure & init the encoder. */
  WavpackConfig cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.bits_per_sample  = p->bits_per_sample;
  cfg.bytes_per_sample = p->bytes_per_sample;
  cfg.num_channels     = (int)p->num_channels;
  cfg.sample_rate      = (int32_t)p->sample_rate;
  cfg.float_norm_exp   = p->is_float ? 127 : 0;
  cfg.qmode            = QMODE_BIG_ENDIAN; /* CAF audio bytes are BE by default */
  /* Channel mask 0 means "no Microsoft channel layout" — appropriate for ambisonics. */
  cfg.channel_mask     = 0;

  if (!WavpackSetConfiguration64(p->wpc, &cfg, -1, NULL)) {
    WavpackCloseFile(p->wpc); p->wpc = NULL;
    return AMBIX_ERR_UNKNOWN;
  }
  if (!WavpackPackInit(p->wpc)) {
    WavpackCloseFile(p->wpc); p->wpc = NULL;
    return AMBIX_ERR_UNKNOWN;
  }
  p->initialized = 1;
  return AMBIX_ERR_SUCCESS;
}

/* ---------- CAF wrapper parser (read) ---------- */

/*
 * Walk the wrapper bytes, populating audio-format fields and locating the
 * uuid chunk (if any). Buffered chunks (markers/regions) are not parsed
 * here — _ambix_read_chunk_wavpack walks the wrapper on demand.
 */
static ambix_err_t
wpk_parse_caf_wrapper(ambix_t *ax) {
  wpk_private_t *p = PWP(ax);
  if (!p->wrap || p->wrap_size < 8 + 12 + 32) return AMBIX_ERR_INVALID_FILE;
  if (memcmp(p->wrap, "caff", 4) != 0) return AMBIX_ERR_INVALID_FILE;
  /* version = wpk_rd_u16(p->wrap+4); flags = wpk_rd_u16(p->wrap+6); — unused */
  uint8_t *q   = p->wrap + 8;
  uint8_t *end = p->wrap + p->wrap_size;
  while (q + 12 <= end) {
    char id[5]; memcpy(id, q, 4); id[4]=0;
    uint64_t sz = wpk_rd_u64(q + 4);
    q += 12;
    if (memcmp(id, "data", 4) == 0) {
      /* data chunk: only mEditCount(4) sits in the wrapper; audio is in WavPack stream */
      break;
    }
    /* Bounds check (allow truncated 'desc'/'uuid' to be tolerated above the data chunk) */
    if (sz > (uint64_t)(end - q)) {
      /* truncated wrapper — consume rest as best effort */
      sz = (uint64_t)(end - q);
    }
    if (memcmp(id, "desc", 4) == 0 && sz >= 32) {
      double      sr            = wpk_rd_f64(q + 0);
      /* mFormatID at q+8 ('lpcm') — assumed */
      uint32_t    fmt_flags     = wpk_rd_u32(q + 12);
      /* mBytesPerPacket at q+16, mFramesPerPacket at q+20 — derivable */
      uint32_t    nch           = wpk_rd_u32(q + 24);
      uint32_t    bits_per_ch   = wpk_rd_u32(q + 28);
      p->sample_rate     = (uint32_t)sr;
      p->num_channels    = nch;
      p->bits_per_sample = (int)bits_per_ch;
      p->bytes_per_sample = (bits_per_ch + 7) / 8;
      p->is_float        = (fmt_flags & 0x01) ? 1 : 0;
    } else if (memcmp(id, "uuid", 4) == 0) {
      p->uuid_data = q;
      p->uuid_size = (int64_t)sz;
    }
    q += sz;
  }
  return AMBIX_ERR_SUCCESS;
}

/* ---------- backend entry points ---------- */

ambix_err_t
_ambix_open_wavpack(ambix_t *ambix, const char *path, const ambix_filemode_t mode, const ambix_info_t *ambixinfo) {
  wpk_private_t *p = (wpk_private_t*)calloc(1, sizeof(wpk_private_t));
  if (!p) return AMBIX_ERR_UNKNOWN;
  ambix->private_data = p;
  ambix->byteswap = 0; /* WavPack returns samples in host byte order */

  if (mode & AMBIX_WRITE) {
    p->writing      = 1;
    p->initialized  = 0;
    p->sample_rate  = (uint32_t)ambixinfo->samplerate;
    p->num_channels = ambixinfo->ambichannels + ambixinfo->extrachannels;
    ambixsamp_to_wavpack(ambixinfo->sampleformat, &p->bits_per_sample, &p->bytes_per_sample, &p->is_float);
    p->fp = fopen(path, "wb");
    if (!p->fp) return AMBIX_ERR_INVALID_FILE;
    /* Mirror the sndfile backend: report a CAF-equivalent ambix file. The
       outer ambix_open() runs _ambix_info_set() based on is_AMBIX +
       fileformat to partition channels into ambi/extra correctly. */
    memset(&ambix->realinfo, 0, sizeof(*ambixinfo));
    ambix->realinfo.samplerate   = ambixinfo->samplerate;
    ambix->realinfo.sampleformat = ambixinfo->sampleformat;
    ambix->channels = (int32_t)p->num_channels;
    ambix->is_AMBIX = 1;
    /* format is filled by the outer dispatcher based on info.fileformat */
    return AMBIX_ERR_SUCCESS;
  }

  /* READ path */
  p->writing = 0;
  char err_buf[80] = {0};
  p->wpc = WavpackOpenFileInput(path, err_buf, OPEN_WRAPPER | OPEN_NORMALIZE, 0);
  if (!p->wpc) return AMBIX_ERR_INVALID_FILE;

  /* Pull format info from WavPack (authoritative for samples), wrapper for chunks. */
  p->num_channels    = (uint32_t)WavpackGetNumChannels(p->wpc);
  p->bits_per_sample = WavpackGetBitsPerSample(p->wpc);
  p->bytes_per_sample = WavpackGetBytesPerSample(p->wpc);
  p->is_float        = (WavpackGetMode(p->wpc) & MODE_FLOAT) ? 1 : 0;
  p->sample_rate     = WavpackGetSampleRate(p->wpc);

  p->wrap_size = WavpackGetWrapperBytes(p->wpc);
  p->wrap      = (p->wrap_size > 0) ? WavpackGetWrapperData(p->wpc) : NULL;

  /* parse wrapper to find uuid chunk and validate format coherence */
  if (p->wrap) wpk_parse_caf_wrapper(ambix);

  /* Populate libambix's realinfo + handle-level state */
  memset(&ambix->realinfo, 0, sizeof(ambix->realinfo));
  ambix->realinfo.frames        = (uint64_t)WavpackGetNumSamples64(p->wpc);
  ambix->realinfo.samplerate    = (double)p->sample_rate;
  ambix->realinfo.extrachannels = p->num_channels;
  ambix->realinfo.sampleformat  = wavpack_to_ambixsamp(p->bits_per_sample, p->is_float);
  ambix->channels = (int32_t)p->num_channels;

  /* If we found a UUID chunk, mark as AMBIX_EXTENDED and fill ambix->matrix. */
  if (p->uuid_data && p->uuid_size >= 16 && _ambix_checkUUID((const char*)p->uuid_data) == 1) {
    if (_ambix_uuid1_to_matrix((const char*)p->uuid_data + 16, p->uuid_size - 16, &ambix->matrix, ambix->byteswap)) {
      ambix->is_AMBIX = 1;
      ambix->format   = AMBIX_EXTENDED;
    } else {
      ambix->is_AMBIX = 1;
      ambix->format   = AMBIX_BASIC;
    }
  } else {
    /* No uuid: BASIC if channel count is a full set (libambix's outer dispatcher decides), else NONE */
    ambix->is_AMBIX = 1;
    ambix->format   = AMBIX_BASIC;
  }
  return AMBIX_ERR_SUCCESS;
}

ambix_err_t
_ambix_close_wavpack(ambix_t *ambix) {
  wpk_private_t *p = PWP(ambix);
  if (!p) return AMBIX_ERR_SUCCESS;

  if (p->writing) {
    /* If we never wrote a sample, lazy_init was never called — synthesize an empty file. */
    if (!p->initialized) wpk_lazy_init_writer(ambix);
    if (p->wpc) {
      WavpackFlushSamples(p->wpc);
      WavpackCloseFile(p->wpc);
      p->wpc = NULL;
    }
    if (p->fp) { fclose(p->fp); p->fp = NULL; }
  } else {
    if (p->wpc) {
      WavpackCloseFile(p->wpc); /* WavPack-managed file, no fp owned by us */
      p->wpc = NULL;
    }
  }

  /* Free buffered state */
  if (p->pending_uuid) free(p->pending_uuid);
  if (p->chunks) {
    for (uint32_t i = 0; i < p->num_chunks; ++i)
      if (p->chunks[i].data) free(p->chunks[i].data);
    free(p->chunks);
  }
  if (p->xfer_buf) free(p->xfer_buf);
  free(p);
  ambix->private_data = NULL;
  return AMBIX_ERR_SUCCESS;
}

int64_t
_ambix_seek_wavpack(ambix_t *ambix, int64_t frames, int whence) {
  wpk_private_t *p = PWP(ambix);
  if (!p->wpc || p->writing) return -1;
  /* libambix's only caller (ambix_seek) passes a frame index; WavPack expects absolute */
  if (!WavpackSeekSample64(p->wpc, frames)) return -1;
  return frames;
}

/* ---------- chunk write (write side) ---------- */

ambix_err_t
_ambix_write_uuidchunk_wavpack(ambix_t *ax, const void *data, int64_t datasize) {
  wpk_private_t *p = PWP(ax);
  if (!p || !p->writing || p->initialized) return AMBIX_ERR_UNKNOWN;
  if (p->pending_uuid) free(p->pending_uuid);
  p->pending_uuid = (uint8_t*)malloc((size_t)datasize);
  if (!p->pending_uuid) return AMBIX_ERR_UNKNOWN;
  memcpy(p->pending_uuid, data, (size_t)datasize);
  p->pending_uuid_size = datasize;
  return AMBIX_ERR_SUCCESS;
}

ambix_err_t
_ambix_write_chunk_wavpack(ambix_t *ax, uint32_t id, const void *data, int64_t datasize) {
  wpk_private_t *p = PWP(ax);
  if (!p || !p->writing || p->initialized) return AMBIX_ERR_UNKNOWN;
  wpk_chunk_t *nc = (wpk_chunk_t*)realloc(p->chunks, (p->num_chunks + 1) * sizeof(wpk_chunk_t));
  if (!nc) return AMBIX_ERR_UNKNOWN;
  p->chunks = nc;
  wpk_chunk_t *c = &p->chunks[p->num_chunks];
  c->id   = id;
  c->size = datasize;
  c->data = NULL;
  if (datasize > 0) {
    c->data = (uint8_t*)malloc((size_t)datasize);
    if (!c->data) return AMBIX_ERR_UNKNOWN;
    memcpy(c->data, data, (size_t)datasize);
  }
  p->num_chunks++;
  return AMBIX_ERR_SUCCESS;
}

/* ---------- chunk read (read side) ---------- */

void *
_ambix_read_chunk_wavpack(ambix_t *ax, uint32_t id, uint32_t chunk_it, int64_t *datasize) {
  wpk_private_t *p = PWP(ax);
  *datasize = 0;
  if (!p || p->writing || !p->wrap || p->wrap_size < 8 + 12) return NULL;

  uint8_t *q   = p->wrap + 8;
  uint8_t *end = p->wrap + p->wrap_size;
  uint32_t want = id; /* big-endian on disk; id arg matches the bytes we wrote */
  uint32_t hits = 0;
  while (q + 12 <= end) {
    uint32_t this_id;
    memcpy(&this_id, q, 4);
    uint64_t sz = wpk_rd_u64(q + 4);
    q += 12;
    if (memcmp(q - 12, "data", 4) == 0) break;
    if (sz > (uint64_t)(end - q)) sz = (uint64_t)(end - q);
    if (this_id == want) {
      if (hits == chunk_it) {
        void *out = malloc((size_t)sz);
        if (!out) { *datasize = 0; return NULL; }
        memcpy(out, q, (size_t)sz);
        *datasize = (int64_t)sz;
        return out;
      }
      hits++;
    }
    q += sz;
  }
  return NULL;
}

/* ---------- sample I/O ---------- */

#define WPK_PACK_LOOP(SAMPLE_T, EXPR)                                          \
  do {                                                                         \
    if (wpk_lazy_init_writer(ambix) != AMBIX_ERR_SUCCESS) return -1;           \
    if (!wpk_xfer_resize(p, (size_t)frames)) return -1;                        \
    size_t total = (size_t)frames * (size_t)p->num_channels;                   \
    int32_t *dst = p->xfer_buf;                                                \
    const SAMPLE_T *src = data;                                                \
    for (size_t i = 0; i < total; ++i) dst[i] = (int32_t)EXPR;                 \
    if (!WavpackPackSamples(p->wpc, p->xfer_buf, (uint32_t)frames)) return -1; \
    return frames;                                                             \
  } while (0)

int64_t
_ambix_writef_int16_wavpack(ambix_t *ambix, const int16_t *data, int64_t frames) {
  wpk_private_t *p = PWP(ambix);
  WPK_PACK_LOOP(int16_t, (int32_t)src[i]);
}
int64_t
_ambix_writef_int32_wavpack(ambix_t *ambix, const int32_t *data, int64_t frames) {
  wpk_private_t *p = PWP(ambix);
  WPK_PACK_LOOP(int32_t, src[i]);
}
int64_t
_ambix_writef_float32_wavpack(ambix_t *ambix, const float32_t *data, int64_t frames) {
  wpk_private_t *p = PWP(ambix);
  if (wpk_lazy_init_writer(ambix) != AMBIX_ERR_SUCCESS) return -1;
  if (!wpk_xfer_resize(p, (size_t)frames)) return -1;
  size_t total = (size_t)frames * (size_t)p->num_channels;
  /* Reinterpret float bits as int32; WavPack sees CONFIG_FLOAT_DATA via float_norm_exp */
  if (p->is_float) {
    memcpy(p->xfer_buf, data, total * sizeof(int32_t));
  } else {
    /* User asked for int format but is feeding float samples — scale to int range */
    double scale = (double)((1u << (p->bits_per_sample - 1)) - 1);
    for (size_t i = 0; i < total; ++i) {
      double v = data[i] * scale;
      if (v >  scale) v =  scale;
      if (v < -scale - 1) v = -scale - 1;
      p->xfer_buf[i] = (int32_t)v;
    }
  }
  if (!WavpackPackSamples(p->wpc, p->xfer_buf, (uint32_t)frames)) return -1;
  return frames;
}
int64_t
_ambix_writef_float64_wavpack(ambix_t *ambix, const float64_t *data, int64_t frames) {
  wpk_private_t *p = PWP(ambix);
  if (wpk_lazy_init_writer(ambix) != AMBIX_ERR_SUCCESS) return -1;
  if (!wpk_xfer_resize(p, (size_t)frames)) return -1;
  size_t total = (size_t)frames * (size_t)p->num_channels;
  if (p->is_float) {
    /* downcast to float32 then reinterpret as int32 bits */
    for (size_t i = 0; i < total; ++i) {
      float f = (float)data[i];
      memcpy(&p->xfer_buf[i], &f, sizeof(int32_t));
    }
  } else {
    double scale = (double)((1u << (p->bits_per_sample - 1)) - 1);
    for (size_t i = 0; i < total; ++i) {
      double v = data[i] * scale;
      if (v >  scale) v =  scale;
      if (v < -scale - 1) v = -scale - 1;
      p->xfer_buf[i] = (int32_t)v;
    }
  }
  if (!WavpackPackSamples(p->wpc, p->xfer_buf, (uint32_t)frames)) return -1;
  return frames;
}

#undef WPK_PACK_LOOP

#define WPK_UNPACK_PROLOGUE                                                    \
  if (!p->wpc || p->writing) return -1;                                        \
  if (!wpk_xfer_resize(p, (size_t)frames)) return -1;                          \
  uint32_t got = WavpackUnpackSamples(p->wpc, p->xfer_buf, (uint32_t)frames);  \
  if (got == 0) return 0;                                                      \
  size_t total = (size_t)got * (size_t)p->num_channels;

int64_t
_ambix_readf_int16_wavpack(ambix_t *ambix, int16_t *data, int64_t frames) {
  wpk_private_t *p = PWP(ambix);
  WPK_UNPACK_PROLOGUE
  for (size_t i = 0; i < total; ++i) {
    int32_t v = p->xfer_buf[i];
    /* If source was higher bit depth, scale down. */
    if (p->bits_per_sample == 24)      v >>= 8;
    else if (p->bits_per_sample == 32) v >>= 16;
    if (v >  32767) v =  32767;
    if (v < -32768) v = -32768;
    data[i] = (int16_t)v;
  }
  return (int64_t)got;
}
int64_t
_ambix_readf_int32_wavpack(ambix_t *ambix, int32_t *data, int64_t frames) {
  wpk_private_t *p = PWP(ambix);
  WPK_UNPACK_PROLOGUE
  for (size_t i = 0; i < total; ++i) {
    int32_t v = p->xfer_buf[i];
    /* Promote lower-bit samples to fill the int32 range. */
    if (p->bits_per_sample == 16)      v <<= 16;
    else if (p->bits_per_sample == 24) v <<= 8;
    data[i] = v;
  }
  return (int64_t)got;
}
int64_t
_ambix_readf_float32_wavpack(ambix_t *ambix, float32_t *data, int64_t frames) {
  wpk_private_t *p = PWP(ambix);
  WPK_UNPACK_PROLOGUE
  if (p->is_float) {
    /* xfer_buf holds float32 bit patterns; reinterpret. */
    memcpy(data, p->xfer_buf, total * sizeof(float32_t));
  } else {
    double scale = 1.0 / (double)((1u << (p->bits_per_sample - 1)));
    for (size_t i = 0; i < total; ++i) {
      data[i] = (float32_t)((double)p->xfer_buf[i] * scale);
    }
  }
  return (int64_t)got;
}
int64_t
_ambix_readf_float64_wavpack(ambix_t *ambix, float64_t *data, int64_t frames) {
  wpk_private_t *p = PWP(ambix);
  WPK_UNPACK_PROLOGUE
  if (p->is_float) {
    /* upcast float32 → float64 */
    for (size_t i = 0; i < total; ++i) {
      float32_t f;
      memcpy(&f, &p->xfer_buf[i], sizeof(float32_t));
      data[i] = (float64_t)f;
    }
  } else {
    double scale = 1.0 / (double)((1u << (p->bits_per_sample - 1)));
    for (size_t i = 0; i < total; ++i) {
      data[i] = (float64_t)((double)p->xfer_buf[i] * scale);
    }
  }
  return (int64_t)got;
}

#undef WPK_UNPACK_PROLOGUE

#endif /* HAVE_WAVPACK */
