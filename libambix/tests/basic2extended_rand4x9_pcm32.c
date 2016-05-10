/* extended_float32_1024 - test ambix extended (PCM32, blocksize 1024)

   Copyright © 2012 IOhannes m zmölnig <zmoelnig@iem.at>.
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

float32_t data_4_9[]={
  0.519497, 0.101224, 0.775246, 0.219242, 0.795973, 0.649863, 0.190978, 0.837028, 0.763130,
  0.165074, 0.276581, 0.220167, 0.383229, 0.937749, 0.381838, 0.025107, 0.846256, 0.773257,
  0.546205, 0.501742, 0.476078, 0.539815, 0.671716, 0.069030, 0.748010, 0.369414, 0.667491,
  0.192167, 0.936164, 0.792496, 0.447073, 0.689901, 0.618242, 0.769460, 0.815128, 0.466140,
};

int check_create_b2e(const char*path, ambix_sampleformat_t format,
		     ambix_matrix_t*matrix, uint32_t extrachannels,
		     uint32_t chunksize, float32_t eps);
int test_datamatrix(const char*name, uint32_t rows, uint32_t cols, float32_t*data,
		    uint32_t xtrachannels, uint32_t chunksize, float32_t eps) {
  int result=0;
  ambix_matrix_t*mtx=0;
  STARTTEST("%s\n", name);
  mtx=ambix_matrix_init(rows,cols,mtx);
  if(!mtx)return 1;
  ambix_matrix_fill_data(mtx, data);
  result=check_create_b2e("test2-b2e-float32.caf", AMBIX_SAMPLEFORMAT_PCM32,
			  mtx,xtrachannels,
			  chunksize, eps);
  ambix_matrix_destroy(mtx);
  return result;
}

int main(int argc, char**argv) {
  int err=0;
  err+=test_datamatrix   ("'rand'[4x9]", 4, 9, data_4_9          , 0, 1024, 5e-7);
  pass();
  return 0;
}