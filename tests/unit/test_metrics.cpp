#include <catch2/catch_test_macros.hpp>

#include <string>

#include "streamforge/core/metrics.hpp"

using namespace streamforge;

TEST_CASE("metrics registry counters and gauges", "[metrics]") {
    MetricsRegistry::instance().reset();

    auto& m = MetricsRegistry::instance();
    m.inc_counter("sf_samples_accepted_total");
    m.inc_counter("sf_samples_accepted_total");
    m.inc_counter("sf_samples_accepted_total", {{"device", "dev-01"}}, 5.0);

    CHECK(m.counter("sf_samples_accepted_total") == 2.0);
    CHECK(m.counter("sf_samples_accepted_total", {{"device", "dev-01"}}) == 5.0);
    CHECK(m.counter("sf_samples_accepted_total", {{"device", "dev-02"}}) == 0.0);
    CHECK(m.counter("unknown_metric") == 0.0);
    // A counter read through the gauge accessor is 0 (kind mismatch).
    CHECK(m.gauge("sf_samples_accepted_total") == 0.0);

    m.set_gauge("sf_queue_depth", {{"stage", "ingest"}}, 42.0);
    CHECK(m.gauge("sf_queue_depth", {{"stage", "ingest"}}) == 42.0);
    m.set_gauge("sf_queue_depth", {{"stage", "ingest"}}, 7.0);
    CHECK(m.gauge("sf_queue_depth", {{"stage", "ingest"}}) == 7.0);
}

TEST_CASE("metrics render the Prometheus text format", "[metrics]") {
    MetricsRegistry::instance().reset();
    auto& m = MetricsRegistry::instance();

    m.set_gauge("sf_queue_depth", {{"stage", "ingest"}}, 3.0);
    m.set_gauge("sf_queue_depth", {{"stage", "ingest"}}, 5.0); // same series: overwritten
    m.set_gauge("sf_queue_depth", {{"stage", "agg"}}, 9.0);
    m.inc_counter("sf_samples_total");

    auto text = m.render_prometheus();
    CHECK(text.find("# TYPE sf_queue_depth gauge\n") != std::string::npos);
    CHECK(text.find("sf_queue_depth{stage=\"agg\"} 9\n") != std::string::npos);
    CHECK(text.find("sf_queue_depth{stage=\"ingest\"} 5\n") != std::string::npos);
    CHECK(text.find("# TYPE sf_samples_total counter\n") != std::string::npos);
    CHECK(text.find("sf_samples_total 1\n") != std::string::npos);
    // Deterministic ordering: queue_depth family comes before samples_total.
    CHECK(text.find("sf_queue_depth") < text.find("sf_samples_total"));
}
