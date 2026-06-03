_5,000,000 synthetic trade ticks, best-of-N timing. Lower size is better; higher throughput is better._

| format | bytes/tick | size | write (M/s) | insert 1k | insert 10k | insert 100k | read scan | read mat |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| ticks zstd | 1.36 | 6.8 MB | 32.0 | 23.0 | 30.0 | 28.5 | 83.5 | 57.3 |
| ticks none | 4.00 | 20.0 MB | 59.2 | 42.3 | 58.6 | 62.6 | 111.0 | 66.0 |
| parquet zstd | 2.62 | 13.1 MB | 15.1 | 4.3 | 8.6 | 10.1 | 79.0 | 70.8 |
| parquet snappy | 5.11 | 25.5 MB | 15.8 | 5.5 | 10.8 | 11.7 | 96.9 | 80.9 |
| feather zstd | 2.07 | 10.4 MB | 56.4 | 10.7 | 44.8 | 61.6 | 81.8 | 56.5 |
| bi5 | 1.60 | 8.0 MB | 0.2 | 0.2 | 0.2 | 0.2 | 7.2 | 6.6 |
| csv | 24.08 | 120.4 MB | 7.8 | 6.0 | 7.7 | 8.0 | 7.9 | 30.7 |
