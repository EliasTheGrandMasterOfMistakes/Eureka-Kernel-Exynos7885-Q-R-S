/*
 * x86 FPU boot time init code:
 */
#include <asm/fpu/internal.h>
#include <asm/tlbflush.h>
#include <asm/setup.h>
#include <asm/cmdline.h>

#include <linux/sched.h>
#include <linux/init.h>

/*
 * Initialize the TS bit in CR0 according to the style of context-switches
 * we are using:
 */
static void fpu__init_cpu_ctx_switch(void)
{
	clts();
}

/*
 * Initialize the registers found in all CPUs, CR0 and CR4:
 */
static void fpu__init_cpu_generic(void)
{
	unsigned long cr0;
	unsigned long cr4_mask = 0;

	if (cpu_has_fxsr)
		cr4_mask |= X86_CR4_OSFXSR;
	if (cpu_has_xmm)
		cr4_mask |= X86_CR4_OSXMMEXCPT;
	if (cr4_mask)
		cr4_set_bits(cr4_mask);

	cr0 = read_cr0();
	cr0 &= ~(X86_CR0_TS|X86_CR0_EM); /* clear TS and EM */
	if (!cpu_has_fpu)
		cr0 |= X86_CR0_EM;
	write_cr0(cr0);

	/* Flush out any pending x87 state: */
#ifdef CONFIG_MATH_EMULATION
	if (!cpu_has_fpu)
		fpstate_init_soft(&current->thread.fpu.state.soft);
	else
#endif
		asm volatile ("fninit");
}

/*
 * Enable all supported FPU features. Called when a CPU is brought online:
 */
void fpu__init_cpu(void)
{
	fpu__init_cpu_generic();
	fpu__init_cpu_xstate();
	fpu__init_cpu_ctx_switch();
}

/*
 * The earliest FPU detection code.
 *
 * Set the X86_FEATURE_FPU CPU-capability bit based on
 * trying to execute an actual sequence of FPU instructions:
 */
static void fpu__init_system_early_generic(void)
{
	unsigned long cr0;
	u16 fsw, fcw;

	fsw = fcw = 0xffff;

	cr0 = read_cr0();
	cr0 &= ~(X86_CR0_TS | X86_CR0_EM);
	write_cr0(cr0);

	if (!test_bit(X86_FEATURE_FPU, (unsigned long *)cpu_caps_cleared)) {
		asm volatile("fninit ; fnstsw %0 ; fnstcw %1"
			     : "+m" (fsw), "+m" (fcw));

		if (fsw == 0 && (fcw & 0x103f) == 0x003f)
			set_cpu_cap(c, X86_FEATURE_FPU);
		else
			clear_cpu_cap(c, X86_FEATURE_FPU);
	}

#ifndef CONFIG_MATH_EMULATION
	if (!cpu_has_fpu) {
		pr_emerg("x86/fpu: Giving up, no FPU found and no math emulation present\n");
		for (;;)
			asm volatile("hlt");
	}
#endif
}

/*
 * Boot time FPU feature detection code:
 */
unsigned int mxcsr_feature_mask __read_mostly = 0xffffffffu;
EXPORT_SYMBOL_GPL(mxcsr_feature_mask);

static void __init fpu__init_system_mxcsr(void)
{
	unsigned int mask = 0;

	if (cpu_has_fxsr) {
		/* Static because GCC does not get 16-byte stack alignment right: */
		static struct fxregs_state fxregs __initdata;

		asm volatile("fxsave %0" : "+m" (fxregs));

		mask = fxregs.mxcsr_mask;

		/*
		 * If zero then use the default features mask,
		 * which has all features set, except the
		 * denormals-are-zero feature bit:
		 */
		if (mask == 0)
			mask = 0x0000ffbf;
	}
	mxcsr_feature_mask &= mask;
}

/*
 * Once per bootup FPU initialization sequences that will run on most x86 CPUs:
 */
static void __init fpu__init_system_generic(void)
{
	/*
	 * Set up the legacy init FPU context. (xstate init might overwrite this
	 * with a more modern format, if the CPU supports it.)
	 */
	fpstate_init(&init_fpstate);

	fpu__init_system_mxcsr();
}

/*
 * Size of the FPU context state. All tasks in the system use the
 * same context size, regardless of what portion they use.
 * This is inherent to the XSAVE architecture which puts all state
 * components into a single, continuous memory block:
 */
unsigned int xstate_size;
EXPORT_SYMBOL_GPL(xstate_size);

/* Enforce that 'MEMBER' is the last field of 'TYPE': */
#define CHECK_MEMBER_AT_END_OF(TYPE, MEMBER) \
	BUILD_BUG_ON(sizeof(TYPE) != offsetofend(TYPE, MEMBER))

/*
 * We append the 'struct fpu' to the task_struct:
 */
static void __init fpu__init_task_struct_size(void)
{
	int task_size = sizeof(struct task_struct);

	/*
	 * Subtract off the static size of the register state.
	 * It potentially has a bunch of padding.
	 */
	task_size -= sizeof(((struct task_struct *)0)->thread.fpu.state);

	/*
	 * Add back the dynamically-calculated register state
	 * size.
	 */
	task_size += xstate_size;

	/*
	 * We dynamically size 'struct fpu', so we require that
	 * it be at the end of 'thread_struct' and that
	 * 'thread_struct' be at the end of 'task_struct'.  If
	 * you hit a compile error here, check the structure to
	 * see if something got added to the end.
	 */
	CHECK_MEMBER_AT_END_OF(struct fpu, state);
	CHECK_MEMBER_AT_END_OF(struct thread_struct, fpu);
	CHECK_MEMBER_AT_END_OF(struct task_struct, thread);

	arch_task_struct_size = task_size;
}

/*
 * Set up the xstate_size based on the legacy FPU context size.
 *
 * We set this up first, and later it will be overwritten by
 * fpu__init_system_xstate() if the CPU knows about xstates.
 */
static void __init fpu__init_system_xstate_size_legacy(void)
{
	static int on_boot_cpu = 1;

	WARN_ON_FPU(!on_boot_cpu);
	on_boot_cpu = 0;

	/*
	 * Note that xstate_size might be overwriten later during
	 * fpu__init_system_xstate().
	 */

	if (!cpu_has_fpu) {
		/*
		 * Disable xsave as we do not support it if i387
		 * emulation is enabled.
		 */
		setup_clear_cpu_cap(X86_FEATURE_XSAVE);
		setup_clear_cpu_cap(X86_FEATURE_XSAVEOPT);
		xstate_size = sizeof(struct swregs_state);
	} else {
		if (cpu_has_fxsr)
			xstate_size = sizeof(struct fxregs_state);
		else
			xstate_size = sizeof(struct fregs_state);
	}
	/*
	 * Quirk: we don't yet handle the XSAVES* instructions
	 * correctly, as we don't correctly convert between
	 * standard and compacted format when interfacing
	 * with user-space - so disable it for now.
	 *
	 * The difference is small: with recent CPUs the
	 * compacted format is only marginally smaller than
	 * the standard FPU state format.
	 *
	 * ( This is easy to backport while we are fixing
	 *   XSAVES* support. )
	 */
	setup_clear_cpu_cap(X86_FEATURE_XSAVES);
}

/*
 * Find supported xfeatures based on cpu features and command-line input.
 * This must be called after fpu__init_parse_early_param() is called and
 * xfeatures_mask is enumerated.
 */
u64 __init fpu__get_supported_xfeatures_mask(void)
{
	return XCNTXT_MASK;
}

/* Legacy code to initialize eager fpu mode. */
static void __init fpu__init_system_ctx_switch(void)
{
	static bool on_boot_cpu = 1;

	WARN_ON_FPU(!on_boot_cpu);
	on_boot_cpu = 0;

	WARN_ON_FPU(current->thread.fpu.fpstate_active);
	current_thread_info()->status = 0;
}

/*
 * We parse fpu parameters early because fpu__init_system() is executed
 * before parse_early_param().
 */
static void __init fpu__init_parse_early_param(void)
{
	if (cmdline_find_option_bool(boot_command_line, "no387"))
		setup_clear_cpu_cap(X86_FEATURE_FPU);

	if (cmdline_find_option_bool(boot_command_line, "nofxsr")) {
		setup_clear_cpu_cap(X86_FEATURE_FXSR);
		setup_clear_cpu_cap(X86_FEATURE_FXSR_OPT);
		setup_clear_cpu_cap(X86_FEATURE_XMM);
	}

	if (cmdline_find_option_bool(boot_command_line, "noxsave"))
		fpu__xstate_clear_all_cpu_caps();

	if (cmdline_find_option_bool(boot_command_line, "noxsaveopt"))
		setup_clear_cpu_cap(X86_FEATURE_XSAVEOPT);

	if (cmdline_find_option_bool(boot_command_line, "noxsaves"))
		setup_clear_cpu_cap(X86_FEATURE_XSAVES);
}

/*
 * Called on the boot CPU once per system bootup, to set up the initial
 * FPU state that is later cloned into all processes:
 */
void __init fpu__init_system(void)
{
	fpu__init_parse_early_param();
	fpu__init_system_early_generic();

	/*
	 * The FPU has to be operational for some of the
	 * later FPU init activities:
	 */
	fpu__init_cpu();

	/*
	 * But don't leave CR0::TS set yet, as some of the FPU setup
	 * methods depend on being able to execute FPU instructions
	 * that will fault on a set TS, such as the FXSAVE in
	 * fpu__init_system_mxcsr().
	 */
	clts();

	fpu__init_system_generic();
	fpu__init_system_xstate_size_legacy();
	fpu__init_system_xstate();
	fpu__init_task_struct_size();

	fpu__init_system_ctx_switch();
}
