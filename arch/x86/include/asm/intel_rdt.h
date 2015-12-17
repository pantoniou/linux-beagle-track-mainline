#ifndef _RDT_H_
#define _RDT_H_

#ifdef CONFIG_INTEL_RDT

#define MAX_CBM_LENGTH			32
#define IA32_L3_CBM_BASE		0xc90
#define CBM_FROM_INDEX(x)		(IA32_L3_CBM_BASE + x)

struct clos_cbm_table {
	unsigned long l3_cbm;
	unsigned int clos_refcnt;
};

#endif
#endif
