#pragma once

#include <cstddef>
#include <list>
#include <string>
#include <unordered_map>

#include "streamforge/processing/types.hpp"
#include "streamforge/storage/store.hpp"

namespace streamforge {
namespace processing {

// Deduplication index (requirement FR-ORD-001, agreed design):
//   - the samples table (unique indexes) is the final authority;
//   - this class keeps a bounded LRU cache of recently seen keys in front of the database
//     to avoid a query per record on hot streams;
//   - a cache miss queries SQLite; results are never decided by a probabilistic filter.
// A duplicate is a record whose dedup key already exists in the samples table.
class DedupIndex {
public:
    explicit DedupIndex(storage::Store& store, size_t cache_capacity = 65536);

    // Returns true when the sample is a duplicate (its key already exists).
    // Marks the key as seen on a miss.
    Result<bool> is_duplicate(const NormalizedSample& sample);

    // Marks a key as seen without a database check (used after INSERT OR IGNORE so the
    // cache reflects what is now durable).
    void mark_seen(const NormalizedSample& sample);

private:
    struct CacheNode {
        std::string key;
    };

    std::string key_of(const NormalizedSample& sample) const;
    void touch(const std::string& key);
    void insert(const std::string& key);

    storage::Store* store_;
    size_t capacity_;
    std::list<std::string> lru_; // front = most recent
    std::unordered_map<std::string, std::list<std::string>::iterator> index_;
};

} // namespace processing
} // namespace streamforge
