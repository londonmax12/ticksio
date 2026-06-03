_5,000,000 synthetic trade ticks. Lower size is better; higher throughput is better._

| format | bytes/tick | size | write (M/s) | insert (M/s) | read (M/s) |
|---|--:|--:|--:|--:|--:|
| ticks zstd | 1.36 | 6.8 MB | 14.8 | 11.0 | 36.3 |
| ticks none | 4.00 | 20.0 MB | 17.7 | 13.4 | 74.9 |
| parquet zstd | 2.62 | 13.1 MB | 9.1 | 6.8 | 63.1 |
| parquet snappy | 5.11 | 25.5 MB | 10.5 | 7.2 | 50.1 |
| feather zstd | 2.07 | 10.4 MB | 43.2 | 42.5 | 30.2 |
| csv | 24.08 | 120.4 MB | 3.8 | 5.4 | 25.6 |
