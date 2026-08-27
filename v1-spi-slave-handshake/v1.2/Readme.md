# V1.2 — SPI Register Protocol

## What This Version Does

V1.2 turns the MCU from a device that can only report its name into one that understands a full command interface. The Raspberry Pi can now read different registers, write configuration values, and receive proper error responses when something is invalid.

## Protocol Design

### Frame Format

All communication uses a fixed 5-byte frame, MSB first, big-endian:

```
Master → Slave:  [CMD] [D3] [D2] [D1] [D0]
Slave  → Master: [ECHO][R3] [R2] [R1] [R0]
```

- **CMD byte**: bit 7 = read (0) or write (1), bits 6:0 = register address
- **ECHO byte**: the CMD from the *previous* frame, echoed back for pipeline integrity checking
- **D3–D0 / R3–R0**: 32-bit data payload

### Why Fixed-Length Frames

The SPI slave is a hardware shift register — it pushes bits out on each clock edge with no ability to pause mid-transfer and inspect how long the frame should be. A fixed 5-byte frame means both sides always agree on where a frame ends, with no runtime length parsing needed.

### Why Pipelined (Two-Frame Read)

The SPI slave must load its TX buffer *before* the transaction begins. It cannot see the incoming command and then decide what to send back within the same frame. So a read takes two frames: frame N carries the read command, frame N+1 carries the response. The ECHO byte in frame N+1 lets the master verify that the data it's receiving actually corresponds to the command it sent in frame N.

This also means NOP frames don't have to be wasted — the master can pipeline a new command into the frame that retrieves the previous result.

### Register Map

| Address | Name          | R/W | Reset Value  | Description                          |
|---------|---------------|-----|--------------|--------------------------------------|
| 0x00    | `DEVICE_ID`   | RO  | 0xAC00ACC0   | Fixed hardware identifier            |
| 0x01    | `FW_VERSION`  | RO  | 0x00010200   | major.minor.patch.build (1 byte each)|
| 0x02    | `STATUS`      | RO  | 0x00000000   | Latched status/error flags           |
| 0x03    | `CONTROL`     | RW  | 0x00000000   | Device control (start/stop, clear)   |
| 0x04    | `SAMPLE_RATE` | RW  | 1000         | Acquisition rate in Hz (1–10000)     |
| 0x7F    | `NOP`         | —   | —            | No operation; used to retrieve data  |

### Error Handling

| Condition                        | Device Behavior                              |
|----------------------------------|----------------------------------------------|
| Read undefined address           | Returns 0x00000000, sets STATUS.CMD_ERR      |
| Write to read-only register      | Ignored, sets STATUS.CMD_ERR                 |
| Write out-of-range SAMPLE_RATE   | Rejected (value unchanged), sets RANGE_ERR   |

Error flags are latched — they stay set until explicitly cleared by writing CONTROL.CLEAR_FLAGS. This avoids the problem of read-to-clear, where observing the status would destroy it.

## Verification Results

**Functional tests** (7 cases): all passed — read ID, read firmware version, write/readback sample rate, reject illegal value, clear error flags, reject bad address, start/stop control.

**Stress test**: 10,000 frames (random mix of reads and writes), 0 echo mismatches, 791 frames/sec sustained.

## What Changed from V1.1

| Aspect          | V1.1                  | V1.2                                    |
|-----------------|-----------------------|-----------------------------------------|
| Capability      | Report device ID only | Read/write multiple registers           |
| Protocol        | No defined format     | 5-byte fixed frame, pipelined           |
| Error handling  | None                  | Invalid address, range, read-only guard |
| Validation      | Manual spot checks    | 7 functional tests + 10K stress test    |