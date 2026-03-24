#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/ktime.h>
#include <linux/debugfs.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>

/*
 * feedcat_kprobes.c - 基于 Kretprobes 的驱动性能诊断模块
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

/* 每个 kretprobe 实例私有数据：记录函数入口时间戳 */
struct probe_data {
    ktime_t entry_time;
};

/* ==================== i2c_transfer 统计 ==================== */
static u32      i2c_last_us   = 0;
static u32      i2c_max_us    = 0;
static atomic_t i2c_call_count = ATOMIC_INIT(0);
static DEFINE_SPINLOCK(i2c_lock);

/* entry handler：函数被调用时触发，记录时间戳到实例私有 data */
static int i2c_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    struct probe_data *data = (struct probe_data *)ri->data;
    data->entry_time = ktime_get();
    return 0;
}

/* return handler：函数返回时触发，计算耗时 */
static int i2c_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    struct probe_data *data = (struct probe_data *)ri->data;
    u32 us = (u32)ktime_to_us(ktime_sub(ktime_get(), data->entry_time));
    unsigned long f;

    spin_lock_irqsave(&i2c_lock, f);
    i2c_last_us = us;
    if (us > i2c_max_us)
        i2c_max_us = us;
    spin_unlock_irqrestore(&i2c_lock, f);
    atomic_inc(&i2c_call_count);
    return 0;
}

static struct kretprobe krp_i2c = {
    .kp.symbol_name = "i2c_transfer",
    .entry_handler  = i2c_entry,
    .handler        = i2c_ret,
    .data_size      = sizeof(struct probe_data),
    .maxactive      = 8,  /* 最多同时追踪 8 个并发调用 */
};

/* ==================== weight_get 统计 ==================== */
static u32      weight_last_us    = 0;
static u32      weight_max_us     = 0;
static atomic_t weight_call_count = ATOMIC_INIT(0);
static DEFINE_SPINLOCK(weight_lock);

static int weight_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    struct probe_data *data = (struct probe_data *)ri->data;
    data->entry_time = ktime_get();
    return 0;
}

static int weight_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    struct probe_data *data = (struct probe_data *)ri->data;
    u32 us = (u32)ktime_to_us(ktime_sub(ktime_get(), data->entry_time));
    unsigned long f;

    spin_lock_irqsave(&weight_lock, f);
    weight_last_us = us;
    if (us > weight_max_us)
        weight_max_us = us;
    spin_unlock_irqrestore(&weight_lock, f);
    atomic_inc(&weight_call_count);
    return 0;
}

static struct kretprobe krp_weight = {
    .kp.symbol_name = "weight_get",
    .entry_handler  = weight_entry,
    .handler        = weight_ret,
    .data_size      = sizeof(struct probe_data),
    .maxactive      = 4,
};

/* ==================== Debugfs ==================== */
static struct dentry *dbg_dir;

static void kprobes_debugfs_init(void)
{
    dbg_dir = debugfs_create_dir("feedcat_kprobes", NULL);
    if (IS_ERR_OR_NULL(dbg_dir)) { dbg_dir = NULL; return; }
    debugfs_create_u32("i2c_last_us",           0444, dbg_dir, &i2c_last_us);
    debugfs_create_u32("i2c_max_us",            0444, dbg_dir, &i2c_max_us);
    debugfs_create_atomic_t("i2c_call_count",   0444, dbg_dir, &i2c_call_count);
    debugfs_create_u32("weight_last_us",         0444, dbg_dir, &weight_last_us);
    debugfs_create_u32("weight_max_us",          0444, dbg_dir, &weight_max_us);
    debugfs_create_atomic_t("weight_call_count", 0444, dbg_dir, &weight_call_count);
    pr_info("[kretprobes] debugfs: /sys/kernel/debug/feedcat_kprobes/\n");
}

/* ==================== 模块入口/出口 ==================== */
static int __init feedcat_kprobes_init(void)
{
    int ret;

    ret = register_kretprobe(&krp_i2c);
    if (ret < 0) {
        pr_err("[kretprobes] register i2c_transfer failed: %d\n", ret);
        return ret;
    }
    pr_info("[kretprobes] i2c_transfer kretprobe planted @ %p\n", krp_i2c.kp.addr);

    ret = register_kretprobe(&krp_weight);
    if (ret < 0)
        pr_warn("[kretprobes] weight_get kretprobe skipped (not exported): %d\n", ret);
    else
        pr_info("[kretprobes] weight_get kretprobe planted @ %p\n", krp_weight.kp.addr);

    kprobes_debugfs_init();
    pr_info("[kretprobes] feedcat_kprobes loaded\n");
    return 0;
}

static void __exit feedcat_kprobes_exit(void)
{
    unregister_kretprobe(&krp_i2c);
    unregister_kretprobe(&krp_weight);
    if (dbg_dir)
        debugfs_remove_recursive(dbg_dir);
    pr_info("[kretprobes] feedcat_kprobes unloaded\n");
}

module_init(feedcat_kprobes_init);
module_exit(feedcat_kprobes_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("FeedCat Kretprobes: I2C and HX711 latency profiler");
MODULE_AUTHOR("Developer");
MODULE_VERSION("V2.0");
