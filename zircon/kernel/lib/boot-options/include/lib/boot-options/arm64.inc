// Copyright 2021 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

// This file is #include'd multiple times with the DEFINE_OPTION macro defined.
// See kernel/lib/boot-options/README.md for details.
//
// These are the arm64-specific boot options.

DEFINE_OPTION("kernel.arm64.disable_spec_mitigations", bool, disable_arm64_spec_mitigations, {false}, R"""(
If set, disables all speculative execution information leak mitigations.

If unset, the per-mitigation defaults will be used.
)""")

DEFINE_OPTION("kernel.arm64.event-stream.enable", bool, event_stream_enabled, {false}, R"""(
When enabled, each ARM cpu will enable an event stream generator, which per-cpu
sets the hidden event flag at a particular rate. This has the effect of kicking
cpus out of any WFE states they may be sitting in.
)""")

DEFINE_OPTION("kernel.arm64.event-stream.freq-hz", uint32_t, event_stream_freq_hz, {10000}, R"""(
If the event stream is enabled, specifies the frequency at which it will attempt
to run. The resolution is limited, so the driver will only be able to pick the
nearest power of 2 from the cpu timer counter.
)""")
