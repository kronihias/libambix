#include "common.h"
void check_create_simple(const char*path, ambix_sampleformat_t format, float32_t eps);

int main(int argc, char**argv) {
  check_create_simple(FILENAME_FILE,  AMBIX_SAMPLEFORMAT_PCM16, 1./20000.);
  return pass();
}
