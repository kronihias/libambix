/* common test functionality

   Copyright © 2012-2016 IOhannes m zmölnig <zmoelnig@iem.at>.
         Institute of Electronic Music and Acoustics (IEM),
         University of Music and Dramatic Arts, Graz

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

#include "common.h"
#include <math.h>

static char* snprintdata(char*out, size_t size, ambixtest_presentationformat_t fmt, const void*data, uint64_t index) {
  switch(fmt) {
  case INT16  :
    snprintf(out, size, "%d", ((int16_t*)data)[index]);
    break;
  case INT32  :
    snprintf(out, size, "%d", ((int32_t*)data)[index]);
    break;
  case FLOAT32:
    snprintf(out, size, "%g", ((float32_t*)data)[index]);
    break;
  case FLOAT64:
    snprintf(out, size, "%g", ((float64_t*)data)[index]);
    break;
  default     :
    snprintf(out, size, "???");
    break;
  }
  out[size-1]=0;
  return out;
}

void matrix_print(const ambix_matrix_t*mtx) {
  printf("matrix[%p] ", mtx);
  printf(" [%dx%d]@%p\n", mtx->rows, mtx->cols, mtx->data);
  if(mtx->data) {
    uint32_t c, r;
    for(r=0; r<mtx->rows; r++) {
      for(c=0; c<mtx->cols; c++)
        printf(" %05f", mtx->data[r][c]);
      printf("\n");
    }
  }
}
#define MAX_OVER 10
float32_t matrix_diff(uint32_t line, const ambix_matrix_t*A, const ambix_matrix_t*B, float32_t eps) {
  uint32_t r, c;
  float32_t sum=0.;
  float32_t maxdiff=-1.f;
  uint64_t overcount=0;

  float32_t**a=NULL;
  float32_t**b=NULL;

  fail_if((NULL==A), line, "left-hand matrix of matrixdiff is NULL");
  fail_if((NULL==B), line, "right-hand matrix of matrixdiff is NULL");
  fail_if(((A->rows!=B->rows) || (A->cols!=B->cols)), line, "matrixdiff matrices do not match [%dx%d]!=[%dx%d]", A->rows, A->cols, B->rows, B->cols);

  a=A->data;
  b=B->data;
  for(r=0; r<A->rows; r++)
    for(c=0; c<B->cols; c++) {
      float32_t v=a[r][c]-b[r][c];
      float32_t vabs=(v<0)?-v:v;
      fail_if(isnan(v), line, "[%d|%d] is NaN: %f <-> %f", r, c, a[r][c], b[r][c]);
      if(vabs>maxdiff)
        maxdiff=vabs;
      sum+=vabs;
      if(vabs>eps) {
        overcount++;
        if(overcount<MAX_OVER)
          printf("%+f - %+f = %+g @ [%02d|%02d]\n", a[r][c], b[r][c], v, r, c);
      }
    }

  if(overcount>MAX_OVER)
    printf("[%d] accumulated error %f over %d/%d frames (eps=%g)\n", line, sum, (int)overcount, (int)(A->cols*B->rows), eps);
  return maxdiff;
}


float32_t data_diff(uint32_t line,
                    ambixtest_presentationformat_t fmt,
                    const void*A, const void*B, uint64_t frames,
                    float32_t eps) {
  uint64_t i;
  float32_t sum=0.;
  float32_t maxdiff=-1.f;
  uint64_t overcount=0;
  char aout[16];
  char bout[16];

  fail_if((NULL==A), line, "left-hand data of datadiff is NULL");
  fail_if((NULL==B), line, "right-hand data of datadiff is NULL");

  for(i=0; i<frames; i++) {
    float64_t v=0;
    float64_t vabs=0;
    switch(fmt) {
    case INT16  :
      v=((int16_t*)A)[i]-((int16_t*)B)[i];
      break;
    case INT32  :
      v=((int32_t*)A)[i]-((int32_t*)B)[i];
      break;
    case FLOAT32:
      v=((float32_t*)A)[i]-((float32_t*)B)[i];
      break;
    case FLOAT64:
      v=((float64_t*)A)[i]-((float64_t*)B)[i];
      break;
    default: break;
    }
    fail_if(isnan(v), line, "[%d] is NaN: %s <-> %s", i,
            snprintdata(aout, 16, fmt, A, i),
            snprintdata(bout, 16, fmt, B, i));
    vabs=(v<0)?-v:v;
    if(vabs>maxdiff)
      maxdiff=vabs;
    sum+=vabs;
    if(vabs>eps) {
      overcount++;
      if(overcount<MAX_OVER)
        printf("%s - %s=%g @ %d\n",
               snprintdata(aout, 16, fmt, A, i),
               snprintdata(bout, 16, fmt, B, i),
               v, (int)i);
    }

  }

  if(overcount>MAX_OVER)
    printf("[%d] accumulated error %f over %d/%d frames (eps=%g)\n", line, sum, (int)overcount, (int)frames, eps);

  return maxdiff;
}


void data_print(ambixtest_presentationformat_t fmt, const void*data, uint64_t frames) {
  uint64_t i;
  char out[16];
  for(i=0; i<frames; i++) {
    printf("%05d: %s\n", (int)i, snprintdata(out, 16, fmt, data, i));
  }
}

void data_transpose(float32_t*outdata, const float32_t*indata, uint32_t inrows, uint32_t incols) {
  uint32_t r, c;
  for(r=0; r<inrows; r++) {
    for(c=0; c<incols; c++) {
      uint32_t outoffset=inrows*c+r;
      uint32_t inoffset=incols*r+c;
      //      printf("writing %d [%f] to %d\n", inoffset, indata[inoffset], outoffset);
      outdata[outoffset]=indata[inoffset];
    }
  }
}

static void setdata(ambixtest_presentationformat_t fmt, void*data, uint64_t index, float64_t value) {
   switch(fmt) {
   case INT16  :
     ((int16_t*)data)[index]=(int16_t)(value*32768.);
     break;
   case INT32  :
     ((int32_t*)data)[index]=(int32_t)(value*2147483648.);
     break;
   case FLOAT32:
     ((float32_t*)data)[index]=(float32_t)value;
     break;
   case FLOAT64:
     ((float64_t*)data)[index]=(float64_t)value;
     break;
   default     : break;
   }
}

static int fmtsize(ambixtest_presentationformat_t fmt) {
   switch(fmt) {
   case INT16  : return 2;
   case INT32  : return 4;
   case FLOAT32: return 4;
   case FLOAT64: return 8;
   default     : break;
   }
   return 0;
}

static void*data_calloc(ambixtest_presentationformat_t fmt, uint64_t frames, uint32_t channels) {
  size_t nmemb=frames*channels*fmtsize(fmt)/sizeof(float64_t)+1;
  size_t size = sizeof(float64_t);
  void*data=calloc(nmemb, size);
  //printf("%dx%d[%d] allocated %d of %d bytes to %p\n", (int)frames, channels, fmt, (int)nmemb, (int)size, data);
  return data;
}

void*data_sine(ambixtest_presentationformat_t fmt, uint64_t frames, uint32_t channels, float32_t freq) {
  float32_t periods=freq/44100.;
  void*data=data_calloc(fmt, frames, channels);
  int64_t frame;
  for(frame=0; frame<frames; frame++) {
    int32_t chan;
    float64_t f=(float64_t)frame*periods;
    float64_t value=0.5*sinf(f);
    for(chan=0; chan<channels; chan++)
      setdata(fmt, data, f*channels+chan, value);
  }
  return data;
}

void*data_ramp(ambixtest_presentationformat_t fmt, uint64_t frames, uint32_t channels) {
  void*data=data_calloc(fmt, frames, channels);
  double increment=1./(double)frames;
  double value=0.;
  int64_t frame;
  for(frame=0; frame<frames; frame++) {
    int32_t chan;
    float32_t v32=value-0.5;
    value+=increment;
    for(chan=0; chan<channels; chan++)
      setdata(fmt, data, frame*channels+chan, v32);
  }
  return data;
}



