#!/bin/bash
set -u

RUN_DIR="$1"
OUT_DIR="$2"
PHASES="$RUN_DIR/output/phases.csv"
PID_FILE="$RUN_DIR/output/owned-pid.txt"
mkdir -p "$OUT_DIR"
printf 'watcher_started_utc=%s\nrun_id=v7-profile-combined-01\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" > "$OUT_DIR/sample-watcher-receipt.txt"

for ((poll=0; poll<850; ++poll)); do
    if [[ -f "$PHASES" ]] && /usr/bin/grep -q '^measure_start,' "$PHASES"; then
        if /usr/bin/grep -q '^measure_end,' "$PHASES"; then
            printf 'status=measure_end_already_seen\n' >> "$OUT_DIR/sample-watcher-receipt.txt"
            exit 2
        fi
        if [[ ! -f "$PID_FILE" ]]; then
            printf 'status=missing_owned_pid\n' >> "$OUT_DIR/sample-watcher-receipt.txt"
            exit 3
        fi
        GAME_PID="$(cat "$PID_FILE")"
        MARKER="$(/usr/bin/grep '^measure_start,' "$PHASES" | tail -n 1)"
        printf 'measure_start_seen_utc=%s\ngame_pid=%s\nmeasure_start_marker=%s\n' \
            "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" "$GAME_PID" "$MARKER" >> "$OUT_DIR/sample-watcher-receipt.txt"
        printf 'DIAGNOSTIC_ONLY_NOT_PERFORMANCE=1\nreason=sample overlaps measured phase 1; do not interpret callback throughput as performance evidence\n' \
            > "$RUN_DIR/output/diagnostic-only.txt"
        if ! kill -0 "$GAME_PID" 2>/dev/null; then
            printf 'status=pid_not_running\n' >> "$OUT_DIR/sample-watcher-receipt.txt"
            exit 4
        fi
        /usr/bin/sample "$GAME_PID" 5 1 -mayDie -file "$OUT_DIR/sample-during-measure.txt" \
            > "$OUT_DIR/sample-command.log" 2>&1
        SAMPLE_STATUS=$?
        printf 'sample_status=%s\nsample_finished_utc=%s\n' "$SAMPLE_STATUS" "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" >> "$OUT_DIR/sample-watcher-receipt.txt"
        if [[ -f "$OUT_DIR/sample-during-measure.txt" ]]; then
            printf 'sample_bytes=%s\n' "$(/usr/bin/stat -f '%z' "$OUT_DIR/sample-during-measure.txt")" >> "$OUT_DIR/sample-watcher-receipt.txt"
            /usr/bin/head -n 18 "$OUT_DIR/sample-during-measure.txt" > "$OUT_DIR/sample-header.txt"
        else
            printf 'sample_bytes=0\n' >> "$OUT_DIR/sample-watcher-receipt.txt"
        fi
        if /usr/bin/grep -q '^measure_end,' "$PHASES"; then
            printf 'measure_end_seen_before_receipt=1\n' >> "$OUT_DIR/sample-watcher-receipt.txt"
        else
            printf 'measure_end_seen_before_receipt=0\n' >> "$OUT_DIR/sample-watcher-receipt.txt"
        fi
        if [[ "$SAMPLE_STATUS" -eq 0 && -s "$OUT_DIR/sample-during-measure.txt" ]]; then
            printf 'status=sampled\n' >> "$OUT_DIR/sample-watcher-receipt.txt"
            exit 0
        fi
        printf 'status=sample_failed\n' >> "$OUT_DIR/sample-watcher-receipt.txt"
        exit 5
    fi
    if [[ -f "$PHASES" ]] && /usr/bin/grep -q '^measure_end,' "$PHASES"; then
        printf 'status=measure_start_missing\n' >> "$OUT_DIR/sample-watcher-receipt.txt"
        exit 6
    fi
    sleep 0.2
done

printf 'status=watcher_timeout\n' >> "$OUT_DIR/sample-watcher-receipt.txt"
exit 7
