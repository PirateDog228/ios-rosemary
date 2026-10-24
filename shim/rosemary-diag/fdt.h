#pragma once
#include <stdint.h>

#define FDT_MAX_DEPTH 8

/*
 * Walk a flattened device tree. For every node start the callback gets
 * prop == 0; for every property prop/val/len are set (val is big-endian
 * data inside the blob). names[0..depth-1] are the node names from the
 * root (names[0] == ""), so names[depth-1] is the node being described.
 * A non-zero return stops the walk. Returns 0 if the blob is not an FDT.
 */
typedef int (*fdt_cb)(void *ctx, int depth, const char **names,
		      const char *prop, const uint8_t *val, uint32_t len);
int fdt_walk(uintptr_t fdt, fdt_cb cb, void *ctx);
uint32_t fdt_totalsize(uintptr_t fdt);

uint32_t fdt32(const uint8_t *p);		/* big-endian cell */
uint64_t fdt64(const uint8_t *p);		/* two cells */
/* true if the node name is "base" or "base@unit" */
int fdt_name_is(const char *name, const char *base);
