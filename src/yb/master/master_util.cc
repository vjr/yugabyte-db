// Copyright (c) YugaByte, Inc.
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

#include "yb/master/master_util.h"

#include <boost/container/stable_vector.hpp>

#include "yb/common/redis_constants_common.h"
#include "yb/common/wire_protocol.h"
#include "yb/consensus/metadata.pb.h"
#include "yb/master/master_defaults.h"
#include "yb/master/master.proxy.h"
#include "yb/util/logging.h"

namespace yb {
namespace master {

using master::GetMasterRegistrationRequestPB;
using master::GetMasterRegistrationResponsePB;
using master::MasterServiceProxy;

namespace {

struct GetMasterRegistrationData {
  GetMasterRegistrationRequestPB req;
  GetMasterRegistrationResponsePB resp;
  rpc::RpcController controller;
  MasterServiceProxy proxy;

  GetMasterRegistrationData(rpc::ProxyCache* proxy_cache, const HostPort& hp)
      : proxy(proxy_cache, hp) {}
};

} // namespace

Status GetMasterEntryForHosts(rpc::ProxyCache* proxy_cache,
                              const std::vector<HostPort>& hostports,
                              MonoDelta timeout,
                              ServerEntryPB* e) {
  CHECK(!hostports.empty());

  boost::container::stable_vector<GetMasterRegistrationData> datas;
  datas.reserve(hostports.size());
  std::atomic<GetMasterRegistrationData*> last_data{nullptr};
  CountDownLatch latch(hostports.size());
  for (size_t i = 0; i != hostports.size(); ++i) {
    datas.emplace_back(proxy_cache, hostports[i]);
    auto& data = datas.back();
    data.controller.set_timeout(timeout);
    data.proxy.GetMasterRegistrationAsync(
        data.req, &data.resp, &data.controller,
        [&data, &latch, &last_data] {
      last_data.store(&data, std::memory_order_release);
      latch.CountDown();
    });
  }

  latch.Wait();

  for (const auto& data : datas) {
    if (!data.controller.status().ok() || data.resp.has_error()) {
      continue;
    }
    e->mutable_instance_id()->CopyFrom(data.resp.instance_id());
    e->mutable_registration()->CopyFrom(data.resp.registration());
    e->set_role(data.resp.role());
    return Status::OK();
  }

  auto last_data_value = last_data.load(std::memory_order_acquire);
  if (last_data_value->controller.status().ok()) {
    return StatusFromPB(last_data_value->resp.error().status());
  } else {
    return last_data_value->controller.status();
  }
}

const HostPortPB& DesiredHostPort(const TSInfoPB& ts_info, const CloudInfoPB& from) {
  return DesiredHostPort(ts_info.broadcast_addresses(), ts_info.private_rpc_addresses(),
                         ts_info.cloud_info(), from);
}

void TakeRegistration(consensus::RaftPeerPB* source, TSInfoPB* dest) {
  dest->mutable_private_rpc_addresses()->Swap(source->mutable_last_known_private_addr());
  dest->mutable_broadcast_addresses()->Swap(source->mutable_last_known_broadcast_addr());
  dest->mutable_cloud_info()->Swap(source->mutable_cloud_info());
}

void CopyRegistration(const consensus::RaftPeerPB& source, TSInfoPB* dest) {
  *dest->mutable_private_rpc_addresses() = source.last_known_private_addr();
  *dest->mutable_broadcast_addresses() = source.last_known_broadcast_addr();
  *dest->mutable_cloud_info() = source.cloud_info();
}

void TakeRegistration(ServerRegistrationPB* source, TSInfoPB* dest) {
  dest->mutable_private_rpc_addresses()->Swap(source->mutable_private_rpc_addresses());
  dest->mutable_broadcast_addresses()->Swap(source->mutable_broadcast_addresses());
  dest->mutable_cloud_info()->Swap(source->mutable_cloud_info());
}

void CopyRegistration(const ServerRegistrationPB& source, TSInfoPB* dest) {
  dest->mutable_private_rpc_addresses()->CopyFrom(source.private_rpc_addresses());
  dest->mutable_broadcast_addresses()->CopyFrom(source.broadcast_addresses());
  dest->mutable_cloud_info()->CopyFrom(source.cloud_info());
}

bool IsSystemNamespace(const std::string& namespace_name) {
  return namespace_name == master::kSystemNamespaceName ||
      namespace_name == master::kSystemAuthNamespaceName ||
      namespace_name == master::kSystemDistributedNamespaceName ||
      namespace_name == master::kSystemSchemaNamespaceName ||
      namespace_name == master::kSystemTracesNamespaceName;
}

YQLDatabase GetDefaultDatabaseType(const std::string& keyspace_name) {
  return keyspace_name == common::kRedisKeyspaceName ? YQLDatabase::YQL_DATABASE_REDIS
                                                     : YQLDatabase::YQL_DATABASE_CQL;
}

YQLDatabase GetDatabaseTypeForTable(const TableType table_type) {
  switch (table_type) {
    case TableType::YQL_TABLE_TYPE:
      return YQLDatabase::YQL_DATABASE_CQL;
    case TableType::REDIS_TABLE_TYPE:
      return YQLDatabase::YQL_DATABASE_REDIS;
    case TableType::PGSQL_TABLE_TYPE:
      return YQLDatabase::YQL_DATABASE_PGSQL;
    case TableType::TRANSACTION_STATUS_TABLE_TYPE:
      // Transactions status table is created in "system" keyspace in CQL.
      return YQLDatabase::YQL_DATABASE_CQL;
  }
  return YQL_DATABASE_UNKNOWN;
}

TableType GetTableTypeForDatabase(const YQLDatabase database_type) {
  switch (database_type) {
    case YQLDatabase::YQL_DATABASE_CQL:
      return TableType::YQL_TABLE_TYPE;
    case YQLDatabase::YQL_DATABASE_REDIS:
      return TableType::REDIS_TABLE_TYPE;
    case YQLDatabase::YQL_DATABASE_PGSQL:
      return TableType::PGSQL_TABLE_TYPE;
    default:
      DCHECK_EQ(database_type, YQLDatabase::YQL_DATABASE_UNKNOWN);
      return TableType::DEFAULT_TABLE_TYPE;
  }
}

Result<bool> NamespaceMatchesIdentifier(
    const NamespaceId& namespace_id, YQLDatabase db_type, const NamespaceName& namespace_name,
    const NamespaceIdentifierPB& ns_identifier) {
  if (ns_identifier.has_id()) {
    return namespace_id == ns_identifier.id();
  }
  if (ns_identifier.has_database_type() && ns_identifier.database_type() != db_type) {
    return false;
  }
  if (ns_identifier.has_name()) {
    return namespace_name == ns_identifier.name();
  }
  return STATUS_FORMAT(
    InvalidArgument, "Wrong namespace identifier format: $0", ns_identifier);
}

Result<bool> TableMatchesIdentifier(
    const TableId& id, const SysTablesEntryPB& table, const TableIdentifierPB& table_identifier) {
  if (table_identifier.has_table_id()) {
    return id == table_identifier.table_id();
  }
  if (!table_identifier.table_name().empty() && table_identifier.table_name() != table.name()) {
    return false;
  }
  if (table_identifier.has_namespace_()) {
    return NamespaceMatchesIdentifier(
        table.namespace_id(), master::GetDatabaseTypeForTable(table.table_type()),
        table.namespace_name(), table_identifier.namespace_());
  }
  return STATUS_FORMAT(
    InvalidArgument, "Wrong table identifier format: $0", table_identifier);
}

} // namespace master
} // namespace yb
