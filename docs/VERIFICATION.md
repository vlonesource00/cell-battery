# Verification

Build **1.0.3**, measured and tested on 15 October 2026; published on 16 October 2026.

| Check | Result |
| --- | --- |
| Executable | Native Windows x64, 317,952 bytes (310.5 KiB) |
| Runtime | Static native libraries and Windows system DLLs; no runtime installation |
| Compact / expanded at 100% scaling | Approximately 218 × 218 / 218 × 435 pixels |
| Actual desktop attachment check | 326 × 326 pixels at 144 DPI (150% display scaling) |
| Native functional checks | **56 passed, 0 failed** |
| Actual desktop checks | Attach hidden test widget, retain command controller, detach successfully |
| Hourly cache | First export succeeded; normal-user scheduled request refreshed the cache without UAC |
| Network functionality | None in the widget or native exporter |

## Resource sample

The final widget was measured with a real Windows app-energy cache loaded. This is a short process sample, not a fixed resource limit or a controlled cross-application comparison. It ran on an 8-logical-processor laptop with Windows build 26300.

| Metric | Observed result |
| --- | --- |
| Duration | 60.419 seconds |
| Process CPU time | 0.046875 seconds |
| Average CPU across the machine | 0.0097% |
| Private committed memory at final sample | 6.379 MiB |
| Working set at final sample | 20.324 MiB |
| Threads / handles at final sample | 2 / 189 |

Private memory is the process's own committed allocation. The working set includes resident code and shared system-library pages. The hourly exporter and powercfg temporarily add CPU and memory use; those refresh bursts are not included in this steady widget sample. Expanded views, new fonts, different cache sizes, and interaction can change these numbers.

Sanitized measurements and plotting inputs are in `../verification/performance.json`. CPU in the graph is calculated per sample interval from process CPU-time differences, divided by elapsed wall time and 8 logical processors. The reported mean uses the full interval. No process IDs, usernames, or private app-history records are included.

## Functional coverage

The native suite covers timestamps, CSV encodings, app-energy aggregation, source/hour filters, bounded caches, malformed data, atomic writes, battery-history gaps, capacity-trend estimates and ETA priority, and actual timer delivery. Layout checks verify the reduced square, downward expansion, control bounds, and pixel-identical partial redraws. Pin tests check position preservation, parenting, unpinning, the hidden controller, and old-preference migration. Cache tests check automatic pickup, on-demand completion, rejecting older caches, and retaining data after a malformed cache.

An additional hidden-window test attached to this machine's actual Explorer desktop and detached successfully. The hourly setup registered a fixed protected helper, completed its first real export, and was then invoked from a normal Windows user token without another UAC prompt. The widget's local cache was checked against the scheduled cache and had matching record count and update time.

Full battery cycles, long-duration resource stability, repeated real sleep/resume or Explorer-restart recovery, and other Windows builds were not exercised. Desktop attachment relies on Explorer's desktop layout; failure leaves a floating widget available. This machine supplied no Windows ETA during battery testing, so its labelled recent-drain fallback was exercised.

## Estimate accuracy

Time left and wattage are estimates. Windows ETA takes priority. Otherwise the widget fits 1–2 minutes of remaining-energy observations after sufficient warmup and capacity changes. The firmware's separately reported rate can disagree with this trend. Software checks validate the calculation and update behavior; they do not establish laboratory power-measurement accuracy or guarantee remaining runtime. App shares are shares of Windows' estimated app energy, not battery percentage points lost.

## Integrity

Executable SHA-256:

```text
ca1240671dbfcc721719a1293d922acbfbdf48bd3d99da9d6ab8ce3ccafd0b3f
```

Run `--self-test "output-folder"` for native checks or `--desktop-check "output-folder"` for the hidden desktop attachment check. The source is included. The portable ZIP and public repository exclude the private data directory and live interface renders.
