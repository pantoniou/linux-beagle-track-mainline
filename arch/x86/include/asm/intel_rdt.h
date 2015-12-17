#ifndef _RDT_H_
#define _RDT_H_

#ifdef CONFIG_INTEL_RDT

struct clos_cbm_table {
	unsigned long l3_cbm;
	unsigned int clos_refcnt;
};

#endif
#endif
