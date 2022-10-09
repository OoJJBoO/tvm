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

PackedFunc RPCProfiler::profile(std::vector<std::string> collector_names) {
    std::vector<profiling::MetricCollector> collectors{};
    for (const auto collector_name : collector_names) {
        if (collector_name == "likwid") {
            // collectors.push_back(profiling::CreateLikwidMetricCollector({}));
        }
    }
    return TypedPackedFunc<std::string(std::string)>(
        [collectors, this](String arg_name) {
            auto prof = profiling::Profiler(devs_, collectors, {{String("Executor"), String("VM")}});
            auto invoke = vm_->GetFunction("invoke", vm_);
            // warmup
            for (int i{}; i < 3; ++i) {
                invoke(arg_name);
            }
            prof.Start();
            invoke(arg_name);
            prof.Stop();
            auto report = prof.Report();
            return report->AsJSON();
        }
    );
}

TVM_REGISTER_GLOBAL("runtime._RPCProfiler").set_body([](TVMArgs args, TVMRetValue* rv) {
    runtime::Module mod = args[0];
    LOG(INFO) << "runtime._RPCProfiler called with type_key " << mod.operator->()->type_key();
    auto* exec = dynamic_cast<tvm::runtime::vm::Executable*>(mod.operator->());
    RPCProfiler prof(GetObjectPtr<tvm::runtime::vm::Executable>(exec), {});
    *rv = prof.profile({});
});

} // namespace rpc_profiling
} // namespace runtime
} // namespace tvm