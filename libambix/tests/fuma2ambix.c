/* fuma2ambix - check the Furse-Malham <-> ambix conversion matrices against
   the ambiX specification.

   Copyright © 2011-2016 IOhannes m zmölnig <zmoelnig@iem.at>.
        Institute of Electronic Music and Acoustics (IEM),
        University of Music and Dramatic Arts, Graz

   This file is part of libambix

   libambix is free software: you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as published by
   the Free Software Foundation, either version 2.1 of the License, or
   (at your option) any later version.

   libambix is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this program; if not, see <http://www.gnu.org/licenses/>.

*/

/* The existing matrix tests only check that AMBIX_MATRIX_FUMA and
 * AMBIX_MATRIX_TO_FUMA invert each other. That holds for any pair of mutually
 * inverse matrices, so it stays green even when both are permuted the same
 * wrong way. These checks pin the forward matrix down against the
 * specification instead.
 *
 * Reference: C. Nachbar, F. Zotter, E. Deleflie, A. Sontacchi, "ambiX - A
 * Suggested Ambisonics Format", Ambisonics Symposium 2011, section 4.2.1:
 *
 *                     [ sqrt(2)  0  0  0 ]
 *   A_(B->ambiX)  =   [       0  0  1  0 ]     applied to [ W X Y Z ]
 *                     [       0  0  0  1 ]
 *                     [       0  1  0  0 ]
 */

#include "common.h"
#include <math.h>

static void check_entry(const ambix_matrix_t *mtx,
                        uint32_t row, uint32_t col, float32_t expected,
                        const char *name) {
  const float32_t eps = 1e-6;
  float32_t got;
  fail_if((row >= mtx->rows || col >= mtx->cols), __LINE__,
          "%s: [%d][%d] out of range for a %dx%d matrix",
          name, row, col, mtx->rows, mtx->cols);
  got = mtx->data[row][col];
  fail_if(!(fabs(got - expected) < eps), __LINE__,
          "%s: [%d][%d] is %f, expected %f", name, row, col, got, expected);
}

/* FuMa channel c must appear in ambix channel `acn` and nowhere else. */
static void check_routing(const ambix_matrix_t *mtx,
                          uint32_t col, uint32_t acn, float32_t gain,
                          const char *name) {
  uint32_t row;
  for(row = 0; row < mtx->rows; row++)
    check_entry(mtx, row, col, (row == acn) ? gain : 0.f, name);
}

static void test_first_order(void) {
  ambix_matrix_t *mtx = ambix_matrix_init(4, 4, NULL);
  STARTTEST("fuma2ambix[4x4]\n");
  skip_if(NULL == mtx, __LINE__, "couldn't create matrix");
  mtx = ambix_matrix_fill(mtx, AMBIX_MATRIX_FUMA);
  skip_if(NULL == mtx, __LINE__, "couldn't fill FuMa matrix");

  /* W X Y Z  ->  ACN0 ACN1 ACN2 ACN3, i.e. W Y Z X */
  check_routing(mtx, 0, 0, sqrt(2.), "fuma2ambix W");
  check_routing(mtx, 1, 3, 1.f,      "fuma2ambix X");
  check_routing(mtx, 2, 1, 1.f,      "fuma2ambix Y");
  check_routing(mtx, 3, 2, 1.f,      "fuma2ambix Z");

  ambix_matrix_destroy(mtx);
}

static void test_higher_orders(void) {
  /* FuMa order: W X Y Z R S T U V K L M N O P Q.
   * ACN = n*n + n + m, so the destinations are: */
  const uint32_t acn[16] = { 0, 3, 1, 2, 6, 7, 5, 8, 4, 12, 13, 11, 14, 10, 15, 9 };
  const float32_t sqrt3_4   = (float32_t)(sqrt(3.)/2.);
  const float32_t sqrt5_8   = (float32_t)(sqrt(5./2.)/2.);
  const float32_t sqrt32_45 = (float32_t)(4.*sqrt(2./5.)/3.);
  const float32_t sqrt5_9   = (float32_t)(sqrt(5.)/3.);
  /* maxN -> SN3D, in FuMa channel order */
  const float32_t gain[16] = {
    (float32_t)sqrt(2.),
    1.f, 1.f, 1.f,
    1.f, sqrt3_4, sqrt3_4, sqrt3_4, sqrt3_4,
    1.f, sqrt32_45, sqrt32_45, sqrt5_9, sqrt5_9, sqrt5_8, sqrt5_8
  };
  uint32_t col;
  ambix_matrix_t *mtx = ambix_matrix_init(16, 16, NULL);

  STARTTEST("fuma2ambix[16x16]\n");
  skip_if(NULL == mtx, __LINE__, "couldn't create matrix");
  mtx = ambix_matrix_fill(mtx, AMBIX_MATRIX_FUMA);
  skip_if(NULL == mtx, __LINE__, "couldn't fill FuMa matrix");

  for(col = 0; col < 16; col++)
    check_routing(mtx, col, acn[col], gain[col], "fuma2ambix");

  ambix_matrix_destroy(mtx);
}

/* The Ambisonics community agreed not to use the Condon-Shortley phase
 * (ambiX paper, sec. 1.4), and the specification's conversion matrix is
 * all-positive. */
static void test_no_negative_coefficients(void) {
  const uint32_t sizes[10] = { 1, 3, 4, 5, 6, 7, 8, 9, 11, 16 };
  const uint32_t rows_v[10] = { 1, 4, 4, 9, 9, 16, 16, 9, 16, 16 };
  uint32_t i, row, col;

  STARTTEST("fuma2ambix signs\n");
  for(i = 0; i < 10; i++) {
    ambix_matrix_t *mtx = ambix_matrix_init(rows_v[i], sizes[i], NULL);
    skip_if(NULL == mtx, __LINE__, "couldn't create matrix");
    mtx = ambix_matrix_fill(mtx, AMBIX_MATRIX_FUMA);
    skip_if(NULL == mtx, __LINE__, "couldn't fill FuMa[%dx%d]",
            rows_v[i], sizes[i]);
    for(row = 0; row < mtx->rows; row++)
      for(col = 0; col < mtx->cols; col++)
        fail_if((mtx->data[row][col] < 0.f), __LINE__,
                "FuMa[%dx%d][%d][%d] is negative (%f): no Condon-Shortley "
                "phase is expected", rows_v[i], sizes[i], row, col,
                mtx->data[row][col]);
    ambix_matrix_destroy(mtx);
  }
}

/* A reduced set must leave the components it does not carry silent, rather
 * than shifting its neighbours into their slots. */
static void test_reduced_sets(void) {
  ambix_matrix_t *mtx = ambix_matrix_init(4, 3, NULL);
  STARTTEST("fuma2ambix[4x3] (WXY)\n");
  skip_if(NULL == mtx, __LINE__, "couldn't create matrix");
  mtx = ambix_matrix_fill(mtx, AMBIX_MATRIX_FUMA);
  skip_if(NULL == mtx, __LINE__, "couldn't fill FuMa[4x3]");

  check_routing(mtx, 0, 0, sqrt(2.), "fuma2ambix[4x3] W");
  check_routing(mtx, 1, 3, 1.f,      "fuma2ambix[4x3] X");
  check_routing(mtx, 2, 1, 1.f,      "fuma2ambix[4x3] Y");
  /* Z (ACN2) is not carried: its whole row must be empty. */
  check_entry(mtx, 2, 0, 0.f, "fuma2ambix[4x3] ACN2");
  check_entry(mtx, 2, 1, 0.f, "fuma2ambix[4x3] ACN2");
  check_entry(mtx, 2, 2, 0.f, "fuma2ambix[4x3] ACN2");

  ambix_matrix_destroy(mtx);
}

/* And the reverse direction is the transpose of the forward one. */
static void test_reverse(void) {
  ambix_matrix_t *mtx = ambix_matrix_init(4, 4, NULL);
  STARTTEST("ambix2fuma[4x4]\n");
  skip_if(NULL == mtx, __LINE__, "couldn't create matrix");
  mtx = ambix_matrix_fill(mtx, AMBIX_MATRIX_TO_FUMA);
  skip_if(NULL == mtx, __LINE__, "couldn't fill TO_FUMA matrix");

  /* ACN0 ACN1 ACN2 ACN3 (W Y Z X) -> W X Y Z */
  check_routing(mtx, 0, 0, (float32_t)(1./sqrt(2.)), "ambix2fuma ACN0");
  check_routing(mtx, 1, 2, 1.f, "ambix2fuma ACN1");
  check_routing(mtx, 2, 3, 1.f, "ambix2fuma ACN2");
  check_routing(mtx, 3, 1, 1.f, "ambix2fuma ACN3");

  ambix_matrix_destroy(mtx);
}

int main(int argc, char **argv) {
  test_first_order();
  test_higher_orders();
  test_no_negative_coefficients();
  test_reduced_sets();
  test_reverse();

  pass();
  return 0;
}
