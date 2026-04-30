#include "common.h"
#include "data.h"

int main()
{
  ambix_t*ambix = NULL;
  void *sndfile = NULL;
  ambix_info_t *info= calloc(1, sizeof(ambix_info_t));

  ambix=ambix_open(AMBIXTEST_FILE1, AMBIX_READ, info);

  fail_if(NULL==ambix, __LINE__, "File was not open");
  sndfile = ambix_get_sndfile (ambix);

  /* libambix no longer wraps libsndfile — ambix_get_sndfile() must
   * always return NULL (kept as ABI-stable stub). */
  fail_if(NULL!=sndfile, __LINE__,
          "ambix_get_sndfile() must return NULL since libambix dropped libsndfile");

  ambix_close (ambix);
  free(info);
  return 0;
}
