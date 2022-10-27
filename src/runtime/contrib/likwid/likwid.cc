#include <likwid.h>
#include <tvm/runtime/contrib/likwid.h>

#include <string>
#include <vector>

namespace tvm {
namespace runtime {
namespace profiling {
namespace likwid {

// ------------------------------------------------------------------------------------------------
// Some constants
// ------------------------------------------------------------------------------------------------

constexpr const char* REGION_NAME = "LikwidMetricCollector";
constexpr const char* OVERFLOW_WARNING = "Detected overflow while reading performance counter, setting value to -1";
constexpr const char* NO_METRICS_WARNING = "Current event group does not have any metrics! Maybe consider enabling collection of raw events?";
constexpr const char* THREAD_COUNT_ERROR = "No threads are known to LIKWID perfmon!";

// ------------------------------------------------------------------------------------------------
// Convenience functions with error printing
// ------------------------------------------------------------------------------------------------

/*! \brief Register default marker region and print errors. */
inline void _marker_register_region() {
    int status = likwid_markerRegisterRegion(REGION_NAME);
    if (status != 0) {
        LOG(ERROR) << "Could not register region! Status: " << status;
    }
}

/*! \brief Start default marker region and print errors. */
inline void _marker_start_region() {
    int status = likwid_markerStartRegion(REGION_NAME);
    if (status != 0) {
        LOG(ERROR) << "Could not start marker region! Status: " << status;
    }
}

/*! \brief Stop default marker region and print errors. */
inline void _marker_stop_region() {
    int status = likwid_markerStopRegion(REGION_NAME);
    if (status != 0) {
        LOG(ERROR) << "Could not stop marker region! Status: " << status;
    }
}

/*! \brief Get results of the given marker region and print errors.
 *
 * \param region_tag [in] The tag of the region to read.
 * \param nevents [in/out] The size of the `events` array. Will be set to 
 * the number of available metrics on return.
 * \param events [in/out] Array containing the collected event counts.
 * \param time [out] The elapsed time since the region was started.
 * \param count [out] The call count of the marker region.
*/
inline void _marker_get_region(const char* region_tag, int* nevents, double* events, double* time, int* count) {
    likwid_markerGetRegion(region_tag, nevents, events, time, count);
    if (nevents == 0) {
        LOG(WARNING) << "Event count is zero!";
    }
}

/*! \brief Read the current event set's counters.
 *
 * \param nevents [in/out] The size of the `events` array. Will be set to 
 * the number of available metrics on return.
 * \param events [in/out] Array containing the collected event counts.
 * \param time [out] The elapsed time since the region was started.
 * \param count [out] The call count of the marker region.
*/
inline void _marker_read_event_counts(int* nevents, double* events, double* time, int* count) {
    _marker_stop_region();
    _marker_get_region(REGION_NAME, nevents, events, time, count);
    _marker_start_region();
}

/*! \brief Read all counters of the given group ID and print errors. */
inline void _perfmon_read_group(int group_id) {
    int status = perfmon_readGroupCounters(group_id);
    if (status < 0) {
        LOG(ERROR) << "Error while reading group counters! Status: " << status;
    }
}

/*! \brief Start perfmon counters and print errors. */
inline void _perfmon_start_counters() {
    int status = perfmon_startCounters();
    if (status != 0) {
        LOG(ERROR) << "Could not start counters! Status: " << status;
    }
}

/*! \brief Stop perfmon counters and print errors. */
inline void _perfmon_stop_counters() {
    int status = perfmon_stopCounters();
    if (status != 0) {
        LOG(ERROR) << "Could not stop counters! Status: " << status;
    }
}

/*! \brief Read the current group's counters on all known threads and report 
 * results.
 *
 * \return An unordered map mapping the names of the metrics inside the current
 * event group to a list of their respective per-thread counts.
*/
std::unordered_map<std::string, std::vector<double>> _perfmon_read_and_get_metrics() {
    int group_id = perfmon_getIdOfActiveGroup();
    _perfmon_read_group(group_id);
    int number_of_metrics = perfmon_getNumberOfMetrics(group_id);
    int number_of_threads = perfmon_getNumberOfThreads();
    std::unordered_map<std::string, std::vector<double>> result_map;
    for (int metric_id{}; metric_id < number_of_metrics; ++metric_id) {
        std::string metric_name = perfmon_getMetricName(group_id, metric_id);
        std::vector<double> results;
        for (int thread_id{}; thread_id < number_of_threads; ++thread_id) {
            results.push_back(perfmon_getMetric(group_id, metric_id, thread_id));
        }
        result_map[metric_name] = results;
    }
    return result_map;
}

/*! \brief Read the current group's counters on all known threads and report 
 * results.
 *
 * \return An unordered map mapping the names of the events inside the current 
 * event group to a list of their respective per-thread counts.
*/
std::unordered_map<std::string, std::vector<double>> _perfmon_read_and_get_results() {
    int group_id = perfmon_getIdOfActiveGroup();
    _perfmon_read_group(group_id);
    int number_of_events = perfmon_getNumberOfEvents(group_id);
    int number_of_threads = perfmon_getNumberOfThreads();
    std::unordered_map<std::string, std::vector<double>> result_map;
    for (int event_id{}; event_id < number_of_events; ++event_id) {
        std::string event_name = perfmon_getEventName(group_id, event_id);
        std::vector<double> results;
        for (int thread_id{}; thread_id < number_of_threads; ++thread_id) {
            results.push_back(perfmon_getResult(group_id, event_id, thread_id));
        }
        result_map[event_name] = results;
    }
    return result_map;
}

// ------------------------------------------------------------------------------------------------
// Likwid MetricCollector
// ------------------------------------------------------------------------------------------------

/*! \brief Object holding start values of collected metrics. */
struct LikwidEventSetNode : public Object {
    std::unordered_map<std::string, std::vector<double>> start_values;
    Device dev;

    /*! \brief Construct a new event set node.
     *
     * \param start_values The event values at the time of creating this node.
     * \param dev The device this node is created for.
    */
    explicit LikwidEventSetNode(std::unordered_map<std::string, std::vector<double>> start_values, Device dev)
        : start_values(start_values), dev(dev) {}

    static constexpr const char* _type_key = "LikwidEventSetNode";
    TVM_DECLARE_FINAL_OBJECT_INFO(LikwidEventSetNode, Object);
};


/*! \brief MetricCollectorNode for metrics collected using likwid-perfctr API.
 *
 * \note Please make sure to run TVM through the likwid-perfctr wrapper 
 * application following the instructions given in the Likwid documentation 
 * when using this collector!
*/
struct LikwidMetricCollectorNode final : public MetricCollectorNode {
    
    /*! \brief Construct a new collector node object.
     *
     * \param collect_raw_events If this is true, also collect the raw events 
     * used in the set event group instead of only the derived metrics.
     * \param collect_thread_values If this is true, also collect the event
     * counts of each known thread instead of only the total.
     * \todo Add compatibility check!
    */
    explicit LikwidMetricCollectorNode(bool collect_raw_events, bool collect_thread_values)
    : _collect_derived_metrics(collect_raw_events), _collect_thread_values(collect_thread_values) {}

    /*! \brief Initialization call. Establish connection to likwid-perfctr API.
     *
     * \param devices Not used by this collector at the moment.
    */
    void Init(Array<DeviceWrapper> devices) override {
        likwid_markerInit();
        likwid_markerThreadInit();
        // Since currently we use a combination of the marker API for 
        // initialization and perfmon calls for actual readings, we need to
        // open a marker region to prevent LIKWID printing warnings when the
        // process terminates. This should not be an issue once we replace the
        // marker API calls with manual perfmon initialization.
        _marker_start_region();
        _marker_stop_region();
    }

    /*! \brief Begin collecting counter data.
     *
     * \param device Not used by this collector at the moment.
     * \returns A `LikwidEventSetNode` containing the values read at the start 
     * of the call. Used by the next `Stop` call to determine difference.
    */
    ObjectRef Start(Device device) override {
        if (device.device_type != kDLCPU) {
            LOG(WARNING) << "For now, this collector only supports CPUs!";
        }
        auto start_values = _perfmon_read_and_get_results();
        return ObjectRef(make_object<LikwidEventSetNode>(start_values, device));
    }

    /*! \brief End data collection and report results.
     *
     * \param object The previously created `LikwidEventSetNode`.
     * \returns A mapping from the names of the collected metrics to their 
     * corresponding values.
    */
    Map<String, ObjectRef> Stop(ObjectRef object) override {
        // Collect event counts
        std::unordered_map<String, ObjectRef> reported_metrics;
        const LikwidEventSetNode* event_set_node = object.as<LikwidEventSetNode>();
        const auto end_values = _perfmon_read_and_get_results();
        for (const auto& name_result : end_values) {
            std::string event_name = name_result.first;
            std::vector<double> end_thread_values = name_result.second;
            std::vector<double> start_thread_values = event_set_node->start_values.at(event_name);
            double total = 0;
            for (std::size_t thread_id{}; thread_id < end_thread_values.size(); ++thread_id) {
                std::string name = event_name + " [Thread " + std::to_string(thread_id) + "]";
                double diff = end_thread_values[thread_id] - start_thread_values[thread_id];
                if (diff < 0) {
                    LOG(WARNING) << OVERFLOW_WARNING;
                    if (!_collect_thread_values) {
                        continue;
                    }
                    reported_metrics[name] = ObjectRef(make_object<CountNode>(-1));
                } else {
                    total += diff;
                    if (!_collect_thread_values) {
                        continue;
                    }
                    reported_metrics[name] = ObjectRef(make_object<CountNode>(diff));
                }
            }
            std::string name = event_name;
            if (_collect_thread_values) {
                name += " [Total]";
            }
            reported_metrics[name] = ObjectRef(make_object<CountNode>(total));
        }

        if (!_collect_derived_metrics) {
            return reported_metrics;
        }

        // Collect metric results
        const auto metric_values = _perfmon_read_and_get_metrics();
        for (const auto& name_result : metric_values) {
            std::string metric_name = name_result.first;
            std::vector<double> metric_values = name_result.second;
            double total = 0;
            for (std::size_t thread_id{}; thread_id < metric_values.size(); ++thread_id) {
                std::string name = metric_name + " [Thread " + std::to_string(thread_id) + "]";
                double count = metric_values[thread_id];
                total += count;
                if (!_collect_thread_values) {
                    continue;
                }
                reported_metrics[name] = ObjectRef(make_object<RatioNode>(count));
            }
            std::string name = metric_name;
            if (_collect_thread_values) {
                name += " [Total]";
            }
            reported_metrics[name] = ObjectRef(make_object<RatioNode>(total));
        }
        return reported_metrics;
    }

    /*! \brief Close connection to likwid-perfctr API.
    */
    ~LikwidMetricCollectorNode() final {
        likwid_markerClose();
    }

private:
    bool _collect_derived_metrics;
    bool _collect_thread_values;

public:
    static constexpr const char* _type_key = "runtime.profiling.LikwidMetricCollector";
    TVM_DECLARE_FINAL_OBJECT_INFO(LikwidMetricCollectorNode, MetricCollectorNode);
};

/*! Wrapper for `LikwidMetricCollectorNode`. */
class LikwidMetricCollector : public MetricCollector {
public:
    explicit LikwidMetricCollector(bool collect_derived_metrics, bool collect_thread_values) {
        data_ = make_object<LikwidMetricCollectorNode>(collect_derived_metrics, collect_thread_values);
    }
    TVM_DEFINE_MUTABLE_OBJECT_REF_METHODS(LikwidMetricCollector, MetricCollector, 
                                          LikwidMetricCollectorNode);
};


/*! \brief Construct a metric collector that uses the likwid-perfctr API to 
 * collect hardware counter data.
 *
 * \note Please make sure to run TVM through the likwid-perfctr wrapper 
 * application following the instructions given in the Likwid documentation!
 * 
 * \param collect_derived_metrics If this is true, also collect the derived 
 * metrics of the set event group instead of only the raw event counts.
 * \param collect_thread_values If this is true, also collect the event
 * counts of each known thread instead of only the total.
 */
MetricCollector CreateLikwidMetricCollector(bool collect_derived_metrics = false, bool collect_thread_values = false) {
    return LikwidMetricCollector(collect_derived_metrics, collect_thread_values);
}

TVM_REGISTER_OBJECT_TYPE(LikwidEventSetNode);
TVM_REGISTER_OBJECT_TYPE(LikwidMetricCollectorNode);

TVM_REGISTER_GLOBAL("runtime.profiling.LikwidMetricCollector")
    .set_body_typed([](bool collect_derived_metrics, bool collect_thread_values) {
        return LikwidMetricCollector(collect_derived_metrics, collect_thread_values);
    });

TVM_REGISTER_GLOBAL("runtime.rpc_likwid_profile_func").set_body_typed(
    rpc_likwid_profile_func
);

// ------------------------------------------------------------------------------------------------
// RPC Profiling
// ------------------------------------------------------------------------------------------------

/*! \brief Execute a profiling run of the given function using the provided vm. 
 *
 * \param vm_mod The `Module` containing the profiler vm to profile on.
 * \param func_name The name of the function to profile.
 * \param collect_derived_metrics If this is true, also collect the derived 
 * metrics of the set event group instead of only the raw event counts.
 * \param collect_thread_values If this is true, also collect the event
 * counts of each known thread instead of only the total.
 * \returns The serialized `Report` of the profiling run.
*/
std::string rpc_likwid_profile_func(Module vm_mod, std::string func_name, bool collect_derived_metrics, bool collect_thread_values) {
    LOG(INFO) << "Received profiling request for function " << func_name;
    auto profile_func = vm_mod.GetFunction("profile");
    Array<MetricCollector> collectors({
        CreateLikwidMetricCollector(collect_derived_metrics, collect_thread_values)
    });
    LOG(INFO) << "Beginning profiling...";
    Report report = profile_func(func_name, collectors);
    LOG(INFO) << "Done. Sending serialized report.";
    return std::string(report->AsJSON().c_str());
}

} // namespace likwid
} // namespace profiling
} // namespace runtime
} // namespace tvm