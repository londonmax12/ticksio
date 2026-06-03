_5,000,000 synthetic trade ticks. Lower size is better; higher throughput is better._

| format | bytes/tick | size | write (M/s) | insert (M/s) | read (M/s) |
|---|--:|--:|--:|--:|--:|
| ticks zstd | 1.36 | 6.8 MB | 38.1 | 27.0 | 64.1 |
| ticks none | 4.00 | 20.0 MB | 70.7 | 52.2 | 105.7 |
| parquet zstd | 2.62 | 13.1 MB | 16.4 | 12.0 | 101.5 |
| parquet snappy | 5.11 | 25.5 MB | 18.5 | 13.2 | 101.9 |
| feather zstd | 2.07 | 10.4 MB | 64.2 | 64.0 | 58.4 |
| bi5 | 1.60 | 8.0 MB | 0.3 | 0.3 | 8.5 |
| csv | 24.08 | 120.4 MB | 14.1 | 13.6 | 54.1 |
