# Case 01: NVIC Priority Group Caused SPI Communication to Collapse

**Platform:** STM32F103VET6 (SPI slave) + Raspberry Pi 4 (SPI master)
**Occurred:** V1.3, first time SPI interrupt mode and the sample timer ran together

## Symptom

- V1.2 regression suite passed in full (SPI worked correctly with the timer not running).
- After writing `CONTROL.START = 1` to start acquisition, every SPI echo byte turned into `0x00` or `0x7F`.
- The MCU emitted a continuous buzzing sound (DATA_READY GPIO toggling at high frequency).
- The device stopped responding to any command entirely.

## Expected

SPI2 (byte-at-a-time interrupt-driven transfer, 5 bytes/frame) and TIM2 (periodic sample generation into the FIFO) should run concurrently without either interrupt corrupting the other's state. Enabling the timer should not affect SPI framing at all.

## Hypotheses

- The timer ISR itself was doing something wrong to the FIFO or shared state.
- A race in the SPI HAL driver unrelated to the timer.
- Clock/timing misconfiguration on TIM2 causing electrical noise on the DATA_READY line.

## Investigation

1. Emptied the timer callback (made it a no-op) and re-ran the SPI regression suite. SPI worked perfectly, so the problem was tied to the *presence* of the timer ISR itself, not the FIFO/data-generation logic inside it.
2. Checked the CubeMX NVIC configuration: both SPI2 and TIM2 global interrupts were at the CubeMX-default priority (0), i.e. equal priority.
3. Checked the NVIC Priority Group setting: CubeMX's default is "0 bits for pre-emption, 4 bits for sub-priority", meaning **no interrupt can pre-empt another**, regardless of their priority values.

## Root Cause

With 0 pre-emption bits, SPI2 and TIM2 interrupts could never interrupt each other. Whichever ISR was running had to finish before the other could start. The TIM2 callback's execution time was long enough that pending SPI byte-interrupts were delayed past the SPI peripheral's next-byte deadline. `HAL_SPI_TransmitReceive_IT` tracks frame progress with an internal byte counter, and a delayed interrupt caused it to lose sync with the actual bytes shifted in/out over the wire, corrupting frame boundaries for the rest of the session.

## Fix

Changed the priority grouping so SPI can pre-empt the timer:

- CubeMX → System Core → NVIC → **Priority Group** = "4 bits for pre-emption, 0 bits for sub-priority"
- **SPI2 global interrupt**: Preemption Priority = 1 (high)
- **TIM2 global interrupt**: Preemption Priority = 3 (low)

This lets an in-progress TIM2 callback be interrupted by an incoming SPI byte, so the HAL byte counter never falls behind the wire.

## Verification

10,000-frame protocol stress test: 0 echo mismatches, sustained at 792 frames/second with the timer running concurrently.
