#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/cpu.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/smp.h>
#include <linux/sched.h> 

#define RUN_TIME_MS 60000 // 60 seconds
#define NUM_WORKERS nr_cpu_ids

static int target_cpu = 0;
module_param(target_cpu, int, 0);
MODULE_PARM_DESC(target_cpu, "Destination CPU for IPI");

static struct timer_list ipi_timer;
static unsigned long start_time;
static struct workqueue_struct *ipi_wq;
static struct work_struct ipi_works[NR_CPUS];
static void ipi_handler(void *info)
{
    return;
}

static void send_ipi(void)
{
    smp_call_function_single(target_cpu, ipi_handler, NULL, 1);
}

static void ipi_worker(struct work_struct *work) {
    int worker_cpu = -1;
    
    for (int i = 0; i < NUM_WORKERS; i++) {
        if (work == &ipi_works[i]) {
            worker_cpu = i;
            break;
        }
    }
    
    if (worker_cpu != -1 && worker_cpu != target_cpu) {
        struct task_struct *task = current;
        cpumask_t mask;
        
        cpumask_clear(&mask);
        cpumask_set_cpu(worker_cpu, &mask);
        set_cpus_allowed_ptr(task, &mask);
    }
    
    while (time_before(jiffies, start_time + msecs_to_jiffies(RUN_TIME_MS))) {
        send_ipi();
        cond_resched();
    }
}

static void timer_callback(struct timer_list *timer) {
    pr_info("IPI spam stopped. Runtime: %lu ms\n",
            jiffies_to_msecs(jiffies - start_time));
}

static int __init ipi_init(void) {
    int i;

    if (target_cpu < 0 || target_cpu >= nr_cpu_ids || !cpu_online(target_cpu)) {
        pr_err("Invalid or offline CPU %d\n", target_cpu);
        return -EINVAL;
    }

    timer_setup(&ipi_timer, timer_callback, 0);
    mod_timer(&ipi_timer, jiffies + msecs_to_jiffies(RUN_TIME_MS));
    start_time = jiffies;

    ipi_wq = alloc_workqueue("ipi_spam_wq", WQ_UNBOUND, NUM_WORKERS);
    if (!ipi_wq) return -ENOMEM;

    pr_info("Spamming IPIs to CPU %d from %d workers\n",
            target_cpu, NUM_WORKERS - 1);

    for (i = 0; i < NUM_WORKERS; i++) {
        if (i == target_cpu) continue;
        INIT_WORK(&ipi_works[i], ipi_worker);
        queue_work(ipi_wq, &ipi_works[i]);
    }

    return 0;
}

static void __exit ipi_exit(void) {
    del_timer_sync(&ipi_timer);
    if (ipi_wq) destroy_workqueue(ipi_wq);
    pr_info("Module unloaded\n");
}

module_init(ipi_init);
module_exit(ipi_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("IPI Storm Tester using SMP functions");