#ifndef KVM__DEMO_H
#define KVM__DEMO_H

struct demo_ioctl_req {
    int irq_virt;
    int irq_phy;
    int level_trigger ; // 1: level, 0: edge                                                                                                                   
    int attach; // 1: attach, 0: detach                                                                                                                        
};

struct demo_ioctl_enable {
    int enable;
};

#define DEMOIO 0xFF

#define DEMO_IOCTL_REQ _IOWR(DEMOIO, 1, struct demo_ioctl_req)
#define DEMO_IOCTL_ENABLE _IOWR(DEMOIO, 2, struct demo_ioctl_enable)

#define KVM_DEMO_IOREMAP      _IOWR(KVMIO, 0xfa, struct kvm_demo_ioremap)
#define DEMO_IOREMAP_REALM_NS (2 << 1)
struct kvm_demo_ioremap {
	unsigned long ipa;
	unsigned long pa;
	unsigned long size;
	unsigned long flags;
};


#endif