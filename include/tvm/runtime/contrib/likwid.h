#ifndef TVM_RUNTIME_CONTRIB_LIKWID_H_
#define TVM_RUNTIME_CONTRIB_LIKWID_H_

#include <tvm/runtime/container/array.h>
#include <tvm/runtime/container/map.h>
#include <tvm/runtime/profiling.h>

namespace tvm {
namespace runtime {
namespace profiling {
namespace likwid {

TVM_DLL MetricCollector CreateLikwidMetricCollector(bool collect_raw_events, bool collect_derived_metrics, bool collect_thread_values);

std::string rpc_likwid_profile_func(Module vm_mod, std::string func_name, bool collect_raw_events, bool collect_derived_metrics, bool collect_thread_values);

} // namespace likwid
} // namespace profiling
} // namespace runtime
} // namespace tvm

#endif // TVM_RUNTIME_CONTRIB_LIKWID_H_
