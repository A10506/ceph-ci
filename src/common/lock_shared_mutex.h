// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include "lock_policy.h"
#ifdef NDEBUG
#include <shared_mutex>
#else
#include "RWLock.h"
#endif

template<LockPolicy lp>
class SharedMutex {
public:
  template<typename... Args>
  SharedMutex(Args&& ...) {}
  // exclusive lock
  void lock() {}
  bool try_lock() {
    return true;
  }
  void unlock() {}
  // shared lock
  void lock_shared() {}
  bool try_lock_shared() {
    return true;
  }
  void unlock_shared() {}
};

#ifdef NDEBUG
template<>
class SharedMutex<LockPolicy::MUTEX> : public std::shared_mutex
{
public:
  template<typename... Args>
  SharedMutex(Args&&... args) {}
};
#else
template<>
class SharedMutex<LockPolicy::MUTEX> {
  RWLock rwlock;
public:
  template<typename... Args>
  SharedMutex(Args&&... args)
    : rwlock(std::forward<Args>(args)...)
  {}
  // exclusive lock
  void lock() {
    rwlock.get_write();
  }
  bool try_lock() {
    return rwlock.try_get_write();
  }
  void unlock() {
    return rwlock.put_write();
  }
  // shared lock
  void lock_shared() {
    rwlock.get_read();
  }
  bool try_lock_shared() {
    return rwlock.try_get_read();
  }
  void unlock_shared() {
    rwlock.put_read();
  }
};
#endif	// NDEBUG
