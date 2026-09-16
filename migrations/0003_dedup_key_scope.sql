-- Migration 0003 (M2): the (device, metric, event_time, normalized_value) dedup key only
-- applies to records WITHOUT a device-side sequence (FR-ORD-001). Sequenced records are
-- deduped by (device, metric, sequence) alone; the old broad index silently dropped
-- distinct sequenced samples that shared a timestamp and value.
DROP INDEX IF EXISTS idx_samples_dedup_key;
CREATE UNIQUE INDEX idx_samples_dedup_key
    ON samples(device_id, metric_id, event_time_us, normalized_value)
    WHERE normalized_value IS NOT NULL AND sequence IS NULL;
