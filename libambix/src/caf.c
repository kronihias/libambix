/* caf.c - native CAF reader/writer backend for libambix      -*- c -*-

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
   Native CAF (Apple Core Audio Format) reader/writer for libambix. Replaces
   the previous libsndfile-backed implementation. Sample I/O is interleaved
   big-endian (the only mode the AMBIX spec mandates for CAF). Chunk synthesis
   and parsing are shared with the WavPack backend via caf_io.h.
*/

#include "private.h"
#include "caf_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef SEEK_SET
# include <sys/types.h>
#endif

/* Portable 64-bit file offsets. MSVC's CRT does not expose POSIX
 * fseeko/ftello/off_t, so map them to the _fseeki64/_ftelli64 equivalents. */
#ifdef _MSC_VER
# include <stdint.h>
typedef int64_t ambix_off_t;
# define fseeko(stream, offset, whence) _fseeki64((stream), (offset), (whence))
# define ftello(stream)                 _ftelli64(stream)
#else
typedef off_t ambix_off_t;
#endif

/* ---------- private state ---------- */

typedef struct caf_private_s {
  FILE   *fp;
  int     writing;
  int     header_written;        /* writer: 1 once wrapper has been emitted */
  /* sample format */
  uint32_t sample_rate;
  uint32_t num_channels;
  int      bits_per_sample;      /* 16/24/32/64 */
  int      bytes_per_sample;     /* 2/3/4/8 */
  int      is_float;             /* 0=PCM, 1=float */
  /* read-side: buffered wrapper bytes (we own these) */
  uint8_t *wrap;
  uint32_t wrap_size;
  caf_parsed_t parsed;
  /* file offsets */
  uint64_t audio_start_offset;   /* byte offset of first audio sample */
  uint64_t audio_byte_size;      /* known size in bytes */
  uint64_t data_size_offset;     /* writer: file offset of 'data' chunk's 8-byte size field */
  uint64_t frames_written;
  /* writer: pending chunks before header emission */
  uint8_t *pending_uuid;
  int64_t  pending_uuid_size;
  caf_chunk_t *chunks;
  uint32_t     num_chunks;
  /* sample-conversion scratch buffer */
  uint8_t *scratch;
  size_t   scratch_bytes;
} caf_private_t;

static inline caf_private_t *PCAF(ambix_t *ax) { return (caf_private_t*)ax->private_data; }

static int host_is_little_endian(void) {
  uint16_t x = 1;
  return *(uint8_t*)&x == 1;
}

/* Ensure scratch can hold (frames * bytes_per_sample * num_channels) bytes. */
static int caf_scratch_resize(caf_private_t *p, size_t frames) {
  size_t need = frames * (size_t)p->bytes_per_sample * (size_t)p->num_channels;
  if (need <= p->scratch_bytes) return 1;
  uint8_t *nb = (uint8_t*)realloc(p->scratch, need);
  if (!nb) return 0;
  p->scratch = nb;
  p->scratch_bytes = need;
  return 1;
}

/* ---------- sample-format mapping ---------- */

static void
ambixsamp_to_caf(ambix_sampleformat_t fmt, int *bits, int *bytes, int *is_float) {
  switch (fmt) {
  case AMBIX_SAMPLEFORMAT_PCM16:   *bits=16; *bytes=2; *is_float=0; break;
  case AMBIX_SAMPLEFORMAT_PCM24:   *bits=24; *bytes=3; *is_float=0; break;
  case AMBIX_SAMPLEFORMAT_PCM32:   *bits=32; *bytes=4; *is_float=0; break;
  case AMBIX_SAMPLEFORMAT_FLOAT32: *bits=32; *bytes=4; *is_float=1; break;
  case AMBIX_SAMPLEFORMAT_FLOAT64: *bits=64; *bytes=8; *is_float=1; break;
  default:                         *bits=24; *bytes=3; *is_float=0; break;
  }
}

static ambix_sampleformat_t
caf_to_ambixsamp(int bits, int is_float) {
  if (is_float) {
    if (bits == 64) return AMBIX_SAMPLEFORMAT_FLOAT64;
    return AMBIX_SAMPLEFORMAT_FLOAT32;
  }
  switch (bits) {
  case 16: return AMBIX_SAMPLEFORMAT_PCM16;
  case 24: return AMBIX_SAMPLEFORMAT_PCM24;
  case 32: return AMBIX_SAMPLEFORMAT_PCM32;
  }
  return AMBIX_SAMPLEFORMAT_NONE;
}

/* ---------- wrapper emission (write side) ---------- */

static ambix_err_t
caf_write_header(ambix_t *ax) {
  caf_private_t *p = PCAF(ax);
  if (p->header_written) return AMBIX_ERR_SUCCESS;
  if (!p->writing || !p->fp || p->num_channels == 0) return AMBIX_ERR_INVALID_FORMAT;

  caf_wrapper_spec_t spec;
  memset(&spec, 0, sizeof(spec));
  spec.sample_rate        = p->sample_rate;
  spec.num_channels       = p->num_channels;
  spec.bits_per_sample    = p->bits_per_sample;
  spec.bytes_per_sample   = p->bytes_per_sample;
  spec.is_float           = p->is_float;
  spec.data_size_sentinel = 0; /* native CAF: patch size at close */
  spec.uuid_data          = p->pending_uuid;
  spec.uuid_size          = p->pending_uuid_size;
  spec.chunks             = p->chunks;
  spec.num_chunks         = p->num_chunks;

  uint32_t wrap_size = 0;
  uint32_t data_size_field_off = 0;
  uint8_t *wrap = cafio_build_wrapper(&spec, &wrap_size, &data_size_field_off);
  if (!wrap) return AMBIX_ERR_UNKNOWN;

  if (fwrite(wrap, 1, wrap_size, p->fp) != wrap_size) {
    free(wrap);
    return AMBIX_ERR_UNKNOWN;
  }
  free(wrap);

  p->data_size_offset   = data_size_field_off;
  p->audio_start_offset = wrap_size;
  p->header_written     = 1;
  return AMBIX_ERR_SUCCESS;
}

/* Patch the 'data' chunk size field at close. */
static ambix_err_t
caf_patch_data_size(caf_private_t *p) {
  if (!p->fp || !p->header_written) return AMBIX_ERR_SUCCESS;
  uint64_t audio_bytes = p->frames_written * (uint64_t)p->bytes_per_sample * (uint64_t)p->num_channels;
  /* CAF data chunk size includes mEditCount(4) + audio bytes */
  uint64_t chunk_size = 4 + audio_bytes;
  uint8_t buf[8];
  cafio_be_u64(buf, chunk_size);
  if (fseeko(p->fp, (ambix_off_t)p->data_size_offset, SEEK_SET) != 0) return AMBIX_ERR_UNKNOWN;
  if (fwrite(buf, 1, 8, p->fp) != 8) return AMBIX_ERR_UNKNOWN;
  if (fseeko(p->fp, 0, SEEK_END) != 0) return AMBIX_ERR_UNKNOWN;
  return AMBIX_ERR_SUCCESS;
}

/* ---------- read-side wrapper acquisition ---------- */

static ambix_err_t
caf_read_wrapper(caf_private_t *p) {
  /* Read 8-byte caff header */
  uint8_t hdr[8];
  if (fread(hdr, 1, 8, p->fp) != 8) return AMBIX_ERR_INVALID_FILE;
  if (memcmp(hdr, "caff", 4) != 0) return AMBIX_ERR_INVALID_FILE;

  /* Accumulate the caff header into wrap, then loop reading 12-byte chunk headers
   * + payloads until we consume the 'data' chunk header + mEditCount. */
  size_t cap = 256;
  uint8_t *buf = (uint8_t*)malloc(cap);
  if (!buf) return AMBIX_ERR_UNKNOWN;
  size_t used = 0;
  memcpy(buf, hdr, 8); used = 8;

  uint64_t data_chunk_size = 0;
  int saw_data = 0;

  for (;;) {
    /* Ensure room for 12-byte chunk header */
    if (used + 12 > cap) {
      cap *= 2;
      uint8_t *nb = (uint8_t*)realloc(buf, cap);
      if (!nb) { free(buf); return AMBIX_ERR_UNKNOWN; }
      buf = nb;
    }
    if (fread(buf + used, 1, 12, p->fp) != 12) {
      /* EOF before 'data' chunk header → malformed */
      free(buf);
      return AMBIX_ERR_INVALID_FILE;
    }
    char id[4]; memcpy(id, buf + used, 4);
    uint64_t sz = cafio_rd_u64(buf + used + 4);
    used += 12;

    if (memcmp(id, "data", 4) == 0) {
      /* Read mEditCount(4) into the wrapper buffer; audio sample bytes stay on disk. */
      if (used + 4 > cap) {
        cap = used + 4;
        uint8_t *nb = (uint8_t*)realloc(buf, cap);
        if (!nb) { free(buf); return AMBIX_ERR_UNKNOWN; }
        buf = nb;
      }
      if (fread(buf + used, 1, 4, p->fp) != 4) {
        free(buf);
        return AMBIX_ERR_INVALID_FILE;
      }
      used += 4;
      data_chunk_size = sz;
      saw_data = 1;
      break;
    }

    /* Bound chunk size to remaining file (tolerates lying headers) */
    if (sz > 0) {
      if (used + sz < used) { free(buf); return AMBIX_ERR_INVALID_FILE; } /* overflow */
      if (used + sz > cap) {
        while (used + sz > cap) cap *= 2;
        uint8_t *nb = (uint8_t*)realloc(buf, cap);
        if (!nb) { free(buf); return AMBIX_ERR_UNKNOWN; }
        buf = nb;
      }
      if (fread(buf + used, 1, (size_t)sz, p->fp) != (size_t)sz) {
        free(buf);
        return AMBIX_ERR_INVALID_FILE;
      }
      used += (size_t)sz;
    }
  }

  if (!saw_data) { free(buf); return AMBIX_ERR_INVALID_FILE; }

  p->wrap = buf;
  p->wrap_size = (uint32_t)used;
  p->audio_start_offset = (uint64_t)ftello(p->fp);

  /* Determine audio_byte_size. Streaming sentinel = 0xFFFFFFFFFFFFFFFF means
   * "audio extends to end of file". Otherwise: data_chunk_size - 4 (mEditCount). */
  if (data_chunk_size == (uint64_t)(int64_t)-1) {
    if (fseeko(p->fp, 0, SEEK_END) != 0) return AMBIX_ERR_INVALID_FILE;
    ambix_off_t end = ftello(p->fp);
    if (end < 0 || (uint64_t)end < p->audio_start_offset) return AMBIX_ERR_INVALID_FILE;
    p->audio_byte_size = (uint64_t)end - p->audio_start_offset;
    fseeko(p->fp, (ambix_off_t)p->audio_start_offset, SEEK_SET);
  } else if (data_chunk_size >= 4) {
    p->audio_byte_size = data_chunk_size - 4;
  } else {
    p->audio_byte_size = 0;
  }

  return cafio_parse_wrapper(p->wrap, p->wrap_size, &p->parsed);
}

/* ---------- backend entry points ---------- */

ambix_err_t
_ambix_open_caf(ambix_t *ambix, const char *path, const ambix_filemode_t mode, const ambix_info_t *ambixinfo) {
  caf_private_t *p = (caf_private_t*)calloc(1, sizeof(caf_private_t));
  if (!p) return AMBIX_ERR_UNKNOWN;
  ambix->private_data = p;
  ambix->byteswap = host_is_little_endian() ? 1 : 0;

  if (mode & AMBIX_WRITE) {
    p->writing = 1;
    p->sample_rate  = (uint32_t)ambixinfo->samplerate;
    p->num_channels = ambixinfo->ambichannels + ambixinfo->extrachannels;
    ambixsamp_to_caf(ambixinfo->sampleformat, &p->bits_per_sample, &p->bytes_per_sample, &p->is_float);
    p->fp = fopen(path, "wb");
    if (!p->fp) return AMBIX_ERR_INVALID_FILE;
    memset(&ambix->realinfo, 0, sizeof(*ambixinfo));
    ambix->realinfo.samplerate   = ambixinfo->samplerate;
    ambix->realinfo.sampleformat = ambixinfo->sampleformat;
    ambix->channels = (int32_t)p->num_channels;
    ambix->is_AMBIX = 1;
    return AMBIX_ERR_SUCCESS;
  }

  /* READ */
  p->writing = 0;
  p->fp = fopen(path, "rb");
  if (!p->fp) return AMBIX_ERR_INVALID_FILE;
  ambix_err_t err = caf_read_wrapper(p);
  if (err != AMBIX_ERR_SUCCESS) return err;

  /* Pull format from parsed wrapper. */
  p->sample_rate     = p->parsed.sample_rate;
  p->num_channels    = p->parsed.num_channels;
  p->bits_per_sample = p->parsed.bits_per_sample;
  p->bytes_per_sample = p->parsed.bytes_per_sample;
  p->is_float        = p->parsed.is_float;

  if (p->num_channels == 0 || p->bytes_per_sample == 0) return AMBIX_ERR_INVALID_FILE;

  uint64_t frame_size = (uint64_t)p->bytes_per_sample * (uint64_t)p->num_channels;
  uint64_t frames = (frame_size > 0) ? (p->audio_byte_size / frame_size) : 0;

  memset(&ambix->realinfo, 0, sizeof(ambix->realinfo));
  ambix->realinfo.frames        = frames;
  ambix->realinfo.samplerate    = (double)p->sample_rate;
  ambix->realinfo.extrachannels = p->num_channels;
  ambix->realinfo.sampleformat  = caf_to_ambixsamp(p->bits_per_sample, p->is_float);
  ambix->channels = (int32_t)p->num_channels;

  /* CAF → ambix file format detection */
  ambix->is_AMBIX = 1;
  if (p->parsed.uuid_data && p->parsed.uuid_size >= 16
      && _ambix_checkUUID((const char*)p->parsed.uuid_data) == 1) {
    if (_ambix_uuid1_to_matrix((const char*)p->parsed.uuid_data + 16,
                               p->parsed.uuid_size - 16,
                               &ambix->matrix, ambix->byteswap)) {
      ambix->format = AMBIX_EXTENDED;
    } else {
      ambix->format = AMBIX_BASIC;
    }
  } else {
    ambix->format = AMBIX_BASIC;
  }
  return AMBIX_ERR_SUCCESS;
}

ambix_err_t
_ambix_close_caf(ambix_t *ambix) {
  caf_private_t *p = PCAF(ambix);
  if (!p) return AMBIX_ERR_SUCCESS;

  if (p->writing) {
    /* Even on empty files, emit a valid wrapper so the file is well-formed. */
    if (!p->header_written) caf_write_header(ambix);
    caf_patch_data_size(p);
    if (p->fp) { fclose(p->fp); p->fp = NULL; }
  } else {
    if (p->fp) { fclose(p->fp); p->fp = NULL; }
  }

  if (p->wrap) free(p->wrap);
  if (p->pending_uuid) free(p->pending_uuid);
  if (p->chunks) {
    for (uint32_t i = 0; i < p->num_chunks; ++i)
      if (p->chunks[i].data) free(p->chunks[i].data);
    free(p->chunks);
  }
  if (p->scratch) free(p->scratch);
  free(p);
  ambix->private_data = NULL;
  return AMBIX_ERR_SUCCESS;
}

int64_t
_ambix_seek_caf(ambix_t *ambix, int64_t frames, int whence) {
  caf_private_t *p = PCAF(ambix);
  if (!p->fp || p->writing) return -1;
  uint64_t frame_size = (uint64_t)p->bytes_per_sample * (uint64_t)p->num_channels;
  if (frame_size == 0) return -1;
  ambix_off_t target;
  /* Mask to ANSI seek constants; caller may OR in libsndfile-style flags but
   * the only valid stdlib values are SEEK_SET/SEEK_CUR/SEEK_END. */
  int w = whence & 0x07;
  switch (w) {
  case SEEK_SET:
    target = (ambix_off_t)(p->audio_start_offset + (uint64_t)frames * frame_size);
    break;
  case SEEK_CUR:
    target = ftello(p->fp) + (ambix_off_t)((int64_t)frame_size * frames);
    break;
  case SEEK_END:
    target = (ambix_off_t)(p->audio_start_offset + p->audio_byte_size)
           + (ambix_off_t)((int64_t)frame_size * frames);
    break;
  default:
    /* If caller did not pass a valid whence, default to SEEK_SET semantics. */
    target = (ambix_off_t)(p->audio_start_offset + (uint64_t)frames * frame_size);
    break;
  }
  if (fseeko(p->fp, target, SEEK_SET) != 0) return -1;
  return (int64_t)(((uint64_t)target - p->audio_start_offset) / frame_size);
}

/* ---------- chunk write/read ---------- */

ambix_err_t
_ambix_write_uuidchunk_caf(ambix_t *ax, const void *data, int64_t datasize) {
  caf_private_t *p = PCAF(ax);
  if (!p || !p->writing || p->header_written) return AMBIX_ERR_UNKNOWN;
  if (p->pending_uuid) free(p->pending_uuid);
  p->pending_uuid = (uint8_t*)malloc((size_t)datasize);
  if (!p->pending_uuid) return AMBIX_ERR_UNKNOWN;
  memcpy(p->pending_uuid, data, (size_t)datasize);
  p->pending_uuid_size = datasize;
  return AMBIX_ERR_SUCCESS;
}

ambix_err_t
_ambix_write_chunk_caf(ambix_t *ax, uint32_t id, const void *data, int64_t datasize) {
  caf_private_t *p = PCAF(ax);
  if (!p || !p->writing || p->header_written) return AMBIX_ERR_UNKNOWN;
  caf_chunk_t *nc = (caf_chunk_t*)realloc(p->chunks, (p->num_chunks + 1) * sizeof(caf_chunk_t));
  if (!nc) return AMBIX_ERR_UNKNOWN;
  p->chunks = nc;
  caf_chunk_t *c = &p->chunks[p->num_chunks];
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

void *
_ambix_read_chunk_caf(ambix_t *ax, uint32_t id, uint32_t chunk_it, int64_t *datasize) {
  caf_private_t *p = PCAF(ax);
  *datasize = 0;
  if (!p || p->writing) return NULL;
  return cafio_find_chunk(p->wrap, p->wrap_size, id, chunk_it, datasize);
}

/* ---------- sample I/O helpers ---------- */

/* Encode one host int32 sample (right-aligned to bits_per_sample) as
 * `bytes_per_sample` big-endian bytes. */
static inline void
caf_pack_int_sample(uint8_t *dst, int32_t v, int bytes_per_sample) {
  switch (bytes_per_sample) {
  case 2:
    dst[0] = (uint8_t)((v >> 8) & 0xff);
    dst[1] = (uint8_t)( v       & 0xff);
    break;
  case 3:
    dst[0] = (uint8_t)((v >> 16) & 0xff);
    dst[1] = (uint8_t)((v >>  8) & 0xff);
    dst[2] = (uint8_t)( v        & 0xff);
    break;
  case 4:
    dst[0] = (uint8_t)((v >> 24) & 0xff);
    dst[1] = (uint8_t)((v >> 16) & 0xff);
    dst[2] = (uint8_t)((v >>  8) & 0xff);
    dst[3] = (uint8_t)( v        & 0xff);
    break;
  }
}

/* Decode one big-endian int sample to host int32, sign-extended from
 * bits_per_sample. Result is right-aligned (i.e. for PCM24 the int32 ranges
 * over [-2^23, 2^23-1]). */
static inline int32_t
caf_unpack_int_sample(const uint8_t *src, int bytes_per_sample) {
  uint32_t u;
  switch (bytes_per_sample) {
  case 2:
    u = ((uint32_t)src[0] << 8) | (uint32_t)src[1];
    if (u & 0x8000) u |= 0xffff0000u;
    return (int32_t)u;
  case 3:
    u = ((uint32_t)src[0] << 16) | ((uint32_t)src[1] << 8) | (uint32_t)src[2];
    if (u & 0x800000) u |= 0xff000000u;
    return (int32_t)u;
  case 4:
    u = ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16)
      | ((uint32_t)src[2] <<  8) | (uint32_t)src[3];
    return (int32_t)u;
  }
  return 0;
}

/* Float→int conversion: round-to-nearest (libsndfile-compatible), then clamp.
 * Using llrint avoids the truncation bias that plain (int32_t)cast introduces
 * (which would push 24-bit round-trip errors above 1 LSB, failing strict
 * tests). max_val/min_val are the inclusive int range for `bits_per_sample`. */
static inline int32_t
caf_round_clamp(double v, double max_val, double min_val) {
  if (v >= max_val) return (int32_t)max_val;
  if (v <= min_val) return (int32_t)min_val;
  return (int32_t)llrint(v);
}

/* ---------- write side ----------
 *
 * Convention (matches the wavpack backend):
 *   - When caller passes int16_t to a PCM-N file (N >= 16), the int16 sits in
 *     the high 16 bits of the int32 representation. Packing for N>16 shifts
 *     left to fill.
 *   - When caller passes int32_t, it's already right-aligned for whatever the
 *     stored width is; we shift right to fit narrower widths.
 *   - When caller passes float to an int PCM file, scale by 2^(N-1)-1 and clamp.
 *   - When caller passes int to a float file, scale by 1/(2^(N-1)).
 */

int64_t
_ambix_writef_int16_caf(ambix_t *ambix, const int16_t *data, int64_t frames) {
  caf_private_t *p = PCAF(ambix);
  if (caf_write_header(ambix) != AMBIX_ERR_SUCCESS) return -1;
  if (!caf_scratch_resize(p, (size_t)frames)) return -1;
  size_t total = (size_t)frames * (size_t)p->num_channels;
  size_t bps = (size_t)p->bytes_per_sample;
  uint8_t *dst = p->scratch;

  if (p->is_float) {
    /* int16 → float */
    double inv = 1.0 / 32768.0;
    if (p->bits_per_sample == 32) {
      for (size_t i = 0; i < total; ++i) {
        float f = (float)((double)data[i] * inv);
        uint32_t u; memcpy(&u, &f, 4);
        cafio_be_u32(dst + i*4, u);
      }
    } else { /* 64 */
      for (size_t i = 0; i < total; ++i) {
        double d = (double)data[i] * inv;
        uint64_t u; memcpy(&u, &d, 8);
        cafio_be_u64(dst + i*8, u);
      }
    }
  } else {
    /* int16 → PCM N */
    int shift = p->bits_per_sample - 16; /* 0 for 16, 8 for 24, 16 for 32 */
    for (size_t i = 0; i < total; ++i) {
      int32_t v = (int32_t)data[i] << (shift > 0 ? shift : 0);
      caf_pack_int_sample(dst + i*bps, v, (int)bps);
    }
  }

  size_t want = total * bps;
  if (fwrite(p->scratch, 1, want, p->fp) != want) return -1;
  p->frames_written += (uint64_t)frames;
  return frames;
}

int64_t
_ambix_writef_int32_caf(ambix_t *ambix, const int32_t *data, int64_t frames) {
  caf_private_t *p = PCAF(ambix);
  if (caf_write_header(ambix) != AMBIX_ERR_SUCCESS) return -1;
  if (!caf_scratch_resize(p, (size_t)frames)) return -1;
  size_t total = (size_t)frames * (size_t)p->num_channels;
  size_t bps = (size_t)p->bytes_per_sample;
  uint8_t *dst = p->scratch;

  if (p->is_float) {
    /* int32 → float (treat int32 as full-range, scale to [-1,1]) */
    double inv = 1.0 / 2147483648.0;
    if (p->bits_per_sample == 32) {
      for (size_t i = 0; i < total; ++i) {
        float f = (float)((double)data[i] * inv);
        uint32_t u; memcpy(&u, &f, 4);
        cafio_be_u32(dst + i*4, u);
      }
    } else {
      for (size_t i = 0; i < total; ++i) {
        double d = (double)data[i] * inv;
        uint64_t u; memcpy(&u, &d, 8);
        cafio_be_u64(dst + i*8, u);
      }
    }
  } else {
    /* int32 → PCM N (treat int32 as full-range; right-shift to narrower widths) */
    int shift = 32 - p->bits_per_sample; /* 16 for 16, 8 for 24, 0 for 32 */
    for (size_t i = 0; i < total; ++i) {
      int32_t v = data[i] >> shift;
      caf_pack_int_sample(dst + i*bps, v, (int)bps);
    }
  }

  size_t want = total * bps;
  if (fwrite(p->scratch, 1, want, p->fp) != want) return -1;
  p->frames_written += (uint64_t)frames;
  return frames;
}

int64_t
_ambix_writef_float32_caf(ambix_t *ambix, const float32_t *data, int64_t frames) {
  caf_private_t *p = PCAF(ambix);
  if (caf_write_header(ambix) != AMBIX_ERR_SUCCESS) return -1;
  if (!caf_scratch_resize(p, (size_t)frames)) return -1;
  size_t total = (size_t)frames * (size_t)p->num_channels;
  size_t bps = (size_t)p->bytes_per_sample;
  uint8_t *dst = p->scratch;

  if (p->is_float) {
    if (p->bits_per_sample == 32) {
      for (size_t i = 0; i < total; ++i) {
        uint32_t u; memcpy(&u, &data[i], 4);
        cafio_be_u32(dst + i*4, u);
      }
    } else { /* 64 */
      for (size_t i = 0; i < total; ++i) {
        double d = (double)data[i];
        uint64_t u; memcpy(&u, &d, 8);
        cafio_be_u64(dst + i*8, u);
      }
    }
  } else {
    /* float → PCM N */
    double scale = (double)((1u << (p->bits_per_sample - 1)) - 1u);
    double max_v = scale;
    double min_v = -scale - 1.0;
    for (size_t i = 0; i < total; ++i) {
      double v = (double)data[i] * (scale + 1.0);
      int32_t iv = caf_round_clamp(v, max_v, min_v);
      caf_pack_int_sample(dst + i*bps, iv, (int)bps);
    }
  }

  size_t want = total * bps;
  if (fwrite(p->scratch, 1, want, p->fp) != want) return -1;
  p->frames_written += (uint64_t)frames;
  return frames;
}

int64_t
_ambix_writef_float64_caf(ambix_t *ambix, const float64_t *data, int64_t frames) {
  caf_private_t *p = PCAF(ambix);
  if (caf_write_header(ambix) != AMBIX_ERR_SUCCESS) return -1;
  if (!caf_scratch_resize(p, (size_t)frames)) return -1;
  size_t total = (size_t)frames * (size_t)p->num_channels;
  size_t bps = (size_t)p->bytes_per_sample;
  uint8_t *dst = p->scratch;

  if (p->is_float) {
    if (p->bits_per_sample == 64) {
      for (size_t i = 0; i < total; ++i) {
        uint64_t u; memcpy(&u, &data[i], 8);
        cafio_be_u64(dst + i*8, u);
      }
    } else { /* 32 */
      for (size_t i = 0; i < total; ++i) {
        float f = (float)data[i];
        uint32_t u; memcpy(&u, &f, 4);
        cafio_be_u32(dst + i*4, u);
      }
    }
  } else {
    double scale = (double)((1u << (p->bits_per_sample - 1)) - 1u);
    double max_v = scale;
    double min_v = -scale - 1.0;
    for (size_t i = 0; i < total; ++i) {
      double v = data[i] * (scale + 1.0);
      int32_t iv = caf_round_clamp(v, max_v, min_v);
      caf_pack_int_sample(dst + i*bps, iv, (int)bps);
    }
  }

  size_t want = total * bps;
  if (fwrite(p->scratch, 1, want, p->fp) != want) return -1;
  p->frames_written += (uint64_t)frames;
  return frames;
}

/* ---------- read side ---------- */

/* Read raw bytes; return number of complete frames read. */
static size_t
caf_read_raw_frames(caf_private_t *p, int64_t frames) {
  if (!caf_scratch_resize(p, (size_t)frames)) return 0;
  size_t bps = (size_t)p->bytes_per_sample;
  size_t bytes_per_frame = bps * (size_t)p->num_channels;
  size_t got_bytes = fread(p->scratch, 1, bytes_per_frame * (size_t)frames, p->fp);
  return got_bytes / bytes_per_frame;
}

int64_t
_ambix_readf_int16_caf(ambix_t *ambix, int16_t *data, int64_t frames) {
  caf_private_t *p = PCAF(ambix);
  if (!p->fp || p->writing) return -1;
  size_t got = caf_read_raw_frames(p, frames);
  if (got == 0) return 0;
  size_t total = got * (size_t)p->num_channels;
  size_t bps = (size_t)p->bytes_per_sample;
  const uint8_t *src = p->scratch;

  if (p->is_float) {
    /* float → int16 (scale by 32768, round-to-nearest) */
    if (p->bits_per_sample == 32) {
      for (size_t i = 0; i < total; ++i) {
        uint32_t u = cafio_rd_u32(src + i*4);
        float f; memcpy(&f, &u, 4);
        data[i] = (int16_t)caf_round_clamp((double)f * 32768.0, 32767.0, -32768.0);
      }
    } else { /* 64 */
      for (size_t i = 0; i < total; ++i) {
        uint64_t u = cafio_rd_u64(src + i*8);
        double d; memcpy(&d, &u, 8);
        data[i] = (int16_t)caf_round_clamp(d * 32768.0, 32767.0, -32768.0);
      }
    }
  } else {
    /* PCM N → int16 (right-shift to fit) */
    int shift = p->bits_per_sample - 16;
    for (size_t i = 0; i < total; ++i) {
      int32_t v = caf_unpack_int_sample(src + i*bps, (int)bps);
      if (shift > 0) v >>= shift;
      if (v >  32767) v =  32767;
      if (v < -32768) v = -32768;
      data[i] = (int16_t)v;
    }
  }
  return (int64_t)got;
}

int64_t
_ambix_readf_int32_caf(ambix_t *ambix, int32_t *data, int64_t frames) {
  caf_private_t *p = PCAF(ambix);
  if (!p->fp || p->writing) return -1;
  size_t got = caf_read_raw_frames(p, frames);
  if (got == 0) return 0;
  size_t total = got * (size_t)p->num_channels;
  size_t bps = (size_t)p->bytes_per_sample;
  const uint8_t *src = p->scratch;

  if (p->is_float) {
    /* float → int32 (scale by 2^31, round-to-nearest) */
    if (p->bits_per_sample == 32) {
      for (size_t i = 0; i < total; ++i) {
        uint32_t u = cafio_rd_u32(src + i*4);
        float f; memcpy(&f, &u, 4);
        data[i] = caf_round_clamp((double)f * 2147483648.0, 2147483647.0, -2147483648.0);
      }
    } else {
      for (size_t i = 0; i < total; ++i) {
        uint64_t u = cafio_rd_u64(src + i*8);
        double d; memcpy(&d, &u, 8);
        data[i] = caf_round_clamp(d * 2147483648.0, 2147483647.0, -2147483648.0);
      }
    }
  } else {
    /* PCM N → int32 (left-shift to fill) */
    int shift = 32 - p->bits_per_sample;
    for (size_t i = 0; i < total; ++i) {
      int32_t v = caf_unpack_int_sample(src + i*bps, (int)bps);
      if (shift > 0) v <<= shift;
      data[i] = v;
    }
  }
  return (int64_t)got;
}

int64_t
_ambix_readf_float32_caf(ambix_t *ambix, float32_t *data, int64_t frames) {
  caf_private_t *p = PCAF(ambix);
  if (!p->fp || p->writing) return -1;
  size_t got = caf_read_raw_frames(p, frames);
  if (got == 0) return 0;
  size_t total = got * (size_t)p->num_channels;
  size_t bps = (size_t)p->bytes_per_sample;
  const uint8_t *src = p->scratch;

  if (p->is_float) {
    if (p->bits_per_sample == 32) {
      for (size_t i = 0; i < total; ++i) {
        uint32_t u = cafio_rd_u32(src + i*4);
        memcpy(&data[i], &u, 4);
      }
    } else {
      for (size_t i = 0; i < total; ++i) {
        uint64_t u = cafio_rd_u64(src + i*8);
        double d; memcpy(&d, &u, 8);
        data[i] = (float)d;
      }
    }
  } else {
    /* PCM N → float (scale by 1/2^(N-1)) */
    double inv = 1.0 / (double)(1u << (p->bits_per_sample - 1));
    for (size_t i = 0; i < total; ++i) {
      int32_t v = caf_unpack_int_sample(src + i*bps, (int)bps);
      data[i] = (float)((double)v * inv);
    }
  }
  return (int64_t)got;
}

int64_t
_ambix_readf_float64_caf(ambix_t *ambix, float64_t *data, int64_t frames) {
  caf_private_t *p = PCAF(ambix);
  if (!p->fp || p->writing) return -1;
  size_t got = caf_read_raw_frames(p, frames);
  if (got == 0) return 0;
  size_t total = got * (size_t)p->num_channels;
  size_t bps = (size_t)p->bytes_per_sample;
  const uint8_t *src = p->scratch;

  if (p->is_float) {
    if (p->bits_per_sample == 64) {
      for (size_t i = 0; i < total; ++i) {
        uint64_t u = cafio_rd_u64(src + i*8);
        memcpy(&data[i], &u, 8);
      }
    } else {
      for (size_t i = 0; i < total; ++i) {
        uint32_t u = cafio_rd_u32(src + i*4);
        float f; memcpy(&f, &u, 4);
        data[i] = (double)f;
      }
    }
  } else {
    double inv = 1.0 / (double)(1u << (p->bits_per_sample - 1));
    for (size_t i = 0; i < total; ++i) {
      int32_t v = caf_unpack_int_sample(src + i*bps, (int)bps);
      data[i] = (double)v * inv;
    }
  }
  return (int64_t)got;
}
