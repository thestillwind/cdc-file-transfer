// Copyright 2022 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "asset_stream_manager/local_assets_stream_manager_service_impl.h"

#include <iomanip>

#include "absl/strings/str_format.h"
#include "absl/strings/str_split.h"
#include "asset_stream_manager/multi_session.h"
#include "asset_stream_manager/session_manager.h"
#include "common/grpc_status.h"
#include "common/log.h"
#include "common/path.h"
#include "common/process.h"
#include "common/sdk_util.h"
#include "common/status.h"
#include "manifest/manifest_updater.h"

namespace cdc_ft {
namespace {

// Parses |instance_name| of the form
// "organizations/{org-id}/projects/{proj-id}/pools/{pool-id}/gamelets/{gamelet-id}"
// into parts. The pool id is not returned.
bool ParseInstanceName(const std::string& instance_name,
                       std::string* instance_id, std::string* project_id,
                       std::string* organization_id) {
  std::string pool_id;
  std::vector<std::string> parts = absl::StrSplit(instance_name, '/');
  if (parts.size() != 10) return false;
  if (parts[0] != "organizations" || parts[1].empty()) return false;
  if (parts[2] != "projects" || parts[3].empty()) return false;
  if (parts[4] != "pools" || parts[5].empty()) return false;
  // Instance id is e.g.
  // edge/e-europe-west3-b/49d010c7be1845ac9a19a9033c64a460ces1
  if (parts[6] != "gamelets" || parts[7].empty() || parts[8].empty() ||
      parts[9].empty())
    return false;
  *organization_id = parts[1];
  *project_id = parts[3];
  *instance_id = absl::StrFormat("%s/%s/%s", parts[7], parts[8], parts[9]);
  return true;
}

// Parses |data| line by line for "|key|: value" and puts the first instance in
// |value| if present. Returns false if |data| does not contain "|key|: value".
// Trims whitespace.
bool ParseValue(const std::string& data, const std::string& key,
                std::string* value) {
  std::istringstream stream(data);

  std::string line;
  while (std::getline(stream, line)) {
    if (line.find(key + ":") == 0) {
      // Trim value.
      size_t start_pos = key.size() + 1;
      while (start_pos < line.size() && isspace(line[start_pos])) {
        start_pos++;
      }
      size_t end_pos = line.size();
      while (end_pos > start_pos && isspace(line[end_pos - 1])) {
        end_pos--;
      }
      *value = line.substr(start_pos, end_pos - start_pos);
      return true;
    }
  }
  return false;
}

// Why oh why?
std::string Quoted(const std::string& s) {
  std::ostringstream ss;
  ss << std::quoted(s);
  return ss.str();
}

}  // namespace

LocalAssetsStreamManagerServiceImpl::LocalAssetsStreamManagerServiceImpl(
    SessionManager* session_manager, ProcessFactory* process_factory,
    metrics::MetricsService* metrics_service)
    : session_manager_(session_manager),
      process_factory_(process_factory),
      metrics_service_(metrics_service) {}

LocalAssetsStreamManagerServiceImpl::~LocalAssetsStreamManagerServiceImpl() =
    default;

grpc::Status LocalAssetsStreamManagerServiceImpl::StartSession(
    grpc::ServerContext* /*context*/, const StartSessionRequest* request,
    StartSessionResponse* /*response*/) {
  LOG_INFO("RPC:StartSession(gamelet_name='%s', workstation_directory='%s'",
           request->gamelet_name(), request->workstation_directory());

  metrics::DeveloperLogEvent evt;
  evt.as_manager_data = std::make_unique<metrics::AssetStreamingManagerData>();
  evt.as_manager_data->session_start_data =
      std::make_unique<metrics::SessionStartData>();
  evt.as_manager_data->session_start_data->absl_status = absl::StatusCode::kOk;
  evt.as_manager_data->session_start_data->status =
      metrics::SessionStartStatus::kOk;
  evt.as_manager_data->session_start_data->origin =
      ConvertOrigin(request->origin());

  // Parse instance/project/org id.
  absl::Status status;
  MultiSession* ms = nullptr;
  std::string instance_id, project_id, organization_id, instance_ip;
  uint16_t instance_port = 0;
  if (!ParseInstanceName(request->gamelet_name(), &instance_id, &project_id,
                         &organization_id)) {
    status = absl::InvalidArgumentError(absl::StrFormat(
        "Failed to parse instance name '%s'", request->gamelet_name()));
  } else {
    evt.project_id = project_id;
    evt.organization_id = organization_id;

    status = InitSsh(instance_id, project_id, organization_id, &instance_ip,
                     &instance_port);

    if (status.ok()) {
      status = session_manager_->StartSession(
          instance_id, project_id, organization_id, instance_ip, instance_port,
          request->workstation_directory(), &ms,
          &evt.as_manager_data->session_start_data->status);
    }
  }

  evt.as_manager_data->session_start_data->absl_status = status.code();
  if (ms) {
    evt.as_manager_data->session_start_data->concurrent_session_count =
        ms->GetSessionCount();
    if (!instance_id.empty() && ms->HasSessionForInstance(instance_id)) {
      ms->RecordSessionEvent(std::move(evt), metrics::EventType::kSessionStart,
                             instance_id);
    } else {
      ms->RecordMultiSessionEvent(std::move(evt),
                                  metrics::EventType::kSessionStart);
    }
  } else {
    metrics_service_->RecordEvent(std::move(evt),
                                  metrics::EventType::kSessionStart);
  }

  if (status.ok()) {
    LOG_INFO("StartSession() succeeded");
  } else {
    LOG_ERROR("StartSession() failed: %s", status.ToString());
  }
  return ToGrpcStatus(status);
}

grpc::Status LocalAssetsStreamManagerServiceImpl::StopSession(
    grpc::ServerContext* /*context*/, const StopSessionRequest* request,
    StopSessionResponse* /*response*/) {
  LOG_INFO("RPC:StopSession(gamelet_id='%s')", request->gamelet_id());

  absl::Status status = session_manager_->StopSession(request->gamelet_id());
  if (status.ok()) {
    LOG_INFO("StopSession() succeeded");
  } else {
    LOG_ERROR("StopSession() failed: %s", status.ToString());
  }
  return ToGrpcStatus(status);
}

metrics::RequestOrigin LocalAssetsStreamManagerServiceImpl::ConvertOrigin(
    StartSessionRequestOrigin origin) const {
  switch (origin) {
    case StartSessionRequest::ORIGIN_UNKNOWN:
      return metrics::RequestOrigin::kUnknown;
    case StartSessionRequest::ORIGIN_CLI:
      return metrics::RequestOrigin::kCli;
    case StartSessionRequest::ORIGIN_PARTNER_PORTAL:
      return metrics::RequestOrigin::kPartnerPortal;
    default:
      return metrics::RequestOrigin::kUnknown;
  }
}

absl::Status LocalAssetsStreamManagerServiceImpl::InitSsh(
    const std::string& instance_id, const std::string& project_id,
    const std::string& organization_id, std::string* instance_ip,
    uint16_t* instance_port) {
  SdkUtil sdk_util;
  instance_ip->clear();
  *instance_port = 0;

  ProcessStartInfo start_info;
  start_info.command = absl::StrFormat(
      "%s ssh init", path::Join(sdk_util.GetDevBinPath(), "ggp"));
  start_info.command += absl::StrFormat(" --instance %s", Quoted(instance_id));
  if (!project_id.empty()) {
    start_info.command += absl::StrFormat(" --project %s", Quoted(project_id));
  }
  if (!organization_id.empty()) {
    start_info.command +=
        absl::StrFormat(" --organization %s", Quoted(organization_id));
  }
  start_info.name = "ggp ssh init";

  std::string output;
  start_info.stdout_handler = [&output, this](const char* data,
                                              size_t data_size) {
    // Note: This is called from a background thread!
    output.append(data, data_size);
    return absl::OkStatus();
  };
  start_info.forward_output_to_log = true;

  std::unique_ptr<Process> process = process_factory_->Create(start_info);
  absl::Status status = process->Start();
  if (!status.ok()) {
    return WrapStatus(status, "Failed to start ggp process");
  }

  status = process->RunUntilExit();
  if (!status.ok()) {
    return WrapStatus(status, "Failed to run ggp process");
  }

  uint32_t exit_code = process->ExitCode();
  if (exit_code != 0) {
    return MakeStatus("ggp process exited with code %u", exit_code);
  }

  // Parse gamelet IP. Should be "Host: <instance_ip ip>".
  if (!ParseValue(output, "Host", instance_ip)) {
    return MakeStatus("Failed to parse host from ggp ssh init response\n%s",
                      output);
  }

  // Parse ssh port. Should be "Port: <port>".
  std::string port_string;
  const bool result = ParseValue(output, "Port", &port_string);
  int int_port = atoi(port_string.c_str());
  if (!result || int_port == 0 || int_port <= 0 || int_port > UINT_MAX) {
    return MakeStatus("Failed to parse ssh port from ggp ssh init response\n%s",
                      output);
  }

  *instance_port = static_cast<uint16_t>(int_port);
  return absl::OkStatus();
}

}  // namespace cdc_ft