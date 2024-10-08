/*
 * Test at the boundary between small and large objects.
 * Inspired by a test case from Zoltan Varga.
 */

#include <stdio.h>
#include <stdlib.h>

#include "gc.h"

#define CHECK_OUT_OF_MEMORY(p) \
    do { \
        if (NULL == (p)) { \
            fprintf(stderr, "Out of memory\n"); \
            exit(69); \
        } \
    } while (0)

int main (void)
{
  int i;

  GC_set_all_interior_pointers(0);
  GC_INIT();
  if (GC_get_find_leak())
    printf("This test program is not designed for leak detection mode\n");

  for (i = 0; i < 20000; ++i) {
    CHECK_OUT_OF_MEMORY(GC_malloc_atomic(4096));
    CHECK_OUT_OF_MEMORY(GC_malloc(4096));
  }

  /* Test delayed start of marker threads, if they are enabled. */
  GC_start_mark_threads();

  for (i = 0; i < 20000; ++i) {
    CHECK_OUT_OF_MEMORY(GC_malloc_atomic(2048));
    CHECK_OUT_OF_MEMORY(GC_malloc(2048));
  }

  printf("Final heap size is %lu\n", (unsigned long)GC_get_heap_size());
  return 0;
}
