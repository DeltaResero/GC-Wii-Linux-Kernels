#ifndef _ASM_X86_K8_H
#define _ASM_X86_K8_H

#include <linux/ioport.h>
#include <linux/pci.h>

extern struct pci_device_id k8_nb_ids[];

extern int early_is_k8_nb(u32 value);
extern struct resource *amd_get_mmconfig_range(struct resource *res);
extern struct pci_dev **k8_northbridges;
extern int num_k8_northbridges;
extern int cache_k8_northbridges(void);
extern void k8_flush_garts(void);
extern int k8_scan_nodes(unsigned long start, unsigned long end);

#ifdef CONFIG_K8_NB
extern int num_k8_northbridges;

static inline struct pci_dev *node_to_k8_nb_misc(int node)
{
	return (node < num_k8_northbridges) ? k8_northbridges[node] : NULL;
}

#else
#define num_k8_northbridges 0

static inline struct pci_dev *node_to_k8_nb_misc(int node)
{
	return NULL;
}
#endif


#endif /* _ASM_X86_K8_H */
