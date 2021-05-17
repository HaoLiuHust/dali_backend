// The MIT License (MIT)
//
// Copyright (c) 2021 NVIDIA CORPORATION
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#ifndef TRITONDALIBACKEND_IO_BUFFER_H
#define TRITONDALIBACKEND_IO_BUFFER_H

#include "src/dali_executor/io_descriptor.h"
#include "src/dali_executor/utils/dali.h"

namespace triton { namespace backend { namespace dali {


void MemCopy(device_type_t dst_dev, void *dst, device_type_t src_dev, const void *src, size_t size,
             cudaStream_t stream = 0);

class IOBufferI {
 public:
  /**
   * @brief Allocate a chunk of `size` of bytes and return a pointer
   *        to the beginning of the chunk.
   * @param size Size of the allocation in bytes.
   * @return Pointer to the beggining of the allocated chunk.
   */
  virtual uint8_t *Allocate(size_t size) = 0;

  /**
   * @brief Cancel all reservations. No memory is deallocated.
   */
  virtual void Clear() = 0;

  /**
   * @brief Reserve `size` bytes of memory.
   *
   * If the buffer's capacity is greater or equal to size, this function is a no-op.
   * @param size Amount of memory to reserve in bytes.
   */
  virtual void Reserve(size_t size) = 0;

  /**
   * @brief Get the the amount of allocated memory.
   * @return Allocation size in bytes.
   */
  virtual size_t Capacity() const = 0;

  /**
   * @brief Get device type of the allocated memory.
   * @return Device type.
   */
  virtual device_type_t DeviceType() const = 0;

  /**
   * @brief Get an immutable descriptor of the buffer.
   * @return Input buffer descriptor.
   */
  virtual IBufferDescr GetDescr() const = 0;

  /**
   * @brief Get a mutable descriptor of the buffer.
   * @return Output buffer descriptor.
   */
  virtual OBufferDescr GetDescr() = 0;

  virtual ~IOBufferI() {}

 protected:
  IOBufferI() {}
};

template<typename T, device_type_t Dev>
using buffer_t = std::conditional_t<Dev == device_type_t::CPU, std::vector<T>, DeviceBuffer<T>>;

template<device_type_t Dev>
class IOBuffer : public IOBufferI {
 public:
  IOBuffer(size_t size = 0) : buffer_() {
    buffer_.resize(size);
    if (Dev == device_type_t::GPU) {
      CUDA_CALL_GUARD(cudaGetDevice(&device_id_));
    }
  }

  uint8_t *Allocate(size_t size) override {
    ENFORCE(filled_ + size <= buffer_.size(),
            make_string("Not enough memory reserved (", Capacity(), " bytes) to allocate ",
                        "a chunk of size ", size));
    auto origin = buffer_.data() + filled_;
    filled_ += size;
    return origin;
  }

  void Clear() override {
    filled_ = 0;
  }

  void Reserve(size_t size) override {
    if (size > buffer_.size()) {
      ENFORCE(filled_ == 0, "Cannot reserve more memory for buffer that was already reserved.");
      buffer_.resize(size);
    }
  }

  size_t Capacity() const override {
    return buffer_.size();
  }

  device_type_t DeviceType() const override {
    return Dev;
  }

  IBufferDescr GetDescr() const override {
    IBufferDescr descr;
    descr.data = buffer_.data();
    descr.size = filled_;
    descr.device = Dev;
    descr.device_id = device_id_;
    return descr;
  }

  OBufferDescr GetDescr() override {
    OBufferDescr descr;
    descr.data = buffer_.data();
    descr.size = filled_;
    descr.device = Dev;
    descr.device_id = device_id_;
    return descr;
  }

 private:
  buffer_t<uint8_t, Dev> buffer_;
  size_t filled_ = 0;
  int device_id_ = 0;
};

}}}  // namespace triton::backend::dali

#endif  // TRITONDALIBACKEND_IO_BUFFER_H