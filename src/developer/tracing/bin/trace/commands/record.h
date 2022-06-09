// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_TRACING_BIN_TRACE_COMMANDS_RECORD_H_
#define SRC_DEVELOPER_TRACING_BIN_TRACE_COMMANDS_RECORD_H_

#include <fuchsia/sys/cpp/fidl.h>
#include <lib/async/cpp/wait.h>
#include <lib/zx/process.h>
#include <lib/zx/time.h>

#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "src/developer/tracing/bin/trace/cmd_utils.h"
#include "src/developer/tracing/bin/trace/command.h"
#include "src/developer/tracing/bin/trace/tracer.h"
#include "src/developer/tracing/bin/trace/utils.h"
#include "src/developer/tracing/lib/trace_converters/chromium_exporter.h"
#include "src/lib/fxl/memory/weak_ptr.h"

namespace tracing {

class RecordCommand : public CommandWithController {
 public:
  struct Options {
    bool Setup(const fxl::CommandLine&);

    std::string test_name;
    std::string app;
    std::vector<std::string> args;
    std::vector<std::string> categories = {};
    zx::duration duration = zx::sec(kDefaultDurationSeconds);
    bool detach = false;
    bool decouple = false;
    bool spawn = false;
    bool return_child_result = true;
    std::optional<std::string> environment_name;
    uint32_t buffer_size_megabytes = kDefaultBufferSizeMegabytes;
    std::vector<ProviderSpec> provider_specs;
    controller::BufferingMode buffering_mode = kDefaultBufferingMode;
    bool binary = false;
    bool compress = false;
    std::string output_file_name = kDefaultOutputFileName;
    std::string benchmark_results_file;
    std::string test_suite;
    std::unordered_map<std::string, Action> trigger_specs;
  };

  static Info Describe();

  explicit RecordCommand(sys::ComponentContext* context);

 protected:
  void Start(const fxl::CommandLine& command_line) override;

 private:
  void TerminateTrace(int32_t return_code);
  void DoneTrace();
  void LaunchComponentApp();
  void LaunchSpawnedApp();
  void StartTimer();
  void OnSpawnedAppExit(async_dispatcher_t* dispatcher, async::WaitBase* wait, zx_status_t status,
                        const zx_packet_signal_t* signal);
  void KillSpawnedApp();
  void OnAlert(std::string alert_name);

  async_dispatcher_t* dispatcher_;
  fuchsia::sys::ComponentControllerPtr component_controller_;
  fuchsia::sys::EnvironmentControllerPtr environment_controller_;
  zx::process spawned_app_;
  async::WaitMethod<RecordCommand, &RecordCommand::OnSpawnedAppExit> wait_spawned_app_;

  std::unique_ptr<std::ostream> binary_out_;
  // TODO(fxbug.dev/22974): Remove |exporter_|.
  std::unique_ptr<ChromiumExporter> exporter_;

  std::unique_ptr<Tracer> tracer_;

  bool tracing_ = false;
  int32_t return_code_ = 0;
  Options options_;

  fxl::WeakPtrFactory<RecordCommand> weak_ptr_factory_;
};

}  // namespace tracing

#endif  // SRC_DEVELOPER_TRACING_BIN_TRACE_COMMANDS_RECORD_H_