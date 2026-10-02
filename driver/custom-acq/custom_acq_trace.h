/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Tracepoints for custom-acq (Plan.md V2/M8: cross-layer diagnostics).
 *
 * The question these answer is "in which layer did it go wrong": hard IRQ,
 * the threaded drain (SPI), the kfifo, or the userspace reader. Each layer
 * boundary gets one event, all on the same ftrace clock, and device-service
 * writes its own state transitions into the same buffer via trace_marker.
 * One `cat trace` then reads as a single timeline from the GPIO edge to the
 * supervisor's decision.
 *
 *   echo 1 > /sys/kernel/tracing/events/custom_acq/enable
 *
 * Disabled tracepoints cost a patched-out branch, so these stay compiled in.
 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM custom_acq

#if !defined(_CUSTOM_ACQ_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _CUSTOM_ACQ_TRACE_H

#include <linux/tracepoint.h>
#include <linux/version.h>

/* __assign_str() lost its second argument in 6.10; this driver still
 * builds for the 6.6 Raspberry Pi OS card as well as the 6.12 image.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
#define CUSTOM_ACQ_ASSIGN_STR(field, src) __assign_str(field)
#else
#define CUSTOM_ACQ_ASSIGN_STR(field, src) __assign_str(field, src)
#endif

/* Hard-IRQ top half: the earliest point the kernel knows about a sample. */
TRACE_EVENT(custom_acq_irq,
	TP_PROTO(s64 irq_ts_ns),
	TP_ARGS(irq_ts_ns),
	TP_STRUCT__entry(__field(s64, irq_ts_ns)),
	TP_fast_assign(__entry->irq_ts_ns = irq_ts_ns;),
	TP_printk("irq_ts_ns=%lld", __entry->irq_ts_ns)
);

/* One threaded-handler pass, emitted when it returns. drain_ns is how long
 * the pass held the SPI bus; since_irq_ns is from the hard IRQ to the end
 * of the pass. A pass that never ends shows up as no event at all - the
 * collapsed regime of case-07 - which is itself the finding.
 */
TRACE_EVENT(custom_acq_drain,
	TP_PROTO(unsigned int drained, s64 drain_ns, s64 since_irq_ns, int err),
	TP_ARGS(drained, drain_ns, since_irq_ns, err),
	TP_STRUCT__entry(
		__field(unsigned int, drained)
		__field(s64, drain_ns)
		__field(s64, since_irq_ns)
		__field(int, err)
	),
	TP_fast_assign(
		__entry->drained = drained;
		__entry->drain_ns = drain_ns;
		__entry->since_irq_ns = since_irq_ns;
		__entry->err = err;
	),
	TP_printk("drained=%u drain_ns=%lld since_irq_ns=%lld err=%d", __entry->drained, __entry->drain_ns,
		  __entry->since_irq_ns, __entry->err)
);

/* A sample leaving the driver's hands: into the kfifo, or dropped and why.
 * age_ns = now - irq_ts_ns, the driver's share of the sample's latency.
 */
TRACE_EVENT(custom_acq_sample,
	TP_PROTO(u32 seq, s64 age_ns, unsigned int kfifo_len, const char *outcome),
	TP_ARGS(seq, age_ns, kfifo_len, outcome),
	TP_STRUCT__entry(
		__field(u32, seq)
		__field(s64, age_ns)
		__field(unsigned int, kfifo_len)
		__string(outcome, outcome)
	),
	TP_fast_assign(
		__entry->seq = seq;
		__entry->age_ns = age_ns;
		__entry->kfifo_len = kfifo_len;
		CUSTOM_ACQ_ASSIGN_STR(outcome, outcome);
	),
	TP_printk("seq=%u age_ns=%lld kfifo_len=%u outcome=%s", __entry->seq, __entry->age_ns,
		  __entry->kfifo_len, __get_str(outcome))
);

/* Userspace took samples out: the kernel/userspace boundary. */
TRACE_EVENT(custom_acq_read,
	TP_PROTO(unsigned int samples, unsigned int kfifo_left),
	TP_ARGS(samples, kfifo_left),
	TP_STRUCT__entry(
		__field(unsigned int, samples)
		__field(unsigned int, kfifo_left)
	),
	TP_fast_assign(
		__entry->samples = samples;
		__entry->kfifo_left = kfifo_left;
	),
	TP_printk("samples=%u kfifo_left=%u", __entry->samples, __entry->kfifo_left)
);

/* A register operation that failed: transfer error or echo mismatch. */
TRACE_EVENT(custom_acq_spi_error,
	TP_PROTO(u8 cmd, u8 echo, int err),
	TP_ARGS(cmd, echo, err),
	TP_STRUCT__entry(
		__field(u8, cmd)
		__field(u8, echo)
		__field(int, err)
	),
	TP_fast_assign(
		__entry->cmd = cmd;
		__entry->echo = echo;
		__entry->err = err;
	),
	TP_printk("cmd=0x%02x echo=0x%02x err=%d", __entry->cmd, __entry->echo, __entry->err)
);

/* A fault injected on purpose (fault_* module parameters), so a trace
 * always shows the cause next to its effects.
 */
TRACE_EVENT(custom_acq_fault,
	TP_PROTO(const char *kind, u32 arg),
	TP_ARGS(kind, arg),
	TP_STRUCT__entry(
		__string(kind, kind)
		__field(u32, arg)
	),
	TP_fast_assign(
		CUSTOM_ACQ_ASSIGN_STR(kind, kind);
		__entry->arg = arg;
	),
	TP_printk("kind=%s arg=%u", __get_str(kind), __entry->arg)
);

#endif /* _CUSTOM_ACQ_TRACE_H */

/* Out-of-tree module: the header sits next to the .c, not in
 * include/trace/events/.
 */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE custom_acq_trace
#include <trace/define_trace.h>
