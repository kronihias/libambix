/* caf_io.c - shared CAF byte-level helpers (wrapper builder/parser)  -*- c -*-

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

#include "caf_io.h"

#include <stdlib.h>
#include <string.h>

/* ---- big-endian byte helpers ---- */

void cafio_be_u16(uint8_t *p, uint16_t v) {
  p[0]=(v>>8)&0xff; p[1]=v&0xff;
}
void cafio_be_u32(uint8_t *p, uint32_t v) {
  p[0]=(v>>24)&0xff; p[1]=(v>>16)&0xff; p[2]=(v>>8)&0xff; p[3]=v&0xff;
}
void cafio_be_u64(uint8_t *p, uint64_t v) {
  p[0]=(v>>56)&0xff; p[1]=(v>>48)&0xff; p[2]=(v>>40)&0xff; p[3]=(v>>32)&0xff;
  p[4]=(v>>24)&0xff; p[5]=(v>>16)&0xff; p[6]=(v>>8)&0xff;  p[7]=v&0xff;
}
void cafio_be_f64(uint8_t *p, double d) {
  uint64_t bits;
  memcpy(&bits, &d, 8);
  cafio_be_u64(p, bits);
}
uint16_t cafio_rd_u16(const uint8_t *p) {
  return ((uint16_t)p[0]<<8) | p[1];
}
uint32_t cafio_rd_u32(const uint8_t *p) {
  return ((uint32_t)p[0]<<24) | ((uint32_t)p[1]<<16) | ((uint32_t)p[2]<<8) | p[3];
}
uint64_t cafio_rd_u64(const uint8_t *p) {
  return ((uint64_t)p[0]<<56) | ((uint64_t)p[1]<<48) | ((uint64_t)p[2]<<40) | ((uint64_t)p[3]<<32)
       | ((uint64_t)p[4]<<24) | ((uint64_t)p[5]<<16) | ((uint64_t)p[6]<<8)  | p[7];
}
double cafio_rd_f64(const uint8_t *p) {
  uint64_t bits = cafio_rd_u64(p);
  double d;
  memcpy(&d, &bits, 8);
  return d;
}

/* ---- wrapper builder ----
 *
 * Layout:
 *   caff header (8)
 *   'desc' chunk (12 + 32) -- mandatory
 *   'uuid' chunk (12 + uuid_size) -- if uuid_data != NULL
 *   for each buffered chunk: (12 + chunk size)
 *   'data' chunk header (12) + mEditCount(4) -- audio bytes are NOT in wrapper
 */
uint8_t *
cafio_build_wrapper(const caf_wrapper_spec_t *p, uint32_t *out_size,
                    uint32_t *out_data_size_field_offset) {
  size_t total = 8 /* caff file header */;
  total += 12 + 32; /* desc */
  if (p->uuid_data && p->uuid_size > 0)
    total += 12 + (size_t)p->uuid_size;
  for (uint32_t i = 0; i < p->num_chunks; ++i)
    total += 12 + (size_t)p->chunks[i].size;
  total += 12 + 4; /* data chunk header + mEditCount */

  uint8_t *buf = (uint8_t*)malloc(total);
  if (!buf) { *out_size = 0; if (out_data_size_field_offset) *out_data_size_field_offset = 0; return NULL; }
  uint8_t *q = buf;

  /* CAF file header: "caff" + version=1 + flags=0 */
  memcpy(q, "caff", 4); q += 4;
  cafio_be_u16(q, 1); q += 2;
  cafio_be_u16(q, 0); q += 2;

  /* desc chunk */
  memcpy(q, "desc", 4); q += 4;
  cafio_be_u64(q, 32); q += 8;
  cafio_be_f64(q, (double)p->sample_rate); q += 8;
  memcpy(q, "lpcm", 4); q += 4;
  /* mFormatFlags: bit 0 = kCAFLinearPCMFormatFlagIsFloat, bit 1 = kCAFLinearPCMFormatFlagIsLittleEndian
     For ambix CAF we use big-endian (bit 1 = 0). */
  cafio_be_u32(q, p->is_float ? 0x01 : 0x00); q += 4;
  cafio_be_u32(q, (uint32_t)(p->bytes_per_sample * p->num_channels)); q += 4; /* mBytesPerPacket */
  cafio_be_u32(q, 1); q += 4; /* mFramesPerPacket */
  cafio_be_u32(q, p->num_channels); q += 4;
  cafio_be_u32(q, (uint32_t)p->bits_per_sample); q += 4;

  /* uuid chunk (if present) */
  if (p->uuid_data && p->uuid_size > 0) {
    memcpy(q, "uuid", 4); q += 4;
    cafio_be_u64(q, (uint64_t)p->uuid_size); q += 8;
    memcpy(q, p->uuid_data, (size_t)p->uuid_size); q += p->uuid_size;
  }

  /* buffered chunks */
  for (uint32_t i = 0; i < p->num_chunks; ++i) {
    memcpy(q, &p->chunks[i].id, 4); q += 4; /* id is already in CAF byte order */
    cafio_be_u64(q, (uint64_t)p->chunks[i].size); q += 8;
    if (p->chunks[i].size > 0)
      memcpy(q, p->chunks[i].data, (size_t)p->chunks[i].size);
    q += p->chunks[i].size;
  }

  /* data chunk: header + mEditCount=0; audio sample bytes are not in the wrapper */
  memcpy(q, "data", 4); q += 4;
  if (out_data_size_field_offset)
    *out_data_size_field_offset = (uint32_t)(q - buf);
  if (p->data_size_sentinel) {
    /* size = -1 (sentinel meaning "to end of file" per CAF spec) */
    cafio_be_u64(q, (uint64_t)(int64_t)-1);
  } else {
    /* placeholder; caller will patch at close */
    cafio_be_u64(q, 0);
  }
  q += 8;
  cafio_be_u32(q, 0); q += 4; /* mEditCount */

  *out_size = (uint32_t)(q - buf);
  return buf;
}

/* ---- wrapper parser ----
 *
 * Walks the wrapper bytes, populating audio-format fields and locating the
 * uuid chunk (if any). Stops at the 'data' chunk header.
 */
ambix_err_t
cafio_parse_wrapper(const uint8_t *bytes, uint32_t size, caf_parsed_t *out) {
  memset(out, 0, sizeof(*out));
  if (!bytes || size < 8 + 12 + 32) return AMBIX_ERR_INVALID_FILE;
  if (memcmp(bytes, "caff", 4) != 0) return AMBIX_ERR_INVALID_FILE;
  /* version = cafio_rd_u16(bytes+4); flags = cafio_rd_u16(bytes+6); — unused */
  const uint8_t *q   = bytes + 8;
  const uint8_t *end = bytes + size;
  while (q + 12 <= end) {
    char id[5]; memcpy(id, q, 4); id[4]=0;
    uint64_t sz = cafio_rd_u64(q + 4);
    q += 12;
    if (memcmp(id, "data", 4) == 0) {
      /* data chunk: only mEditCount(4) sits in the wrapper; audio is past it */
      break;
    }
    if (sz > (uint64_t)(end - q)) {
      /* truncated wrapper — consume rest as best effort */
      sz = (uint64_t)(end - q);
    }
    if (memcmp(id, "desc", 4) == 0 && sz >= 32) {
      double      sr            = cafio_rd_f64(q + 0);
      /* mFormatID at q+8 ('lpcm') — assumed */
      uint32_t    fmt_flags     = cafio_rd_u32(q + 12);
      /* mBytesPerPacket at q+16, mFramesPerPacket at q+20 — derivable */
      uint32_t    nch           = cafio_rd_u32(q + 24);
      uint32_t    bits_per_ch   = cafio_rd_u32(q + 28);
      out->sample_rate     = (uint32_t)sr;
      out->num_channels    = nch;
      out->bits_per_sample = (int)bits_per_ch;
      out->bytes_per_sample = (bits_per_ch + 7) / 8;
      out->is_float        = (fmt_flags & 0x01) ? 1 : 0;
    } else if (memcmp(id, "uuid", 4) == 0) {
      out->uuid_data = q;
      out->uuid_size = (int64_t)sz;
    }
    q += sz;
  }
  return AMBIX_ERR_SUCCESS;
}

/* ---- chunk lookup (read side) ---- */

void *
cafio_find_chunk(const uint8_t *bytes, uint32_t size,
                 uint32_t id, uint32_t chunk_it, int64_t *datasize) {
  *datasize = 0;
  if (!bytes || size < 8 + 12) return NULL;

  const uint8_t *q   = bytes + 8;
  const uint8_t *end = bytes + size;
  uint32_t hits = 0;
  while (q + 12 <= end) {
    uint32_t this_id;
    memcpy(&this_id, q, 4);
    uint64_t sz = cafio_rd_u64(q + 4);
    q += 12;
    if (memcmp(q - 12, "data", 4) == 0) break;
    if (sz > (uint64_t)(end - q)) sz = (uint64_t)(end - q);
    if (this_id == id) {
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
