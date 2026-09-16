#include "streamforge/processing/windows.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "streamforge/core/time.hpp"
#include "streamforge/core/types.hpp"

namespace streamforge {
namespace processing {

namespace {

struct WindowStats {
    int64_t sample_count = 0;
    int64_t valid_count = 0;
    int64_t missing_count = 0;
    int64_t bad_quality_count = 0;
    int64_t synthetic_count = 0;
    bool has_min_max = false;
    double min_value = 0.0;
    double max_value = 0.0;
    double sum_value = 0.0; // compensated (Kahan) sum
    double avg_value = 0.0;
    double stddev = 0.0; // population stddev (Welford)
    bool has_first_last = false;
    double first_value = 0.0;
    int64_t first_time_us = 0;
    double last_value = 0.0;
    int64_t last_time_us = 0;
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
};

// Recomputes one window from the durable samples (value stats use non-null,
// non-device-error rows; counts include everything per requirement 7a).
WindowStats compute_stats(storage::Store& store, const WindowAggregator::WindowKey& key, int64_t end_us,
                          const MetricCfg* metric) {
    WindowStats stats;
    auto st = store.db().prepare("SELECT event_time_us, value, quality, flags FROM samples WHERE device_id=? AND"
                                 " metric_id=? AND event_time_us>=? AND event_time_us<? ORDER BY event_time_us");
    if (!st.ok())
        return stats;
    st.value().bind_text(1, key.device_id);
    st.value().bind_text(2, key.metric_id);
    st.value().bind_int64(3, key.start_us);
    st.value().bind_int64(4, end_us);

    // Welford accumulator and Kahan compensation for the sum.
    double mean = 0.0, m2 = 0.0;
    double kahan_c = 0.0;
    std::vector<double> values;
    int64_t expected_capacity =
        metric && metric->expected_period_us ? (end_us - key.start_us) / *metric->expected_period_us : 0;

    for (;;) {
        auto step = st.value().step();
        if (!step.ok())
            return stats;
        if (step.value() != storage::Stmt::Step::Row)
            break;
        const int64_t t = st.value().column_int64(0);
        const bool value_null = st.value().column_is_null(1);
        const double v = st.value().column_double(1);
        const int64_t quality = st.value().column_int64(2);
        const int64_t flags = st.value().column_int64(3);

        stats.sample_count += 1;
        if ((flags & sample_flags::kSynthetic) != 0)
            stats.synthetic_count += 1;
        if (quality == 2)
            stats.bad_quality_count += 1;
        const bool value_ok = !value_null && quality != 2;
        if (!value_ok)
            continue;
        stats.valid_count += 1;
        if (!stats.has_min_max) {
            stats.has_min_max = true;
            stats.min_value = v;
            stats.max_value = v;
        } else {
            if (v < stats.min_value)
                stats.min_value = v;
            if (v > stats.max_value)
                stats.max_value = v;
        }
        if (!stats.has_first_last) {
            stats.has_first_last = true;
            stats.first_value = v;
            stats.first_time_us = t;
        }
        stats.last_value = v;
        stats.last_time_us = t;
        // Kahan compensated sum.
        double y = v - kahan_c;
        double t_sum = stats.sum_value + y;
        kahan_c = (t_sum - stats.sum_value) - y;
        stats.sum_value = t_sum;
        // Welford.
        double delta = v - mean;
        mean += delta / static_cast<double>(stats.valid_count);
        m2 += delta * (v - mean);
        values.push_back(v);
    }

    if (stats.valid_count > 0) {
        stats.avg_value = stats.sum_value / static_cast<double>(stats.valid_count);
        stats.stddev = stats.valid_count > 1 ? std::sqrt(m2 / static_cast<double>(stats.valid_count)) : 0.0;
        // Exact quantiles via the nearest-rank definition (error 0 <= the 1% allowance).
        auto q = [&](double fraction) {
            const double rank = std::ceil(fraction * static_cast<double>(values.size()));
            size_t idx = static_cast<size_t>(rank) - 1;
            if (idx >= values.size())
                idx = values.size() - 1;
            std::vector<double> copy = values;
            std::nth_element(copy.begin(), copy.begin() + static_cast<long>(idx), copy.end());
            return copy[idx];
        };
        stats.p50 = q(0.50);
        stats.p95 = q(0.95);
        stats.p99 = q(0.99);
    }

    // Missing count: expected sample positions (by period) minus the actual sample count,
    // clamped at zero; only when the metric declares an expected period.
    if (metric && metric->expected_period_us && expected_capacity > 0) {
        int64_t missing = expected_capacity - stats.sample_count;
        stats.missing_count = missing > 0 ? missing : 0;
    }
    return stats;
}

Result<void> write_window(storage::Store& store, const WindowAggregator::WindowKey& key, int64_t end_us,
                          int64_t version, const WindowStats& s, const MetricCfg* metric) {
    static const char* kSql =
        "INSERT INTO aggregates(device_id, metric_id, window_type, start_us, end_us, version,"
        " sample_count, valid_count, missing_count, bad_quality_count, synthetic_count,"
        " min_value, max_value, avg_value, sum_value, stddev, first_value, first_time_us,"
        " last_value, last_time_us, p50, p95, p99, superseded, created_at_us)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,0,?)"
        " ON CONFLICT(device_id, metric_id, window_type, start_us, version) DO UPDATE SET"
        " end_us=excluded.end_us, sample_count=excluded.sample_count,"
        " valid_count=excluded.valid_count, missing_count=excluded.missing_count,"
        " bad_quality_count=excluded.bad_quality_count, synthetic_count=excluded.synthetic_count,"
        " min_value=excluded.min_value, max_value=excluded.max_value, avg_value=excluded.avg_value,"
        " sum_value=excluded.sum_value, stddev=excluded.stddev,"
        " first_value=excluded.first_value, first_time_us=excluded.first_time_us,"
        " last_value=excluded.last_value, last_time_us=excluded.last_time_us,"
        " p50=excluded.p50, p95=excluded.p95, p99=excluded.p99, superseded=0,"
        " created_at_us=excluded.created_at_us";
    auto txn = storage::Txn::begin(store.db());
    if (!txn.ok())
        return Result<void>::Err(txn.error());
    auto st = txn.value().prepare(kSql);
    if (!st.ok())
        return Result<void>::Err(st.error());
    int idx = 1;
    st.value().bind_text(idx++, key.device_id);
    st.value().bind_text(idx++, key.metric_id);
    st.value().bind_text(idx++, WindowAggregator::window_type_text(key.window_seconds));
    st.value().bind_int64(idx++, key.start_us);
    st.value().bind_int64(idx++, end_us);
    st.value().bind_int64(idx++, version);
    st.value().bind_int64(idx++, s.sample_count);
    st.value().bind_int64(idx++, s.valid_count);
    st.value().bind_int64(idx++, s.missing_count);
    st.value().bind_int64(idx++, s.bad_quality_count);
    st.value().bind_int64(idx++, s.synthetic_count);
    if (s.has_min_max) {
        st.value().bind_double(idx++, s.min_value);
        st.value().bind_double(idx++, s.max_value);
    } else {
        st.value().bind_null(idx++);
        st.value().bind_null(idx++);
    }
    if (s.valid_count > 0) {
        st.value().bind_double(idx++, s.avg_value);
        st.value().bind_double(idx++, s.sum_value);
        st.value().bind_double(idx++, s.stddev);
        st.value().bind_double(idx++, s.first_value);
        st.value().bind_int64(idx++, s.first_time_us);
        st.value().bind_double(idx++, s.last_value);
        st.value().bind_int64(idx++, s.last_time_us);
        st.value().bind_double(idx++, s.p50);
        st.value().bind_double(idx++, s.p95);
        st.value().bind_double(idx++, s.p99);
    } else {
        for (int i = 0; i < 10; ++i)
            st.value().bind_null(idx++); // avg..p99
    }
    st.value().bind_int64(idx++, now_unix_us());
    auto step = st.value().step();
    if (!step.ok() || step.value() != storage::Stmt::Step::Done) {
        return Result<void>::Err(Error::make(ErrorCode::DbStep, "aggregate write failed: " + store.db().last_error()));
    }
    auto commit = txn.value().commit();
    if (!commit.ok())
        return Result<void>::Err(commit.error());
    return Result<void>::Ok();
}

} // namespace

std::string WindowAggregator::window_type_text(int64_t seconds) {
    return std::to_string(seconds) + "s";
}

int64_t WindowAggregator::align_down(int64_t t_us, int64_t window_seconds) {
    const int64_t size_us = window_seconds * 1000000LL;
    return (t_us / size_us) * size_us; // UTC-aligned: epoch is UTC-aligned by definition
}

WindowAggregator::WindowAggregator(const ConfigSnapshot& cs, storage::Store& store) : cs_(&cs), store_(&store) {}

bool WindowAggregator::is_closed(const WindowKey& key) {
    if (closed_.count(key))
        return true;
    auto st = store_->db().prepare("SELECT 1 FROM aggregates WHERE device_id=? AND metric_id=? AND window_type=? AND"
                                   " start_us=? LIMIT 1");
    if (!st.ok())
        return false;
    st.value().bind_text(1, key.device_id);
    st.value().bind_text(2, key.metric_id);
    st.value().bind_text(3, window_type_text(key.window_seconds));
    st.value().bind_int64(4, key.start_us);
    auto step = st.value().step();
    if (!step.ok() || step.value() != storage::Stmt::Step::Row)
        return false;
    closed_.insert(key);
    return true;
}

void WindowAggregator::on_sample(const NormalizedSample& sample) {
    const bool late = (sample.flags & sample_flags::kLate) != 0;
    for (int64_t seconds : cs_->cfg.windows) {
        WindowKey key{sample.device_id, sample.metric_id, seconds, align_down(sample.event_time_us, seconds)};
        if (closed_.count(key))
            continue;
        if (late && is_closed(key))
            continue; // closed window: only correction may touch it
        open_windows_[sample.device_id].insert(key);
    }
}

Result<int64_t> WindowAggregator::next_version(const WindowKey& key) {
    auto st = store_->db().prepare("SELECT COALESCE(MAX(version), 0) FROM aggregates WHERE device_id=? AND metric_id=?"
                                   " AND window_type=? AND start_us=?");
    if (!st.ok())
        return Result<int64_t>::Err(st.error());
    st.value().bind_text(1, key.device_id);
    st.value().bind_text(2, key.metric_id);
    st.value().bind_text(3, window_type_text(key.window_seconds));
    st.value().bind_int64(4, key.start_us);
    auto step = st.value().step();
    if (!step.ok() || step.value() != storage::Stmt::Step::Row) {
        return Result<int64_t>::Err(Error::make(ErrorCode::DbStep, "version query failed"));
    }
    return Result<int64_t>::Ok(st.value().column_int64(0) + 1);
}

Result<void> WindowAggregator::close_window(const WindowKey& key, int64_t end_us) {
    const MetricCfg* metric = cs_->metric(key.metric_id);
    WindowStats stats = compute_stats(*store_, key, end_us, metric);

    // Version selection: write version 1 unless it exists and was superseded by a
    // correction — then append the next free version (FR-AGG-003).
    auto v1_exists =
        store_->db().prepare("SELECT superseded FROM aggregates WHERE device_id=? AND metric_id=? AND window_type=?"
                             " AND start_us=? AND version=1");
    if (!v1_exists.ok())
        return Result<void>::Err(v1_exists.error());
    v1_exists.value().bind_text(1, key.device_id);
    v1_exists.value().bind_text(2, key.metric_id);
    v1_exists.value().bind_text(3, window_type_text(key.window_seconds));
    v1_exists.value().bind_int64(4, key.start_us);
    auto step = v1_exists.value().step();
    if (!step.ok())
        return Result<void>::Err(step.error());
    closed_.insert(key);
    if (step.value() != storage::Stmt::Step::Row) {
        return write_window(*store_, key, end_us, 1, stats, metric);
    }
    const int64_t v1_superseded = v1_exists.value().column_int64(0);
    if (v1_superseded == 0) {
        // Re-close of an open window (e.g. after a crash or re-import): refresh in place.
        return write_window(*store_, key, end_us, 1, stats, metric);
    }
    auto version = next_version(key);
    if (!version.ok())
        return Result<void>::Err(version.error());
    return write_window(*store_, key, end_us, version.value(), stats, metric);
}

Result<void> WindowAggregator::on_watermark(const std::string& device_id, int64_t watermark) {
    auto it = open_windows_.find(device_id);
    if (it == open_windows_.end())
        return Result<void>::Ok();
    std::set<WindowKey> pending = it->second; // close_window may erase from open_windows_
    for (const auto& key : pending) {
        int64_t end_us = key.start_us + key.window_seconds * 1000000LL;
        if (end_us <= watermark) {
            auto rc = close_window(key, end_us);
            if (!rc.ok())
                return rc;
            open_windows_[device_id].erase(key);
        }
    }
    return Result<void>::Ok();
}

Result<void> WindowAggregator::close_all(const std::string& device_id) {
    auto it = open_windows_.find(device_id);
    if (it == open_windows_.end())
        return Result<void>::Ok();
    std::set<WindowKey> pending = it->second;
    for (const auto& key : pending) {
        int64_t end_us = key.start_us + key.window_seconds * 1000000LL;
        auto rc = close_window(key, end_us);
        if (!rc.ok())
            return rc;
        open_windows_[device_id].erase(key);
    }
    return Result<void>::Ok();
}

Result<void> WindowAggregator::correct_for_sample(const NormalizedSample& sample) {
    if (!cs_->cfg.pipeline.window_correction)
        return Result<void>::Ok();
    for (int64_t seconds : cs_->cfg.windows) {
        WindowKey key{sample.device_id, sample.metric_id, seconds, align_down(sample.event_time_us, seconds)};
        int64_t end_us = key.start_us + seconds * 1000000LL;

        auto v1 = store_->db().prepare("SELECT superseded FROM aggregates WHERE device_id=? AND metric_id=? AND"
                                       " window_type=? AND start_us=? AND version=1");
        if (!v1.ok())
            return Result<void>::Err(v1.error());
        v1.value().bind_text(1, key.device_id);
        v1.value().bind_text(2, key.metric_id);
        v1.value().bind_text(3, window_type_text(seconds));
        v1.value().bind_int64(4, key.start_us);
        auto step = v1.value().step();
        if (!step.ok())
            return Result<void>::Err(step.error());
        if (step.value() != storage::Stmt::Step::Row) {
            continue; // window not closed yet: regular closing will include the sample
        }
        // Mark existing versions superseded and append a corrected one.
        auto sup = store_->db().prepare("UPDATE aggregates SET superseded=1 WHERE device_id=? AND metric_id=? AND"
                                        " window_type=? AND start_us=?");
        if (!sup.ok())
            return Result<void>::Err(sup.error());
        sup.value().bind_text(1, key.device_id);
        sup.value().bind_text(2, key.metric_id);
        sup.value().bind_text(3, window_type_text(seconds));
        sup.value().bind_int64(4, key.start_us);
        auto sup_step = sup.value().step();
        if (!sup_step.ok() || sup_step.value() != storage::Stmt::Step::Done) {
            return Result<void>::Err(Error::make(ErrorCode::DbStep, "supersede failed"));
        }
        const MetricCfg* metric = cs_->metric(key.metric_id);
        WindowStats stats = compute_stats(*store_, key, end_us, metric);
        auto version = next_version(key);
        if (!version.ok())
            return Result<void>::Err(version.error());
        auto rc = write_window(*store_, key, end_us, version.value(), stats, metric);
        if (!rc.ok())
            return rc;
        open_windows_[sample.device_id].erase(key);
    }
    return Result<void>::Ok();
}

} // namespace processing
} // namespace streamforge
