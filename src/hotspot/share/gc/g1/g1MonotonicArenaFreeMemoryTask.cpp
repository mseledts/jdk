/*
 * Copyright (c) 2021, 2025, Oracle and/or its affiliates. All rights reserved.
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

#include "ci/ciUtilities.hpp"
#include "gc/g1/g1CardSetMemory.inline.hpp"
#include "gc/g1/g1CollectedHeap.hpp"
#include "gc/g1/g1HeapRegionRemSet.hpp"
#include "gc/g1/g1MonotonicArenaFreeMemoryTask.hpp"
#include "gc/g1/g1MonotonicArenaFreePool.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/shared/gcTraceTime.inline.hpp"
#include "gc/shared/suspendibleThreadSet.hpp"
#include "runtime/os.hpp"

constexpr const char* G1MonotonicArenaFreeMemoryTask::_state_names[];

const char* G1MonotonicArenaFreeMemoryTask::get_state_name(State value) const {
  return _state_names[static_cast<std::underlying_type_t<State>>(value)];
}

bool G1MonotonicArenaFreeMemoryTask::deadline_exceeded(jlong deadline) {
  return os::elapsed_counter() >= deadline;
}

static size_t keep_size(size_t free, size_t used, double percent) {
  size_t to_keep = used * percent;
  return MIN2(free, to_keep);
}

bool G1MonotonicArenaFreeMemoryTask::calculate_return_infos(jlong deadline) {
  // Ignore the deadline in this step as it is very short.

  G1MonotonicArenaMemoryStats used = _total_used;
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  G1MonotonicArenaFreePool* freelist_pool = g1h->card_set_freelist_pool();
  G1MonotonicArenaMemoryStats free = freelist_pool->memory_sizes();

  _return_info = new G1ReturnMemoryProcessorSet(used.num_pools());
  for (uint i = 0; i < used.num_pools(); i++) {
    size_t return_to_vm_size = keep_size(free._num_mem_sizes[i],
                                         used._num_mem_sizes[i],
                                         G1RemSetFreeMemoryKeepExcessRatio);
    log_trace(gc, task)("Monotonic Arena Free Memory: Type %s: Free: %zu (%zu) "
                        "Used: %zu Keep: %zu",
                        G1CardSetConfiguration::mem_object_type_name_str(i),
                        free._num_mem_sizes[i], free._num_segments[i],
                        used._num_mem_sizes[i], return_to_vm_size);

    _return_info->append(new G1ReturnMemoryProcessor(return_to_vm_size));
  }

  freelist_pool->update_unlink_processors(_return_info);
  return false;
}

bool G1MonotonicArenaFreeMemoryTask::return_memory_to_vm(jlong deadline) {
  for (int i = 0; i < _return_info->length(); i++) {
    G1ReturnMemoryProcessor* info = _return_info->at(i);
    if (!info->finished_return_to_vm()) {
      if (info->return_to_vm(deadline)) {
        return true;
      }
    }
  }
  return false;
}

bool G1MonotonicArenaFreeMemoryTask::return_memory_to_os(jlong deadline) {
  for (int i = 0; i < _return_info->length(); i++) {
    G1ReturnMemoryProcessor* info = _return_info->at(i);
    if (!info->finished_return_to_os()) {
      if (info->return_to_os(deadline)) {
        return true;
      }
    }
  }
  return false;
}

bool G1MonotonicArenaFreeMemoryTask::cleanup_return_infos() {
  for (int i = 0; i < _return_info->length(); i++) {
     G1ReturnMemoryProcessor* info = _return_info->at(i);
     delete info;
  }
  delete _return_info;

  _return_info = nullptr;
  return false;
}

bool G1MonotonicArenaFreeMemoryTask::free_excess_arena_memory() {
  jlong start = os::elapsed_counter();
  jlong end = start +
              (os::elapsed_frequency() / 1000) * G1RemSetFreeMemoryStepDurationMillis;

  log_trace(gc, task)("Monotonic Arena Free Memory: Step start %1.3f end %1.3f",
                      TimeHelper::counter_to_millis(start), TimeHelper::counter_to_millis(end));

  State next_state;

  do {
    switch (_state) {
      case State::CalculateUsed: {
        if (calculate_return_infos(end)) {
          next_state = _state;
          return true;
        }
        next_state = State::ReturnToVM;
        break;
      }
      case State::ReturnToVM: {
        if (return_memory_to_vm(end)) {
          next_state = _state;
          return true;
        }
        next_state = State::ReturnToOS;
        break;
      }
      case State::ReturnToOS: {
        if (return_memory_to_os(end)) {
          next_state = _state;
          return true;
        }
        next_state = State::Cleanup;
        break;
      }
      case State::Cleanup: {
        cleanup_return_infos();
        next_state = State::Inactive;
        break;
      }
      default:
        log_error(gc, task)("Should not try to free excess monotonic area memory in %s state", get_state_name(_state));
        ShouldNotReachHere();
        break;
    }

    set_state(next_state);
  } while (_state != State::Inactive && !deadline_exceeded(end));

  log_trace(gc, task)("Monotonic Arena Free Memory: Step took %1.3fms, done %s",
                      TimeHelper::counter_to_millis(os::elapsed_counter() - start),
                      bool_to_str(_state == State::CalculateUsed));

  return is_active();
}

void G1MonotonicArenaFreeMemoryTask::set_state(State new_state) {
  log_trace(gc, task)("Monotonic Arena Free Memory: State change from %s to %s",
                      get_state_name(_state),
                      get_state_name(new_state));
  _state = new_state;
}

bool G1MonotonicArenaFreeMemoryTask::is_active() const {
  return _state != State::Inactive;
}

jlong G1MonotonicArenaFreeMemoryTask::reschedule_delay_ms() const {
  return G1RemSetFreeMemoryRescheduleDelayMillis;
}

G1MonotonicArenaFreeMemoryTask::G1MonotonicArenaFreeMemoryTask(const char* name) :
  G1ServiceTask(name), _state(State::CalculateUsed), _return_info(nullptr) { }

void G1MonotonicArenaFreeMemoryTask::execute() {
  SuspendibleThreadSetJoiner sts;

  if (free_excess_arena_memory()) {
    schedule(reschedule_delay_ms());
  }
}

void G1MonotonicArenaFreeMemoryTask::notify_new_stats(G1MonotonicArenaMemoryStats* young_gen_stats,
                                                      G1MonotonicArenaMemoryStats* collection_set_candidate_stats) {
  assert_at_safepoint_on_vm_thread();

  _total_used = *young_gen_stats;
  _total_used.add(*collection_set_candidate_stats);

  if (!is_active()) {
    set_state(State::CalculateUsed);
    G1CollectedHeap::heap()->service_thread()->schedule_task(this, 0);
  }
}
