#ifndef ESCAPE_H
#define ESCAPE_H

/* escape - return to Swiss/loader stub without a power cycle.
 *
 * Swiss plants a stub at 0x80001800. On real hardware the "STUBHAXX" magic
 * sits at 0x80001808 (two instructions after _start). Stock libogc's
 * __stub_found() / __reload() only recognize magic starting at 0x80001804,
 * so exit()/__reload() often hot-reset to the IPL instead of Swiss.
 *
 * We detect either layout, then jump to 0x80001800 ourselves.
 */

#include <gccore.h>
#include <stdlib.h>
#include <string.h>

#define ESC_STUB_ADDR 0x80001800u

extern void __reload(void);

static inline int escape_available(void)
{
	const char *p = (const char *)(ESC_STUB_ADDR + 4);

	/* Magic at +4 (some stubs) or +8 (Swiss crt0: mfmsr; b; "STUBHAXX") */
	return memcmp(p, "STUBHAXX", 8) == 0 || memcmp(p + 4, "STUBHAXX", 8) == 0;
}

/* Write back every dirty line and drop stale instructions, because nothing else
 * on this path does. Compare libogc's own two arms (system.c):
 *
 *     if(__stub_found()) { __exception_closeall(); reload(); }   <- no cache
 *     __SYS_DoHotReset(0);  ->  _viReg[1] = 0; ICFlashInvalidate();
 *
 * The reset arm flushes; the stub arm does not. A real reset clears the caches
 * as a side effect, so this whole class of bug stays invisible until you leave
 * by jumping. Two ways it bites: our dirty lines can be written back AFTER the
 * stub has placed the next image at those addresses, overwriting fresh code with
 * our garbage; and the icache can still hold our instructions for them, so a
 * correctly loaded image executes the wrong bytes anyway. Both are load
 * dependent, which fits a fault that only shows on a second boot and wanders.
 *
 * Flushing writes the stub's own 256 bytes back to RAM too, so 0x80001800
 * survives this rather than being endangered by it. */
#define ESC_MEM1_BASE ((void *)0x80000000u)
#define ESC_MEM1_SIZE (24u * 1024u * 1024u)

static inline void escape_cache_park(void)
{
	DCFlushRange(ESC_MEM1_BASE, ESC_MEM1_SIZE);
	ICFlashInvalidate();
	__asm__ __volatile__("isync" ::: "memory");
}

/* Disable EE, then jump to the loader stub. Does not return. */
static inline void escape_jump_stub(void)
{
	void (*stub)(void) = (void (*)(void))ESC_STUB_ADDR;
	u32 msr;

	escape_cache_park();
	__asm__ __volatile__(
		"mfmsr %0\n\t"
		"rlwinm %0,%0,0,17,15\n\t"
		"mtmsr %0\n\t"
		"isync"
		: "=r"(msr)
		:
		: "memory");
	stub();
	for (;;)
		;
}

/* Blank VI so the last GX frame (or EFB/XFB trash) is not still on screen
 * while Swiss redraws. Safe from IRQ — no VSync wait. */
static inline void escape_video_black(void)
{
	GX_AbortFrame();
	GX_Flush();
	VIDEO_SetBlack(TRUE);
	VIDEO_Flush();
}

/* Clear any SI/EXI transfer still marked in flight.
 *
 * libogc waits for these with no timeout, in SYS_Init and therefore before
 * main: __si_init spins on SICOMCSR bit 0 (si.c) and __exi_init spins on the
 * CR start bit of all three EXI channels (exi.c). A start bit is normally
 * cleared by the transfer's completion interrupt, and escape_jump_stub() drops
 * MSR[EE] before that can happen, so leaving one set hangs the NEXT DOL before
 * it can print anything. An untouched framebuffer is zero bytes, which is green
 * in YUYV, so the symptom is a green screen on the second boot and nothing else.
 *
 * Note SIPOLL (_siReg[12]) and SICOMCSR (_siReg[13]) are different registers:
 * stopping auto-poll does not retire a command transfer.
 *
 * Raw MMIO because this also runs from __SYS_PreInit, before libogc exists.
 * Every wait is bounded - a drain that can hang is no better than the hang. */
#define ESC_SI_BASE     0xCC006400u
#define ESC_EXI_BASE    0xCC006800u
#define ESC_DRAIN_SPINS 200000

static inline void escape_drain_bus(void)
{
	volatile u32 *si = (volatile u32 *)ESC_SI_BASE;
	volatile u32 (*exi)[5] = (volatile u32 (*)[5])ESC_EXI_BASE;
	int chan, spin;

	si[12] = 0;
	for (spin = 0; spin < ESC_DRAIN_SPINS && (si[13] & 1u); spin++)
		;
	si[13] = 1u << 31;		/* clear TSTART, ack TCINT */

	for (chan = 0; chan < 3; chan++) {
		for (spin = 0; spin < ESC_DRAIN_SPINS &&
				(exi[chan][3] & 1u); spin++)
			;
		exi[chan][3] = 0;
	}
}

/* Heal the bus on the way IN as well, since draining on the way out only helps
 * when we live long enough to do it - not after a crash, a wedge, or RESET
 * landing between the write that starts a transfer and the interrupt that ends
 * it. libogc calls __SYS_PreInit (weak) at the top of SYS_Init with interrupts
 * already off and ahead of __exi_init and __si_init, which is exactly the slot
 * for this. Expand once at file scope; it must be a strong global, so it cannot
 * be static or inline. MMIO only here - no heap, no stdio, no IRQs yet. The
 * trailing extern just lets the call site end in a semicolon. */
#define ESCAPE_SYS_PREINIT()             \
	void __SYS_PreInit(void)         \
	{                                \
		escape_drain_bus();      \
	}                                \
	extern int esc_preinit_installed

/* Leave VI in a KNOWN state, which matters more than leaving it black.
 *
 * VIDEO_Init does not unconditionally program timing - it skips it when VI is
 * already running (video.c:2601):
 *
 *     if(!(_viReg[1]&0x0001)) { rmode = VIDEO_GetPreferredMode(NULL);
 *                               __VIInit(rmode->viTVMode); }
 *
 * and a few lines later reads the TV mode back out of that same register
 * (HorVer.tv = _SHIFTR(_viReg[1],8,2)). Handing the next DOL a live VI does not
 * just leave a stale picture up: it makes that DOL adopt OUR mode bits and skip
 * its own timing setup. VIDEO_SetBlack does not clear the enable bit, so
 * blanking is not enough. __SYS_DoHotReset writes _viReg[1] = 0 for this very
 * reason - on the arm that resets, not the arm that jumps to a stub.
 *
 * _viReg is halfwords (vu16* at 0xCC002000), so DCR is at +0x02, not +0x04. */
#define ESC_VI_BASE 0xCC002000u
#define ESC_PI_BASE 0xCC003000u

static inline void escape_vi_off(void)
{
	volatile u16 *vi = (volatile u16 *)ESC_VI_BASE;

	vi[1] = 0;
}

/* Blunt interrupt mask, one store. Cannot be a boot hang on its own: the next
 * DOL's __irq_init writes PI_INTMSK itself (irq.c) before either wait that
 * hangs, and all of SYS_Init runs under _CPU_ISR_Disable. Kept because it
 * retires the class for free. PI_INTSR is left alone deliberately - its bits are
 * write-1-to-clear, so a blind store asserts rather than clears. */
static inline void escape_pi_mask(void)
{
	volatile u32 *pi = (volatile u32 *)ESC_PI_BASE;

	pi[1] = 0;
}

/* Stop everything that could still dispatch or drive the bus. No waits and no
 * locks, so it is safe from the RESET interrupt. EXI_Detach does more than
 * unhooking a callback: it clears EXI_FLAG_ATTACH and masks that channel's
 * EXI/EXT interrupts. AUDIO_StopDMA is a single register read-modify-write, so
 * it is safe even in a build that never initialised audio. */
static inline void escape_quiesce(void)
{
	PAD_SetSamplingCallback(NULL);
	SI_DisablePolling(0xF0000000u);
	SI_EnablePollingInterrupt(FALSE);
	EXI_RegisterEXICallback(0, NULL);
	EXI_RegisterEXICallback(1, NULL);
	EXI_RegisterEXICallback(2, NULL);
	EXI_Detach(0);
	EXI_Detach(1);
	EXI_Detach(2);
	AUDIO_StopDMA();
	escape_pi_mask();
}

/* Last thing before the jump, once nothing else can touch the hardware. Drain
 * after libogc has finished with the bus, never before, or an orderly shutdown
 * can end up waiting on a transfer we already aborted. */
static inline void escape_bus_park(void)
{
	escape_drain_bus();
	escape_vi_off();
}

static inline void escape_now(void)
{
	escape_video_black();
	escape_quiesce();
	escape_bus_park();
	if (escape_available())
		escape_jump_stub();
	/* No stub: same fallback as libogc __reload */
	__reload();
}

/* Orderly quit from a healthy main loop: black + a couple of VSyncs so the
 * blank is latched before the stub runs.
 *
 * SYS_ResetSystem(SYS_SHUTDOWN) is what exit() used to run for us, and it is
 * worth keeping ahead of the jump: __dsp_shutdown, every registered reset
 * function (PAD, memory card, DVD), __exception_closeall, and the low-memory
 * clears - none of which touch 0x80001800..0x800018FF, so the stub survives it.
 * Not done in escape_now() because __call_resetfuncs is called in a `while`
 * loop and can block, and the wedge path must not be able to wedge. */
static inline void escape_exit(void)
{
	escape_video_black();
	VIDEO_WaitVSync();
	VIDEO_WaitVSync();
	escape_quiesce();
	SYS_ResetSystem(SYS_SHUTDOWN, 0, FALSE);
	escape_bus_park();
	if (escape_available())
		escape_jump_stub();
	exit(0);
}

static PADStatus esc_pad[PAD_CHANMAX];
static u16 esc_pad_prev[PAD_CHANMAX];

static inline void escape_pad_sample(void)
{
	PAD_Read(esc_pad);
}

static inline u16 escape_pad_held(int chan)
{
	if (chan < 0 || chan >= PAD_CHANMAX)
		return 0;
	return (esc_pad[chan].err == PAD_ERR_NONE) ? esc_pad[chan].button : 0;
}

static inline u16 escape_start_pressed(void)
{
	u16 hit = 0;
	int i;

	for (i = 0; i < PAD_CHANMAX; i++) {
		u16 now = escape_pad_held(i);

		if ((now & ~esc_pad_prev[i]) & PAD_BUTTON_START)
			hit = PAD_BUTTON_START;
		esc_pad_prev[i] = now;
	}
	return hit;
}

static int esc_reset_line_ok = 0;

static inline int escape_reset_held(void)
{
	return esc_reset_line_ok && SYS_ResetButtonDown() != 0;
}

static inline int escape_requested(void)
{
	return escape_start_pressed() != 0 || escape_reset_held();
}

static void escape_reset_cb(u32 irq, void *ctx)
{
	(void)irq;
	(void)ctx;
	if (esc_reset_line_ok)
		escape_now();
}

static inline void escape_install(void)
{
	esc_reset_line_ok = (SYS_ResetButtonDown() == 0);
	SYS_SetResetCallback(escape_reset_cb);
}

#endif /* ESCAPE_H */
