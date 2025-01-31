/*
 * Copyright (C) 2012,2013 - ARM Ltd
 * Author: Marc Zyngier <marc.zyngier@arm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __ARM64_KVM_MMU_H__
#define __ARM64_KVM_MMU_H__

#include <asm/page.h>
#include <asm/memory.h>
#include <asm/cpufeature.h>

/*
 * As ARMv8.0 only has the TTBR0_EL2 register, we cannot express
 * "negative" addresses. This makes it impossible to directly share
 * mappings with the kernel.
 *
 * Instead, give the HYP mode its own VA region at a fixed offset from
 * the kernel by just masking the top bits (which are all ones for a
 * kernel address). We need to find out how many bits to mask.
 *
 * We want to build a set of page tables that cover both parts of the
 * idmap (the trampoline page used to initialize EL2), and our normal
 * runtime VA space, at the same time.
 *
 * Given that the kernel uses VA_BITS for its entire address space,
 * and that half of that space (VA_BITS - 1) is used for the linear
 * mapping, we can also limit the EL2 space to (VA_BITS - 1).
 *
 * The main question is "Within the VA_BITS space, does EL2 use the
 * top or the bottom half of that space to shadow the kernel's linear
 * mapping?". As we need to idmap the trampoline page, this is
 * determined by the range in which this page lives.
 *
 * If the page is in the bottom half, we have to use the top half. If
 * the page is in the top half, we have to use the bottom half:
 *
 * T = __pa_symbol(__hyp_idmap_text_start)
 * if (T & BIT(VA_BITS - 1))
 *	HYP_VA_MIN = 0  //idmap in upper half
 * else
 *	HYP_VA_MIN = 1 << (VA_BITS - 1)
 * HYP_VA_MAX = HYP_VA_MIN + (1 << (VA_BITS - 1)) - 1
 *
 * This of course assumes that the trampoline page exists within the
 * VA_BITS range. If it doesn't, then it means we're in the odd case
 * where the kernel idmap (as well as HYP) uses more levels than the
 * kernel runtime page tables (as seen when the kernel is configured
 * for 4k pages, 39bits VA, and yet memory lives just above that
 * limit, forcing the idmap to use 4 levels of page tables while the
 * kernel itself only uses 3). In this particular case, it doesn't
 * matter which side of VA_BITS we use, as we're guaranteed not to
 * conflict with anything.
 *
 * When using VHE, there are no separate hyp mappings and all KVM
 * functionality is already mapped as part of the main kernel
 * mappings, and none of this applies in that case.
 */

#define HYP_PAGE_OFFSET_HIGH_MASK	((UL(1) << VA_BITS) - 1)
#define HYP_PAGE_OFFSET_LOW_MASK	((UL(1) << (VA_BITS - 1)) - 1)

#ifdef __ASSEMBLY__

#include <asm/alternative.h>
#include <asm/cpufeature.h>

/*
 * Convert a kernel VA into a HYP VA.
 * reg: VA to be converted.
 *
 * This generates the following sequences:
 * - High mask:
 *		and x0, x0, #HYP_PAGE_OFFSET_HIGH_MASK
 *		nop
 * - Low mask:
 *		and x0, x0, #HYP_PAGE_OFFSET_HIGH_MASK
 *		and x0, x0, #HYP_PAGE_OFFSET_LOW_MASK
 * - VHE:
 *		nop
 *		nop
 *
 * The "low mask" version works because the mask is a strict subset of
 * the "high mask", hence performing the first mask for nothing.
 * Should be completely invisible on any viable CPU.
 */
.macro kern_hyp_va	reg
alternative_if_not ARM64_HAS_VIRT_HOST_EXTN
	and     \reg, \reg, #HYP_PAGE_OFFSET_HIGH_MASK
alternative_else_nop_endif
alternative_if ARM64_HYP_OFFSET_LOW
	and     \reg, \reg, #HYP_PAGE_OFFSET_LOW_MASK
alternative_else_nop_endif
.endm

#else

#include <asm/pgalloc.h>
#include <asm/cache.h>
#include <asm/cacheflush.h>
#include <asm/mmu_context.h>
#include <asm/pgtable.h>

static inline unsigned long __kern_hyp_va(unsigned long v)
{
	asm volatile(ALTERNATIVE("and %0, %0, %1",
				 "nop",
				 ARM64_HAS_VIRT_HOST_EXTN)
		     : "+r" (v)
		     : "i" (HYP_PAGE_OFFSET_HIGH_MASK));
	asm volatile(ALTERNATIVE("nop",
				 "and %0, %0, %1",
				 ARM64_HYP_OFFSET_LOW)
		     : "+r" (v)
		     : "i" (HYP_PAGE_OFFSET_LOW_MASK));
	return v;
}

#define kern_hyp_va(v) 	((typeof(v))(__kern_hyp_va((unsigned long)(v))))

/*
 * Obtain the PC-relative address of a kernel symbol
 * s: symbol
 *
 * The goal of this macro is to return a symbol's address based on a
 * PC-relative computation, as opposed to a loading the VA from a
 * constant pool or something similar. This works well for HYP, as an
 * absolute VA is guaranteed to be wrong. Only use this if trying to
 * obtain the address of a symbol (i.e. not something you obtained by
 * following a pointer).
 */
#define hyp_symbol_addr(s)						\
	({								\
		typeof(s) *addr;					\
		asm("adrp	%0, %1\n"				\
		    "add	%0, %0, :lo12:%1\n"			\
		    : "=r" (addr) : "S" (&s));				\
		addr;							\
	})

/*
 * We currently only support a 40bit IPA.
 */
#define KVM_PHYS_SHIFT	(40)
#define KVM_PHYS_SIZE	(1UL << KVM_PHYS_SHIFT)
#define KVM_PHYS_MASK	(KVM_PHYS_SIZE - 1UL)

#include <asm/stage2_pgtable.h>

int create_hyp_mappings(void *from, void *to, pgprot_t prot);
int create_hyp_io_mappings(void *from, void *to, phys_addr_t);
void free_hyp_pgds(void);

void stage2_unmap_vm(struct kvm *kvm);
int kvm_alloc_stage2_pgd(struct kvm *kvm);
void kvm_free_stage2_pgd(struct kvm *kvm);
int kvm_phys_addr_ioremap(struct kvm *kvm, phys_addr_t guest_ipa,
			  phys_addr_t pa, unsigned long size, bool writable);

int kvm_handle_guest_abort(struct kvm_vcpu *vcpu, struct kvm_run *run);

void kvm_mmu_free_memory_caches(struct kvm_vcpu *vcpu);

phys_addr_t kvm_mmu_get_httbr(void);
phys_addr_t kvm_get_idmap_vector(void);
int kvm_mmu_init(void);
void kvm_clear_hyp_idmap(void);

#define	kvm_set_pte(ptep, pte)		set_pte(ptep, pte)
#define	kvm_set_pmd(pmdp, pmd)		set_pmd(pmdp, pmd)

static inline pte_t kvm_s2pte_mkwrite(pte_t pte)
{
	pte_val(pte) |= PTE_S2_RDWR;
	return pte;
}

static inline pmd_t kvm_s2pmd_mkwrite(pmd_t pmd)
{
	pmd_val(pmd) |= PMD_S2_RDWR;
	return pmd;
}

static inline void kvm_set_s2pte_readonly(pte_t *ptep)
{
	pteval_t old_pteval, pteval;

	pteval = READ_ONCE(pte_val(*ptep));
	do {
		old_pteval = pteval;
		pteval &= ~PTE_S2_RDWR;
		pteval |= PTE_S2_RDONLY;
		pteval = cmpxchg_relaxed(&pte_val(*ptep), old_pteval, pteval);
	} while (pteval != old_pteval);
}

static inline bool kvm_s2pte_readonly(pte_t *ptep)
{
	return (READ_ONCE(pte_val(*ptep)) & PTE_S2_RDWR) == PTE_S2_RDONLY;
}

static inline void kvm_set_s2pmd_readonly(pmd_t *pmdp)
{
	kvm_set_s2pte_readonly((pte_t *)pmdp);
}

static inline bool kvm_s2pmd_readonly(pmd_t *pmdp)
{
	return kvm_s2pte_readonly((pte_t *)pmdp);
}

static inline bool kvm_page_empty(void *ptr)
{
	struct page *ptr_page = virt_to_page(ptr);
	return page_count(ptr_page) == 1;
}

#define hyp_pte_table_empty(ptep) kvm_page_empty(ptep)

#ifdef __PAGETABLE_PMD_FOLDED
#define hyp_pmd_table_empty(pmdp) (0)
#else
#define hyp_pmd_table_empty(pmdp) kvm_page_empty(pmdp)
#endif

#ifdef __PAGETABLE_PUD_FOLDED
#define hyp_pud_table_empty(pudp) (0)
#else
#define hyp_pud_table_empty(pudp) kvm_page_empty(pudp)
#endif

struct kvm;

#define kvm_flush_dcache_to_poc(a,l)	__flush_dcache_area((a), (l))

static inline bool vcpu_has_cache_enabled(struct kvm_vcpu *vcpu)
{
	return (vcpu_sys_reg(vcpu, SCTLR_EL1) & 0b101) == 0b101;
}

static inline void __coherent_cache_guest_page(struct kvm_vcpu *vcpu,
					       kvm_pfn_t pfn,
					       unsigned long size)
{
	void *va = page_address(pfn_to_page(pfn));

	kvm_flush_dcache_to_poc(va, size);

	if (icache_is_aliasing()) {
		/* any kind of VIPT cache */
		__flush_icache_all();
	} else if (is_kernel_in_hyp_mode() || !icache_is_vpipt()) {
		/* PIPT or VPIPT at EL2 (see comment in __kvm_tlb_flush_vmid_ipa) */
		flush_icache_range((unsigned long)va,
				   (unsigned long)va + size);
	}
}

static inline void __kvm_flush_dcache_pte(pte_t pte)
{
	struct page *page = pte_page(pte);
	kvm_flush_dcache_to_poc(page_address(page), PAGE_SIZE);
}

static inline void __kvm_flush_dcache_pmd(pmd_t pmd)
{
	struct page *page = pmd_page(pmd);
	kvm_flush_dcache_to_poc(page_address(page), PMD_SIZE);
}

static inline void __kvm_flush_dcache_pud(pud_t pud)
{
	struct page *page = pud_page(pud);
	kvm_flush_dcache_to_poc(page_address(page), PUD_SIZE);
}

#define kvm_virt_to_phys(x)		__pa_symbol(x)

void kvm_set_way_flush(struct kvm_vcpu *vcpu);
void kvm_toggle_cache(struct kvm_vcpu *vcpu, bool was_enabled);

static inline bool __kvm_cpu_uses_extended_idmap(void)
{
	return __cpu_uses_extended_idmap();
}

static inline void __kvm_extend_hypmap(pgd_t *boot_hyp_pgd,
				       pgd_t *hyp_pgd,
				       pgd_t *merged_hyp_pgd,
				       unsigned long hyp_idmap_start)
{
	int idmap_idx;

	/*
	 * Use the first entry to access the HYP mappings. It is
	 * guaranteed to be free, otherwise we wouldn't use an
	 * extended idmap.
	 */
	VM_BUG_ON(pgd_val(merged_hyp_pgd[0]));
	merged_hyp_pgd[0] = __pgd(__pa(hyp_pgd) | PMD_TYPE_TABLE);

	/*
	 * Create another extended level entry that points to the boot HYP map,
	 * which contains an ID mapping of the HYP init code. We essentially
	 * merge the boot and runtime HYP maps by doing so, but they don't
	 * overlap anyway, so this is fine.
	 */
	idmap_idx = hyp_idmap_start >> VA_BITS;
	VM_BUG_ON(pgd_val(merged_hyp_pgd[idmap_idx]));
	merged_hyp_pgd[idmap_idx] = __pgd(__pa(boot_hyp_pgd) | PMD_TYPE_TABLE);
}

static inline unsigned int kvm_get_vmid_bits(void)
{
	int reg = read_sanitised_ftr_reg(SYS_ID_AA64MMFR1_EL1);

	return (cpuid_feature_extract_unsigned_field(reg, ID_AA64MMFR1_VMIDBITS_SHIFT) == 2) ? 16 : 8;
}

/*
 * We are not in the kvm->srcu critical section most of the time, so we take
 * the SRCU read lock here. Since we copy the data from the user page, we
 * can immediately drop the lock again.
 */
static inline int kvm_read_guest_lock(struct kvm *kvm,
				      gpa_t gpa, void *data, unsigned long len)
{
	int srcu_idx = srcu_read_lock(&kvm->srcu);
	int ret = kvm_read_guest(kvm, gpa, data, len);

	srcu_read_unlock(&kvm->srcu, srcu_idx);

	return ret;
}

#ifdef CONFIG_HARDEN_BRANCH_PREDICTOR
#include <asm/mmu.h>

static inline void *kvm_get_hyp_vector(void)
{
	struct bp_hardening_data *data = arm64_get_bp_hardening_data();
	void *vect = kvm_ksym_ref(__kvm_hyp_vector);

	if (data->fn) {
		vect = __bp_harden_hyp_vecs_start +
		       data->hyp_vectors_slot * SZ_2K;

		if (!has_vhe())
			vect = lm_alias(vect);
	}

	return vect;
}

static inline int kvm_map_vectors(void)
{
	return create_hyp_mappings(kvm_ksym_ref(__bp_harden_hyp_vecs_start),
				   kvm_ksym_ref(__bp_harden_hyp_vecs_end),
				   PAGE_HYP_EXEC);
}

#else
static inline void *kvm_get_hyp_vector(void)
{
	return kvm_ksym_ref(__kvm_hyp_vector);
}

static inline int kvm_map_vectors(void)
{
	return 0;
}
#endif

#ifdef CONFIG_ARM64_SSBD
DECLARE_PER_CPU_READ_MOSTLY(u64, arm64_ssbd_callback_required);

static inline int hyp_map_aux_data(void)
{
	int cpu, err;

	for_each_possible_cpu(cpu) {
		u64 *ptr;

		ptr = per_cpu_ptr(&arm64_ssbd_callback_required, cpu);
		err = create_hyp_mappings(ptr, ptr + 1, PAGE_HYP);
		if (err)
			return err;
	}
	return 0;
}
#else
static inline int hyp_map_aux_data(void)
{
	return 0;
}
#endif

#endif /* __ASSEMBLY__ */
#endif /* __ARM64_KVM_MMU_H__ */
