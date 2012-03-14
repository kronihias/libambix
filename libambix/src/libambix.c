/* libambix.c -  Ambisonics Xchange Library              -*- c -*-

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
#include <stdlib.h>
#endif /* HAVE_STDLIB_H */
#ifdef HAVE_STRING_H
# include <string.h>
#endif /* HAVE_STRING_H */


static ambix_err_t _check_write_ambixinfo(ambixinfo_t*info) {
  /* FIXXME: rather than failing, we could force the values to be correct */
  switch(info->fileformat) {
  case AMBIX_NONE:
    if(info->ambichannels>0)
      return AMBIX_ERR_INVALID_FORMAT;
    break;
  case AMBIX_SIMPLE:
    if(info->extrachannels>0)
      return AMBIX_ERR_INVALID_FORMAT;
    if(!ambix_isFullSet(info->ambichannels))
      return AMBIX_ERR_INVALID_FORMAT;
    break;
  }

  return AMBIX_ERR_SUCCESS;
}

static _ambix_info_set(ambix_t*ambix
                       , ambix_fileformat_t format
                       , int32_t otherchannels
                       , int32_t ambichannels
                       , int32_t fullambichannels
                       ) {
  switch(format) {
  case AMBIX_NONE:
    ambichannels=fullambichannels=0;
    break;
  case AMBIX_SIMPLE:
    otherchannels=0;
    break;
  default:
    break;
  }
  ambix->realinfo.fileformat=format;
  ambix->realinfo.ambichannels=ambichannels;
  ambix->realinfo.extrachannels=otherchannels;
  ambix->ambisonics_order==fullambichannels>0?ambix_channels2order(fullambichannels):0;
}

ambix_t* 	ambix_open	(const char *path, const ambix_filemode_t mode, ambixinfo_t*ambixinfo) {
  ambix_t*ambix=NULL;
  ambix_err_t err;
  int32_t ambichannels, otherchannels;

  if((AMBIX_WRITE & mode) && (AMBIX_READ & mode)) {
    /* RDRW not yet implemented */
    return NULL;
  }

  if(AMBIX_WRITE & mode) {
    err=_check_write_ambixinfo(ambixinfo);
    if(err!=AMBIX_ERR_SUCCESS)
      return NULL;
    ambichannels=ambixinfo->ambichannels;
    otherchannels=ambixinfo->extrachannels;
  }

  ambix=calloc(1, sizeof(ambix_t));
  if(AMBIX_ERR_SUCCESS == _ambix_open(ambix, path, mode, ambixinfo)) {
    const ambix_fileformat_t wantformat=ambixinfo->fileformat;
    ambix_fileformat_t haveformat;
    uint32_t channels = ambix->channels;
    /* successfully opened, initialize common stuff... */
    if(ambix->is_CAF) {
      if(AMBIX_WRITE & mode) {
        switch(wantformat) {
        case(AMBIX_NONE):
          _ambix_info_set(ambix, AMBIX_NONE, channels, 0, 0);
          break;
        case(AMBIX_SIMPLE):
          _ambix_info_set(ambix, AMBIX_SIMPLE, 0, channels, channels);
          break;
        case(AMBIX_EXTENDED):
          /* the number of full channels is not clear yet!
           * the user has to call setAdaptorMatrix() first */
          _ambix_info_set(ambix, AMBIX_EXTENDED, otherchannels, ambichannels, 0);
          break;
        }
      } else {
        if(ambix->has_UUID) {
          /* check whether channels are (N+1)^2
           * if so, we have a simple-ambix file, else it is just an ordinary caf
           */
          if(ambix->matrix.cols <= channels &&  /* reduced set must be fully present */
             ambix_isFullSet(ambix->matrix.rows)) { /* expanded set must be a full set */
            /* it's a simple AMBIX! */
            _ambix_info_set(ambix, AMBIX_EXTENDED, channels-ambix->matrix.cols, ambix->matrix.cols, ambix->matrix.rows);
          } else {
            /* ouch! matrix is not valid! */
            _ambix_info_set(ambix, AMBIX_NONE, channels, 0, 0);
          }
        } else {
          /* no uuid chunk found, it's probably a SIMPLE ambix file */

          /* check whether channels are (N+1)^2
           * if so, we have a simple-ambix file, else it is just an ordinary caf
           */
          if(ambix_isFullSet(channels)) { /* expanded set must be a full set */
            /* it's a simple AMBIX! */
            _ambix_info_set(ambix, AMBIX_SIMPLE, 0, channels, channels);
          } else {
            /* it's an ordinary CAF file */
            _ambix_info_set(ambix, AMBIX_NONE, channels, 0, 0);
          }
        }
      }
    } else {
      /* it's not a CAF file.... */
      _ambix_info_set(ambix, AMBIX_NONE, channels, 0, 0);
    }

    haveformat=ambix->realinfo.fileformat;

    ambix->filemode=mode;
    memcpy(&ambix->info, &ambix->realinfo, sizeof(ambix->info));

    if(0) {
    } else if(AMBIX_SIMPLE==wantformat && AMBIX_EXTENDED==haveformat) {
      ambix->info.fileformat=AMBIX_SIMPLE;
      ambix->use_matrix=1;
    } else if(AMBIX_EXTENDED==wantformat && AMBIX_SIMPLE==haveformat) {
      ambix_matrix_init(ambix->realinfo.ambichannels, ambix->realinfo.ambichannels, &ambix->matrix);
      ambix_matrix_eye(&ambix->matrix);
      ambix->info.fileformat=AMBIX_EXTENDED;
      ambix->use_matrix=0;
    }

    memcpy(ambixinfo, &ambix->info, sizeof(ambix->info));

    if(_ambix_adaptorbuffer_resize(ambix, DEFAULT_ADAPTORBUFFER_SIZE, sizeof(float32_t)) == AMBIX_ERR_SUCCESS)
      return ambix;
  }

  ambix_close(ambix);
  return NULL;
}

ambix_err_t	ambix_close	(ambix_t*ambix) {
  int res=AMBIX_ERR_SUCCESS;
  if(NULL==ambix) {
    return AMBIX_ERR_INVALID_HANDLE;
  }
  res=_ambix_close(ambix);

  _ambix_adaptorbuffer_destroy(ambix);
  ambix_matrix_deinit(&ambix->matrix);
  ambix_matrix_deinit(&ambix->matrix2);

  free(ambix);
  ambix=NULL;
  return res;
}

SNDFILE*ambix_getSndfile	(ambix_t*ambix) {
  return _ambix_get_sndfile(ambix);
}


const ambixmatrix_t*ambix_getAdaptorMatrix	(ambix_t*ambix) {
  if(AMBIX_EXTENDED==ambix->info.fileformat)
    return &(ambix->matrix);
  return NULL;
}
ambix_err_t ambix_setAdaptorMatrix	(ambix_t*ambix, const ambixmatrix_t*matrix) {
  if(0) {
  } else if((ambix->filemode & AMBIX_READ ) && (AMBIX_SIMPLE   == ambix->info.fileformat)) {
    /* multiply the matrix with the previous adaptor matrix */
    if(AMBIX_EXTENDED == ambix->realinfo.fileformat) {
      ambixmatrix_t*mtx=ambix_matrix_multiply(matrix, &ambix->matrix, &ambix->matrix2);
      if(mtx != &ambix->matrix2)
        return AMBIX_ERR_UNKNOWN;
      ambix->use_matrix=2;
      return AMBIX_ERR_SUCCESS;
    } else {
      ambixmatrix_t*mtx=NULL;
      if(matrix->cols != ambix->realinfo.ambichannels) {
        return AMBIX_ERR_INVALID_DIMENSION;
      }
      mtx=ambix_matrix_copy(matrix, &ambix->matrix2);
      ambix->use_matrix=2;
    }
  } else if((ambix->filemode & AMBIX_WRITE) && (AMBIX_EXTENDED == ambix->info.fileformat)) {
    /* too late, writing started already */
    if(ambix->startedWriting)
      return AMBIX_ERR_UNKNOWN;

    /* check whether the matrix will expand to a full set */
    if(!ambix_isFullSet(matrix->rows))
      return AMBIX_ERR_INVALID_DIMENSION;

    if(!ambix_matrix_copy(matrix, &ambix->matrix))
      return AMBIX_ERR_UNKNOWN;
    /* ready to write it to file */
    return AMBIX_ERR_SUCCESS;
  }

  return AMBIX_ERR_UNKNOWN;
}

ambix_err_t	ambix_write_header	(ambix_t*ambix) {
  void*data=NULL;
  if(ambix->filemode & AMBIX_WRITE) {
    if((AMBIX_EXTENDED == ambix->info.fileformat)) {
      ambix_err_t res;
      /* generate UUID-chunk */
      uint64_t datalen=_ambix_matrix_to_uuid1(&ambix->matrix, NULL, ambix->byteswap);
      uint64_t usedlen=1+datalen/sizeof(float32_t);

      if(datalen<1)
        return AMBIX_ERR_UNKNOWN;

      data=calloc(usedlen, sizeof(float32_t));
      if(_ambix_matrix_to_uuid1(&ambix->matrix, data, ambix->byteswap)!=datalen)
        goto cleanup;

      /* and write it to file */
      res=_ambix_write_uuidchunk(ambix, data, datalen);
      if(data)
        free(data);
      return res;
    }
    return AMBIX_ERR_INVALID_FORMAT;
  } else
    return AMBIX_ERR_INVALID_FILE;

 cleanup:
  if(data)
    free(data);
  return AMBIX_ERR_UNKNOWN;
}

static ambix_err_t _ambix_check_write(ambix_t*ambix, const void*ambidata, const void*otherdata, int64_t frames) {
  /* TODO: add some checks whether writing is feasible
   * e.g. format=extended but no (or wrong) matrix present */
  if((ambix->realinfo.fileformat==AMBIX_EXTENDED) && !ambix_isFullSet(ambix->matrix.rows))
    return AMBIX_ERR_INVALID_DIMENSION;

  ambix->startedWriting=1;
  return AMBIX_ERR_SUCCESS;
}

static ambix_err_t _ambix_check_read(ambix_t*ambix, const void*ambidata, const void*otherdata, int64_t frames) {
  /* TODO: add some checks whether writing is feasible
   * e.g. format=extended but no (or wrong) matrix present */
  ambix->startedReading=1;
  return AMBIX_ERR_SUCCESS;
}


#define AMBIX_READF(type) \
  int64_t ambix_readf_##type (ambix_t*ambix, type##_t*ambidata, type##_t*otherdata, int64_t frames) { \
    int64_t realframes;                                                 \
    type##_t*adaptorbuffer;                                             \
    ambix_err_t err= _ambix_check_read(ambix, (const void*)ambidata, (const void*)otherdata, frames); \
    if(AMBIX_ERR_SUCCESS != err) return -err;                           \
    _ambix_adaptorbuffer_resize(ambix, frames, sizeof(type##_t));       \
    adaptorbuffer=(type##_t*)ambix->adaptorbuffer;                      \
    realframes=_ambix_readf_##type(ambix, adaptorbuffer, frames);       \
    switch(ambix->use_matrix) {                                         \
    case 1:                                                             \
      _ambix_splitAdaptormatrix_##type(adaptorbuffer, ambix->info.ambichannels+ambix->info.extrachannels, &ambix->matrix          , ambidata, otherdata, realframes); \
      break;                                                            \
    case 2:                                                             \
      _ambix_splitAdaptormatrix_##type(adaptorbuffer, ambix->info.ambichannels+ambix->info.extrachannels, &ambix->matrix2         , ambidata, otherdata, realframes); \
      break;                                                            \
    default:                                                            \
      _ambix_splitAdaptor_##type      (adaptorbuffer, ambix->info.ambichannels+ambix->info.extrachannels, ambix->info.ambichannels, ambidata, otherdata, realframes); \
    };                                                                  \
    return realframes;                                                  \
  }

#define AMBIX_WRITEF(type) \
  int64_t ambix_writef_##type (ambix_t*ambix, type##_t *ambidata, type##_t*otherdata, int64_t frames) { \
    type##_t*adaptorbuffer;                                             \
    ambix_err_t err= _ambix_check_write(ambix, (const void*)ambidata, (const void*)otherdata, frames); \
    if(AMBIX_ERR_SUCCESS != err) return -err;                           \
    _ambix_adaptorbuffer_resize(ambix, frames, sizeof(type##_t));       \
    adaptorbuffer=(type##_t*)ambix->adaptorbuffer;                      \
    _ambix_mergeAdaptor_##type(ambidata, ambix->info.ambichannels, otherdata, ambix->info.extrachannels, adaptorbuffer, frames); \
    return _ambix_writef_##type(ambix, adaptorbuffer, frames);          \
  }


AMBIX_READF(int16);

AMBIX_READF(int32);

AMBIX_READF(float32);

AMBIX_WRITEF(int16);

AMBIX_WRITEF(int32);

AMBIX_WRITEF(float32);
