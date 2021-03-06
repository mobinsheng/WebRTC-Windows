/*
 *  Copyright (c) 2013 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "webrtc/modules/remote_bitrate_estimator/test/bwe_test_framework.h"

namespace webrtc {
namespace testing {
namespace bwe {

Random::Random(uint32_t seed)
    : a_(0x531FDB97 ^ seed),
      b_(0x6420ECA8 + seed) {
}

float Random::Rand() {
  const float kScale = 1.0f / 0xffffffff;
  float result = kScale * b_;
  a_ ^= b_;
  b_ += a_;
  return result;
}

int Random::Gaussian(int mean, int standard_deviation) {
  // Creating a Normal distribution variable from two independent uniform
  // variables based on the Box-Muller transform, which is defined on the
  // interval (0, 1], hence the mask+add below.
  const double kPi = 3.14159265358979323846;
  const double kScale = 1.0 / 0x80000000ul;
  double u1 = kScale * ((a_ & 0x7ffffffful) + 1);
  double u2 = kScale * ((b_ & 0x7ffffffful) + 1);
  a_ ^= b_;
  b_ += a_;
  return static_cast<int>(mean + standard_deviation *
      std::sqrt(-2 * std::log(u1)) * std::cos(2 * kPi * u2));
}

Packet::Packet()
    : send_time_us_(0),
      payload_size_(0) {
   memset(&header_, 0, sizeof(header_));
}

Packet::Packet(int64_t send_time_us, uint32_t payload_size,
          const RTPHeader& header)
  : send_time_us_(send_time_us),
    payload_size_(payload_size),
    header_(header) {
}

Packet::Packet(int64_t send_time_us, uint32_t sequence_number)
    : send_time_us_(send_time_us),
      payload_size_(0) {
   memset(&header_, 0, sizeof(header_));
   header_.sequenceNumber = sequence_number;
}

bool Packet::operator<(const Packet& rhs) const {
  return send_time_us_ < rhs.send_time_us_;
}

void Packet::set_send_time_us(int64_t send_time_us) {
  assert(send_time_us >= 0);
  send_time_us_ = send_time_us;
}

bool IsTimeSorted(const Packets& packets) {
  PacketsConstIt last_it = packets.begin();
  for (PacketsConstIt it = last_it; it != packets.end(); ++it) {
    if (it != last_it && *it < *last_it) {
      return false;
    }
    last_it = it;
  }
  return true;
}

PacketProcessor::PacketProcessor(PacketProcessorListener* listener)
    : listener_(listener) {
  if (listener_) {
    listener_->AddPacketProcessor(this);
  }
}

PacketProcessor::~PacketProcessor() {
  if (listener_) {
    listener_->RemovePacketProcessor(this);
  }
}

RateCounterFilter::RateCounterFilter(PacketProcessorListener* listener)
    : PacketProcessor(listener),
      kWindowSizeUs(1000000),
      packets_per_second_(0),
      bytes_per_second_(0),
      last_accumulated_us_(0),
      window_(),
      pps_stats_(),
      kbps_stats_() {
}

RateCounterFilter::~RateCounterFilter() {
  LogStats();
}

void RateCounterFilter::LogStats() {
  BWE_TEST_LOGGING_CONTEXT("RateCounterFilter");
  pps_stats_.Log("pps");
  kbps_stats_.Log("kbps");
}

void RateCounterFilter::RunFor(int64_t /*time_ms*/, Packets* in_out) {
  assert(in_out);
  for (PacketsConstIt it = in_out->begin(); it != in_out->end(); ++it) {
    packets_per_second_++;
    bytes_per_second_ += it->payload_size();
    last_accumulated_us_ = it->send_time_us();
  }
  window_.insert(window_.end(), in_out->begin(), in_out->end());
  while (!window_.empty()) {
    const Packet& packet = window_.front();
    if (packet.send_time_us() > (last_accumulated_us_ - kWindowSizeUs)) {
      break;
    }
    assert(packets_per_second_ >= 1);
    assert(bytes_per_second_ >= packet.payload_size());
    packets_per_second_--;
    bytes_per_second_ -= packet.payload_size();
    window_.pop_front();
  }
  pps_stats_.Push(packets_per_second_);
  kbps_stats_.Push((bytes_per_second_ * 8) / 1000.0);
}

LossFilter::LossFilter(PacketProcessorListener* listener)
    : PacketProcessor(listener),
      random_(0x12345678),
      loss_fraction_(0.0f) {
}

void LossFilter::SetLoss(float loss_percent) {
  BWE_TEST_LOGGING_ENABLE(false);
  BWE_TEST_LOGGING_LOG1("Loss", "%f%%", loss_percent);
  assert(loss_percent >= 0.0f);
  assert(loss_percent <= 100.0f);
  loss_fraction_ = loss_percent * 0.01f;
}

void LossFilter::RunFor(int64_t /*time_ms*/, Packets* in_out) {
  assert(in_out);
  for (PacketsIt it = in_out->begin(); it != in_out->end(); ) {
    if (random_.Rand() < loss_fraction_) {
      it = in_out->erase(it);
    } else {
      ++it;
    }
  }
}

DelayFilter::DelayFilter(PacketProcessorListener* listener)
    : PacketProcessor(listener),
      delay_us_(0),
      last_send_time_us_(0) {
}

void DelayFilter::SetDelay(int64_t delay_ms) {
  BWE_TEST_LOGGING_ENABLE(false);
  BWE_TEST_LOGGING_LOG1("Delay", "%d ms", static_cast<int>(delay_ms));
  assert(delay_ms >= 0);
  delay_us_ = delay_ms * 1000;
}

void DelayFilter::RunFor(int64_t /*time_ms*/, Packets* in_out) {
  assert(in_out);
  for (PacketsIt it = in_out->begin(); it != in_out->end(); ++it) {
    int64_t new_send_time_us = it->send_time_us() + delay_us_;
    last_send_time_us_ = std::max(last_send_time_us_, new_send_time_us);
    it->set_send_time_us(last_send_time_us_);
  }
}

JitterFilter::JitterFilter(PacketProcessorListener* listener)
    : PacketProcessor(listener),
      random_(0x89674523),
      stddev_jitter_us_(0),
      last_send_time_us_(0) {
}

void JitterFilter::SetJitter(int64_t stddev_jitter_ms) {
  BWE_TEST_LOGGING_ENABLE(false);
  BWE_TEST_LOGGING_LOG1("Jitter", "%d ms",
                        static_cast<int>(stddev_jitter_ms));
  assert(stddev_jitter_ms >= 0);
  stddev_jitter_us_ = stddev_jitter_ms * 1000;
}

void JitterFilter::RunFor(int64_t /*time_ms*/, Packets* in_out) {
  assert(in_out);
  for (PacketsIt it = in_out->begin(); it != in_out->end(); ++it) {
    int64_t new_send_time_us = it->send_time_us();
    new_send_time_us += random_.Gaussian(0, stddev_jitter_us_);
    last_send_time_us_ = std::max(last_send_time_us_, new_send_time_us);
    it->set_send_time_us(last_send_time_us_);
  }
}

ReorderFilter::ReorderFilter(PacketProcessorListener* listener)
    : PacketProcessor(listener),
      random_(0x27452389),
      reorder_fraction_(0.0f) {
}

void ReorderFilter::SetReorder(float reorder_percent) {
  BWE_TEST_LOGGING_ENABLE(false);
  BWE_TEST_LOGGING_LOG1("Reordering", "%f%%", reorder_percent);
  assert(reorder_percent >= 0.0f);
  assert(reorder_percent <= 100.0f);
  reorder_fraction_ = reorder_percent * 0.01f;
}

void ReorderFilter::RunFor(int64_t /*time_ms*/, Packets* in_out) {
  assert(in_out);
  if (in_out->size() >= 2) {
    PacketsIt last_it = in_out->begin();
    PacketsIt it = last_it;
    while (++it != in_out->end()) {
      if (random_.Rand() < reorder_fraction_) {
        int64_t t1 = last_it->send_time_us();
        int64_t t2 = it->send_time_us();
        std::swap(*last_it, *it);
        last_it->set_send_time_us(t1);
        it->set_send_time_us(t2);
      }
      last_it = it;
    }
  }
}

ChokeFilter::ChokeFilter(PacketProcessorListener* listener)
    : PacketProcessor(listener),
      kbps_(1200),
      max_delay_us_(0),
      last_send_time_us_(0) {
}

void ChokeFilter::SetCapacity(uint32_t kbps) {
  BWE_TEST_LOGGING_ENABLE(false);
  BWE_TEST_LOGGING_LOG1("BitrateChoke", "%d kbps", kbps);
  kbps_ = kbps;
}

void ChokeFilter::SetMaxDelay(int64_t max_delay_ms) {
  BWE_TEST_LOGGING_ENABLE(false);
  BWE_TEST_LOGGING_LOG1("Max Delay", "%d ms", static_cast<int>(max_delay_ms));
  assert(max_delay_ms >= 0);
  max_delay_us_ = max_delay_ms * 1000;
}

void ChokeFilter::RunFor(int64_t /*time_ms*/, Packets* in_out) {
  assert(in_out);
  for (PacketsIt it = in_out->begin(); it != in_out->end(); ) {
    int64_t earliest_send_time_us = last_send_time_us_ +
        (it->payload_size() * 8 * 1000 + kbps_ / 2) / kbps_;
    int64_t new_send_time_us = std::max(it->send_time_us(),
                                        earliest_send_time_us);
    if (max_delay_us_ == 0 ||
        max_delay_us_ >= (new_send_time_us - it->send_time_us())) {
      it->set_send_time_us(new_send_time_us);
      last_send_time_us_ = new_send_time_us;
      ++it;
    } else {
      it = in_out->erase(it);
    }
  }
}

PacketSender::PacketSender(PacketProcessorListener* listener)
    : PacketProcessor(listener) {
}

VideoSender::VideoSender(PacketProcessorListener* listener, float fps,
                         uint32_t kbps, uint32_t ssrc, float first_frame_offset)
    : PacketSender(listener),
      kMaxPayloadSizeBytes(1000),
      kTimestampBase(0xff80ff00ul),
      frame_period_ms_(1000.0 / fps),
      next_frame_ms_(frame_period_ms_ * first_frame_offset),
      now_ms_(0.0),
      bytes_per_second_((1000 * kbps) / 8),
      frame_size_bytes_(bytes_per_second_ / fps),
      prototype_header_() {
  assert(first_frame_offset >= 0.0f);
  assert(first_frame_offset < 1.0f);
  memset(&prototype_header_, 0, sizeof(prototype_header_));
  prototype_header_.ssrc = ssrc;
  prototype_header_.sequenceNumber = 0xf000u;
}

uint32_t VideoSender::GetCapacityKbps() const {
  return (bytes_per_second_ * 8) / 1000;
}

void VideoSender::RunFor(int64_t time_ms, Packets* in_out) {
  assert(in_out);
  now_ms_ += time_ms;
  Packets newPackets;
  while (now_ms_ >= next_frame_ms_) {
    prototype_header_.sequenceNumber++;
    prototype_header_.timestamp = kTimestampBase +
        static_cast<uint32_t>(next_frame_ms_ * 90.0);
    prototype_header_.extension.absoluteSendTime = (kTimestampBase +
        ((static_cast<int64_t>(next_frame_ms_ * (1 << 18)) + 500) / 1000)) &
            0x00fffffful;
    prototype_header_.extension.transmissionTimeOffset = 0;

    // Generate new packets for this frame, all with the same timestamp,
    // but the payload size is capped, so if the whole frame doesn't fit in
    // one packet, we will see a number of equally sized packets followed by
    // one smaller at the tail.
    int64_t send_time_us = next_frame_ms_ * 1000.0;
    uint32_t payload_size = frame_size_bytes_;
    while (payload_size > 0) {
      uint32_t size = std::min(kMaxPayloadSizeBytes, payload_size);
      newPackets.push_back(Packet(send_time_us, size, prototype_header_));
      payload_size -= size;
    }

    next_frame_ms_ += frame_period_ms_;
  }
  in_out->merge(newPackets);
}
}  // namespace bwe
}  // namespace testing
}  // namespace webrtc
