# Notes

Personal working notes, kept in Chinese, written for the author's own
future reference while learning/building this project — not part of
the polished deliverable. Nothing here is linked from the top-level
`README.md` or `docs/performance.md`/`docs/debugging/`. Content is
genuine and technically accurate, just informal in tone and language.

| File | What it covers |
| --- | --- |
| `learning-qa.md` | Confirmed Q&A log, mostly from the V3 (Device Tree + kernel driver) stage. |
| `security-knowledge-map.zh.md` | Map of the embedded-security territory (boot chain, integrity/encryption, TEE, update, key infrastructure) doubling as a review checklist, with per-topic status markers. Paired with the work plan in `private/security-plan.md`. |
| `systems-programming-patterns.md` | Generic, reusable systems-programming patterns learned along the way (not tied to one file/case), including real-time Linux tuning below the process level. |
| `driver-code-walkthrough.zh.md` | Line-by-line walkthrough of `driver/custom-acq/custom_acq.c`. |
| `device-service-code-walkthrough.zh.md` | Line-by-line walkthrough of `userspace/device-service/`. |
| `devbus-code-walkthrough.zh.md` | Walkthrough of `userspace/devbus/` — shared-memory layout, the lock-free rings, memory ordering, crash reclaim. The hardest code in the repo. |
| `yocto-layer-code-walkthrough.zh.md` | Walkthrough of `yocto/meta-device-platform/` (layers, recipes, BitBake mechanics). |
| `v1.3-learning-note.zh.md` | Raw debugging notes from building MCU firmware v1.3. |
| `v1.3-code-walkthrough.zh.md` | Line-by-line walkthrough of the v1.3 MCU firmware + Pi-side test script. |
