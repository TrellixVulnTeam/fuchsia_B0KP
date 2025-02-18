// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "usb-audio-stream.h"

#include <lib/zx/clock.h>
#include <lib/zx/vmar.h>
#include <string.h>
#include <zircon/hw/usb/audio.h>
#include <zircon/process.h>
#include <zircon/time.h>
#include <zircon/types.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>

#include <audio-proto-utils/format-utils.h>
#include <ddk/device.h>
#include <fbl/algorithm.h>
#include <fbl/auto_call.h>
#include <fbl/auto_lock.h>
#include <usb/usb-request.h>

#include "src/lib/digest/digest.h"
#include "usb-audio-device.h"
#include "usb-audio-stream-interface.h"
#include "usb-audio.h"

namespace audio {
namespace usb {

namespace audio_fidl = ::llcpp::fuchsia::hardware::audio;

static constexpr uint32_t MAX_OUTSTANDING_REQ = 3;

UsbAudioStream::UsbAudioStream(UsbAudioDevice* parent, std::unique_ptr<UsbAudioStreamInterface> ifc)
    : UsbAudioStreamBase(parent->zxdev()),
      AudioStreamProtocol(ifc->direction() == Direction::Input),
      parent_(*parent),
      ifc_(std::move(ifc)),
      create_time_(zx::clock::get_monotonic().get()),
      loop_(&kAsyncLoopConfigNeverAttachToThread) {
  snprintf(log_prefix_, sizeof(log_prefix_), "UsbAud %04hx:%04hx %s-%03d", parent_.vid(),
           parent_.pid(), is_input() ? "input" : "output", ifc_->term_link());
  loop_.StartThread("usb-audio-stream-loop");
}

UsbAudioStream::~UsbAudioStream() {
  // We destructing.  All of our requests should be sitting in the free list.
  ZX_DEBUG_ASSERT(allocated_req_cnt_ == free_req_cnt_);

  while (!list_is_empty(&free_req_)) {
    usb_request_release(usb_req_list_remove_head(&free_req_, parent_.parent_req_size()));
  }
}

fbl::RefPtr<UsbAudioStream> UsbAudioStream::Create(UsbAudioDevice* parent,
                                                   std::unique_ptr<UsbAudioStreamInterface> ifc) {
  ZX_DEBUG_ASSERT(parent != nullptr);
  ZX_DEBUG_ASSERT(ifc != nullptr);

  fbl::AllocChecker ac;
  auto stream = fbl::AdoptRef(new (&ac) UsbAudioStream(parent, std::move(ifc)));
  if (!ac.check()) {
    LOG_EX(ERROR, *parent, "Out of memory while attempting to allocate UsbAudioStream\n");
    return nullptr;
  }

  stream->ComputePersistentUniqueId();

  return stream;
}

zx_status_t UsbAudioStream::Bind() {
  // TODO(johngro): Do this differently when we have the ability to queue io
  // transactions to a USB isochronous endpoint and can have the bus driver
  // DMA directly from the ring buffer we have set up with our user.
  {
    fbl::AutoLock req_lock(&req_lock_);

    list_initialize(&free_req_);
    free_req_cnt_ = 0;
    allocated_req_cnt_ = 0;

    uint64_t req_size = parent_.parent_req_size() + sizeof(usb_req_internal_t);
    for (uint32_t i = 0; i < MAX_OUTSTANDING_REQ; ++i) {
      usb_request_t* req;
      zx_status_t status = usb_request_alloc(&req, ifc_->max_req_size(), ifc_->ep_addr(), req_size);
      if (status != ZX_OK) {
        LOG(ERROR, "Failed to allocate usb request %u/%u (size %u): %d\n", i + 1,
            MAX_OUTSTANDING_REQ, ifc_->max_req_size(), status);
        return status;
      }

      status = usb_req_list_add_head(&free_req_, req, parent_.parent_req_size());
      ZX_DEBUG_ASSERT(status == ZX_OK);
      ++free_req_cnt_;
      ++allocated_req_cnt_;
    }
  }

  char name[64];
  snprintf(name, sizeof(name), "usb-audio-%s-%03d", is_input() ? "input" : "output",
           ifc_->term_link());

  zx_status_t status = UsbAudioStreamBase::DdkAdd(name);
  if (status == ZX_OK) {
    // If bind/setup has succeeded, then the devmgr now holds a reference to us.
    // Manually increase our reference count to account for this.
    this->AddRef();
  } else {
    LOG(ERROR, "Failed to publish UsbAudioStream device node (name \"%s\", status %d)\n", name,
        status);
  }

  // Configure and fetch a deadline profile for our USB IRQ callback thread.  We
  // will be running at a 1 mSec isochronous rate, and we mostly want to be sure
  // that we are done and have queued the next job before the next cycle starts.
  // Currently, there shouldn't be any great amount of work to be done, just
  // memcpying the data into the buffer used by the USB controller driver.
  // 250uSec should be more than enough time.
  status = device_get_deadline_profile(
      zxdev_,
      ZX_USEC(250),  // capacity: we agree to run for no more than 250 uSec max
      ZX_USEC(700),  // deadline: Let this be scheduled as late as 700 uSec into the cycle
      ZX_USEC(995),  // period:   we need to do this at a rate of ~1KHz
      "src/media/audio/drivers/usb-audio/usb-audio-stream",
      profile_handle_.reset_and_get_address());

  if (status != ZX_OK) {
    LOG(ERROR, "Failed to retrieve profile, status %d\n", status);
    return status;
  }

  return status;
}

void UsbAudioStream::RequestCompleteCallback(void* ctx, usb_request_t* request) {
  ZX_DEBUG_ASSERT(ctx != nullptr);
  reinterpret_cast<UsbAudioStream*>(ctx)->RequestComplete(request);
}

void UsbAudioStream::ComputePersistentUniqueId() {
  // Do the best that we can to generate a persistent ID unique to this audio
  // stream by blending information from a number of sources.  In particular,
  // consume...
  //
  // 1) This USB device's top level device descriptor (this contains the
  //    VID/PID of the device, among other things)
  // 2) The contents of the descriptor list used to describe the control and
  //    streaming interfaces present in the device.
  // 3) The manufacturer, product, and serial number string descriptors (if
  //    present)
  // 4) The stream interface ID.
  //
  // The goal here is to produce something like a UUID which is as unique to a
  // specific instance of a specific device as we can make it, but which
  // should persist across boots even in the presence of driver updates an
  // such.  Even so, upper levels of code will still need to deal with the sad
  // reality that some types of devices may end up looking the same between
  // two different instances.  If/when this becomes an issue, we may need to
  // pursue other options.  One choice might be to change the way devices are
  // enumerated in the USB section of the device tree so that their path has
  // only to do with physical topology, and has no runtime enumeration order
  // dependencies.  At that point in time, adding the topology into the hash
  // should do the job, but would imply that the same device plugged into two
  // different ports will have a different unique ID for the purposes of
  // saving and restoring driver settings (as it does in some operating
  // systems today).
  //
  uint16_t vid = parent_.desc().idVendor;
  uint16_t pid = parent_.desc().idProduct;
  audio_stream_unique_id_t fallback_id{
      .data = {'U', 'S', 'B', ' ', static_cast<uint8_t>(vid >> 8), static_cast<uint8_t>(vid),
               static_cast<uint8_t>(pid >> 8), static_cast<uint8_t>(pid), ifc_->iid()}};
  persistent_unique_id_ = fallback_id;

  digest::Digest sha;
  sha.Init();

  // #1: Top level descriptor.
  sha.Update(&parent_.desc(), sizeof(parent_.desc()));

  // #2: The descriptor list
  const auto& desc_list = parent_.desc_list();
  ZX_DEBUG_ASSERT((desc_list != nullptr) && (desc_list->size() > 0));
  sha.Update(desc_list->data(), desc_list->size());

  // #3: The various descriptor strings which may exist.
  const fbl::Array<uint8_t>* desc_strings[] = {&parent_.mfr_name(), &parent_.prod_name(),
                                               &parent_.serial_num()};
  for (const auto str : desc_strings) {
    if (str->size()) {
      sha.Update(str->data(), str->size());
    }
  }

  // #4: The stream interface's ID.
  auto iid = ifc_->iid();
  sha.Update(&iid, sizeof(iid));

  // Finish the SHA and attempt to copy as much of the results to our internal
  // cached representation as we can.
  sha.Final();
  sha.CopyTruncatedTo(persistent_unique_id_.data, sizeof(persistent_unique_id_.data));
}

void UsbAudioStream::ReleaseRingBufferLocked() {
  if (ring_buffer_virt_ != nullptr) {
    ZX_DEBUG_ASSERT(ring_buffer_size_ != 0);
    zx::vmar::root_self()->unmap(reinterpret_cast<uintptr_t>(ring_buffer_virt_), ring_buffer_size_);
    ring_buffer_virt_ = nullptr;
    ring_buffer_size_ = 0;
  }
  ring_buffer_vmo_.reset();
}

void UsbAudioStream::GetChannel(GetChannelCompleter::Sync& completer) {
  fbl::AutoLock lock(&lock_);

  // Attempt to allocate a new driver channel and bind it to us.  If we don't
  // already have an stream_channel_, flag this channel is the privileged
  // connection (The connection which is allowed to do things like change
  // formats).
  bool privileged = (stream_channel_ == nullptr);

  zx::channel stream_channel_local;
  zx::channel stream_channel_remote;
  auto status = zx::channel::create(0, &stream_channel_local, &stream_channel_remote);
  if (status != ZX_OK) {
    completer.Close(ZX_ERR_NO_MEMORY);
    return;
  }

  auto stream_channel = StreamChannel::Create<StreamChannel>(this);
  if (stream_channel == nullptr) {
    completer.Close(ZX_ERR_NO_MEMORY);
    return;
  }
  stream_channels_.push_back(stream_channel);
  fidl::OnUnboundFn<audio_fidl::StreamConfig::Interface> on_unbound =
      [this, stream_channel](audio_fidl::StreamConfig::Interface*, fidl::UnbindInfo,
                             fidl::ServerEnd<llcpp::fuchsia::hardware::audio::StreamConfig>) {
        fbl::AutoLock channel_lock(&lock_);
        this->DeactivateStreamChannelLocked(stream_channel.get());
      };

  fidl::BindServer<audio_fidl::StreamConfig::Interface>(
      loop_.dispatcher(), std::move(stream_channel_local), stream_channel.get(),
      std::move(on_unbound));

  if (privileged) {
    ZX_DEBUG_ASSERT(stream_channel_ == nullptr);
    stream_channel_ = stream_channel;
  }
  completer.Reply(std::move(stream_channel_remote));
}

void UsbAudioStream::DdkUnbind(ddk::UnbindTxn txn) {
  // We stop the loop so we can safely deactivate channels via RAII via DdkRelease.
  loop_.Shutdown();

  // Unpublish our device node.
  txn.Reply();
}

void UsbAudioStream::DdkRelease() {
  // Reclaim our reference from the driver framework and let it go out of
  // scope.  If this is our last reference (it should be), we will destruct
  // immediately afterwards.
  auto stream = fbl::ImportFromRawPtr(this);

  // Make sure that our parent is no longer holding a reference to us.
  parent_.RemoveAudioStream(stream);
}

void UsbAudioStream::GetSupportedFormats(
    StreamChannel::GetSupportedFormatsCompleter::Sync& completer) {
  const auto& formats = ifc_->formats();
  if (formats.size() > std::numeric_limits<uint16_t>::max()) {
    LOG(ERROR, "Too many formats (%zu) to send during AUDIO_STREAM_CMD_GET_FORMATS request!\n",
        formats.size());
    return;
  }

  // Build formats compatible with FIDL from a vector of audio_stream_format_range_t.
  // Needs to be alive until the reply is sent.
  struct FidlCompatibleFormats {
    fbl::Vector<uint8_t> number_of_channels;
    fbl::Vector<audio_fidl::SampleFormat> sample_formats;
    fbl::Vector<uint32_t> frame_rates;
    fbl::Vector<uint8_t> valid_bits_per_sample;
    fbl::Vector<uint8_t> bytes_per_sample;
  };
  fbl::Vector<FidlCompatibleFormats> fidl_compatible_formats;
  for (auto& i : formats) {
    audio_fidl::SampleFormat sample_format = audio_fidl::SampleFormat::PCM_SIGNED;
    ZX_DEBUG_ASSERT(!(i.range_.sample_formats & AUDIO_SAMPLE_FORMAT_BITSTREAM));
    ZX_DEBUG_ASSERT(!(i.range_.sample_formats & AUDIO_SAMPLE_FORMAT_FLAG_INVERT_ENDIAN));

    if (i.range_.sample_formats & AUDIO_SAMPLE_FORMAT_FLAG_UNSIGNED) {
      sample_format = audio_fidl::SampleFormat::PCM_UNSIGNED;
    }

    auto noflag_format = static_cast<audio_sample_format_t>(i.range_.sample_formats &
                                                            ~AUDIO_SAMPLE_FORMAT_FLAG_MASK);

    auto sizes = audio::utils::GetSampleSizes(noflag_format);

    ZX_DEBUG_ASSERT(sizes.valid_bits_per_sample != 0);
    ZX_DEBUG_ASSERT(sizes.bytes_per_sample != 0);

    if (noflag_format == AUDIO_SAMPLE_FORMAT_32BIT_FLOAT) {
      sample_format = audio_fidl::SampleFormat::PCM_FLOAT;
    }

    fbl::Vector<uint32_t> rates;
    // Ignore flags if min and max are equal.
    if (i.range_.min_frames_per_second == i.range_.max_frames_per_second) {
      rates.push_back(i.range_.min_frames_per_second);
    } else {
      ZX_DEBUG_ASSERT(!(i.range_.flags & ASF_RANGE_FLAG_FPS_CONTINUOUS));
      audio::utils::FrameRateEnumerator enumerator(i.range_);
      for (uint32_t rate : enumerator) {
        rates.push_back(rate);
      }
    }

    fbl::Vector<uint8_t> number_of_channels;
    for (uint8_t j = i.range_.min_channels; j <= i.range_.max_channels; ++j) {
      number_of_channels.push_back(j);
    }

    fidl_compatible_formats.push_back({
        std::move(number_of_channels),
        {sample_format},
        std::move(rates),
        {sizes.valid_bits_per_sample},
        {sizes.bytes_per_sample},
    });
  }

  // Get FIDL PcmSupportedFormats from FIDL compatible vectors.
  // Needs to be alive until the reply is sent.
  fbl::Vector<audio_fidl::PcmSupportedFormats> fidl_pcm_formats;
  for (auto& i : fidl_compatible_formats) {
    audio_fidl::PcmSupportedFormats formats;
    formats.number_of_channels = ::fidl::VectorView<uint8_t>(
        fidl::unowned_ptr(i.number_of_channels.data()), i.number_of_channels.size());
    formats.sample_formats = ::fidl::VectorView<audio_fidl::SampleFormat>(
        fidl::unowned_ptr(i.sample_formats.data()), i.sample_formats.size());
    formats.frame_rates =
        ::fidl::VectorView<uint32_t>(fidl::unowned_ptr(i.frame_rates.data()), i.frame_rates.size());
    formats.bytes_per_sample = ::fidl::VectorView<uint8_t>(
        fidl::unowned_ptr(i.bytes_per_sample.data()), i.bytes_per_sample.size());
    formats.valid_bits_per_sample = ::fidl::VectorView<uint8_t>(
        fidl::unowned_ptr(i.valid_bits_per_sample.data()), i.valid_bits_per_sample.size());
    fidl_pcm_formats.push_back(std::move(formats));
  }

  // Get builders from PcmSupportedFormats tables.
  // Needs to be alive until the reply is sent.
  fbl::Vector<audio_fidl::SupportedFormats::UnownedBuilder> fidl_builders;
  for (auto& i : fidl_pcm_formats) {
    auto builder = audio_fidl::SupportedFormats::UnownedBuilder();
    builder.set_pcm_supported_formats(fidl::unowned_ptr(&i));
    fidl_builders.push_back(std::move(builder));
  }

  // Build FIDL SupportedFormats from PcmSupportedFormats's builders.
  // Needs to be alive until the reply is sent.
  fbl::Vector<audio_fidl::SupportedFormats> fidl_formats;
  for (auto& i : fidl_builders) {
    fidl_formats.push_back(i.build());
  }

  completer.Reply(::fidl::VectorView<audio_fidl::SupportedFormats>(
      fidl::unowned_ptr(fidl_formats.data()), fidl_formats.size()));
}

void UsbAudioStream::CreateRingBuffer(StreamChannel* channel, audio_fidl::wire::Format format,
                                      ::fidl::ServerEnd<audio_fidl::RingBuffer> ring_buffer,
                                      StreamChannel::CreateRingBufferCompleter::Sync& completer) {
  // Only the privileged stream channel is allowed to change the format.
  {
    fbl::AutoLock channel_lock(&lock_);
    if (channel != stream_channel_.get()) {
      LOG(ERROR, "Unprivileged channel cannot set the format");
      completer.Close(ZX_ERR_INVALID_ARGS);
      return;
    }
  }

  auto req = format.pcm_format();

  if (req.channels_to_use_bitmask != AUDIO_SET_FORMAT_REQ_BITMASK_DISABLED &&
      req.channels_to_use_bitmask != ((1 << req.number_of_channels) - 1)) {
    LOG(ERROR, "Unsupported format: Invalid channels to use bitmask (0x%lX)\n",
        req.channels_to_use_bitmask);
    completer.Close(ZX_ERR_INVALID_ARGS);
    return;
  }

  audio_sample_format_t sample_format =
      audio::utils::GetSampleFormat(req.valid_bits_per_sample, 8 * req.bytes_per_sample);

  if (sample_format == 0) {
    LOG(ERROR, "Unsupported format: Invalid bits per sample (%u/%u)\n", req.valid_bits_per_sample,
        8 * req.bytes_per_sample);
    completer.Close(ZX_ERR_INVALID_ARGS);
    return;
  }

  if (req.sample_format == audio_fidl::SampleFormat::PCM_FLOAT) {
    sample_format = AUDIO_SAMPLE_FORMAT_32BIT_FLOAT;
    if (req.valid_bits_per_sample != 32 || req.bytes_per_sample != 4) {
      LOG(ERROR, "Unsupported format: Not 32 per sample/channel for float\n");
      completer.Close(ZX_ERR_INVALID_ARGS);
      return;
    }
  }

  if (req.sample_format == audio_fidl::SampleFormat::PCM_UNSIGNED) {
    sample_format |= AUDIO_SAMPLE_FORMAT_FLAG_UNSIGNED;
  }

  // Look up the details about the interface and the endpoint which will be
  // used for the requested format.
  size_t format_ndx;
  zx_status_t status =
      ifc_->LookupFormat(req.frame_rate, req.number_of_channels, sample_format, &format_ndx);
  if (status != ZX_OK) {
    LOG(ERROR, "Could not find a suitable format in %s", __PRETTY_FUNCTION__);
    completer.Close(ZX_ERR_INVALID_ARGS);
    return;
  }

  // Determine the frame size needed for this requested format, then compute
  // the size of our short packets, and the constants used to generate the
  // short/long packet cadence.  For now, assume that we will be operating at
  // a 1mSec isochronous rate.
  //
  // Make sure that we can fit our longest payload length into one of our
  // usb requests.
  //
  // Store the results of all of these calculations in local variables.  Do
  // not commit them to member variables until we are certain that we are
  // going to go ahead with this format change.
  //
  // TODO(johngro) : Unless/until we can find some way to set the USB bus
  // driver to perform direct DMA to/from the Ring Buffer VMO without the need
  // for software intervention, we may want to expose ways to either increase
  // the isochronous interval (to minimize load) or to use USB 2.0 125uSec
  // sub-frame timing (to decrease latency) if possible.
  uint32_t frame_size;
  frame_size =
      audio::utils::ComputeFrameSize(static_cast<uint16_t>(req.number_of_channels), sample_format);
  if (!frame_size) {
    LOG(ERROR, "Failed to compute frame size (ch %hu fmt 0x%08x)", req.number_of_channels,
        sample_format);
    completer.Close(ZX_ERR_INVALID_ARGS);
    return;
  }

  static constexpr uint32_t iso_packet_rate = 1000;
  uint32_t bytes_per_packet, fractional_bpp_inc, long_payload_len;
  bytes_per_packet = (req.frame_rate / iso_packet_rate) * frame_size;
  fractional_bpp_inc = (req.frame_rate % iso_packet_rate);
  long_payload_len = bytes_per_packet + (fractional_bpp_inc ? frame_size : 0);

  ZX_DEBUG_ASSERT(format_ndx < ifc_->formats().size());
  if (long_payload_len > ifc_->formats()[format_ndx].max_req_size_) {
    completer.Close(ZX_ERR_INVALID_ARGS);
    return;
  }

  // Deny the format change request if the ring buffer is not currently stopped.
  {
    // TODO(johngro) : If the ring buffer is running, should we automatically
    // stop it instead of returning bad state?
    fbl::AutoLock req_lock(&req_lock_);
    if (ring_buffer_state_ != RingBufferState::STOPPED) {
      completer.Close(ZX_ERR_BAD_STATE);
      return;
    }
  }

  fbl::AutoLock req_lock(&lock_);
  // Looks like we are going ahead with this format change.  Tear down any
  // exiting ring buffer interface before proceeding.
  if (rb_channel_ != nullptr) {
    rb_channel_.reset();
  }

  // Record the details of our cadence and format selection
  selected_format_ndx_ = format_ndx;
  selected_frame_rate_ = req.frame_rate;
  frame_size_ = frame_size;
  iso_packet_rate_ = iso_packet_rate;
  bytes_per_packet_ = bytes_per_packet;
  fractional_bpp_inc_ = fractional_bpp_inc;

  // Compute the effective fifo depth for this stream.  Right now, we are in a
  // situation where, for an output, we need to memcpy payloads from the mixer
  // ring buffer into the jobs that we send to the USB host controller.  For an
  // input, when the jobs complete, we need to copy the data from the completed
  // job into the ring buffer.
  //
  // This gives us two different "fifo" depths we may need to report.  For an
  // input, if job X just completed, we will be copying the data sometime during
  // job X+1, assuming that we are hitting our callback targets.  Because of
  // this, we should be safe to report our fifo depth as being 2 times the size
  // of a single maximum sized job.
  //
  // For output, we are attempting to stay MAX_OUTSTANDING_REQ ahead, and we are
  // copying the data from the mixer ring buffer as we go.  Because of this, our
  // reported fifo depth is going to be MAX_OUTSTANDING_REQ maximum sized jobs
  // ahead of the nominal read pointer.
  fifo_bytes_ = bytes_per_packet_ * (is_input() ? 2 : MAX_OUTSTANDING_REQ);

  // If we have no fractional portion to accumulate, we always send
  // short packets.  If our fractional portion is <= 1/2 of our
  // isochronous rate, then we will never send two long packets back
  // to back.
  if (fractional_bpp_inc_) {
    fifo_bytes_ += frame_size_;
    if (fractional_bpp_inc_ > (iso_packet_rate_ >> 1)) {
      fifo_bytes_ += frame_size_;
    }
  }

  // Create a new ring buffer channel which can be used to move bulk data and
  // bind it to us.
  rb_channel_ = Channel::Create<Channel>();

  fidl::OnUnboundFn<audio_fidl::RingBuffer::Interface> on_unbound =
      [this](audio_fidl::RingBuffer::Interface*, fidl::UnbindInfo,
             fidl::ServerEnd<llcpp::fuchsia::hardware::audio::RingBuffer>) {
        fbl::AutoLock lock(&lock_);
        this->DeactivateRingBufferChannelLocked(rb_channel_.get());
      };

  fidl::BindServer<audio_fidl::RingBuffer::Interface>(loop_.dispatcher(), std::move(ring_buffer),
                                                      this, std::move(on_unbound));
}

void UsbAudioStream::WatchGainState(StreamChannel* channel,
                                    StreamChannel::WatchGainStateCompleter::Sync& completer) {
  ZX_DEBUG_ASSERT(!channel->gain_completer_);
  channel->gain_completer_ = completer.ToAsync();

  ZX_DEBUG_ASSERT(ifc_->path() != nullptr);
  const auto& path = *(ifc_->path());

  audio_proto::GainState cur_gain_state = {};
  cur_gain_state.cur_mute = path.cur_mute();
  cur_gain_state.cur_agc = path.cur_agc();
  cur_gain_state.cur_gain = path.cur_gain();
  cur_gain_state.can_mute = path.has_mute();
  cur_gain_state.can_agc = path.has_agc();
  cur_gain_state.min_gain = path.min_gain();
  cur_gain_state.max_gain = path.max_gain();
  cur_gain_state.gain_step = path.gain_res();
  // Reply is delayed if there is no change since the last reported gain state.
  if (channel->last_reported_gain_state_ != cur_gain_state) {
    auto builder = audio_fidl::GainState::UnownedBuilder();
    fidl::aligned<bool> mute = cur_gain_state.cur_mute;
    fidl::aligned<bool> agc = cur_gain_state.cur_agc;
    fidl::aligned<float> gain = cur_gain_state.cur_gain;
    if (cur_gain_state.can_mute) {
      builder.set_muted(fidl::unowned_ptr(&mute));
    }
    if (cur_gain_state.can_agc) {
      builder.set_agc_enabled(fidl::unowned_ptr(&agc));
    }
    builder.set_gain_db(fidl::unowned_ptr(&gain));
    channel->last_reported_gain_state_ = cur_gain_state;
    channel->gain_completer_->Reply(builder.build());
    channel->gain_completer_.reset();
  }
}

void UsbAudioStream::WatchClockRecoveryPositionInfo(
    WatchClockRecoveryPositionInfoCompleter::Sync& completer) {
  fbl::AutoLock req_lock(&req_lock_);
  position_completer_ = completer.ToAsync();
}

void UsbAudioStream::SetGain(audio_fidl::wire::GainState state,
                             StreamChannel::SetGainCompleter::Sync& completer) {
  // TODO(johngro): Actually perform the set operation on our audio path.
  ZX_DEBUG_ASSERT(ifc_->path() != nullptr);
  auto& path = *(ifc_->path());
  bool illegal_mute = state.has_muted() && state.muted() && !path.has_mute();
  bool illegal_agc = state.has_agc_enabled() && state.agc_enabled() && !path.has_agc();
  bool illegal_gain = state.has_gain_db() && (state.gain_db() != 0) && !path.has_gain();

  if (illegal_mute || illegal_agc || illegal_gain) {
    // If this request is illegal, make no changes.
  } else {
    if (state.has_muted()) {
      state.muted() = path.SetMute(parent_.usb_proto(), state.muted());
    }

    if (state.has_agc_enabled()) {
      state.agc_enabled() = path.SetAgc(parent_.usb_proto(), state.agc_enabled());
    }

    if (state.has_gain_db()) {
      state.gain_db() = path.SetGain(parent_.usb_proto(), state.gain_db());
    }

    fbl::AutoLock channel_lock(&lock_);
    for (auto& channel : stream_channels_) {
      if (channel.gain_completer_) {
        channel.gain_completer_->Reply(std::move(state));
        channel.gain_completer_.reset();
      }
    }
  }
}

void UsbAudioStream::WatchPlugState(StreamChannel* channel,
                                    StreamChannel::WatchPlugStateCompleter::Sync& completer) {
  ZX_DEBUG_ASSERT(!channel->plug_completer_);
  channel->plug_completer_ = completer.ToAsync();

  // As long as the usb device is present, we are plugged. A second reply is delayed indefinitely
  // since there will be no change from the last reported plugged state.
  fidl::aligned<bool> plugged = true;
  if (channel->last_reported_plugged_state_ == StreamChannel::Plugged::kNotReported ||
      (channel->last_reported_plugged_state_ == StreamChannel::Plugged::kPlugged) != plugged) {
    auto builder = audio_fidl::PlugState::UnownedBuilder();
    builder.set_plugged(fidl::unowned_ptr(&plugged));
    fidl::aligned<zx_time_t> plug_time = create_time_;
    builder.set_plug_state_time(fidl::unowned_ptr(&plug_time));
    channel->last_reported_plugged_state_ =
        plugged ? StreamChannel::Plugged::kPlugged : StreamChannel::Plugged::kUnplugged;
    channel->plug_completer_->Reply(builder.build());
    channel->plug_completer_.reset();
  }
}

void UsbAudioStream::GetProperties(StreamChannel::GetPropertiesCompleter::Sync& completer) {
  auto builder = audio_fidl::StreamProperties::UnownedBuilder();
  fidl::Array<uint8_t, audio_fidl::UNIQUE_ID_SIZE> unique_id = {};
  for (size_t i = 0; i < audio_fidl::UNIQUE_ID_SIZE; ++i) {
    unique_id.data_[i] = persistent_unique_id_.data[i];
  }
  fidl::aligned<fidl::Array<uint8_t, audio_fidl::UNIQUE_ID_SIZE>> aligned_unique_id = unique_id;
  builder.set_unique_id(fidl::unowned_ptr(&aligned_unique_id));
  fidl::aligned<bool> input = is_input();
  builder.set_is_input(fidl::unowned_ptr(&input));

  const auto& path = *(ifc_->path());
  fidl::aligned<bool> mute = path.has_mute();
  builder.set_can_mute(fidl::unowned_ptr(&mute));
  fidl::aligned<bool> agc = path.has_agc();
  builder.set_can_agc(fidl::unowned_ptr(&agc));
  fidl::aligned<float> min_gain = path.min_gain();
  builder.set_min_gain_db(fidl::unowned_ptr(&min_gain));
  fidl::aligned<float> max_gain = path.max_gain();
  builder.set_max_gain_db(fidl::unowned_ptr(&max_gain));
  fidl::aligned<float> gain_step = path.gain_res();
  builder.set_gain_step_db(fidl::unowned_ptr(&gain_step));

  fidl::StringView product(
      fidl::unowned_ptr(reinterpret_cast<const char*>(parent_.prod_name().begin())),
      parent_.prod_name().size());
  builder.set_product(fidl::unowned_ptr(&product));
  fidl::StringView manufacturer(
      fidl::unowned_ptr(reinterpret_cast<const char*>(parent_.mfr_name().begin())),
      parent_.mfr_name().size());
  builder.set_manufacturer(fidl::unowned_ptr(&manufacturer));

  fidl::aligned<uint32_t> domain = clock_domain_;
  builder.set_clock_domain(fidl::unowned_ptr(&domain));

  fidl::aligned<audio_fidl::PlugDetectCapabilities> cap =
      audio_fidl::PlugDetectCapabilities::HARDWIRED;
  builder.set_plug_detect_capabilities(fidl::unowned_ptr(&cap));
  completer.Reply(builder.build());
}

void UsbAudioStream::GetProperties(GetPropertiesCompleter::Sync& completer) {
  auto builder = audio_fidl::RingBufferProperties::UnownedBuilder();
  fidl::aligned<uint32_t> fifo_depth = fifo_bytes_;
  builder.set_fifo_depth(fidl::unowned_ptr(&fifo_depth));
  // TODO(johngro): Report the actual external delay.
  fidl::aligned<int64_t> delay = static_cast<int64_t>(0);
  builder.set_external_delay(fidl::unowned_ptr(&delay));
  fidl::aligned<bool> flush = true;
  builder.set_needs_cache_flush_or_invalidate(fidl::unowned_ptr(&flush));
  completer.Reply(builder.build());
}

void UsbAudioStream::GetVmo(uint32_t min_frames, uint32_t notifications_per_ring,
                            GetVmoCompleter::Sync& completer) {
  zx::vmo client_rb_handle;
  uint32_t map_flags, client_rights;

  {
    // We cannot create a new ring buffer if we are not currently stopped.
    fbl::AutoLock req_lock(&req_lock_);
    if (ring_buffer_state_ != RingBufferState::STOPPED) {
      LOG(ERROR, "Tried to get VMO in non-stopped state");
      return;
    }
  }

  // Unmap and release any previous ring buffer.
  {
    fbl::AutoLock req_lock(&lock_);
    ReleaseRingBufferLocked();
  }

  auto cleanup = fbl::MakeAutoCall([&completer, this]() {
    {
      fbl::AutoLock req_lock(&this->lock_);
      this->ReleaseRingBufferLocked();
    }
    completer.ReplyError(audio_fidl::GetVmoError::INTERNAL_ERROR);
  });

  // Compute the ring buffer size.  It needs to be at least as big
  // as the virtual fifo depth.
  ZX_DEBUG_ASSERT(frame_size_ && ((fifo_bytes_ % frame_size_) == 0));
  ZX_DEBUG_ASSERT(fifo_bytes_ && ((fifo_bytes_ % fifo_bytes_) == 0));
  ring_buffer_size_ = min_frames;
  ring_buffer_size_ *= frame_size_;
  if (ring_buffer_size_ < fifo_bytes_)
    ring_buffer_size_ = fbl::round_up(fifo_bytes_, frame_size_);

  // Set up our state for generating notifications.
  if (notifications_per_ring) {
    bytes_per_notification_ = ring_buffer_size_ / notifications_per_ring;
  } else {
    bytes_per_notification_ = 0;
  }

  // Create the ring buffer vmo we will use to share memory with the client.
  zx_status_t status = zx::vmo::create(ring_buffer_size_, 0, &ring_buffer_vmo_);
  if (status != ZX_OK) {
    LOG(ERROR, "Failed to create ring buffer (size %u, res %d)\n", ring_buffer_size_, status);
    return;
  }

  // Map the VMO into our address space.
  //
  // TODO(johngro): skip this step when APIs in the USB bus driver exist to
  // DMA directly from the VMO.
  map_flags = ZX_VM_PERM_READ;
  if (is_input())
    map_flags |= ZX_VM_PERM_WRITE;

  status = zx::vmar::root_self()->map(map_flags, 0, ring_buffer_vmo_, 0, ring_buffer_size_,
                                      reinterpret_cast<uintptr_t*>(&ring_buffer_virt_));
  if (status != ZX_OK) {
    LOG(ERROR, "Failed to map ring buffer (size %u, res %d)\n", ring_buffer_size_, status);
    return;
  }

  // Create the client's handle to the ring buffer vmo and set it back to them.
  client_rights = ZX_RIGHT_TRANSFER | ZX_RIGHT_MAP | ZX_RIGHT_READ;
  if (!is_input())
    client_rights |= ZX_RIGHT_WRITE;

  status = ring_buffer_vmo_.duplicate(client_rights, &client_rb_handle);
  if (status != ZX_OK) {
    LOG(ERROR, "Failed to duplicate ring buffer handle (res %d)\n", status);
    return;
  }

  uint32_t num_ring_buffer_frames = ring_buffer_size_ / frame_size_;

  cleanup.cancel();
  completer.ReplySuccess(num_ring_buffer_frames, std::move(client_rb_handle));
}

void UsbAudioStream::Start(StartCompleter::Sync& completer) {
  fbl::AutoLock req_lock(&req_lock_);

  if (ring_buffer_state_ != RingBufferState::STOPPED) {
    // The ring buffer is running, do not linger in the lock while we send
    // the error code back to the user.
    LOG(ERROR, "Attempt to start an already started ring buffer");
    completer.Reply(zx::clock::get_monotonic().get());
    return;
  }

  // We are idle, all of our usb requests should be sitting in the free list.
  ZX_DEBUG_ASSERT(allocated_req_cnt_ == free_req_cnt_);

  // Activate the format.
  zx_status_t status = ifc_->ActivateFormat(selected_format_ndx_, selected_frame_rate_);
  if (status != ZX_OK) {
    LOG(ERROR, "Failed to activate format %d", status);
    completer.Reply(zx::clock::get_monotonic().get());
    return;
  }

  // Initialize the counters used to...
  // 1) generate the short/long packet cadence.
  // 2) generate notifications.
  // 3) track the position in the ring buffer.
  fractional_bpp_acc_ = 0;
  notification_acc_ = 0;
  ring_buffer_offset_ = 0;
  ring_buffer_pos_ = 0;

  // Schedule the frame number which the first transaction will go out on.
  //
  // TODO(johngro): This cannot be the current frame number, that train
  // has already left the station.  It probably should not be the next frame
  // number either as that train might be just about to leave the station.
  //
  // For now, set this to be the current frame number +2 and use the first
  // transaction complete callback to estimate the DMA start time.  Moving
  // forward, when the USB bus driver can tell us which frame a transaction
  // went out on, schedule the transaction using the special "on the next USB
  // isochronous frame" sentinel value and figure out which frame that was
  // during the callback.
  usb_frame_num_ = usb_get_current_frame(&parent_.usb_proto()) + 2;

  // Flag ourselves as being in the starting state, then queue up all of our
  // transactions.
  ring_buffer_state_ = RingBufferState::STARTING;
  while (!list_is_empty(&free_req_))
    QueueRequestLocked();

  *start_completer_ = completer.ToAsync();
}

void UsbAudioStream::Stop(StopCompleter::Sync& completer) {
  fbl::AutoLock req_lock(&req_lock_);

  // TODO(johngro): Fix this to use the cancel transaction capabilities added
  // to the USB bus driver.
  //
  // Also, investigate whether or not the cancel interface is synchronous or
  // whether we will need to maintain an intermediate stopping state.
  if (ring_buffer_state_ != RingBufferState::STARTED) {
    LOG(ERROR, "Attempt to stop a not started ring buffer");
    completer.Reply();
  }

  ring_buffer_state_ = RingBufferState::STOPPING;
  *stop_completer_ = completer.ToAsync();
}

void UsbAudioStream::RequestComplete(usb_request_t* req) {
  enum class Action {
    NONE,
    SIGNAL_STARTED,
    SIGNAL_STOPPED,
    NOTIFY_POSITION,
    HANDLE_UNPLUG,
  };

  audio_fidl::RingBufferPositionInfo position_info = {};

  uint64_t complete_time = zx::clock::get_monotonic().get();
  Action when_finished = Action::NONE;

  // TODO(johngro) : See fxbug.dev/30888.  Eliminate this as soon as we have a more
  // official way of meeting real-time latency requirements.  Also, the fact
  // that this boosting gets done after the first transaction completes
  // degrades the quality of the startup time estimate (if the system is under
  // high load when the system starts up).  As a general issue, there are
  // better ways of refining this estimate than bumping the thread prio before
  // the first transaction gets queued.  Therefor, we just have a poor
  // estimate for now and will need to live with the consequences.
  if (!req_complete_prio_bumped_) {
    zx_object_set_profile(zx_thread_self(), profile_handle_.get(), 0);
    req_complete_prio_bumped_ = true;
  }

  {
    fbl::AutoLock req_lock(&req_lock_);

    // Cache the status and length of this usb request.
    zx_status_t req_status = req->response.status;
    uint32_t req_length = static_cast<uint32_t>(req->header.length);

    // Complete the usb request.  This will return the transaction to the free
    // list and (in the case of an input stream) copy the payload to the
    // ring buffer, and update the ring buffer position.
    //
    // TODO(johngro): copying the payload out of the ring buffer is an
    // operation which goes away when we get to the zero copy world.
    CompleteRequestLocked(req);

    // Did the transaction fail because the device was unplugged?  If so,
    // enter the stopping state and close the connections to our clients.
    if (req_status == ZX_ERR_IO_NOT_PRESENT) {
      ring_buffer_state_ = RingBufferState::STOPPING_AFTER_UNPLUG;
    } else {
      // If we are supposed to be delivering notifications, check to see
      // if it is time to do so.
      if (bytes_per_notification_) {
        notification_acc_ += req_length;

        if ((ring_buffer_state_ == RingBufferState::STARTED) &&
            (notification_acc_ >= bytes_per_notification_)) {
          when_finished = Action::NOTIFY_POSITION;
          notification_acc_ = (notification_acc_ % bytes_per_notification_);
          position_info.timestamp = zx::clock::get_monotonic().get();
          position_info.position = ring_buffer_pos_;
        }
      }
    }

    switch (ring_buffer_state_) {
      case RingBufferState::STOPPING:
        if (free_req_cnt_ == allocated_req_cnt_) {
          when_finished = Action::SIGNAL_STOPPED;
        }
        break;

      case RingBufferState::STOPPING_AFTER_UNPLUG:
        if (free_req_cnt_ == allocated_req_cnt_) {
          when_finished = Action::HANDLE_UNPLUG;
        }
        break;

      case RingBufferState::STARTING:
        when_finished = Action::SIGNAL_STARTED;
        break;

      case RingBufferState::STARTED:
        QueueRequestLocked();
        break;

      case RingBufferState::STOPPED:
      default:
        LOG(ERROR, "Invalid state (%u) in %s\n", static_cast<uint32_t>(ring_buffer_state_),
            __PRETTY_FUNCTION__);
        ZX_DEBUG_ASSERT(false);
        break;
    }
  }

  if (when_finished != Action::NONE) {
    fbl::AutoLock lock(&lock_);
    switch (when_finished) {
      case Action::SIGNAL_STARTED:
        if (rb_channel_ != nullptr) {
          // TODO(johngro) : this start time estimate is not as good as it
          // could be.  We really need to have the USB bus driver report
          // the relationship between the USB frame counter and the system
          // tick counter (and track the relationship in the case that the
          // USB oscillator is not derived from the system oscillator).
          // Then we can accurately report the start time as the time of
          // the tick on which we scheduled the first transaction.
          fbl::AutoLock req_lock(&req_lock_);
          start_completer_->Reply(zx_time_sub_duration(complete_time, ZX_MSEC(1)));
        }
        {
          fbl::AutoLock req_lock(&req_lock_);
          ring_buffer_state_ = RingBufferState::STARTED;
        }
        break;

      case Action::HANDLE_UNPLUG:
        if (rb_channel_ != nullptr) {
          rb_channel_.reset();
        }

        if (stream_channel_ != nullptr) {
          stream_channel_.reset();
        }

        {
          fbl::AutoLock req_lock(&req_lock_);
          ring_buffer_state_ = RingBufferState::STOPPED;
        }
        break;

      case Action::SIGNAL_STOPPED:
        if (rb_channel_ != nullptr) {
          fbl::AutoLock req_lock(&req_lock_);
          stop_completer_->Reply();
        }
        {
          fbl::AutoLock req_lock(&req_lock_);
          ring_buffer_state_ = RingBufferState::STOPPED;
          ifc_->ActivateIdleFormat();
        }
        break;

      case Action::NOTIFY_POSITION: {
        fbl::AutoLock req_lock(&req_lock_);
        if (position_completer_) {
          position_completer_->Reply(position_info);
          position_completer_.reset();
        }
      } break;

      default:
        ZX_DEBUG_ASSERT(false);
        break;
    }
  }
}

void UsbAudioStream::QueueRequestLocked() {
  ZX_DEBUG_ASSERT((ring_buffer_state_ == RingBufferState::STARTING) ||
                  (ring_buffer_state_ == RingBufferState::STARTED));
  ZX_DEBUG_ASSERT(!list_is_empty(&free_req_));

  // Figure out how much we want to send or receive this time (short or long
  // packet)
  uint32_t todo = bytes_per_packet_;
  fractional_bpp_acc_ += fractional_bpp_inc_;
  if (fractional_bpp_acc_ >= iso_packet_rate_) {
    fractional_bpp_acc_ -= iso_packet_rate_;
    todo += frame_size_;
    ZX_DEBUG_ASSERT(fractional_bpp_acc_ < iso_packet_rate_);
  }

  // Grab a free usb request.
  auto req = usb_req_list_remove_head(&free_req_, parent_.parent_req_size());
  ZX_DEBUG_ASSERT(req != nullptr);
  ZX_DEBUG_ASSERT(free_req_cnt_ > 0);
  --free_req_cnt_;

  // If this is an output stream, copy our data into the usb request.
  // TODO(johngro): eliminate this when we can get to a zero-copy world.
  if (!is_input()) {
    uint32_t avail = ring_buffer_size_ - ring_buffer_offset_;
    ZX_DEBUG_ASSERT(ring_buffer_offset_ < ring_buffer_size_);
    ZX_DEBUG_ASSERT((avail % frame_size_) == 0);
    uint32_t amt = std::min(avail, todo);

    const uint8_t* src = reinterpret_cast<uint8_t*>(ring_buffer_virt_) + ring_buffer_offset_;
    // Not security-critical -- we're copying to a ring buffer that's moving based off of time
    // anyways. If we don't copy enough data we'll just keep playing the same sample in a loop.
    __UNUSED ssize_t copied = usb_request_copy_to(req, src, amt, 0);
    if (amt == avail) {
      ring_buffer_offset_ = todo - amt;
      if (ring_buffer_offset_ > 0) {
        copied = usb_request_copy_to(req, ring_buffer_virt_, ring_buffer_offset_, amt);
      }
    } else {
      ring_buffer_offset_ += amt;
    }
  }

  req->header.frame = usb_frame_num_++;
  req->header.length = todo;
  usb_request_complete_t complete = {
      .callback = UsbAudioStream::RequestCompleteCallback,
      .ctx = this,
  };
  usb_request_queue(&parent_.usb_proto(), req, &complete);
}

void UsbAudioStream::CompleteRequestLocked(usb_request_t* req) {
  ZX_DEBUG_ASSERT(req);

  // If we are an input stream, copy the payload into the ring buffer.
  if (is_input()) {
    uint32_t todo = static_cast<uint32_t>(req->header.length);

    uint32_t avail = ring_buffer_size_ - ring_buffer_offset_;
    ZX_DEBUG_ASSERT(ring_buffer_offset_ < ring_buffer_size_);
    ZX_DEBUG_ASSERT((avail % frame_size_) == 0);

    uint32_t amt = std::min(avail, todo);
    uint8_t* dst = reinterpret_cast<uint8_t*>(ring_buffer_virt_) + ring_buffer_offset_;

    if (req->response.status == ZX_OK) {
      __UNUSED ssize_t size = usb_request_copy_from(req, dst, amt, 0);
      if (amt < todo) {
        __UNUSED ssize_t size = usb_request_copy_from(req, ring_buffer_virt_, todo - amt, amt);
      }
    } else {
      // TODO(johngro): filling with zeros is only the proper thing to do
      // for signed formats.  USB does support unsigned 8-bit audio; if
      // that is our format, we should fill with 0x80 instead in order to
      // fill with silence.
      memset(dst, 0, amt);
      if (amt < todo) {
        memset(ring_buffer_virt_, 0, todo - amt);
      }
    }
  }

  // Update the ring buffer position.
  ring_buffer_pos_ += static_cast<uint32_t>(req->header.length);
  if (ring_buffer_pos_ >= ring_buffer_size_) {
    ring_buffer_pos_ -= ring_buffer_size_;
    ZX_DEBUG_ASSERT(ring_buffer_pos_ < ring_buffer_size_);
  }

  // If this is an input stream, the ring buffer offset should always be equal
  // to the stream position.
  if (is_input()) {
    ring_buffer_offset_ = ring_buffer_pos_;
  }

  // Return the transaction to the free list.
  zx_status_t status = usb_req_list_add_head(&free_req_, req, parent_.parent_req_size());
  ZX_DEBUG_ASSERT(status == ZX_OK);
  ++free_req_cnt_;
  ZX_DEBUG_ASSERT(free_req_cnt_ <= allocated_req_cnt_);
}

void UsbAudioStream::DeactivateStreamChannelLocked(StreamChannel* channel) {
  ZX_DEBUG_ASSERT(stream_channel_.get() == channel);
  ZX_DEBUG_ASSERT(rb_channel_.get() != channel);
  stream_channel_.reset();
}

void UsbAudioStream::DeactivateRingBufferChannelLocked(const Channel* channel) {
  ZX_DEBUG_ASSERT(stream_channel_.get() != channel);
  ZX_DEBUG_ASSERT(rb_channel_.get() == channel);

  {
    fbl::AutoLock req_lock(&req_lock_);
    if (ring_buffer_state_ != RingBufferState::STOPPED) {
      ring_buffer_state_ = RingBufferState::STOPPING;
    }
  }

  rb_channel_.reset();
}

}  // namespace usb
}  // namespace audio
