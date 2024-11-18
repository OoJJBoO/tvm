#include <likwid.h>
#include <stdio.h>
#include <tvm/runtime/contrib/likwid.h>

#include <string>
#include <vector>

namespace tvm {
namespace runtime {
namespace profiling {
namespace likwid {

// TODO: Add pinning functionality!

// -------------------------------------------------------------------------------------------------
// Event Set Node
// -------------------------------------------------------------------------------------------------

struct LikwidCounterStateNode : public Object {
 public:
  std::unordered_map<std::string, std::vector<double>> data;
  Device device;

  explicit LikwidCounterStateNode(std::unordered_map<std::string, std::vector<double>> data,
                                  Device dev)
      : data(data), device(dev) {}

  static constexpr const char* _type_key = "LikwidCounterStateNode";

  TVM_DECLARE_FINAL_OBJECT_INFO(LikwidCounterStateNode, Object);
};

TVM_REGISTER_OBJECT_TYPE(LikwidCounterStateNode);

// -------------------------------------------------------------------------------------------------
// Collector configuration
// -------------------------------------------------------------------------------------------------

/// @brief Data class defining the setup parameters for constructing a LikwidMetricCollector.
struct LikwidMetricCollectorConfigurationNode : public Object {
  Array<String> event_strings;
  bool report_events;
  bool report_metrics;
  bool report_per_thread;

 public:
  /// @brief Construct a new LikwidMetricCollectorConfigurationNode.
  /// @param event_strings The event strings to use for LIKWID setup.
  /// @param report_events If true, report raw event counts.
  /// @param report_metrics If true, report derived metrics defined by the group (if any).
  /// @param report_per_thread If true, report event counts and metrics per thread in the form
  /// [NAME]_[THREAD_ID]. Otherwise, only total values are reported.
  LikwidMetricCollectorConfigurationNode(Array<String> event_strings, bool report_events,
                                         bool report_metrics, bool report_per_thread)
      : event_strings(event_strings),
        report_events(report_events),
        report_metrics(report_metrics),
        report_per_thread(report_per_thread) {}

  static constexpr const char* _type_key = "runtime.profiling.LikwidMetricCollectorConfiguration";

  TVM_DECLARE_FINAL_OBJECT_INFO(LikwidMetricCollectorConfigurationNode, Object);
};

TVM_REGISTER_OBJECT_TYPE(LikwidMetricCollectorConfigurationNode);

/// @brief Container for a LikwidMetricCollectorConfigurationNode.
struct LikwidMetricCollectorConfiguration : public ObjectRef {
  /// @brief Construct a new LikwidMetricCollectorConfiguration.
  /// @param event_strings The event strings to use for LIKWID setup.
  /// @param report_events If true, report raw event counts.
  /// @param report_metrics If true, report derived metrics defined by the group (if any).
  /// @param report_per_thread If true, report event counts and metrics per thread in the form
  /// [NAME]_[THREAD_ID]. Otherwise, only total values are reported.
  explicit LikwidMetricCollectorConfiguration(Array<String> event_strings, bool report_events,
                                              bool report_metrics, bool report_per_thread) {
    data_ = make_object<LikwidMetricCollectorConfigurationNode>(event_strings, report_events,
                                                                report_metrics, report_per_thread);
  }

  TVM_DEFINE_MUTABLE_OBJECT_REF_METHODS(LikwidMetricCollectorConfiguration, ObjectRef,
                                        LikwidMetricCollectorConfigurationNode);
};

TVM_REGISTER_GLOBAL("runtime.profiling.LikwidMetricCollectorConfiguration")
    .set_body_typed([](Array<String> event_string, bool report_events, bool report_metrics,
                       bool report_per_thread) {
      return LikwidMetricCollectorConfiguration(event_string, report_events, report_metrics,
                                                report_per_thread);
    });

// -------------------------------------------------------------------------------------------------
// Likwid Metric Collector Node
// -------------------------------------------------------------------------------------------------

/// @brief A metric collector implementation that uses the LIKWID API to read hardware counters.
struct LikwidMetricCollectorNode final : public MetricCollectorNode {
  /// @brief Construct a new LikwidMetricCollectorNode.
  /// @param config The configuration to use for LIKWID setup and metric reporting.
  explicit LikwidMetricCollectorNode(LikwidMetricCollectorConfiguration config)
      : _config(config.as<LikwidMetricCollectorConfigurationNode>()) {}

  void Init(Array<DeviceWrapper> devices) override {
    // Check if the user provided devices not supported by this collector and warn accordingly.
    if (devices.size() > 1 || devices[0]->device.device_type != kDLCPU) {
      LOG(WARNING) << "This collector only collects events on the primary CPU TVM is running on; "
                      "Other devices will be ignored.";
    }

    // Initialize LIKWID data structures.
    topology_init();
    affinity_init();

    // Get CPU information and initialize perfmon facilities.
    CpuTopology_t cpu_topology = get_cpuTopology();
    _n_threads = cpu_topology->numHWThreads;
    std::vector<int> cpus = std::vector<int>(_n_threads);
    for (int cpu_id = 0; cpu_id < _n_threads; ++cpu_id) {
      cpus[cpu_id] = cpu_topology->threadPool[cpu_id].apicId;
    }
    int init_status = perfmon_init(cpu_topology->numHWThreads, cpus.data());
    if (init_status < 0) {
      LOG(ERROR) << "Encountered an error while initializing perfmon module. Error code: "
                 << std::to_string(-init_status);
      WarnAndDisable();
      return;
    }

    // Get pre-defined group names of current architecture to check against later.
    char **group_names, **short_infos, **long_infos;
    int n_groups = perfmon_getGroups(&group_names, &short_infos, &long_infos);
    std::vector<std::string> group_strings(n_groups);
    for (int group_idx = 0; group_idx < n_groups; ++group_idx) {
      group_strings[group_idx] = group_names[group_idx];
    }
    perfmon_returnGroups(n_groups, group_names, short_infos, long_infos);

    // Construct full event string from the provided sub-strings.
    std::string event_set_string = "";
    int n_event_strings = _config->event_strings.size();
    for (int event_string_idx = 0; event_string_idx < n_event_strings; ++event_string_idx) {
      std::string substring = _config->event_strings[event_string_idx];
      if (substring.find(":") == std::string::npos) {
        auto group_name_iter = std::find(group_strings.begin(), group_strings.end(), substring);
        if (group_name_iter == std::end(group_strings)) {
          LOG(WARNING) << "Group \'" << substring << "\' does not exist for this architecture. "
                       << "Skipping it.";
          continue;
        }
      }
      event_set_string = event_set_string + substring;
      if (event_string_idx < n_event_strings - 1) {
        event_set_string = event_set_string + ",";
      }
    }

    // Check if the final event string is valid, register perfmon event set, and get group info.
    if (event_set_string.size() == 0) {
      LOG(ERROR) << "Final event set string is empty.";
      WarnAndDisable();
      return;
    }
    _group_id = perfmon_addEventSet(event_set_string.data());
    _n_events = perfmon_getNumberOfEvents(_group_id);
    _n_metrics = perfmon_getNumberOfMetrics(_group_id);

    // Setup and start counters.
    int counter_status = perfmon_setupCounters(_group_id);
    if (counter_status < 0) {
      LOG(ERROR) << "Encountered an error during counter setup. Error code: "
                 << std::to_string(-counter_status)
                 << ((counter_status == -1) ? " (counters could not be set up)"
                                            : " (invalid group id)");
      WarnAndDisable();
      return;
    }
    int counter_start_status = perfmon_startCounters();
    if (counter_start_status < 0) {
      LOG(ERROR) << "Encountered an error during counter startup on thread "
                 << std::to_string(-counter_start_status - 1) << ".";
      WarnAndDisable();
    }
  }

  ObjectRef Start(Device device) override {
    // Can not read any metrics if group id was not initialized.
    if (_group_id < 0) {
      return ObjectRef(nullptr);
    }
    return ObjectRef(make_object<LikwidCounterStateNode>(ReadCounterResults(), device));
  }

  Map<String, ObjectRef> Stop(ObjectRef object) override {
    // Can not read any metrics if group id was not initialized.
    if (_group_id < 0) {
      return std::unordered_map<String, ObjectRef>();
    }

    // Read group counters and get initial results from object reference.
    const auto current_counts = ReadCounterResults();
    const auto& node = *object.as<LikwidCounterStateNode>();
    const auto& initial_counts = node.data;

    // Collect event counts if desired.
    std::unordered_map<String, ObjectRef> count_nodes;
    if (_config->report_events) {
      for (const auto& iter : current_counts) {
        const std::string& event_name = iter.first;
        const std::vector<double>& initial_cnt = initial_counts.at(event_name);
        const std::vector<double>& current_cnt = current_counts.at(event_name);
        double cnt_total = 0;
        for (int thread_id = 0; thread_id < _n_threads; ++thread_id) {
          double cnt_difference = current_cnt.at(thread_id) - initial_cnt.at(thread_id);
          if (_config->report_per_thread) {
            std::string event_name_thread = event_name + "_" + std::to_string(thread_id);
            count_nodes[event_name_thread] = ObjectRef(make_object<CountNode>(cnt_difference));
          }
          cnt_total += cnt_difference;
        }
        count_nodes[event_name] = ObjectRef(make_object<CountNode>(cnt_total));
      }
    }

    // Collect metrics if desired.
    if (_config->report_metrics) {
      for (int metric_id = 0; metric_id < _n_metrics; ++metric_id) {
        std::string metric_name = perfmon_getMetricName(_group_id, metric_id);
        double metric_total = 0;
        for (int thread_id = 0; thread_id < _n_threads; ++thread_id) {
          double metric_result = perfmon_getMetric(_group_id, metric_id, thread_id);
          if (_config->report_per_thread) {
            std::string metric_name_thread = metric_name + "_" + std::to_string(thread_id);
            count_nodes[metric_name_thread] = ObjectRef(make_object<RatioNode>(metric_result));
          }
          metric_total += metric_result;
        }
        count_nodes[metric_name] = ObjectRef(make_object<RatioNode>(metric_total));
      }
    }

    return count_nodes;
  }

  ~LikwidMetricCollectorNode() final {
    // Counters were only started if the group id was set.
    if (_group_id >= 0) {
      int counter_stop_status = perfmon_stopCounters();
      if (counter_stop_status < 0) {
        LOG(ERROR) << "Encountered an error while stopping counters of thread "
                   << std::to_string(-counter_stop_status - 1) << ".";
      }
    }

    // Finalize and cleanup LIKWID data structures.
    perfmon_finalize();
    topology_finalize();
  }

 private:
  void WarnAndDisable() {
    LOG(WARNING) << "Metric collector could not be initialized. Disabling metric collection.";
    _group_id = -1;
  }

  std::unordered_map<std::string, std::vector<double>> ReadCounterResults() const {
    int read_status = perfmon_readGroupCounters(_group_id);
    if (read_status < 0) {
      LOG(ERROR) << "Encountered an error while reading group counters of thread "
                 << std::to_string(-read_status - 1) << ".";
      return std::unordered_map<std::string, std::vector<double>>();
    }
    std::unordered_map<std::string, std::vector<double>> counts;
    for (int event_id = 0; event_id < _n_events; ++event_id) {
      std::vector<double> thread_counts(_n_threads);
      for (int thread_id = 0; thread_id < _n_threads; ++thread_id) {
        thread_counts[thread_id] = perfmon_getResult(_group_id, event_id, thread_id);
      }
      std::string name = perfmon_getEventName(_group_id, event_id);
      counts[name] = thread_counts;
    }
    return counts;
  }

 private:
  const LikwidMetricCollectorConfigurationNode* _config = nullptr;
  int _group_id;
  int _n_threads;
  int _n_events;
  int _n_metrics;

 public:
  static constexpr const char* _type_key = "runtime.profiling.LikwidMetricCollector";
  TVM_DECLARE_FINAL_OBJECT_INFO(LikwidMetricCollectorNode, MetricCollectorNode);
};

/// @brief Wrapper for a LikwidMetricCollectorNode.
class LikwidMetricCollector : public MetricCollector {
 public:
  /// @brief Construct a new LikwidMetricCollector object.
  /// @param config The configuration to use for LIKWID setup and metric reporting.
  explicit LikwidMetricCollector(LikwidMetricCollectorConfiguration config) {
    data_ = make_object<LikwidMetricCollectorNode>(config);
  }

  TVM_DEFINE_MUTABLE_OBJECT_REF_METHODS(LikwidMetricCollector, MetricCollector,
                                        LikwidMetricCollectorNode);
};

TVM_REGISTER_OBJECT_TYPE(LikwidMetricCollectorNode);

TVM_REGISTER_GLOBAL("runtime.profiling.LikwidMetricCollector")
    .set_body_typed([](LikwidMetricCollectorConfiguration config) {
      return LikwidMetricCollector(config);
    });

// -------------------------------------------------------------------------------------------------
// RPC Profiling
// -------------------------------------------------------------------------------------------------

/// @brief Create a new metric collector that uses the LIKWID API for reading hardware counters.
/// @param config The configuration to use for LIKWID setup and metric reporting.
/// @return
MetricCollector CreateLikwidMetricCollector(LikwidMetricCollectorConfiguration config) {
  return LikwidMetricCollector(config);
}

/// @brief Profile a given VM module function remotely using performance counters read through the
/// LIKWID API. A new metric collector is constructed on the remote and reports are sent back after
/// the profiling run is completed.
/// @param vm_mod The remote VM module.
/// @param func_name The name of the function to profile.
/// @param config The configuration to use for creating the remote LIKWID metric collector.
/// @return
std::string rpc_likwid_profile_func(Module vm_mod, String func_name,
                                    LikwidMetricCollectorConfiguration config) {
  LOG(INFO) << "Received profiling request for function " << func_name;
  auto profile_func = vm_mod.GetFunction("profile");
  Array<MetricCollector> collectors({CreateLikwidMetricCollector(config)});
  LOG(INFO) << "Begin profiling...";
  Report report = profile_func(func_name, collectors);
  LOG(INFO) << "Done. Sending back serialized report.";
  return std::string(report->AsJSON().c_str());
}

TVM_REGISTER_GLOBAL("runtime.rpc_likwid_profile_func").set_body_typed(rpc_likwid_profile_func);

}  // namespace likwid
}  // namespace profiling
}  // namespace runtime
}  // namespace tvm