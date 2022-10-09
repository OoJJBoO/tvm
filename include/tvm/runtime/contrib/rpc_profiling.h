#ifndef TVM_RUNTIME_CONTRIB_RPC_PROFILING_H_
#define TVM_RUNTIME_CONTRIB_RPC_PROFILING_H_

#include <tvm/runtime/container/adt.h>
#include <tvm/runtime/data_type.h>
#include <tvm/runtime/registry.h>
#include <tvm/runtime/vm/vm.h>

#include <vector>
#include <string>

namespace tvm {
namespace runtime {
namespace rpc_profiling {

class RPCProfiler {
public:
    RPCProfiler(const ObjectPtr<tvm::runtime::vm::Executable>& exec, std::vector<Device> devices)
    : devs_(devices) {
        vm_ = make_object<tvm::runtime::vm::VirtualMachine>();
        vm_->LoadExecutable(exec);
    }

    PackedFunc profile(std::vector<std::string> collector_names);

private:
    ObjectPtr<tvm::runtime::vm::VirtualMachine> vm_;
    std::vector<Device> devs_;
};

} // namespace rpc_profiling
} // namespace runtime
} // namespace tvm

#endif // TVM_RUNTIME_CONTRIB_RPC_PROFILING_H_