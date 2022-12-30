/* Copyright (c) 2020 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "meta/processors/session/SessionManagerProcessor.h"

namespace nebula {
namespace meta {

void CreateSessionProcessor::process(const cpp2::CreateSessionReq& req) {
  folly::SharedMutex::WriteHolder holder(LockUtils::sessionLock());
  const auto& user = req.get_user();
  auto ret = userExist(user);
  if (ret != nebula::cpp2::ErrorCode::SUCCEEDED) {
    LOG(INFO) << "User does not exist, errorCode: " << apache::thrift::util::enumNameSafe(ret);
    handleErrorCode(ret);
    onFinished();
    return;
  }

  cpp2::Session session;
  // The sessionId is generated by microsecond timestamp
  session.session_id_ref() = time::WallClock::fastNowInMicroSec();
  session.create_time_ref() = session.get_session_id();
  session.update_time_ref() = session.get_create_time();
  session.user_name_ref() = user;
  session.graph_addr_ref() = req.get_graph_addr();
  session.client_ip_ref() = req.get_client_ip();

  std::vector<kvstore::KV> data;
  data.emplace_back(MetaKeyUtils::sessionKey(session.get_session_id()),
                    MetaKeyUtils::sessionVal(session));
  resp_.session_ref() = session;
  ret = doSyncPut(std::move(data));
  if (ret != nebula::cpp2::ErrorCode::SUCCEEDED) {
    LOG(INFO) << "Put data error on meta server, errorCode: "
              << apache::thrift::util::enumNameSafe(ret);
  }
  handleErrorCode(ret);
  onFinished();
}

void UpdateSessionsProcessor::process(const cpp2::UpdateSessionsReq& req) {
  folly::SharedMutex::WriteHolder holder(LockUtils::sessionLock());
  std::vector<kvstore::KV> data;
  std::unordered_map<nebula::SessionID,
                     std::unordered_map<nebula::ExecutionPlanID, cpp2::QueryDesc>>
      killedQueries;

  std::vector<SessionID> killedSessions;

  for (auto& session : req.get_sessions()) {
    auto sessionId = session.get_session_id();
    auto sessionKey = MetaKeyUtils::sessionKey(sessionId);
    auto ret = doGet(sessionKey);
    if (!nebula::ok(ret)) {
      auto errCode = nebula::error(ret);
      LOG(INFO) << "Session id '" << sessionId << "' not found";
      // If the session requested to be updated can not be found in meta, the session has been
      // killed
      if (errCode == nebula::cpp2::ErrorCode::E_KEY_NOT_FOUND) {
        killedSessions.emplace_back(sessionId);
        continue;
      }
    }

    // update sessions to be saved if query is being killed, and return them to
    // client.
    auto& newQueries = *session.queries_ref();
    std::unordered_map<nebula::ExecutionPlanID, cpp2::QueryDesc> killedQueriesInCurrentSession;
    auto sessionInMeta = MetaKeyUtils::parseSessionVal(nebula::value(ret));
    for (const auto& savedQuery : sessionInMeta.get_queries()) {
      auto epId = savedQuery.first;
      auto newQuery = newQueries.find(epId);
      if (newQuery == newQueries.end()) {
        continue;
      }
      auto& desc = savedQuery.second;
      if (desc.get_status() == cpp2::QueryStatus::KILLING) {
        const_cast<cpp2::QueryDesc&>(newQuery->second).status_ref() = cpp2::QueryStatus::KILLING;
        killedQueriesInCurrentSession.emplace(epId, desc);
      }
    }
    if (!killedQueriesInCurrentSession.empty()) {
      killedQueries[sessionId] = std::move(killedQueriesInCurrentSession);
    }

    if (sessionInMeta.get_update_time() > session.get_update_time()) {
      VLOG(3) << "The session id: " << session.get_session_id()
              << ", the new update time: " << session.get_update_time()
              << ", the old update time: " << sessionInMeta.get_update_time();
      continue;
    }

    data.emplace_back(MetaKeyUtils::sessionKey(sessionId), MetaKeyUtils::sessionVal(session));
  }

  auto ret = doSyncPut(std::move(data));
  if (ret != nebula::cpp2::ErrorCode::SUCCEEDED) {
    LOG(INFO) << "Put data error on meta server, errorCode: "
              << apache::thrift::util::enumNameSafe(ret);
    handleErrorCode(ret);
    onFinished();
    return;
  }

  resp_.killed_queries_ref() = std::move(killedQueries);
  resp_.killed_sessions_ref() = std::move(killedSessions);
  handleErrorCode(nebula::cpp2::ErrorCode::SUCCEEDED);
  onFinished();
}

void ListSessionsProcessor::process(const cpp2::ListSessionsReq&) {
  folly::SharedMutex::ReadHolder holder(LockUtils::sessionLock());
  auto& prefix = MetaKeyUtils::sessionPrefix();
  auto ret = doPrefix(prefix);
  if (!nebula::ok(ret)) {
    handleErrorCode(nebula::error(ret));
    onFinished();
    return;
  }

  std::vector<cpp2::Session> sessions;
  auto iter = nebula::value(ret).get();
  while (iter->valid()) {
    auto session = MetaKeyUtils::parseSessionVal(iter->val());
    VLOG(3) << "List session: " << session.get_session_id();
    sessions.emplace_back(std::move(session));
    iter->next();
  }
  resp_.sessions_ref() = std::move(sessions);
  for (auto& session : resp_.get_sessions()) {
    LOG(INFO) << "resp list session: " << session.get_session_id();
  }
  handleErrorCode(nebula::cpp2::ErrorCode::SUCCEEDED);
  onFinished();
}

void GetSessionProcessor::process(const cpp2::GetSessionReq& req) {
  folly::SharedMutex::ReadHolder holder(LockUtils::sessionLock());
  auto sessionId = req.get_session_id();
  auto sessionKey = MetaKeyUtils::sessionKey(sessionId);
  auto ret = doGet(sessionKey);
  if (!nebula::ok(ret)) {
    auto errCode = nebula::error(ret);
    LOG(INFO) << "Session id `" << sessionId << "' not found";
    if (errCode == nebula::cpp2::ErrorCode::E_KEY_NOT_FOUND) {
      errCode = nebula::cpp2::ErrorCode::E_SESSION_NOT_FOUND;
    }
    handleErrorCode(errCode);
    onFinished();
    return;
  }

  auto session = MetaKeyUtils::parseSessionVal(nebula::value(ret));
  resp_.session_ref() = std::move(session);
  handleErrorCode(nebula::cpp2::ErrorCode::SUCCEEDED);
  onFinished();
}

void RemoveSessionProcessor::process(const cpp2::RemoveSessionReq& req) {
  folly::SharedMutex::WriteHolder holder(LockUtils::sessionLock());
  std::vector<SessionID> killedSessions;

  auto sessionIds = req.get_session_ids();

  for (auto sessionId : sessionIds) {
    auto sessionKey = MetaKeyUtils::sessionKey(sessionId);
    auto ret = doGet(sessionKey);

    // If the session is not found, we should continue to remove other sessions.
    if (!nebula::ok(ret)) {
      LOG(INFO) << "Session id `" << sessionId << "' not found";
      continue;
    }

    // Remove session key from kvstore
    folly::Baton<true, std::atomic> baton;
    nebula::cpp2::ErrorCode errorCode;
    kvstore_->asyncRemove(kDefaultSpaceId,
                          kDefaultPartId,
                          sessionKey,
                          [this, &baton, &errorCode](nebula::cpp2::ErrorCode code) {
                            this->handleErrorCode(code);
                            errorCode = code;
                            baton.post();
                          });
    baton.wait();

    // continue if the session is not removed successfully
    if (errorCode != nebula::cpp2::ErrorCode::SUCCEEDED) {
      LOG(ERROR) << "Remove session key failed, error code: " << static_cast<int32_t>(errorCode);
      continue;
    }

    // record the removed session id
    killedSessions.emplace_back(sessionId);
  }

  handleErrorCode(nebula::cpp2::ErrorCode::SUCCEEDED);
  resp_.removed_session_ids_ref() = std::move(killedSessions);
  onFinished();
}

void KillQueryProcessor::process(const cpp2::KillQueryReq& req) {
  folly::SharedMutex::WriteHolder holder(LockUtils::sessionLock());
  auto& killQueries = req.get_kill_queries();

  std::vector<kvstore::KV> data;
  for (auto& kv : killQueries) {
    auto sessionId = kv.first;
    auto sessionKey = MetaKeyUtils::sessionKey(sessionId);
    auto ret = doGet(sessionKey);
    if (!nebula::ok(ret)) {
      auto errCode = nebula::error(ret);
      LOG(INFO) << "Session id `" << sessionId << "' not found";
      if (errCode == nebula::cpp2::ErrorCode::E_KEY_NOT_FOUND) {
        errCode = nebula::cpp2::ErrorCode::E_SESSION_NOT_FOUND;
      }
      handleErrorCode(errCode);
      onFinished();
      return;
    }

    auto session = MetaKeyUtils::parseSessionVal(nebula::value(ret));
    for (auto& epId : kv.second) {
      auto query = session.queries_ref()->find(epId);
      if (query == session.queries_ref()->end()) {
        handleErrorCode(nebula::cpp2::ErrorCode::E_QUERY_NOT_FOUND);
        onFinished();
        return;
      }
      query->second.status_ref() = cpp2::QueryStatus::KILLING;
    }

    data.emplace_back(MetaKeyUtils::sessionKey(sessionId), MetaKeyUtils::sessionVal(session));
  }

  auto putRet = doSyncPut(std::move(data));
  if (putRet != nebula::cpp2::ErrorCode::SUCCEEDED) {
    LOG(INFO) << "Put data error on meta server, errorCode: "
              << apache::thrift::util::enumNameSafe(putRet);
  }
  handleErrorCode(putRet);
  onFinished();
}

}  // namespace meta
}  // namespace nebula
