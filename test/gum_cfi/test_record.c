/* SPDX-License-Identifier: BSD-3-Clause */
#define _GNU_SOURCE
#include <frida-gum.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

enum { PAGE_COUNT = 3 };
static guint8 *mapping;
static gsize page_size;
static unsigned queries;

static gboolean count_query_protection(gconstpointer address,
                                       GumPageProtection *protection) {
  ++queries;
  return gum_memory_query_protection(address, protection);
}

/* Exercise the static production parser and count its real Gum queries. */
#define gum_memory_query_protection count_query_protection
#include "gum_peak_pc_api.c"
#undef gum_memory_query_protection

static void protect(unsigned page, int protection) {
  assert(mprotect(mapping + page * page_size, page_size, protection) == 0);
}

static void normal(guint8 *p, guint32 length) {
  memcpy(p, &length, sizeof(length));
}

static void extended(guint8 *p, guint64 length) {
  normal(p, UINT32_MAX);
  memcpy(p + 4, &length, sizeof(length));
}

static void check(const guint8 *p, gsize header, gsize length,
                  unsigned expected_queries) {
  PeakCfiCursor cursor = {0};
  guint offset_size = 0;
  queries = 0;
  assert(peak_cfi_record(p, &cursor, &offset_size) == (header != 0));
  assert(queries == expected_queries);
  if (header) {
    assert(cursor.ok && cursor.p == p + header && cursor.end == cursor.p + length);
    assert(offset_size == (header == 4 ? 4 : 8));
  } else {
    assert(!cursor.ok && !cursor.p && !cursor.end);
  }
}

int main(void) {
  gum_init_embedded();
  page_size = gum_query_page_size();
  assert(page_size > 32 && page_size <= UINT32_MAX / PAGE_COUNT);
  mapping = mmap(NULL, PAGE_COUNT * page_size, PROT_NONE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(mapping != MAP_FAILED);
  for (unsigned i = 0; i < PAGE_COUNT; ++i)
    protect(i, PROT_READ | PROT_WRITE);

  guint8 *edge = mapping + page_size;
  normal(mapping, 4);
  check(mapping, 4, 4, 1);
  extended(mapping, 8);
  check(mapping, 12, 8, 2);

  normal(edge - 8, 4);
  check(edge - 8, 4, 4, 1);
  extended(edge - 20, 8);
  check(edge - 20, 12, 8, 2);

  normal(edge - 8, 5);
  check(edge - 8, 4, 5, 3);
  normal(edge - 2, 4);
  check(edge - 2, 4, 4, 4);
  extended(edge - 20, 9);
  check(edge - 20, 12, 9, 4);
  extended(edge - 6, 8);
  check(edge - 6, 12, 8, 5);

  normal(mapping, 2 * page_size);
  check(mapping, 4, 2 * page_size, 4);
  protect(2, PROT_NONE);
  check(mapping, 0, 0, 4);

  normal(edge - 8, 5);
  protect(1, PROT_NONE);
  check(edge - 8, 0, 0, 3);
  protect(1, PROT_READ | PROT_WRITE);
  normal(edge - 2, 4);
  protect(1, PROT_NONE);
  check(edge - 2, 0, 0, 2);
  protect(1, PROT_READ | PROT_WRITE);
  extended(edge - 6, 8);
  protect(1, PROT_NONE);
  check(edge - 6, 0, 0, 3);
  extended(edge - 20, 8);
  check(edge - 20, 12, 8, 2);

  normal(mapping, 3);
  check(mapping, 0, 0, 1);
  extended(mapping, 7);
  check(mapping, 0, 0, 2);
  extended(mapping, UINT64_MAX);
  check(mapping, 0, 0, 2); /* Header size plus length wraps. */
  extended(mapping, UINTPTR_MAX - (guintptr)mapping - 11);
  check(mapping, 0, 0, 2); /* Record end address wraps. */
  check((void *)(UINTPTR_MAX - 2), 0, 0, 0); /* Header address wraps. */

  normal(mapping, 4);
  check(mapping, 4, 4, 1);
  protect(0, PROT_NONE);
  check(mapping, 0, 0, 1); /* Recheck at the same address. */

  assert(munmap(mapping, PAGE_COUNT * page_size) == 0);
  gum_deinit_embedded();
  return 0;
}
