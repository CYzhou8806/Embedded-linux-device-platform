# Language Audit — Chinese vs English

Scan date: 2026-08-28. Purpose: reference for V8 polish (GitHub cleanup), not
an action item now. Lists every file containing Chinese characters, and a
recommendation on whether it's worth converting to English.

## Keep Chinese (intentional, personal-facing — don't touch)

| File | Why |
|---|---|
| `Plan.md` | Personal strategy doc, not meant for external readers, explicitly the "终版" per its own header. |
| `CLAUDE.md` | Loaded only in this dev session, not part of the shipped project story. |
| `docs/session-log.md` | Local rolling status log, excluded from git (`.git/info/exclude`), never pushed. |
| `v1-spi-slave-handshake/v1.2/learningNote.md` | Personal raw debugging notes — CLAUDE.md's doc map explicitly says this stays Chinese/informal; the polished English version is `docs/debugging/case-*.md`. |
| `v1-spi-slave-handshake/v1.3/LearningNote.md` | Same as above. |
| `.gitignore` | Comments only, never rendered to an external reader. |

## Worth converting later (source code an interviewer/reviewer would actually open)

| File | Content | Recommendation |
|---|---|---|
| `v1-spi-slave-handshake/v1.2/MCU/Core/Src/main.c` | 3 Chinese comments | Convert — small, part of shipped firmware source. |
| `v1-spi-slave-handshake/v1.3/MCU_v1-MCU-device-control/Core/Src/main.c` | ~40 Chinese comments (design rationale: FIFO, SPI recovery, register logic) | Convert — this is the main firmware file a reviewer opens; comments carry real design reasoning worth showing in English. |
| `v1-spi-slave-handshake/v1.3/MCU_v1-MCU-device-control/Core/Src/spi.c` | 2 Chinese comments (NSS/SCK pull config rationale) | Convert — same reasoning as above. |
| `v1-spi-slave-handshake/v1.2/RaspPi/test_v12.py` | Chinese docstrings, print strings, comments | Convert — test script is part of the V2 deliverable. |
| `v1-spi-slave-handshake/v1.2/RaspPi/stress_v12.py` | Same | Convert. |
| `v1-spi-slave-handshake/v1.3/RaspPi/testv13.py` | Same, largest file (413 lines) | Convert — this is the stress-test script referenced in the V1.3 Readme's verification results, likely to be opened. |

Total scope if converting: ~1700 lines across 6 files, mostly comment/string
lines, not full-file rewrites.

## Ambiguous — decide based on audience

| File | Content | Note |
|---|---|---|
| `v1-spi-slave-handshake/v1.3/CODE_WALKTHROUGH.md` | Fully Chinese, written as a self-review doc ("目的是让你自己检查里面有没有功能/设计问题") | This reads as a personal working doc, not a polished deliverable like `docs/debugging/case-*.md`. Either keep Chinese and treat like LearningNote, or fold its useful content into a future `docs/*.md` in English and drop the original. Not worth translating as-is. |
| `tools/README.md`, `tools/*.sh` | Heavy Chinese comments (16–37 lines each) | `tools/README.md` is listed in CLAUDE.md's doc map as project documentation, but it's dev-environment setup for yourself (Hyper-V/USB-IP/OpenOCD), not something a hiring reviewer is likely to read closely. Low priority — convert only if you want the whole repo to read as English-first. |

## Suggested order when this gets tackled (V8 stage)

1. `v1-spi-slave-handshake/v1.3/MCU_v1-MCU-device-control/Core/Src/{main.c,spi.c}` — highest visibility, current firmware version.
2. `v1-spi-slave-handshake/v1.3/RaspPi/testv13.py` — current test script.
3. `v1-spi-slave-handshake/v1.2/...` — older version, lower priority, could even be left as historical record.
4. `tools/` and `CODE_WALKTHROUGH.md` — optional, do last if at all.

Not in scope, ever: `Plan.md`, `CLAUDE.md`, `docs/session-log.md`,
`LearningNote.md` / `learningNote.md` — these are working documents by design.
