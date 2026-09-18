#include "streamforge/rules/rule_engine.hpp"

#include <algorithm>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "streamforge/core/log.hpp"
#include "streamforge/core/time.hpp"
#include "streamforge/core/uuid.hpp"

namespace streamforge {
namespace rules {

using expr::EvalContext;
using expr::ExprValue;

const char* rule_state_name(RuleState state) {
    switch (state) {
    case RuleState::Normal:
        return "NORMAL";
    case RuleState::Pending:
        return "PENDING";
    case RuleState::Firing:
        return "FIRING";
    case RuleState::Recovering:
        return "RECOVERING";
    case RuleState::Cooldown:
        return "COOLDOWN";
    }
    return "NORMAL";
}

bool parse_rule_state(const std::string& name, RuleState& out) {
    if (name == "NORMAL") {
        out = RuleState::Normal;
        return true;
    }
    if (name == "PENDING") {
        out = RuleState::Pending;
        return true;
    }
    if (name == "FIRING") {
        out = RuleState::Firing;
        return true;
    }
    if (name == "RECOVERING") {
        out = RuleState::Recovering;
        return true;
    }
    if (name == "COOLDOWN") {
        out = RuleState::Cooldown;
        return true;
    }
    return false;
}

namespace {

Result<RuleType> parse_rule_type(const std::string& type) {
    if (type == "threshold")
        return Result<RuleType>::Ok(RuleType::Threshold);
    if (type == "duration")
        return Result<RuleType>::Ok(RuleType::Duration);
    if (type == "rate")
        return Result<RuleType>::Ok(RuleType::Rate);
    if (type == "absence")
        return Result<RuleType>::Ok(RuleType::Absence);
    if (type == "composite")
        return Result<RuleType>::Ok(RuleType::Composite);
    return Result<RuleType>::Err(Error::make(ErrorCode::ConfigValidation, "unknown rule type '" + type + "'"));
}

// True when the metric id exists as a base metric or a derived metric.
bool metric_exists(const ConfigSnapshot& cs, const std::string& id) {
    if (cs.metric(id) != nullptr)
        return true;
    for (const auto& dm : cs.cfg.derived_metrics)
        if (dm.id == id)
            return true;
    return false;
}

// Collects the first bind error. bind_* return Result<void> like prepare/step/commit,
// so every call site checks (audit consistency fix).
class BindGuard {
public:
    void operator()(Result<void> r) {
        if (!r.ok() && err_.ok())
            err_ = std::move(r);
    }
    [[nodiscard]] const Result<void>& result() const {
        return err_;
    }

private:
    Result<void> err_ = Result<void>::Ok();
};

} // namespace

RuleEngine::RuleEngine(storage::Store& store) : store_(&store) {}

Result<CompiledRule> compile_rule(const RuleCfg& raw, const ConfigSnapshot& cs) {
    CompiledRule rule;
    rule.id = raw.id;
    rule.version = 1;

    auto type = parse_rule_type(raw.type);
    if (!type.ok()) {
        Error e = type.error();
        e.ctx("rule", raw.id);
        return Result<CompiledRule>::Err(std::move(e));
    }
    rule.type = type.take();
    rule.severity = raw.severity;
    rule.trigger_expr_src = raw.trigger;
    rule.duration_us = raw.duration_us;
    rule.cooldown_us = raw.cooldown_us;
    rule.merge_interval_us = raw.merge_interval_us;
    rule.device_tags = raw.devices;

    auto trigger = expr::parse_expression(raw.trigger);
    if (!trigger.ok()) {
        Error e = trigger.error();
        e.ctx("rule", raw.id);
        e.ctx("field", "trigger");
        return Result<CompiledRule>::Err(std::move(e));
    }
    rule.trigger_ast = trigger.take();

    if (!raw.recovery.empty()) {
        auto recovery = expr::parse_expression(raw.recovery);
        if (!recovery.ok()) {
            Error e = recovery.error();
            e.ctx("rule", raw.id);
            e.ctx("field", "recovery");
            return Result<CompiledRule>::Err(std::move(e));
        }
        rule.recovery_ast = recovery.take();
        rule.recovery_expr_src = raw.recovery;
        rule.uses_recovery_expr = true;
    }

    // Metric references must exist in the config (base or derived metrics).
    std::vector<std::string> refs;
    expr::collect_metric_refs(*rule.trigger_ast, refs);
    if (rule.recovery_ast)
        expr::collect_metric_refs(*rule.recovery_ast, refs);
    for (const auto& m : refs) {
        if (!metric_exists(cs, m)) {
            Error e = Error::make(ErrorCode::ConfigValidation, "rule references unknown metric '" + m + "'");
            e.ctx("rule", raw.id);
            e.ctx("metric", m);
            return Result<CompiledRule>::Err(std::move(e));
        }
    }
    rule.metric_refs = std::move(refs);

    return Result<CompiledRule>::Ok(std::move(rule));
}

Result<RuleEvalResult> evaluate_rule(const CompiledRule& rule, const expr::EvalContext& ctx) {
    RuleEvalResult out;
    if (!ctx.lookup_metric)
        return Result<RuleEvalResult>::Err(streamforge::Error::make(streamforge::ErrorCode::InvalidArgument,
                                                                    "no metric provider for rule evaluation"));

    auto trigger = expr::evaluate(*rule.trigger_ast, ctx);
    if (!trigger.ok())
        return Result<RuleEvalResult>::Err(trigger.error());
    out.has_data = trigger.value().type != expr::ExprType::Null;
    if (!out.has_data) {
        // Insufficient data: hold the current state — neither trigger nor recover.
        out.condition_met = false;
        out.recovery_met = false;
        return Result<RuleEvalResult>::Ok(std::move(out));
    }
    auto flag = trigger.value().as_bool();
    if (!flag)
        return Result<RuleEvalResult>::Err(streamforge::Error::make(streamforge::ErrorCode::InvalidArgument,
                                                                    "trigger expression must evaluate to boolean"));
    out.condition_met = *flag;

    if (rule.uses_recovery_expr && rule.recovery_ast) {
        auto recovery = expr::evaluate(*rule.recovery_ast, ctx);
        if (!recovery.ok())
            return Result<RuleEvalResult>::Err(recovery.error());
        if (recovery.value().type == expr::ExprType::Null) {
            out.recovery_met = false; // recovery metric missing: hold
            return Result<RuleEvalResult>::Ok(std::move(out));
        }
        auto rflag = recovery.value().as_bool();
        if (!rflag)
            return Result<RuleEvalResult>::Err(streamforge::Error::make(
                streamforge::ErrorCode::InvalidArgument, "recovery expression must evaluate to boolean"));
        out.recovery_met = *rflag;
    } else {
        out.recovery_met = !out.condition_met;
    }
    return Result<RuleEvalResult>::Ok(std::move(out));
}

Result<void> RuleEngine::load_rules(const ConfigSnapshot& cs) {
    // Compile everything first: on any failure the previous rule set stays active.
    std::vector<CompiledRule> next;
    next.reserve(cs.cfg.rules.size());
    for (const auto& raw : cs.cfg.rules) {
        auto compiled = compile_rule(raw, cs);
        if (!compiled.ok())
            return Result<void>::Err(compiled.error());
        next.push_back(compiled.take());
    }

    std::lock_guard<std::mutex> lk(mu_);

    auto still_present = [&next](const std::string& id) {
        return std::any_of(next.begin(), next.end(), [&](const CompiledRule& r) { return r.id == id; });
    };

    // Close open incidents of rules removed by this reload (FR-RULE-003).
    std::vector<std::string> removed;
    for (const auto& old : rules_)
        if (!still_present(old.id))
            removed.push_back(old.id);
    for (const auto& id : removed) {
        auto closed = close_rule_incidents(id, "RULE_REMOVED");
        if (!closed.ok())
            return closed; // rules_ untouched; the caller may retry
    }

    rules_ = std::move(next);
    for (auto it = states_.begin(); it != states_.end();) {
        if (still_present(it->first.first))
            ++it;
        else
            it = states_.erase(it);
    }
    device_tags_.clear();
    for (const auto& d : cs.cfg.devices)
        device_tags_[d.id] = d.tags;
    config_version_ = cs.version;
    return Result<void>::Ok();
}

Result<void> RuleEngine::evaluate_device(const std::string& device_id, int64_t eval_time_us,
                                         const std::map<std::string, double>& metric_values,
                                         const std::map<std::string, std::string>& /*metric_units*/) {
    // metric_units is reserved for unit-aware rule checks (future work).
    std::lock_guard<std::mutex> lk(mu_);

    for (const auto& rule : rules_) {
        if (!device_matches(rule, device_id))
            continue;

        expr::EvalContext ctx;
        ctx.device_id = device_id;
        ctx.eval_time_us = eval_time_us;
        ctx.lookup_metric = [&metric_values](const std::string& m) -> std::optional<double> {
            auto it = metric_values.find(m);
            if (it == metric_values.end())
                return std::nullopt;
            return it->second;
        };
        // ctx.lookup_window stays unset: window functions in rule expressions surface
        // as an evaluation error and skip the rule (logged below) instead of failing
        // the pipeline.

        auto evaluated = evaluate_rule(rule, ctx);
        if (!evaluated.ok()) {
            logger("rules")->warn("rule '{}' evaluation failed for device '{}': {}", rule.id, device_id,
                                  evaluated.error().message);
            continue;
        }
        const RuleEvalResult& r = evaluated.value();
        DeviceState& st = ensure_state(rule.id, device_id);
        st.last_eval_us = eval_time_us;

        // Event-time driven state machine (FR-RULE-002). One evaluation may walk
        // COOLDOWN -> NORMAL -> (PENDING|FIRING); each branch breaks once the state
        // settles for this sample.
        for (;;) {
            if (st.state == RuleState::Cooldown) {
                if (eval_time_us - st.since_us < rule.cooldown_us)
                    break; // still suppressed
                auto t = transition_to(rule.id, device_id, RuleState::Normal, eval_time_us);
                if (!t.ok())
                    return t;
                // cooldown expired: fall through and process this sample as NORMAL
            }
            if (st.state == RuleState::Normal || st.state == RuleState::Pending) {
                if (!r.has_data)
                    break; // insufficient data: hold
                if (!r.condition_met) {
                    if (st.state == RuleState::Pending) {
                        auto t = transition_to(rule.id, device_id, RuleState::Normal, eval_time_us);
                        if (!t.ok())
                            return t;
                    }
                    break;
                }
                if (st.state == RuleState::Normal && rule.duration_us > 0) {
                    // condition met with a sustain requirement: start accumulating
                    auto t = transition_to(rule.id, device_id, RuleState::Pending, eval_time_us);
                    if (!t.ok())
                        return t;
                    break;
                }
                if (st.state == RuleState::Pending && eval_time_us - st.since_us < rule.duration_us)
                    break; // still accumulating
                auto f = fire_incident(rule, device_id, st, eval_time_us, metric_values);
                if (!f.ok())
                    return f;
                break;
            }
            if (st.state == RuleState::Firing || st.state == RuleState::Recovering) {
                if (st.state == RuleState::Firing && r.has_data && r.condition_met) {
                    auto hit = record_hit(st.open_incident_uuid, peak_value(rule, metric_values), eval_time_us);
                    if (!hit.ok())
                        return hit;
                }
                if (!r.has_data)
                    break; // hold: an open incident is not recovered on missing data
                if (st.state == RuleState::Recovering && !r.recovery_met) {
                    // recovery aborted mid-hold: back to FIRING, incident stays open
                    auto t = transition_to(rule.id, device_id, RuleState::Firing, eval_time_us);
                    if (!t.ok())
                        return t;
                    break;
                }
                if (!r.recovery_met)
                    break; // keep firing
                if (st.state == RuleState::Firing && rule.duration_us > 0) {
                    auto t = transition_to(rule.id, device_id, RuleState::Recovering, eval_time_us);
                    if (!t.ok())
                        return t;
                    break;
                }
                if (st.state == RuleState::Recovering && eval_time_us - st.since_us < rule.duration_us)
                    break; // recovery hold still accumulating
                auto c = close_active_incident(rule, device_id, st, eval_time_us);
                if (!c.ok())
                    return c;
                break;
            }
            break;
        }
    }
    return Result<void>::Ok();
}

RuleEngine::DeviceState& RuleEngine::ensure_state(const std::string& rule_id, const std::string& device_id) {
    auto key = std::make_pair(rule_id, device_id);
    auto it = states_.find(key);
    if (it != states_.end())
        return it->second;

    DeviceState st;
    // DB is the source of truth: restore the persisted machine state so a restart
    // continues where it left off instead of overwriting FIRING with defaults.
    auto s = store_->db().prepare(
        "SELECT state, since_us, last_eval_us, open_incident_uuid, last_incident_uuid, last_closed_us"
        " FROM rule_states WHERE rule_id=? AND rule_version=1 AND device_id=?");
    if (s.ok()) {
        BindGuard b;
        b(s.value().bind_text(1, rule_id));
        b(s.value().bind_text(2, device_id));
        if (b.result().ok()) {
            auto step = s.value().step();
            if (step.ok() && step.value() == storage::Stmt::Step::Row) {
                const storage::Stmt& row = s.value();
                if (!parse_rule_state(row.column_text(0), st.state))
                    st.state = RuleState::Normal;
                st.since_us = row.column_int64(1);
                st.last_eval_us = row.column_is_null(2) ? 0 : row.column_int64(2);
                st.open_incident_uuid = row.column_is_null(3) ? std::string() : row.column_text(3);
                st.last_incident_uuid = row.column_is_null(4) ? std::string() : row.column_text(4);
                st.last_closed_us = row.column_is_null(5) ? 0 : row.column_int64(5);
            }
        }
    }
    return states_.emplace(key, st).first->second;
}

Result<void> RuleEngine::persist_state(const std::string& rule_id, const std::string& device_id,
                                       const DeviceState& state) {
    auto st = store_->db().prepare(
        "INSERT INTO rule_states(rule_id, rule_version, device_id, state, since_us, last_eval_us,"
        " open_incident_uuid, last_incident_uuid, last_closed_us) VALUES(?, 1, ?, ?, ?, ?, ?, ?, ?)"
        " ON CONFLICT(rule_id, rule_version, device_id) DO UPDATE SET"
        " state=excluded.state, since_us=excluded.since_us, last_eval_us=excluded.last_eval_us,"
        " open_incident_uuid=excluded.open_incident_uuid, last_incident_uuid=excluded.last_incident_uuid,"
        " last_closed_us=excluded.last_closed_us");
    if (!st.ok())
        return Result<void>::Err(st.error());
    BindGuard b;
    b(st.value().bind_text(1, rule_id));
    b(st.value().bind_text(2, device_id));
    b(st.value().bind_text(3, rule_state_name(state.state)));
    b(st.value().bind_int64(4, state.since_us));
    b(st.value().bind_int64(5, state.last_eval_us));
    if (state.open_incident_uuid.empty())
        b(st.value().bind_null(6));
    else
        b(st.value().bind_text(6, state.open_incident_uuid));
    // last_incident_uuid is NOT NULL in the schema: bind '' instead of NULL.
    b(st.value().bind_text(7, state.last_incident_uuid));
    b(st.value().bind_int64(8, state.last_closed_us));
    if (!b.result().ok())
        return Result<void>::Err(b.result().error());
    auto step = st.value().step();
    if (!step.ok() || step.value() != storage::Stmt::Step::Done)
        return Result<void>::Err(Error::make(ErrorCode::DbStep, "rule state persist failed"));
    return Result<void>::Ok();
}

Result<void> RuleEngine::transition_to(const std::string& rule_id, const std::string& device_id, RuleState new_state,
                                       int64_t now_us) {
    auto& state = ensure_state(rule_id, device_id);
    state.state = new_state;
    state.since_us = now_us;
    return persist_state(rule_id, device_id, state);
}

Result<std::string> RuleEngine::open_incident(const CompiledRule& rule, const std::string& device_id, int64_t now_us,
                                              double peak) {
    std::string uuid = uuid_v4();
    static const char* kSql = "INSERT INTO incidents(incident_uuid, rule_id, rule_version, device_id, severity,"
                              " state, started_us, peak_value, hit_count, config_version, created_at_us)"
                              " VALUES(?, ?, ?, ?, ?, 'open', ?, ?, 1, ?, ?)";
    auto st = store_->db().prepare(kSql);
    if (!st.ok())
        return Result<std::string>::Err(st.error());
    BindGuard b;
    b(st.value().bind_text(1, uuid));
    b(st.value().bind_text(2, rule.id));
    b(st.value().bind_int64(3, rule.version));
    b(st.value().bind_text(4, device_id));
    b(st.value().bind_text(5, rule.severity));
    b(st.value().bind_int64(6, now_us));
    b(st.value().bind_double(7, peak));
    b(st.value().bind_int64(8, static_cast<int64_t>(config_version_)));
    b(st.value().bind_int64(9, now_us));
    if (!b.result().ok())
        return Result<std::string>::Err(b.result().error());
    auto step = st.value().step();
    if (!step.ok() || step.value() != storage::Stmt::Step::Done) {
        return Result<std::string>::Err(streamforge::Error::make(streamforge::ErrorCode::DbStep,
                                                                 "incident insert failed: " + store_->db().last_error()));
    }
    return Result<std::string>::Ok(std::move(uuid));
}

Result<void> RuleEngine::reopen_incident(const std::string& incident_uuid, double peak, int64_t now_us) {
    auto txn = storage::Txn::begin(store_->db());
    if (!txn.ok())
        return Result<void>::Err(txn.error());
    {
        auto st = txn.value().prepare(
            "UPDATE incidents SET state='open', ended_us=NULL, close_reason=NULL, last_hit_us=?,"
            " peak_value=MAX(peak_value, ?), reopen_count=reopen_count+1"
            " WHERE incident_uuid=? AND state='closed'");
        if (!st.ok())
            return Result<void>::Err(st.error());
        BindGuard b;
        b(st.value().bind_int64(1, now_us));
        b(st.value().bind_double(2, peak));
        b(st.value().bind_text(3, incident_uuid));
        if (!b.result().ok())
            return Result<void>::Err(b.result().error());
        auto step = st.value().step();
        if (!step.ok() || step.value() != storage::Stmt::Step::Done)
            return Result<void>::Err(Error::make(ErrorCode::DbStep, "incident reopen failed"));
        if (store_->db().changes() != 1)
            return Result<void>::Err(
                Error::make(ErrorCode::InvalidArgument, "reopen target is not a closed incident: " + incident_uuid));
    }
    {
        auto st = txn.value().prepare(
            "INSERT INTO incident_history(incident_uuid, at_us, from_state, to_state, reason)"
            " VALUES(?, ?, 'closed', 'open', 'REOPENED')");
        if (!st.ok())
            return Result<void>::Err(st.error());
        BindGuard b;
        b(st.value().bind_text(1, incident_uuid));
        b(st.value().bind_int64(2, now_us));
        if (!b.result().ok())
            return Result<void>::Err(b.result().error());
        auto step = st.value().step();
        if (!step.ok() || step.value() != storage::Stmt::Step::Done)
            return Result<void>::Err(Error::make(ErrorCode::DbStep, "incident reopen history insert failed"));
    }
    auto commit = txn.value().commit();
    if (!commit.ok())
        return Result<void>::Err(commit.error());
    return Result<void>::Ok();
}

Result<void> RuleEngine::record_hit(const std::string& incident_uuid, double peak, int64_t now_us) {
    auto st = store_->db().prepare(
        "UPDATE incidents SET last_hit_us=?, peak_value=MAX(peak_value, ?), hit_count=hit_count+1"
        " WHERE incident_uuid=? AND state='open'");
    if (!st.ok())
        return Result<void>::Err(st.error());
    BindGuard b;
    b(st.value().bind_int64(1, now_us));
    b(st.value().bind_double(2, peak));
    b(st.value().bind_text(3, incident_uuid));
    if (!b.result().ok())
        return Result<void>::Err(b.result().error());
    auto step = st.value().step();
    if (!step.ok() || step.value() != storage::Stmt::Step::Done)
        return Result<void>::Err(Error::make(ErrorCode::DbStep, "incident hit update failed"));
    return Result<void>::Ok();
}

Result<void> RuleEngine::close_incident_in_txn(storage::Txn& txn, const std::string& incident_uuid,
                                               const std::string& reason, int64_t now_us) {
    {
        // Only an open incident may be closed; otherwise the history row would lie.
        auto st = txn.prepare("SELECT state FROM incidents WHERE incident_uuid=?");
        if (!st.ok())
            return Result<void>::Err(st.error());
        BindGuard b;
        b(st.value().bind_text(1, incident_uuid));
        if (!b.result().ok())
            return Result<void>::Err(b.result().error());
        auto step = st.value().step();
        if (!step.ok())
            return Result<void>::Err(step.error());
        if (step.value() != storage::Stmt::Step::Row)
            return Result<void>::Err(
                Error::make(ErrorCode::InvalidArgument, "incident not found: " + incident_uuid));
        if (st.value().column_text(0) != "open")
            return Result<void>::Err(
                Error::make(ErrorCode::InvalidArgument, "incident is not open: " + incident_uuid));
    }
    {
        auto st =
            txn.prepare("UPDATE incidents SET state='closed', ended_us=?, close_reason=? WHERE incident_uuid=?");
        if (!st.ok())
            return Result<void>::Err(st.error());
        BindGuard b;
        b(st.value().bind_int64(1, now_us));
        b(st.value().bind_text(2, reason));
        b(st.value().bind_text(3, incident_uuid));
        if (!b.result().ok())
            return Result<void>::Err(b.result().error());
        auto step = st.value().step();
        if (!step.ok() || step.value() != storage::Stmt::Step::Done)
            return Result<void>::Err(Error::make(ErrorCode::DbStep, "incident close failed"));
    }
    {
        auto st = txn.prepare("INSERT INTO incident_history(incident_uuid, at_us, from_state, to_state, reason)"
                              " VALUES(?, ?, 'open', 'closed', ?)");
        if (!st.ok())
            return Result<void>::Err(st.error());
        BindGuard b;
        b(st.value().bind_text(1, incident_uuid));
        b(st.value().bind_int64(2, now_us));
        b(st.value().bind_text(3, reason));
        if (!b.result().ok())
            return Result<void>::Err(b.result().error());
        auto step = st.value().step();
        if (!step.ok() || step.value() != storage::Stmt::Step::Done)
            return Result<void>::Err(Error::make(ErrorCode::DbStep, "incident history insert failed"));
    }
    return Result<void>::Ok();
}

Result<void> RuleEngine::close_incident(const std::string& incident_uuid, const std::string& reason, int64_t now_us) {
    auto txn = storage::Txn::begin(store_->db());
    if (!txn.ok())
        return Result<void>::Err(txn.error());
    auto closed = close_incident_in_txn(txn.value(), incident_uuid, reason, now_us);
    if (!closed.ok())
        return closed; // Txn destructor rolls back
    auto commit = txn.value().commit();
    if (!commit.ok())
        return Result<void>::Err(commit.error());
    return Result<void>::Ok();
}

// Collects the rule's open incidents in one transaction and closes them together so
// a mid-batch failure leaves no partially closed set.
Result<void> RuleEngine::close_rule_incidents(const std::string& rule_id, const std::string& reason) {
    auto txn = storage::Txn::begin(store_->db());
    if (!txn.ok())
        return Result<void>::Err(txn.error());
    std::vector<std::string> uuids;
    {
        auto st = txn.value().prepare("SELECT incident_uuid FROM incidents WHERE rule_id=? AND state='open'");
        if (!st.ok())
            return Result<void>::Err(st.error());
        BindGuard b;
        b(st.value().bind_text(1, rule_id));
        if (!b.result().ok())
            return Result<void>::Err(b.result().error());
        for (;;) {
            auto step = st.value().step();
            if (!step.ok())
                return Result<void>::Err(step.error());
            if (step.value() != storage::Stmt::Step::Row)
                break;
            uuids.push_back(st.value().column_text(0));
        }
    }
    for (const auto& uuid : uuids) {
        auto closed = close_incident_in_txn(txn.value(), uuid, reason, now_unix_us());
        if (!closed.ok())
            return closed; // Txn destructor rolls back the whole batch
    }
    auto commit = txn.value().commit();
    if (!commit.ok())
        return Result<void>::Err(commit.error());
    return Result<void>::Ok();
}

Result<void> RuleEngine::fire_incident(const CompiledRule& rule, const std::string& device_id, DeviceState& st,
                                       int64_t now_us, const std::map<std::string, double>& metric_values) {
    double peak = peak_value(rule, metric_values);
    // Reopen within the merge window instead of opening a duplicate (FR-RULE-003).
    if (!st.last_incident_uuid.empty() && rule.merge_interval_us > 0 &&
        now_us - st.last_closed_us <= rule.merge_interval_us) {
        auto reopened = reopen_incident(st.last_incident_uuid, peak, now_us);
        if (!reopened.ok())
            return reopened;
        st.state = RuleState::Firing;
        st.since_us = now_us;
        st.open_incident_uuid = st.last_incident_uuid;
        st.last_incident_uuid.clear();
        st.last_closed_us = 0;
        return persist_state(rule.id, device_id, st);
    }
    auto uuid = open_incident(rule, device_id, now_us, peak);
    if (!uuid.ok())
        return Result<void>::Err(uuid.error());
    st.state = RuleState::Firing;
    st.since_us = now_us;
    st.open_incident_uuid = uuid.take();
    return persist_state(rule.id, device_id, st);
}

Result<void> RuleEngine::close_active_incident(const CompiledRule& rule, const std::string& device_id, DeviceState& st,
                                               int64_t now_us) {
    auto closed = close_incident(st.open_incident_uuid, "RECOVERED", now_us);
    if (!closed.ok())
        return closed;
    st.last_incident_uuid = st.open_incident_uuid;
    st.last_closed_us = now_us;
    st.open_incident_uuid.clear();
    st.state = rule.cooldown_us > 0 ? RuleState::Cooldown : RuleState::Normal;
    st.since_us = now_us;
    return persist_state(rule.id, device_id, st);
}

bool RuleEngine::device_matches(const CompiledRule& rule, const std::string& device_id) const {
    if (rule.device_tags.empty())
        return true;
    auto it = device_tags_.find(device_id);
    if (it == device_tags_.end())
        return false;
    for (const auto& [tag, value] : rule.device_tags) {
        bool found = false;
        for (const auto& dt : it->second) {
            if (dt.first == tag && dt.second == value) {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

// Peak of the metric values referenced by the rule; 0.0 when none has a value.
double RuleEngine::peak_value(const CompiledRule& rule, const std::map<std::string, double>& metric_values) const {
    double peak = 0.0;
    bool first = true;
    for (const auto& m : rule.metric_refs) {
        auto it = metric_values.find(m);
        if (it == metric_values.end())
            continue;
        if (first || it->second > peak) {
            peak = it->second;
            first = false;
        }
    }
    return peak;
}

Result<bool> RuleEngine::ack_incident(const std::string& incident_uuid, const std::string& by,
                                      const std::string& comment) {
    return store_->ack_incident(incident_uuid, by, comment, streamforge::now_unix_us());
}

} // namespace rules
} // namespace streamforge
