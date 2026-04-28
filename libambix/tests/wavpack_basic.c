/* wavpack_basic.c - basic WavPack-backend round-trip tests for libambix

   Copyright © 2026 Matthias Kronlachner

   This file is part of libambix

   libambix is free software; you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as
   published by the Free Software Foundation; either version 2.1 of
   the License, or (at your option) any later version.

   libambix is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.
*/

#include "common.h"
#include <unistd.h>
#include <string.h>

/* Verify the file on disk really begins with the WavPack magic 'wvpk'. */
static void
fail_if_not_wavpack(const char *path) {
  unsigned char magic[4] = {0};
  FILE *fp = fopen(path, "rb");
  fail_if((NULL == fp), __LINE__, "could not reopen %s for magic-byte check", path);
  size_t got = fread(magic, 1, 4, fp);
  fclose(fp);
  fail_if((got < 4), __LINE__, "file %s shorter than 4 bytes", path);
  fail_if(!(magic[0]=='w' && magic[1]=='v' && magic[2]=='p' && magic[3]=='k'),
          __LINE__, "expected wvpk magic in %s, got %02x%02x%02x%02x",
          path, magic[0], magic[1], magic[2], magic[3]);
}

static void
roundtrip_basic(const char *path, ambix_sampleformat_t format, float32_t eps) {
  ambix_info_t info, rinfo;
  ambix_t *ambix = NULL;
  float32_t *orgdata, *data, *resultdata;
  uint32_t frames   = 16384;   /* keep tests quick */
  uint32_t channels = 4;       /* 1st-order ambix */
  float32_t periods = 137;
  int64_t err64;
  float32_t diff;
  uint32_t gotframes;

  printf("WavPack round-trip via '%s' [fmt=%d]\n", path, (int)format);

  resultdata = (float32_t*)calloc(channels * frames, sizeof(float32_t));
  data       = (float32_t*)calloc(channels * frames, sizeof(float32_t));

  memset(&info, 0, sizeof(info));
  info.fileformat   = AMBIX_BASIC;
  info.ambichannels = channels;
  info.extrachannels = 0;
  info.samplerate   = 44100;
  info.sampleformat = format;

  /* WRITE: explicitly request the WavPack backend */
  memcpy(&rinfo, &info, sizeof(info));
  ambix = ambix_open(path, AMBIX_WRITE | AMBIX_USE_WAVPACK, &rinfo);
  fail_if((NULL == ambix), __LINE__, "couldn't open %s for WavPack write", path);

  orgdata = data_sine(FLOAT32, frames, channels, periods);
  fail_if((NULL == orgdata), __LINE__, "couldn't generate test signal");
  memcpy(data, orgdata, frames * channels * sizeof(float32_t));

  err64 = ambix_writef_float32(ambix, data, NULL, frames);
  fail_if((err64 != frames), __LINE__, "wrote only %d of %d frames", (int)err64, (int)frames);

  fail_if((AMBIX_ERR_SUCCESS != ambix_close(ambix)), __LINE__, "closing WavPack ambix file");
  ambix = NULL;

  /* sanity: the file is actually WavPack on disk */
  fail_if_not_wavpack(path);

  /* READ: backend chosen by magic bytes */
  ambix = ambix_open(path, AMBIX_READ, &rinfo);
  fail_if((NULL == ambix), __LINE__, "couldn't reopen %s for read", path);

  fail_if((info.samplerate   != rinfo.samplerate),
          __LINE__, "samplerate mismatch: %g != %g", (float)info.samplerate, (float)rinfo.samplerate);
  fail_if((info.ambichannels != rinfo.ambichannels),
          __LINE__, "ambichannels mismatch: %u != %u", info.ambichannels, rinfo.ambichannels);

  gotframes = 0;
  do {
    err64 = ambix_readf_float32(ambix, resultdata + (gotframes * channels), NULL, (frames - gotframes));
    fail_if((err64 < 0), __LINE__, "read failed after %u/%u frames", gotframes, frames);
    if (err64 == 0) break;
    gotframes += (uint32_t)err64;
  } while (gotframes < frames);

  fail_if((gotframes != frames), __LINE__, "got %u of %u frames back", gotframes, frames);

  diff = data_diff(__LINE__, FLOAT32, orgdata, resultdata, frames * channels, eps);
  fail_if((diff > eps), __LINE__, "data diff %f > %f (lossless round-trip violated)", diff, eps);

  fail_if((AMBIX_ERR_SUCCESS != ambix_close(ambix)), __LINE__, "closing for read");
  ambix = NULL;

  free(data);
  free(resultdata);
  free(orgdata);
  ambixtest_rmfile(path);
}

int main(int argc, char **argv) {
  /* float32: lossless via reinterpretation */
  roundtrip_basic(FILENAME_FILE,  AMBIX_SAMPLEFORMAT_FLOAT32, 1e-7f);
  return pass();
}
