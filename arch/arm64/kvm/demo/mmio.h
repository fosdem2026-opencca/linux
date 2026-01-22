#ifndef DEMO_MMIO_H
#define DEMO_MMIO_H

#include "ioctl.h"

#define GPU_GUEST_RAM_BASE 0x80000000ULL
#define GPU_GUEST_RAM_SIZE 0x20000000ULL
#define GPU_IDENTITY_MAP 1

static inline bool demo_ipa_in_identity(phys_addr_t ipa)
{	
	return GPU_IDENTITY_MAP && ipa >= GPU_GUEST_RAM_BASE &&
	       ipa <  GPU_GUEST_RAM_BASE + GPU_GUEST_RAM_SIZE &&
	       IS_ALIGNED(ipa, PAGE_SIZE);
}

static inline struct page *demo_identity_page(phys_addr_t ipa)
{
	/* IPA == PA here */
	
	struct page *p = pfn_to_page(ipa >> PAGE_SHIFT);
	return p;
}

int demo_is_kvm_vmfd(struct file *filp, unsigned int ioctl, unsigned long arg);

int demo_do_kvm_vmfd(struct kvm *kvm,
                                   struct file *filp,
                                   unsigned int ioctl,
                                   unsigned long arg);

#endif