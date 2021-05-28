// The MIT License (MIT)
//
// Copyright (c) 2020 NVIDIA CORPORATION
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

#include "src/dali_executor/dali_executor.h"
#include "src/dali_executor/utils/dali.h"

namespace triton { namespace backend { namespace dali {

void DaliExecutor::SetupInputs(const std::vector<IDescr>& inputs) {
  assert(!inputs.empty());
  int batch_size = inputs[0].meta.shape.num_samples();
  for (size_t i = 1; i < inputs.size(); ++i) {
    assert(inputs[i].meta.shape.num_samples() == batch_size &&
           "All inputs should have equal batch size.");
  }
  std::vector<IDescr> c_inputs{};
  for (auto& inp : inputs) {
    size_t inp_size = inp.meta.shape.num_elements() * dali_type_size(inp.meta.type);
    if (IsNoCopy(inp)) {
      assert(inp_size <= inp.buffers[0].size);
      c_inputs.push_back(inp);
    } else {
      // Copy buffers to a contiguous buffer on the proper device
      c_inputs.push_back(ScheduleInputCopy(inp));
      assert(inp_size <= c_inputs.back().buffers[0].size);
    }
  }
  RunInputCopy();
  for (auto& inp : c_inputs) {
    pipeline_.SetInput(inp);
  }
}


IDescr DaliExecutor::ScheduleInputCopy(const IDescr& input) {
  assert(input.buffers.size() > 0);
  IOBufferI* buffer;
  if (input.buffers[0].device == device_type_t::CPU) {
    buffer = &cpu_buffers_[input.meta.name + "_inp"];
  } else {
    buffer = &gpu_buffers_[input.meta.name + "_inp"];
  }
  size_t size = 0;
  for (auto& buf : input.buffers)
    size += buf.size;
  buffer->resize(size);
  auto descriptor = buffer->get_descr();
  char* dst = reinterpret_cast<char*>(descriptor.data);
  for (auto& buf : input.buffers) {
    thread_pool_.AddWork(
        [descriptor, dst, buf](int) {
          MemCopy(descriptor.device, dst, buf.device, buf.data, buf.size);
        },
        buf.size, true);
    dst += buf.size;
  }
  return IDescr{input.meta, {descriptor}};
}

void DaliExecutor::RunInputCopy() {
  thread_pool_.RunAll();
}


bool DaliExecutor::IsNoCopy(const IDescr& input) {
  return input.buffers.size() == 1 && (input.buffers[0].device == device_type_t::CPU ||
                                       input.buffers[0].device_id == pipeline_.DeviceId());
}

std::vector<OutputInfo> DaliExecutor::Run(const std::vector<IDescr>& inputs) {
  SetupInputs(inputs);
  try {
    pipeline_.Run();
    pipeline_.Output();
  } catch (std::runtime_error& e) {
    pipeline_.Reset();
    throw e;
  }
  std::vector<OutputInfo> ret(pipeline_.GetNumOutput());
  auto outputs_shapes = pipeline_.GetOutputShapes();
  for (size_t out_idx = 0; out_idx < ret.size(); out_idx++) {
    ret[out_idx] = {outputs_shapes[out_idx], pipeline_.GetOutputType(out_idx),
                    pipeline_.GetOutputDevice(out_idx)};
  }
  return ret;
}

void DaliExecutor::PutOutputs(const std::vector<ODescr>& outputs) {
  for (uint32_t output_idx = 0; output_idx < outputs.size(); ++output_idx) {
    if (outputs[output_idx].buffers.size() == 1) {
      auto buffer = outputs[output_idx].buffers[0];
      pipeline_.PutOutput(buffer.data, output_idx, buffer.device);
    } else {
      const auto& name = outputs[output_idx].meta.name;
      const auto& out_buffers = outputs[output_idx].buffers;
      size_t size = 0;
      for (auto& out_buff : out_buffers) {
        size += out_buff.size;
      }
      IOBufferI* interm_buffer;
      if (pipeline_.GetOutputDevice(output_idx) == device_type_t::CPU) {
        interm_buffer = &cpu_buffers_[name + "_out"];
      } else {
        interm_buffer = &gpu_buffers_[name + "_out"];
      }
      interm_buffer->resize(size);
      auto interm_descr = interm_buffer->get_descr();
      pipeline_.PutOutput(interm_descr.data, output_idx, interm_descr.device);
      char* src = reinterpret_cast<char*>(interm_descr.data);
      for (auto& buf : out_buffers) {
        thread_pool_.AddWork(
            [src, buf, interm_descr](int) {
              MemCopy(buf.device, buf.data, interm_descr.device, src, buf.size);
            },
            buf.size, false);  // deferred execution
        src += buf.size;
      }
    }
  }
  pipeline_.SyncOutputStream();
  thread_pool_.RunAll();
}

}}}  // namespace triton::backend::dali
