#include <tvm/runtime/contrib/rpc_profiling.h>

#include <tvm/runtime/container/adt.h>
#include <tvm/runtime/data_type.h>
#include <tvm/runtime/registry.h>
#include <tvm/runtime/vm/vm.h>
#include <tvm/runtime/profiling.h>
#include <tvm/runtime/contrib/likwid.h>

#include <vector>
#include <string>

namespace tvm {
namespace runtime {
namespace rpc_profiling {

/*! \brief Execute a profiling run of the given function using the provided vm.
*/
std::string rpc_likwid_profile_func(runtime::Module vm_mod, std::string func_name) {
    LOG(INFO) << "Received profiling request for function " << func_name;
    auto profile_func = vm_mod.GetFunction("profile");
    Array<profiling::MetricCollector> collectors({
        profiling::CreateLikwidMetricCollector(Array<profiling::DeviceWrapper>())
    });
    LOG(INFO) << "Beginning profiling...";
    profiling::Report report = profile_func(func_name, collectors);
    LOG(INFO) << "Done. Sending serialized report.";
    return std::string(report->AsJSON().c_str());
}

TVM_REGISTER_GLOBAL("runtime.rpc_likwid_profile_func").set_body_typed(
    rpc_likwid_profile_func
);

} // namespace rpc_profiling
} // namespace runtime
} // namespace tvm