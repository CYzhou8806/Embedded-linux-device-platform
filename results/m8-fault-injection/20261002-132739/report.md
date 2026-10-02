# M8 fault-injection report

| scenario | injected | expected layer | first anomaly | verdict | spread (ms after inject) | supervisor |
|---|---|---|---|---|---|---|
| baseline | - | none | none | correct | none | no transition |
| drain_delay | drain_delay | drain | drain | correct | drain -0.1, reader -0.1, driver +165.3 | no transition |
| drop | drop | driver | driver | correct | driver +31.7 | no transition |
| spi_error | spi_error | spi | spi | correct | spi +57.8 | no transition |
| stall | stall | drain | drain | correct | drain +1.0, reader +1.0 | Running -> Recovering on Stall (no sample for 4264 ms); Recovering -> Running on Recovered (samples flowing after attempt 1) |
| slow_consumer | - | reader | reader | correct | reader +4.2, driver +132.9 | no transition |
| overload | - | drain | drain | correct | drain -0.5, reader -0.5, driver +110.7 | no transition |

7/7 scenarios attributed to the expected layer.
