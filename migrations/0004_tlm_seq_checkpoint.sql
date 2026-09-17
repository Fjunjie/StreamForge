-- Migration 0004 (M2): separate the TLM stage-1 frame-sequence checkpoint from the
-- stage-2 staging cursor. Previously one column carried both meanings, which made
-- stage 2 skip every staged row of a TLM file (cursor = last frame sequence).
ALTER TABLE checkpoints ADD COLUMN stage1_tlm_seq INTEGER NOT NULL DEFAULT 0;
