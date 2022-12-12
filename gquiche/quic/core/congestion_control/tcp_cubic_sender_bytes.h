// Copyright (c) 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TCP cubic send side congestion algorithm, emulates the behavior of TCP cubic.

#ifndef QUICHE_QUIC_CORE_CONGESTION_CONTROL_TCP_CUBIC_SENDER_BYTES_H_
#define QUICHE_QUIC_CORE_CONGESTION_CONTROL_TCP_CUBIC_SENDER_BYTES_H_

#include <cstdint>
#include <string>

#include "gquiche/quic/core/congestion_control/cubic_bytes.h"
#include "gquiche/quic/core/congestion_control/hybrid_slow_start.h"
#include "gquiche/quic/core/congestion_control/prr_sender.h"
#include "gquiche/quic/core/congestion_control/send_algorithm_interface.h"
#include "gquiche/quic/core/quic_bandwidth.h"
#include "gquiche/quic/core/quic_connection_stats.h"
#include "gquiche/quic/core/quic_packets.h"
#include "gquiche/quic/core/quic_time.h"
#include "gquiche/quic/platform/api/quic_export.h"

namespace quic {

class RttStats;

// Maximum window to allow when doing bandwidth resumption.
const QuicPacketCount kMaxResumptionCongestionWindow = 200;

namespace test {
class TcpCubicSenderBytesPeer;
}  // namespace test

class QUIC_EXPORT_PRIVATE TcpCubicSenderBytes : public SendAlgorithmInterface {
 public:
  TcpCubicSenderBytes(const QuicClock* clock, const RttStats* rtt_stats,
                      bool reno, QuicPacketCount initial_tcp_congestion_window,
                      QuicPacketCount max_congestion_window,
                      QuicConnectionStats* stats);
  TcpCubicSenderBytes(const TcpCubicSenderBytes&) = delete;
  TcpCubicSenderBytes& operator=(const TcpCubicSenderBytes&) = delete;
  ~TcpCubicSenderBytes() override;
  struct QUIC_EXPORT_PRIVATE DebugState {
    explicit DebugState(const TcpCubicSenderBytes& sender);
    DebugState(const DebugState& state);
    QuicTime::Delta min_rtt;
    QuicTime::Delta latest_rtt;
    QuicTime::Delta smoothed_rtt;
    QuicTime::Delta mean_deviation;
    QuicBandwidth bandwidth_est;
  };

  // Start implementation of SendAlgorithmInterface.
  void SetFromConfig(const QuicConfig& config,
                     Perspective perspective) override;
  void ApplyConnectionOptions(
      const QuicTagVector& /*connection_options*/) override {}
  void AdjustNetworkParameters(const NetworkParams& params) override;
  void SetNumEmulatedConnections(int num_connections);
  void SetInitialCongestionWindowInPackets(
      QuicPacketCount congestion_window) override;
  void SetExtraLossThreshold(float extra_loss_threshold) override;
  void SetUpdateRangeTime(QuicTime::Delta update_range_time) override;
  void SetIsUpdatePacketLostFlag(bool is_update_min_packet_lost) override;
  void SetUseBandwidthListFlag(bool is_use_bandwidth_list) override;
  void OnConnectionMigration() override;
  void OnCongestionEvent(bool rtt_updated, QuicByteCount prior_in_flight,
                         QuicTime event_time,
                         const AckedPacketVector& acked_packets,
                         const LostPacketVector& lost_packets) override;
  void OnPacketSent(QuicTime sent_time, QuicByteCount bytes_in_flight,
                    QuicPacketNumber packet_number, QuicByteCount bytes,
                    HasRetransmittableData is_retransmittable) override;
  void OnPacketNeutered(QuicPacketNumber /*packet_number*/) override {}
  void OnRetransmissionTimeout(bool packets_retransmitted) override;
  bool CanSend(QuicByteCount bytes_in_flight) override;
  QuicBandwidth PacingRate(QuicByteCount bytes_in_flight) const override;
  QuicBandwidth BandwidthEstimate() const override;
  bool HasGoodBandwidthEstimateForResumption() const override { return false; }
  QuicByteCount GetCongestionWindow() const override;
  QuicByteCount GetSlowStartThreshold() const override;
  CongestionControlType GetCongestionControlType() const override;
  bool InSlowStart() const override;
  bool InRecovery() const override;
  std::string GetDebugState() const override;
  void OnApplicationLimited(QuicByteCount bytes_in_flight) override;
  void PopulateConnectionStats(QuicConnectionStats* /*stats*/) const override {}
  // End implementation of SendAlgorithmInterface.
  
  DebugState ExportDebugState() const;
  QuicByteCount min_congestion_window() const { return min_congestion_window_; }

 protected:
  // Compute the TCP Reno beta based on the current number of connections.
  float RenoBeta() const;

  bool IsCwndLimited(QuicByteCount bytes_in_flight) const;

  // TODO(ianswett): Remove these and migrate to OnCongestionEvent.
  void OnPacketAcked(QuicPacketNumber acked_packet_number,
                     QuicByteCount acked_bytes, QuicByteCount prior_in_flight,
                     QuicTime event_time);
  void SetCongestionWindowFromBandwidthAndRtt(QuicBandwidth bandwidth,
                                              QuicTime::Delta rtt);
  void SetMinCongestionWindowInPackets(QuicPacketCount congestion_window);
  void ExitSlowstart();
  void OnPacketLost(QuicPacketNumber packet_number, QuicByteCount lost_bytes,
                    QuicByteCount prior_in_flight);
  void MaybeIncreaseCwnd(QuicPacketNumber acked_packet_number,
                         QuicByteCount acked_bytes,
                         QuicByteCount prior_in_flight, QuicTime event_time);
  void HandleRetransmissionTimeout();

 private:
  friend class test::TcpCubicSenderBytesPeer;

  HybridSlowStart hybrid_slow_start_;
  PrrSender prr_;
  const RttStats* rtt_stats_;
  QuicConnectionStats* stats_;

  // If true, Reno congestion control is used instead of Cubic.
  const bool reno_;

  // Number of connections to simulate.
  uint32_t num_connections_;

  // Track the largest packet that has been sent.
  QuicPacketNumber largest_sent_packet_number_;

  // Track the largest packet that has been acked.
  QuicPacketNumber largest_acked_packet_number_;

  // Track the largest packet number outstanding when a CWND cutback occurs.
  QuicPacketNumber largest_sent_at_last_cutback_;

  // Whether to use 4 packets as the actual min, but pace lower.
  bool min4_mode_;

  // Whether the last loss event caused us to exit slowstart.
  // Used for stats collection of slowstart_packets_lost
  bool last_cutback_exited_slowstart_;

  // When true, exit slow start with large cutback of congestion window.
  bool slow_start_large_reduction_;

  // When true, use unity pacing instead of PRR.
  bool no_prr_;

  CubicBytes cubic_;

  // ACK counter for the Reno implementation.
  uint64_t num_acked_packets_;

  // Congestion window in bytes.
  QuicByteCount congestion_window_;

  // Minimum congestion window in bytes.
  QuicByteCount min_congestion_window_;

  // Maximum congestion window in bytes.
  QuicByteCount max_congestion_window_;

  // Slow start congestion window in bytes, aka ssthresh.
  QuicByteCount slowstart_threshold_;

  // Initial TCP congestion window in bytes. This variable can only be set when
  // this algorithm is created.
  const QuicByteCount initial_tcp_congestion_window_;

  // Initial maximum TCP congestion window in bytes. This variable can only be
  // set when this algorithm is created.
  const QuicByteCount initial_max_tcp_congestion_window_;

  // The minimum window when exiting slow start with large reduction.
  QuicByteCount min_slow_start_exit_window_;
};

}  // namespace quic

#endif  // QUICHE_QUIC_CORE_CONGESTION_CONTROL_TCP_CUBIC_SENDER_BYTES_H_
