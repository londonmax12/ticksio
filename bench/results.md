_5,000,000 synthetic trade ticks, best-of-N timing. Lower size is better; higher throughput is better._

| format | bytes/tick | size | write (M/s) | insert 1k | insert 10k | insert 100k | read scan | read mat |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| ticks zstd | 1.36 | 6.8 MB | 32.2 | 23.2 | 31.3 | 30.0 | 95.1 | 62.0 |
| ticks none | 4.00 | 20.0 MB | 66.1 | 45.5 | 65.1 | 68.6 | 119.5 | 72.1 |
| parquet zstd | 2.62 | 13.1 MB | 15.4 | 4.4 | 8.3 | 10.3 | 75.2 | 59.6 |
| parquet snappy | 5.11 | 25.5 MB | 14.7 | 5.5 | 10.6 | 11.8 | 99.8 | 71.1 |
| feather zstd | 2.07 | 10.4 MB | 62.0 | 10.6 | 43.7 | 56.7 | 73.2 | 45.5 |
| bi5 | 1.60 | 8.0 MB | 0.2 | 0.2 | 0.2 | 0.2 | 7.5 | 6.5 |
| csv | 24.08 | 120.4 MB | 8.8 | 6.4 | 8.0 | 9.0 | 8.4 | 42.8 |
