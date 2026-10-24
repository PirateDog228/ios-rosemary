#pragma once

/* Board-specific constants shared by the framebuffer, MMU, and test shim. */
#ifdef ROSEMARY
#define FB_BASE                 0x7bd20000UL
#define FB_SIZE                 0x022e0000UL
#define FB_W                    1080
#define FB_H                    2400
#define FB_STRIDE               (1088 * 4)

#define GICD_BASE               0x0c000000UL
#define GICR_BASE               0x0c040000UL
#define GICR_SIZE               0x200000UL
#define VTIMER_INTID            27
#define PTIMER_INTID            30
#else
#define FB_BASE                 0x9c000000UL
#define FB_SIZE                 0x00800000UL
#define FB_W                    1080
#define FB_H                    2400
#define FB_STRIDE               (FB_W * 4)

#define GICD_BASE               0x17a00000UL
#define GICR_BASE               0x17a60000UL
#define GICR_SIZE               0x100000UL
#define VTIMER_INTID            19
#define PTIMER_INTID            18
#endif
