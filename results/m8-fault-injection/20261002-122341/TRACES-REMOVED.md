First board run (image 1.3.1). Six of the seven traces were removed before
committing (~160 MB): with sched_switch and irq_handler_* enabled the 8 MB
ring overwrote half of each, including the `m8: inject` marker the
analysis is anchored on, so they could not be analyzed (report.md lists
them as errors). The `spi_error` trace is kept: it is the evidence for
docs/debugging/case-15 (one injected SPI error stopping acquisition for
3.9 s). status-*.json files are kept for every scenario.
