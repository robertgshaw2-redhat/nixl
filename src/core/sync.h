/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef SYNC_H
#define SYNC_H
#include "common/util.h"
#include "nixl_params.h"
#include "absl/synchronization/mutex.h"
#include <shared_mutex>

class nixlLock {
    public:
        nixlLock(const nixl_thread_sync_t sync_mode) {
            switch (sync_mode) {
            case nixl_thread_sync_t::NIXL_THREAD_SYNC_NONE:
                lock_cb = unlock_cb = lock_shared_cb = unlock_shared_cb = []() {};
                break;
            case nixl_thread_sync_t::NIXL_THREAD_SYNC_STRICT:
                lock_cb = lock_shared_cb = [this]() {
                    m.Lock();
                };
                unlock_cb = unlock_shared_cb = [this]() {
                    m.Unlock();
                };
                break;
            case nixl_thread_sync_t::NIXL_THREAD_SYNC_RW:
                lock_cb = [this]() {
                    m.Lock();
                };
                unlock_cb = [this]() {
                    m.Unlock();
                };
                lock_shared_cb = [this]() {
                    m.ReaderLock();
                };
                unlock_shared_cb = [this]() {
                    m.ReaderUnlock();
                };
                break;
            }
        }

        void lock() {
            lock_cb();
        }

        void lock_shared() {
            lock_shared_cb();
        }

        void unlock() {
            unlock_cb();
        }

        void unlock_shared() {
            unlock_shared_cb();
        }

    private:
        std::function<void()> lock_cb;
        std::function<void()> unlock_cb;
        std::function<void()> lock_shared_cb;
        std::function<void()> unlock_shared_cb;

        absl::Mutex m;
};

#define NIXL_LOCK_GUARD(lock) const std::lock_guard<nixlLock> UNIQUE_NAME(lock_guard) (lock)
#define NIXL_SHARED_LOCK_GUARD(lock) const std::shared_lock<nixlLock> UNIQUE_NAME(lock_guard) (lock)

#endif /* SYNC_H */
