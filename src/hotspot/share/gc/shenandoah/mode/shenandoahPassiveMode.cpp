/*
 * Copyright (c) 2019, 2021, Red Hat, Inc. All rights reserved.
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

#include "gc/shenandoah/heuristics/shenandoahHeuristics.hpp"
#include "gc/shenandoah/heuristics/shenandoahPassiveHeuristics.hpp"
#include "gc/shenandoah/heuristics/shenandoahSpaceInfo.hpp"
#include "gc/shenandoah/mode/shenandoahPassiveMode.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "logging/log.hpp"
#include "logging/logTag.hpp"
#include "runtime/java.hpp"

void ShenandoahPassiveMode::initialize_flags() const {
  // Do not allow concurrent cycles.
  FLAG_SET_DEFAULT(ExplicitGCInvokesConcurrent, false);
  FLAG_SET_DEFAULT(ShenandoahImplicitGCInvokesConcurrent, false);

  // No need for evacuation reserve with Full GC, only for Degenerated GC.
  if (!ShenandoahDegeneratedGC) {
    SHENANDOAH_ERGO_OVERRIDE_DEFAULT(ShenandoahEvacReserve, 0);
  }

  // Disable known barriers by default.
  SHENANDOAH_ERGO_DISABLE_FLAG(ShenandoahLoadRefBarrier);
  SHENANDOAH_ERGO_DISABLE_FLAG(ShenandoahSATBBarrier);
  SHENANDOAH_ERGO_DISABLE_FLAG(ShenandoahCASBarrier);
  SHENANDOAH_ERGO_DISABLE_FLAG(ShenandoahCloneBarrier);
  SHENANDOAH_ERGO_DISABLE_FLAG(ShenandoahStackWatermarkBarrier);
  SHENANDOAH_ERGO_DISABLE_FLAG(ShenandoahCardBarrier);

  // Final configuration checks
  // Passive mode does not instantiate the machinery to support the card table.
  // Exit if the flag has been explicitly set.
  SHENANDOAH_CHECK_FLAG_UNSET(ShenandoahCardBarrier);
}

ShenandoahHeuristics* ShenandoahPassiveMode::initialize_heuristics(ShenandoahSpaceInfo* space_info) const {
  if (ShenandoahGCHeuristics == nullptr) {
    vm_exit_during_initialization("Unknown -XX:ShenandoahGCHeuristics option (null)");
  }
  return new ShenandoahPassiveHeuristics(space_info);
}
