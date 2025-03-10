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
#include "yb/master/async_rpc_tasks.h"

#include "yb/common/wire_protocol.h"

#include "yb/consensus/consensus_meta.h"
#include "yb/consensus/consensus.proxy.h"

#include "yb/master/master.h"
#include "yb/master/ts_descriptor.h"
#include "yb/master/catalog_manager.h"

#include "yb/rpc/messenger.h"

#include "yb/tserver/tserver_admin.proxy.h"

#include "yb/util/flag_tags.h"
#include "yb/util/format.h"
#include "yb/util/logging.h"
#include "yb/util/thread_restrictions.h"

DEFINE_int32(unresponsive_ts_rpc_timeout_ms, 60 * 60 * 1000,  // 1 hour
             "After this amount of time (or after we have retried unresponsive_ts_rpc_retry_limit "
             "times, whichever happens first), the master will stop attempting to contact a tablet "
             "server in order to perform operations such as deleting a tablet.");
TAG_FLAG(unresponsive_ts_rpc_timeout_ms, advanced);

DEFINE_int32(unresponsive_ts_rpc_retry_limit, 20,
             "After this number of retries (or unresponsive_ts_rpc_timeout_ms expires, whichever "
             "happens first), the master will stop attempting to contact a tablet server in order "
             "to perform operations such as deleting a tablet.");
TAG_FLAG(unresponsive_ts_rpc_retry_limit, advanced);

DEFINE_test_flag(
    int32, slowdown_master_async_rpc_tasks_by_ms, 0,
    "For testing purposes, slow down the run method to take longer.");

// The flags are defined in catalog_manager.cc.
DECLARE_int32(master_ts_rpc_timeout_ms);
DECLARE_int32(tablet_creation_timeout_ms);

namespace yb {
namespace master {

using namespace std::placeholders;

using std::string;
using std::shared_ptr;

using strings::Substitute;
using consensus::RaftPeerPB;
using tserver::TabletServerErrorPB;

// ============================================================================
//  Class PickSpecificUUID.
// ============================================================================
Status PickSpecificUUID::PickReplica(TSDescriptor** ts_desc) {
  shared_ptr<TSDescriptor> ts;
  if (!master_->ts_manager()->LookupTSByUUID(ts_uuid_, &ts)) {
    return STATUS(NotFound, "unknown tablet server id", ts_uuid_);
  }
  *ts_desc = ts.get();
  return Status::OK();
}

string ReplicaMapToString(const TabletInfo::ReplicaMap& replicas) {
  string ret = "";
  for (const auto& r : replicas) {
    if (!ret.empty()) {
      ret += ", ";
    } else {
      ret += "(";
    }
    ret += r.second.ts_desc->permanent_uuid();
  }
  ret += ")";
  return ret;
}

// ============================================================================
//  Class PickLeaderReplica.
// ============================================================================
Status PickLeaderReplica::PickReplica(TSDescriptor** ts_desc) {
  TabletInfo::ReplicaMap replica_locations;
  tablet_->GetReplicaLocations(&replica_locations);
  for (const TabletInfo::ReplicaMap::value_type& r : replica_locations) {
    if (r.second.role == consensus::RaftPeerPB::LEADER) {
      *ts_desc = r.second.ts_desc;
      return Status::OK();
    }
  }
  return STATUS(NotFound, Substitute("No leader found for tablet $0 with $1 replicas : $2.",
                                     tablet_.get()->ToString(), replica_locations.size(),
                                     ReplicaMapToString(replica_locations)));
}

// ============================================================================
//  Class RetryingTSRpcTask.
// ============================================================================

RetryingTSRpcTask::RetryingTSRpcTask(Master *master,
                                     ThreadPool* callback_pool,
                                     gscoped_ptr<TSPicker> replica_picker,
                                     const scoped_refptr<TableInfo>& table)
  : master_(master),
    callback_pool_(callback_pool),
    replica_picker_(replica_picker.Pass()),
    table_(table),
    start_ts_(MonoTime::Now()),
    attempt_(0),
    state_(MonitoredTaskState::kWaiting) {
  deadline_ = start_ts_;
  deadline_.AddDelta(MonoDelta::FromMilliseconds(FLAGS_unresponsive_ts_rpc_timeout_ms));
}

// Send the subclass RPC request.
Status RetryingTSRpcTask::Run() {
  VLOG_WITH_PREFIX(1) << "Start Running";
  auto task_state = state();
  if (task_state == MonitoredTaskState::kAborted) {
    UnregisterAsyncTask();  // May delete this.
    return STATUS(IllegalState, "Unable to run task because it has been aborted");
  }
  DCHECK(task_state == MonitoredTaskState::kWaiting) << "State: " << ToString(task_state);

  const Status s = ResetTSProxy();
  if (!s.ok()) {
    if (PerformStateTransition(MonitoredTaskState::kWaiting, MonitoredTaskState::kFailed)) {
      UnregisterAsyncTask();  // May delete this.
      return s.CloneAndPrepend("Failed to reset TS proxy");
    } else if (state() == MonitoredTaskState::kAborted) {
      UnregisterAsyncTask();  // May delete this.
      return STATUS(IllegalState, "Unable to run task because it has been aborted");
    } else {
      LOG_WITH_PREFIX(FATAL) << "Failed to change task to MonitoredTaskState::kFailed state";
    }
  } else {
    rpc_.Reset();
  }

  // Calculate and set the timeout deadline.
  MonoTime timeout = MonoTime::Now();
  timeout.AddDelta(MonoDelta::FromMilliseconds(FLAGS_master_ts_rpc_timeout_ms));
  const MonoTime& deadline = MonoTime::Earliest(timeout, deadline_);
  rpc_.set_deadline(deadline);

  if (!PerformStateTransition(MonitoredTaskState::kWaiting, MonitoredTaskState::kRunning)) {
    if (state() == MonitoredTaskState::kAborted) {
      UnregisterAsyncTask();  // May delete this.
      return STATUS(Aborted, "Unable to run task because it has been aborted");
    } else {
      LOG_WITH_PREFIX(DFATAL) <<
          "Task transition MonitoredTaskState::kWaiting -> MonitoredTaskState::kRunning failed";
      return STATUS_FORMAT(IllegalState, "Task in invalid state $0", state());
    }
  }
  if (PREDICT_FALSE(FLAGS_slowdown_master_async_rpc_tasks_by_ms > 0)) {
    VLOG(1) << "Slowing down " << this->description() << " by "
            << FLAGS_slowdown_master_async_rpc_tasks_by_ms << " ms.";
    bool old_thread_restriction = ThreadRestrictions::SetWaitAllowed(true);
    SleepFor(MonoDelta::FromMilliseconds(FLAGS_slowdown_master_async_rpc_tasks_by_ms));
    ThreadRestrictions::SetWaitAllowed(old_thread_restriction);
    VLOG(2) << "Slowing down " << this->description() << " done. Resuming.";
  }
  if (!SendRequest(++attempt_)) {
    if (!RescheduleWithBackoffDelay()) {
      UnregisterAsyncTask();  // May call 'delete this'.
    }
  }
  return Status::OK();
}

// Abort this task and return its value before it was successfully aborted. If the task entered
// a different terminal state before we were able to abort it, return that state.
MonitoredTaskState RetryingTSRpcTask::AbortAndReturnPrevState() {
  auto prev_state = state();
  while (!IsStateTerminal(prev_state)) {
    auto expected = prev_state;
    if (state_.compare_exchange_weak(expected, MonitoredTaskState::kAborted)) {
      AbortIfScheduled();
      UnregisterAsyncTask();
      return prev_state;
    }
    prev_state = state();
  }
  UnregisterAsyncTask();
  return prev_state;
}

void RetryingTSRpcTask::AbortTask() {
  AbortAndReturnPrevState();
}

void RetryingTSRpcTask::RpcCallback() {
  // Defer the actual work of the callback off of the reactor thread.
  // This is necessary because our callbacks often do synchronous writes to
  // the catalog table, and we can't do synchronous IO on the reactor.
  CHECK_OK(callback_pool_->SubmitFunc(
      std::bind(&RetryingTSRpcTask::DoRpcCallback, shared_from(this))));
}

// Handle the actual work of the RPC callback. This is run on the master's worker
// pool, rather than a reactor thread, so it may do blocking IO operations.
void RetryingTSRpcTask::DoRpcCallback() {
  if (!rpc_.status().ok()) {
    LOG(WARNING) << "TS " << target_ts_desc_->permanent_uuid() << ": "
                 << type_name() << " RPC failed for tablet "
                 << tablet_id() << ": " << rpc_.status().ToString();
  } else if (state() != MonitoredTaskState::kAborted) {
    HandleResponse(attempt_);  // Modifies state_.
  }

  // Schedule a retry if the RPC call was not successful.
  if (RescheduleWithBackoffDelay()) {
    return;
  }

  UnregisterAsyncTask();  // May call 'delete this'.
}

bool RetryingTSRpcTask::RescheduleWithBackoffDelay() {
  auto task_state = state();
  if (task_state != MonitoredTaskState::kRunning) {
    if (task_state != MonitoredTaskState::kComplete) {
      LOG_WITH_PREFIX(INFO) << "No reschedule for this task";
    }
    return false;
  }

  if (RetryLimitTaskType() && attempt_ > FLAGS_unresponsive_ts_rpc_retry_limit) {
    LOG(WARNING) << "Reached maximum number of retries ("
                 << FLAGS_unresponsive_ts_rpc_retry_limit << ") for request " << description()
                 << ", task=" << this << " state=" << state();
    TransitionToTerminalState(MonitoredTaskState::kRunning, MonitoredTaskState::kFailed);
    return false;
  }

  MonoTime now = MonoTime::Now();
  // We assume it might take 10ms to process the request in the best case,
  // fail if we have less than that amount of time remaining.
  int64_t millis_remaining = deadline_.GetDeltaSince(now).ToMilliseconds() - 10;
  // Exponential backoff with jitter.
  int64_t base_delay_ms;
  if (attempt_ <= 12) {
    base_delay_ms = 1 << (attempt_ + 3);  // 1st retry delayed 2^4 ms, 2nd 2^5, etc.
  } else {
    base_delay_ms = 60 * 1000;  // cap at 1 minute
  }
  // Normal rand is seeded by default with 1. Using the same for rand_r seed.
  unsigned int seed = 1;
  int64_t jitter_ms = rand_r(&seed) % 50;  // Add up to 50ms of additional random delay.
  int64_t delay_millis = std::min<int64_t>(base_delay_ms + jitter_ms, millis_remaining);

  if (delay_millis <= 0) {
    LOG_WITH_PREFIX(WARNING) << "Request timed out";
    TransitionToTerminalState(MonitoredTaskState::kRunning, MonitoredTaskState::kFailed);
  } else {
    MonoTime new_start_time = now;
    new_start_time.AddDelta(MonoDelta::FromMilliseconds(delay_millis));
    LOG(INFO) << "Scheduling retry of " << description() << ", state="
              << state() << " with a delay of " << delay_millis
              << "ms (attempt = " << attempt_ << ")...";

    if (!PerformStateTransition(MonitoredTaskState::kRunning, MonitoredTaskState::kScheduling)) {
      LOG_WITH_PREFIX(WARNING) << "Unable to mark this task as MonitoredTaskState::kScheduling";
      return false;
    }
    auto task_id = master_->messenger()->ScheduleOnReactor(
        std::bind(&RetryingTSRpcTask::RunDelayedTask, shared_from(this), _1),
        MonoDelta::FromMilliseconds(delay_millis), SOURCE_LOCATION(), master_->messenger());
    reactor_task_id_.store(task_id, std::memory_order_release);

    if (task_id == rpc::kInvalidTaskId) {
      AbortTask();
      UnregisterAsyncTask();
      return false;
    }

    if (!PerformStateTransition(MonitoredTaskState::kScheduling, MonitoredTaskState::kWaiting)) {
      // The only valid reason for state not being MonitoredTaskState is because the task got
      // aborted.
      if (state() != MonitoredTaskState::kAborted) {
        LOG_WITH_PREFIX(FATAL) << "Unable to mark task as MonitoredTaskState::kWaiting";
      }
      AbortIfScheduled();
      return false;
    }
    return true;
  }
  return false;
}

void RetryingTSRpcTask::RunDelayedTask(const Status& status) {
  if (state() == MonitoredTaskState::kAborted) {
    UnregisterAsyncTask();  // May delete this.
    return;
  }

  if (!status.ok()) {
    LOG_WITH_PREFIX(WARNING) << "Async tablet task failed or was cancelled: " << status;
    if (status.IsAborted() || status.IsServiceUnavailable()) {
      AbortTask();
    }
    UnregisterAsyncTask();  // May delete this.
    return;
  }

  string desc = description();  // Save in case we need to log after deletion.
  Status s = Run();  // May delete this.
  if (!s.ok()) {
    LOG_WITH_PREFIX(WARNING) << "Async tablet task failed: " << s.ToString();
  }
}

void RetryingTSRpcTask::UnregisterAsyncTaskCallback() {}

void RetryingTSRpcTask::UnregisterAsyncTask() {
  UnregisterAsyncTaskCallback();

  auto s = state();
  if (!IsStateTerminal(s)) {
    LOG_WITH_PREFIX(FATAL) << "Invalid task state " << s;
  }
  end_ts_ = MonoTime::Now();
  if (table_ != nullptr) {
    table_->RemoveTask(shared_from_this());
  }
}

void RetryingTSRpcTask::AbortIfScheduled() {
  auto reactor_task_id = reactor_task_id_.load(std::memory_order_acquire);
  if (reactor_task_id != rpc::kInvalidTaskId) {
    master_->messenger()->AbortOnReactor(reactor_task_id);
  }
}

Status RetryingTSRpcTask::ResetTSProxy() {
  // TODO: if there is no replica available, should we still keep the task running?
  RETURN_NOT_OK(replica_picker_->PickReplica(&target_ts_desc_));

  shared_ptr<tserver::TabletServerServiceProxy> ts_proxy;
  shared_ptr<tserver::TabletServerAdminServiceProxy> ts_admin_proxy;
  shared_ptr<consensus::ConsensusServiceProxy> consensus_proxy;
  RETURN_NOT_OK(target_ts_desc_->GetProxy(&ts_proxy));
  RETURN_NOT_OK(target_ts_desc_->GetProxy(&ts_admin_proxy));
  RETURN_NOT_OK(target_ts_desc_->GetProxy(&consensus_proxy));

  ts_proxy_.swap(ts_proxy);
  ts_admin_proxy_.swap(ts_admin_proxy);
  consensus_proxy_.swap(consensus_proxy);

  return Status::OK();
}

void RetryingTSRpcTask::TransitionToTerminalState(MonitoredTaskState expected,
                                                  MonitoredTaskState terminal_state) {
  if (!PerformStateTransition(expected, terminal_state)) {
    if (terminal_state != MonitoredTaskState::kAborted && state() == MonitoredTaskState::kAborted) {
      LOG_WITH_PREFIX(WARNING) << "Unable to perform transition " << expected << " -> "
                               << terminal_state << ". Task has been aborted";
    } else {
      LOG_WITH_PREFIX(DFATAL) << "State transition " << expected << " -> "
                              << terminal_state << " failed. Current task is in an invalid state";
    }
  }
}

// ============================================================================
//  Class AsyncCreateReplica.
// ============================================================================
AsyncCreateReplica::AsyncCreateReplica(Master *master,
                                       ThreadPool *callback_pool,
                                       const string& permanent_uuid,
                                       const scoped_refptr<TabletInfo>& tablet)
  : RetrySpecificTSRpcTask(master, callback_pool, permanent_uuid, tablet->table().get()),
    tablet_id_(tablet->tablet_id()) {
  deadline_ = start_ts_;
  deadline_.AddDelta(MonoDelta::FromMilliseconds(FLAGS_tablet_creation_timeout_ms));

  auto table_lock = tablet->table()->LockForRead();
  const SysTabletsEntryPB& tablet_pb = tablet->metadata().dirty().pb;

  req_.set_dest_uuid(permanent_uuid);
  req_.set_table_id(tablet->table()->id());
  req_.set_tablet_id(tablet->tablet_id());
  req_.set_table_type(tablet->table()->metadata().state().pb.table_type());
  req_.mutable_partition()->CopyFrom(tablet_pb.partition());
  req_.set_table_name(table_lock->data().pb.name());
  req_.mutable_schema()->CopyFrom(table_lock->data().pb.schema());
  req_.mutable_partition_schema()->CopyFrom(table_lock->data().pb.partition_schema());
  req_.mutable_config()->CopyFrom(tablet_pb.committed_consensus_state().config());
  if (table_lock->data().pb.has_index_info()) {
    req_.mutable_index_info()->CopyFrom(table_lock->data().pb.index_info());
  }
}

void AsyncCreateReplica::HandleResponse(int attempt) {
  if (!resp_.has_error()) {
    TransitionToTerminalState(MonitoredTaskState::kRunning, MonitoredTaskState::kComplete);
  } else {
    Status s = StatusFromPB(resp_.error().status());
    if (s.IsAlreadyPresent()) {
      LOG(INFO) << "CreateTablet RPC for tablet " << tablet_id_
                << " on TS " << permanent_uuid_ << " returned already present: "
                << s.ToString();
      TransitionToTerminalState(MonitoredTaskState::kRunning, MonitoredTaskState::kComplete);
    } else {
      LOG(WARNING) << "CreateTablet RPC for tablet " << tablet_id_
                   << " on TS " << permanent_uuid_ << " failed: " << s.ToString();
    }
  }
}

bool AsyncCreateReplica::SendRequest(int attempt) {
  ts_admin_proxy_->CreateTabletAsync(req_, &resp_, &rpc_, BindRpcCallback());
  VLOG(1) << "Send create tablet request to " << permanent_uuid_ << ":\n"
          << " (attempt " << attempt << "):\n"
          << req_.DebugString();
  return true;
}

// ============================================================================
//  Class AsyncDeleteReplica.
// ============================================================================
void AsyncDeleteReplica::HandleResponse(int attempt) {
  bool delete_done = false;
  if (resp_.has_error()) {
    Status status = StatusFromPB(resp_.error().status());

    // Do not retry on a fatal error
    TabletServerErrorPB::Code code = resp_.error().code();
    switch (code) {
      case TabletServerErrorPB::TABLET_NOT_FOUND:
        LOG(WARNING) << "TS " << permanent_uuid_ << ": delete failed for tablet " << tablet_id_
                     << " because the tablet was not found. No further retry: "
                     << status.ToString();
        TransitionToTerminalState(MonitoredTaskState::kRunning, MonitoredTaskState::kComplete);
        delete_done = true;
        break;
      case TabletServerErrorPB::CAS_FAILED:
        LOG(WARNING) << "TS " << permanent_uuid_ << ": delete failed for tablet " << tablet_id_
                     << " due to a CAS failure. No further retry: " << status.ToString();
        TransitionToTerminalState(MonitoredTaskState::kRunning, MonitoredTaskState::kComplete);
        delete_done = true;
        break;
      case TabletServerErrorPB::WRONG_SERVER_UUID:
        LOG(WARNING) << "TS " << permanent_uuid_ << ": delete failed for tablet " << tablet_id_
                     << " due to an incorrect UUID. No further retry: " << status.ToString();
        TransitionToTerminalState(MonitoredTaskState::kRunning, MonitoredTaskState::kComplete);
        delete_done = true;
        break;
      default:
        LOG(WARNING) << "TS " << permanent_uuid_ << ": delete failed for tablet " << tablet_id_
                     << " with error code " << TabletServerErrorPB::Code_Name(code)
                     << ": " << status.ToString();
        break;
    }
  } else {
    if (table_) {
      LOG(INFO) << "TS " << permanent_uuid_ << ": tablet " << tablet_id_
                << " (table " << table_->ToString() << ") successfully deleted";
    } else {
      LOG(WARNING) << "TS " << permanent_uuid_ << ": tablet " << tablet_id_
                   << " did not belong to a known table, but was successfully deleted";
    }
    TransitionToTerminalState(MonitoredTaskState::kRunning, MonitoredTaskState::kComplete);
    delete_done = true;
    VLOG(1) << "TS " << permanent_uuid_ << ": delete complete on tablet " << tablet_id_;
  }
  if (delete_done) {
    UnregisterAsyncTaskCallback();
  }
}

bool AsyncDeleteReplica::SendRequest(int attempt) {
  tserver::DeleteTabletRequestPB req;
  req.set_dest_uuid(permanent_uuid_);
  req.set_tablet_id(tablet_id_);
  req.set_reason(reason_);
  req.set_delete_type(delete_type_);
  if (cas_config_opid_index_less_or_equal_) {
    req.set_cas_config_opid_index_less_or_equal(*cas_config_opid_index_less_or_equal_);
  }

  ts_admin_proxy_->DeleteTabletAsync(req, &resp_, &rpc_, BindRpcCallback());
  VLOG(1) << "Send delete tablet request to " << permanent_uuid_
          << " (attempt " << attempt << "):\n"
          << req.DebugString();
  return true;
}

void AsyncDeleteReplica::UnregisterAsyncTaskCallback() {
  master_->catalog_manager()->NotifyTabletDeleteFinished(permanent_uuid_, tablet_id_);
}

// ============================================================================
//  Class AsyncAlterTable.
// ============================================================================
AsyncAlterTable::AsyncAlterTable(Master *master,
                                 ThreadPool* callback_pool,
                                 const scoped_refptr<TabletInfo>& tablet)
  : RetryingTSRpcTask(master,
                      callback_pool,
                      gscoped_ptr<TSPicker>(new PickLeaderReplica(tablet)),
                      tablet->table().get()),
    tablet_(tablet) {
}

string AsyncAlterTable::description() const {
  return tablet_->ToString() + " Alter Table RPC";
}

TabletId AsyncAlterTable::tablet_id() const {
  return tablet_->tablet_id();
}

TabletServerId AsyncAlterTable::permanent_uuid() const {
  return target_ts_desc_ != nullptr ? target_ts_desc_->permanent_uuid() : "";
}

void AsyncAlterTable::HandleResponse(int attempt) {
  if (resp_.has_error()) {
    Status status = StatusFromPB(resp_.error().status());

    // Do not retry on a fatal error
    switch (resp_.error().code()) {
      case TabletServerErrorPB::TABLET_NOT_FOUND:
      case TabletServerErrorPB::MISMATCHED_SCHEMA:
      case TabletServerErrorPB::TABLET_HAS_A_NEWER_SCHEMA:
        LOG(WARNING) << "TS " << permanent_uuid() << ": alter failed for tablet "
                     << tablet_->ToString() << " no further retry: " << status.ToString();
        TransitionToTerminalState(MonitoredTaskState::kRunning, MonitoredTaskState::kComplete);
        break;
      default:
        LOG(WARNING) << "TS " << permanent_uuid() << ": alter failed for tablet "
                     << tablet_->ToString() << ": " << status.ToString();
        break;
    }
  } else {
    TransitionToTerminalState(MonitoredTaskState::kRunning, MonitoredTaskState::kComplete);
    VLOG(1) << "TS " << permanent_uuid() << ": alter complete on tablet " << tablet_->ToString();
  }

  server::UpdateClock(resp_, master_->clock());

  if (state() == MonitoredTaskState::kComplete) {
    // TODO: proper error handling here.
    CHECK_OK(master_->catalog_manager()->HandleTabletSchemaVersionReport(
        tablet_.get(), schema_version_));
  } else {
    VLOG(1) << "Task is not completed";
  }
}

bool AsyncAlterTable::SendRequest(int attempt) {
  auto l = table_->LockForRead();

  tserver::ChangeMetadataRequestPB req;
  req.set_schema_version(l->data().pb.version());
  req.set_dest_uuid(permanent_uuid());
  req.set_tablet_id(tablet_->tablet_id());

  if (l->data().pb.has_wal_retention_secs()) {
    req.set_wal_retention_secs(l->data().pb.wal_retention_secs());
  }

  req.mutable_schema()->CopyFrom(l->data().pb.schema());
  req.set_new_table_name(l->data().pb.name());
  req.mutable_indexes()->CopyFrom(l->data().pb.indexes());
  req.set_propagated_hybrid_time(master_->clock()->Now().ToUint64());

  schema_version_ = l->data().pb.version();

  l->Unlock();

  ts_admin_proxy_->AlterSchemaAsync(req, &resp_, &rpc_, BindRpcCallback());
  VLOG(1) << "Send alter table request to " << permanent_uuid()
          << " (attempt " << attempt << "):\n"
          << req.DebugString();
  return true;
}

// ============================================================================
//  Class AsyncCopartitionTable.
// ============================================================================
AsyncCopartitionTable::AsyncCopartitionTable(Master *master,
                                             ThreadPool* callback_pool,
                                             const scoped_refptr<TabletInfo>& tablet,
                                             const scoped_refptr<TableInfo>& table)
    : RetryingTSRpcTask(master,
                        callback_pool,
                        gscoped_ptr<TSPicker>(new PickLeaderReplica(tablet)),
                        table.get()),
      tablet_(tablet), table_(table) {
}

string AsyncCopartitionTable::description() const {
  return tablet_->ToString() + " handling copartition Table RPC for table " + table_->ToString();
}

TabletId AsyncCopartitionTable::tablet_id() const {
  return tablet_->tablet_id();
}

TabletServerId AsyncCopartitionTable::permanent_uuid() const {
  return target_ts_desc_ != nullptr ? target_ts_desc_->permanent_uuid() : "";
}

// TODO(sagnik): modify this to fill all relevant fields for the AsyncCopartition request.
bool AsyncCopartitionTable::SendRequest(int attempt) {

  tserver::CopartitionTableRequestPB req;
  req.set_dest_uuid(permanent_uuid());
  req.set_tablet_id(tablet_->tablet_id());
  req.set_table_id(table_->id());
  req.set_table_name(table_->name());

  ts_admin_proxy_->CopartitionTableAsync(req, &resp_, &rpc_, BindRpcCallback());
  VLOG(1) << "Send copartition table request to " << permanent_uuid()
          << " (attempt " << attempt << "):\n"
          << req.DebugString();
  return true;
}

// TODO(sagnik): modify this to handle the AsyncCopartition Response and retry fail as necessary.
void AsyncCopartitionTable::HandleResponse(int attempt) {
  LOG(INFO) << "master can't handle server responses yet";
}

// ============================================================================
//  Class AsyncTruncate.
// ============================================================================
AsyncTruncate::AsyncTruncate(Master *master,
                             ThreadPool* callback_pool,
                             const scoped_refptr<TabletInfo>& tablet)
    : RetryingTSRpcTask(master,
                        callback_pool,
                        gscoped_ptr<TSPicker>(new PickLeaderReplica(tablet)),
                        tablet->table().get()),
      tablet_(tablet) {
}

string AsyncTruncate::description() const {
  return tablet_->ToString() + " Truncate Tablet RPC";
}

TabletId AsyncTruncate::tablet_id() const {
  return tablet_->tablet_id();
}

TabletServerId AsyncTruncate::permanent_uuid() const {
  return target_ts_desc_ != nullptr ? target_ts_desc_->permanent_uuid() : "";
}

void AsyncTruncate::HandleResponse(int attempt) {
  if (resp_.has_error()) {
    const Status s = StatusFromPB(resp_.error().status());
    const TabletServerErrorPB::Code code = resp_.error().code();
    LOG(WARNING) << "TS " << permanent_uuid() << ": truncate failed for tablet " << tablet_id()
                 << " with error code " << TabletServerErrorPB::Code_Name(code)
                 << ": " << s.ToString();
  } else {
    VLOG(1) << "TS " << permanent_uuid() << ": truncate complete on tablet " << tablet_id();
    TransitionToTerminalState(MonitoredTaskState::kRunning, MonitoredTaskState::kComplete);
  }

  server::UpdateClock(resp_, master_->clock());
}

bool AsyncTruncate::SendRequest(int attempt) {
  tserver::TruncateRequestPB req;
  req.set_tablet_id(tablet_id());
  req.set_propagated_hybrid_time(master_->clock()->Now().ToUint64());
  ts_proxy_->TruncateAsync(req, &resp_, &rpc_, BindRpcCallback());
  VLOG(1) << "Send truncate tablet request to " << permanent_uuid()
          << " (attempt " << attempt << "):\n"
          << req.DebugString();
  return true;
}

// ============================================================================
//  Class CommonInfoForRaftTask.
// ============================================================================
CommonInfoForRaftTask::CommonInfoForRaftTask(
    Master* master, ThreadPool* callback_pool, const scoped_refptr<TabletInfo>& tablet,
    const consensus::ConsensusStatePB& cstate, const string& change_config_ts_uuid)
    : RetryingTSRpcTask(
          master, callback_pool, gscoped_ptr<TSPicker>(new PickLeaderReplica(tablet)),
          tablet->table()),
      tablet_(tablet),
      cstate_(cstate),
      change_config_ts_uuid_(change_config_ts_uuid) {
  deadline_ = MonoTime::Max();  // Never time out.
}

TabletId CommonInfoForRaftTask::tablet_id() const {
  return tablet_->tablet_id();
}

TabletServerId CommonInfoForRaftTask::permanent_uuid() const {
  return target_ts_desc_ != nullptr ? target_ts_desc_->permanent_uuid() : "";
}

// ============================================================================
//  Class AsyncChangeConfigTask.
// ============================================================================
string AsyncChangeConfigTask::description() const {
  return strings::Substitute(
      "$0 RPC for tablet $1 on peer $2 with cas_config_opid_index $3", type_name(),
      tablet_->tablet_id(), permanent_uuid(), cstate_.config().opid_index());
}

bool AsyncChangeConfigTask::SendRequest(int attempt) {
  // Bail if we're retrying in vain.
  int64_t latest_index;
  {
    auto tablet_lock = tablet_->LockForRead();
    latest_index = tablet_lock->data().pb.committed_consensus_state().config().opid_index();
    // Adding this logic for a race condition that occurs in this scenario:
    // 1. CatalogManager receives a DeleteTable request and sends DeleteTablet requests to the
    // tservers, but doesn't yet update the tablet in memory state to not running.
    // 2. The CB runs and sees that this tablet is still running, sees that it is over-replicated
    // (since the placement now dictates it should have 0 replicas),
    // but before it can send the ChangeConfig RPC to a tserver.
    // 3. That tserver processes the DeleteTablet request.
    // 4. The ChangeConfig RPC now returns tablet not found,
    // which prompts an infinite retry of the RPC.
    bool tablet_running = tablet_lock->data().is_running();
    if (!tablet_running) {
      AbortTask();
      return false;
    }
  }
  if (latest_index > cstate_.config().opid_index()) {
    LOG_WITH_PREFIX(INFO) << "Latest config for has opid_index of " << latest_index
                          << " while this task has opid_index of "
                          << cstate_.config().opid_index() << ". Aborting task.";
    AbortTask();
    return false;
  }

  // Logging should be covered inside based on failure reasons.
  if (!PrepareRequest(attempt)) {
    AbortTask();
    return false;
  }

  consensus_proxy_->ChangeConfigAsync(req_, &resp_, &rpc_, BindRpcCallback());
  VLOG(1) << "Task " << description() << " sent request:\n" << req_.DebugString();
  return true;
}

void AsyncChangeConfigTask::HandleResponse(int attempt) {
  if (!resp_.has_error()) {
    TransitionToTerminalState(MonitoredTaskState::kRunning, MonitoredTaskState::kComplete);
    LOG_WITH_PREFIX(INFO) << Substitute(
        "Change config succeeded on leader TS $0 for tablet $1 with type $2 for replica $3",
        permanent_uuid(), tablet_->tablet_id(), type_name(), change_config_ts_uuid_);
    return;
  }

  Status status = StatusFromPB(resp_.error().status());

  // Do not retry on some known errors, otherwise retry forever or until cancelled.
  switch (resp_.error().code()) {
    case TabletServerErrorPB::CAS_FAILED:
    case TabletServerErrorPB::ADD_CHANGE_CONFIG_ALREADY_PRESENT:
    case TabletServerErrorPB::REMOVE_CHANGE_CONFIG_NOT_PRESENT:
    case TabletServerErrorPB::NOT_THE_LEADER:
      LOG_WITH_PREFIX(WARNING) << "ChangeConfig() failed on leader " << permanent_uuid()
                               << ". No further retry: " << status.ToString();
      TransitionToTerminalState(MonitoredTaskState::kRunning, MonitoredTaskState::kFailed);
      break;
    default:
      LOG_WITH_PREFIX(INFO) << "ChangeConfig() failed on leader " << permanent_uuid()
                            << " due to error "
                            << TabletServerErrorPB::Code_Name(resp_.error().code())
                            << ". This operation will be retried. Error detail: "
                            << status.ToString();
      break;
  }
}

// ============================================================================
//  Class AsyncAddServerTask.
// ============================================================================
bool AsyncAddServerTask::PrepareRequest(int attempt) {
  // Select the replica we wish to add to the config.
  // Do not include current members of the config.
  unordered_set<string> replica_uuids;
  for (const RaftPeerPB& peer : cstate_.config().peers()) {
    InsertOrDie(&replica_uuids, peer.permanent_uuid());
  }
  TSDescriptorVector ts_descs;
  master_->ts_manager()->GetAllLiveDescriptors(&ts_descs);
  shared_ptr<TSDescriptor> replacement_replica;
  for (auto ts_desc : ts_descs) {
    if (ts_desc->permanent_uuid() == change_config_ts_uuid_) {
      // This is given by the client, so we assume it is a well chosen uuid.
      replacement_replica = ts_desc;
      break;
    }
  }
  if (PREDICT_FALSE(!replacement_replica)) {
    LOG(WARNING) << "Could not find desired replica " << change_config_ts_uuid_ << " in live set!";
    return false;
  }

  req_.set_dest_uuid(permanent_uuid());
  req_.set_tablet_id(tablet_->tablet_id());
  req_.set_type(consensus::ADD_SERVER);
  req_.set_cas_config_opid_index(cstate_.config().opid_index());
  RaftPeerPB* peer = req_.mutable_server();
  peer->set_permanent_uuid(replacement_replica->permanent_uuid());
  peer->set_member_type(member_type_);
  TSRegistrationPB peer_reg = replacement_replica->GetRegistration();

  if (peer_reg.common().private_rpc_addresses().empty()) {
    YB_LOG_EVERY_N(WARNING, 100) << LogPrefix() << "Candidate replacement "
                                 << replacement_replica->permanent_uuid()
                                 << " has no registered rpc address: "
                                 << peer_reg.ShortDebugString();
    return false;
  }

  TakeRegistration(peer_reg.mutable_common(), peer);

  return true;
}

// ============================================================================
//  Class AsyncRemoveServerTask.
// ============================================================================
bool AsyncRemoveServerTask::PrepareRequest(int attempt) {
  bool found = false;
  for (const RaftPeerPB& peer : cstate_.config().peers()) {
    if (change_config_ts_uuid_ == peer.permanent_uuid()) {
      found = true;
    }
  }

  if (!found) {
    LOG(WARNING) << "Asked to remove TS with uuid " << change_config_ts_uuid_
                 << " but could not find it in config peers!";
    return false;
  }

  req_.set_dest_uuid(permanent_uuid());
  req_.set_tablet_id(tablet_->tablet_id());
  req_.set_type(consensus::REMOVE_SERVER);
  req_.set_cas_config_opid_index(cstate_.config().opid_index());
  RaftPeerPB* peer = req_.mutable_server();
  peer->set_permanent_uuid(change_config_ts_uuid_);

  return true;
}

// ============================================================================
//  Class AsyncTryStepDown.
// ============================================================================
bool AsyncTryStepDown::PrepareRequest(int attempt) {
  LOG(INFO) << Substitute("Prep Leader step down $0, leader_uuid=$1, change_ts_uuid=$2",
                          attempt, permanent_uuid(), change_config_ts_uuid_);
  if (attempt > 1) {
    return false;
  }

  // If we were asked to remove the server even if it is the leader, we have to call StepDown, but
  // only if our current leader is the server we are asked to remove.
  if (permanent_uuid() != change_config_ts_uuid_) {
    LOG(WARNING) << "Incorrect state - config leader " << permanent_uuid() << " does not match "
                 << "target uuid " << change_config_ts_uuid_ << " for a leader step down op.";
    return false;
  }

  stepdown_req_.set_dest_uuid(change_config_ts_uuid_);
  stepdown_req_.set_tablet_id(tablet_->tablet_id());
  if (!new_leader_uuid_.empty()) {
    stepdown_req_.set_new_leader_uuid(new_leader_uuid_);
  }

  return true;
}

bool AsyncTryStepDown::SendRequest(int attempt) {
  if (!PrepareRequest(attempt)) {
    AbortTask();
    return false;
  }

  LOG(INFO) << Substitute("Stepping down leader $0 for tablet $1",
                          change_config_ts_uuid_, tablet_->tablet_id());
  consensus_proxy_->LeaderStepDownAsync(
      stepdown_req_, &stepdown_resp_, &rpc_, BindRpcCallback());

  return true;
}

void AsyncTryStepDown::HandleResponse(int attempt) {
  if (!rpc_.status().ok()) {
    AbortTask();
    LOG(WARNING) << Substitute(
        "Got error on stepdown for tablet $0 with leader $1, attempt $2 and error $3",
        tablet_->tablet_id(), permanent_uuid(), attempt, rpc_.status().ToString());

    return;
  }

  TransitionToTerminalState(MonitoredTaskState::kRunning, MonitoredTaskState::kComplete);
  const bool stepdown_failed = stepdown_resp_.error().status().code() != AppStatusPB::OK;
  LOG(INFO) << Format("Leader step down done attempt=$0, leader_uuid=$1, change_uuid=$2, "
                      "error=$3, failed=$4, should_remove=$5 for tablet $6.",
                      attempt, permanent_uuid(), change_config_ts_uuid_, stepdown_resp_.error(),
                      stepdown_failed, should_remove_, tablet_->tablet_id());

  if (stepdown_failed) {
    tablet_->RegisterLeaderStepDownFailure(change_config_ts_uuid_,
        MonoDelta::FromMilliseconds(stepdown_resp_.has_time_since_election_failure_ms() ?
                                    stepdown_resp_.time_since_election_failure_ms() : 0));
  }

  if (should_remove_) {
    auto task = std::make_shared<AsyncRemoveServerTask>(
        master_, callback_pool_, tablet_, cstate_, change_config_ts_uuid_);

    tablet_->table()->AddTask(task);
    Status status = task->Run();
    WARN_NOT_OK(status, "Failed to send new RemoveServer request");
  }
}

}  // namespace master
}  // namespace yb
