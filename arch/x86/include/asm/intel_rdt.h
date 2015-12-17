#ifndef _RDT_H_
#define _RDT_H_

#ifdef CONFIG_INTEL_RDT

#include <linux/jump_label.h>

#define MAX_CBM_LENGTH			32
#define IA32_L3_CBM_BASE		0xc90
#define CBM_FROM_INDEX(x)		(IA32_L3_CBM_BASE + x)

extern struct static_key rdt_enable_key;
void __intel_rdt_sched_in(void *dummy);

struct clos_cbm_table {
	unsigned long l3_cbm;
	unsigned int clos_refcnt;
};

/*
 * intel_rdt_sched_in() - Writes the task's CLOSid to IA32_PQR_MSR
 *
 * Following considerations are made so that this has minimal impact
 * on scheduler hot path:
 * - This will stay as no-op unless we are running on an Intel SKU
 * which supports L3 cache allocation.
 * - Caches the per cpu CLOSid values and does the MSR write only
 * when a task with a different CLOSid is scheduled in.
 */
static inline void intel_rdt_sched_in(void)
{
	/*
	 * Call the schedule in code only when RDT is enabled.
	 */
	if (static_key_false(&rdt_enable_key))
		__intel_rdt_sched_in(NULL);
}

#else

static inline void intel_rdt_sched_in(void) {}

#endif
#endif
