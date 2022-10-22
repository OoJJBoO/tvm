#include <likwid.h>
#include <tvm/runtime/contrib/likwid.h>

#include <string>
#include <vector>

#ifdef LIKWID_PERFMON
#include <likwid-marker.h>
#else
#define LIKWID_MARKER_INIT
#define LIKWID_MARKER_THREADINIT
#define LIKWID_MARKER_SWITCH
#define LIKWID_MARKER_REGISTER(regionTag)
#define LIKWID_MARKER_START(regionTag)
#define LIKWID_MARKER_STOP(regionTag)
#define LIKWID_MARKER_CLOSE
#define LIKWID_MARKER_GET(regionTag, nevents, events, time, count)
#endif

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

// ------------------------------------------------------------------------------------------------
// Likwid MetricCollector
// ------------------------------------------------------------------------------------------------

/*! \brief Object holding start values of collected metrics. */
struct LikwidEventSetNode : public Object {
    std::vector<double> start_values;
    Device dev;

    /*! \brief Construct a new event set node.
     *
     * \param start_values The event values at the time of creating this node.
     * \param dev The device this node is created for.
    */
    explicit LikwidEventSetNode(std::vector<double> start_values, Device dev) 
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
     * \todo Add compatibility check!
    */
    explicit LikwidMetricCollectorNode(bool collect_raw_events)
    : _collect_raw_events(collect_raw_events) {}

    /*! \brief Initialization call. Establish connection to likwid-perfctr API.
     *
     * \param devices Not used by this collector at the moment.
    */
    void Init(Array<DeviceWrapper> devices) override {
        likwid_markerInit();
        _marker_register_region();
        _marker_start_region();
    }

    /*! \brief Start marker region and begin collecting data.
     *
     * \param device Not used by this collector at the moment.
     * \returns A `LikwidEventSetNode` containing the values read at the start 
     * of the call. Used by the next `Stop` call to determine difference.
    */
    ObjectRef Start(Device device) override {
        likwid_markerThreadInit();
        int nevents = 20;
        double events[20];
        double time;
        int count;
        _marker_read_event_counts(&nevents, events, &time, &count);
        std::vector<double> start_values(events, events + nevents * sizeof(double));
        return ObjectRef(make_object<LikwidEventSetNode>(start_values, device));
    }

    /*! \brief Stop marker region and end data collection.
     *
     * \param object The previously created `LikwidEventSetNode`.
     * \returns A mapping from the names of the collected metrics to their 
     * corresponding values.
    */
    Map<String, ObjectRef> Stop(ObjectRef object) override {
        const LikwidEventSetNode* event_set_node = object.as<LikwidEventSetNode>();

        // Collect raw events if set to do so
        int nevents = 20;
        double events[20];
        double time;
        int count;
        _marker_read_event_counts(&nevents, events, &time, &count);
        std::vector<double> end_values(events, events + nevents * sizeof(double));
        int group_id = perfmon_getIdOfActiveGroup();
        std::unordered_map<String, ObjectRef> reported_metrics;
        for (int eventId{}; _collect_raw_events && eventId < nevents; ++eventId) {
            double diff = end_values[eventId] - event_set_node->start_values[eventId];
            String event_name = String(perfmon_getEventName(group_id, eventId));
            if (diff < 0) {
                LOG(WARNING) << OVERFLOW_WARNING;
                reported_metrics[event_name] = ObjectRef(make_object<CountNode>(-1));
            } else {
                reported_metrics[event_name] = ObjectRef(make_object<CountNode>(diff));
            }
        }

        // Collect metrics of active group for each thread known to perfmon
        int number_of_threads = perfmon_getNumberOfThreads();
        if (number_of_threads <= 0) {
            LOG(ERROR) << THREAD_COUNT_ERROR;
            return reported_metrics;
        }
        int number_of_metrics = perfmon_getNumberOfMetrics(group_id);
        if (!_collect_raw_events && number_of_metrics == 0) {
            LOG(WARNING) << NO_METRICS_WARNING;
            return reported_metrics;
        }
        for (int metric_id{}; metric_id < number_of_metrics; ++metric_id) {
            for (int thread_id{}; thread_id < number_of_threads; ++thread_id) {
                std::string metric_name = perfmon_getMetricName(group_id, metric_id);
                metric_name += std::string(" [Thread ");
                metric_name += std::to_string(thread_id);
                metric_name += std::string("]");
                double result = perfmon_getMetric(group_id, metric_id, thread_id);
                reported_metrics[metric_name] = ObjectRef(make_object<CountNode>(result));
            }
        }
        return reported_metrics;
    }

    /*! \brief Close marker region and remove connection to likwid-perfctr API.
    */
    ~LikwidMetricCollectorNode() final {
        _marker_stop_region();
        likwid_markerClose();
    }

private:
    bool _collect_raw_events;

public:
    static constexpr const char* _type_key = "runtime.profiling.LikwidMetricCollector";
    TVM_DECLARE_FINAL_OBJECT_INFO(LikwidMetricCollectorNode, MetricCollectorNode);
};

/*! Wrapper for `LikwidMetricCollectorNode`. */
class LikwidMetricCollector : public MetricCollector {
public:
    explicit LikwidMetricCollector(bool doCollectRawEvents) {
        data_ = make_object<LikwidMetricCollectorNode>(doCollectRawEvents);
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
 * \param collect_raw_events If this is true, also collect the raw events used 
 * in the set event group instead of only the derived metrics.
 */
MetricCollector CreateLikwidMetricCollector(bool collect_raw_events = false) {
    return LikwidMetricCollector(collect_raw_events);
}

TVM_REGISTER_OBJECT_TYPE(LikwidEventSetNode);
TVM_REGISTER_OBJECT_TYPE(LikwidMetricCollectorNode);

TVM_REGISTER_GLOBAL("runtime.profiling.LikwidMetricCollector")
    .set_body_typed([](bool collect_raw_events) {
        return LikwidMetricCollector(collect_raw_events);
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
 * \param collect_raw_events If this is true, also collect the raw events used 
 * in the set event group instead of only the derived metrics.
 * \returns The serialized `Report` of the profiling run.
*/
std::string rpc_likwid_profile_func(Module vm_mod, std::string func_name, bool collect_raw_events) {
    LOG(INFO) << "Received profiling request for function " << func_name;
    auto profile_func = vm_mod.GetFunction("profile");
    Array<MetricCollector> collectors({
        CreateLikwidMetricCollector(collect_raw_events)
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