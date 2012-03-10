/* adaptor.c -  extracting ambisonics data from data using adaptor matrices              -*- c -*-

   Copyright © 2012 IOhannes m zmölnig <zmoelnig@iem.at>.
         Institute of Electronic Music and Acoustics (IEM),
         University of Music and Dramatic Arts, Graz

   This file is part of libambix

   libambix is free software; you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as
   published by the Free Software Foundation; either version 2.1 of
   the License, or (at your option) any later version.

   Libgcrypt is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this program; if not, see <http://www.gnu.org/licenses/>.

*/

#include "private.h"

#ifdef HAVE_STDLIB_H
# include <stdlib.h>
#endif /* HAVE_STDLIB_H */

ambix_err_t _ambix_adaptorbuffer_resize(ambix_t*ambix, uint64_t frames, uint16_t itemsize) {
  uint32_t channels=ambix->info.ambichannels + ambix->info.otherchannels;

  uint64_t size=channels*frames*itemsize;

  if(frames<1 || channels<1)
    return AMBIX_ERR_SUCCESS;
  if(size<1)
    return AMBIX_ERR_UNKNOWN;

  if(size > ambix->adaptorbuffersize) {
    /* re-allocate memory! */
    ambix->adaptorbuffer=realloc(ambix->adaptorbuffer, size);
    if(ambix->adaptorbuffer)
      ambix->adaptorbuffersize=size;
    else {
      ambix->adaptorbuffersize=0;
      return AMBIX_ERR_UNKNOWN;
    }
  }
  return AMBIX_ERR_SUCCESS;

  //    DEFAULT_ADAPTORBUFFER_SIZE
  return AMBIX_ERR_UNKNOWN;
}

ambix_err_t _ambix_adaptorbuffer_destroy(ambix_t*ambix) {
  if(ambix->adaptorbuffer)
    free(ambix->adaptorbuffer);
  ambix->adaptorbuffer=NULL;
  ambix->adaptorbuffersize=0;
  return AMBIX_ERR_SUCCESS;
}


#define _AMBIX_ADAPTOR(type) \
  ambix_err_t _ambix_adaptor_##type(type##_t*source, uint32_t sourcechannels, uint32_t ambichannels, type##_t*dest_ambi, type##_t*dest_other, int64_t frames) { \
    int64_t frame;                                                      \
    for(frame=0; frame<frames; frame++) {                               \
      uint32_t chan;                                                    \
      for(chan=0; chan<ambichannels; chan++)                            \
        *dest_ambi++=*source++;                                         \
      for(chan=ambichannels; chan<sourcechannels; chan++)               \
        *dest_other++=*source++;                                        \
    }                                                                   \
    return AMBIX_ERR_SUCCESS;                                           \
  }

_AMBIX_ADAPTOR(float32);
_AMBIX_ADAPTOR(int32);
_AMBIX_ADAPTOR(int16);

ambix_err_t _ambix_adaptormatrix_float32(float32_t*source, uint32_t sourcechannels, ambixmatrix_t*matrix, float32_t*dest_ambi, float32_t*dest_other, int64_t frames) {
  float32_t**mtx=matrix->data;
  const uint32_t rows=matrix->rows;
  const uint32_t cols=matrix->cols;
  int64_t f;
  for(f=0; f<frames; f++) {
    uint32_t chan;
    for(chan=0; chan<rows; chan++) {
      float32_t*src=source;
      float32_t sum=0.;
      uint32_t c;
      for(c=0; c<cols; c++) {
        sum+=mtx[chan][c] * src[c];
      }
      *dest_ambi++=sum;
      source+=cols;
    }
    for(chan=cols; chan<sourcechannels; chan++)
      *dest_other++=*source++;
  }
  return AMBIX_ERR_UNKNOWN;
}
/* both _int16 and _int32 are highly unoptimized! */
/* LATER: add some fixed point magic to speed things up */
ambix_err_t _ambix_adaptormatrix_int16(int16_t*source, uint32_t sourcechannels, ambixmatrix_t*matrix, int16_t*dest_ambi, int16_t*dest_other, int64_t frames) {
  float32_t**mtx=matrix->data;
  const uint32_t rows=matrix->rows;
  const uint32_t cols=matrix->cols;
  int64_t f;
  for(f=0; f<frames; f++) {
    uint32_t chan;
    for(chan=0; chan<rows; chan++) {
      int16_t*src=source;
      float32_t sum=0.;
      uint32_t c;
      for(c=0; c<cols; c++) {
        sum+=mtx[chan][c] * src[c];
      }
      *dest_ambi++=sum;
      source+=cols;
    }
    for(chan=cols; chan<sourcechannels; chan++)
      *dest_other++=*source++;
  }
  return AMBIX_ERR_UNKNOWN;
}
ambix_err_t _ambix_adaptormatrix_int32(int32_t*source, uint32_t sourcechannels, ambixmatrix_t*matrix, int32_t*dest_ambi, int32_t*dest_other, int64_t frames) {
  float32_t**mtx=matrix->data;
  const uint32_t rows=matrix->rows;
  const uint32_t cols=matrix->cols;
  int64_t f;
  for(f=0; f<frames; f++) {
    uint32_t chan;
    for(chan=0; chan<rows; chan++) {
      int32_t*src=source;
      float32_t sum=0.;
      uint32_t c;
      for(c=0; c<cols; c++) {
        sum+=mtx[chan][c] * src[c];
      }
      *dest_ambi++=sum;
      source+=cols;
    }
    for(chan=cols; chan<sourcechannels; chan++)
      *dest_other++=*source++;
  }
  return AMBIX_ERR_UNKNOWN;
}