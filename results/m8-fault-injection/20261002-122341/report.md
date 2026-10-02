# M8 fault-injection report

| scenario | injected | expected layer | first anomaly | verdict | spread (ms after inject) | supervisor |
|---|---|---|---|---|---|---|
| baseline | | | | error: no 'm8: inject' marker in trace | | |
| drain_delay | | | | error: no 'm8: inject' marker in trace | | |
| drop | | | | error: no 'm8: inject' marker in trace | | |
| spi_error | spi_error | spi | mcu_irq | **wrong** | mcu_irq +58.7, spi +59.3 | Running -> Recovering on Stall (no sample for 3868 ms); Recovering -> Running on Recovered (samples flowing after attempt 1) |
| stall | | | | error: no 'm8: inject' marker in trace | | |
| slow_consumer | | | | error: no 'm8: inject' marker in trace | | |
| overload | | | | error: no 'm8: inject' marker in trace | | |

0/1 scenarios attributed to the expected layer.
