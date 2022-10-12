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

constexpr const char* REGION_NAME = "LikwidMetricCollector";
constexpr const char* OVERFLOW_WARNING = "Detected overflow while reading performance counter, setting value to -1";

/*! \brief Object holding start values of collected metrics. */
struct LikwidEventSetNode : public Object {

    std::vector<double> start_values;
    Device dev;

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
     * \param devices Not used for now. 
     * \todo Add compatibility check!
    */
    explicit LikwidMetricCollectorNode(Array<DeviceWrapper> devices) {}

    /*! \brief Initialization call. Establish connection to likwid-perfctr API.
     *
     * \param devices Not used by this collector at the moment.
    */
    void Init(Array<DeviceWrapper> devices) override {
        likwid_markerInit();
        likwid_markerRegisterRegion(REGION_NAME);
        likwid_markerStartRegion(REGION_NAME);
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
        _read_event_counts(&nevents, events, &time, &count);
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
        int nevents = 20;
        double events[20];
        double time;
        int count;
        _read_event_counts(&nevents, events, &time, &count);
        std::vector<double> end_values(events, events + nevents * sizeof(double));
        int groupId = perfmon_getIdOfActiveGroup();
        std::unordered_map<String, ObjectRef> reported_metrics;
        for (int eventId{}; eventId < nevents; ++eventId) {
            double diff = end_values[eventId] - event_set_node->start_values[eventId];
            String eventName = String(perfmon_getEventName(groupId, eventId));
            if (diff < 0) {
                LOG(WARNING) << OVERFLOW_WARNING;
                reported_metrics[eventName] = ObjectRef(make_object<CountNode>(-1));
            } else {
                reported_metrics[eventName] = ObjectRef(make_object<CountNode>(diff));
            }
        }
        return reported_metrics;
    }

    /*! \brief Close marker region and remove connection to likwid-perfctr API.
    */
    ~LikwidMetricCollectorNode() final {
        int res = likwid_markerStopRegion(REGION_NAME);
        if (res < 0) {
            LOG(ERROR) << "Could not stop marker region! Error code: " << res;
        }
        likwid_markerClose();
    }

    /*! \brief Read the current event set's counters.
     *
     * \param nevents [in/out] The size of the `events` array. Will be set to 
     * the number of available metrics on return.
     * \param events [in/out] Array containing the collected event counts.
     * \param time [out] The elapsed time since the region was started.
     * \param count [out] The call count of the marker region.
    */
    void _read_event_counts(int* nevents, double* events, double* time, int* count) {
        int status = likwid_markerStopRegion(REGION_NAME);
        if (status < 0) {
            LOG(ERROR) << "Could not stop marker region! Error code: " << status;
        }
        likwid_markerGetRegion(REGION_NAME, nevents, events, time, count);
        if (nevents == 0) {
            LOG(WARNING) << "Event count is zero!";
        }
        status = likwid_markerStartRegion(REGION_NAME);
        if (status < 0) {
            LOG(ERROR) << "Could not start marker region! Error code: " << status;
        }
    }

    static constexpr const char* _type_key = "runtime.profiling.LikwidMetricCollector";
    TVM_DECLARE_FINAL_OBJECT_INFO(LikwidMetricCollectorNode, MetricCollectorNode);
};

/*! Wrapper for `LikwidMetricCollectorNode`. */
class LikwidMetricCollector : public MetricCollector {
public:
    explicit LikwidMetricCollector(Array<DeviceWrapper> devices) {
        data_ = make_object<LikwidMetricCollectorNode>(devices);
    }
    TVM_DEFINE_MUTABLE_OBJECT_REF_METHODS(LikwidMetricCollector, MetricCollector, 
                                          LikwidMetricCollectorNode);
};


MetricCollector CreateLikwidMetricCollector(Array<DeviceWrapper> devices) {
    return LikwidMetricCollector(devices);
}


TVM_REGISTER_OBJECT_TYPE(LikwidEventSetNode);
TVM_REGISTER_OBJECT_TYPE(LikwidMetricCollectorNode);


TVM_REGISTER_GLOBAL("runtime.profiling.LikwidMetricCollector")
    .set_body_typed([](Array<DeviceWrapper> devices) {
        return LikwidMetricCollector(devices);
    });


std::string rpc_likwid_profile_func(runtime::Module vm_mod, std::string func_name) {
    LOG(INFO) << "Received profiling request for function " << func_name;
    auto profile_func = vm_mod.GetFunction("profile");
    Array<profiling::MetricCollector> collectors({
        profiling::likwid::CreateLikwidMetricCollector(Array<profiling::DeviceWrapper>())
    });
    LOG(INFO) << "Beginning profiling...";
    profiling::Report report = profile_func(func_name, collectors);
    LOG(INFO) << "Done. Sending serialized report.";
    return std::string(report->AsJSON().c_str());
}

TVM_REGISTER_GLOBAL("runtime.rpc_likwid_profile_func").set_body_typed(
    rpc_likwid_profile_func
);


#undef LIKWID_REGION_NAME
#undef LIKWID_OVERFLOW_WARNING


} // namespace likwid
} // namespace profiling
} // namespace runtime
} // namespace tvm