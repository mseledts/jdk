/*
 * Copyright (c) 2015, 2022, Red Hat, Inc. All rights reserved.
 * Copyright Amazon.com Inc. or its affiliates. All Rights Reserved.
 * Copyright (c) 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHMARK_INLINE_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHMARK_INLINE_HPP

#include "gc/shenandoah/shenandoahMark.hpp"

#include "gc/shared/continuationGCSupport.inline.hpp"
#include "gc/shenandoah/shenandoahAgeCensus.hpp"
#include "gc/shenandoah/shenandoahAsserts.hpp"
#include "gc/shenandoah/shenandoahBarrierSet.inline.hpp"
#include "gc/shenandoah/shenandoahClosures.inline.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahMarkingContext.inline.hpp"
#include "gc/shenandoah/shenandoahOldGeneration.hpp"
#include "gc/shenandoah/shenandoahScanRemembered.inline.hpp"
#include "gc/shenandoah/shenandoahStringDedup.inline.hpp"
#include "gc/shenandoah/shenandoahTaskqueue.inline.hpp"
#include "gc/shenandoah/shenandoahUtils.hpp"
#include "memory/iterator.inline.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/prefetch.inline.hpp"
#include "utilities/devirtualizer.inline.hpp"
#include "utilities/powerOfTwo.hpp"

template <StringDedupMode STRING_DEDUP>
void ShenandoahMark::dedup_string(oop obj, StringDedup::Requests* const req) {
  if (STRING_DEDUP == ENQUEUE_DEDUP) {
    if (ShenandoahStringDedup::is_candidate(obj)) {
      req->add(obj);
    }
  } else if (STRING_DEDUP == ALWAYS_DEDUP) {
    if (ShenandoahStringDedup::is_string_candidate(obj) &&
        !ShenandoahStringDedup::dedup_requested(obj)) {
        req->add(obj);
    }
  }
}

template <class T, ShenandoahGenerationType GENERATION, StringDedupMode STRING_DEDUP>
void ShenandoahMark::do_task(ShenandoahObjToScanQueue* q, T* cl, ShenandoahLiveData* live_data, StringDedup::Requests* const req, ShenandoahMarkTask* task, uint worker_id) {
  oop obj = task->obj();

  shenandoah_assert_not_forwarded(nullptr, obj);
  shenandoah_assert_marked(nullptr, obj);
  shenandoah_assert_not_in_cset_except(nullptr, obj, ShenandoahHeap::heap()->cancelled_gc());

  // Are we in weak subgraph scan?
  bool weak = task->is_weak();
  cl->set_weak(weak);

  if (task->is_not_chunked()) {
    if (obj->is_instance()) {
      // Case 1: Normal oop, process as usual.
      if (ContinuationGCSupport::relativize_stack_chunk(obj)) {
          // Loom doesn't support mixing of weak marking and strong marking of
          // stack chunks.
          cl->set_weak(false);
      }

      obj->oop_iterate(cl);
      dedup_string<STRING_DEDUP>(obj, req);
    } else if (obj->is_objArray()) {
      // Case 2: Object array instance and no chunk is set. Must be the first
      // time we visit it, start the chunked processing.
      do_chunked_array_start<T>(q, cl, obj, weak);
    } else {
      // Case 3: Primitive array. Do nothing, no oops there. We use the same
      // performance tweak TypeArrayKlass::oop_oop_iterate_impl is using:
      // We skip iterating over the klass pointer since we know that
      // Universe::TypeArrayKlass never moves.
      assert (obj->is_typeArray(), "should be type array");
    }
    // Count liveness the last: push the outstanding work to the queues first
    // Avoid double-counting objects that are visited twice due to upgrade
    // from final- to strong mark.
    if (task->count_liveness()) {
      count_liveness<GENERATION>(live_data, obj, worker_id);
    }
  } else {
    // Case 4: Array chunk, has sensible chunk id. Process it.
    do_chunked_array<T>(q, cl, obj, task->chunk(), task->pow(), weak);
  }
}

template <ShenandoahGenerationType GENERATION>
inline void ShenandoahMark::count_liveness(ShenandoahLiveData* live_data, oop obj, uint worker_id) {
  const ShenandoahHeap* const heap = ShenandoahHeap::heap();
  const size_t region_idx = heap->heap_region_index_containing(obj);
  ShenandoahHeapRegion* const region = heap->get_region(region_idx);
  const size_t size = obj->size();

  // Age census for objects in the young generation
  if (GENERATION == YOUNG || (GENERATION == GLOBAL && region->is_young())) {
    assert(heap->mode()->is_generational(), "Only if generational");
    if (ShenandoahGenerationalAdaptiveTenuring && !ShenandoahGenerationalCensusAtEvac) {
      assert(region->is_young(), "Only for young objects");
      uint age = ShenandoahHeap::get_object_age(obj);
      ShenandoahAgeCensus* const census = ShenandoahGenerationalHeap::heap()->age_census();
      CENSUS_NOISE(census->add(age, region->age(), region->youth(), size, worker_id);)
      NO_CENSUS_NOISE(census->add(age, region->age(), size, worker_id);)
    }
  }

  if (!region->is_humongous_start()) {
    assert(!region->is_humongous(), "Cannot have continuations here");
    assert(region->is_affiliated(), "Do not count live data within Free Regular Region %zu", region_idx);
    ShenandoahLiveData cur = live_data[region_idx];
    size_t new_val = size + cur;
    if (new_val >= SHENANDOAH_LIVEDATA_MAX) {
      // overflow, flush to region data
      region->increase_live_data_gc_words(new_val);
      live_data[region_idx] = 0;
    } else {
      // still good, remember in locals
      live_data[region_idx] = (ShenandoahLiveData) new_val;
    }
  } else {
    shenandoah_assert_in_correct_region(nullptr, obj);
    size_t num_regions = ShenandoahHeapRegion::required_regions(size * HeapWordSize);

    assert(region->is_affiliated(), "Do not count live data within FREE Humongous Start Region %zu", region_idx);
    for (size_t i = region_idx; i < region_idx + num_regions; i++) {
      ShenandoahHeapRegion* chain_reg = heap->get_region(i);
      assert(chain_reg->is_humongous(), "Expecting a humongous region");
      assert(chain_reg->is_affiliated(), "Do not count live data within FREE Humongous Continuation Region %zu", i);
      chain_reg->increase_live_data_gc_words(chain_reg->used() >> LogHeapWordSize);
    }
  }
}

template <class T>
inline void ShenandoahMark::do_chunked_array_start(ShenandoahObjToScanQueue* q, T* cl, oop obj, bool weak) {
  assert(obj->is_objArray(), "expect object array");
  objArrayOop array = objArrayOop(obj);
  int len = array->length();

  // Mark objArray klass metadata
  if (Devirtualizer::do_metadata(cl)) {
    Devirtualizer::do_klass(cl, array->klass());
  }

  if (len <= (int) ObjArrayMarkingStride*2) {
    // A few slices only, process directly
    array->oop_iterate_range(cl, 0, len);
  } else {
    int bits = log2i_graceful(len);
    // Compensate for non-power-of-two arrays, cover the array in excess:
    if (len != (1 << bits)) bits++;

    // Only allow full chunks on the queue. This frees do_chunked_array() from checking from/to
    // boundaries against array->length(), touching the array header on every chunk.
    //
    // To do this, we cut the prefix in full-sized chunks, and submit them on the queue.
    // If the array is not divided in chunk sizes, then there would be an irregular tail,
    // which we will process separately.

    int last_idx = 0;

    int chunk = 1;
    int pow = bits;

    // Handle overflow
    if (pow >= 31) {
      assert (pow == 31, "sanity");
      pow--;
      chunk = 2;
      last_idx = (1 << pow);
      bool pushed = q->push(ShenandoahMarkTask(array, true, weak, 1, pow));
      assert(pushed, "overflow queue should always succeed pushing");
    }

    // Split out tasks, as suggested in ShenandoahMarkTask docs. Record the last
    // successful right boundary to figure out the irregular tail.
    while ((1 << pow) > (int)ObjArrayMarkingStride &&
           (chunk*2 < ShenandoahMarkTask::chunk_size())) {
      pow--;
      int left_chunk = chunk*2 - 1;
      int right_chunk = chunk*2;
      int left_chunk_end = left_chunk * (1 << pow);
      if (left_chunk_end < len) {
        bool pushed = q->push(ShenandoahMarkTask(array, true, weak, left_chunk, pow));
        assert(pushed, "overflow queue should always succeed pushing");
        chunk = right_chunk;
        last_idx = left_chunk_end;
      } else {
        chunk = left_chunk;
      }
    }

    // Process the irregular tail, if present
    int from = last_idx;
    if (from < len) {
      array->oop_iterate_range(cl, from, len);
    }
  }
}

template <class T>
inline void ShenandoahMark::do_chunked_array(ShenandoahObjToScanQueue* q, T* cl, oop obj, int chunk, int pow, bool weak) {
  assert(obj->is_objArray(), "expect object array");
  objArrayOop array = objArrayOop(obj);

  assert (ObjArrayMarkingStride > 0, "sanity");

  // Split out tasks, as suggested in ShenandoahMarkTask docs. Avoid pushing tasks that
  // are known to start beyond the array.
  while ((1 << pow) > (int)ObjArrayMarkingStride && (chunk*2 < ShenandoahMarkTask::chunk_size())) {
    pow--;
    chunk *= 2;
    bool pushed = q->push(ShenandoahMarkTask(array, true, weak, chunk - 1, pow));
    assert(pushed, "overflow queue should always succeed pushing");
  }

  int chunk_size = 1 << pow;

  int from = (chunk - 1) * chunk_size;
  int to = chunk * chunk_size;

#ifdef ASSERT
  int len = array->length();
  assert (0 <= from && from < len, "from is sane: %d/%d", from, len);
  assert (0 < to && to <= len, "to is sane: %d/%d", to, len);
#endif

  array->oop_iterate_range(cl, from, to);
}

template <ShenandoahGenerationType GENERATION>
class ShenandoahSATBBufferClosure : public SATBBufferClosure {
private:
  ShenandoahObjToScanQueue* _queue;
  ShenandoahObjToScanQueue* _old_queue;
  ShenandoahHeap* _heap;
  ShenandoahMarkingContext* const _mark_context;
public:
  ShenandoahSATBBufferClosure(ShenandoahObjToScanQueue* q, ShenandoahObjToScanQueue* old_q) :
    _queue(q),
    _old_queue(old_q),
    _heap(ShenandoahHeap::heap()),
    _mark_context(_heap->marking_context())
  {
  }

  void do_buffer(void **buffer, size_t size) {
    assert(size == 0 || !_heap->has_forwarded_objects() || _heap->is_concurrent_old_mark_in_progress(), "Forwarded objects are not expected here");
    for (size_t i = 0; i < size; ++i) {
      oop *p = (oop *) &buffer[i];
      ShenandoahMark::mark_through_ref<oop, GENERATION>(p, _queue, _old_queue, _mark_context, false);
    }
  }
};

template<ShenandoahGenerationType GENERATION>
bool ShenandoahMark::in_generation(ShenandoahHeap* const heap, oop obj) {
  // Each in-line expansion of in_generation() resolves GENERATION at compile time.
  if (GENERATION == YOUNG) {
    return heap->is_in_young(obj);
  }

  if (GENERATION == OLD) {
    return heap->is_in_old(obj);
  }

  assert((GENERATION == GLOBAL || GENERATION == NON_GEN), "Unexpected generation type");
  assert(heap->is_in(obj), "Object must be in heap");
  return true;
}

template<class T, ShenandoahGenerationType GENERATION>
inline void ShenandoahMark::mark_through_ref(T *p, ShenandoahObjToScanQueue* q, ShenandoahObjToScanQueue* old_q, ShenandoahMarkingContext* const mark_context, bool weak) {
  // Note: This is a very hot code path, so the code should be conditional on GENERATION template
  // parameter where possible, in order to generate the most efficient code.

  T o = RawAccess<>::oop_load(p);
  if (!CompressedOops::is_null(o)) {
    oop obj = CompressedOops::decode_not_null(o);

    ShenandoahGenerationalHeap* heap = ShenandoahGenerationalHeap::heap();
    shenandoah_assert_not_forwarded(p, obj);
    shenandoah_assert_not_in_cset_except(p, obj, heap->cancelled_gc());
    if (in_generation<GENERATION>(heap, obj)) {
      mark_ref(q, mark_context, weak, obj);
      shenandoah_assert_marked(p, obj);
      if (GENERATION == YOUNG && heap->is_in_old(p)) {
        // Mark card as dirty because remembered set scanning still finds interesting pointer.
        heap->old_generation()->mark_card_as_dirty((HeapWord*)p);
      } else if (GENERATION == GLOBAL && heap->is_in_old(p) && heap->is_in_young(obj)) {
        // Mark card as dirty because GLOBAL marking finds interesting pointer.
        heap->old_generation()->mark_card_as_dirty((HeapWord*)p);
      }
    } else if (old_q != nullptr) {
      // Young mark, bootstrapping old_q or concurrent with old_q marking.
      mark_ref(old_q, mark_context, weak, obj);
      shenandoah_assert_marked(p, obj);
    } else if (GENERATION == OLD) {
      // Old mark, found a young pointer.
      if (heap->is_in(p)) {
        assert(heap->is_in_young(obj), "Expected young object.");
        heap->old_generation()->mark_card_as_dirty(p);
      }
    }
  }
}

template<>
inline void ShenandoahMark::mark_through_ref<oop, ShenandoahGenerationType::NON_GEN>(oop *p, ShenandoahObjToScanQueue* q, ShenandoahObjToScanQueue* old_q, ShenandoahMarkingContext* const mark_context, bool weak) {
  mark_non_generational_ref(p, q, mark_context, weak);
}

template<>
inline void ShenandoahMark::mark_through_ref<narrowOop, ShenandoahGenerationType::NON_GEN>(narrowOop *p, ShenandoahObjToScanQueue* q, ShenandoahObjToScanQueue* old_q, ShenandoahMarkingContext* const mark_context, bool weak) {
  mark_non_generational_ref(p, q, mark_context, weak);
}

template<class T>
inline void ShenandoahMark::mark_non_generational_ref(T* p, ShenandoahObjToScanQueue* q,
                                                      ShenandoahMarkingContext* const mark_context, bool weak) {
  oop o = RawAccess<>::oop_load(p);
  if (!CompressedOops::is_null(o)) {
    oop obj = CompressedOops::decode_not_null(o);

    shenandoah_assert_not_forwarded(p, obj);
    shenandoah_assert_not_in_cset_except(p, obj, ShenandoahHeap::heap()->cancelled_gc());

    mark_ref(q, mark_context, weak, obj);

    shenandoah_assert_marked(p, obj);
  }
}

inline void ShenandoahMark::mark_ref(ShenandoahObjToScanQueue* q,
                              ShenandoahMarkingContext* const mark_context,
                              bool weak, oop obj) {
  bool skip_live = false;
  bool marked;
  if (weak) {
    marked = mark_context->mark_weak(obj);
  } else {
    marked = mark_context->mark_strong(obj, /* was_upgraded = */ skip_live);
  }
  if (marked) {
    bool pushed = q->push(ShenandoahMarkTask(obj, skip_live, weak));
    assert(pushed, "overflow queue should always succeed pushing");
  }
}

ShenandoahObjToScanQueueSet* ShenandoahMark::task_queues() const {
  return _task_queues;
}

ShenandoahObjToScanQueue* ShenandoahMark::get_queue(uint index) const {
  return _task_queues->queue(index);
}

ShenandoahObjToScanQueue* ShenandoahMark::get_old_queue(uint index) const {
  if (_old_gen_task_queues != nullptr) {
    return _old_gen_task_queues->queue(index);
  }
  return nullptr;
}

#endif // SHARE_GC_SHENANDOAH_SHENANDOAHMARK_INLINE_HPP
