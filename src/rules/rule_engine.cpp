#include "streamforge/rules/rule_engine.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>
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

RuleEngine::RuleEngine(storage::Store& store) : store_(&store) {}

Result<CompiledRule> compile_rule(const RuleCfg& raw, const ConfigSnapshot& cs) {
    (void)cs;
    CompiledRule rule;
    rule.id = raw.id;
    rule.version = 1;
    rule.severity = raw.severity;
    rule.trigger_expr_src = raw.trigger;
    rule.duration_us = raw.duration_us;
    rule.cooldown_us = raw.cooldown_us;
    rule.merge_interval_us = raw.merge_interval_us;
    rule.device_tags = raw.devices;

    auto trigger = expr::parse_expression(raw.trigger);
    if (!trigger.ok()) {
        streamforge::Error e = trigger.error();
        e.ctx("rule", raw.id);
        e.ctx("field", "trigger");
        return Result<CompiledRule>::Err(std::move(e));
    }
    rule.trigger_ast = trigger.take();

    if (!raw.recovery.empty()) {
        auto recovery = expr::parse_expression(raw.recovery);
        if (!recovery.ok()) {
            streamforge::Error e = recovery.error();
            e.ctx("rule", raw.id);
            e.ctx("field", "recovery");
            return Result<CompiledRule>::Err(std::move(e));
        }
        rule.recovery_ast = recovery.take();
        rule.uses_recovery_expr = true;
    }

    return Result<CompiledRule>::Ok(std::move(rule));
}

Result<RuleEvalResult> evaluate_rule(const CompiledRule& rule, const std::string& device_id,
                                     const expr::EvalContext& ctx) {
    RuleEvalResult out;
    if (!ctx.lookup_metric)
        return Result<RuleEvalResult>::Err(streamforge::Error::make(streamforge::ErrorCode::InvalidArgument,
                                                                    "no metric provider for rule evaluation"));

    auto trigger = expr::evaluate(*rule.trigger_ast, ctx);
    if (!trigger.ok())
        return Result<RuleEvalResult>::Err(trigger.error());
    auto flag = trigger.value().as_bool();
    if (!flag)
        return Result<RuleEvalResult>::Err(streamforge::Error::make(streamforge::ErrorCode::InvalidArgument,
                                                                    "trigger expression must evaluate to boolean"));
    out.condition_met = *flag;
    out.has_data = trigger.value().type != expr::ExprType::Null;

    if (rule.uses_recovery_expr && rule.recovery_ast) {
        auto recovery = expr::evaluate(*rule.recovery_ast, ctx);
        if (!recovery.ok())
            return Result<RuleEvalResult>::Err(recovery.error());
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
    rules_.clear();
    for (const auto& raw : cs.cfg.rules) {
        auto compiled = compile_rule(raw, cs);
        if (!compiled.ok()) {
            streamforge::Error e = compiled.error();
            return Result<void>::Err(std::move(e));
        }
        rules_.push_back(compiled.take());
    }
    return Result<void>::Ok();
}

Result<std::string> RuleEngine::open_incident(const CompiledRule& rule, const std::string& device_id, int64_t now_us,
                                              double peak_value) {
    std::string uuid = uuid_v4();
    static const char* kSql = "INSERT INTO incidents(incident_uuid, rule_id, rule_version, device_id, severity,"
                              " state, started_us, peak_value, hit_count, config_version, created_at_us)"
                              " VALUES(?, ?, ?, ?, ?, 'open', ?, ?, 1, ?, ?)";
    auto st = store_->db().prepare(kSql);
    if (!st.ok())
        return Result<std::string>::Err(st.error());
    st.value().bind_text(1, uuid);
    st.value().bind_text(2, rule.id);
    st.value().bind_int64(3, rule.version);
    st.value().bind_text(4, device_id);
    st.value().bind_text(5, rule.severity);
    st.value().bind_int64(6, now_us);
    st.value().bind_double(7, peak_value);
    st.value().bind_int64(8, 0);
    st.value().bind_int64(9, now_us);
    auto step = st.value().step();
    if (!step.ok() || step.value() != storage::Stmt::Step::Done) {
        return Result<std::string>::Err(streamforge::Error::make(
            streamforge::ErrorCode::DbStep, "incident insert failed: " + store_->db().last_error()));
    }
    return Result<std::string>::Ok(std::move(uuid));
}

Result<void> RuleEngine::close_incident(const std::string& incident_uuid, const std::string& reason, int64_t now_us) {
    auto txn = storage::Txn::begin(store_->db());
    if (!txn.ok())
        return Result<void>::Err(txn.error());
    {
        auto st = txn.value().prepare(
            "UPDATE incidents SET state='closed', ended_us=?, close_reason=? WHERE incident_uuid=?");
        if (!st.ok())
            return Result<void>::Err(st.error());
        st.value().bind_int64(1, now_us);
        st.value().bind_text(2, reason);
        st.value().bind_text(3, incident_uuid);
        auto step = st.value().step();
        if (!step.ok() || step.value() != storage::Stmt::Step::Done) {
            return Result<void>::Err(streamforge::Error::make(streamforge::ErrorCode::DbStep, "incident close failed"));
        }
    }
    {
        auto st = txn.value().prepare("INSERT INTO incident_history(incident_uuid, at_us, from_state, to_state, reason)"
                                      " VALUES(?, ?, 'open', 'closed', ?)");
        if (!st.ok())
            return Result<void>::Err(st.error());
        st.value().bind_text(1, incident_uuid);
        st.value().bind_int64(2, now_us);
        st.value().bind_text(3, reason);
        auto step = st.value().step();
        if (!step.ok() || step.value() != storage::Stmt::Step::Done) {
            return Result<void>::Err(
                streamforge::Error::make(streamforge::ErrorCode::DbStep, "incident history insert failed"));
        }
    }
    auto commit = txn.value().commit();
    if (!commit.ok())
        return Result<void>::Err(commit.error());
    return Result<void>::Ok();
}

Result<void> RuleEngine::persist_state(const std::string& rule_id, const std::string& device_id,
                                       const DeviceState& state) {
    auto st =
        store_->db().prepare("INSERT INTO rule_states(rule_id, rule_version, device_id, state, since_us, last_eval_us,"
                             " open_incident_uuid) VALUES(?, 1, ?, ?, ?, ?, ?)"
                             " ON CONFLICT(rule_id, rule_version, device_id) DO UPDATE SET"
                             " state=excluded.state, since_us=excluded.since_us,"
                             " last_eval_us=excluded.last_eval_us, open_incident_uuid=excluded.open_incident_uuid");
    if (!st.ok())
        return Result<void>::Err(st.error());
    st.value().bind_text(1, rule_id);
    st.value().bind_text(2, device_id);
    st.value().bind_text(3, rule_state_name(state.state));
    st.value().bind_int64(4, state.since_us);
    st.value().bind_int64(5, state.last_eval_us);
    if (state.open_incident_uuid.empty())
        st.value().bind_null(6);
    else
        st.value().bind_text(6, state.open_incident_uuid);
    auto step = st.value().step();
    if (!step.ok() || step.value() != storage::Stmt::Step::Done) {
        return Result<void>::Err(streamforge::Error::make(streamforge::ErrorCode::DbStep, "rule state persist failed"));
    }
    return Result<void>::Ok();
}

Result<void> RuleEngine::transition_to(const std::string& rule_id, const std::string& device_id, RuleState new_state,
                                       int64_t now_us) {
    auto key = std::make_pair(rule_id, device_id);
    auto& state = states_[key];
    state.state = new_state;
    state.since_us = now_us;
    return persist_state(rule_id, device_id, state);
}

Result<bool> RuleEngine::ack_incident(const std::string& incident_uuid, const std::string& by,
                                      const std::string& comment) {
    return store_->ack_incident(incident_uuid, by, comment, streamforge::now_unix_us());
}

Result<void> RuleEngine::close_rule_incidents(const std::string& rule_id, const std::string& reason) {
    auto incidents = store_->query_incidents("open", "", 10000);
    if (!incidents.ok())
        return Result<void>::Err(incidents.error());
    for (const auto& inc : incidents.value()) {
        if (inc.rule_id != rule_id)
            continue;
        auto closed = close_incident(inc.uuid, reason, streamforge::now_unix_us());
        if (!closed.ok())
            return closed;
    }
    return Result<void>::Ok();
}

} // namespace rules
} // namespace streamforge
