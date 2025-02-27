// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/tserver/mini_tablet_server.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <utility>

#include <glog/logging.h>

#include "yb/gutil/macros.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/common/schema.h"
#include "yb/consensus/log.pb.h"
#include "yb/consensus/consensus.pb.h"

#include "yb/rpc/messenger.h"

#include "yb/server/metadata.h"
#include "yb/server/rpc_server.h"

#include "yb/tablet/tablet.h"
#include "yb/common/common.pb.h"
#include "yb/tablet/tablet-harness.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/ts_tablet_manager.h"

#include "yb/rocksutil/rocksdb_encrypted_file_factory.h"

#include "yb/util/encrypted_file_factory.h"
#include "yb/util/flag_tags.h"
#include "yb/util/header_manager_impl.h"
#include "yb/util/net/sockaddr.h"
#include "yb/util/net/tunnel.h"
#include "yb/util/scope_exit.h"
#include "yb/util/status.h"
#include "yb/util/universe_key_manager.h"

using std::pair;

using yb::consensus::Consensus;
using yb::consensus::ConsensusOptions;
using yb::consensus::RaftPeerPB;
using yb::consensus::RaftConfigPB;
using yb::log::Log;
using yb::log::LogOptions;
using strings::Substitute;
using yb::tablet::TabletPeer;

DECLARE_bool(rpc_server_allow_ephemeral_ports);
DECLARE_double(leader_failure_max_missed_heartbeat_periods);
DECLARE_int32(TEST_nodes_per_cloud);

DEFINE_test_flag(bool, private_broadcast_address, false,
                 "Use private address for broadcast address in tests.");

namespace yb {
namespace tserver {

MiniTabletServer::MiniTabletServer(const std::vector<std::string>& wal_paths,
                                   const std::vector<std::string>& data_paths,
                                   uint16_t rpc_port,
                                   const TabletServerOptions& extra_opts, int index)
  : started_(false),
    opts_(extra_opts),
    index_(index + 1),
    universe_key_manager_(new UniverseKeyManager()),
    encrypted_env_(NewEncryptedEnv(DefaultHeaderManager(universe_key_manager_.get()))),
    rocksdb_encrypted_env_(
      NewRocksDBEncryptedEnv(DefaultHeaderManager(universe_key_manager_.get()))) {

  // Start RPC server on loopback.
  FLAGS_rpc_server_allow_ephemeral_ports = true;
  opts_.rpc_opts.rpc_bind_addresses = server::TEST_RpcBindEndpoint(index_, rpc_port);
  // A.B.C.D.xip.io resolves to A.B.C.D so it is very useful for testing.
  opts_.broadcast_addresses = {
    HostPort(server::TEST_RpcAddress(index_,
                                     server::Private(FLAGS_TEST_private_broadcast_address)),
    rpc_port) };
  opts_.webserver_opts.port = 0;
  opts_.webserver_opts.bind_interface = opts_.broadcast_addresses.front().host();
  if (!opts_.has_placement_cloud()) {
    opts_.SetPlacement(Format("cloud$0", (index_ + 1) / FLAGS_TEST_nodes_per_cloud),
                       Format("rack$0", index_), "zone");
  }
  opts_.fs_opts.wal_paths = wal_paths;
  opts_.fs_opts.data_paths = data_paths;
  opts_.universe_key_manager = universe_key_manager_.get();
  opts_.env = encrypted_env_.get();
  opts_.rocksdb_env = rocksdb_encrypted_env_.get();
}

MiniTabletServer::MiniTabletServer(const string& fs_root,
                                   uint16_t rpc_port,
                                   const TabletServerOptions& extra_opts,
                                   int index)
  : MiniTabletServer({ fs_root }, { fs_root }, rpc_port, extra_opts, index) {
}

MiniTabletServer::~MiniTabletServer() {
}

Result<std::unique_ptr<MiniTabletServer>> MiniTabletServer::CreateMiniTabletServer(
    const string& fs_root, uint16_t rpc_port, int index) {
  auto options_result = TabletServerOptions::CreateTabletServerOptions();
  RETURN_NOT_OK(options_result);
  return std::make_unique<MiniTabletServer>(fs_root, rpc_port, *options_result, index);
}

Status MiniTabletServer::Start() {
  CHECK(!started_);

  std::unique_ptr<TabletServer> server(new enterprise::TabletServer(opts_));
  RETURN_NOT_OK(server->Init());

  RETURN_NOT_OK(server->Start());

  server_.swap(server);

  RETURN_NOT_OK(Reconnect());

  started_ = true;
  return Status::OK();
}

void MiniTabletServer::Isolate() {
  server::TEST_Isolate(server_->messenger());
  tunnel_->Shutdown();
}

Status MiniTabletServer::Reconnect() {
  server::TEST_SetupConnectivity(server_->messenger(), index_);

  if (FLAGS_TEST_private_broadcast_address) {
    return Status::OK();
  }

  tunnel_ = std::make_unique<Tunnel>(&server_->messenger()->io_service());
  auto started_tunnel = false;
  auto se = ScopeExit([this, &started_tunnel] {
    if (!started_tunnel) {
      tunnel_->Shutdown();
    }
  });

  std::vector<Endpoint> local;
  RETURN_NOT_OK(opts_.broadcast_addresses[0].ResolveAddresses(&local));
  Endpoint remote = VERIFY_RESULT(ParseEndpoint(opts_.rpc_opts.rpc_bind_addresses, 0));
  RETURN_NOT_OK(tunnel_->Start(
      local.front(), remote, [messenger = server_->messenger()](const IpAddress& address) {
    return !messenger->TEST_ShouldArtificiallyRejectIncomingCallsFrom(address);
  }));
  started_tunnel = true;
  return Status::OK();
}

Status MiniTabletServer::WaitStarted() {
  return server_->WaitInited();
}

void MiniTabletServer::Shutdown() {
  if (tunnel_) {
    tunnel_->Shutdown();
  }
  if (started_) {
    // Save bind address and port so we can later restart the server.
    opts_.rpc_opts.rpc_bind_addresses = server::TEST_RpcBindEndpoint(
        index_, bound_rpc_addr().port());
    opts_.webserver_opts.port = bound_http_addr().port();
    server_->Shutdown();
    tunnel_.reset();
    server_.reset();
  }
  started_ = false;
}

namespace {

CHECKED_STATUS ForAllTablets(
    MiniTabletServer* mts,
    std::function<Status(TabletPeer* tablet_peer)> action) {
  if (!mts->server()) {
    return STATUS(IllegalState, "Server is not running");
  }
  auto tablets = mts->server()->tablet_manager()->GetTabletPeers();
  for (const auto& tablet : tablets) {
    RETURN_NOT_OK(action(tablet.get()));
  }
  return Status::OK();
}

}  // namespace

Status MiniTabletServer::FlushTablets(tablet::FlushMode mode, tablet::FlushFlags flags) {
  if (!server_) {
    return Status::OK();
  }
  return ForAllTablets(this, [mode, flags](TabletPeer* tablet_peer) -> Status {
    if (!tablet_peer->tablet()) {
      return Status::OK();
    }
    return tablet_peer->tablet()->Flush(mode, flags);
  });
}

Status MiniTabletServer::CompactTablets() {
  if (!server_) {
    return Status::OK();
  }
  return ForAllTablets(this, [](TabletPeer* tablet_peer) {
    if (tablet_peer->tablet()) {
      tablet_peer->tablet()->ForceRocksDBCompactInTest();
    }
    return Status::OK();
  });
}

Status MiniTabletServer::SwitchMemtables() {
  return ForAllTablets(this, [](TabletPeer* tablet_peer) {
    return tablet_peer->tablet()->TEST_SwitchMemtable();
  });
}

Status MiniTabletServer::CleanTabletLogs() {
  if (!server_) {
    // Nothing to clean.
    return Status::OK();
  }
  return ForAllTablets(this, [](TabletPeer* tablet_peer) {
    return tablet_peer->RunLogGC();
  });
}

Status MiniTabletServer::Restart() {
  CHECK(started_);
  Shutdown();
  return Start();
}

Status MiniTabletServer::RestartStoppedServer() {
  Shutdown();
  return Start();
}

RaftConfigPB MiniTabletServer::CreateLocalConfig() const {
  CHECK(started_) << "Must Start()";
  RaftConfigPB config;
  RaftPeerPB* peer = config.add_peers();
  peer->set_permanent_uuid(server_->instance_pb().permanent_uuid());
  peer->set_member_type(RaftPeerPB::VOTER);
  auto host_port = peer->mutable_last_known_private_addr()->Add();
  host_port->set_host(bound_rpc_addr().address().to_string());
  host_port->set_port(bound_rpc_addr().port());
  return config;
}

Status MiniTabletServer::AddTestTablet(const std::string& ns_id,
                                       const std::string& table_id,
                                       const std::string& tablet_id,
                                       const Schema& schema,
                                       TableType table_type) {
  return AddTestTablet(ns_id, table_id, tablet_id, schema, CreateLocalConfig(), table_type);
}

Status MiniTabletServer::AddTestTablet(const std::string& ns_id,
                                       const std::string& table_id,
                                       const std::string& tablet_id,
                                       const Schema& schema,
                                       const RaftConfigPB& config,
                                       TableType table_type) {
  CHECK(started_) << "Must Start()";
  Schema schema_with_ids = SchemaBuilder(schema).Build();
  pair<PartitionSchema, Partition> partition = tablet::CreateDefaultPartition(schema_with_ids);

  auto table_info = std::make_shared<tablet::TableInfo>(
      table_id, ns_id, table_id, table_type, schema_with_ids, IndexMap(),
      boost::none /* index_info */, 0 /* schema_version */, partition.first);

  return ResultToStatus(server_->tablet_manager()->CreateNewTablet(
      table_info, tablet_id, partition.second, config));
}

void MiniTabletServer::FailHeartbeats(bool fail_heartbeats_for_tests) {
  server_->set_fail_heartbeats_for_tests(fail_heartbeats_for_tests);
}

Endpoint MiniTabletServer::bound_rpc_addr() const {
  CHECK(started_);
  return server_->first_rpc_address();
}

Endpoint MiniTabletServer::bound_http_addr() const {
  CHECK(started_);
  return server_->first_http_address();
}

} // namespace tserver
} // namespace yb
