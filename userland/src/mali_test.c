/*
 * mali_test for ios-rosemary: RedwoodMali service -> get_param, create_bo, mmap, submit a
 * "write value" job, wait for it, check the word. Panfrost ioctl structures as in hakios.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <IOKit/IOKitLib.h>

struct get_param { uint32_t param, pad; uint64_t value; };
struct create_bo { uint32_t size, flags, handle, pad; uint64_t offset; };
struct submit {
	uint64_t jc, in_syncs;
	uint32_t in_sync_count, out_sync;
	uint64_t bo_handles;
	uint32_t bo_handle_count, requirements;
};
struct wait_bo { uint32_t handle, pad; int64_t timeout_ns; };
struct gem_close { uint32_t handle, pad; };

static io_connect_t conn;

static kern_return_t call(uint32_t sel, void *d, size_t n)
{
	size_t r = n;
	return IOConnectCallStructMethod(conn, sel, d, n, d, &r);
}

int main(void)
{
	struct get_param p; struct create_bo bo; struct submit s; struct wait_bo w; struct gem_close c;
	mach_vm_address_t where = 0; mach_vm_size_t size = 0;
	volatile uint32_t *job; kern_return_t kr; uint32_t handles[1];
	io_service_t svc;

	setvbuf(stdout, NULL, _IONBF, 0);
	printf("mali_test: started\n");
	svc = IOServiceGetMatchingService(0, IOServiceMatching("RedwoodMali"));
	if (!svc) { printf("mali_test: no RedwoodMali service\n"); return 2; }
	kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
	IOObjectRelease(svc);
	if (kr) { printf("mali_test: open failed %#x\n", kr); return 3; }
	for (uint32_t i = 0; i < 3; i++) {
		memset(&p, 0, sizeof p); p.param = i;
		kr = call(0x44, &p, sizeof p);
		printf("mali_test: param %u = %#llx (%#x)\n", i, (unsigned long long)p.value, kr);
	}
	memset(&bo, 0, sizeof bo); bo.size = 4096;
	kr = call(0x42, &bo, sizeof bo);
	printf("mali_test: bo handle %u gpu %#llx (%#x)\n", bo.handle, (unsigned long long)bo.offset, kr);
	if (kr) return 4;
	kr = IOConnectMapMemory64(conn, bo.handle, mach_task_self(), &where, &size, kIOMapAnywhere);
	printf("mali_test: mapped %#llx size %llu (%#x)\n", (unsigned long long)where, (unsigned long long)size, kr);
	if (kr) return 5;

	job = (volatile uint32_t *)where;
	memset((void *)job, 0, 4096);
	job[4] = (2u << 1) | 1u | (1u << 16);
	job[8] = (uint32_t)(bo.offset + 0x800);
	job[9] = (uint32_t)((bo.offset + 0x800) >> 32);
	job[10] = 6;
	job[12] = 0x6d616c69u;
	job[0x200] = 0;
	__asm__ volatile ("dsb sy");

	handles[0] = bo.handle;
	memset(&s, 0, sizeof s);
	s.jc = bo.offset;
	s.bo_handles = (uint64_t)(uintptr_t)handles;
	s.bo_handle_count = 1;
	kr = call(0x40, &s, sizeof s);
	printf("mali_test: submit %#x (job seq %llu)\n", kr, (unsigned long long)s.jc);
	memset(&w, 0, sizeof w); w.handle = bo.handle; w.timeout_ns = 1000000000LL;
	kr = call(0x41, &w, sizeof w);
	printf("mali_test: wait %#x\n", kr);
	__asm__ volatile ("dsb sy");
	int ok = job[0x200] == 0x6d616c69u;
	printf("mali_test: word = %#x %s\n", job[0x200], ok ? "-- THE GPU DID IT FROM USER SPACE" : "-- not written");

	IOConnectUnmapMemory64(conn, bo.handle, mach_task_self(), where);
	c.handle = bo.handle; c.pad = 0; call(0x09, &c, sizeof c);
	IOServiceClose(conn);
	return ok ? 0 : 6;
}
