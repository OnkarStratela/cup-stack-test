# Cup Stack Test

Discrete event test rig for the CAEN UHF RFID reader / antenna combo. The
operator chucks **either one mug or a stack of two mugs** (each carrying an
RFID label) into the bin once per event. The reader inventories tags inside a
fixed scan window and the program records what was thrown vs. what was actually
detected.

The test is balanced: by default **100 single-mug throws + 100 stack-of-two
throws = 200 events**, presented in randomised order so the operator cannot bias
the antenna by predicting the next event type.

## Files

| File | Purpose |
| --- | --- |
| `cup_stack_test.c` | Cup-stack test program. |
| `compile_cup_stack_test.sh` | Builds `cup_stack_test` against the CAEN sources in `SRC/`. |
| `run_cup_stack_test.sh` | Sets USB perms, runs the build, then launches the binary. |
| `SRC/` | CAEN Light library sources/headers (do not modify). |

## Build & run

```bash
chmod +x run_cup_stack_test.sh
./run_cup_stack_test.sh                   # 100 single + 100 stack, 3000 ms scan window
./run_cup_stack_test.sh 50 50 4000        # 50 single + 50 stack, 4000 ms scan window
```

Or build and run separately:

```bash
chmod +x compile_cup_stack_test.sh
./compile_cup_stack_test.sh
./cup_stack_test [single_throws] [stack_throws] [scan_window_ms]
```

CLI args:

| Arg | Meaning | Default |
| --- | --- | --- |
| `single_throws` | Number of single-mug events. | 100 |
| `stack_throws` | Number of stack-of-2 events. | 100 |
| `scan_window_ms` | Inventory window length per event, in ms. | 3000 |

## Operator workflow

1. The program connects to the reader on `/dev/ttyACM0`, sets power, and
   prints a randomised event schedule summary.
2. For each event the program prints the throw type that's about to come
   (`SINGLE MUG` or `STACK OF 2 MUGS`) and the expected unique tag count.
3. **Clear the bin**, press `ENTER`, and immediately throw the mug(s) into
   the bin. The scan window starts the moment ENTER is pressed.
4. The reader inventories continuously for `scan_window_ms` ms. All tag
   reads (with RSSI) inside the window are aggregated.
5. After the window closes, the result is printed and **immediately appended
   to the CSV** (so a power loss never destroys completed work).
6. Retrieve the mug(s), clear the bin, and proceed to the next event.

### Stopping early

- **First Ctrl+C**: finishes the current event and stops cleanly afterwards.
- **Second Ctrl+C**: aborts immediately.

In both cases all completed events are already saved on disk.

## Output CSV

Each run writes to `cup_stack_test_<YYYYMMDD_HHMMSS>.csv` (timestamped, never
overwrites previous runs). One row per event, autosaved per event.

| Column | Description |
| --- | --- |
| `event_id` | 1-based event index. |
| `timestamp` | Wall-clock time when the event scan started (`YYYY-MM-DD HH:MM:SS`). |
| `throw_type` | `single` or `stack`. |
| `expected_count` | Expected unique tag count (1 for single, 2 for stack). |
| `detected_count` | Unique tags actually detected during the scan window. |
| `result` | `correct` (== expected), `under` (< expected), `over` (> expected). |
| `total_reads` | Total inventory hits across all tags within the window (not just unique). |
| `scan_duration_ms` | Actual scan duration (≈ requested window). |
| `avg_rssi` | Mean RSSI over **all** reads in this event. |
| `max_rssi` / `min_rssi` | Best/worst RSSI over all reads in this event. |
| `unique_tag_count` | Count of unique EPCs (same as `detected_count`). |
| `tag_ids` | Semicolon-joined list of unique EPCs detected this event. |
| `tag_read_counts` | Semicolon-joined per-tag read counts (parallel to `tag_ids`). |
| `tag_avg_rssi` | Per-tag mean RSSI. |
| `tag_min_rssi` | Per-tag worst RSSI. |
| `tag_max_rssi` | Per-tag best RSSI. |
| `first_seen_ms` | Per-tag time-to-first-read, ms since the scan window began. |
| `last_seen_ms` | Per-tag time-to-last-read, ms since the scan window began. |

The `tag_*` columns are quoted strings with `;` separating per-tag entries so
the columns line up positionally (same order as `tag_ids`).

### Example row

```
12,2026-04-28 14:32:45,stack,2,2,correct,38,3000,-455,-380,-512,2,"E20000172211010418905449;E2000017221101041890544A","21;17","-450;-460","-480;-512","-380;-405","42;65","2980;2970"
```

## Tunables

Edit the `#define`s near the top of `cup_stack_test.c` if needed:

| Macro | Default | Notes |
| --- | --- | --- |
| `DEFAULT_SINGLE_THROWS` | 100 | Single-mug events. |
| `DEFAULT_STACK_THROWS` | 100 | Stack-of-2 events. |
| `DEFAULT_SCAN_WINDOW_MS` | 3000 | Per-event scan window length. |
| `DEFAULT_POWER` | 316 | CAEN power (matches existing scripts). |
| `INVENTORY_GAP_US` | 10000 | Sleep between inventories during scan window. |
| `MAX_TAGS_PER_EVENT` | 64 | Safety cap; large enough for typical stacks. |
| `READER_PORT` | `/dev/ttyACM0` | Serial device. |
| `READER_BAUD` | 921600 | Same baud as `rfid_reader.c`. |

## Troubleshooting

If the reader doesn't connect:
- Check the USB connection.
- `sudo chmod 666 /dev/ttyACM0` (handled automatically by `run_cup_stack_test.sh`).
- Or add user to dialout group: `sudo usermod -a -G dialout $USER` then logout/login.
