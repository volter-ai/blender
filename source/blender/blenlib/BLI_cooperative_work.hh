/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <functional>
#include <utility>

namespace blender {
/** Caller-thread backpressure for native work owned by a resumable browser job.
 * Worker threads have no scope. A failed checkpoint is latched until the owner
 * returns through its normal error path; intermediate native allocations still
 * finish construction and are cleaned up normally. No task may mutate Blender
 * data from the checkpoint callback. */
class CooperativeWork {
  inline static thread_local CooperativeWork *current_ = nullptr;
  CooperativeWork *previous_;
  std::function<bool()> callback_;
  bool failed_ = false;
 public:
  explicit CooperativeWork(std::function<bool()> callback)
      : previous_(current_), callback_(std::move(callback)) { current_ = this; }
  ~CooperativeWork() { current_ = previous_; }
  CooperativeWork(const CooperativeWork &) = delete;
  CooperativeWork &operator=(const CooperativeWork &) = delete;
  bool poll() {
    if (!failed_ && callback_) failed_ = !callback_();
    return !failed_;
  }
  bool failed() const { return failed_; }
  static bool checkpoint() { return current_ ? current_->poll() : true; }
};
}  // namespace blender
