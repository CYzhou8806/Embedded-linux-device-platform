#pragma once

// The payload this project publishes on the "acq/samples" service: one
// acquisition sample, byte-identical to driver/custom-acq's
// struct custom_acq_sample as read out of /dev/acq0.
//
// It lives in one header because devbus's type check can catch a
// publisher and a subscriber disagreeing about the type's *name*, size or
// alignment, but not about the order of two same-sized fields - and this
// struct previously existed as three hand-kept copies (acq_bridge,
// sample_publisher, sample_subscriber), with device-service about to make
// a fourth.
//
// Deliberately at global scope, not in namespace devbus: the type name is
// part of devbus's type hash, so moving it into a namespace would change
// the hash and stop already-built publishers and subscribers from talking
// to each other. It is also an application payload, not part of the
// middleware - devbus itself never looks inside it.

#include <cstdint>
#include <type_traits>

struct AcqSample {
	uint32_t seq;       // MCU sequence number, wraps at 2^32
	uint32_t value;     // the sample itself
	int64_t irq_ts_ns;  // driver hard-IRQ timestamp, CLOCK_MONOTONIC
};

static_assert(sizeof(AcqSample) == 16, "must match struct custom_acq_sample in the driver");
static_assert(alignof(AcqSample) == 8);
static_assert(std::is_trivially_copyable_v<AcqSample> && std::is_standard_layout_v<AcqSample>);
