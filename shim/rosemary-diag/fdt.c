/* Just enough FDT walking for the Rosemary diagnostic. */
#include "fdt.h"

#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4
#define FDT_END		9

uint32_t fdt32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

uint64_t fdt64(const uint8_t *p)
{
	return ((uint64_t)fdt32(p) << 32) | fdt32(p + 4);
}

static uint32_t be32(uintptr_t a)
{
	return fdt32((const uint8_t *)a);
}

uint32_t fdt_totalsize(uintptr_t fdt)
{
	return be32(fdt + 4);
}

int fdt_name_is(const char *name, const char *base)
{
	while (*base && *name == *base)
		name++, base++;
	return !*base && (*name == 0 || *name == '@');
}

static uintptr_t align4(uintptr_t a)
{
	return (a + 3) & ~3UL;
}

int fdt_walk(uintptr_t fdt, fdt_cb cb, void *ctx)
{
	if (!fdt || (fdt & 3) || be32(fdt) != FDT_MAGIC)
		return 0;
	uintptr_t p = fdt + be32(fdt + 8);		/* off_dt_struct */
	uintptr_t strings = fdt + be32(fdt + 12);	/* off_dt_strings */
	uintptr_t end = p + be32(fdt + 36);		/* size_dt_struct */
	const char *names[FDT_MAX_DEPTH];
	int depth = 0;

	while (p < end) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			uintptr_t q = p;
			while (*(const char *)q)
				q++;
			p = align4(q + 1);
			if (depth < FDT_MAX_DEPTH)
				names[depth] = name;
			depth++;
			if (depth <= FDT_MAX_DEPTH &&
			    cb(ctx, depth, names, 0, 0, 0))
				return 1;
			break;
		}
		case FDT_END_NODE:
			depth--;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p);
			uint32_t nameoff = be32(p + 4);
			const uint8_t *val = (const uint8_t *)(p + 8);
			p = align4(p + 8 + len);
			if (depth >= 1 && depth <= FDT_MAX_DEPTH &&
			    cb(ctx, depth, names, (const char *)(strings + nameoff), val, len))
				return 1;
			break;
		}
		case FDT_NOP:
			break;
		default:			/* FDT_END or garbage */
			return 1;
		}
	}
	return 1;
}
