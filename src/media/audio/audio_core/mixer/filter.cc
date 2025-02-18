// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#include "src/media/audio/audio_core/mixer/filter.h"

#include <lib/syslog/cpp/macros.h>
#include <lib/trace/event.h>
#include <lib/zx/clock.h>

#include <memory>
#include <mutex>

#include "src/media/audio/audio_core/mixer/coefficient_table.h"

namespace media::audio::mixer {

// Display the filter table values.
void Filter::DisplayTable(const CoefficientTable& filter_coefficients) {
  FX_LOGS(INFO) << "Filter: source rate " << source_rate_ << ", dest rate " << dest_rate_
                << ", width 0x" << std::hex << side_width_;

  FX_LOGS(INFO) << " **************************************************************";
  FX_LOGS(INFO) << " *** Displaying filter coefficient data for length 0x" << std::hex
                << side_width_ << "  ***";
  FX_LOGS(INFO) << " **************************************************************";

  char str[256];
  str[0] = 0;
  int n;
  for (uint32_t idx = 0; idx < side_width_; ++idx) {
    if (idx % 16 == 0) {
      FX_LOGS(INFO) << str;
      n = sprintf(str, " [%5x] ", idx);
    }
    if (filter_coefficients[idx] < std::numeric_limits<float>::epsilon() &&
        filter_coefficients[idx] > -std::numeric_limits<float>::epsilon() &&
        filter_coefficients[idx] != 0.0f) {
      n += sprintf(str + n, "!%10.7f!", filter_coefficients[idx]);
    } else {
      n += sprintf(str + n, " %10.7f ", filter_coefficients[idx]);
    }
  }
  FX_LOGS(INFO) << str;
  FX_LOGS(INFO) << " **************************************************************";
}

// Used to debug computation of output values (ComputeSample), from coefficients and input values.
constexpr bool kTraceComputation = false;

float Filter::ComputeSampleFromTable(const CoefficientTable& filter_coefficients,
                                     uint32_t frac_offset, float* center) {
  FX_DCHECK(frac_offset <= frac_size_) << frac_offset;
  if constexpr (kTraceComputation) {
    FX_LOGS(INFO) << "For frac_offset " << std::hex << frac_offset << " ("
                  << (static_cast<float>(frac_offset) / frac_size_) << "):";
  }

  float result = 0.0f;

  // We use some raw pointers here to make the loops below vectorizable. Note that the coefficient
  // table stores values with an integer stride contiguously, which is what we want for the loops
  // below.
  //
  // Ex:
  //  coefficient_ptr[1] == filter_coefficients[frac_offset + frac_size_];
  //
  // Negative side first
  float* sample_ptr = center;
  size_t source_frames = (side_width_ + (frac_size_ - 1) - frac_offset) >> num_frac_bits_;
  const float* coefficient_ptr = filter_coefficients.ReadSlice(frac_offset, source_frames);
  for (size_t source_idx = 0; source_idx < source_frames; ++source_idx) {
    FX_CHECK(coefficient_ptr != nullptr);
    auto contribution = (*sample_ptr) * coefficient_ptr[source_idx];
    if constexpr (kTraceComputation) {
      FX_LOGS(INFO) << "Adding source[" << -static_cast<ssize_t>(source_idx) << "] "
                    << (*sample_ptr) << " x " << coefficient_ptr[source_idx] << " = "
                    << contribution;
    }
    result += contribution;
    --sample_ptr;
  }

  // Then positive side
  sample_ptr = center + 1;
  // Reduction of:
  //   side_width_ + (frac_size_ - 1) - (frac_size_ - frac_offset)
  source_frames = (side_width_ + frac_offset - 1) >> num_frac_bits_;
  coefficient_ptr = filter_coefficients.ReadSlice(frac_size_ - frac_offset, source_frames);
  for (size_t source_idx = 0; source_idx < source_frames; ++source_idx) {
    FX_CHECK(coefficient_ptr != nullptr);
    auto contribution = sample_ptr[source_idx] * coefficient_ptr[source_idx];
    if constexpr (kTraceComputation) {
      FX_LOGS(INFO) << "Adding source[" << 1 + source_idx << "] " << sample_ptr[source_idx] << " x "
                    << coefficient_ptr[source_idx] << " = " << contribution;
    }
    result += contribution;
  }
  if constexpr (kTraceComputation) {
    FX_LOGS(INFO) << "... to get " << result;
  }
  return result;
}

// PointFilter
//
// Calculate our nearest-neighbor filter. With it we perform frame-rate conversion.
CoefficientTable* CreatePointFilterTable(PointFilter::Inputs inputs) {
  TRACE_DURATION("audio", "CreatePointFilterTable");
  auto out = new CoefficientTable(inputs.side_width, inputs.num_frac_bits);
  auto& table = *out;
  auto width = inputs.side_width;
  auto frac_size = 1u << inputs.num_frac_bits;

  // We need not account for rate_conversion_ratio here.
  auto transition_idx = frac_size >> 1u;

  // We know that transition_idx will always be the last idx in the filter table, because in our
  // ctor we set side_width to (1u << (num_frac_bits - 1u)) + 1u, which == (frac_size >> 1u) +
  // 1u
  FX_DCHECK(transition_idx + 1u == width);

  // Just a rectangular window, actually.
  for (auto idx = 0u; idx < transition_idx; ++idx) {
    table[idx] = 1.0f;
  }

  // Here we average, so that we are zero-phase
  table[transition_idx] = 0.5f;

  for (auto idx = transition_idx + 1; idx < width; ++idx) {
    table[idx] = 0.0f;
  }

  return out;
}

// LinearFilter
//
// Calculate our linear-interpolation filter. With it we perform frame-rate conversion.
CoefficientTable* CreateLinearFilterTable(LinearFilter::Inputs inputs) {
  TRACE_DURATION("audio", "CreateLinearFilterTable");
  auto out = new CoefficientTable(inputs.side_width, inputs.num_frac_bits);
  auto& table = *out;
  auto width = inputs.side_width;
  auto frac_size = 1u << inputs.num_frac_bits;

  // We need not account for rate_conversion_ratio here.
  uint32_t transition_idx = frac_size;

  // Just a Bartlett (triangular) window, actually.
  for (auto idx = 0u; idx < transition_idx; ++idx) {
    auto factor = static_cast<float>(transition_idx - idx) / transition_idx;

    if (factor >= std::numeric_limits<float>::epsilon() ||
        factor <= -std::numeric_limits<float>::epsilon()) {
      table[idx] = factor;
    } else {
      table[idx] = 0.0f;
    }
  }
  for (auto idx = transition_idx; idx < width; ++idx) {
    table[idx] = 0.0f;
  }

  return out;
}

// SincFilter
//
// Calculate our windowed-sinc FIR filter. With it we perform band-limited frame-rate conversion.
CoefficientTable* CreateSincFilterTable(SincFilter::Inputs inputs) {
  TRACE_DURATION("audio", "CreateSincFilterTable");
  auto start_time = zx::clock::get_monotonic();
  auto out = new CoefficientTable(inputs.side_width, inputs.num_frac_bits);
  auto& table = *out;

  const auto width = inputs.side_width;
  const auto frac_one = 1u << inputs.num_frac_bits;

  // By capping this at 1.0, we set our low-pass filter to the lower of [source_rate, dest_rate].
  const double conversion_rate = M_PI * fmin(inputs.rate_conversion_ratio, 1.0);

  // Construct a sinc-based LPF, from our rate-conversion ratio and filter width.
  const double theta_factor = conversion_rate / frac_one;

  // Concurrently, calculate a VonHann window function. These form the windowed-sinc filter.
  const double normalize_width_factor = M_PI / width;

  table[0] = 1.0f;
  for (auto idx = 1u; idx < width; ++idx) {
    const double theta = theta_factor * idx;
    const double sinc_theta = sin(theta) / theta;

    // TODO(mpuryear): Pre-populate a static VonHann|Blackman|Kaiser window; don't recalc each one.
    const double raised_cosine = cos(normalize_width_factor * idx) * 0.5 + 0.5;

    table[idx] = sinc_theta * raised_cosine;
  }

  // Normalize our filter so that it doesn't change amplitude for DC (0 hz).
  // While doing this, zero out any denormal float values as an optimization.
  double amplitude_at_dc = 0.0;

  for (auto idx = frac_one; idx < width; idx += frac_one) {
    amplitude_at_dc += table[idx];
  }
  amplitude_at_dc = (2 * amplitude_at_dc) + table[0];

  const double normalize_factor = 1.0 / amplitude_at_dc;
  const double pre_normalized_epsilon = std::numeric_limits<float>::epsilon() * amplitude_at_dc;

  std::transform(table.begin(), table.end(), table.begin(),
                 [normalize_factor, pre_normalized_epsilon](float sample) -> float {
                   if (sample < pre_normalized_epsilon && sample > -pre_normalized_epsilon) {
                     return 0.0f;
                   }
                   return sample * normalize_factor;
                 });

  auto end_time = zx::clock::get_monotonic();
  FX_LOGS(INFO) << "CreateSincFilterTable took " << (end_time - start_time).to_nsecs()
                << " ns with Inputs { side_width=" << inputs.side_width
                << ", num_frac_bits=" << inputs.num_frac_bits
                << ", rate_conversion_ratio=" << inputs.rate_conversion_ratio << " }";
  return out;
}

SincFilter::CacheT* CreateSincFilterCoefficientTableCache() {
  auto cache = new SincFilter::CacheT(CreateSincFilterTable);

  auto make_inputs = [](uint32_t source_rate, uint32_t dest_rate) {
    return SincFilter::Inputs{
        .side_width = SincFilter::GetFilterWidth(source_rate, dest_rate),
        .num_frac_bits = kPtsFractionalBits,
        .rate_conversion_ratio = static_cast<double>(dest_rate) / static_cast<double>(source_rate),
    };
  };

  // To avoid lengthy construction time, cache some coefficient tables persistently.
  // See fxbug.dev/45074 and fxbug.dev/57666.
  SincFilter::persistent_cache_ = new std::vector<SincFilter::CacheT::SharedPtr>;
  SincFilter::persistent_cache_->push_back(cache->Get(make_inputs(48000, 48000)));
  SincFilter::persistent_cache_->push_back(cache->Get(make_inputs(96000, 48000)));
  SincFilter::persistent_cache_->push_back(cache->Get(make_inputs(48000, 96000)));
  SincFilter::persistent_cache_->push_back(cache->Get(make_inputs(96000, 16000)));
  SincFilter::persistent_cache_->push_back(cache->Get(make_inputs(44100, 48000)));
  return cache;
}

// static
PointFilter::CacheT* const PointFilter::cache_ = new PointFilter::CacheT(CreatePointFilterTable);
LinearFilter::CacheT* const LinearFilter::cache_ =
    new LinearFilter::CacheT(CreateLinearFilterTable);

// Must initialize persistent_cache_ first as it's used by the Create function.
std::vector<SincFilter::CacheT::SharedPtr>* SincFilter::persistent_cache_ = nullptr;
SincFilter::CacheT* const SincFilter::cache_ = CreateSincFilterCoefficientTableCache();

}  // namespace media::audio::mixer
