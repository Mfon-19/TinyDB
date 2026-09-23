#pragma once

#include <pthread.h>

namespace tinydb::detail {

// A shared mutex that blocks new readers while a writer waits, so a steady
// stream of readers cannot starve commits.
class SharedMutex {
public:
  SharedMutex() noexcept {
    pthread_rwlockattr_t attributes;
    pthread_rwlockattr_init(&attributes);
    pthread_rwlockattr_setkind_np(&attributes,
                                  PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
    pthread_rwlock_init(&lock_, &attributes);
    pthread_rwlockattr_destroy(&attributes);
  }
  ~SharedMutex() { pthread_rwlock_destroy(&lock_); }

  SharedMutex(const SharedMutex &) = delete;
  auto operator=(const SharedMutex &) -> SharedMutex & = delete;

  void lock() noexcept { pthread_rwlock_wrlock(&lock_); }
  void unlock() noexcept { pthread_rwlock_unlock(&lock_); }
  void lock_shared() noexcept { pthread_rwlock_rdlock(&lock_); }
  void unlock_shared() noexcept { pthread_rwlock_unlock(&lock_); }

private:
  pthread_rwlock_t lock_;
};

} // namespace tinydb::detail
