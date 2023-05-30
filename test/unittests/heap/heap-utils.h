// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_UNITTESTS_HEAP_HEAP_UTILS_H_
#define V8_UNITTESTS_HEAP_HEAP_UTILS_H_

#include "src/base/macros.h"
#include "src/common/globals.h"
#include "src/heap/heap.h"
#include "test/unittests/test-utils.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace v8 {
namespace internal {

class HeapInternalsBase {
 protected:
  void SimulateIncrementalMarking(Heap* heap, bool force_completion);
  void SimulateFullSpace(
      v8::internal::NewSpace* space,
      std::vector<Handle<FixedArray>>* out_handles = nullptr);
  void SimulateFullSpace(v8::internal::PagedSpace* space);
  void FillCurrentPage(v8::internal::NewSpace* space,
                       std::vector<Handle<FixedArray>>* out_handles = nullptr);
};

inline void CollectGarbage(AllocationSpace space, Isolate* isolate) {
  isolate->heap()->CollectGarbage(space, GarbageCollectionReason::kTesting);
}

inline void CollectAllAvailableGarbage(Isolate* isolate) {
  isolate->heap()->CollectAllAvailableGarbage(
      GarbageCollectionReason::kTesting);
}

inline void PreciseCollectAllGarbage(Isolate* isolate) {
  isolate->heap()->PreciseCollectAllGarbage(GCFlag::kNoFlags,
                                            GarbageCollectionReason::kTesting);
}

template <typename TMixin>
class WithHeapInternals : public TMixin, HeapInternalsBase {
 public:
  WithHeapInternals() = default;
  WithHeapInternals(const WithHeapInternals&) = delete;
  WithHeapInternals& operator=(const WithHeapInternals&) = delete;

  void CollectGarbage(AllocationSpace space) {
    heap()->CollectGarbage(space, GarbageCollectionReason::kTesting);
  }

  void CollectAllGarbage() {
    heap()->CollectAllGarbage(GCFlag::kNoFlags,
                              GarbageCollectionReason::kTesting);
  }

  void CollectAllAvailableGarbage() {
    heap()->CollectAllAvailableGarbage(GarbageCollectionReason::kTesting);
  }

  void PreciseCollectAllGarbage() {
    heap()->PreciseCollectAllGarbage(GCFlag::kNoFlags,
                                     GarbageCollectionReason::kTesting);
  }

  Heap* heap() const { return this->i_isolate()->heap(); }

  void SimulateIncrementalMarking(bool force_completion = true) {
    return HeapInternalsBase::SimulateIncrementalMarking(heap(),
                                                         force_completion);
  }

  void SimulateFullSpace(
      v8::internal::NewSpace* space,
      std::vector<Handle<FixedArray>>* out_handles = nullptr) {
    return HeapInternalsBase::SimulateFullSpace(space, out_handles);
  }
  void SimulateFullSpace(v8::internal::PagedSpace* space) {
    return HeapInternalsBase::SimulateFullSpace(space);
  }

  void GrowNewSpace() {
    IsolateSafepointScope scope(heap());
    NewSpace* new_space = heap()->new_space();
    if (new_space->TotalCapacity() < new_space->MaximumCapacity()) {
      new_space->Grow();
    }
    CHECK(new_space->EnsureCurrentCapacity());
  }

  void SealCurrentObjects() {
    // If you see this check failing, disable the flag at the start of your
    // test: v8_flags.stress_concurrent_allocation = false; Background thread
    // allocating concurrently interferes with this function.
    CHECK(!v8_flags.stress_concurrent_allocation);
    CollectGarbage(OLD_SPACE);
    CollectGarbage(OLD_SPACE);
    heap()->EnsureSweepingCompleted(
        Heap::SweepingForcedFinalizationMode::kV8Only);
    heap()->old_space()->FreeLinearAllocationArea();
    for (Page* page : *heap()->old_space()) {
      page->MarkNeverAllocateForTesting();
    }
  }

  void GcAndSweep(AllocationSpace space) {
    heap()->CollectGarbage(space, GarbageCollectionReason::kTesting);
    if (heap()->sweeping_in_progress()) {
      IsolateSafepointScope scope(heap());
      heap()->EnsureSweepingCompleted(
          Heap::SweepingForcedFinalizationMode::kV8Only);
    }
  }

  void EmptyNewSpaceUsingGC() { CollectGarbage(OLD_SPACE); }
};

using TestWithHeapInternals =                  //
    WithHeapInternals<                         //
        WithInternalIsolateMixin<              //
            WithIsolateScopeMixin<             //
                WithIsolateMixin<              //
                    WithDefaultPlatformMixin<  //
                        ::testing::Test>>>>>;

using TestWithHeapInternalsAndContext =  //
    WithContextMixin<                    //
        TestWithHeapInternals>;

template <typename GlobalOrPersistent>
bool InYoungGeneration(v8::Isolate* isolate, const GlobalOrPersistent& global) {
  CHECK(!v8_flags.single_generation);
  v8::HandleScope scope(isolate);
  auto tmp = global.Get(isolate);
  return Heap::InYoungGeneration(*v8::Utils::OpenHandle(*tmp));
}

bool IsNewObjectInCorrectGeneration(HeapObject object);

template <typename GlobalOrPersistent>
bool IsNewObjectInCorrectGeneration(v8::Isolate* isolate,
                                    const GlobalOrPersistent& global) {
  v8::HandleScope scope(isolate);
  auto tmp = global.Get(isolate);
  return IsNewObjectInCorrectGeneration(*v8::Utils::OpenHandle(*tmp));
}

void FinalizeGCIfRunning(Isolate* isolate);

}  // namespace internal
}  // namespace v8

#endif  // V8_UNITTESTS_HEAP_HEAP_UTILS_H_
