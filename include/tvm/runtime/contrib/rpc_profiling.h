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

std::string rpc_likwid_profile_func(runtime::Module vm_mod, std::string func_name);

} // namespace rpc_profiling
} // namespace runtime
} // namespace tvm

#endif // TVM_RUNTIME_CONTRIB_RPC_PROFILING_H_