// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-

#pragma once

#include "lock_policy.h"
#ifdef NDEBUG
#include <mutex>
#else
#include "Mutex.h"
#endif

template<LockPolicy lp>
class LockMutex {
  // empty helper class except when the template argument is not LockPolicy::MUTEX
  struct Dummy {
    void lock() {}
    bool try_lock() {
      return true;
    }
    void unlock() {}
    bool is_locked() const {
      return true;
    }
  };

public:
  using type = Dummy;

  // discard the constructor params
  template<typename... Args>
  static Dummy create(Args&& ...) {
    return Dummy{};
  }
};

#ifdef NDEBUG
template<>
class LockMutex<LockPolicy::MUTEX> {
public:
  using type = std::mutex;
  // discard the constructor params
  template<typename... Args>
  static std::mutex create(Args&& ...) {
    return std::mutex{};
  }
};
#else
template<>
class LockMutex<LockPolicy::MUTEX> {
  struct MutexWrapper {
    template<typename... Args>
    MutexWrapper(Args&& ...args)
      : mutex{std::forward<Args>(args)...}
    {}
    bool is_locked() const {
      return mutex.is_locked();
    }
    void lock() {
      return mutex.Lock();
    }
    bool try_lock() {
      return mutex.TryLock();
    }
    void unlock() {
      mutex.Unlock();
    }
    Mutex& native_handle() {
      return mutex;
    }
  private:
    Mutex mutex;
  };
public:
  using type = MutexWrapper;
  template<typename... Args>
  static MutexWrapper create(Args&& ...args) {
    return MutexWrapper(std::forward<Args>(args)...);
  }
};
#endif	// NDEBUG

template<LockPolicy lp> using LockMutexT = typename LockMutex<lp>::type;
