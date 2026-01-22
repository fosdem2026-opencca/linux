/*
 * Demo IRQ router
 * To convert phy irq of gpu into
 * virtual irqs and inject them into CVM.
 */
#ifndef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#endif

#include <asm/ioctl.h>

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/debugfs.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/kvm_host.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/ratelimit.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/workqueue.h>

#include "irq.h"

static bool debug;
module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "Enable debug");

#define PIN_SPI_CORE 1

#define LEVEL_TRIGGERED 1
#define EDGE_TRIGGERED 0

#define demo_dbg(fmt, ...)                                                     \
    do {                                                                       \
        if (debug)                                                             \
            pr_debug(fmt, ##__VA_ARGS__);                                      \
    } while (0)

#define demo_info(fmt, ...) pr_info(fmt, ##__VA_ARGS__)
#define demo_err(fmt, ...) pr_err(fmt, ##__VA_ARGS__)

struct demo_irq_context
{
    int phy_irq;
    int virt_irq;
    int level; /* 1 level, 0 edge */
    int phy_irq_masked;
    struct kvm_irq_ack_notifier* irq_ack_desc;
    unsigned long count;
};

struct demo_irq_work_data
{
    struct work_struct work;
    int level_signalize; /* 1=assert, 0=deassert */
    struct demo_irq_context* irq;
};

#define demo_context_irqs_size 2048

struct demo_context
{
    struct kvm* kvm;
    spinlock_t lock;
    struct dentry* debugfs_dir;
    struct demo_irq_context*
      irqs[demo_context_irqs_size]; /* indexed by virt_irq (gsi+32) */
    struct workqueue_struct* virt_inj_wq;
} demo_context;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
#define KPROBE_KALLSYMS_LOOKUP 1
typedef unsigned long (*kallsyms_lookup_name_t)(const char* name);
static kallsyms_lookup_name_t kallsyms_lookup_name_func;
#define kallsyms_lookup_name kallsyms_lookup_name_func
static struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };
#endif

static struct list_head* _vm_list;

typedef int (*kvm_vgic_inject_irq_t)(struct kvm* kvm,
                                     struct kvm_vcpu* vcpu,
                                     unsigned int intid,
                                     bool level,
                                     void* owner);
static kvm_vgic_inject_irq_t kvm_vgic_inject_irq_func;

typedef void (*kvm_register_irq_ack_notifier_t)(
  struct kvm* kvm,
  struct kvm_irq_ack_notifier* kian);
static kvm_register_irq_ack_notifier_t kvm_register_irq_ack_notifier_func;

typedef void (*kvm_unregister_irq_ack_notifier_t)(
  struct kvm* kvm,
  struct kvm_irq_ack_notifier* kian);
static kvm_unregister_irq_ack_notifier_t kvm_unregister_irq_ack_notifier_func;

static int
kprobe_init(void)
{
#ifdef KPROBE_KALLSYMS_LOOKUP
    int ret = register_kprobe(&kp);
    if (ret) {
        demo_err("register_kprobe(kallsyms_lookup_name) failed: %d\n", ret);
        return ret;
    }
    kallsyms_lookup_name = (kallsyms_lookup_name_t)kp.addr;
    unregister_kprobe(&kp);

    if (!kallsyms_lookup_name) {
        demo_err("Could not retrieve kallsyms_lookup_name address\n");
        return -ENXIO;
    }
#endif

    _vm_list = (void*)kallsyms_lookup_name("vm_list");
    if (!_vm_list) {
        demo_err("lookup failed: vm_list\n");
        return -ENXIO;
    }

    kvm_vgic_inject_irq_func =
      (void*)kallsyms_lookup_name("kvm_vgic_inject_irq");
    if (!kvm_vgic_inject_irq_func) {
        demo_err("lookup failed: kvm_vgic_inject_irq\n");
        return -ENXIO;
    }

    kvm_register_irq_ack_notifier_func =
      (void*)kallsyms_lookup_name("kvm_register_irq_ack_notifier");
    if (!kvm_register_irq_ack_notifier_func) {
        demo_err("lookup failed: kvm_register_irq_ack_notifier\n");
        return -ENXIO;
    }

    kvm_unregister_irq_ack_notifier_func =
      (void*)kallsyms_lookup_name("kvm_unregister_irq_ack_notifier");
    if (!kvm_unregister_irq_ack_notifier_func) {
        demo_err("lookup failed: kvm_unregister_irq_ack_notifier\n");
        return -ENXIO;
    }

    return 0;
}

static int
pin_spi_to_core(int phy_irq /* linux irq */, int target_cpu)
{
    cpumask_t cpumask;
    int ret;

    cpumask_clear(&cpumask);
    cpumask_set_cpu(target_cpu, &cpumask);

    ret = irq_set_affinity(phy_irq, &cpumask);
    if (ret < 0) {
        demo_err(
          "pin irq %d to core %d failed: %d\n", phy_irq, target_cpu, ret);
        return ret;
    }

    demo_dbg("pinned irq %d to core %d\n", phy_irq, target_cpu);
    return 0;
}

static int
to_linux_irq(unsigned int gic_id)
{
    struct device_node* gic_np;
    struct irq_domain* domain;

    gic_np = of_find_compatible_node(NULL, NULL, "arm,gic-v3");
    if (!gic_np) {
        demo_err("No GICv3 node found\n");
        return -ENODEV;
    }

    domain = irq_find_host(gic_np);
    of_node_put(gic_np);

    if (!domain) {
        demo_err("No irqdomain for GICv3\n");
        return -ENODEV;
    }

    return irq_create_mapping(domain, gic_id);
}

static int
inject_virt_irq(int virt_irq, int level_up)
{
    if (unlikely(!demo_context.kvm)) {
        demo_dbg("inject_virt_irq called without KVM instance (virt=%d)\n",
                 virt_irq);
        return -ENODEV;
    }

    demo_dbg("kvm_vgic_inject_irq: virt=%d level=%d\n", virt_irq, level_up);
    return kvm_vgic_inject_irq_func(
      demo_context.kvm, NULL, virt_irq, level_up, NULL);
}

static void
virt_irq_worker(struct work_struct* work)
{
    struct demo_irq_work_data* work_data =
      container_of(work, struct demo_irq_work_data, work);
    struct demo_irq_context* irq = work_data->irq;

    if (unlikely(!demo_context.kvm)) {
        kfree(work_data);
        return;
    }

    mutex_lock(&demo_context.kvm->lock);
    inject_virt_irq(irq->virt_irq, work_data->level_signalize);
    mutex_unlock(&demo_context.kvm->lock);

    kfree(work_data);
}

static void
enqueue_virt_irq_work(struct demo_irq_context* ctx, int level_signalize)
{
    struct demo_irq_work_data* work_data;

    work_data = kmalloc(sizeof(*work_data), GFP_ATOMIC);
    if (!work_data) {
        demo_err("alloc work_data failed\n");
        return;
    }

    work_data->irq = ctx;
    work_data->level_signalize = level_signalize;
    INIT_WORK(&work_data->work, virt_irq_worker);
    queue_work(demo_context.virt_inj_wq, &work_data->work);
}

static irqreturn_t
irq_handler_edge(int irq, void* dev_id)
{
    struct demo_irq_context* ctx = dev_id;

    if (unlikely(!ctx))
        return IRQ_NONE;

    /* keep some visibility without log spam */
    if ((ctx->count % 3000) == 0)
        demo_dbg("edge: %d -> %d (count=%lu)\n",
                 ctx->phy_irq,
                 ctx->virt_irq,
                 ctx->count);

    ctx->count++;
    enqueue_virt_irq_work(ctx, 1);
    return IRQ_HANDLED;
}

static irqreturn_t
irq_handler_level(int irq, void* dev_id)
{
    struct demo_irq_context* ctx = dev_id;

    if (unlikely(!ctx))
        return IRQ_NONE;

    ctx->count++;

    /* Level-triggered: mask physical IRQ until guest EOIs */
    if (!ctx->phy_irq_masked) {
        disable_irq_nosync(ctx->phy_irq);
        ctx->phy_irq_masked = 1;

        demo_dbg("level: masked phy=%d virt=%d (count=%lu)\n",
                 ctx->phy_irq,
                 ctx->virt_irq,
                 ctx->count);

        enqueue_virt_irq_work(ctx, 1);
        return IRQ_HANDLED;
    }

    return IRQ_HANDLED;
}

static int
phy_irq_unmask(struct demo_irq_context* irq_ctx)
{
    if (irq_ctx->phy_irq_masked) {
        irq_ctx->phy_irq_masked = 0;
        demo_dbg("unmask phy_irq %d\n", irq_ctx->phy_irq);
        enable_irq(irq_ctx->phy_irq);
        return 0;
    }

    demo_dbg("unmask called but phy_irq already unmasked: %d\n",
             irq_ctx->phy_irq);
    return 0;
}

static void
kvm_irq_acked_callback(struct kvm_irq_ack_notifier* kian)
{
    int virt_irq = kian->gsi + 32;
    struct demo_irq_context* irq_context;

    demo_dbg("kvm_irq_acked: virt=%d gsi=%d\n", virt_irq, kian->gsi);

    if (demo_context.kvm) {
        mutex_lock(&demo_context.kvm->lock);
        inject_virt_irq(virt_irq, 0);
        mutex_unlock(&demo_context.kvm->lock);
    }

    irq_context = demo_context.irqs[virt_irq];
    if (unlikely(!irq_context)) {
        demo_err("ack for unknown virt_irq=%d\n", virt_irq);
        return;
    }

    phy_irq_unmask(irq_context);
}

static int
register_eoi_notifier(int virt_irq, struct kvm_irq_ack_notifier** ret_desc)
{
    struct kvm_irq_ack_notifier* desc;

    if (unlikely(!demo_context.kvm))
        return -ENODEV;

    desc = kmalloc(sizeof(*desc), GFP_KERNEL);
    if (!desc) {
        demo_err("alloc kvm_irq_ack_notifier failed\n");
        return -ENOMEM;
    }

    memset(desc, 0, sizeof(*desc));
    desc->gsi = virt_irq - 32;
    desc->irq_acked = kvm_irq_acked_callback;

    demo_dbg("register ack notifier: virt=%d gsi=%d\n", virt_irq, desc->gsi);
    kvm_register_irq_ack_notifier_func(demo_context.kvm, desc);

    *ret_desc = desc;
    return 0;
}

static int
register_eoi_notifiers(void)
{
    ssize_t i;

    for (i = 0; i < demo_context_irqs_size; i++) {
        struct demo_irq_context* irq_context = demo_context.irqs[i];
        if (!irq_context)
            continue;

        if (irq_context->level == LEVEL_TRIGGERED) {
            /* only needed for level-triggered */
            register_eoi_notifier(irq_context->virt_irq,
                                  &irq_context->irq_ack_desc);
        }
    }

    return 0;
}

static int
unregister_eoi_notifiers(void)
{
    ssize_t i;

    for (i = 0; i < demo_context_irqs_size; i++) {
        struct demo_irq_context* irq_context = demo_context.irqs[i];
        if (!irq_context)
            continue;

        if (irq_context->level == LEVEL_TRIGGERED &&
            irq_context->irq_ack_desc) {
            demo_dbg("unregister ack notifier: %d -> %d\n",
                     irq_context->phy_irq,
                     irq_context->virt_irq);

            kvm_unregister_irq_ack_notifier_func(demo_context.kvm,
                                                 irq_context->irq_ack_desc);
            kfree(irq_context->irq_ack_desc);
            irq_context->irq_ack_desc = NULL;
        }

        kfree(irq_context);
        demo_context.irqs[i] = NULL;
    }

    return 0;
}

static void
clean_up_irq_handlers(void)
{
    ssize_t i;

    for (i = 0; i < demo_context_irqs_size; i++) {
        struct demo_irq_context* irq_context = demo_context.irqs[i];
        if (!irq_context)
            continue;

        demo_dbg("free_irq phy_irq=%d\n", irq_context->phy_irq);
        free_irq(irq_context->phy_irq, (void*)irq_context);
    }
}

static int
load_kvm_instance(void)
{
    struct kvm* kvm;

    kvm = list_first_entry_or_null(_vm_list, struct kvm, vm_list);
    if (!kvm) {
        demo_info("no KVM instance found\n");
        return -ENODEV;
    }

    demo_context.kvm = kvm;
    demo_dbg("using kvm=%p\n", kvm);
    return 0;
}

static int
demo_bringup(void)
{
    /* kvmtool opens this debugfs file on setup */
    if (!demo_context.kvm)
        return load_kvm_instance();

    return 0;
}

static int
fop_open(struct inode* inode, struct file* f)
{
    demo_dbg("open\n");
    demo_bringup();
    return 0;
}

static int
demo_teardown(void)
{
    /* kvmtool died or user disabled routing */
    clean_up_irq_handlers();

    if (demo_context.kvm) {
        unregister_eoi_notifiers();
        demo_context.kvm = NULL;
    }

    return 0;
}

static int
fop_release(struct inode* inode, struct file* f)
{
    demo_dbg("release\n");
    demo_teardown();
    return 0;
}


static int
add_irq_forwarding(int phy_irq, int virt_irq, int level)
{
    int ret;
    struct demo_irq_context* ctx;

    if (virt_irq < 0 || virt_irq >= demo_context_irqs_size)
        return -EINVAL;

    ctx = kmalloc(sizeof(*ctx), GFP_KERNEL);
    if (!ctx)
        return -ENOMEM;

    memset(ctx, 0, sizeof(*ctx));
    ctx->phy_irq = phy_irq;
    ctx->virt_irq = virt_irq;
    ctx->level = level;

    demo_info("add forwarding: phy=%d -> virt=%d (%s)\n",
              phy_irq,
              virt_irq,
              level ? "level" : "edge");

    demo_context.irqs[virt_irq] = ctx;

    if (level == EDGE_TRIGGERED) {
        ret = request_irq(
          phy_irq, irq_handler_edge, IRQF_SHARED, "irq_router_edge", ctx);
    } else if (level == LEVEL_TRIGGERED) {
        ret = request_irq(
          phy_irq, irq_handler_level, IRQF_SHARED, "irq_router_level", ctx);
    } else {
        ret = -EINVAL;
    }

    if (ret < 0) {
        demo_err(
          "request_irq failed phy=%d virt=%d: %d\n", phy_irq, virt_irq, ret);
        demo_context.irqs[virt_irq] = NULL;
        kfree(ctx);
        return ret;
    }

    ret = pin_spi_to_core(phy_irq, PIN_SPI_CORE);
    if (ret < 0) {
        demo_err("pin_spi_to_core failed: %d\n", ret);
        free_irq(phy_irq, ctx);
        demo_context.irqs[virt_irq] = NULL;
        kfree(ctx);
        return ret;
    }

    return 0;
}


static long
fop_ioctl(struct file* file, unsigned int cmd, unsigned long arg)
{
    int ret = 0;

    demo_dbg("ioctl cmd=%u\n", cmd);

    switch (cmd) {
        case DEMO_IOCTL_REQ: {
            struct demo_ioctl_req data;
            int linux_irq_nr;

            if (copy_from_user(&data, (void __user*)arg, sizeof(data)))
                return -EFAULT;

            demo_dbg("ioctl REQ phy=%d virt=%d attach=%d level=%d\n",
                     data.irq_phy,
                     data.irq_virt,
                     data.attach,
                     data.level_trigger);

            linux_irq_nr = to_linux_irq(data.irq_phy);
            if (linux_irq_nr <= 0) {
                demo_err(
                  "to_linux_irq(%d) failed: %d\n", data.irq_phy, linux_irq_nr);
                return -EINVAL;
            }

            demo_dbg("mapped gic=%d -> linux_irq=%d (virt=%d)\n",
                     data.irq_phy,
                     linux_irq_nr,
                     data.irq_virt);

            if (data.attach) {
                ret = add_irq_forwarding(
                  linux_irq_nr, data.irq_virt, data.level_trigger);
                if (ret < 0) {
                    demo_err("add_irq_forwarding failed: %d\n", ret);
                    return ret;
                }
            }

            return 0;
        }

        case DEMO_IOCTL_ENABLE: {
            struct demo_ioctl_enable data;

            if (copy_from_user(&data, (void __user*)arg, sizeof(data)))
                return -EFAULT;

            demo_dbg("ioctl ENABLE enable=%d\n", data.enable);

            if (data.enable) {
                demo_bringup();
                register_eoi_notifiers();
            } else {
                demo_teardown();
            }

            return 0;
        }

        default:
            return -ENOTTY;
    }
}

static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = fop_open,
    .release = fop_release,
    .unlocked_ioctl = fop_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = fop_ioctl,
#endif
};

static int __init
demo_irq_init(void)
{
    struct dentry* file;
    int ret;

    memset(&demo_context, 0, sizeof(demo_context));
    spin_lock_init(&demo_context.lock);

    ret = kprobe_init();
    if (ret) {
        demo_err("kprobe init failed: %d\n", ret);
        return ret;
    }

    demo_context.debugfs_dir = debugfs_create_dir("demo", NULL);

    if (!demo_context.debugfs_dir) {
        demo_err("debugfs_create_dir failed\n");
        return -ENOMEM;
    }

    file = debugfs_create_file(
      "irq_router", 0600, demo_context.debugfs_dir, NULL, &fops);

    if (!file) {
        demo_err("debugfs_create_file failed\n");
        debugfs_remove_recursive(demo_context.debugfs_dir);
        return -ENOMEM;
    }

    pr_info("debugfs file: %s/%s\n",
            demo_context.debugfs_dir->d_name.name,
            file->d_name.name);

    demo_context.virt_inj_wq =
      alloc_workqueue("demo_virt_irq_wq", WQ_UNBOUND, 0);

    if (!demo_context.virt_inj_wq) {
        demo_err("alloc_workqueue failed\n");
        debugfs_remove_recursive(demo_context.debugfs_dir);
        return -ENOMEM;
    }

    demo_info("loaded\n");
    return 0;
}

static void __exit
demo_irq_exit(void)
{
    demo_dbg("exit\n");

    demo_teardown();

    if (demo_context.virt_inj_wq) {
        destroy_workqueue(demo_context.virt_inj_wq);
        demo_context.virt_inj_wq = NULL;
    }

    debugfs_remove_recursive(demo_context.debugfs_dir);
    demo_context.debugfs_dir = NULL;

    demo_info("unloaded\n");
}

module_init(demo_irq_init);
module_exit(demo_irq_exit);

MODULE_LICENSE("GPL");
