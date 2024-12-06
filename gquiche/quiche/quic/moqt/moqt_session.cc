// Copyright 2023 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "quiche/quic/moqt/moqt_session.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>


#include "absl/algorithm/container.h"
#include "absl/container/btree_map.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/container/node_hash_map.h"
#include "absl/functional/bind_front.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "quiche/quic/core/quic_types.h"
#include "quiche/quic/moqt/moqt_framer.h"
#include "quiche/quic/moqt/moqt_messages.h"
#include "quiche/quic/moqt/moqt_parser.h"
#include "quiche/quic/moqt/moqt_priority.h"
#include "quiche/quic/moqt/moqt_publisher.h"
#include "quiche/quic/moqt/moqt_subscribe_windows.h"
#include "quiche/quic/moqt/moqt_track.h"
#include "quiche/quic/platform/api/quic_logging.h"
#include "quiche/common/platform/api/quiche_bug_tracker.h"
#include "quiche/common/platform/api/quiche_logging.h"
#include "quiche/common/quiche_buffer_allocator.h"
#include "quiche/common/quiche_stream.h"
#include "quiche/common/simple_buffer_allocator.h"
#include "quiche/web_transport/web_transport.h"

#define ENDPOINT \
  (perspective() == Perspective::IS_SERVER ? "MoQT Server: " : "MoQT Client: ")

namespace moqt {

namespace {

using ::quic::Perspective;

constexpr MoqtPriority kDefaultSubscriberPriority = 0x80;

// WebTransport lets applications split a session into multiple send groups
// that have equal weight for scheduling. We don't have a use for that, so the
// send group is always the same.
constexpr webtransport::SendGroupId kMoqtSendGroupId = 0;

bool PublisherHasData(const MoqtTrackPublisher& publisher) {
  absl::StatusOr<MoqtTrackStatusCode> status = publisher.GetTrackStatus();
  return status.ok() && DoesTrackStatusImplyHavingData(*status);
}

SubscribeWindow SubscribeMessageToWindow(const MoqtSubscribe& subscribe,
                                         MoqtTrackPublisher& publisher) {
  const FullSequence sequence = PublisherHasData(publisher)
                                    ? publisher.GetLargestSequence()
                                    : FullSequence{0, 0};
  switch (GetFilterType(subscribe)) {
    case MoqtFilterType::kLatestGroup:
      return SubscribeWindow(sequence.group, 0);
    case MoqtFilterType::kLatestObject:
      return SubscribeWindow(sequence.group, sequence.object);
    case MoqtFilterType::kAbsoluteStart:
      return SubscribeWindow(*subscribe.start_group, *subscribe.start_object);
    case MoqtFilterType::kAbsoluteRange:
      return SubscribeWindow(*subscribe.start_group, *subscribe.start_object,
                             *subscribe.end_group, *subscribe.end_object);
    case MoqtFilterType::kNone:
      QUICHE_BUG(MoqtSession_Subscription_invalid_filter_passed);
      return SubscribeWindow(0, 0);
  }
}

class DefaultPublisher : public MoqtPublisher {
 public:
  static DefaultPublisher* GetInstance() {
    static DefaultPublisher* instance = new DefaultPublisher();
    return instance;
  }

  absl::StatusOr<std::shared_ptr<MoqtTrackPublisher>> GetTrack(
      const FullTrackName& track_name) override {
    return absl::NotFoundError("No tracks published");
  }
};
}  // namespace

MoqtSession::MoqtSession(webtransport::Session* session,
                         MoqtSessionParameters parameters,
                         MoqtSessionCallbacks callbacks)
    : session_(session),
      parameters_(parameters),
      callbacks_(std::move(callbacks)),
      framer_(quiche::SimpleBufferAllocator::Get(), parameters.using_webtrans),
      publisher_(DefaultPublisher::GetInstance()),
      local_max_subscribe_id_(parameters.max_subscribe_id),
      liveness_token_(std::make_shared<Empty>()) {}

MoqtSession::ControlStream* MoqtSession::GetControlStream() {
  if (!control_stream_.has_value()) {
    return nullptr;
  }
  webtransport::Stream* raw_stream = session_->GetStreamById(*control_stream_);
  if (raw_stream == nullptr) {
    return nullptr;
  }
  return static_cast<ControlStream*>(raw_stream->visitor());
}

void MoqtSession::SendControlMessage(quiche::QuicheBuffer message) {
  ControlStream* control_stream = GetControlStream();
  if (control_stream == nullptr) {
    QUICHE_LOG(DFATAL) << "Trying to send a message on the control stream "
                          "while it does not exist";
    return;
  }
  control_stream->SendOrBufferMessage(std::move(message));
}

void MoqtSession::OnSessionReady() {
  QUICHE_DLOG(INFO) << ENDPOINT << "Underlying session ready";
  if (parameters_.perspective == Perspective::IS_SERVER) {
    return;
  }

  webtransport::Stream* control_stream =
      session_->OpenOutgoingBidirectionalStream();
  if (control_stream == nullptr) {
    Error(MoqtError::kInternalError, "Unable to open a control stream");
    return;
  }
  control_stream->SetVisitor(
      std::make_unique<ControlStream>(this, control_stream));
  control_stream_ = control_stream->GetStreamId();
  MoqtClientSetup setup = MoqtClientSetup{
      .supported_versions = std::vector<MoqtVersion>{parameters_.version},
      .role = MoqtRole::kPubSub,
      .max_subscribe_id = parameters_.max_subscribe_id,
      .supports_object_ack = parameters_.support_object_acks,
  };
  if (!parameters_.using_webtrans) {
    setup.path = parameters_.path;
  }
  SendControlMessage(framer_.SerializeClientSetup(setup));
  QUIC_DLOG(INFO) << ENDPOINT << "Send the SETUP message";
}

void MoqtSession::OnSessionClosed(webtransport::SessionErrorCode,
                                  const std::string& error_message) {
  if (!error_.empty()) {
    // Avoid erroring out twice.
    return;
  }
  QUICHE_DLOG(INFO) << ENDPOINT << "Underlying session closed with message: "
                    << error_message;
  error_ = error_message;
  std::move(callbacks_.session_terminated_callback)(error_message);
}

void MoqtSession::OnIncomingBidirectionalStreamAvailable() {
  while (webtransport::Stream* stream =
             session_->AcceptIncomingBidirectionalStream()) {
    if (control_stream_.has_value()) {
      Error(MoqtError::kProtocolViolation, "Bidirectional stream already open");
      return;
    }
    stream->SetVisitor(std::make_unique<ControlStream>(this, stream));
    stream->visitor()->OnCanRead();
  }
}
void MoqtSession::OnIncomingUnidirectionalStreamAvailable() {
  while (webtransport::Stream* stream =
             session_->AcceptIncomingUnidirectionalStream()) {
    stream->SetVisitor(std::make_unique<IncomingDataStream>(this, stream));
    stream->visitor()->OnCanRead();
  }
}

void MoqtSession::OnDatagramReceived(absl::string_view datagram) {
  MoqtObject message;
  std::optional<absl::string_view> payload = ParseDatagram(datagram, message);
  if (!payload.has_value()) {
    Error(MoqtError::kProtocolViolation, "Malformed datagram received");
    return;
  }
  QUICHE_DLOG(INFO) << ENDPOINT
                    << "Received OBJECT message in datagram for subscribe_id "
                    << " for track alias " << message.track_alias
                    << " with sequence " << message.group_id << ":"
                    << message.object_id << " priority "
                    << message.publisher_priority << " length "
                    << payload->size();
  SubscribeRemoteTrack* track = RemoteTrackByAlias(message.track_alias);
  if (track == nullptr) {
    return;
  }
  if (!track->CheckDataStreamType(MoqtDataStreamType::kObjectDatagram)) {
    Error(MoqtError::kProtocolViolation,
          "Received DATAGRAM for non-datagram track");
    return;
  }
  if (!track->InWindow(FullSequence(message.group_id, message.object_id))) {
    // TODO(martinduke): a recent SUBSCRIBE_UPDATE could put us here, and it's
    // not an error.
    return;
  }
  QUICHE_CHECK(!track->is_fetch());
  track->OnObjectOrOk();
  SubscribeRemoteTrack::Visitor* visitor = track->visitor();
  if (visitor != nullptr) {
    visitor->OnObjectFragment(
        track->full_track_name(),
        FullSequence{message.group_id, 0, message.object_id},
        message.publisher_priority, message.object_status, *payload, true);
  }
}

void MoqtSession::Error(MoqtError code, absl::string_view error) {
  if (!error_.empty()) {
    // Avoid erroring out twice.
    return;
  }
  QUICHE_DLOG(INFO) << ENDPOINT << "MOQT session closed with code: "
                    << static_cast<int>(code) << " and message: " << error;
  error_ = std::string(error);
  session_->CloseSession(static_cast<uint64_t>(code), error);
  std::move(callbacks_.session_terminated_callback)(error);
}

// TODO: Create state that allows ANNOUNCE_OK/ERROR on spurious namespaces to
// trigger session errors.
void MoqtSession::Announce(FullTrackName track_namespace,
                           MoqtOutgoingAnnounceCallback announce_callback) {
  if (peer_role_ == MoqtRole::kPublisher) {
    std::move(announce_callback)(
        track_namespace,
        MoqtAnnounceErrorReason{MoqtAnnounceErrorCode::kInternalError,
                                "ANNOUNCE cannot be sent to Publisher"});
    return;
  }
  if (pending_outgoing_announces_.contains(track_namespace)) {
    std::move(announce_callback)(
        track_namespace,
        MoqtAnnounceErrorReason{
            MoqtAnnounceErrorCode::kInternalError,
            "ANNOUNCE message already outstanding for namespace"});
    return;
  }
  MoqtAnnounce message;
  message.track_namespace = track_namespace;
  SendControlMessage(framer_.SerializeAnnounce(message));
  QUIC_DLOG(INFO) << ENDPOINT << "Sent ANNOUNCE message for "
                  << message.track_namespace;
  pending_outgoing_announces_[track_namespace] = std::move(announce_callback);
}

bool MoqtSession::SubscribeAbsolute(const FullTrackName& name,
                                    uint64_t start_group, uint64_t start_object,
                                    SubscribeRemoteTrack::Visitor* visitor,
                                    MoqtSubscribeParameters parameters) {
  MoqtSubscribe message;
  message.full_track_name = name;
  message.subscriber_priority = kDefaultSubscriberPriority;
  message.group_order = std::nullopt;
  message.start_group = start_group;
  message.start_object = start_object;
  message.end_group = std::nullopt;
  message.end_object = std::nullopt;
  message.parameters = std::move(parameters);
  return Subscribe(message, visitor);
}

bool MoqtSession::SubscribeAbsolute(const FullTrackName& name,
                                    uint64_t start_group, uint64_t start_object,
                                    uint64_t end_group,
                                    SubscribeRemoteTrack::Visitor* visitor,
                                    MoqtSubscribeParameters parameters) {
  if (end_group < start_group) {
    QUIC_DLOG(ERROR) << "Subscription end is before beginning";
    return false;
  }
  MoqtSubscribe message;
  message.full_track_name = name;
  message.subscriber_priority = kDefaultSubscriberPriority;
  message.group_order = std::nullopt;
  message.start_group = start_group;
  message.start_object = start_object;
  message.end_group = end_group;
  message.end_object = std::nullopt;
  message.parameters = std::move(parameters);
  return Subscribe(message, visitor);
}

bool MoqtSession::SubscribeAbsolute(const FullTrackName& name,
                                    uint64_t start_group, uint64_t start_object,
                                    uint64_t end_group, uint64_t end_object,
                                    SubscribeRemoteTrack::Visitor* visitor,
                                    MoqtSubscribeParameters parameters) {
  if (end_group < start_group) {
    QUIC_DLOG(ERROR) << "Subscription end is before beginning";
    return false;
  }
  if (end_group == start_group && end_object < start_object) {
    QUIC_DLOG(ERROR) << "Subscription end is before beginning";
    return false;
  }
  MoqtSubscribe message;
  message.full_track_name = name;
  message.subscriber_priority = kDefaultSubscriberPriority;
  message.group_order = std::nullopt;
  message.start_group = start_group;
  message.start_object = start_object;
  message.end_group = end_group;
  message.end_object = end_object;
  message.parameters = std::move(parameters);
  return Subscribe(message, visitor);
}

bool MoqtSession::SubscribeCurrentObject(const FullTrackName& name,
                                         SubscribeRemoteTrack::Visitor* visitor,
                                         MoqtSubscribeParameters parameters) {
  MoqtSubscribe message;
  message.full_track_name = name;
  message.subscriber_priority = kDefaultSubscriberPriority;
  message.group_order = std::nullopt;
  message.start_group = std::nullopt;
  message.start_object = std::nullopt;
  message.end_group = std::nullopt;
  message.end_object = std::nullopt;
  message.parameters = std::move(parameters);
  return Subscribe(message, visitor);
}

bool MoqtSession::SubscribeCurrentGroup(const FullTrackName& name,
                                        SubscribeRemoteTrack::Visitor* visitor,
                                        MoqtSubscribeParameters parameters) {
  MoqtSubscribe message;
  message.full_track_name = name;
  message.subscriber_priority = kDefaultSubscriberPriority;
  message.group_order = std::nullopt;
  // First object of current group.
  message.start_group = std::nullopt;
  message.start_object = 0;
  message.end_group = std::nullopt;
  message.end_object = std::nullopt;
  message.parameters = std::move(parameters);
  return Subscribe(message, visitor);
}

void MoqtSession::Unsubscribe(const FullTrackName& name) {
  RemoteTrack* track = RemoteTrackByName(name);
  if (track == nullptr) {
    return;
  }
  MoqtUnsubscribe message;
  message.subscribe_id = track->subscribe_id();
  SendControlMessage(framer_.SerializeUnsubscribe(message));
  // Destroy state.
  upstream_by_name_.erase(name);
  upstream_by_id_.erase(track->subscribe_id());
  subscribe_by_alias_.erase(
      static_cast<SubscribeRemoteTrack*>(track)->track_alias());
}

void MoqtSession::PublishedFetch::FetchStreamVisitor::OnCanWrite() {
  std::shared_ptr<PublishedFetch> fetch = fetch_.lock();
  if (fetch == nullptr) {
    return;
  }
  PublishedObject object;
  while (stream_->CanWrite()) {
    MoqtFetchTask::GetNextObjectResult result =
        fetch->fetch_task()->GetNextObject(object);
    switch (result) {
      case MoqtFetchTask::GetNextObjectResult::kSuccess:
        // Skip ObjectDoesNotExist in FETCH.
        if (object.status == MoqtObjectStatus::kObjectDoesNotExist) {
          continue;
        }
        if (fetch->session_->WriteObjectToStream(
                stream_, fetch->fetch_id_, object,
                MoqtDataStreamType::kStreamHeaderFetch, !stream_header_written_,
                /*fin=*/false)) {
          stream_header_written_ = true;
        }
        break;
      case MoqtFetchTask::GetNextObjectResult::kPending:
        return;
      case MoqtFetchTask::GetNextObjectResult::kEof:
        // TODO(martinduke): Either prefetch the next object, or alter the API
        // so that we're not sending FIN in a separate frame.
        if (!quiche::SendFinOnStream(*stream_).ok()) {
          QUIC_DVLOG(1) << "Sending FIN onStream " << stream_->GetStreamId()
                        << " failed";
        }
        return;
      case MoqtFetchTask::GetNextObjectResult::kError:
        stream_->ResetWithUserCode(static_cast<webtransport::StreamErrorCode>(
            fetch->fetch_task()->GetStatus().code()));
        return;
    }
  }
}

bool MoqtSession::SubscribeIsDone(uint64_t subscribe_id, SubscribeDoneCode code,
                                  absl::string_view reason_phrase) {
  auto it = published_subscriptions_.find(subscribe_id);
  if (it == published_subscriptions_.end()) {
    return false;
  }

  PublishedSubscription& subscription = *it->second;
  std::vector<webtransport::StreamId> streams_to_reset =
      subscription.GetAllStreams();

  MoqtSubscribeDone subscribe_done;
  subscribe_done.subscribe_id = subscribe_id;
  subscribe_done.status_code = code;
  subscribe_done.reason_phrase = reason_phrase;
  subscribe_done.final_id = subscription.largest_sent();
  SendControlMessage(framer_.SerializeSubscribeDone(subscribe_done));
  QUIC_DLOG(INFO) << ENDPOINT << "Sent SUBSCRIBE_DONE message for "
                  << subscribe_id;
  // Clean up the subscription
  published_subscriptions_.erase(it);
  for (webtransport::StreamId stream_id : streams_to_reset) {
    webtransport::Stream* stream = session_->GetStreamById(stream_id);
    if (stream == nullptr) {
      continue;
    }
    stream->ResetWithUserCode(kResetCodeSubscriptionGone);
  }
  return true;
}

bool MoqtSession::Subscribe(MoqtSubscribe& message,
                            SubscribeRemoteTrack::Visitor* visitor,
                            std::optional<uint64_t> provided_track_alias) {
  if (peer_role_ == MoqtRole::kSubscriber) {
    QUIC_DLOG(INFO) << ENDPOINT << "Tried to send SUBSCRIBE to subscriber peer";
    return false;
  }
  // TODO(martinduke): support authorization info
  if (next_subscribe_id_ >= peer_max_subscribe_id_) {
    QUIC_DLOG(INFO) << ENDPOINT << "Tried to send SUBSCRIBE with ID "
                    << next_subscribe_id_
                    << " which is greater than the maximum ID "
                    << peer_max_subscribe_id_;
    return false;
  }
  if (upstream_by_name_.contains(message.full_track_name)) {
    QUIC_DLOG(INFO) << ENDPOINT << "Tried to send SUBSCRIBE for track "
                    << message.full_track_name
                    << " which is already subscribed";
    return false;
  }
  if (provided_track_alias.has_value() &&
      subscribe_by_alias_.contains(*provided_track_alias)) {
    Error(MoqtError::kProtocolViolation, "Provided track alias already in use");
    return false;
  }
  message.subscribe_id = next_subscribe_id_++;
  message.track_alias =
      provided_track_alias.value_or(next_remote_track_alias_++);
  if (SupportsObjectAck() && visitor != nullptr) {
    // Since we do not expose subscribe IDs directly in the API, instead wrap
    // the session and subscribe ID in a callback.
    visitor->OnCanAckObjects(absl::bind_front(&MoqtSession::SendObjectAck, this,
                                              message.subscribe_id));
  } else {
    QUICHE_DLOG_IF(WARNING, message.parameters.object_ack_window.has_value())
        << "Attempting to set object_ack_window on a connection that does not "
           "support it.";
    message.parameters.object_ack_window = std::nullopt;
  }
  SendControlMessage(framer_.SerializeSubscribe(message));
  QUIC_DLOG(INFO) << ENDPOINT << "Sent SUBSCRIBE message for "
                  << message.full_track_name;
  auto track = std::make_unique<SubscribeRemoteTrack>(message, visitor);
  upstream_by_name_.emplace(message.full_track_name, track.get());
  upstream_by_id_.emplace(message.subscribe_id, track.get());
  subscribe_by_alias_.emplace(message.track_alias, std::move(track));
  return true;
}

webtransport::Stream* MoqtSession::OpenOrQueueDataStream(
    uint64_t subscription_id, FullSequence first_object) {
  auto it = published_subscriptions_.find(subscription_id);
  if (it == published_subscriptions_.end()) {
    // It is possible that the subscription has been discarded while the stream
    // was in the queue; discard those streams.
    return nullptr;
  }
  PublishedSubscription& subscription = *it->second;
  if (!session_->CanOpenNextOutgoingUnidirectionalStream()) {
    subscription.AddQueuedOutgoingDataStream(first_object);
    // The subscription will notify the session about how to update the
    // session's queue.
    // TODO: limit the number of streams in the queue.
    return nullptr;
  }
  return OpenDataStream(subscription, first_object);
}

webtransport::Stream* MoqtSession::OpenDataStream(
    PublishedSubscription& subscription, FullSequence first_object) {
  webtransport::Stream* new_stream =
      session_->OpenOutgoingUnidirectionalStream();
  if (new_stream == nullptr) {
    QUICHE_BUG(MoqtSession_OpenDataStream_blocked)
        << "OpenDataStream called when creation of new streams is blocked.";
    return nullptr;
  }
  new_stream->SetVisitor(std::make_unique<OutgoingDataStream>(
      this, new_stream, subscription, first_object));
  subscription.OnDataStreamCreated(new_stream->GetStreamId(), first_object);
  return new_stream;
}

bool MoqtSession::OpenDataStream(std::shared_ptr<PublishedFetch> fetch) {
  webtransport::Stream* new_stream =
      session_->OpenOutgoingUnidirectionalStream();
  if (new_stream == nullptr) {
    QUICHE_BUG(MoqtSession_OpenDataStream_blocked)
        << "OpenDataStream called when creation of new streams is blocked.";
    return false;
  }
  fetch->SetStreamId(new_stream->GetStreamId());
  new_stream->SetVisitor(
      std::make_unique<PublishedFetch::FetchStreamVisitor>(fetch, new_stream));
  if (new_stream->CanWrite()) {
    new_stream->visitor()->OnCanWrite();
  }
  return true;
}

SubscribeRemoteTrack* MoqtSession::RemoteTrackByAlias(uint64_t track_alias) {
  auto it = subscribe_by_alias_.find(track_alias);
  if (it == subscribe_by_alias_.end()) {
    return nullptr;
  }
  return it->second.get();
}

RemoteTrack* MoqtSession::RemoteTrackById(uint64_t subscribe_id) {
  auto it = upstream_by_id_.find(subscribe_id);
  if (it == upstream_by_id_.end()) {
    return nullptr;
  }
  return it->second;
}

RemoteTrack* MoqtSession::RemoteTrackByName(const FullTrackName& name) {
  auto it = upstream_by_name_.find(name);
  if (it == upstream_by_name_.end()) {
    return nullptr;
  }
  return it->second;
}

void MoqtSession::OnCanCreateNewOutgoingUnidirectionalStream() {
  while (!subscribes_with_queued_outgoing_data_streams_.empty() &&
         session_->CanOpenNextOutgoingUnidirectionalStream()) {
    auto next = subscribes_with_queued_outgoing_data_streams_.rbegin();
    auto subscription = published_subscriptions_.find(next->subscription_id);
    if (subscription == published_subscriptions_.end()) {
      auto fetch = incoming_fetches_.find(next->subscription_id);
      // Create the stream if the fetch still exists.
      if (fetch != incoming_fetches_.end() && !OpenDataStream(fetch->second)) {
        return;  // A QUIC_BUG has fired because this shouldn't happen.
      }
      // FETCH needs only one stream, and can be deleted from the queue. Or,
      // there is no subscribe and no fetch; the entry in the queue is invalid.
      subscribes_with_queued_outgoing_data_streams_.erase((++next).base());
      continue;
    }
    // Open the stream. The second argument pops the item from the
    // subscription's queue, which might update
    // subscribes_with_queued_outgoing_data_streams_.
    webtransport::Stream* stream =
        OpenDataStream(*subscription->second,
                       subscription->second->NextQueuedOutgoingDataStream());
    if (stream != nullptr) {
      stream->visitor()->OnCanWrite();
    }
  }
}

void MoqtSession::UpdateQueuedSendOrder(
    uint64_t subscribe_id,
    std::optional<webtransport::SendOrder> old_send_order,
    std::optional<webtransport::SendOrder> new_send_order) {
  if (old_send_order == new_send_order) {
    return;
  }
  if (old_send_order.has_value()) {
    subscribes_with_queued_outgoing_data_streams_.erase(
        SubscriptionWithQueuedStream{*old_send_order, subscribe_id});
  }
  if (new_send_order.has_value()) {
    subscribes_with_queued_outgoing_data_streams_.emplace(*new_send_order,
                                                          subscribe_id);
  }
}

void MoqtSession::GrantMoreSubscribes(uint64_t num_subscribes) {
  local_max_subscribe_id_ += num_subscribes;
  MoqtMaxSubscribeId message;
  message.max_subscribe_id = local_max_subscribe_id_;
  SendControlMessage(framer_.SerializeMaxSubscribeId(message));
}

bool MoqtSession::ValidateSubscribeId(uint64_t subscribe_id) {
  if (peer_role_ == MoqtRole::kPublisher) {
    QUIC_DLOG(INFO) << ENDPOINT << "Publisher peer sent SUBSCRIBE";
    Error(MoqtError::kProtocolViolation, "Received SUBSCRIBE from publisher");
    return false;
  }
  if (subscribe_id >= local_max_subscribe_id_) {
    QUIC_DLOG(INFO) << ENDPOINT << "Received SUBSCRIBE with too large ID";
    Error(MoqtError::kTooManySubscribes,
          "Received SUBSCRIBE with too large ID");
    return false;
  }
  if (subscribe_id < next_incoming_subscribe_id_) {
    QUIC_DLOG(INFO) << ENDPOINT << "Subscribe ID not monotonically increasing";
    Error(MoqtError::kProtocolViolation,
          "Subscribe ID not monotonically increasing");
    return false;
  }
  next_incoming_subscribe_id_ = subscribe_id + 1;
  return true;
}

template <class Parser>
static void ForwardStreamDataToParser(webtransport::Stream& stream,
                                      Parser& parser) {
  bool fin =
      quiche::ProcessAllReadableRegions(stream, [&](absl::string_view chunk) {
        parser.ProcessData(chunk, /*end_of_stream=*/false);
      });
  if (fin) {
    parser.ProcessData("", /*end_of_stream=*/true);
  }
}

MoqtSession::ControlStream::ControlStream(MoqtSession* session,
                                          webtransport::Stream* stream)
    : session_(session),
      stream_(stream),
      parser_(session->parameters_.using_webtrans, *this) {
  stream_->SetPriority(
      webtransport::StreamPriority{/*send_group_id=*/kMoqtSendGroupId,
                                   /*send_order=*/kMoqtControlStreamSendOrder});
}

void MoqtSession::ControlStream::OnCanRead() {
  ForwardStreamDataToParser(*stream_, parser_);
}
void MoqtSession::ControlStream::OnCanWrite() {
  // We buffer serialized control frames unconditionally, thus OnCanWrite()
  // requires no handling for control streams.
}

void MoqtSession::ControlStream::OnResetStreamReceived(
    webtransport::StreamErrorCode error) {
  session_->Error(MoqtError::kProtocolViolation,
                  absl::StrCat("Control stream reset with error code ", error));
}
void MoqtSession::ControlStream::OnStopSendingReceived(
    webtransport::StreamErrorCode error) {
  session_->Error(MoqtError::kProtocolViolation,
                  absl::StrCat("Control stream reset with error code ", error));
}

void MoqtSession::ControlStream::OnClientSetupMessage(
    const MoqtClientSetup& message) {
  session_->control_stream_ = stream_->GetStreamId();
  if (perspective() == Perspective::IS_CLIENT) {
    session_->Error(MoqtError::kProtocolViolation,
                    "Received CLIENT_SETUP from server");
    return;
  }
  if (absl::c_find(message.supported_versions, session_->parameters_.version) ==
      message.supported_versions.end()) {
    // TODO(martinduke): Is this the right error code? See issue #346.
    session_->Error(MoqtError::kProtocolViolation,
                    absl::StrCat("Version mismatch: expected 0x",
                                 absl::Hex(session_->parameters_.version)));
    return;
  }
  session_->peer_supports_object_ack_ = message.supports_object_ack;
  QUICHE_DLOG(INFO) << ENDPOINT << "Received the SETUP message";
  if (session_->parameters_.perspective == Perspective::IS_SERVER) {
    MoqtServerSetup response;
    response.selected_version = session_->parameters_.version;
    response.role = MoqtRole::kPubSub;
    response.max_subscribe_id = session_->parameters_.max_subscribe_id;
    response.supports_object_ack = session_->parameters_.support_object_acks;
    SendOrBufferMessage(session_->framer_.SerializeServerSetup(response));
    QUIC_DLOG(INFO) << ENDPOINT << "Sent the SETUP message";
  }
  // TODO: handle role and path.
  if (message.max_subscribe_id.has_value()) {
    session_->peer_max_subscribe_id_ = *message.max_subscribe_id;
  }
  std::move(session_->callbacks_.session_established_callback)();
  session_->peer_role_ = *message.role;
}

void MoqtSession::ControlStream::OnServerSetupMessage(
    const MoqtServerSetup& message) {
  if (perspective() == Perspective::IS_SERVER) {
    session_->Error(MoqtError::kProtocolViolation,
                    "Received SERVER_SETUP from client");
    return;
  }
  if (message.selected_version != session_->parameters_.version) {
    // TODO(martinduke): Is this the right error code? See issue #346.
    session_->Error(MoqtError::kProtocolViolation,
                    absl::StrCat("Version mismatch: expected 0x",
                                 absl::Hex(session_->parameters_.version)));
    return;
  }
  session_->peer_supports_object_ack_ = message.supports_object_ack;
  QUIC_DLOG(INFO) << ENDPOINT << "Received the SETUP message";
  // TODO: handle role and path.
  if (message.max_subscribe_id.has_value()) {
    session_->peer_max_subscribe_id_ = *message.max_subscribe_id;
  }
  std::move(session_->callbacks_.session_established_callback)();
  session_->peer_role_ = *message.role;
}

void MoqtSession::ControlStream::SendSubscribeError(
    const MoqtSubscribe& message, SubscribeErrorCode error_code,
    absl::string_view reason_phrase, uint64_t track_alias) {
  MoqtSubscribeError subscribe_error;
  subscribe_error.subscribe_id = message.subscribe_id;
  subscribe_error.error_code = error_code;
  subscribe_error.reason_phrase = reason_phrase;
  subscribe_error.track_alias = track_alias;
  SendOrBufferMessage(
      session_->framer_.SerializeSubscribeError(subscribe_error));
}

void MoqtSession::ControlStream::SendFetchError(
    uint64_t subscribe_id, SubscribeErrorCode error_code,
    absl::string_view reason_phrase) {
  MoqtFetchError fetch_error;
  fetch_error.subscribe_id = subscribe_id;
  fetch_error.error_code = error_code;
  fetch_error.reason_phrase = reason_phrase;
  SendOrBufferMessage(session_->framer_.SerializeFetchError(fetch_error));
}

void MoqtSession::ControlStream::OnSubscribeMessage(
    const MoqtSubscribe& message) {
  if (!session_->ValidateSubscribeId(message.subscribe_id)) {
    return;
  }
  QUIC_DLOG(INFO) << ENDPOINT << "Received a SUBSCRIBE for "
                  << message.full_track_name;

  const FullTrackName& track_name = message.full_track_name;
  absl::StatusOr<std::shared_ptr<MoqtTrackPublisher>> track_publisher =
      session_->publisher_->GetTrack(track_name);
  if (!track_publisher.ok()) {
    QUIC_DLOG(INFO) << ENDPOINT << "SUBSCRIBE for " << track_name
                    << " rejected by the application: "
                    << track_publisher.status();
    SendSubscribeError(message, SubscribeErrorCode::kTrackDoesNotExist,
                       track_publisher.status().message(), message.track_alias);
    return;
  }
  std::optional<FullSequence> largest_id;
  if (PublisherHasData(**track_publisher)) {
    largest_id = (*track_publisher)->GetLargestSequence();
  }
  if (message.start_group.has_value() && largest_id.has_value() &&
      *message.start_group < largest_id->group) {
    SendSubscribeError(message, SubscribeErrorCode::kInvalidRange,
                       "SUBSCRIBE starts in previous group",
                       message.track_alias);
    return;
  }
  MoqtDeliveryOrder delivery_order = (*track_publisher)->GetDeliveryOrder();

  MoqtPublishingMonitorInterface* monitoring = nullptr;
  auto monitoring_it =
      session_->monitoring_interfaces_for_published_tracks_.find(track_name);
  if (monitoring_it !=
      session_->monitoring_interfaces_for_published_tracks_.end()) {
    monitoring = monitoring_it->second;
    session_->monitoring_interfaces_for_published_tracks_.erase(monitoring_it);
  }

  if (session_->subscribed_track_names_.contains(track_name)) {
    session_->Error(MoqtError::kProtocolViolation,
                    "Duplicate subscribe for track");
    return;
  }
  auto subscription = std::make_unique<MoqtSession::PublishedSubscription>(
      session_, *std::move(track_publisher), message, monitoring);
  auto [it, success] = session_->published_subscriptions_.emplace(
      message.subscribe_id, std::move(subscription));
  if (!success) {
    SendSubscribeError(message, SubscribeErrorCode::kInternalError,
                       "Duplicate subscribe ID", message.track_alias);
    return;
  }

  MoqtSubscribeOk subscribe_ok;
  subscribe_ok.subscribe_id = message.subscribe_id;
  subscribe_ok.group_order = delivery_order;
  subscribe_ok.largest_id = largest_id;
  SendOrBufferMessage(session_->framer_.SerializeSubscribeOk(subscribe_ok));

  if (largest_id.has_value()) {
    it->second->Backfill();
  }
}

void MoqtSession::ControlStream::OnSubscribeOkMessage(
    const MoqtSubscribeOk& message) {
  RemoteTrack* track = session_->RemoteTrackById(message.subscribe_id);
  if (track == nullptr) {
    QUIC_DLOG(INFO) << ENDPOINT << "Received the SUBSCRIBE_OK for "
                    << "subscribe_id = " << message.subscribe_id
                    << " but no track exists";
    // Subscription state might have been destroyed for internal reasons.
    return;
  }
  if (track->is_fetch()) {
    session_->Error(MoqtError::kProtocolViolation,
                    "Received SUBSCRIBE_OK for a FETCH");
    return;
  }
  QUIC_DLOG(INFO) << ENDPOINT << "Received the SUBSCRIBE_OK for "
                  << "subscribe_id = " << message.subscribe_id << " "
                  << track->full_track_name();
  SubscribeRemoteTrack* subscribe = static_cast<SubscribeRemoteTrack*>(track);
  subscribe->OnObjectOrOk();
  // TODO(martinduke): Handle expires field.
  // TODO(martinduke): Resize the window based on largest_id.
  if (subscribe->visitor() != nullptr) {
    subscribe->visitor()->OnReply(track->full_track_name(), message.largest_id,
                                  std::nullopt);
  }
  subscribe->OnObjectOrOk();
}

void MoqtSession::ControlStream::OnSubscribeErrorMessage(
    const MoqtSubscribeError& message) {
  RemoteTrack* track = session_->RemoteTrackById(message.subscribe_id);
  if (track == nullptr) {
    QUIC_DLOG(INFO) << ENDPOINT << "Received the SUBSCRIBE_ERROR for "
                    << "subscribe_id = " << message.subscribe_id
                    << " but no track exists";
    // Subscription state might have been destroyed for internal reasons.
    return;
  }
  if (track->is_fetch()) {
    session_->Error(MoqtError::kProtocolViolation,
                    "Received SUBSCRIBE_ERROR for a FETCH");
    return;
  }
  if (!track->ErrorIsAllowed()) {
    session_->Error(MoqtError::kProtocolViolation,
                    "Received SUBSCRIBE_ERROR after SUBSCRIBE_OK or objects");
    return;
  }
  QUIC_DLOG(INFO) << ENDPOINT << "Received the SUBSCRIBE_ERROR for "
                  << "subscribe_id = " << message.subscribe_id << " ("
                  << track->full_track_name() << ")"
                  << ", error = " << static_cast<int>(message.error_code)
                  << " (" << message.reason_phrase << ")";
  SubscribeRemoteTrack* subscribe = static_cast<SubscribeRemoteTrack*>(track);
  // Delete secondary references to the track. Preserve the owner
  // (subscribe_by_alias_) to get the original subscribe, if needed. Erasing the
  // other references now prevents an error due to a duplicate subscription in
  // Subscribe().
  session_->upstream_by_id_.erase(subscribe->subscribe_id());
  session_->upstream_by_name_.erase(subscribe->full_track_name());
  if (message.error_code == SubscribeErrorCode::kRetryTrackAlias) {
    // Automatically resubscribe with new alias.
    MoqtSubscribe& subscribe_message = subscribe->GetSubscribe();
    session_->Subscribe(subscribe_message, subscribe->visitor(),
                        message.track_alias);
  } else if (subscribe->visitor() != nullptr) {
    subscribe->visitor()->OnReply(subscribe->full_track_name(), std::nullopt,
                                  message.reason_phrase);
  }
  session_->subscribe_by_alias_.erase(subscribe->track_alias());
}

void MoqtSession::ControlStream::OnUnsubscribeMessage(
    const MoqtUnsubscribe& message) {
  session_->SubscribeIsDone(message.subscribe_id,
                            SubscribeDoneCode::kUnsubscribed, "");
}

void MoqtSession::ControlStream::OnSubscribeUpdateMessage(
    const MoqtSubscribeUpdate& message) {
  auto it = session_->published_subscriptions_.find(message.subscribe_id);
  if (it == session_->published_subscriptions_.end()) {
    return;
  }
  FullSequence start(message.start_group, message.start_object);
  std::optional<FullSequence> end;
  if (message.end_group.has_value()) {
    end = FullSequence(*message.end_group, message.end_object.has_value()
                                               ? *message.end_object
                                               : UINT64_MAX);
  }
  it->second->Update(start, end, message.subscriber_priority);
}

void MoqtSession::ControlStream::OnAnnounceMessage(
    const MoqtAnnounce& message) {
  if (session_->peer_role_ == MoqtRole::kSubscriber) {
    QUIC_DLOG(INFO) << ENDPOINT << "Subscriber peer sent SUBSCRIBE";
    session_->Error(MoqtError::kProtocolViolation,
                    "Received ANNOUNCE from Subscriber");
    return;
  }
  std::optional<MoqtAnnounceErrorReason> error =
      session_->callbacks_.incoming_announce_callback(message.track_namespace);
  if (error.has_value()) {
    MoqtAnnounceError reply;
    reply.track_namespace = message.track_namespace;
    reply.error_code = error->error_code;
    reply.reason_phrase = error->reason_phrase;
    SendOrBufferMessage(session_->framer_.SerializeAnnounceError(reply));
    return;
  }
  MoqtAnnounceOk ok;
  ok.track_namespace = message.track_namespace;
  SendOrBufferMessage(session_->framer_.SerializeAnnounceOk(ok));
}

void MoqtSession::ControlStream::OnAnnounceOkMessage(
    const MoqtAnnounceOk& message) {
  auto it = session_->pending_outgoing_announces_.find(message.track_namespace);
  if (it == session_->pending_outgoing_announces_.end()) {
    session_->Error(MoqtError::kProtocolViolation,
                    "Received ANNOUNCE_OK for nonexistent announce");
    return;
  }
  std::move(it->second)(message.track_namespace, std::nullopt);
  session_->pending_outgoing_announces_.erase(it);
}

void MoqtSession::ControlStream::OnAnnounceErrorMessage(
    const MoqtAnnounceError& message) {
  auto it = session_->pending_outgoing_announces_.find(message.track_namespace);
  if (it == session_->pending_outgoing_announces_.end()) {
    session_->Error(MoqtError::kProtocolViolation,
                    "Received ANNOUNCE_ERROR for nonexistent announce");
    return;
  }
  std::move(it->second)(
      message.track_namespace,
      MoqtAnnounceErrorReason{message.error_code,
                              std::string(message.reason_phrase)});
  session_->pending_outgoing_announces_.erase(it);
}

void MoqtSession::ControlStream::OnAnnounceCancelMessage(
    const MoqtAnnounceCancel& message) {
  // TODO: notify the application about this.
}

void MoqtSession::ControlStream::OnMaxSubscribeIdMessage(
    const MoqtMaxSubscribeId& message) {
  if (session_->peer_role_ == MoqtRole::kSubscriber) {
    QUIC_DLOG(INFO) << ENDPOINT << "Subscriber peer sent MAX_SUBSCRIBE_ID";
    session_->Error(MoqtError::kProtocolViolation,
                    "Received MAX_SUBSCRIBE_ID from Subscriber");
    return;
  }
  if (message.max_subscribe_id < session_->peer_max_subscribe_id_) {
    QUIC_DLOG(INFO) << ENDPOINT
                    << "Peer sent MAX_SUBSCRIBE_ID message with "
                       "lower value than previous";
    session_->Error(MoqtError::kProtocolViolation,
                    "MAX_SUBSCRIBE_ID message has lower value than previous");
    return;
  }
  session_->peer_max_subscribe_id_ = message.max_subscribe_id;
}

void MoqtSession::ControlStream::OnFetchMessage(const MoqtFetch& message) {
  if (!session_->ValidateSubscribeId(message.subscribe_id)) {
    return;
  }
  QUIC_DLOG(INFO) << ENDPOINT << "Received a FETCH for "
                  << message.full_track_name;

  const FullTrackName& track_name = message.full_track_name;
  absl::StatusOr<std::shared_ptr<MoqtTrackPublisher>> track_publisher =
      session_->publisher_->GetTrack(track_name);
  if (!track_publisher.ok()) {
    QUIC_DLOG(INFO) << ENDPOINT << "FETCH for " << track_name
                    << " rejected by the application: "
                    << track_publisher.status();
    SendFetchError(message.subscribe_id, SubscribeErrorCode::kTrackDoesNotExist,
                   track_publisher.status().message());
    return;
  }
  std::unique_ptr<MoqtFetchTask> fetch =
      (*track_publisher)
          ->Fetch(message.start_object, message.end_group, message.end_object,
                  message.group_order.value_or(
                      (*track_publisher)->GetDeliveryOrder()));
  if (!fetch->GetStatus().ok()) {
    QUIC_DLOG(INFO) << ENDPOINT << "FETCH for " << track_name
                    << " could not initialize the task";
    SendFetchError(message.subscribe_id, SubscribeErrorCode::kInvalidRange,
                   fetch->GetStatus().message());
    return;
  }
  auto published_fetch = std::make_unique<PublishedFetch>(
      message.subscribe_id, session_, std::move(fetch));
  auto result = session_->incoming_fetches_.emplace(message.subscribe_id,
                                                    std::move(published_fetch));
  if (!result.second) {  // Emplace failed.
    QUIC_DLOG(INFO) << ENDPOINT << "FETCH for " << track_name
                    << " could not be added to the session";
    SendFetchError(message.subscribe_id, SubscribeErrorCode::kInternalError,
                   "Could not initialize FETCH state");
    return;
  }
  MoqtFetchOk fetch_ok;
  fetch_ok.subscribe_id = message.subscribe_id;
  fetch_ok.group_order =
      message.group_order.value_or((*track_publisher)->GetDeliveryOrder());
  fetch_ok.largest_id = result.first->second->fetch_task()->GetLargestId();
  SendOrBufferMessage(session_->framer_.SerializeFetchOk(fetch_ok));
  if (!session_->session()->CanOpenNextOutgoingUnidirectionalStream() ||
      !session_->OpenDataStream(result.first->second)) {
    // Put the FETCH in the queue for a new stream.
    session_->UpdateQueuedSendOrder(
        message.subscribe_id, std::nullopt,
        SendOrderForStream(message.subscriber_priority,
                           (*track_publisher)->GetPublisherPriority(),
                           /*group_id=*/0,
                           message.group_order.value_or(
                               (*track_publisher)->GetDeliveryOrder())));
  }
}

void MoqtSession::ControlStream::OnParsingError(MoqtError error_code,
                                                absl::string_view reason) {
  session_->Error(error_code, absl::StrCat("Parse error: ", reason));
}

void MoqtSession::ControlStream::SendOrBufferMessage(
    quiche::QuicheBuffer message, bool fin) {
  quiche::StreamWriteOptions options;
  options.set_send_fin(fin);
  // TODO: while we buffer unconditionally, we should still at some point tear
  // down the connection if we've buffered too many control messages; otherwise,
  // there is potential for memory exhaustion attacks.
  options.set_buffer_unconditionally(true);
  std::array<absl::string_view, 1> write_vector = {message.AsStringView()};
  absl::Status success = stream_->Writev(absl::MakeSpan(write_vector), options);
  if (!success.ok()) {
    session_->Error(MoqtError::kInternalError,
                    "Failed to write a control message");
  }
}

void MoqtSession::IncomingDataStream::OnObjectMessage(const MoqtObject& message,
                                                      absl::string_view payload,
                                                      bool end_of_message) {
  QUICHE_DVLOG(1) << ENDPOINT << "Received OBJECT message on stream "
                  << stream_->GetStreamId() << " for track alias "
                  << message.track_alias << " with sequence "
                  << message.group_id << ":" << message.object_id
                  << " priority " << message.publisher_priority << " length "
                  << payload.size() << " length " << message.payload_length
                  << (end_of_message ? "F" : "");
  if (!session_->parameters_.deliver_partial_objects) {
    if (!end_of_message) {  // Buffer partial object.
      if (partial_object_.empty()) {
        // Avoid redundant allocations by reserving the appropriate amount of
        // memory if known.
        partial_object_.reserve(message.payload_length);
      }
      absl::StrAppend(&partial_object_, payload);
      return;
    }
    if (!partial_object_.empty()) {  // Completes the object
      absl::StrAppend(&partial_object_, payload);
      payload = absl::string_view(partial_object_);
    }
  }
  QUICHE_BUG_IF(quic_bug_object_with_no_stream_type,
                !parser_.stream_type().has_value())
      << "Object delivered without a stream type";
  // Get a pointer to the upstream state.
  RemoteTrack* track = track_.GetIfAvailable();
  if (track == nullptr) {
    track = (*parser_.stream_type() == MoqtDataStreamType::kStreamHeaderFetch)
                // message.track_alias is actually a fetch ID for fetches.
                ? session_->RemoteTrackById(message.track_alias)
                : session_->RemoteTrackByAlias(message.track_alias);
    if (track == nullptr) {
      stream_->SendStopSending(kResetCodeSubscriptionGone);
      // Received object for nonexistent track.
      return;
    }
    track_ = track->weak_ptr();
  }
  if (!track->CheckDataStreamType(*parser_.stream_type())) {
    session_->Error(MoqtError::kProtocolViolation,
                    "Received object for a track with a different stream type");
    return;
  }
  if (!track->InWindow(FullSequence(message.group_id, message.object_id))) {
    // This is not an error. It can be the result of a recent SUBSCRIBE_UPDATE.
    return;
  }
  track->OnObjectOrOk();
  SubscribeRemoteTrack* subscribe = static_cast<SubscribeRemoteTrack*>(track);
  if (subscribe->visitor() != nullptr) {
    subscribe->visitor()->OnObjectFragment(
        track->full_track_name(),
        FullSequence{message.group_id, message.subgroup_id.value_or(0),
                     message.object_id},
        message.publisher_priority, message.object_status, payload,
        end_of_message);
  }
  partial_object_.clear();
}

void MoqtSession::IncomingDataStream::OnCanRead() { parser_.ReadAllData(); }

void MoqtSession::IncomingDataStream::OnControlMessageReceived() {
  session_->Error(MoqtError::kProtocolViolation,
                  "Received a control message on a data stream");
}

void MoqtSession::IncomingDataStream::OnParsingError(MoqtError error_code,
                                                     absl::string_view reason) {
  session_->Error(error_code, absl::StrCat("Parse error: ", reason));
}

MoqtSession::PublishedSubscription::PublishedSubscription(
    MoqtSession* session, std::shared_ptr<MoqtTrackPublisher> track_publisher,
    const MoqtSubscribe& subscribe,
    MoqtPublishingMonitorInterface* monitoring_interface)
    : subscription_id_(subscribe.subscribe_id),
      session_(session),
      track_publisher_(track_publisher),
      track_alias_(subscribe.track_alias),
      window_(SubscribeMessageToWindow(subscribe, *track_publisher)),
      subscriber_priority_(subscribe.subscriber_priority),
      subscriber_delivery_order_(subscribe.group_order),
      monitoring_interface_(monitoring_interface) {
  track_publisher->AddObjectListener(this);
  if (monitoring_interface_ != nullptr) {
    monitoring_interface_->OnObjectAckSupportKnown(
        subscribe.parameters.object_ack_window.has_value());
  }
  QUIC_DLOG(INFO) << ENDPOINT << "Created subscription for "
                  << subscribe.full_track_name;
  session_->subscribed_track_names_.insert(subscribe.full_track_name);
}

MoqtSession::PublishedSubscription::~PublishedSubscription() {
  track_publisher_->RemoveObjectListener(this);
  session_->subscribed_track_names_.erase(track_publisher_->GetTrackName());
}

SendStreamMap& MoqtSession::PublishedSubscription::stream_map() {
  // The stream map is lazily initialized, since initializing it requires
  // knowing the forwarding preference in advance, and it might not be known
  // when the subscription is first created.
  if (!lazily_initialized_stream_map_.has_value()) {
    QUICHE_DCHECK(
        DoesTrackStatusImplyHavingData(*track_publisher_->GetTrackStatus()));
    lazily_initialized_stream_map_.emplace(
        track_publisher_->GetForwardingPreference());
  }
  return *lazily_initialized_stream_map_;
}

void MoqtSession::PublishedSubscription::Update(
    FullSequence start, std::optional<FullSequence> end,
    MoqtPriority subscriber_priority) {
  window_.UpdateStartEnd(start, end);
  subscriber_priority_ = subscriber_priority;
  // TODO: update priority of all data streams that are currently open.

  // TODO: reset streams that are no longer in-window.
  // TODO: send SUBSCRIBE_DONE if required.
  // TODO: send an error for invalid updates now that it's a part of draft-05.
}

void MoqtSession::PublishedSubscription::set_subscriber_priority(
    MoqtPriority priority) {
  if (priority == subscriber_priority_) {
    return;
  }
  if (queued_outgoing_data_streams_.empty()) {
    subscriber_priority_ = priority;
    return;
  }
  webtransport::SendOrder old_send_order =
      FinalizeSendOrder(queued_outgoing_data_streams_.rbegin()->first);
  subscriber_priority_ = priority;
  session_->UpdateQueuedSendOrder(subscription_id_, old_send_order,
                                  FinalizeSendOrder(old_send_order));
};

void MoqtSession::PublishedSubscription::OnNewObjectAvailable(
    FullSequence sequence) {
  if (!window_.InWindow(sequence)) {
    return;
  }

  MoqtForwardingPreference forwarding_preference =
      track_publisher_->GetForwardingPreference();
  if (forwarding_preference == MoqtForwardingPreference::kDatagram) {
    SendDatagram(sequence);
    return;
  }

  std::optional<webtransport::StreamId> stream_id =
      stream_map().GetStreamForSequence(sequence);
  webtransport::Stream* raw_stream = nullptr;
  if (stream_id.has_value()) {
    raw_stream = session_->session_->GetStreamById(*stream_id);
  } else {
    raw_stream = session_->OpenOrQueueDataStream(subscription_id_, sequence);
  }
  if (raw_stream == nullptr) {
    return;
  }

  OutgoingDataStream* stream =
      static_cast<OutgoingDataStream*>(raw_stream->visitor());
  stream->SendObjects(*this);
}

void MoqtSession::PublishedSubscription::OnTrackPublisherGone() {
  session_->SubscribeIsDone(subscription_id_, SubscribeDoneCode::kGoingAway,
                            "Publisher is gone");
}

void MoqtSession::PublishedSubscription::OnNewFinAvailable(
    FullSequence sequence) {
  if (!window_.InWindow(sequence)) {
    return;
  }
  std::optional<webtransport::StreamId> stream_id =
      stream_map().GetStreamForSequence(sequence);
  if (!stream_id.has_value()) {
    return;
  }
  webtransport::Stream* raw_stream =
      session_->session_->GetStreamById(*stream_id);
  if (raw_stream == nullptr) {
    return;
  }
  OutgoingDataStream* stream =
      static_cast<OutgoingDataStream*>(raw_stream->visitor());
  stream->Fin(sequence);
}

void MoqtSession::PublishedSubscription::OnGroupAbandoned(uint64_t group_id) {
  std::vector<webtransport::StreamId> streams =
      stream_map().GetStreamsForGroup(group_id);
  for (webtransport::StreamId stream_id : streams) {
    webtransport::Stream* raw_stream =
        session_->session_->GetStreamById(stream_id);
    if (raw_stream == nullptr) {
      continue;
    }
    raw_stream->ResetWithUserCode(kResetCodeTimedOut);
  }
}

void MoqtSession::PublishedSubscription::Backfill() {
  const FullSequence start = window_.start();
  const FullSequence end = track_publisher_->GetLargestSequence();
  const MoqtForwardingPreference preference =
      track_publisher_->GetForwardingPreference();

  absl::flat_hash_set<ReducedSequenceIndex> already_opened;
  std::vector<FullSequence> objects =
      track_publisher_->GetCachedObjectsInRange(start, end);
  QUICHE_DCHECK(absl::c_is_sorted(objects));
  for (FullSequence sequence : objects) {
    auto [it, was_missing] =
        already_opened.insert(ReducedSequenceIndex(sequence, preference));
    if (!was_missing) {
      // For every stream mapping unit present, we only need to notify of the
      // earliest object on it, since the stream itself will pull the rest.
      continue;
    }
    OnNewObjectAvailable(sequence);
  }
}

std::vector<webtransport::StreamId>
MoqtSession::PublishedSubscription::GetAllStreams() const {
  if (!lazily_initialized_stream_map_.has_value()) {
    return {};
  }
  return lazily_initialized_stream_map_->GetAllStreams();
}

webtransport::SendOrder MoqtSession::PublishedSubscription::GetSendOrder(
    FullSequence sequence) const {
  MoqtForwardingPreference forwarding_preference =
      track_publisher_->GetForwardingPreference();

  MoqtPriority publisher_priority = track_publisher_->GetPublisherPriority();
  MoqtDeliveryOrder delivery_order = subscriber_delivery_order().value_or(
      track_publisher_->GetDeliveryOrder());
  if (forwarding_preference == MoqtForwardingPreference::kDatagram) {
    QUICHE_BUG(quic_bug_GetSendOrder_for_Datagram)
        << "Datagram Track requesting SendOrder";
    return 0;
  }
  return SendOrderForStream(subscriber_priority_, publisher_priority,
                            sequence.group, sequence.subgroup, delivery_order);
}

// Returns the highest send order in the subscription.
void MoqtSession::PublishedSubscription::AddQueuedOutgoingDataStream(
    FullSequence first_object) {
  std::optional<webtransport::SendOrder> start_send_order =
      queued_outgoing_data_streams_.empty()
          ? std::optional<webtransport::SendOrder>()
          : queued_outgoing_data_streams_.rbegin()->first;
  webtransport::SendOrder send_order = GetSendOrder(first_object);
  // Zero out the subscriber priority bits, since these will be added when
  // updating the session.
  queued_outgoing_data_streams_.emplace(
      UpdateSendOrderForSubscriberPriority(send_order, 0), first_object);
  if (!start_send_order.has_value()) {
    session_->UpdateQueuedSendOrder(subscription_id_, std::nullopt, send_order);
  } else if (*start_send_order < send_order) {
    session_->UpdateQueuedSendOrder(
        subscription_id_, FinalizeSendOrder(*start_send_order), send_order);
  }
}

FullSequence
MoqtSession::PublishedSubscription::NextQueuedOutgoingDataStream() {
  QUICHE_DCHECK(!queued_outgoing_data_streams_.empty());
  if (queued_outgoing_data_streams_.empty()) {
    return FullSequence();
  }
  auto it = queued_outgoing_data_streams_.rbegin();
  webtransport::SendOrder old_send_order = FinalizeSendOrder(it->first);
  FullSequence first_object = it->second;
  // converting a reverse iterator to an iterator involves incrementing it and
  // then taking base().
  queued_outgoing_data_streams_.erase((++it).base());
  if (queued_outgoing_data_streams_.empty()) {
    session_->UpdateQueuedSendOrder(subscription_id_, old_send_order,
                                    std::nullopt);
  } else {
    webtransport::SendOrder new_send_order =
        FinalizeSendOrder(queued_outgoing_data_streams_.rbegin()->first);
    if (old_send_order != new_send_order) {
      session_->UpdateQueuedSendOrder(subscription_id_, old_send_order,
                                      new_send_order);
    }
  }
  return first_object;
}

void MoqtSession::PublishedSubscription::OnDataStreamCreated(
    webtransport::StreamId id, FullSequence start_sequence) {
  stream_map().AddStream(start_sequence, id);
}
void MoqtSession::PublishedSubscription::OnDataStreamDestroyed(
    webtransport::StreamId id, FullSequence end_sequence) {
  stream_map().RemoveStream(end_sequence, id);
}

void MoqtSession::PublishedSubscription::OnObjectSent(FullSequence sequence) {
  if (largest_sent_.has_value()) {
    largest_sent_ = std::max(*largest_sent_, sequence);
  } else {
    largest_sent_ = sequence;
  }
  // TODO: send SUBSCRIBE_DONE if the subscription is done.
}

MoqtSession::OutgoingDataStream::OutgoingDataStream(
    MoqtSession* session, webtransport::Stream* stream,
    PublishedSubscription& subscription, FullSequence first_object)
    : session_(session),
      stream_(stream),
      subscription_id_(subscription.subscription_id()),
      next_object_(first_object),
      session_liveness_(session->liveness_token_) {
  UpdateSendOrder(subscription);
}

MoqtSession::OutgoingDataStream::~OutgoingDataStream() {
  // Though it might seem intuitive that the session object has to outlive the
  // connection object (and this is indeed how something like QuicSession and
  // QuicStream works), this is not the true for WebTransport visitors: the
  // session getting destroyed will inevitably lead to all related streams being
  // destroyed, but the actual order of destruction is not guaranteed.  Thus, we
  // need to check if the session still exists while accessing it in a stream
  // destructor.
  if (session_liveness_.expired()) {
    return;
  }
  auto it = session_->published_subscriptions_.find(subscription_id_);
  if (it != session_->published_subscriptions_.end()) {
    it->second->OnDataStreamDestroyed(stream_->GetStreamId(), next_object_);
  }
}

void MoqtSession::OutgoingDataStream::OnCanWrite() {
  PublishedSubscription* subscription = GetSubscriptionIfValid();
  if (subscription == nullptr) {
    return;
  }
  SendObjects(*subscription);
}

MoqtSession::PublishedSubscription*
MoqtSession::OutgoingDataStream::GetSubscriptionIfValid() {
  auto it = session_->published_subscriptions_.find(subscription_id_);
  if (it == session_->published_subscriptions_.end()) {
    stream_->ResetWithUserCode(kResetCodeSubscriptionGone);
    return nullptr;
  }

  PublishedSubscription* subscription = it->second.get();
  MoqtTrackPublisher& publisher = subscription->publisher();
  absl::StatusOr<MoqtTrackStatusCode> status = publisher.GetTrackStatus();
  if (!status.ok()) {
    // TODO: clean up the subscription.
    return nullptr;
  }
  if (!DoesTrackStatusImplyHavingData(*status)) {
    QUICHE_BUG(GetSubscriptionIfValid_InvalidTrackStatus)
        << "The track publisher returned a status indicating that no objects "
           "are available, but a stream for those objects exists.";
    session_->Error(MoqtError::kInternalError,
                    "Invalid track state provided by application");
    return nullptr;
  }
  return subscription;
}

void MoqtSession::OutgoingDataStream::SendObjects(
    PublishedSubscription& subscription) {
  while (stream_->CanWrite()) {
    std::optional<PublishedObject> object =
        subscription.publisher().GetCachedObject(next_object_);
    if (!object.has_value()) {
      break;
    }
    if (!subscription.InWindow(next_object_)) {
      // It is possible that the next object became irrelevant due to a
      // SUBSCRIBE_UPDATE.  Close the stream if so.
      bool success = stream_->SendFin();
      QUICHE_BUG_IF(OutgoingDataStream_fin_due_to_update, !success)
          << "Writing FIN failed despite CanWrite() being true.";
      return;
    }
    QUICHE_DCHECK(next_object_ <= object->sequence);
    MoqtTrackPublisher& publisher = subscription.publisher();
    QUICHE_DCHECK(DoesTrackStatusImplyHavingData(*publisher.GetTrackStatus()));
    MoqtForwardingPreference forwarding_preference =
        publisher.GetForwardingPreference();
    UpdateSendOrder(subscription);
    if (forwarding_preference == MoqtForwardingPreference::kDatagram) {
      QUICHE_BUG(quic_bug_SendObjects_for_Datagram)
          << "Datagram Track requesting SendObjects";
      return;
    }
    next_object_.object = object->sequence.object + 1;
    if (session_->WriteObjectToStream(
            stream_, subscription.track_alias(), *object,
            MoqtDataStreamType::kStreamHeaderSubgroup, !stream_header_written_,
            object->fin_after_this)) {
      stream_header_written_ = true;
      subscription.OnObjectSent(object->sequence);
    }
  }
}

void MoqtSession::OutgoingDataStream::Fin(FullSequence last_object) {
  if (next_object_ <= last_object) {
    // There is still data to send, do nothing.
    return;
  }
  // All data has already been sent; send a pure FIN.
  bool success = stream_->SendFin();
  QUICHE_BUG_IF(OutgoingDataStream_fin_failed, !success)
      << "Writing pure FIN failed.";
}

bool MoqtSession::WriteObjectToStream(webtransport::Stream* stream, uint64_t id,
                                      const PublishedObject& object,
                                      MoqtDataStreamType type,
                                      bool is_first_on_stream, bool fin) {
  QUICHE_DCHECK(stream->CanWrite());
  MoqtObject header;
  header.track_alias = id;
  header.group_id = object.sequence.group;
  header.subgroup_id = object.sequence.subgroup;
  header.object_id = object.sequence.object;
  header.publisher_priority = object.publisher_priority;
  header.object_status = object.status;
  header.payload_length = object.payload.length();

  quiche::QuicheBuffer serialized_header =
      framer_.SerializeObjectHeader(header, type, is_first_on_stream);
  // TODO(vasilvv): add a version of WebTransport write API that accepts
  // memslices so that we can avoid a copy here.
  std::array<absl::string_view, 2> write_vector = {
      serialized_header.AsStringView(), object.payload.AsStringView()};
  quiche::StreamWriteOptions options;
  options.set_send_fin(fin);
  absl::Status write_status = stream->Writev(write_vector, options);
  if (!write_status.ok()) {
    QUICHE_BUG(MoqtSession_WriteObjectToStream_write_failed)
        << "Writing into MoQT stream failed despite CanWrite() being true "
           "before; status: "
        << write_status;
    Error(MoqtError::kInternalError, "Data stream write error");
    return false;
  }

  QUIC_DVLOG(1) << "Stream " << stream->GetStreamId() << " successfully wrote "
                << object.sequence << ", fin = " << fin;
  return true;
}

void MoqtSession::PublishedSubscription::SendDatagram(FullSequence sequence) {
  std::optional<PublishedObject> object =
      track_publisher_->GetCachedObject(sequence);
  if (!object.has_value()) {
    QUICHE_BUG(PublishedSubscription_SendDatagram_object_not_in_cache)
        << "Got notification about an object that is not in the cache";
    return;
  }

  MoqtObject header;
  header.track_alias = track_alias();
  header.group_id = object->sequence.group;
  header.object_id = object->sequence.object;
  header.publisher_priority = track_publisher_->GetPublisherPriority();
  header.object_status = object->status;
  header.subgroup_id = std::nullopt;
  header.payload_length = object->payload.length();
  quiche::QuicheBuffer datagram = session_->framer_.SerializeObjectDatagram(
      header, object->payload.AsStringView());
  session_->session_->SendOrQueueDatagram(datagram.AsStringView());
  OnObjectSent(object->sequence);
}

void MoqtSession::OutgoingDataStream::UpdateSendOrder(
    PublishedSubscription& subscription) {
  stream_->SetPriority(
      webtransport::StreamPriority{/*send_group_id=*/kMoqtSendGroupId,
                                   subscription.GetSendOrder(next_object_)});
}

}  // namespace moqt