#ifndef TVM_RUNTIME_CONTRIB_LIKWID_H_
#define TVM_RUNTIME_CONTRIB_LIKWID_H_

#include <tvm/runtime/container/array.h>
#include <tvm/runtime/container/map.h>
#include <tvm/runtime/profiling.h>

namespace tvm {
namespace runtime {
namespace profiling {
namespace likwid {

struct LikwidMetricCollectorConfiguration;

TVM_DLL MetricCollector CreateLikwidMetricCollector(LikwidMetricCollectorConfiguration config);

std::string rpc_likwid_profile_func(Module vm_mod, String func_name,
                                    LikwidMetricCollectorConfiguration config);

}  // namespace likwid
}  // namespace profiling
}  // namespace runtime
}  // namespace tvm

#endif  // TVM_RUNTIME_CONTRIB_LIKWID_H_