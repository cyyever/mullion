/*
 * Copyright 2026 Feifan He for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#pragma once

#include <cassert>
#include <cstddef>
#include <memory>
#include <utility>

namespace mullion {

// Contiguous power-of-2 circular FIFO buffer (avoids std::deque's chunk indirection for small queues).
template <typename T> class RingQueue {
public:
  RingQueue() = default;

  RingQueue(const RingQueue &) = delete;
  RingQueue &operator=(const RingQueue &) = delete;

  RingQueue(RingQueue &&other) noexcept :
      data_(std::exchange(other.data_, nullptr)),
      head_(std::exchange(other.head_, 0)),
      size_(std::exchange(other.size_, 0)),
      capacity_(std::exchange(other.capacity_, 0)) {}

  RingQueue &
  operator=(RingQueue &&other) noexcept {
    if (this != &other) {
      clear_and_deallocate();
      data_ = std::exchange(other.data_, nullptr);
      head_ = std::exchange(other.head_, 0);
      size_ = std::exchange(other.size_, 0);
      capacity_ = std::exchange(other.capacity_, 0);
    }
    return *this;
  }

  ~RingQueue() { clear_and_deallocate(); }

  bool
  empty() const noexcept {
    return size_ == 0;
  }

  size_t
  size() const noexcept {
    return size_;
  }

  T &
  front() noexcept {
    assert(size_ > 0);
    return data_[head_];
  }

  const T &
  front() const noexcept {
    assert(size_ > 0);
    return data_[head_];
  }

  T &
  back() noexcept {
    assert(size_ > 0);
    return data_[(head_ + size_ - 1) & (capacity_ - 1)];
  }

  const T &
  back() const noexcept {
    assert(size_ > 0);
    return data_[(head_ + size_ - 1) & (capacity_ - 1)];
  }

  void
  push(const T &value) {
    if (size_ == capacity_) {
      T copy(value);
      grow();
      std::construct_at(data_ + ((head_ + size_) & (capacity_ - 1)), std::move(copy));
    } else {
      std::construct_at(data_ + ((head_ + size_) & (capacity_ - 1)), value);
    }
    size_++;
  }

  void
  push(T &&value) {
    if (size_ == capacity_) {
      T moved(std::move(value));
      grow();
      std::construct_at(data_ + ((head_ + size_) & (capacity_ - 1)), std::move(moved));
    } else {
      std::construct_at(data_ + ((head_ + size_) & (capacity_ - 1)), std::move(value));
    }
    size_++;
  }

  template <typename... Args>
  T &
  emplace(Args &&...args) {
    if (size_ == capacity_)
      grow();
    T *slot = std::construct_at(data_ + ((head_ + size_) & (capacity_ - 1)), std::forward<Args>(args)...);
    size_++;
    return *slot;
  }

  void
  pop() noexcept {
    assert(size_ > 0);
    std::destroy_at(data_ + head_);
    head_ = (head_ + 1) & (capacity_ - 1);
    size_--;
  }

private:
  void
  grow() {
    size_t new_cap = capacity_ ? capacity_ * 2 : 8;
    std::allocator<T> alloc;
    T *new_data = alloc.allocate(new_cap);
    for (size_t i = 0; i < size_; i++) {
      T *src = data_ + ((head_ + i) & (capacity_ - 1));
      std::construct_at(new_data + i, std::move(*src));
      std::destroy_at(src);
    }
    if (data_)
      alloc.deallocate(data_, capacity_);
    data_ = new_data;
    head_ = 0;
    capacity_ = new_cap;
  }

  void
  clear_and_deallocate() noexcept {
    if (!data_)
      return;
    for (size_t i = 0; i < size_; i++)
      std::destroy_at(data_ + ((head_ + i) & (capacity_ - 1)));
    std::allocator<T> alloc;
    alloc.deallocate(data_, capacity_);
    data_ = nullptr;
    head_ = 0;
    size_ = 0;
    capacity_ = 0;
  }

  T *data_ = nullptr;
  size_t head_ = 0;
  size_t size_ = 0;
  size_t capacity_ = 0;
};

} // namespace mullion
