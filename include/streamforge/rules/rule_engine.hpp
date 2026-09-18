#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "streamforge/config/config.hpp"
#include "streamforge/core/error.hpp"
#include "streamforge/core/types.hpp"
#include "streamforge/processing/expression.hpp"
#include "streamforge/storage/store.hpp"

namespace streamforge {
namespace rules {

// Rule types (requirement FR-RULE-001).
enum class RuleType { Threshold, Duration, Rate, Absence, Composite };

// Rule state machine states (requirement FR-RULE-002).
enum class RuleState { Normal, Pending, Firing, Recovering, Cooldown };

const char* rule_state_name(RuleState state);
bool parse_rule_state(const std::string& name, RuleState& out);

// A compiled rule: expression ASTs validated against the config at load time
// (rule type mapped, metric references checked for existence).
struct CompiledRule {
    std::string id;
    int64_t version = 1;
    RuleType type = RuleType::Threshold;
    std::string severity;                                         // info|warning|high|critical
    std::string trigger_expr_src;                                 // raw trigger expression source
    expr::ExprPtr trigger_ast;                                    // parsed trigger expression
    std::string recovery_expr_src;                                // raw recovery expression source (empty = auto)
    expr::ExprPtr recovery_ast;                                   // parsed recovery expression (may be null)
    int64_t duration_us = 0;                                      // how long condition must hold (0 = immediate)
    int64_t cooldown_us = 0;                                      // suppression after event close
    int64_t merge_interval_us = 0;                                // reopen window for recently-closed incidents
    std::vector<std::pair<std::string, std::string>> device_tags; // selector
    bool uses_recovery_expr = false;                              // true if a custom recovery expression was configured
    std::vector<std::string> metric_refs;                         // deduped metric ids in trigger+recovery
                                                                  // (peak-value source)
};

// Compiles the raw rule config into a CompiledRule with parsed ASTs. Fails on an
// unknown rule type, unparsable expression, or a reference to a metric that is
// neither a base nor a derived metric of the config.
Result<CompiledRule> compile_rule(const RuleCfg& raw, const ConfigSnapshot& cs);

// A rule evaluation context for one (rule, device) pair.
struct RuleEvalResult {
    bool condition_met = false; // trigger expression evaluated to true
    bool recovery_met = false;  // recovery expression evaluated to true (if configured)
    bool has_data = true;       // false when metric data is insufficient (hold state)
};

// Evaluates the trigger and recovery expressions for a device. Insufficient data
// (Null result) yields has_data=false with both flags false — the caller holds the
// current state instead of triggering or recovering.
Result<RuleEvalResult> evaluate_rule(const CompiledRule& rule, const expr::EvalContext& ctx);

// Largest duration literal used by window functions (AVG/DELTA/RATE) across the rules,
// bounding the SampleBuffer retention the host must keep; 0 when none is used.
int64_t max_rule_window_us(const std::vector<CompiledRule>& rules);

// Manages the state machine transitions and incident lifecycle for all (rule, device)
// pairs. The rule_states and incidents tables are the source of truth; in-memory
// state is lazily restored from them so a restart never overwrites persisted state.
class RuleEngine {
public:
    explicit RuleEngine(storage::Store& store);

    // Loads/compiles rules from the config snapshot. Atomic: on any compile error the
    // previous rule set stays active. Rules removed by the reload get their open
    // incidents closed with reason RULE_REMOVED (FR-RULE-003). Called on startup and
    // hot reload. Thread-safe with evaluate_device.
    Result<void> load_rules(const ConfigSnapshot& cs);

    // Evaluates all rules matching the device and applies state transitions.
    // Called for each emitted sample. `metric_values` provides the current metric
    // values for the device (from the pipeline's processing chain). All timing
    // decisions use eval_time_us (event time, FR-RULE-002). Per-rule evaluation
    // errors are logged and skip that rule; they do not fail other rules.
    Result<void> evaluate_device(const std::string& device_id, int64_t eval_time_us,
                                 const std::map<std::string, double>& metric_values,
                                 const std::map<std::string, std::string>& metric_units);

    // Registers the window-function provider backing AVG/DELTA/RATE in rule
    // expressions (the host binds it to the rolling SampleBuffer). Without a
    // provider those functions report an evaluation error and skip the rule.
    void set_window_provider(
        std::function<std::optional<double>(const std::string& device, const std::string& metric, int64_t duration_us,
                                            const std::string& func)>
            provider);

    // Close all open incidents for a rule in one transaction.
    Result<void> close_rule_incidents(const std::string& rule_id, const std::string& reason);

    // Acknowledge an incident.
    Result<bool> ack_incident(const std::string& incident_uuid, const std::string& by, const std::string& comment);

private:
    struct DeviceState {
        RuleState state = RuleState::Normal;
        int64_t since_us = 0; // when the current state was entered
        int64_t last_eval_us = 0;
        std::string open_incident_uuid;
        std::string last_incident_uuid; // most recently closed incident (merge window)
        int64_t last_closed_us = 0;
    };

    // In-memory state for (rule, device); lazily restored from rule_states on first
    // touch so a restart continues from the persisted machine state.
    DeviceState& ensure_state(const std::string& rule_id, const std::string& device_id);

    // State transition and incident lifecycle helpers. All expect mu_ to be held.
    Result<void> persist_state(const std::string& rule_id, const std::string& device_id, const DeviceState& state);
    Result<void> transition_to(const std::string& rule_id, const std::string& device_id, RuleState new_state,
                               int64_t now_us);
    Result<std::string> open_incident(const CompiledRule& rule, const std::string& device_id, int64_t now_us,
                                      double peak_value);
    Result<void> reopen_incident(const std::string& incident_uuid, double peak_value, int64_t now_us);
    Result<void> record_hit(const std::string& incident_uuid, double peak_value, int64_t now_us);
    Result<void> close_incident_in_txn(storage::Txn& txn, const std::string& incident_uuid,
                                       const std::string& reason, int64_t now_us);
    Result<void> close_incident(const std::string& incident_uuid, const std::string& reason, int64_t now_us);
    Result<void> fire_incident(const CompiledRule& rule, const std::string& device_id, DeviceState& st, int64_t now_us,
                               const std::map<std::string, double>& metric_values);
    Result<void> close_active_incident(const CompiledRule& rule, const std::string& device_id, DeviceState& st,
                                       int64_t now_us);

    bool device_matches(const CompiledRule& rule, const std::string& device_id) const;
    double peak_value(const CompiledRule& rule, const std::map<std::string, double>& metric_values) const;

    storage::Store* store_;
    mutable std::mutex mu_; // guards rules_, states_, device_tags_, config_version_
    std::vector<CompiledRule> rules_;
    std::map<std::pair<std::string, std::string>, DeviceState> states_; // (rule_id, device_id)
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> device_tags_;
    uint64_t config_version_ = 0;
    std::function<std::optional<double>(const std::string& device, const std::string& metric, int64_t duration_us,
                                        const std::string& func)>
        window_provider_;
};

} // namespace rules
} // namespace streamforge
