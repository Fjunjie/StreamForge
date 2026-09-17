#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
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

// A compiled rule: expression ASTs resolved against the config at load time.
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
};

// Compiles the raw rule config into a CompiledRule with parsed ASTs.
Result<CompiledRule> compile_rule(const RuleCfg& raw, const ConfigSnapshot& cs);

// A rule evaluation context for one (rule, device) pair.
struct RuleEvalResult {
    bool condition_met = false; // trigger expression evaluated to true
    bool recovery_met = false;  // recovery expression evaluated to true (if configured)
    bool has_data = true;       // false when metric data is insufficient
};

// Evaluates the trigger and recovery expressions for a device.
// The lookup functions provide metric values from the device's latest samples.
Result<RuleEvalResult> evaluate_rule(const CompiledRule& rule, const std::string& device_id,
                                     const expr::EvalContext& ctx);

// Manages the state machine transitions and incident lifecycle for all (rule, device)
// pairs. State is persisted in the rule_states and incidents tables.
class RuleEngine {
public:
    explicit RuleEngine(storage::Store& store);

    // Loads/compiles rules from the config snapshot. Called on startup and hot reload.
    Result<void> load_rules(const ConfigSnapshot& cs);

    // Evaluates all rules for the given device and applies state transitions.
    // Called for each emitted sample. `metric_values` provides the current metric
    // values for the device (from the pipeline's processing chain).
    Result<void> evaluate_device(const std::string& device_id, int64_t eval_time_us,
                                 const std::map<std::string, double>& metric_values,
                                 const std::map<std::string, std::string>& metric_units);

    // Close all FIRING incidents for a rule (called when the rule is deleted on hot reload).
    Result<void> close_rule_incidents(const std::string& rule_id, const std::string& reason);

    // Persist the current state machine snapshot for a (rule, device).
    struct PersistedState {
        std::string rule_id;
        int64_t rule_version = 0;
        std::string device_id;
        RuleState state = RuleState::Normal;
        int64_t since_us = 0; // when the current state was entered
        std::string open_incident_uuid;
    };

    // Acknowledge an incident.
    Result<bool> ack_incident(const std::string& incident_uuid, const std::string& by, const std::string& comment);

private:
    struct DeviceState {
        RuleState state = RuleState::Normal;
        int64_t since_us = 0; // when the current state was entered
        int64_t last_eval_us = 0;
        std::string open_incident_uuid;
        int64_t cooldown_until_us = 0;
    };

    // State transition helpers.
    Result<void> transition_to(const std::string& rule_id, const std::string& device_id, RuleState new_state,
                               int64_t now_us);
    Result<std::string> open_incident(const CompiledRule& rule, const std::string& device_id, int64_t now_us,
                                      double peak_value);
    Result<void> close_incident(const std::string& incident_uuid, const std::string& reason, int64_t now_us);
    Result<void> persist_state(const std::string& rule_id, const std::string& device_id, const DeviceState& state);

    storage::Store* store_;
    std::vector<CompiledRule> rules_;
    std::map<std::pair<std::string, std::string>, DeviceState> states_; // (rule_id, device_id)
    expr::EvalContext eval_ctx_;
};

} // namespace rules
} // namespace streamforge
