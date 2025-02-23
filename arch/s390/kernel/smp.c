/*
 *  SMP related functions
 *
 *    Copyright IBM Corp. 1999, 2012
 *    Author(s): Denis Joseph Barrow,
 *		 Martin Schwidefsky <schwidefsky@de.ibm.com>,
 *		 Heiko Carstens <heiko.carstens@de.ibm.com>,
 *
 *  based on other smp stuff by
 *    (c) 1995 Alan Cox, CymruNET Ltd  <alan@cymru.net>
 *    (c) 1998 Ingo Molnar
 *
 * The code outside of smp.c uses logical cpu numbers, only smp.c does
 * the translation of logical to physical cpu ids. All new code that
 * operates on physical cpu numbers needs to go into smp.c.
 */

#define KMSG_COMPONENT "cpu"
#define pr_fmt(fmt) KMSG_COMPONENT ": " fmt

#include <linux/workqueue.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/err.h>
#include <linux/spinlock.h>
#include <linux/kernel_stat.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/irqflags.h>
#include <linux/cpu.h>
#include <linux/slab.h>
#include <linux/crash_dump.h>
#include <linux/memblock.h>
#include <asm/asm-offsets.h>
#include <asm/diag.h>
#include <asm/switch_to.h>
#include <asm/facility.h>
#include <asm/ipl.h>
#include <asm/setup.h>
#include <asm/irq.h>
#include <asm/tlbflush.h>
#include <asm/vtimer.h>
#include <asm/lowcore.h>
#include <asm/sclp.h>
#include <asm/vdso.h>
#include <asm/debug.h>
#include <asm/os_info.h>
#include <asm/sigp.h>
#include <asm/idle.h>
#include "entry.h"

enum {
	ec_schedule = 0,
	ec_call_function_single,
	ec_stop_cpu,
};

enum {
	CPU_STATE_STANDBY,
	CPU_STATE_CONFIGURED,
};

static DEFINE_PER_CPU(struct cpu *, cpu_device);

struct pcpu {
	struct _lowcore *lowcore;	/* lowcore page(s) for the cpu */
	unsigned long ec_mask;		/* bit mask for ec_xxx functions */
	signed char state;		/* physical cpu state */
	signed char polarization;	/* physical polarization */
	u16 address;			/* physical cpu address */
};

static u8 boot_core_type;
static struct pcpu pcpu_devices[NR_CPUS];

unsigned int smp_cpu_mt_shift;
EXPORT_SYMBOL(smp_cpu_mt_shift);

unsigned int smp_cpu_mtid;
EXPORT_SYMBOL(smp_cpu_mtid);

static unsigned int smp_max_threads __initdata = -1U;

static int __init early_nosmt(char *s)
{
	smp_max_threads = 1;
	return 0;
}
early_param("nosmt", early_nosmt);

static int __init early_smt(char *s)
{
	get_option(&s, &smp_max_threads);
	return 0;
}
early_param("smt", early_smt);

/*
 * The smp_cpu_state_mutex must be held when changing the state or polarization
 * member of a pcpu data structure within the pcpu_devices arreay.
 */
DEFINE_MUTEX(smp_cpu_state_mutex);

/*
 * Signal processor helper functions.
 */
static inline int __pcpu_sigp_relax(u16 addr, u8 order, unsigned long parm,
				    u32 *status)
{
	int cc;

	while (1) {
		cc = __pcpu_sigp(addr, order, parm, NULL);
		if (cc != SIGP_CC_BUSY)
			return cc;
		cpu_relax();
	}
}

static int pcpu_sigp_retry(struct pcpu *pcpu, u8 order, u32 parm)
{
	int cc, retry;

	for (retry = 0; ; retry++) {
		cc = __pcpu_sigp(pcpu->address, order, parm, NULL);
		if (cc != SIGP_CC_BUSY)
			break;
		if (retry >= 3)
			udelay(10);
	}
	return cc;
}

static inline int pcpu_stopped(struct pcpu *pcpu)
{
	u32 uninitialized_var(status);

	if (__pcpu_sigp(pcpu->address, SIGP_SENSE,
			0, &status) != SIGP_CC_STATUS_STORED)
		return 0;
	return !!(status & (SIGP_STATUS_CHECK_STOP|SIGP_STATUS_STOPPED));
}

static inline int pcpu_running(struct pcpu *pcpu)
{
	if (__pcpu_sigp(pcpu->address, SIGP_SENSE_RUNNING,
			0, NULL) != SIGP_CC_STATUS_STORED)
		return 1;
	/* Status stored condition code is equivalent to cpu not running. */
	return 0;
}

/*
 * Find struct pcpu by cpu address.
 */
static struct pcpu *pcpu_find_address(const struct cpumask *mask, u16 address)
{
	int cpu;

	for_each_cpu(cpu, mask)
		if (pcpu_devices[cpu].address == address)
			return pcpu_devices + cpu;
	return NULL;
}

static void pcpu_ec_call(struct pcpu *pcpu, int ec_bit)
{
	int order;

	if (test_and_set_bit(ec_bit, &pcpu->ec_mask))
		return;
	order = pcpu_running(pcpu) ? SIGP_EXTERNAL_CALL : SIGP_EMERGENCY_SIGNAL;
	pcpu_sigp_retry(pcpu, order, 0);
}

#define ASYNC_FRAME_OFFSET (ASYNC_SIZE - STACK_FRAME_OVERHEAD - __PT_SIZE)
#define PANIC_FRAME_OFFSET (PAGE_SIZE - STACK_FRAME_OVERHEAD - __PT_SIZE)

static int pcpu_alloc_lowcore(struct pcpu *pcpu, int cpu)
{
	unsigned long async_stack, panic_stack;
	struct _lowcore *lc;

	if (pcpu != &pcpu_devices[0]) {
		pcpu->lowcore =	(struct _lowcore *)
			__get_free_pages(GFP_KERNEL | GFP_DMA, LC_ORDER);
		async_stack = __get_free_pages(GFP_KERNEL, ASYNC_ORDER);
		panic_stack = __get_free_page(GFP_KERNEL);
		if (!pcpu->lowcore || !panic_stack || !async_stack)
			goto out;
	} else {
		async_stack = pcpu->lowcore->async_stack - ASYNC_FRAME_OFFSET;
		panic_stack = pcpu->lowcore->panic_stack - PANIC_FRAME_OFFSET;
	}
	lc = pcpu->lowcore;
	memcpy(lc, &S390_lowcore, 512);
	memset((char *) lc + 512, 0, sizeof(*lc) - 512);
	lc->async_stack = async_stack + ASYNC_FRAME_OFFSET;
	lc->panic_stack = panic_stack + PANIC_FRAME_OFFSET;
	lc->cpu_nr = cpu;
	lc->spinlock_lockval = arch_spin_lockval(cpu);
	lc->br_r1_trampoline = 0x07f1;	/* br %r1 */
	if (MACHINE_HAS_VX)
		lc->vector_save_area_addr =
			(unsigned long) &lc->vector_save_area;
	if (vdso_alloc_per_cpu(lc))
		goto out;
	lowcore_ptr[cpu] = lc;
	pcpu_sigp_retry(pcpu, SIGP_SET_PREFIX, (u32)(unsigned long) lc);
	return 0;
out:
	if (pcpu != &pcpu_devices[0]) {
		free_page(panic_stack);
		free_pages(async_stack, ASYNC_ORDER);
		free_pages((unsigned long) pcpu->lowcore, LC_ORDER);
	}
	return -ENOMEM;
}

#ifdef CONFIG_HOTPLUG_CPU

static void pcpu_free_lowcore(struct pcpu *pcpu)
{
	pcpu_sigp_retry(pcpu, SIGP_SET_PREFIX, 0);
	lowcore_ptr[pcpu - pcpu_devices] = NULL;
	vdso_free_per_cpu(pcpu->lowcore);
	if (pcpu == &pcpu_devices[0])
		return;
	free_page(pcpu->lowcore->panic_stack-PANIC_FRAME_OFFSET);
	free_pages(pcpu->lowcore->async_stack-ASYNC_FRAME_OFFSET, ASYNC_ORDER);
	free_pages((unsigned long) pcpu->lowcore, LC_ORDER);
}

#endif /* CONFIG_HOTPLUG_CPU */

static void pcpu_prepare_secondary(struct pcpu *pcpu, int cpu)
{
	struct _lowcore *lc = pcpu->lowcore;

	if (MACHINE_HAS_TLB_LC)
		cpumask_set_cpu(cpu, &init_mm.context.cpu_attach_mask);
	cpumask_set_cpu(cpu, mm_cpumask(&init_mm));
	atomic_inc(&init_mm.context.attach_count);
	lc->cpu_nr = cpu;
	lc->spinlock_lockval = arch_spin_lockval(cpu);
	lc->percpu_offset = __per_cpu_offset[cpu];
	lc->kernel_asce = S390_lowcore.kernel_asce;
	lc->machine_flags = S390_lowcore.machine_flags;
	lc->user_timer = lc->system_timer = lc->steal_timer = 0;
	__ctl_store(lc->cregs_save_area, 0, 15);
	save_access_regs((unsigned int *) lc->access_regs_save_area);
	memcpy(lc->stfle_fac_list, S390_lowcore.stfle_fac_list,
	       sizeof(lc->stfle_fac_list));
	memcpy(lc->alt_stfle_fac_list, S390_lowcore.alt_stfle_fac_list,
	       sizeof(lc->alt_stfle_fac_list));
}

static void pcpu_attach_task(struct pcpu *pcpu, struct task_struct *tsk)
{
	struct _lowcore *lc = pcpu->lowcore;
	struct thread_info *ti = task_thread_info(tsk);

	lc->kernel_stack = (unsigned long) task_stack_page(tsk)
		+ THREAD_SIZE - STACK_FRAME_OVERHEAD - sizeof(struct pt_regs);
	lc->thread_info = (unsigned long) task_thread_info(tsk);
	lc->current_task = (unsigned long) tsk;
	lc->lpp = LPP_MAGIC;
	lc->current_pid = tsk->pid;
	lc->user_timer = ti->user_timer;
	lc->system_timer = ti->system_timer;
	lc->steal_timer = 0;
}

static void pcpu_start_fn(struct pcpu *pcpu, void (*func)(void *), void *data)
{
	struct _lowcore *lc = pcpu->lowcore;

	lc->restart_stack = lc->kernel_stack;
	lc->restart_fn = (unsigned long) func;
	lc->restart_data = (unsigned long) data;
	lc->restart_source = -1UL;
	pcpu_sigp_retry(pcpu, SIGP_RESTART, 0);
}

/*
 * Call function via PSW restart on pcpu and stop the current cpu.
 */
static void pcpu_delegate(struct pcpu *pcpu, void (*func)(void *),
			  void *data, unsigned long stack)
{
	struct _lowcore *lc = lowcore_ptr[pcpu - pcpu_devices];
	unsigned long source_cpu = stap();

	__load_psw_mask(PSW_KERNEL_BITS);
	if (pcpu->address == source_cpu)
		func(data);	/* should not return */
	/* Stop target cpu (if func returns this stops the current cpu). */
	pcpu_sigp_retry(pcpu, SIGP_STOP, 0);
	/* Restart func on the target cpu and stop the current cpu. */
	mem_assign_absolute(lc->restart_stack, stack);
	mem_assign_absolute(lc->restart_fn, (unsigned long) func);
	mem_assign_absolute(lc->restart_data, (unsigned long) data);
	mem_assign_absolute(lc->restart_source, source_cpu);
	__bpon();
	asm volatile(
		"0:	sigp	0,%0,%2	# sigp restart to target cpu\n"
		"	brc	2,0b	# busy, try again\n"
		"1:	sigp	0,%1,%3	# sigp stop to current cpu\n"
		"	brc	2,1b	# busy, try again\n"
		: : "d" (pcpu->address), "d" (source_cpu),
		    "K" (SIGP_RESTART), "K" (SIGP_STOP)
		: "0", "1", "cc");
	for (;;) ;
}

/*
 * Enable additional logical cpus for multi-threading.
 */
static int pcpu_set_smt(unsigned int mtid)
{
	register unsigned long reg1 asm ("1") = (unsigned long) mtid;
	int cc;

	if (smp_cpu_mtid == mtid)
		return 0;
	asm volatile(
		"	sigp	%1,0,%2	# sigp set multi-threading\n"
		"	ipm	%0\n"
		"	srl	%0,28\n"
		: "=d" (cc) : "d" (reg1), "K" (SIGP_SET_MULTI_THREADING)
		: "cc");
	if (cc == 0) {
		smp_cpu_mtid = mtid;
		smp_cpu_mt_shift = 0;
		while (smp_cpu_mtid >= (1U << smp_cpu_mt_shift))
			smp_cpu_mt_shift++;
		pcpu_devices[0].address = stap();
	}
	return cc;
}

/*
 * Call function on an online CPU.
 */
void smp_call_online_cpu(void (*func)(void *), void *data)
{
	struct pcpu *pcpu;

	/* Use the current cpu if it is online. */
	pcpu = pcpu_find_address(cpu_online_mask, stap());
	if (!pcpu)
		/* Use the first online cpu. */
		pcpu = pcpu_devices + cpumask_first(cpu_online_mask);
	pcpu_delegate(pcpu, func, data, (unsigned long) restart_stack);
}

/*
 * Call function on the ipl CPU.
 */
void smp_call_ipl_cpu(void (*func)(void *), void *data)
{
	struct _lowcore *lc = pcpu_devices->lowcore;

	if (pcpu_devices[0].address == stap())
		lc = &S390_lowcore;

	pcpu_delegate(&pcpu_devices[0], func, data,
		      lc->panic_stack - PANIC_FRAME_OFFSET + PAGE_SIZE);
}

int smp_find_processor_id(u16 address)
{
	int cpu;

	for_each_present_cpu(cpu)
		if (pcpu_devices[cpu].address == address)
			return cpu;
	return -1;
}

int smp_vcpu_scheduled(int cpu)
{
	return pcpu_running(pcpu_devices + cpu);
}

void smp_yield_cpu(int cpu)
{
	if (MACHINE_HAS_DIAG9C) {
		diag_stat_inc_norecursion(DIAG_STAT_X09C);
		asm volatile("diag %0,0,0x9c"
			     : : "d" (pcpu_devices[cpu].address));
	} else if (MACHINE_HAS_DIAG44) {
		diag_stat_inc_norecursion(DIAG_STAT_X044);
		asm volatile("diag 0,0,0x44");
	}
}

/*
 * Send cpus emergency shutdown signal. This gives the cpus the
 * opportunity to complete outstanding interrupts.
 */
static void smp_emergency_stop(cpumask_t *cpumask)
{
	u64 end;
	int cpu;

	end = get_tod_clock() + (1000000UL << 12);
	for_each_cpu(cpu, cpumask) {
		struct pcpu *pcpu = pcpu_devices + cpu;
		set_bit(ec_stop_cpu, &pcpu->ec_mask);
		while (__pcpu_sigp(pcpu->address, SIGP_EMERGENCY_SIGNAL,
				   0, NULL) == SIGP_CC_BUSY &&
		       get_tod_clock() < end)
			cpu_relax();
	}
	while (get_tod_clock() < end) {
		for_each_cpu(cpu, cpumask)
			if (pcpu_stopped(pcpu_devices + cpu))
				cpumask_clear_cpu(cpu, cpumask);
		if (cpumask_empty(cpumask))
			break;
		cpu_relax();
	}
}

/*
 * Stop all cpus but the current one.
 */
void smp_send_stop(void)
{
	cpumask_t cpumask;
	int cpu;

	/* Disable all interrupts/machine checks */
	__load_psw_mask(PSW_KERNEL_BITS | PSW_MASK_DAT);
	trace_hardirqs_off();

	debug_set_critical();
	cpumask_copy(&cpumask, cpu_online_mask);
	cpumask_clear_cpu(smp_processor_id(), &cpumask);

	if (oops_in_progress)
		smp_emergency_stop(&cpumask);

	/* stop all processors */
	for_each_cpu(cpu, &cpumask) {
		struct pcpu *pcpu = pcpu_devices + cpu;
		pcpu_sigp_retry(pcpu, SIGP_STOP, 0);
		while (!pcpu_stopped(pcpu))
			cpu_relax();
	}
}

/*
 * This is the main routine where commands issued by other
 * cpus are handled.
 */
static void smp_handle_ext_call(void)
{
	unsigned long bits;

	/* handle bit signal external calls */
	bits = xchg(&pcpu_devices[smp_processor_id()].ec_mask, 0);
	if (test_bit(ec_stop_cpu, &bits))
		smp_stop_cpu();
	if (test_bit(ec_schedule, &bits))
		scheduler_ipi();
	if (test_bit(ec_call_function_single, &bits))
		generic_smp_call_function_single_interrupt();
}

static void do_ext_call_interrupt(struct ext_code ext_code,
				  unsigned int param32, unsigned long param64)
{
	inc_irq_stat(ext_code.code == 0x1202 ? IRQEXT_EXC : IRQEXT_EMS);
	smp_handle_ext_call();
}

void arch_send_call_function_ipi_mask(const struct cpumask *mask)
{
	int cpu;

	for_each_cpu(cpu, mask)
		pcpu_ec_call(pcpu_devices + cpu, ec_call_function_single);
}

void arch_send_call_function_single_ipi(int cpu)
{
	pcpu_ec_call(pcpu_devices + cpu, ec_call_function_single);
}

/*
 * this function sends a 'reschedule' IPI to another CPU.
 * it goes straight through and wastes no time serializing
 * anything. Worst case is that we lose a reschedule ...
 */
void smp_send_reschedule(int cpu)
{
	pcpu_ec_call(pcpu_devices + cpu, ec_schedule);
}

/*
 * parameter area for the set/clear control bit callbacks
 */
struct ec_creg_mask_parms {
	unsigned long orval;
	unsigned long andval;
	int cr;
};

/*
 * callback for setting/clearing control bits
 */
static void smp_ctl_bit_callback(void *info)
{
	struct ec_creg_mask_parms *pp = info;
	unsigned long cregs[16];

	__ctl_store(cregs, 0, 15);
	cregs[pp->cr] = (cregs[pp->cr] & pp->andval) | pp->orval;
	__ctl_load(cregs, 0, 15);
}

/*
 * Set a bit in a control register of all cpus
 */
void smp_ctl_set_bit(int cr, int bit)
{
	struct ec_creg_mask_parms parms = { 1UL << bit, -1UL, cr };

	on_each_cpu(smp_ctl_bit_callback, &parms, 1);
}
EXPORT_SYMBOL(smp_ctl_set_bit);

/*
 * Clear a bit in a control register of all cpus
 */
void smp_ctl_clear_bit(int cr, int bit)
{
	struct ec_creg_mask_parms parms = { 0, ~(1UL << bit), cr };

	on_each_cpu(smp_ctl_bit_callback, &parms, 1);
}
EXPORT_SYMBOL(smp_ctl_clear_bit);

#ifdef CONFIG_CRASH_DUMP

static void __init __smp_store_cpu_state(struct save_area_ext *sa_ext,
					 u16 address, int is_boot_cpu)
{
	void *lc = (void *)(unsigned long) store_prefix();
	unsigned long vx_sa;

	if (is_boot_cpu) {
		/* Copy the registers of the boot CPU. */
		copy_oldmem_page(1, (void *) &sa_ext->sa, sizeof(sa_ext->sa),
				 SAVE_AREA_BASE - PAGE_SIZE, 0);
		if (MACHINE_HAS_VX)
			save_vx_regs_safe(sa_ext->vx_regs);
		return;
	}
	/* Get the registers of a non-boot cpu. */
	__pcpu_sigp_relax(address, SIGP_STOP_AND_STORE_STATUS, 0, NULL);
	memcpy_real(&sa_ext->sa, lc + SAVE_AREA_BASE, sizeof(sa_ext->sa));
	if (!MACHINE_HAS_VX)
		return;
	/* Get the VX registers */
	vx_sa = memblock_alloc(PAGE_SIZE, PAGE_SIZE);
	if (!vx_sa)
		panic("could not allocate memory for VX save area\n");
	__pcpu_sigp_relax(address, SIGP_STORE_ADDITIONAL_STATUS, vx_sa, NULL);
	memcpy(sa_ext->vx_regs, (void *) vx_sa, sizeof(sa_ext->vx_regs));
	memblock_free(vx_sa, PAGE_SIZE);
}

int smp_store_status(int cpu)
{
	unsigned long vx_sa;
	struct pcpu *pcpu;

	pcpu = pcpu_devices + cpu;
	if (__pcpu_sigp_relax(pcpu->address, SIGP_STOP_AND_STORE_STATUS,
			      0, NULL) != SIGP_CC_ORDER_CODE_ACCEPTED)
		return -EIO;
	if (!MACHINE_HAS_VX)
		return 0;
	vx_sa = __pa(pcpu->lowcore->vector_save_area_addr);
	__pcpu_sigp_relax(pcpu->address, SIGP_STORE_ADDITIONAL_STATUS,
			  vx_sa, NULL);
	return 0;
}

#endif /* CONFIG_CRASH_DUMP */

/*
 * Collect CPU state of the previous, crashed system.
 * There are four cases:
 * 1) standard zfcp dump
 *    condition: OLDMEM_BASE == NULL && ipl_info.type == IPL_TYPE_FCP_DUMP
 *    The state for all CPUs except the boot CPU needs to be collected
 *    with sigp stop-and-store-status. The boot CPU state is located in
 *    the absolute lowcore of the memory stored in the HSA. The zcore code
 *    will allocate the save area and copy the boot CPU state from the HSA.
 * 2) stand-alone kdump for SCSI (zfcp dump with swapped memory)
 *    condition: OLDMEM_BASE != NULL && ipl_info.type == IPL_TYPE_FCP_DUMP
 *    The state for all CPUs except the boot CPU needs to be collected
 *    with sigp stop-and-store-status. The firmware or the boot-loader
 *    stored the registers of the boot CPU in the absolute lowcore in the
 *    memory of the old system.
 * 3) kdump and the old kernel did not store the CPU state,
 *    or stand-alone kdump for DASD
 *    condition: OLDMEM_BASE != NULL && !is_kdump_kernel()
 *    The state for all CPUs except the boot CPU needs to be collected
 *    with sigp stop-and-store-status. The kexec code or the boot-loader
 *    stored the registers of the boot CPU in the memory of the old system.
 * 4) kdump and the old kernel stored the CPU state
 *    condition: OLDMEM_BASE != NULL && is_kdump_kernel()
 *    The state of all CPUs is stored in ELF sections in the memory of the
 *    old system. The ELF sections are picked up by the crash_dump code
 *    via elfcorehdr_addr.
 */
void __init smp_save_dump_cpus(void)
{
#ifdef CONFIG_CRASH_DUMP
	int addr, cpu, boot_cpu_addr, max_cpu_addr;
	struct save_area_ext *sa_ext;
	bool is_boot_cpu;

	if (is_kdump_kernel())
		/* Previous system stored the CPU states. Nothing to do. */
		return;
	if (!(OLDMEM_BASE || ipl_info.type == IPL_TYPE_FCP_DUMP))
		/* No previous system present, normal boot. */
		return;
	/* Set multi-threading state to the previous system. */
	pcpu_set_smt(sclp.mtid_prev);
	max_cpu_addr = SCLP_MAX_CORES << sclp.mtid_prev;
	for (cpu = 0, addr = 0; addr <= max_cpu_addr; addr++) {
		if (__pcpu_sigp_relax(addr, SIGP_SENSE, 0, NULL) ==
		    SIGP_CC_NOT_OPERATIONAL)
			continue;
		cpu += 1;
	}
	dump_save_areas.areas = (void *)memblock_alloc(sizeof(void *) * cpu, 8);
	dump_save_areas.count = cpu;
	boot_cpu_addr = stap();
	for (cpu = 0, addr = 0; addr <= max_cpu_addr; addr++) {
		if (__pcpu_sigp_relax(addr, SIGP_SENSE, 0, NULL) ==
		    SIGP_CC_NOT_OPERATIONAL)
			continue;
		sa_ext = (void *) memblock_alloc(sizeof(*sa_ext), 8);
		dump_save_areas.areas[cpu] = sa_ext;
		if (!sa_ext)
			panic("could not allocate memory for save area\n");
		is_boot_cpu = (addr == boot_cpu_addr);
		cpu += 1;
		if (is_boot_cpu && !OLDMEM_BASE)
			/* Skip boot CPU for standard zfcp dump. */
			continue;
		/* Get state for this CPU. */
		__smp_store_cpu_state(sa_ext, addr, is_boot_cpu);
	}
	diag308_reset();
	pcpu_set_smt(0);
#endif /* CONFIG_CRASH_DUMP */
}

void smp_cpu_set_polarization(int cpu, int val)
{
	pcpu_devices[cpu].polarization = val;
}

int smp_cpu_get_polarization(int cpu)
{
	return pcpu_devices[cpu].polarization;
}

static struct sclp_core_info *smp_get_core_info(void)
{
	static int use_sigp_detection;
	struct sclp_core_info *info;
	int address;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (info && (use_sigp_detection || sclp_get_core_info(info))) {
		use_sigp_detection = 1;
		for (address = 0;
		     address < (SCLP_MAX_CORES << smp_cpu_mt_shift);
		     address += (1U << smp_cpu_mt_shift)) {
			if (__pcpu_sigp_relax(address, SIGP_SENSE, 0, NULL) ==
			    SIGP_CC_NOT_OPERATIONAL)
				continue;
			info->core[info->configured].core_id =
				address >> smp_cpu_mt_shift;
			info->configured++;
		}
		info->combined = info->configured;
	}
	return info;
}

static int smp_add_present_cpu(int cpu);

static int smp_add_core(struct sclp_core_entry *core, cpumask_t *avail,
			bool configured, bool early)
{
	struct pcpu *pcpu;
	int cpu, nr, i;
	u16 address;

	nr = 0;
	if (sclp.has_core_type && core->type != boot_core_type)
		return nr;
	cpu = cpumask_first(avail);
	address = core->core_id << smp_cpu_mt_shift;
	for (i = 0; (i <= smp_cpu_mtid) && (cpu < nr_cpu_ids); i++) {
		if (pcpu_find_address(cpu_present_mask, address + i))
			continue;
		pcpu = pcpu_devices + cpu;
		pcpu->address = address + i;
		if (configured)
			pcpu->state = CPU_STATE_CONFIGURED;
		else
			pcpu->state = CPU_STATE_STANDBY;
		smp_cpu_set_polarization(cpu, POLARIZATION_UNKNOWN);
		set_cpu_present(cpu, true);
		if (!early && smp_add_present_cpu(cpu) != 0)
			set_cpu_present(cpu, false);
		else
			nr++;
		cpumask_clear_cpu(cpu, avail);
		cpu = cpumask_next(cpu, avail);
	}
	return nr;
}

static int __smp_rescan_cpus(struct sclp_core_info *info, bool early)
{
	struct sclp_core_entry *core;
	cpumask_t avail;
	bool configured;
	u16 core_id;
	int nr, i;

	nr = 0;
	cpumask_xor(&avail, cpu_possible_mask, cpu_present_mask);
	/*
	 * Add IPL core first (which got logical CPU number 0) to make sure
	 * that all SMT threads get subsequent logical CPU numbers.
	 */
	if (early) {
		core_id = pcpu_devices[0].address >> smp_cpu_mt_shift;
		for (i = 0; i < info->configured; i++) {
			core = &info->core[i];
			if (core->core_id == core_id) {
				nr += smp_add_core(core, &avail, true, early);
				break;
			}
		}
	}
	for (i = 0; i < info->combined; i++) {
		configured = i < info->configured;
		nr += smp_add_core(&info->core[i], &avail, configured, early);
	}
	return nr;
}

static void __init smp_detect_cpus(void)
{
	unsigned int cpu, mtid, c_cpus, s_cpus;
	struct sclp_core_info *info;
	u16 address;

	/* Get CPU information */
	info = smp_get_core_info();
	if (!info)
		panic("smp_detect_cpus failed to allocate memory\n");

	/* Find boot CPU type */
	if (sclp.has_core_type) {
		address = stap();
		for (cpu = 0; cpu < info->combined; cpu++)
			if (info->core[cpu].core_id == address) {
				/* The boot cpu dictates the cpu type. */
				boot_core_type = info->core[cpu].type;
				break;
			}
		if (cpu >= info->combined)
			panic("Could not find boot CPU type");
	}

	/* Set multi-threading state for the current system */
	mtid = boot_core_type ? sclp.mtid : sclp.mtid_cp;
	mtid = (mtid < smp_max_threads) ? mtid : smp_max_threads - 1;
	pcpu_set_smt(mtid);

	/* Print number of CPUs */
	c_cpus = s_cpus = 0;
	for (cpu = 0; cpu < info->combined; cpu++) {
		if (sclp.has_core_type &&
		    info->core[cpu].type != boot_core_type)
			continue;
		if (cpu < info->configured)
			c_cpus += smp_cpu_mtid + 1;
		else
			s_cpus += smp_cpu_mtid + 1;
	}
	pr_info("%d configured CPUs, %d standby CPUs\n", c_cpus, s_cpus);

	/* Add CPUs present at boot */
	get_online_cpus();
	__smp_rescan_cpus(info, true);
	put_online_cpus();
	kfree(info);
}

/*
 *	Activate a secondary processor.
 */
static void smp_start_secondary(void *cpuvoid)
{
	S390_lowcore.last_update_clock = get_tod_clock();
	S390_lowcore.restart_stack = (unsigned long) restart_stack;
	S390_lowcore.restart_fn = (unsigned long) do_restart;
	S390_lowcore.restart_data = 0;
	S390_lowcore.restart_source = -1UL;
	restore_access_regs(S390_lowcore.access_regs_save_area);
	__ctl_load(S390_lowcore.cregs_save_area, 0, 15);
	__load_psw_mask(PSW_KERNEL_BITS | PSW_MASK_DAT);
	cpu_init();
	preempt_disable();
	init_cpu_timer();
	vtime_init();
	pfault_init();
	notify_cpu_starting(smp_processor_id());
	set_cpu_online(smp_processor_id(), true);
	inc_irq_stat(CPU_RST);
	local_irq_enable();
	cpu_startup_entry(CPUHP_ONLINE);
}

/* Upping and downing of CPUs */
int __cpu_up(unsigned int cpu, struct task_struct *tidle)
{
	struct pcpu *pcpu = pcpu_devices + cpu;
	int rc;

	if (pcpu->state != CPU_STATE_CONFIGURED)
		return -EIO;
	if (pcpu_sigp_retry(pcpu, SIGP_INITIAL_CPU_RESET, 0) !=
	    SIGP_CC_ORDER_CODE_ACCEPTED)
		return -EIO;

	rc = pcpu_alloc_lowcore(pcpu, cpu);
	if (rc)
		return rc;
	pcpu_prepare_secondary(pcpu, cpu);
	pcpu_attach_task(pcpu, tidle);
	pcpu_start_fn(pcpu, smp_start_secondary, NULL);
	/* Wait until cpu puts itself in the online & active maps */
	while (!cpu_online(cpu) || !cpu_active(cpu))
		cpu_relax();
	return 0;
}

static unsigned int setup_possible_cpus __initdata;

static int __init _setup_possible_cpus(char *s)
{
	get_option(&s, &setup_possible_cpus);
	return 0;
}
early_param("possible_cpus", _setup_possible_cpus);

#ifdef CONFIG_HOTPLUG_CPU

int __cpu_disable(void)
{
	unsigned long cregs[16];

	/* Handle possible pending IPIs */
	smp_handle_ext_call();
	set_cpu_online(smp_processor_id(), false);
	/* Disable pseudo page faults on this cpu. */
	pfault_fini();
	/* Disable interrupt sources via control register. */
	__ctl_store(cregs, 0, 15);
	cregs[0]  &= ~0x0000ee70UL;	/* disable all external interrupts */
	cregs[6]  &= ~0xff000000UL;	/* disable all I/O interrupts */
	cregs[14] &= ~0x1f000000UL;	/* disable most machine checks */
	__ctl_load(cregs, 0, 15);
	clear_cpu_flag(CIF_NOHZ_DELAY);
	return 0;
}

void __cpu_die(unsigned int cpu)
{
	struct pcpu *pcpu;

	/* Wait until target cpu is down */
	pcpu = pcpu_devices + cpu;
	while (!pcpu_stopped(pcpu))
		cpu_relax();
	pcpu_free_lowcore(pcpu);
	atomic_dec(&init_mm.context.attach_count);
	cpumask_clear_cpu(cpu, mm_cpumask(&init_mm));
	if (MACHINE_HAS_TLB_LC)
		cpumask_clear_cpu(cpu, &init_mm.context.cpu_attach_mask);
}

void __noreturn cpu_die(void)
{
	idle_task_exit();
	__bpon();
	pcpu_sigp_retry(pcpu_devices + smp_processor_id(), SIGP_STOP, 0);
	for (;;) ;
}

#endif /* CONFIG_HOTPLUG_CPU */

void __init smp_fill_possible_mask(void)
{
	unsigned int possible, sclp_max, cpu;

	sclp_max = max(sclp.mtid, sclp.mtid_cp) + 1;
	sclp_max = min(smp_max_threads, sclp_max);
	sclp_max = sclp.max_cores * sclp_max ?: nr_cpu_ids;
	possible = setup_possible_cpus ?: nr_cpu_ids;
	possible = min(possible, sclp_max);
	for (cpu = 0; cpu < possible && cpu < nr_cpu_ids; cpu++)
		set_cpu_possible(cpu, true);
}

void __init smp_prepare_cpus(unsigned int max_cpus)
{
	/* request the 0x1201 emergency signal external interrupt */
	if (register_external_irq(EXT_IRQ_EMERGENCY_SIG, do_ext_call_interrupt))
		panic("Couldn't request external interrupt 0x1201");
	/* request the 0x1202 external call external interrupt */
	if (register_external_irq(EXT_IRQ_EXTERNAL_CALL, do_ext_call_interrupt))
		panic("Couldn't request external interrupt 0x1202");
	smp_detect_cpus();
}

void __init smp_prepare_boot_cpu(void)
{
	struct pcpu *pcpu = pcpu_devices;

	pcpu->state = CPU_STATE_CONFIGURED;
	pcpu->address = stap();
	pcpu->lowcore = (struct _lowcore *)(unsigned long) store_prefix();
	S390_lowcore.percpu_offset = __per_cpu_offset[0];
	smp_cpu_set_polarization(0, POLARIZATION_UNKNOWN);
	set_cpu_present(0, true);
	set_cpu_online(0, true);
}

void __init smp_cpus_done(unsigned int max_cpus)
{
}

void __init smp_setup_processor_id(void)
{
	S390_lowcore.cpu_nr = 0;
	S390_lowcore.spinlock_lockval = arch_spin_lockval(0);
}

/*
 * the frequency of the profiling timer can be changed
 * by writing a multiplier value into /proc/profile.
 *
 * usually you want to run this on all CPUs ;)
 */
int setup_profiling_timer(unsigned int multiplier)
{
	return 0;
}

#ifdef CONFIG_HOTPLUG_CPU
static ssize_t cpu_configure_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	ssize_t count;

	mutex_lock(&smp_cpu_state_mutex);
	count = sprintf(buf, "%d\n", pcpu_devices[dev->id].state);
	mutex_unlock(&smp_cpu_state_mutex);
	return count;
}

static ssize_t cpu_configure_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct pcpu *pcpu;
	int cpu, val, rc, i;
	char delim;

	if (sscanf(buf, "%d %c", &val, &delim) != 1)
		return -EINVAL;
	if (val != 0 && val != 1)
		return -EINVAL;
	get_online_cpus();
	mutex_lock(&smp_cpu_state_mutex);
	rc = -EBUSY;
	/* disallow configuration changes of online cpus and cpu 0 */
	cpu = dev->id;
	cpu = smp_get_base_cpu(cpu);
	if (cpu == 0)
		goto out;
	for (i = 0; i <= smp_cpu_mtid; i++)
		if (cpu_online(cpu + i))
			goto out;
	pcpu = pcpu_devices + cpu;
	rc = 0;
	switch (val) {
	case 0:
		if (pcpu->state != CPU_STATE_CONFIGURED)
			break;
		rc = sclp_core_deconfigure(pcpu->address >> smp_cpu_mt_shift);
		if (rc)
			break;
		for (i = 0; i <= smp_cpu_mtid; i++) {
			if (cpu + i >= nr_cpu_ids || !cpu_present(cpu + i))
				continue;
			pcpu[i].state = CPU_STATE_STANDBY;
			smp_cpu_set_polarization(cpu + i,
						 POLARIZATION_UNKNOWN);
		}
		topology_expect_change();
		break;
	case 1:
		if (pcpu->state != CPU_STATE_STANDBY)
			break;
		rc = sclp_core_configure(pcpu->address >> smp_cpu_mt_shift);
		if (rc)
			break;
		for (i = 0; i <= smp_cpu_mtid; i++) {
			if (cpu + i >= nr_cpu_ids || !cpu_present(cpu + i))
				continue;
			pcpu[i].state = CPU_STATE_CONFIGURED;
			smp_cpu_set_polarization(cpu + i,
						 POLARIZATION_UNKNOWN);
		}
		topology_expect_change();
		break;
	default:
		break;
	}
out:
	mutex_unlock(&smp_cpu_state_mutex);
	put_online_cpus();
	return rc ? rc : count;
}
static DEVICE_ATTR(configure, 0644, cpu_configure_show, cpu_configure_store);
#endif /* CONFIG_HOTPLUG_CPU */

static ssize_t show_cpu_address(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", pcpu_devices[dev->id].address);
}
static DEVICE_ATTR(address, 0444, show_cpu_address, NULL);

static struct attribute *cpu_common_attrs[] = {
#ifdef CONFIG_HOTPLUG_CPU
	&dev_attr_configure.attr,
#endif
	&dev_attr_address.attr,
	NULL,
};

static struct attribute_group cpu_common_attr_group = {
	.attrs = cpu_common_attrs,
};

static struct attribute *cpu_online_attrs[] = {
	&dev_attr_idle_count.attr,
	&dev_attr_idle_time_us.attr,
	NULL,
};

static struct attribute_group cpu_online_attr_group = {
	.attrs = cpu_online_attrs,
};

static int smp_cpu_notify(struct notifier_block *self, unsigned long action,
			  void *hcpu)
{
	unsigned int cpu = (unsigned int)(long)hcpu;
	struct device *s = &per_cpu(cpu_device, cpu)->dev;
	int err = 0;

	switch (action & ~CPU_TASKS_FROZEN) {
	case CPU_ONLINE:
		err = sysfs_create_group(&s->kobj, &cpu_online_attr_group);
		break;
	case CPU_DEAD:
		sysfs_remove_group(&s->kobj, &cpu_online_attr_group);
		break;
	}
	return notifier_from_errno(err);
}

static int smp_add_present_cpu(int cpu)
{
	struct device *s;
	struct cpu *c;
	int rc;

	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	per_cpu(cpu_device, cpu) = c;
	s = &c->dev;
	c->hotpluggable = 1;
	rc = register_cpu(c, cpu);
	if (rc)
		goto out;
	rc = sysfs_create_group(&s->kobj, &cpu_common_attr_group);
	if (rc)
		goto out_cpu;
	if (cpu_online(cpu)) {
		rc = sysfs_create_group(&s->kobj, &cpu_online_attr_group);
		if (rc)
			goto out_online;
	}
	rc = topology_cpu_init(c);
	if (rc)
		goto out_topology;
	return 0;

out_topology:
	if (cpu_online(cpu))
		sysfs_remove_group(&s->kobj, &cpu_online_attr_group);
out_online:
	sysfs_remove_group(&s->kobj, &cpu_common_attr_group);
out_cpu:
#ifdef CONFIG_HOTPLUG_CPU
	unregister_cpu(c);
#endif
out:
	return rc;
}

#ifdef CONFIG_HOTPLUG_CPU

int __ref smp_rescan_cpus(void)
{
	struct sclp_core_info *info;
	int nr;

	info = smp_get_core_info();
	if (!info)
		return -ENOMEM;
	get_online_cpus();
	mutex_lock(&smp_cpu_state_mutex);
	nr = __smp_rescan_cpus(info, false);
	mutex_unlock(&smp_cpu_state_mutex);
	put_online_cpus();
	kfree(info);
	if (nr)
		topology_schedule_update();
	return 0;
}

static ssize_t __ref rescan_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf,
				  size_t count)
{
	int rc;

	rc = lock_device_hotplug_sysfs();
	if (rc)
		return rc;
	rc = smp_rescan_cpus();
	unlock_device_hotplug();
	return rc ? rc : count;
}
static DEVICE_ATTR(rescan, 0200, NULL, rescan_store);
#endif /* CONFIG_HOTPLUG_CPU */

static int __init s390_smp_init(void)
{
	int cpu, rc = 0;

#ifdef CONFIG_HOTPLUG_CPU
	rc = device_create_file(cpu_subsys.dev_root, &dev_attr_rescan);
	if (rc)
		return rc;
#endif
	cpu_notifier_register_begin();
	for_each_present_cpu(cpu) {
		rc = smp_add_present_cpu(cpu);
		if (rc)
			goto out;
	}

	__hotcpu_notifier(smp_cpu_notify, 0);

out:
	cpu_notifier_register_done();
	return rc;
}
subsys_initcall(s390_smp_init);
