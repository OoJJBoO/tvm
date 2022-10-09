if (USE_RPC_PROFILING)
  message(STATUS "Build Profiler that is usable over RPC")
  target_sources(tvm_runtime_objs PRIVATE src/runtime/contrib/rpc_profiling/rpc_profiling.cc)
endif()