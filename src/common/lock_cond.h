// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-

#pragma once

#include "lock_policy.h"
#include "lock_mutex.h"
#ifdef NDEBUG
#include <condition_variable>
#else
#include "Cond.h"
#endif

class SharedLRUTest;

// empty helper class except when the template argument is not LockPolicy::MUTEX
template<LockPolicy lock_policy>
class LockCond {
public:
  void wait(std::unique_lock<LockMutex<lock_policy>>&) {}
  template<class Predicate>
  void wait(std::unique_lock<LockMutex<lock_policy>>& lock, Predicate pred) {
    while (!pred()) {
      // TODO: PAUSE on x86
      wait(lock);
    }
  }
  void notify_one() noexcept {}
};

#ifdef NDEBUG
template<>
class LockCond<LockPolicy::MUTEX> : public std::condition_variable
{};
#else
template<>
class LockCond<LockPolicy::MUTEX> {
public:
  void wait(std::unique_lock<LockMutexT<LockPolicy::MUTEX>>& lock) {
    cond.Wait(lock.mutex()->native_handle());
  }
  template<class Predicate>
  void wait(std::unique_lock<LockMutexT<LockPolicy::MUTEX>>& lock,
	    Predicate pred) {
    while (!pred()) {
      wait(lock);
    }
  }
  void notify_one() noexcept {
    cond.Signal();
  }
private:
  Cond cond;
};
#endif	// NDEBUG
