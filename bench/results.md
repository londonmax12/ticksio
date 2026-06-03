_10,000,000 synthetic trade ticks, best-of-N timing. Lower size is better; higher throughput is better._

| format | bytes/tick | size | write (M/s) | insert 1k | insert 10k | insert 100k | read scan | read mat |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| ticks zstd | 1.36 | 13.6 MB | 28.4 | 18.6 | 26.0 | 24.5 | 78.4 | 86.3 |
| ticks none | 4.00 | 40.0 MB | 48.0 | 36.4 | 51.9 | 54.7 | 113.3 | 114.2 |
| parquet zstd | 2.65 | 26.5 MB | 10.1 | 2.7 | 5.4 | 7.1 | 59.6 | 51.1 |
| parquet snappy | 5.13 | 51.3 MB | 14.2 | 4.1 | 8.9 | 9.9 | 68.1 | 49.6 |
| feather zstd | 2.07 | 20.7 MB | 36.4 | 6.3 | 21.4 | 38.5 | 53.0 | 39.8 |
| bi5 | 1.58 | 15.8 MB | 0.3 | 0.2 | 0.1 | 0.1 | 5.9 | 3.9 |
| csv | 24.08 | 240.8 MB | 1.2 | 3.5 | 4.6 | 4.3 | 4.0 | 16.6 |
