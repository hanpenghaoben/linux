/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_TLBFLUSH_H
#define _ASM_X86_TLBFLUSH_H

#include <linux/mm.h>
#include <linux/sched.h>

#include <asm/processor.h>
#include <asm/cpufeature.h>
#include <asm/special_insns.h>
#include <asm/smp.h>
#include <asm/invpcid.h>

static inline u64 inc_mm_tlb_gen(struct mm_struct *mm)
{
	/*
	 * Bump the generation count.  This also serves as a full barrier
	 * that synchronizes with switch_mm(): callers are required to order
	 * their read of mm_cpumask after their writes to the paging
	 * structures.
	 */
	return atomic64_inc_return(&mm->context.tlb_gen);
}

/* There are 12 bits of space for ASIDS in CR3 */
#define CR3_HW_ASID_BITS		12
/*
 * When enabled, PAGE_TABLE_ISOLATION consumes a single bit for
 * user/kernel switches
 */
#define PTI_CONSUMED_ASID_BITS		0

#define CR3_AVAIL_ASID_BITS (CR3_HW_ASID_BITS - PTI_CONSUMED_ASID_BITS)
/*
 * ASIDs are zero-based: 0->MAX_AVAIL_ASID are valid.  -1 below to account
 * for them being zero-based.  Another -1 is because ASID 0 is reserved for
 * use by non-PCID-aware users.
 */
#define MAX_ASID_AVAILABLE ((1 << CR3_AVAIL_ASID_BITS) - 2)

static inline u16 kern_pcid(u16 asid)
{
	VM_WARN_ON_ONCE(asid > MAX_ASID_AVAILABLE);
	/*
	 * If PCID is on, ASID-aware code paths put the ASID+1 into the
	 * PCID bits.  This serves two purposes.  It prevents a nasty
	 * situation in which PCID-unaware code saves CR3, loads some other
	 * value (with PCID == 0), and then restores CR3, thus corrupting
	 * the TLB for ASID 0 if the saved ASID was nonzero.  It also means
	 * that any bugs involving loading a PCID-enabled CR3 with
	 * CR4.PCIDE off will trigger deterministically.
	 */
	return asid + 1;
}

struct pgd_t;
static inline unsigned long build_cr3(pgd_t *pgd, u16 asid)
{
	if (static_cpu_has(X86_FEATURE_PCID)) {
		return __sme_pa(pgd) | kern_pcid(asid);
	} else {
		VM_WARN_ON_ONCE(asid != 0);
		return __sme_pa(pgd);
	}
}

static inline unsigned long build_cr3_noflush(pgd_t *pgd, u16 asid)
{
	VM_WARN_ON_ONCE(asid > MAX_ASID_AVAILABLE);
	VM_WARN_ON_ONCE(!this_cpu_has(X86_FEATURE_PCID));
	return __sme_pa(pgd) | kern_pcid(asid) | CR3_NOFLUSH;
}

#ifdef CONFIG_PARAVIRT
#include <asm/paravirt.h>
#else
#define __flush_tlb() __native_flush_tlb()
#define __flush_tlb_global() __native_flush_tlb_global()
#define __flush_tlb_single(addr) __native_flush_tlb_single(addr)
#endif

static inline bool tlb_defer_switch_to_init_mm(void)
{
	/*
	 * If we have PCID, then switching to init_mm is reasonably
	 * fast.  If we don't have PCID, then switching to init_mm is
	 * quite slow, so we try to defer it in the hopes that we can
	 * avoid it entirely.  The latter approach runs the risk of
	 * receiving otherwise unnecessary IPIs.
	 *
	 * This choice is just a heuristic.  The tlb code can handle this
	 * function returning true or false regardless of whether we have
	 * PCID.
	 */
	return !static_cpu_has(X86_FEATURE_PCID);
}

/*
 * 6 because 6 should be plenty and struct tlb_state will fit in
 * two cache lines.
 */
#define TLB_NR_DYN_ASIDS 6

struct tlb_context {
	u64 ctx_id;
	u64 tlb_gen;
};

struct tlb_state {
	/*
	 * cpu_tlbstate.loaded_mm should match CR3 whenever interrupts
	 * are on.  This means that it may not match current->active_mm,
	 * which will contain the previous user mm when we're in lazy TLB
	 * mode even if we've already switched back to swapper_pg_dir.
	 */
	struct mm_struct *loaded_mm;
	u16 loaded_mm_asid;
	u16 next_asid;

	/*
	 * We can be in one of several states:
	 *
	 *  - Actively using an mm.  Our CPU's bit will be set in
	 *    mm_cpumask(loaded_mm) and is_lazy == false;
	 *
	 *  - Not using a real mm.  loaded_mm == &init_mm.  Our CPU's bit
	 *    will not be set in mm_cpumask(&init_mm) and is_lazy == false.
	 *
	 *  - Lazily using a real mm.  loaded_mm != &init_mm, our bit
	 *    is set in mm_cpumask(loaded_mm), but is_lazy == true.
	 *    We're heuristically guessing that the CR3 load we
	 *    skipped more than makes up for the overhead added by
	 *    lazy mode.
	 */
	bool is_lazy;

	/*
	 * Access to this CR4 shadow and to H/W CR4 is protected by
	 * disabling interrupts when modifying either one.
	 */
	unsigned long cr4;

	/*
	 * This is a list of all contexts that might exist in the TLB.
	 * There is one per ASID that we use, and the ASID (what the
	 * CPU calls PCID) is the index into ctxts.
	 *
	 * For each context, ctx_id indicates which mm the TLB's user
	 * entries came from.  As an invariant, the TLB will never
	 * contain entries that are out-of-date as when that mm reached
	 * the tlb_gen in the list.
	 *
	 * To be clear, this means that it's legal for the TLB code to
	 * flush the TLB without updating tlb_gen.  This can happen
	 * (for now, at least) due to paravirt remote flushes.
	 *
	 * NB: context 0 is a bit special, since it's also used by
	 * various bits of init code.  This is fine -- code that
	 * isn't aware of PCID will end up harmlessly flushing
	 * context 0.
	 */
	struct tlb_context ctxs[TLB_NR_DYN_ASIDS];
};
DECLARE_PER_CPU_SHARED_ALIGNED(struct tlb_state, cpu_tlbstate);

/* Initialize cr4 shadow for this CPU. */
static inline void cr4_init_shadow(void)
{
	this_cpu_write(cpu_tlbstate.cr4, __read_cr4());
}

static inline void __cr4_set(unsigned long cr4)
{
	lockdep_assert_irqs_disabled();
	this_cpu_write(cpu_tlbstate.cr4, cr4);
	__write_cr4(cr4);
}

/* Set in this cpu's CR4. */
static inline void cr4_set_bits(unsigned long mask)
{
	unsigned long cr4, flags;

	local_irq_save(flags);
	cr4 = this_cpu_read(cpu_tlbstate.cr4);
	if ((cr4 | mask) != cr4)
		__cr4_set(cr4 | mask);
	local_irq_restore(flags);
}

/* Clear in this cpu's CR4. */
static inline void cr4_clear_bits(unsigned long mask)
{
	unsigned long cr4, flags;

	local_irq_save(flags);
	cr4 = this_cpu_read(cpu_tlbstate.cr4);
	if ((cr4 & ~mask) != cr4)
		__cr4_set(cr4 & ~mask);
	local_irq_restore(flags);
}

static inline void cr4_toggle_bits_irqsoff(unsigned long mask)
{
	unsigned long cr4;

	cr4 = this_cpu_read(cpu_tlbstate.cr4);
	__cr4_set(cr4 ^ mask);
}

/* Read the CR4 shadow. */
static inline unsigned long cr4_read_shadow(void)
{
	return this_cpu_read(cpu_tlbstate.cr4);
}

/*
 * Save some of cr4 feature set we're using (e.g.  Pentium 4MB
 * enable and PPro Global page enable), so that any CPU's that boot
 * up after us can get the correct flags.  This should only be used
 * during boot on the boot cpu.
 */
extern unsigned long mmu_cr4_features;
extern u32 *trampoline_cr4_features;

static inline void cr4_set_bits_and_update_boot(unsigned long mask)
{
	mmu_cr4_features |= mask;
	if (trampoline_cr4_features)
		*trampoline_cr4_features = mmu_cr4_features;
	cr4_set_bits(mask);
}

extern void initialize_tlbstate_and_flush(void);

/*
 * flush the entire current user mapping
 */
static inline void __native_flush_tlb(void)
{
	/*
	 * If current->mm == NULL then we borrow a mm which may change during a
	 * task switch and therefore we must not be preempted while we write CR3
	 * back:
	 */
	preempt_disable();
	native_write_cr3(__native_read_cr3());
	preempt_enable();
}

/*
 * flush everything
 */
static inline void __native_flush_tlb_global(void)
{
	unsigned long cr4, flags;

	if (static_cpu_has(X86_FEATURE_INVPCID)) {
		/*
		 * Using INVPCID is considerably faster than a pair of writes
		 * to CR4 sandwiched inside an IRQ flag save/restore.
		 */
		invpcid_flush_all();
		return;
	}

	/*
	 * Read-modify-write to CR4 - protect it from preemption and
	 * from interrupts. (Use the raw variant because this code can
	 * be called from deep inside debugging code.)
	 */
	raw_local_irq_save(flags);

	cr4 = this_cpu_read(cpu_tlbstate.cr4);
	/* toggle PGE */
	native_write_cr4(cr4 ^ X86_CR4_PGE);
	/* write old PGE again and flush TLBs */
	native_write_cr4(cr4);

	raw_local_irq_restore(flags);
}

/*
 * flush one page in the user mapping
 */
static inline void __native_flush_tlb_single(unsigned long addr)
{
	asm volatile("invlpg (%0)" ::"r" (addr) : "memory");
}

/*
 * flush everything
 */
static inline void __flush_tlb_all(void)
{
	if (boot_cpu_has(X86_FEATURE_PGE)) {
		__flush_tlb_global();
	} else {
		/*
		 * !PGE -> !PCID (setup_pcid()), thus every flush is total.
		 */
		__flush_tlb();
	}

	/*
	 * Note: if we somehow had PCID but not PGE, then this wouldn't work --
	 * we'd end up flushing kernel translations for the current ASID but
	 * we might fail to flush kernel translations for other cached ASIDs.
	 *
	 * To avoid this issue, we force PCID off if PGE is off.
	 */
}

/*
 * flush one page in the kernel mapping
 */
static inline void __flush_tlb_one(unsigned long addr)
{
	count_vm_tlb_event(NR_TLB_LOCAL_FLUSH_ONE);
	__flush_tlb_single(addr);
}

#define TLB_FLUSH_ALL	-1UL

/*
 * TLB flushing:
 *
 *  - flush_tlb_all() flushes all processes TLBs
 *  - flush_tlb_mm(mm) flushes the specified mm context TLB's
 *  - flush_tlb_page(vma, vmaddr) flushes one page
 *  - flush_tlb_range(vma, start, end) flushes a range of pages
 *  - flush_tlb_kernel_range(start, end) flushes a range of kernel pages
 *  - flush_tlb_others(cpumask, info) flushes TLBs on other cpus
 *
 * ..but the i386 has somewhat limited tlb flushing capabilities,
 * and page-granular flushes are available only on i486 and up.
 */
struct flush_tlb_info {
	/*
	 * We support several kinds of flushes.
	 *
	 * - Fully flush a single mm.  .mm will be set, .end will be
	 *   TLB_FLUSH_ALL, and .new_tlb_gen will be the tlb_gen to
	 *   which the IPI sender is trying to catch us up.
	 *
	 * - Partially flush a single mm.  .mm will be set, .start and
	 *   .end will indicate the range, and .new_tlb_gen will be set
	 *   such that the changes between generation .new_tlb_gen-1 and
	 *   .new_tlb_gen are entirely contained in the indicated range.
	 *
	 * - Fully flush all mms whose tlb_gens have been updated.  .mm
	 *   will be NULL, .end will be TLB_FLUSH_ALL, and .new_tlb_gen
	 *   will be zero.
	 */
	struct mm_struct	*mm;
	unsigned long		start;
	unsigned long		end;
	u64			new_tlb_gen;
};

#define local_flush_tlb() __flush_tlb()

#define flush_tlb_mm(mm)	flush_tlb_mm_range(mm, 0UL, TLB_FLUSH_ALL, 0UL)

#define flush_tlb_range(vma, start, end)	\
		flush_tlb_mm_range(vma->vm_mm, start, end, vma->vm_flags)

extern void flush_tlb_all(void);
extern void flush_tlb_mm_range(struct mm_struct *mm, unsigned long start,
				unsigned long end, unsigned long vmflag);
extern void flush_tlb_kernel_range(unsigned long start, unsigned long end);

static inline void flush_tlb_page(struct vm_area_struct *vma, unsigned long a)
{
	flush_tlb_mm_range(vma->vm_mm, a, a + PAGE_SIZE, VM_NONE);
}

void native_flush_tlb_others(const struct cpumask *cpumask,
			     const struct flush_tlb_info *info);

static inline void arch_tlbbatch_add_mm(struct arch_tlbflush_unmap_batch *batch,
					struct mm_struct *mm)
{
	inc_mm_tlb_gen(mm);
	cpumask_or(&batch->cpumask, &batch->cpumask, mm_cpumask(mm));
}

extern void arch_tlbbatch_flush(struct arch_tlbflush_unmap_batch *batch);

#ifndef CONFIG_PARAVIRT
#define flush_tlb_others(mask, info)	\
	native_flush_tlb_others(mask, info)
#endif

#endif /* _ASM_X86_TLBFLUSH_H */
