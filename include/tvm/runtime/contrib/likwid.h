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
 */
TVM_DLL MetricCollector CreateLikwidMetricCollector(Array<DeviceWrapper> devices);

} // namespace likwid
} // namespace profiling
} // namespace runtime
} // namespace tvm

#endif // TVM_RUNTIME_CONTRIB_LIKWID_H_
