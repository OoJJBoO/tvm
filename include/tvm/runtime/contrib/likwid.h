#ifndef TVM_RUNTIME_CONTRIB_LIKWID_H_
#define TVM_RUNTIME_CONTRIB_LIKWID_H_

#include <tvm/runtime/container/array.h>
#include <tvm/runtime/container/map.h>
#include <tvm/runtime/profiling.h>

namespace tvm {
namespace runtime {
namespace profiling {
namespace likwid {

/*! \brief Construct a metric collector that uses the likwid-perfctr API to 
 * collect hardware counter data.
 *
 * \note Please make sure to run TVM through the likwid-perfctr wrapper 
 * application following the instructions given in the Likwid documentation!
 * 
 * \param devices Not used at the moment.
 */
TVM_DLL MetricCollector CreateLikwidMetricCollector(Array<DeviceWrapper> devices);

/*! \brief Execute a profiling run of the given function using the provided vm. 
 *
 * \param vm_mod The `Module` containing the profiler vm to profile on.
 * \param func_name The name of the function to profile.
 * \returns The serialized `Report` of the profiling run.
*/
std::string rpc_likwid_profile_func(runtime::Module vm_mod, std::string func_name);

} // namespace likwid
} // namespace profiling
} // namespace runtime
} // namespace tvm

#endif // TVM_RUNTIME_CONTRIB_LIKWID_H_
