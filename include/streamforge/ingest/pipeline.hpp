#pragma once

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "streamforge/config/config.hpp"
#include "streamforge/core/error.hpp"
#include "streamforge/core/types.hpp"
#include "streamforge/core/units.hpp"
#include "streamforge/ingest/error_report.hpp"
#include "streamforge/ingest/file_identity.hpp"
#include "streamforge/processing/calibration.hpp"
#include "streamforge/processing/dedup.hpp"
#include "streamforge/processing/missing.hpp"
#include "streamforge/processing/types.hpp"
#include "streamforge/processing/watermark.hpp"
#include "streamforge/processing/windows.hpp"
#include "streamforge/storage/store.hpp"

namespace streamforge {

// Two-phase import pipeline (agreed error-rate design):
//
//   Stage 1 (PROCESSING_STAGE1): stream-parse the file, run basic validation and write the
//   results batch-by-batch into the staging table. No samples, aggregates or rules are
//   touched. Memory stays bounded because batches flush to SQLite.
//
//   Decision at EOF: when the business error rate exceeds the configured threshold the
//   staging data is dropped, the file is marked QUARANTINED and moved to the quarantine
//   directory with an .error.json report. Otherwise the file becomes VALIDATED.
//
//   Stage 2 (PROCESSING_STAGE2): batches from the staging table enter the formal pipeline
//   (in M1: direct sample insertion; calibration/conversion/dedup arrive in M2) with its own
//   checkpoint. On completion the staging rows are deleted and the file is archived.
//
// Every stage commits parse progress, staging rows and checkpoints in the same transaction,
// so a crash at any point resumes without duplicating business results (FR-DB-002/FR-REC-*).
class ImportPipeline {
public:
    struct ProcessResult {
        enum class Outcome { Completed, SkippedDuplicate, Quarantined, Failed, Interrupted };
        Outcome outcome = Outcome::Failed;
        std::string file_id;
        Error error;
        int64_t accepted = 0;
        int64_t format_errors = 0;
        int64_t business_errors = 0;
        int64_t total = 0;
    };

    ImportPipeline(std::shared_ptr<const ConfigSnapshot> cs, std::shared_ptr<storage::Store> store);

    // Test hook: invoked before committing batch `index` (1-based, per stage); returning true
    // simulates a crash before the commit so recovery can be exercised.
    std::function<bool(int index)> fault_injector;
    // Graceful shutdown: checked between batches; when true the file stays resumable.
    std::function<bool()> should_stop;

    ProcessResult process_file(const std::string& path);

    // Startup recovery (FR-REC-002): resumes files left PROCESSING/VALIDATED, marks rows whose
    // source changed, and completes pending archive moves for COMPLETED files.
    struct RecoveryStats {
        int resumed = 0;
        int source_changed = 0;
        int archived = 0;
        int missing = 0;
    };
    Result<RecoveryStats> recover_pending();

private:
    struct Stage1Context {
        storage::FileCounts counts;
        storage::CheckpointRow cp;
        std::vector<SampleError> sample_errors; // first 100 for the quarantine report
    };

    ProcessResult handle_existing(const storage::SourceFileRow& row);
    ProcessResult start_new_file(const FileIdentity& identity);
    ProcessResult run_stage1(const storage::SourceFileRow& row, const FileIdentity& identity, bool resume,
                             int64_t resume_offset, int64_t resume_line_base, int64_t resume_tlm_sequence);
    // Formal processing (M2): calibration, unit conversion, dedup, watermark reorder,
    // missing/interpolation and window aggregation over one file's staged records.
    Result<void> run_stage2(const storage::SourceFileRow& row);
    void init_stage2_context(const storage::SourceFileRow& row);
    void ensure_device_seeded(const std::string& device_id);
    // Normalizes one staged record: calibration -> unit conversion -> validity range.
    [[nodiscard]] Result<processing::NormalizedSample> normalize_staged(const storage::StagedRecord& r) const;
    // Processes one normalized sample: dedup pre-check, sample row for the current batch
    // and window registration. Synthetic samples skip the dedup pre-check and rely on
    // INSERT OR IGNORE. Returns the row appended to `out`.
    Result<void> process_emitted(const storage::SourceFileRow& file, processing::NormalizedSample& s, bool synthetic,
                                 std::vector<storage::SampleRow>& out);
    ProcessResult quarantine_now(const storage::SourceFileRow& row, const std::string& error_code,
                                 const std::string& summary, const Stage1Context& ctx);

    std::shared_ptr<const ConfigSnapshot> cs_;
    std::shared_ptr<storage::Store> store_;
    int fault_batch_counter_ = 0; // global batch index across both stages of one file
    // Per-file stage-2 context (rebuilt from the database on resume).
    std::unique_ptr<processing::DedupIndex> dedup_;
    std::unique_ptr<processing::WindowAggregator> windows_;
    std::unique_ptr<processing::GapDetector> gaps_;
    std::unique_ptr<processing::ReorderBuffer> reorder_;
    std::unique_ptr<std::vector<processing::NormalizedSample>> emitted_;
    std::unique_ptr<std::vector<processing::NormalizedSample>> synth_buffer_;
    std::unique_ptr<std::vector<processing::GapDetector::MissingInterval>> interval_buffer_;
    std::set<std::string> seeded_devices_;
};

} // namespace streamforge
