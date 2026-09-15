-- StreamForge initial schema (M1).
-- Timestamps are UTC Unix microseconds; IDs of cross-process objects are UUIDv4 strings.

CREATE TABLE devices (
    id              TEXT PRIMARY KEY,
    display_name    TEXT NOT NULL DEFAULT '',
    timezone        TEXT NOT NULL DEFAULT 'UTC',
    tags_json       TEXT NOT NULL DEFAULT '{}',
    enabled         INTEGER NOT NULL DEFAULT 1,
    config_version  INTEGER NOT NULL,
    created_at_us   INTEGER NOT NULL
);

CREATE TABLE metrics (
    id                  TEXT PRIMARY KEY,
    canonical_unit      TEXT NOT NULL,
    valid_min           REAL,
    valid_max           REAL,
    expected_period_us  INTEGER,
    jitter_us           INTEGER,
    max_gap_us          INTEGER,
    interpolation       TEXT NOT NULL DEFAULT 'none',
    config_version      INTEGER NOT NULL,
    created_at_us       INTEGER NOT NULL
);

CREATE TABLE metric_units (
    metric_id TEXT NOT NULL REFERENCES metrics(id) ON DELETE CASCADE,
    unit      TEXT NOT NULL,
    PRIMARY KEY (metric_id, unit)
);

CREATE TABLE device_metrics (
    device_id TEXT NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    metric_id TEXT NOT NULL REFERENCES metrics(id) ON DELETE CASCADE,
    PRIMARY KEY (device_id, metric_id)
);

CREATE TABLE calibrations (
    device_id        TEXT NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    metric_id        TEXT NOT NULL REFERENCES metrics(id) ON DELETE CASCADE,
    seg_min          REAL NOT NULL,
    seg_max          REAL NOT NULL,
    slope            REAL NOT NULL,
    intercept        REAL NOT NULL,
    reject_unmatched INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (device_id, metric_id, seg_min)
);

CREATE TABLE source_files (
    id               TEXT PRIMARY KEY,
    path             TEXT NOT NULL,
    size_bytes       INTEGER NOT NULL,
    mtime_us         INTEGER NOT NULL,
    sha256_64k       TEXT NOT NULL,
    identity_hash    TEXT NOT NULL UNIQUE,
    format           TEXT NOT NULL,
    status           TEXT NOT NULL,
    record_count     INTEGER NOT NULL DEFAULT 0,
    accepted_count   INTEGER NOT NULL DEFAULT 0,
    format_errors    INTEGER NOT NULL DEFAULT 0,
    business_errors  INTEGER NOT NULL DEFAULT 0,
    error_summary    TEXT,
    config_version   INTEGER NOT NULL,
    first_seen_at_us INTEGER NOT NULL,
    completed_at_us  INTEGER
);
CREATE INDEX idx_source_files_path ON source_files(path);
CREATE INDEX idx_source_files_status ON source_files(status);

CREATE TABLE checkpoints (
    file_id       TEXT PRIMARY KEY REFERENCES source_files(id) ON DELETE CASCADE,
    stage         INTEGER NOT NULL DEFAULT 1,
    stage1_offset INTEGER NOT NULL DEFAULT 0,
    stage1_line   INTEGER NOT NULL DEFAULT 0,
    stage2_cursor INTEGER NOT NULL DEFAULT 0,
    updated_at_us INTEGER NOT NULL
);

-- File-level staging table (agreed design for error-rate handling):
-- parse and basic-validation results land here in batches; the formal pipeline only
-- consumes them after the whole file validated under the error-rate threshold.
CREATE TABLE staging_samples (
    seq            INTEGER PRIMARY KEY AUTOINCREMENT,
    file_id        TEXT NOT NULL REFERENCES source_files(id) ON DELETE CASCADE,
    position       INTEGER NOT NULL,
    line_no        INTEGER NOT NULL,
    ingest_time_us INTEGER NOT NULL,
    status         INTEGER NOT NULL,             -- 0 accepted, 1 format error, 2 business error
    device_id      TEXT,
    metric         TEXT,
    event_time_us  INTEGER,
    value          REAL,
    value_is_null  INTEGER NOT NULL DEFAULT 0,
    has_value      INTEGER NOT NULL DEFAULT 0,
    unit           TEXT,
    quality        INTEGER NOT NULL DEFAULT 0,
    sequence       INTEGER,
    tags_json      TEXT NOT NULL DEFAULT '{}',
    ext_json       TEXT,
    error_code     INTEGER,
    error_message  TEXT
);
CREATE INDEX idx_staging_file ON staging_samples(file_id, seq);

CREATE TABLE samples (
    id                 INTEGER PRIMARY KEY AUTOINCREMENT,
    sample_uuid        TEXT NOT NULL UNIQUE,
    device_id          TEXT NOT NULL REFERENCES devices(id),
    metric_id          TEXT NOT NULL REFERENCES metrics(id),
    event_time_us      INTEGER NOT NULL,
    ingest_time_us     INTEGER NOT NULL,
    value              REAL,                     -- SQL NULL encodes a null value
    input_unit         TEXT,
    quality            INTEGER NOT NULL DEFAULT 0,
    source_file_id     TEXT,
    source_position    INTEGER,
    sequence           INTEGER,
    flags              INTEGER NOT NULL DEFAULT 0,
    config_version     INTEGER NOT NULL,
    expression_version INTEGER,
    tags_json          TEXT NOT NULL DEFAULT '{}',
    normalized_value   TEXT                      -- deterministic dedup key text (used from M2)
);
CREATE UNIQUE INDEX idx_samples_dedup_seq
    ON samples(device_id, metric_id, sequence) WHERE sequence IS NOT NULL;
CREATE UNIQUE INDEX idx_samples_dedup_key
    ON samples(device_id, metric_id, event_time_us, normalized_value)
    WHERE normalized_value IS NOT NULL;
CREATE INDEX idx_samples_query ON samples(device_id, metric_id, event_time_us);

CREATE TABLE missing_intervals (
    id             INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id      TEXT NOT NULL,
    metric_id      TEXT NOT NULL,
    start_us       INTEGER NOT NULL,
    end_us         INTEGER NOT NULL,
    expected_count INTEGER NOT NULL,
    created_at_us  INTEGER NOT NULL
);
CREATE INDEX idx_missing_lookup ON missing_intervals(device_id, metric_id, start_us);

CREATE TABLE aggregates (
    id                INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id         TEXT NOT NULL,
    metric_id         TEXT NOT NULL,
    window_type       TEXT NOT NULL,
    start_us          INTEGER NOT NULL,
    end_us            INTEGER NOT NULL,
    version           INTEGER NOT NULL DEFAULT 1,
    sample_count      INTEGER NOT NULL DEFAULT 0,
    valid_count       INTEGER NOT NULL DEFAULT 0,
    missing_count     INTEGER NOT NULL DEFAULT 0,
    bad_quality_count INTEGER NOT NULL DEFAULT 0,
    synthetic_count   INTEGER NOT NULL DEFAULT 0,
    min_value         REAL,
    max_value         REAL,
    avg_value         REAL,
    sum_value         REAL,
    stddev            REAL,
    first_value       REAL,
    first_time_us     INTEGER,
    last_value        REAL,
    last_time_us      INTEGER,
    p50               REAL,
    p95               REAL,
    p99               REAL,
    superseded        INTEGER NOT NULL DEFAULT 0,
    created_at_us     INTEGER NOT NULL,
    UNIQUE (device_id, metric_id, window_type, start_us, version)
);

CREATE TABLE rules (
    id             TEXT NOT NULL,
    version        INTEGER NOT NULL,
    definition_json TEXT NOT NULL,
    config_version INTEGER NOT NULL,
    created_at_us  INTEGER NOT NULL,
    PRIMARY KEY (id, version)
);

CREATE TABLE rule_states (
    rule_id            TEXT NOT NULL,
    rule_version       INTEGER NOT NULL,
    device_id          TEXT NOT NULL,
    state              TEXT NOT NULL,
    since_us           INTEGER NOT NULL,
    last_eval_us       INTEGER,
    open_incident_uuid TEXT,
    PRIMARY KEY (rule_id, rule_version, device_id)
);

CREATE TABLE incidents (
    incident_uuid TEXT PRIMARY KEY,
    rule_id       TEXT NOT NULL,
    rule_version  INTEGER NOT NULL,
    device_id     TEXT NOT NULL,
    severity      TEXT NOT NULL,
    state         TEXT NOT NULL,                -- open|closed
    started_us    INTEGER NOT NULL,
    last_hit_us   INTEGER,
    ended_us      INTEGER,
    peak_value    REAL,
    hit_count     INTEGER NOT NULL DEFAULT 0,
    reopen_count  INTEGER NOT NULL DEFAULT 0,
    close_reason  TEXT,
    acked_by      TEXT,
    acked_at_us   INTEGER,
    ack_comment   TEXT,
    context_json  TEXT,
    config_version INTEGER NOT NULL,
    created_at_us INTEGER NOT NULL,
    UNIQUE (rule_id, rule_version, device_id, started_us)
);
CREATE INDEX idx_incidents_open ON incidents(state) WHERE ended_us IS NULL;
CREATE INDEX idx_incidents_lookup ON incidents(rule_id, device_id, started_us);

CREATE TABLE incident_history (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    incident_uuid TEXT NOT NULL REFERENCES incidents(incident_uuid) ON DELETE CASCADE,
    at_us         INTEGER NOT NULL,
    from_state    TEXT,
    to_state      TEXT,
    reason        TEXT,
    actor         TEXT,
    details_json  TEXT
);
CREATE INDEX idx_incident_history ON incident_history(incident_uuid, at_us);

CREATE TABLE audit_log (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    at_us        INTEGER NOT NULL,
    op           TEXT NOT NULL,
    actor        TEXT NOT NULL DEFAULT '',
    target       TEXT NOT NULL DEFAULT '',
    result       TEXT NOT NULL DEFAULT '',
    request_id   TEXT NOT NULL DEFAULT '',
    details_json TEXT NOT NULL DEFAULT '{}'
);
