// Copyright (C) 2022 Speedb Ltd. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once
#include <assert.h>
#include <stdlib.h>

#include <algorithm>
#include <atomic>
#include <type_traits>

#include "memory/allocator.h"
#include "port/likely.h"
#include "port/port.h"
#include "rocksdb/slice.h"
#include "util/coding.h"
#include "util/random.h"

namespace ROCKSDB_NAMESPACE {

template <class Comparator>
class SpdbSortedList {
 private:
  struct Node;
  struct Splice;

 public:
  using DecodedKey =
      typename std::remove_reference<Comparator>::type::DecodedType;

  static const uint16_t kMaxPossibleHeight = 32;

  // Create a new SpdbSortedList object that will use "cmp" for comparing
  // keys, and will allocate memory using "*allocator".  Objects allocated
  // in the allocator must remain allocated for the lifetime of the
  // spdbsortedlist object.
  explicit SpdbSortedList(Comparator cmp, Allocator* allocator,
                          int32_t max_height = 12,
                          int32_t branching_factor = 4);
  // No copying allowed
  SpdbSortedList(const SpdbSortedList&) = delete;
  SpdbSortedList& operator=(const SpdbSortedList&) = delete;

  // Allocates a key and a spdbsortedlist node, returning a pointer to the key
  // portion of the node.  This method is thread-safe if the allocator
  // is thread-safe.
  char* AllocateSpdbItem(size_t key_size, size_t header_size,
                         char** spdb_sorted_key);

  // Allocate a splice using allocator.
  Splice* AllocateSplice();

  // Allocate a splice on heap.
  Splice* AllocateSpliceOnHeap();

  // Inserts a key allocated by AllocateKey, after the actual key value
  // has been filled in.
  //
  // REQUIRES: nothing that compares equal to key is currently in the list.
  // REQUIRES: no concurrent calls to any of inserts.
  bool Insert(const char* key, bool concurrently);

  template <bool UseCAS>
  bool Insert(const char* key, Splice* splice, bool allow_partial_splice_fix);

  // Return estimated number of entries smaller than `key`.
  uint64_t EstimateCount(const char* key) const;

  // Iteration over the contents of a spdbsortedlist
  class Iterator {
   public:
    // Initialize an iterator over the specified list.
    // The returned iterator is not valid.
    explicit Iterator(const SpdbSortedList* list);

    // Change the underlying spdbsortedlist used for this iterator
    // This enables us not changing the iterator without deallocating
    // an old one and then allocating a new one
    void SetList(const SpdbSortedList* list);

    void SetSeek(const char* key);

    // Returns true iff the iterator is positioned at a valid node.
    bool Valid() const;

    // Returns the key at the current position.
    // REQUIRES: Valid()
    const char* key() const;

    // Advances to the next position.
    // REQUIRES: Valid()
    void Next();

    // Advances to the previous position.
    // REQUIRES: Valid()
    void Prev();

    // Advance to the first entry with a key >= target
    void Seek(const char* target);

    // Retreat to the last entry with a key <= target
    void SeekForPrev(const char* target);

    // Advance to a random entry in the list.
    void RandomSeek();

    // Position at the first entry in list.
    // Final state of iterator is Valid() iff list is not empty.
    void SeekToFirst();

    // Position at the last entry in list.
    // Final state of iterator is Valid() iff list is not empty.
    void SeekToLast();

   private:
    const SpdbSortedList* list_;
    Node* node_;
    // Intentionally copyable
  };

 private:
  const uint16_t kMaxHeight_;
  const uint16_t kBranching_;
  const uint32_t kScaledInverseBranching_;

  Allocator* const allocator_;  // Allocator used for allocations of nodes
  // Immutable after construction
  Comparator const compare_;
  Node* const head_;

  // Modified only by Insert().  Read racily by readers, but stale
  // values are ok.
  std::atomic<int> max_height_;  // Height of the entire list

  // seq_splice_ is a Splice used for insertions in the non-concurrent
  // case.  It caches the prev and next found during the most recent
  // non-concurrent insertion.
  Splice* seq_splice_;

  inline int GetMaxHeight() const {
    return max_height_.load(std::memory_order_relaxed);
  }

  int RandomHeight();

  Node* AllocateNode(size_t key_size, int height);

  bool Equal(const char* a, const char* b) const {
    return (compare_(a, b) == 0);
  }

  bool LessThan(const char* a, const char* b) const {
    return (compare_(a, b) < 0);
  }

  // Return true if key is greater than the data stored in "n".  Null n
  // is considered infinite.  n should not be head_.
  bool KeyIsAfterNode(const char* key, Node* n) const;
  bool KeyIsAfterNode(const DecodedKey& key, Node* n) const;

  // Returns the earliest node with a key >= key.
  // Return nullptr if there is no such node.
  Node* FindGreaterOrEqual(const char* key) const;

  // Return the latest node with a key < key.
  // Return head_ if there is no such node.
  // Fills prev[level] with pointer to previous node at "level" for every
  // level in [0..max_height_-1], if prev is non-null.
  Node* FindLessThan(const char* key, Node** prev = nullptr) const;

  // Return the latest node with a key < key on bottom_level. Start searching
  // from root node on the level below top_level.
  // Fills prev[level] with pointer to previous node at "level" for every
  // level in [bottom_level..top_level-1], if prev is non-null.
  Node* FindLessThan(const char* key, Node** prev, Node* root, int top_level,
                     int bottom_level) const;

  // Return the last node in the list.
  // Return head_ if list is empty.
  Node* FindLast() const;

  // Returns a random entry.
  Node* FindRandomEntry() const;

  // Traverses a single level of the list, setting *out_prev to the last
  // node before the key and *out_next to the first node after. Assumes
  // that the key is not present in the spdbsortedlist. On entry, before should
  // point to a node that is before the key, and after should point to
  // a node that is after the key.  after should be nullptr if a good after
  // node isn't conveniently available.
  template <bool prefetch_before>
  void FindSpliceForLevel(const DecodedKey& key, Node* before, Node* after,
                          int level, Node** out_prev, Node** out_next);

  // Recomputes Splice levels from highest_level (inclusive) down to
  // lowest_level (inclusive).
  void RecomputeSpliceLevels(const DecodedKey& key, Splice* splice,
                             int recompute_level);
};

// Implementation details follow

template <class Comparator>
struct SpdbSortedList<Comparator>::Splice {
  // The invariant of a Splice is that prev_[i+1].key <= prev_[i].key <
  // next_[i].key <= next_[i+1].key for all i.  That means that if a
  // key is bracketed by prev_[i] and next_[i] then it is bracketed by
  // all higher levels.  It is _not_ required that prev_[i]->Next(i) ==
  // next_[i] (it probably did at some point in the past, but intervening
  // or concurrent operations might have inserted nodes in between).
  int height_ = 0;
  Node** prev_;
  Node** next_;
};

// The Node data type is more of a pointer into custom-managed memory than
// a traditional C++ struct.  The key is stored in the bytes immediately
// after the struct, and the next_ pointers for nodes with height > 1 are
// stored immediately _before_ the struct.  This avoids the need to include
// any pointer or sizing data, which reduces per-node memory overheads.
template <class Comparator>
struct SpdbSortedList<Comparator>::Node {
  // Stores the height of the node in the memory location normally used for
  // next_[0].  This is used for passing data from AllocateKey to Insert.
  void StashHeight(const int height) {
    assert(sizeof(int) <= sizeof(next_[0]));
    memcpy(static_cast<void*>(&next_[0]), &height, sizeof(int));
  }

  // Retrieves the value passed to StashHeight.  Undefined after a call
  // to SetNext or NoBarrier_SetNext.
  int UnstashHeight() const {
    int rv;
    memcpy(&rv, &next_[0], sizeof(int));
    return rv;
  }

  const char* Key() const { return reinterpret_cast<const char*>(&next_[1]); }

  // Accessors/mutators for links.  Wrapped in methods so we can add
  // the appropriate barriers as necessary, and perform the necessary
  // addressing trickery for storing links below the Node in memory.
  Node* Next(int n) {
    assert(n >= 0);
    // Use an 'acquire load' so that we observe a fully initialized
    // version of the returned Node.
    return ((&next_[0] - n)->load(std::memory_order_acquire));
  }

  void SetNext(int n, Node* x) {
    assert(n >= 0);
    // Use a 'release store' so that anybody who reads through this
    // pointer observes a fully initialized version of the inserted node.
    (&next_[0] - n)->store(x, std::memory_order_release);
  }

  bool CASNext(int n, Node* expected, Node* x) {
    assert(n >= 0);
    return (&next_[0] - n)->compare_exchange_strong(expected, x);
  }

  // No-barrier variants that can be safely used in a few locations.
  Node* NoBarrier_Next(int n) {
    assert(n >= 0);
    return (&next_[0] - n)->load(std::memory_order_relaxed);
  }

  void NoBarrier_SetNext(int n, Node* x) {
    assert(n >= 0);
    (&next_[0] - n)->store(x, std::memory_order_relaxed);
  }

  // Insert node after prev on specific level.
  void InsertAfter(Node* prev, int level) {
    // NoBarrier_SetNext() suffices since we will add a barrier when
    // we publish a pointer to "this" in prev.
    NoBarrier_SetNext(level, prev->NoBarrier_Next(level));
    prev->SetNext(level, this);
  }

 private:
  // next_[0] is the lowest level link (level 0).  Higher levels are
  // stored _earlier_, so level 1 is at next_[-1].
  std::atomic<Node*> next_[1];
};

template <class Comparator>
inline SpdbSortedList<Comparator>::Iterator::Iterator(
    const SpdbSortedList* list) {
  SetList(list);
}

template <class Comparator>
inline void SpdbSortedList<Comparator>::Iterator::SetList(
    const SpdbSortedList* list) {
  list_ = list;
  node_ = nullptr;
}

template <class Comparator>
inline void SpdbSortedList<Comparator>::Iterator::SetSeek(const char* key) {
  node_ = reinterpret_cast<Node*>(const_cast<char*>(key)) - 1;
}

template <class Comparator>
inline bool SpdbSortedList<Comparator>::Iterator::Valid() const {
  return node_ != nullptr;
}

template <class Comparator>
inline const char* SpdbSortedList<Comparator>::Iterator::key() const {
  assert(Valid());
  return node_->Key();
}

template <class Comparator>
inline void SpdbSortedList<Comparator>::Iterator::Next() {
  assert(Valid());
  node_ = node_->Next(0);
}

template <class Comparator>
inline void SpdbSortedList<Comparator>::Iterator::Prev() {
  // Instead of using explicit "prev" links, we just search for the
  // last node that falls before key.
  assert(Valid());
  node_ = list_->FindLessThan(node_->Key());
  if (node_ == list_->head_) {
    node_ = nullptr;
  }
}

template <class Comparator>
inline void SpdbSortedList<Comparator>::Iterator::Seek(const char* target) {
  node_ = list_->FindGreaterOrEqual(target);
}

template <class Comparator>
inline void SpdbSortedList<Comparator>::Iterator::SeekForPrev(
    const char* target) {
  Seek(target);
  if (!Valid()) {
    SeekToLast();
  }
  while (Valid() && list_->LessThan(target, key())) {
    Prev();
  }
}

template <class Comparator>
inline void SpdbSortedList<Comparator>::Iterator::RandomSeek() {
  node_ = list_->FindRandomEntry();
}

template <class Comparator>
inline void SpdbSortedList<Comparator>::Iterator::SeekToFirst() {
  node_ = list_->head_->Next(0);
}

template <class Comparator>
inline void SpdbSortedList<Comparator>::Iterator::SeekToLast() {
  node_ = list_->FindLast();
  if (node_ == list_->head_) {
    node_ = nullptr;
  }
}

template <class Comparator>
int SpdbSortedList<Comparator>::RandomHeight() {
  auto rnd = Random::GetTLSInstance();

  // Increase height with probability 1 in kBranching
  int height = 1;
  while (height < kMaxHeight_ && height < kMaxPossibleHeight &&
         rnd->Next() < kScaledInverseBranching_) {
    height++;
  }
  assert(height > 0);
  assert(height <= kMaxHeight_);
  assert(height <= kMaxPossibleHeight);
  return height;
}

template <class Comparator>
bool SpdbSortedList<Comparator>::KeyIsAfterNode(const char* key,
                                                Node* n) const {
  // nullptr n is considered infinite
  assert(n != head_);
  return (n != nullptr) && (compare_(n->Key(), key) < 0);
}

template <class Comparator>
bool SpdbSortedList<Comparator>::KeyIsAfterNode(const DecodedKey& key,
                                                Node* n) const {
  // nullptr n is considered infinite
  assert(n != head_);
  return (n != nullptr) && (compare_(n->Key(), key) < 0);
}

template <class Comparator>
typename SpdbSortedList<Comparator>::Node*
SpdbSortedList<Comparator>::FindGreaterOrEqual(const char* key) const {
  // Note: It looks like we could reduce duplication by implementing
  // this function as FindLessThan(key)->Next(0), but we wouldn't be able
  // to exit early on equality and the result wouldn't even be correct.
  // A concurrent insert might occur after FindLessThan(key) but before
  // we get a chance to call Next(0).
  Node* x = head_;
  int level = GetMaxHeight() - 1;
  Node* last_bigger = nullptr;
  const DecodedKey key_decoded = compare_.decode_key(key);
  while (true) {
    Node* next = x->Next(level);
    if (next != nullptr) {
      PREFETCH(next->Next(level), 0, 1);
    }
    // Make sure the lists are sorted
    assert(x == head_ || next == nullptr || KeyIsAfterNode(next->Key(), x));
    // Make sure we haven't overshot during our search
    assert(x == head_ || KeyIsAfterNode(key_decoded, x));
    int cmp = (next == nullptr || next == last_bigger)
                  ? 1
                  : compare_(next->Key(), key_decoded);
    if (cmp == 0 || (cmp > 0 && level == 0)) {
      return next;
    } else if (cmp < 0) {
      // Keep searching in this list
      x = next;
    } else {
      // Switch to next list, reuse compare_() result
      last_bigger = next;
      level--;
    }
  }
}

template <class Comparator>
typename SpdbSortedList<Comparator>::Node*
SpdbSortedList<Comparator>::FindLessThan(const char* key, Node** prev) const {
  return FindLessThan(key, prev, head_, GetMaxHeight(), 0);
}

template <class Comparator>
typename SpdbSortedList<Comparator>::Node*
SpdbSortedList<Comparator>::FindLessThan(const char* key, Node** prev,
                                         Node* root, int top_level,
                                         int bottom_level) const {
  assert(top_level > bottom_level);
  int level = top_level - 1;
  Node* x = root;
  // KeyIsAfter(key, last_not_after) is definitely false
  Node* last_not_after = nullptr;
  const DecodedKey key_decoded = compare_.decode_key(key);
  while (true) {
    assert(x != nullptr);
    Node* next = x->Next(level);
    if (next != nullptr) {
      PREFETCH(next->Next(level), 0, 1);
    }
    assert(x == head_ || next == nullptr || KeyIsAfterNode(next->Key(), x));
    assert(x == head_ || KeyIsAfterNode(key_decoded, x));
    if (next != last_not_after && KeyIsAfterNode(key_decoded, next)) {
      // Keep searching in this list
      assert(next != nullptr);
      x = next;
    } else {
      if (prev != nullptr) {
        prev[level] = x;
      }
      if (level == bottom_level) {
        return x;
      } else {
        // Switch to next list, reuse KeyIsAfterNode() result
        last_not_after = next;
        level--;
      }
    }
  }
}

template <class Comparator>
typename SpdbSortedList<Comparator>::Node*
SpdbSortedList<Comparator>::FindLast() const {
  Node* x = head_;
  int level = GetMaxHeight() - 1;
  while (true) {
    Node* next = x->Next(level);
    if (next == nullptr) {
      if (level == 0) {
        return x;
      } else {
        // Switch to next list
        level--;
      }
    } else {
      x = next;
    }
  }
}

template <class Comparator>
typename SpdbSortedList<Comparator>::Node*
SpdbSortedList<Comparator>::FindRandomEntry() const {
  // TODO(bjlemaire): consider adding PREFETCH calls.
  Node *x = head_, *scan_node = nullptr, *limit_node = nullptr;

  // We start at the max level.
  // FOr each level, we look at all the nodes at the level, and
  // we randomly pick one of them. Then decrement the level
  // and reiterate the process.
  // eg: assume GetMaxHeight()=5, and there are #100 elements (nodes).
  // level 4 nodes: lvl_nodes={#1, #15, #67, #84}. Randomly pick #15.
  // We will consider all the nodes between #15 (inclusive) and #67
  // (exclusive). #67 is called 'limit_node' here.
  // level 3 nodes: lvl_nodes={#15, #21, #45, #51}. Randomly choose
  // #51. #67 remains 'limit_node'.
  // [...]
  // level 0 nodes: lvl_nodes={#56,#57,#58,#59}. Randomly pick $57.
  // Return Node #57.
  std::vector<Node*> lvl_nodes;
  Random* rnd = Random::GetTLSInstance();
  int level = GetMaxHeight() - 1;

  while (level >= 0) {
    lvl_nodes.clear();
    scan_node = x;
    while (scan_node != limit_node) {
      lvl_nodes.push_back(scan_node);
      scan_node = scan_node->Next(level);
    }
    uint32_t rnd_idx = rnd->Next() % lvl_nodes.size();
    x = lvl_nodes[rnd_idx];
    if (rnd_idx + 1 < lvl_nodes.size()) {
      limit_node = lvl_nodes[rnd_idx + 1];
    }
    level--;
  }
  // There is a special case where x could still be the head_
  // (note that the head_ contains no key).
  return x == head_ ? head_->Next(0) : x;
}

template <class Comparator>
uint64_t SpdbSortedList<Comparator>::EstimateCount(const char* key) const {
  uint64_t count = 0;

  Node* x = head_;
  int level = GetMaxHeight() - 1;
  const DecodedKey key_decoded = compare_.decode_key(key);
  while (true) {
    assert(x == head_ || compare_(x->Key(), key_decoded) < 0);
    Node* next = x->Next(level);
    if (next != nullptr) {
      PREFETCH(next->Next(level), 0, 1);
    }
    if (next == nullptr || compare_(next->Key(), key_decoded) >= 0) {
      if (level == 0) {
        return count;
      } else {
        // Switch to next list
        count *= kBranching_;
        level--;
      }
    } else {
      x = next;
      count++;
    }
  }
}

template <class Comparator>
SpdbSortedList<Comparator>::SpdbSortedList(const Comparator cmp,
                                           Allocator* allocator,
                                           int32_t max_height,
                                           int32_t branching_factor)
    : kMaxHeight_(static_cast<uint16_t>(max_height)),
      kBranching_(static_cast<uint16_t>(branching_factor)),
      kScaledInverseBranching_((Random::kMaxNext + 1) / kBranching_),
      allocator_(allocator),
      compare_(cmp),
      head_(AllocateNode(0, max_height)),
      max_height_(1),
      seq_splice_(AllocateSplice()) {
  assert(max_height > 0 && kMaxHeight_ == static_cast<uint32_t>(max_height));
  assert(branching_factor > 1 &&
         kBranching_ == static_cast<uint32_t>(branching_factor));
  assert(kScaledInverseBranching_ > 0);

  for (int i = 0; i < kMaxHeight_; ++i) {
    head_->SetNext(i, nullptr);
  }
}

template <class Comparator>
char* SpdbSortedList<Comparator>::AllocateSpdbItem(size_t key_size,
                                                   size_t header_size,
                                                   char** spdb_sorted_key) {
  int height = RandomHeight();
  auto prefix = header_size + sizeof(std::atomic<Node*>) * (height - 1);

  // prefix is space for the height - 1 pointers that we store before
  // the Node instance (next_[-(height - 1) .. -1]).  Node starts at
  // raw + prefix, and holds the bottom-mode (level 0) spdbsortedlist pointer
  // next_[0].  key_size is the bytes for the key, which comes just after
  // the Node.
  char* raw = allocator_->AllocateAligned(prefix + sizeof(Node) + key_size);
  Node* x = reinterpret_cast<Node*>(raw + prefix);
  x->StashHeight(height);
  *spdb_sorted_key = const_cast<char*>(x->Key());
  return raw;
}

template <class Comparator>
typename SpdbSortedList<Comparator>::Node*
SpdbSortedList<Comparator>::AllocateNode(size_t key_size, int height) {
  auto prefix = sizeof(std::atomic<Node*>) * (height - 1);

  // prefix is space for the height - 1 pointers that we store before
  // the Node instance (next_[-(height - 1) .. -1]).  Node starts at
  // raw + prefix, and holds the bottom-mode (level 0) spdbsortedlist pointer
  // next_[0].  key_size is the bytes for the key, which comes just after
  // the Node.
  char* raw = allocator_->AllocateAligned(prefix + sizeof(Node) + key_size);
  Node* x = reinterpret_cast<Node*>(raw + prefix);

  // Once we've linked the node into the spdbsortedlist we don't actually need
  // to know its height, because we can implicitly use the fact that we
  // traversed into a node at level h to known that h is a valid level
  // for that node.  We need to convey the height to the Insert step,
  // however, so that it can perform the proper links.  Since we're not
  // using the pointers at the moment, StashHeight temporarily borrow
  // storage from next_[0] for that purpose.
  x->StashHeight(height);
  return x;
}

template <class Comparator>
typename SpdbSortedList<Comparator>::Splice*
SpdbSortedList<Comparator>::AllocateSplice() {
  // size of prev_ and next_
  size_t array_size = sizeof(Node*) * (kMaxHeight_ + 1);
  char* raw = allocator_->AllocateAligned(sizeof(Splice) + array_size * 2);
  Splice* splice = reinterpret_cast<Splice*>(raw);
  splice->height_ = 0;
  splice->prev_ = reinterpret_cast<Node**>(raw + sizeof(Splice));
  splice->next_ = reinterpret_cast<Node**>(raw + sizeof(Splice) + array_size);
  return splice;
}

template <class Comparator>
typename SpdbSortedList<Comparator>::Splice*
SpdbSortedList<Comparator>::AllocateSpliceOnHeap() {
  size_t array_size = sizeof(Node*) * (kMaxHeight_ + 1);
  char* raw = new char[sizeof(Splice) + array_size * 2];
  Splice* splice = reinterpret_cast<Splice*>(raw);
  splice->height_ = 0;
  splice->prev_ = reinterpret_cast<Node**>(raw + sizeof(Splice));
  splice->next_ = reinterpret_cast<Node**>(raw + sizeof(Splice) + array_size);
  return splice;
}

template <class Comparator>
bool SpdbSortedList<Comparator>::Insert(const char* key, bool concurrently) {
  if (concurrently) {
    Node* prev[kMaxPossibleHeight];
    Node* next[kMaxPossibleHeight];
    Splice splice;
    splice.prev_ = prev;
    splice.next_ = next;    
  }
  return Insert<concurrently>(key, seq_splice_, false);
}

template <class Comparator>
template <bool prefetch_before>
void SpdbSortedList<Comparator>::FindSpliceForLevel(const DecodedKey& key,
                                                    Node* before, Node* after,
                                                    int level, Node** out_prev,
                                                    Node** out_next) {
  while (true) {
    Node* next = before->Next(level);
    if (next != nullptr) {
      PREFETCH(next->Next(level), 0, 1);
    }
    if (prefetch_before == true) {
      if (next != nullptr && level > 0) {
        PREFETCH(next->Next(level - 1), 0, 1);
      }
    }
    assert(before == head_ || next == nullptr ||
           KeyIsAfterNode(next->Key(), before));
    assert(before == head_ || KeyIsAfterNode(key, before));
    if (next == after || !KeyIsAfterNode(key, next)) {
      // found it
      *out_prev = before;
      *out_next = next;
      return;
    }
    before = next;
  }
}

template <class Comparator>
void SpdbSortedList<Comparator>::RecomputeSpliceLevels(const DecodedKey& key,
                                                       Splice* splice,
                                                       int recompute_level) {
  assert(recompute_level > 0);
  assert(recompute_level <= splice->height_);
  for (int i = recompute_level - 1; i >= 0; --i) {
    FindSpliceForLevel<true>(key, splice->prev_[i + 1], splice->next_[i + 1], i,
                             &splice->prev_[i], &splice->next_[i]);
  }
}

template <class Comparator>
template <bool UseCAS>
bool SpdbSortedList<Comparator>::Insert(const char* key, Splice* splice,
                                        bool allow_partial_splice_fix) {
  Node* x = reinterpret_cast<Node*>(const_cast<char*>(key)) - 1;
  const DecodedKey key_decoded = compare_.decode_key(key);
  int height = x->UnstashHeight();
  assert(height >= 1 && height <= kMaxHeight_);

  int max_height = max_height_.load(std::memory_order_relaxed);
  while (height > max_height) {
    if (max_height_.compare_exchange_weak(max_height, height)) {
      // successfully updated it
      max_height = height;
      break;
    }
    // else retry, possibly exiting the loop because somebody else
    // increased it
  }
  assert(max_height <= kMaxPossibleHeight);

  int recompute_height = 0;
  if (splice->height_ < max_height) {
    // Either splice has never been used or max_height has grown since
    // last use.  We could potentially fix it in the latter case, but
    // that is tricky.
    splice->prev_[max_height] = head_;
    splice->next_[max_height] = nullptr;
    splice->height_ = max_height;
    recompute_height = max_height;
  } else {
    // Splice is a valid proper-height splice that brackets some
    // key, but does it bracket this one?  We need to validate it and
    // recompute a portion of the splice (levels 0..recompute_height-1)
    // that is a superset of all levels that don't bracket the new key.
    // Several choices are reasonable, because we have to balance the work
    // saved against the extra comparisons required to validate the Splice.
    //
    // One strategy is just to recompute all of orig_splice_height if the
    // bottom level isn't bracketing.  This pessimistically assumes that
    // we will either get a perfect Splice hit (increasing sequential
    // inserts) or have no locality.
    //
    // Another strategy is to walk up the Splice's levels until we find
    // a level that brackets the key.  This strategy lets the Splice
    // hint help for other cases: it turns insertion from O(log N) into
    // O(log D), where D is the number of nodes in between the key that
    // produced the Splice and the current insert (insertion is aided
    // whether the new key is before or after the splice).  If you have
    // a way of using a prefix of the key to map directly to the closest
    // Splice out of O(sqrt(N)) Splices and we make it so that splices
    // can also be used as hints during read, then we end up with Oshman's
    // and Shavit's SkipTrie, which has O(log log N) lookup and insertion
    // (compare to O(log N) for spdbsortedlist).
    //
    // We control the pessimistic strategy with allow_partial_splice_fix.
    // A good strategy is probably to be pessimistic for seq_splice_,
    // optimistic if the caller actually went to the work of providing
    // a Splice.
    while (recompute_height < max_height) {
      if (splice->prev_[recompute_height]->Next(recompute_height) !=
          splice->next_[recompute_height]) {
        // splice isn't tight at this level, there must have been some inserts
        // to this
        // location that didn't update the splice.  We might only be a little
        // stale, but if
        // the splice is very stale it would be O(N) to fix it.  We haven't used
        // up any of
        // our budget of comparisons, so always move up even if we are
        // pessimistic about
        // our chances of success.
        ++recompute_height;
      } else if (splice->prev_[recompute_height] != head_ &&
                 !KeyIsAfterNode(key_decoded,
                                 splice->prev_[recompute_height])) {
        // key is from before splice
        if (allow_partial_splice_fix) {
          // skip all levels with the same node without more comparisons
          Node* bad = splice->prev_[recompute_height];
          while (splice->prev_[recompute_height] == bad) {
            ++recompute_height;
          }
        } else {
          // we're pessimistic, recompute everything
          recompute_height = max_height;
        }
      } else if (KeyIsAfterNode(key_decoded, splice->next_[recompute_height])) {
        // key is from after splice
        if (allow_partial_splice_fix) {
          Node* bad = splice->next_[recompute_height];
          while (splice->next_[recompute_height] == bad) {
            ++recompute_height;
          }
        } else {
          recompute_height = max_height;
        }
      } else {
        // this level brackets the key, we won!
        break;
      }
    }
  }
  assert(recompute_height <= max_height);
  if (recompute_height > 0) {
    RecomputeSpliceLevels(key_decoded, splice, recompute_height);
  }

  bool splice_is_valid = true;
  if (UseCAS) {
    for (int i = 0; i < height; ++i) {
      while (true) {
        // Checking for duplicate keys on the level 0 is sufficient
        if (UNLIKELY(i == 0 && splice->next_[i] != nullptr &&
                     compare_(x->Key(), splice->next_[i]->Key()) >= 0)) {
          // duplicate key
          return false;
        }
        if (UNLIKELY(i == 0 && splice->prev_[i] != head_ &&
                     compare_(splice->prev_[i]->Key(), x->Key()) >= 0)) {
          // duplicate key
          return false;
        }
        assert(splice->next_[i] == nullptr ||
               compare_(x->Key(), splice->next_[i]->Key()) < 0);
        assert(splice->prev_[i] == head_ ||
               compare_(splice->prev_[i]->Key(), x->Key()) < 0);
        x->NoBarrier_SetNext(i, splice->next_[i]);
        if (splice->prev_[i]->CASNext(i, splice->next_[i], x)) {
          // success
          break;
        }
        // CAS failed, we need to recompute prev and next. It is unlikely
        // to be helpful to try to use a different level as we redo the
        // search, because it should be unlikely that lots of nodes have
        // been inserted between prev[i] and next[i]. No point in using
        // next[i] as the after hint, because we know it is stale.
        FindSpliceForLevel<false>(key_decoded, splice->prev_[i], nullptr, i,
                                  &splice->prev_[i], &splice->next_[i]);

        // Since we've narrowed the bracket for level i, we might have
        // violated the Splice constraint between i and i-1.  Make sure
        // we recompute the whole thing next time.
        if (i > 0) {
          splice_is_valid = false;
        }
      }
    }
  } else {
    for (int i = 0; i < height; ++i) {
      if (i >= recompute_height &&
          splice->prev_[i]->Next(i) != splice->next_[i]) {
        FindSpliceForLevel<false>(key_decoded, splice->prev_[i], nullptr, i,
                                  &splice->prev_[i], &splice->next_[i]);
      }
      // Checking for duplicate keys on the level 0 is sufficient
      if (UNLIKELY(i == 0 && splice->next_[i] != nullptr &&
                   compare_(x->Key(), splice->next_[i]->Key()) >= 0)) {
        // duplicate key
        return false;
      }
      if (UNLIKELY(i == 0 && splice->prev_[i] != head_ &&
                   compare_(splice->prev_[i]->Key(), x->Key()) >= 0)) {
        // duplicate key
        return false;
      }
      assert(splice->next_[i] == nullptr ||
             compare_(x->Key(), splice->next_[i]->Key()) < 0);
      assert(splice->prev_[i] == head_ ||
             compare_(splice->prev_[i]->Key(), x->Key()) < 0);
      assert(splice->prev_[i]->Next(i) == splice->next_[i]);
      x->NoBarrier_SetNext(i, splice->next_[i]);
      splice->prev_[i]->SetNext(i, x);
    }
  }
  if (splice_is_valid) {
    for (int i = 0; i < height; ++i) {
      splice->prev_[i] = x;
    }
    assert(splice->prev_[splice->height_] == head_);
    assert(splice->next_[splice->height_] == nullptr);
    for (int i = 0; i < splice->height_; ++i) {
      assert(splice->next_[i] == nullptr ||
             compare_(key, splice->next_[i]->Key()) < 0);
      assert(splice->prev_[i] == head_ ||
             compare_(splice->prev_[i]->Key(), key) <= 0);
      assert(splice->prev_[i + 1] == splice->prev_[i] ||
             splice->prev_[i + 1] == head_ ||
             compare_(splice->prev_[i + 1]->Key(), splice->prev_[i]->Key()) <
                 0);
      assert(splice->next_[i + 1] == splice->next_[i] ||
             splice->next_[i + 1] == nullptr ||
             compare_(splice->next_[i]->Key(), splice->next_[i + 1]->Key()) <
                 0);
    }
  } else {
    splice->height_ = 0;
  }
  return true;
}



}  // namespace ROCKSDB_NAMESPACE