/* caf_io.h - shared CAF byte-level helpers (wrapper builder/parser)  -*- c -*-

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
 * Shared between the native CAF backend (caf.c) and the WavPack backend
 * (wavpack.c, which carries an honest-to-CAF byte sequence as its wrapper).
 * Internal-only: AMBIX_INTERNAL must be defined.
 */

#ifndef AMBIX_CAF_IO_H
#define AMBIX_CAF_IO_H

#ifndef AMBIX_INTERNAL
# error caf_io.h must only be used from within libambix
#endif

#include <stdint.h>
#include <stddef.h>
#include <ambix/ambix.h>

/* ---- big-endian byte helpers ---- */

void     cafio_be_u16(uint8_t *p, uint16_t v);
void     cafio_be_u32(uint8_t *p, uint32_t v);
void     cafio_be_u64(uint8_t *p, uint64_t v);
void     cafio_be_f64(uint8_t *p, double   d);
uint16_t cafio_rd_u16(const uint8_t *p);
uint32_t cafio_rd_u32(const uint8_t *p);
uint64_t cafio_rd_u64(const uint8_t *p);
double   cafio_rd_f64(const uint8_t *p);

/* ---- buffered chunk list ---- */

typedef struct caf_chunk_s {
  uint32_t id;        /* 4cc bytes as written (memcpy of 4 ASCII chars) */
  uint8_t *data;
  int64_t  size;
} caf_chunk_t;

/* ---- wrapper builder ---- */

typedef struct caf_wrapper_spec_s {
  uint32_t sample_rate;
  uint32_t num_channels;
  int      bits_per_sample;     /* 16/24/32/64 */
  int      bytes_per_sample;    /* 2/3/4/8 */
  int      is_float;            /* 0=PCM, 1=float */
  /* Data chunk size policy:
   *   1 = write -1 sentinel ("audio extends to end of file"); used by WavPack
   *   0 = write 0 placeholder; caller patches the size field at close. */
  int      data_size_sentinel;
  const uint8_t *uuid_data;
  int64_t  uuid_size;
  const caf_chunk_t *chunks;
  uint32_t num_chunks;
} caf_wrapper_spec_t;

/* Build the bytes from caff header up to & including 'data' chunk header +
 * mEditCount (4 bytes). Audio sample bytes are NOT included.
 * Returns malloc()d buffer on success, sets *out_size; caller frees.
 * If out_data_size_field_offset != NULL, returns the offset (within the
 * returned buffer) of the 8-byte data-chunk size field, so the caller can
 * patch it later by seeking to that offset in the file. */
uint8_t *cafio_build_wrapper(const caf_wrapper_spec_t *spec,
                             uint32_t *out_size,
                             uint32_t *out_data_size_field_offset);

/* ---- wrapper parser ---- */

typedef struct caf_parsed_s {
  uint32_t sample_rate;
  uint32_t num_channels;
  int      bits_per_sample;
  int      bytes_per_sample;
  int      is_float;
  /* uuid chunk (pointer into source bytes; not owned) */
  const uint8_t *uuid_data;
  int64_t        uuid_size;
} caf_parsed_t;

/* Walk wrapper bytes (caff header + chunks up to 'data'), populate format
 * fields and locate uuid chunk (if any). Returns success even if no uuid
 * present; only fails on malformed magic / truncated header. */
ambix_err_t cafio_parse_wrapper(const uint8_t *bytes, uint32_t size,
                                caf_parsed_t *out);

/* Find chunk with the given 4cc id, returning a malloc()d copy of its
 * payload (caller frees). chunk_it selects which match (0 = first).
 * Stops at the 'data' chunk. Returns NULL and sets *datasize=0 if not found. */
void *cafio_find_chunk(const uint8_t *bytes, uint32_t size,
                       uint32_t id, uint32_t chunk_it, int64_t *datasize);

#endif /* AMBIX_CAF_IO_H */
