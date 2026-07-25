#ifndef LRU_K_REPLACER_H
#define LRU_K_REPLACER_H

#include <unordered_map>
#include <list>
#include <vector>
#include <deque>
#include <cstddef>
#include <functional>
#include <memory>


 //History queue: Tracks recent accesses (count < k)
 //Hot queue: Tracks frames with k or more accesses

class LRUKReplacer {
public:

    explicit LRUKReplacer(size_t num_frames, size_t k = 2);
    
    ~LRUKReplacer() = default;

    // Disable copy and move
    LRUKReplacer(const LRUKReplacer&) = delete;
    LRUKReplacer& operator=(const LRUKReplacer&) = delete;
    LRUKReplacer(LRUKReplacer&&) = delete;
    LRUKReplacer& operator=(LRUKReplacer&&) = delete;

    void RecordAccess(size_t frame_id);

    void Pin(size_t frame_id);
    void Unpin(size_t frame_id);

    int Evict();

    void SetEvictable(size_t frame_id, bool evictable);

    size_t GetEvictableCount() const;
    size_t GetEvictionCount() const;   // Total frames evicted since construction/Reset
    size_t GetTotalFrames() const;

    bool IsEvictable(size_t frame_id) const;
    bool IsPinned(size_t frame_id) const;

    size_t GetAccessCount(size_t frame_id) const;

    void Reset();

private:
    struct FrameInfo {
        size_t frame_id;
        std::deque<size_t> access_history;  // Sliding window of last k timestamps
        size_t access_count;                // Total accesses recorded
        bool is_pinned;
        bool is_evictable;
        
        FrameInfo() : frame_id(0), access_count(0), is_pinned(false), 
                      is_evictable(false) {}
    };

    size_t total_frames_;
    size_t k_;
    size_t current_time_;
    size_t eviction_count_;
    
    std::vector<FrameInfo> frame_info_;
    
    // Cold queue: frames with < k accesses
    std::list<int> cold_queue_;
    std::unordered_map<int, std::list<int>::iterator> position_in_cold_;
    
    // Hot queue: frames with >= k accesses
    std::list<int> hot_queue_;
    std::unordered_map<int, std::list<int>::iterator> position_in_hot_;

    void AddToQueue(size_t frame_id);
    void RemoveFromQueue(size_t frame_id);
    size_t GetKthAccessTime(size_t frame_id) const;
};

#endif