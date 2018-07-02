// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
#pragma once

#include <atomic>
#include <condition_variable>
#include <tuple>
#include <type_traits>
#include <boost/lockfree/queue.hpp>
#include <boost/optional.hpp>
#include <core/future.hh>
#include <core/gate.hh>
#include <core/semaphore.hh>
#include <core/sharded.hh>

#include "Condition.h"

namespace ceph::thread {

struct WorkItem {
  virtual ~WorkItem() {}
  virtual void process() = 0;
};

template<typename Func, typename T = std::invoke_result_t<Func>>
struct Task final : WorkItem {
  Func func;
  seastar::future_state<T> state;
  ceph::thread::Condition on_done;
public:
  explicit Task(Func&& f)
    : func(std::move(f))
  {}
  void process() override {
    try {
      state.set(func());
    } catch (...) {
      state.set_exception(std::current_exception());
    }
    on_done.notify();
  }
  seastar::future<T> get_future() {
    return on_done.wait().then([this] {
      return seastar::make_ready_future<T>(state.get0(std::move(state).get()));
    });
  }
};

struct SubmitQueue {
  seastar::semaphore free_slots;
  seastar::gate pending_tasks;
  explicit SubmitQueue(size_t num_free_slots)
    : free_slots(num_free_slots)
  {}
  seastar::future<> stop() {
    return pending_tasks.close();
  }
};

/// an engine for scheduling non-seastar tasks from seastar fibers
class ThreadPool {
  std::atomic<bool> stopping = false;
  std::mutex mutex;
  std::condition_variable cond;
  std::vector<std::thread> threads;
  seastar::sharded<SubmitQueue> submit_queue;
  const size_t queue_size;
  boost::lockfree::queue<WorkItem*> pending;

  void loop();
  bool is_stopping() const {
    return stopping.load(std::memory_order_relaxed);
  }
  static void pin(unsigned cpu_id);
  seastar::semaphore& local_free_slots() {
    return submit_queue.local().free_slots;
  }
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;
public:
  /**
   * @param queue_sz the depth of pending queue. before a task is scheduled,
   *                 it waits in this queue. we will round this number to
   *                 multiple of the number of cores.
   * @param n_threads the number of threads in this thread pool.
   * @param cpu the CPU core to which this thread pool is assigned
   * @note each @c Task has its own ceph::thread::Condition, which possesses
   * possesses an fd, so we should keep the size of queue under a resonable
   * limit.
   */
  ThreadPool(size_t n_threads, size_t queue_sz, unsigned cpu);
  ~ThreadPool();
  seastar::future<> start();
  seastar::future<> stop();
  template<typename Func, typename...Args>
  auto submit(Func&& func, Args&&... args) {
    auto packaged = [func=std::move(func),
                     args=std::forward_as_tuple(args...)] {
      return std::apply(std::move(func), std::move(args));
    };
    return seastar::with_gate(submit_queue.local().pending_tasks,
      [packaged=std::move(packaged), this] {
        return local_free_slots().wait()
          .then([packaged=std::move(packaged), this] {
            auto task = new Task{std::move(packaged)};
            auto fut = task->get_future();
            pending.push(task);
            cond.notify_one();
            return fut.finally([task, this] {
              local_free_slots().signal();
              delete task;
            });
          });
        });
  }
};

} // namespace ceph::thread
