#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/ktime.h>
#include <linux/debugfs.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>

/*
 * feedcat_kprobes.c - 基于 Kprobes 的驱动性能诊断模块
 *
 * 功能：
 *   1. 挂载 i2c_transfer，量化 AHT30 每次 I2C 总线传输耗时（微秒）
 *   2. 挂载 weight_get，量化 HX711 每次采样全路径耗时（微秒）
 *   3. 通过 Debugfs 导出统计，无需重新编译驱动即可运行时分析
 *
 * 使用：
 *   insmod feedcat_kprobes.ko
 *   cat /sys/kernel/debug/feedcat_kprobes/i2c_last_us      # 最近 I2C 耗时(us)
 *   cat /sys/kernel/debug/feedcat_kprobes/i2c_max_us       # 历史最大 I2C 耗时
 *   cat /sys/kernel/debug/feedcat_kprobes/i2c_call_count   # I2C 调用总次数
 *   cat /sys/kernel/debug/feedcat_kprobes/weight_last_us   # 最近 weight_get 耗时
 *   cat /sys/kernel/debug/feedcat_kprobes/weight_max_us    # 历史最大 weight_get 耗时
 *   cat /sys/kernel/debug/feedcat_kprobes/weight_call_count
 *   rmmod feedcat_kprobes
 */

/* ==================== i2c_transfer 统计 ==================== */
static ktime_t  i2c_entry_time;
static u32      i2c_last_us   = 0;
static u32      i2c_max_us    = 0;
static atomic_t i2c_call_count = ATOMIC_INIT(0);
static DEFINE_SPINLOCK(i2c_lock);

static int i2c_pre(struct kprobe *p, struct pt_regs *regs)
{
    i2c_entry_time = ktime_get();
    return 0;
}

static void i2c_post(struct kprobe *p, struct pt_regs *regs, unsigned long flags)
{
    u32 us = (u32)ktime_to_us(ktime_sub(ktime_get(), i2c_entry_time));
    unsigned long f;
    spin_lock_irqsave(&i2c_lock, f);
    i2c_last_us = us;
    if (us > i2c_max_us) i2c_max_us = us;
    spin_unlock_irqrestore(&i2c_lock, f);
    atomic_inc(&i2c_call_count);
}

static struct kprobe kp_i2c = {
    .symbol_name  = "i2c_transfer",
    .pre_handler  = i2c_pre,
    .post_handler = i2c_post,
};

/* ==================== weight_get 统计 ==================== */
static ktime_t  weight_entry_time;
static u32      weight_last_us    = 0;
static u32      weight_max_us     = 0;
static atomic_t weight_call_count = ATOMIC_INIT(0);
static DEFINE_SPINLOCK(weight_lock);

static int weight_pre(struct kprobe *p, struct pt_regs *regs)
{
    weight_entry_time = ktime_get();
    return 0;
}

static void weight_post(struct kprobe *p, struct pt_regs *regs, unsigned long flags)
{
    u32 us = (u32)ktime_to_us(ktime_sub(ktime_get(), weight_entry_time));
    unsigned long f;
    spin_lock_irqsave(&weight_lock, f);
    weight_last_us = us;
    if (us > weight_max_us) weight_max_us = us;
    spin_unlock_irqrestore(&weight_lock, f);
    atomic_inc(&weight_call_count);
}

static struct kprobe kp_weight = {
    .symbol_name  = "weight_get",
    .pre_handler  = weight_pre,
    .post_handler = weight_post,
};

/* ==================== Debugfs ==================== */
static struct dentry *dbg_dir;

static void kprobes_debugfs_init(void)
{
    dbg_dir = debugfs_create_dir("feedcat_kprobes", NULL);
    if (IS_ERR_OR_NULL(dbg_dir)) { dbg_dir = NULL; return; }
    debugfs_create_u32("i2c_last_us",          0444, dbg_dir, &i2c_last_us);
    debugfs_create_u32("i2c_max_us",           0444, dbg_dir, &i2c_max_us);
    debugfs_create_atomic_t("i2c_call_count",  0444, dbg_dir, &i2c_call_count);
    debugfs_create_u32("weight_last_us",        0444, dbg_dir, &weight_last_us);
    debugfs_create_u32("weight_max_us",         0444, dbg_dir, &weight_max_us);
    debugfs_create_atomic_t("weight_call_count",0444, dbg_dir, &weight_call_count);
    pr_info("[kprobes] debugfs: /sys/kernel/debug/feedcat_kprobes/\n");
}

/* ==================== 模块入口/出口 ==================== */
static int __init feedcat_kprobes_init(void)
{
    int ret;

    ret = register_kprobe(&kp_i2c);
    if (ret < 0) {
        pr_err("[kprobes] register i2c_transfer failed: %d\n", ret);
        return ret;
    }
    pr_info("[kprobes] i2c_transfer kprobe planted @ %p\n", kp_i2c.addr);

    ret = register_kprobe(&kp_weight);
    if (ret < 0)
        pr_warn("[kprobes] weight_get kprobe skipped (not exported): %d\n", ret);
    else
        pr_info("[kprobes] weight_get kprobe planted @ %p\n", kp_weight.addr);

    kprobes_debugfs_init();
    pr_info("[kprobes] feedcat_kprobes loaded\n");
    return 0;
}

static void __exit feedcat_kprobes_exit(void)
{
    unregister_kprobe(&kp_i2c);
    unregister_kprobe(&kp_weight);
    if (dbg_dir) debugfs_remove_recursive(dbg_dir);
    pr_info("[kprobes] feedcat_kprobes unloaded\n");
}

module_init(feedcat_kprobes_init);
module_exit(feedcat_kprobes_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("FeedCat Kprobes: I2C and HX711 latency profiler");
MODULE_AUTHOR("Developer");
MODULE_VERSION("V1.0");
