-- Migration 0005 (M3): persist the incident merge-window anchor on the rule state so a
-- restart inside the merge interval still reopens the original incident (FR-RULE-003)
-- instead of opening a duplicate one.
ALTER TABLE rule_states ADD COLUMN last_incident_uuid TEXT NOT NULL DEFAULT '';
ALTER TABLE rule_states ADD COLUMN last_closed_us INTEGER NOT NULL DEFAULT 0;
