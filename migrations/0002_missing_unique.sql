-- Migration 0002 (M2): deterministic missing-interval detection.
-- Gap detection is deterministic for a given stream, so re-processing after a crash
-- must not create duplicate interval rows; identity is (device, metric, start).
CREATE UNIQUE INDEX idx_missing_unique ON missing_intervals(device_id, metric_id, start_us);
