# Case 02: Disabling Interrupts to Protect the FIFO Caused SPI OVR and Frame Desync

**Platform:** STM32F103VET6 (SPI slave) + Raspberry Pi 4 (SPI master)
**Occurred:** V1.3, right after Case 01's NVIC priority fix, when `fifo_push()` was added to the TIM2 callback

## Symptom

- With the TIM2 callback left empty, SPI was completely stable: 10,000-frame stress test, zero mismatches.
- As soon as `fifo_push()` (which wraps its body in `__disable_irq()` / `__enable_irq()`) was called from the callback, the MCU crashed a few seconds into acquisition.
- All echo bytes became `0x7F`, and all returned data was `0x7F7F7F7F` (the value read back from a dead/idle MISO line).
- A short buzz was heard, then the device stopped responding entirely.

## Expected

`fifo_head`/`fifo_count` bookkeeping needed protection because the TIM2 ISR (producer, increments count) and the SPI ISR (consumer, decrements count) both touch it, and `count++`/`count--` are not atomic on Cortex-M3 (read-modify-write across three instructions). Guarding the critical section with `__disable_irq()`/`__enable_irq()` was expected to make this safe without otherwise affecting SPI behavior.

## Hypotheses

- The FIFO logic itself had an off-by-one or index-wrap bug.
- `fifo_push()`'s critical section was too long and causing missed samples (performance concern, not correctness).
- Something about calling a function with these intrinsics from an ISR broke calling conventions.

## Investigation

1. Left the TIM2 callback empty. SPI was fine, which ruled out the timer ISR itself (Case 01's fix held).
2. Added `fifo_push()` (with its `__disable_irq()` section). The crash reappeared, which isolated the fault specifically to code that disables interrupts, not to FIFO indexing logic in general.
3. Reviewed how `HAL_SPI_TransmitReceive_IT` actually works: it is byte-interrupt-driven, each 5-byte frame requires 5 separate SPI interrupts (one per byte), because the SPI peripheral's data register (DR) holds only a single byte at a time.
4. Traced the timeline: while `__disable_irq()` masked all interrupts, the Raspberry Pi (master) kept clocking bytes in on its own schedule, independent of the slave's interrupt state.

## Root Cause

`__disable_irq()` masks the SPI2 interrupt along with everything else. If the Raspberry Pi transmits a new byte while interrupts are disabled, the SPI hardware shifts it into DR before the previous byte has been read out by the ISR. This is a classic SPI **overrun (OVR)**: the old byte is lost, and the HAL's internal byte-position counter, which assumed one interrupt per byte, falls one byte behind the real bit stream. Every frame boundary after that point is shifted, so every subsequent byte is misinterpreted as the wrong protocol field, permanently.

Timeline:
1. Pi transmits byte 3 of a frame → latched into DR.
2. TIM2 fires, calls `fifo_push()` → `__disable_irq()`.
3. Pi transmits byte 4 → overwrites DR (byte 3 is lost, OVR flag set).
4. `__enable_irq()` → SPI ISR fires, HAL reads DR and treats the byte-4 value as byte 3.
5. Every frame from then on is offset by one byte, corrupting the CMD/ECHO protocol until reset.

## Fix

Removed the shared `fifo_count` variable and its critical section entirely, replacing it with a lock-free single-producer/single-consumer ring buffer where the timer ISR only ever writes `fifo_head` and the SPI ISR only ever writes `fifo_tail`:

```c
static void fifo_push(uint32_t value) {
    uint16_t next = (fifo_head + 1) % FIFO_DEPTH;
    if (next == fifo_tail) {
        fifo_overflow++;
        return;
    }
    fifo_buf[fifo_head].sequence = seq_counter++;
    fifo_buf[fifo_head].value    = value;
    fifo_head = next;
    // No __disable_irq(). Timer only writes head, SPI only writes tail.
    // Aligned 16-bit reads/writes are a single atomic instruction on Cortex-M3.
}
```

Because each ISR only ever writes its own pointer and only reads the other's, there is no read-modify-write race, and interrupts never need to be masked. SPI can no longer lose a byte to OVR.

## Verification

- 100 Hz acquisition for 10 seconds: 1000 samples produced, 0 sequence gaps, 0 overflow.
- 10,000-frame protocol stress test: 0 echo mismatches.
- 1000 Hz acquisition: SPI protocol remained stable; the only limiting factor was the Raspberry Pi's `spidev` polling rate, not a firmware bug.
