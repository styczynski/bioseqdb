#include <stdio.h>
#include "version.h"

int main(int argc, char **argv) {
  printf("Top secret: %s, version: %s\n", TOP_SECRET_KEY, VERSION);
  return 0;
}
