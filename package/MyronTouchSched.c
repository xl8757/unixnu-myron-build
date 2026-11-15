// SPDX-License-Identifier: BSD-2-Clause-Patent
// The scheduler calls of MyronTouch.cpp. thread_bind() and master_processor are MACH_KERNEL_PRIVATE, out of reach of the
// IOKit C++ flags, so this file is built with the kernel's own C flags (those of osfmk startup.o), like GeniSpi.c.
#include <kern/processor.h>
#include <kern/sched_prim.h>
#include <kern/cpu_number.h>

void myron_touch_bind_boot_cpu(void);
int myron_touch_cpu(void);

// Binds the calling thread to the boot core and moves it there at once: the binding takes effect at the next
// thread_block(). XNU's own pattern (sched_vm_group_maintenance in osfmk/kern/sched_prim.c).
void myron_touch_bind_boot_cpu(void) {
    thread_bind(master_processor);
    thread_block(THREAD_CONTINUE_NULL);
}

int myron_touch_cpu(void) {return cpu_number();}
