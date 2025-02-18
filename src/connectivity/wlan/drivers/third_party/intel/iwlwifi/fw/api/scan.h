/******************************************************************************
 *
 * Copyright(c) 2012 - 2014 Intel Corporation. All rights reserved.
 * Copyright(c) 2013 - 2015 Intel Mobile Communications GmbH
 * Copyright(c) 2016 - 2017 Intel Deutschland GmbH
 * Copyright(c) 2018        Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *  * Neither the name Intel Corporation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *****************************************************************************/

#ifndef SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_INTEL_IWLWIFI_FW_API_SCAN_H_
#define SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_INTEL_IWLWIFI_FW_API_SCAN_H_

#include <stdint.h>

#include <ddk/hw/wlan/ieee80211/c/banjo.h>

#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/fuchsia_porting.h"

/* Scan Commands, Responses, Notifications */

/* Max number of IEs for direct SSID scans in a command */
#define PROBE_OPTION_MAX 20

/**
 * struct iwl_ssid_ie - directed scan network information element
 *
 * Up to 20 of these may appear in REPLY_SCAN_CMD,
 * selected by "type" bit field in struct iwl_scan_channel;
 * each channel may select different ssids from among the 20 entries.
 * SSID IEs get transmitted in reverse order of entry.
 *
 * @id: element ID
 * @len: element length
 * @ssid: element (SSID) data
 */
struct iwl_ssid_ie {
  uint8_t id;
  uint8_t len;
  uint8_t ssid[IEEE80211_SSID_LEN_MAX];
} __packed; /* SCAN_DIRECT_SSID_IE_API_S_VER_1 */

/* scan offload */
#define IWL_SCAN_MAX_BLACKLIST_LEN 64
#define IWL_SCAN_SHORT_BLACKLIST_LEN 16
#define IWL_SCAN_MAX_PROFILES 11
#define SCAN_OFFLOAD_PROBE_REQ_SIZE 512

/* Default watchdog (in MS) for scheduled scan iteration */
#define IWL_SCHED_SCAN_WATCHDOG cpu_to_le16(15000)

#define IWL_GOOD_CRC_TH_DEFAULT cpu_to_le16(1)
#define CAN_ABORT_STATUS 1

#define IWL_FULL_SCAN_MULTIPLIER 5
#define IWL_FAST_SCHED_SCAN_ITERATIONS 3
#define IWL_MAX_SCHED_SCAN_PLANS 2
#define IWL_SCAN_MAX_NUM_OF_CHANNELS 52

/* TODO(49686): check if there is a dynamic way to derive this value. Currently
 * this is set to 2x the average time it takes to finish scan.
 */
#define IWL_SCAN_TIMEOUT_SEC 10

enum scan_framework_client {
  SCAN_CLIENT_SCHED_SCAN = BIT(0),
  SCAN_CLIENT_NETDETECT = BIT(1),
  SCAN_CLIENT_ASSET_TRACKING = BIT(2),
};

/**
 * struct iwl_scan_offload_blacklist - SCAN_OFFLOAD_BLACKLIST_S
 * @ssid:       MAC address to filter out
 * @reported_rssi:  AP rssi reported to the host
 * @client_bitmap: clients ignore this entry  - enum scan_framework_client
 */
struct iwl_scan_offload_blacklist {
  uint8_t ssid[ETH_ALEN];
  uint8_t reported_rssi;
  uint8_t client_bitmap;
} __packed;

enum iwl_scan_offload_network_type {
  IWL_NETWORK_TYPE_BSS = 1,
  IWL_NETWORK_TYPE_IBSS = 2,
  IWL_NETWORK_TYPE_ANY = 3,
};

enum iwl_scan_offload_band_selection {
  IWL_SCAN_OFFLOAD_SELECT_2_4 = 0x4,
  IWL_SCAN_OFFLOAD_SELECT_5_2 = 0x8,
  IWL_SCAN_OFFLOAD_SELECT_ANY = 0xc,
};

/**
 * struct iwl_scan_offload_profile - SCAN_OFFLOAD_PROFILE_S
 * @ssid_index:     index to ssid list in fixed part
 * @unicast_cipher: encryption algorithm to match - bitmap
 * @auth_alg:       authentication algorithm to match - bitmap
 * @network_type:   enum iwl_scan_offload_network_type
 * @band_selection: enum iwl_scan_offload_band_selection
 * @client_bitmap:  clients waiting for match - enum scan_framework_client
 * @reserved:       reserved
 */
struct iwl_scan_offload_profile {
  uint8_t ssid_index;
  uint8_t unicast_cipher;
  uint8_t auth_alg;
  uint8_t network_type;
  uint8_t band_selection;
  uint8_t client_bitmap;
  uint8_t reserved[2];
} __packed;

/**
 * struct iwl_scan_offload_profile_cfg - SCAN_OFFLOAD_PROFILES_CFG_API_S_VER_1
 * @profiles:       profiles to search for match
 * @blacklist_len:  length of blacklist
 * @num_profiles:   num of profiles in the list
 * @match_notify:   clients waiting for match found notification
 * @pass_match:     clients waiting for the results
 * @active_clients: active clients bitmap - enum scan_framework_client
 * @any_beacon_notify:  clients waiting for match notification without match
 * @reserved:       reserved
 */
struct iwl_scan_offload_profile_cfg {
  struct iwl_scan_offload_profile profiles[IWL_SCAN_MAX_PROFILES];
  uint8_t blacklist_len;
  uint8_t num_profiles;
  uint8_t match_notify;
  uint8_t pass_match;
  uint8_t active_clients;
  uint8_t any_beacon_notify;
  uint8_t reserved[2];
} __packed;

/**
 * struct iwl_scan_schedule_lmac - schedule of scan offload
 * @delay:      delay between iterations, in seconds.
 * @iterations:     num of scan iterations
 * @full_scan_mul:  number of partial scans before each full scan
 */
struct iwl_scan_schedule_lmac {
  __le16 delay;
  uint8_t iterations;
  uint8_t full_scan_mul;
} __packed; /* SCAN_SCHEDULE_API_S */

enum iwl_scan_offload_complete_status {
  IWL_SCAN_OFFLOAD_COMPLETED = 1,
  IWL_SCAN_OFFLOAD_ABORTED = 2,
};

enum iwl_scan_ebs_status {
  IWL_SCAN_EBS_SUCCESS,
  IWL_SCAN_EBS_FAILED,
  IWL_SCAN_EBS_CHAN_NOT_FOUND,
  IWL_SCAN_EBS_INACTIVE,
};

/**
 * struct iwl_scan_req_tx_cmd - SCAN_REQ_TX_CMD_API_S
 * @tx_flags: combination of TX_CMD_FLG_*
 * @rate_n_flags: rate for *all* Tx attempts, if TX_CMD_FLG_STA_RATE_MSK is
 *  cleared. Combination of RATE_MCS_*
 * @sta_id: index of destination station in FW station table
 * @reserved: for alignment and future use
 */
struct iwl_scan_req_tx_cmd {
  __le32 tx_flags;
  __le32 rate_n_flags;
  uint8_t sta_id;
  uint8_t reserved[3];
} __packed;

enum iwl_scan_channel_flags_lmac {
  IWL_UNIFIED_SCAN_CHANNEL_FULL = BIT(27),
  IWL_UNIFIED_SCAN_CHANNEL_PARTIAL = BIT(28),
};

/**
 * struct iwl_scan_channel_cfg_lmac - SCAN_CHANNEL_CFG_S_VER2
 * @flags:      bits 1-20: directed scan to i'th ssid
 *          other bits &enum iwl_scan_channel_flags_lmac
 * @channel_num:    channel number 1-13 etc
 * @iter_count:     scan iteration on this channel
 * @iter_interval:  interval in seconds between iterations on one channel
 */
struct iwl_scan_channel_cfg_lmac {
  __le32 flags;
  __le16 channel_num;
  __le16 iter_count;
  __le32 iter_interval;
} __packed;

/*
 * struct iwl_scan_probe_segment - PROBE_SEGMENT_API_S_VER_1
 * @offset: offset in the data block
 * @len: length of the segment
 */
struct iwl_scan_probe_segment {
  __le16 offset;
  __le16 len;
} __packed;

/* iwl_scan_probe_req - PROBE_REQUEST_FRAME_API_S_VER_2
 * @mac_header: first (and common) part of the probe
 * @band_data: band specific data
 * @common_data: last (and common) part of the probe
 * @buf: raw data block
 */
struct iwl_scan_probe_req {
  struct iwl_scan_probe_segment mac_header;
  struct iwl_scan_probe_segment band_data[2];
  struct iwl_scan_probe_segment common_data;
  uint8_t buf[SCAN_OFFLOAD_PROBE_REQ_SIZE];
} __packed;

enum iwl_scan_channel_flags {
  IWL_SCAN_CHANNEL_FLAG_EBS = BIT(0),
  IWL_SCAN_CHANNEL_FLAG_EBS_ACCURATE = BIT(1),
  IWL_SCAN_CHANNEL_FLAG_CACHE_ADD = BIT(2),
  IWL_SCAN_CHANNEL_FLAG_EBS_FRAG = BIT(3),
};

/* struct iwl_scan_channel_opt - CHANNEL_OPTIMIZATION_API_S
 * @flags: enum iwl_scan_channel_flags
 * @non_ebs_ratio: defines the ratio of number of scan iterations where EBS is
 *  involved.
 *  1 - EBS is disabled.
 *  2 - every second scan will be full scan(and so on).
 */
struct iwl_scan_channel_opt {
  __le16 flags;
  __le16 non_ebs_ratio;
} __packed;

/**
 * enum iwl_mvm_lmac_scan_flags - LMAC scan flags
 * @IWL_MVM_LMAC_SCAN_FLAG_PASS_ALL: pass all beacons and probe responses
 *  without filtering.
 * @IWL_MVM_LMAC_SCAN_FLAG_PASSIVE: force passive scan on all channels
 * @IWL_MVM_LMAC_SCAN_FLAG_PRE_CONNECTION: single channel scan
 * @IWL_MVM_LMAC_SCAN_FLAG_ITER_COMPLETE: send iteration complete notification
 * @IWL_MVM_LMAC_SCAN_FLAG_MULTIPLE_SSIDS: multiple SSID matching
 * @IWL_MVM_LMAC_SCAN_FLAG_FRAGMENTED: all passive scans will be fragmented
 * @IWL_MVM_LMAC_SCAN_FLAGS_RRM_ENABLED: insert WFA vendor-specific TPC report
 *  and DS parameter set IEs into probe requests.
 * @IWL_MVM_LMAC_SCAN_FLAG_EXTENDED_DWELL: use extended dwell time on channels
 *  1, 6 and 11.
 * @IWL_MVM_LMAC_SCAN_FLAG_MATCH: Send match found notification on matches
 */
enum iwl_mvm_lmac_scan_flags {
  IWL_MVM_LMAC_SCAN_FLAG_PASS_ALL = BIT(0),
  IWL_MVM_LMAC_SCAN_FLAG_PASSIVE = BIT(1),
  IWL_MVM_LMAC_SCAN_FLAG_PRE_CONNECTION = BIT(2),
  IWL_MVM_LMAC_SCAN_FLAG_ITER_COMPLETE = BIT(3),
  IWL_MVM_LMAC_SCAN_FLAG_MULTIPLE_SSIDS = BIT(4),
  IWL_MVM_LMAC_SCAN_FLAG_FRAGMENTED = BIT(5),
  IWL_MVM_LMAC_SCAN_FLAGS_RRM_ENABLED = BIT(6),
  IWL_MVM_LMAC_SCAN_FLAG_EXTENDED_DWELL = BIT(7),
  IWL_MVM_LMAC_SCAN_FLAG_MATCH = BIT(9),
};

enum iwl_scan_priority {
  IWL_SCAN_PRIORITY_LOW,
  IWL_SCAN_PRIORITY_MEDIUM,
  IWL_SCAN_PRIORITY_HIGH,
};

enum iwl_scan_priority_ext {
  IWL_SCAN_PRIORITY_EXT_0_LOWEST,
  IWL_SCAN_PRIORITY_EXT_1,
  IWL_SCAN_PRIORITY_EXT_2,
  IWL_SCAN_PRIORITY_EXT_3,
  IWL_SCAN_PRIORITY_EXT_4,
  IWL_SCAN_PRIORITY_EXT_5,
  IWL_SCAN_PRIORITY_EXT_6,
  IWL_SCAN_PRIORITY_EXT_7_HIGHEST,
};

/**
 * struct iwl_scan_req_lmac - SCAN_REQUEST_CMD_API_S_VER_1
 * @reserved1: for alignment and future use
 * @n_channels: num of channels to scan
 * @active_dwell: dwell time for active channels
 * @passive_dwell: dwell time for passive channels
 * @fragmented_dwell: dwell time for fragmented passive scan
 * @extended_dwell: dwell time for channels 1, 6 and 11 (in certain cases)
 * @reserved2: for alignment and future use
 * @rx_chain_select: PHY_RX_CHAIN_* flags
 * @scan_flags: &enum iwl_mvm_lmac_scan_flags
 * @max_out_time: max time (in TU) to be out of associated channel
 * @suspend_time: pause scan this long (TUs) when returning to service channel
 * @flags: RXON flags
 * @filter_flags: RXON filter
 * @tx_cmd: tx command for active scan; for 2GHz and for 5GHz
 * @direct_scan: list of SSIDs for directed active scan
 * @scan_prio: enum iwl_scan_priority
 * @iter_num: number of scan iterations
 * @delay: delay in seconds before first iteration
 * @schedule: two scheduling plans. The first one is finite, the second one can
 *  be infinite.
 * @channel_opt: channel optimization options, for full and partial scan
 * @data: channel configuration and probe request packet.
 */
struct iwl_scan_req_lmac {
  /* SCAN_REQUEST_FIXED_PART_API_S_VER_7 */
  __le32 reserved1;
  uint8_t n_channels;
  uint8_t active_dwell;
  uint8_t passive_dwell;
  uint8_t fragmented_dwell;
  uint8_t extended_dwell;
  uint8_t reserved2;
  __le16 rx_chain_select;
  __le32 scan_flags;
  __le32 max_out_time;
  __le32 suspend_time;
  /* RX_ON_FLAGS_API_S_VER_1 */
  __le32 flags;
  __le32 filter_flags;
  struct iwl_scan_req_tx_cmd tx_cmd[2];
  struct iwl_ssid_ie direct_scan[PROBE_OPTION_MAX];
  __le32 scan_prio;
  /* SCAN_REQ_PERIODIC_PARAMS_API_S */
  __le32 iter_num;
  __le32 delay;
  struct iwl_scan_schedule_lmac schedule[IWL_MAX_SCHED_SCAN_PLANS];
  struct iwl_scan_channel_opt channel_opt[2];
  uint8_t data[];
} __packed;

/**
 * struct iwl_scan_results_notif - scan results for one channel -
 *  SCAN_RESULT_NTF_API_S_VER_3
 * @channel: which channel the results are from
 * @band: 0 for 5.2 GHz, 1 for 2.4 GHz
 * @probe_status: SCAN_PROBE_STATUS_*, indicates success of probe request
 * @num_probe_not_sent: # of request that weren't sent due to not enough time
 * @duration: duration spent in channel, in usecs
 */
struct iwl_scan_results_notif {
  uint8_t channel;
  uint8_t band;
  uint8_t probe_status;
  uint8_t num_probe_not_sent;
  __le32 duration;
} __packed;

/**
 * struct iwl_lmac_scan_complete_notif - notifies end of scanning (all channels)
 *  SCAN_COMPLETE_NTF_API_S_VER_3
 * @scanned_channels: number of channels scanned (and number of valid results)
 * @status: one of SCAN_COMP_STATUS_*
 * @bt_status: BT on/off status
 * @last_channel: last channel that was scanned
 * @tsf_low: TSF timer (lower half) in usecs
 * @tsf_high: TSF timer (higher half) in usecs
 * @results: an array of scan results, only "scanned_channels" of them are valid
 */
struct iwl_lmac_scan_complete_notif {
  uint8_t scanned_channels;
  uint8_t status;
  uint8_t bt_status;
  uint8_t last_channel;
  __le32 tsf_low;
  __le32 tsf_high;
  struct iwl_scan_results_notif results[];
} __packed;

/**
 * struct iwl_scan_offload_complete - PERIODIC_SCAN_COMPLETE_NTF_API_S_VER_2
 * @last_schedule_line: last schedule line executed (fast or regular)
 * @last_schedule_iteration: last scan iteration executed before scan abort
 * @status: &enum iwl_scan_offload_complete_status
 * @ebs_status: EBS success status &enum iwl_scan_ebs_status
 * @time_after_last_iter: time in seconds elapsed after last iteration
 * @reserved: reserved
 */
struct iwl_periodic_scan_complete {
  uint8_t last_schedule_line;
  uint8_t last_schedule_iteration;
  uint8_t status;
  uint8_t ebs_status;
  __le32 time_after_last_iter;
  __le32 reserved;
} __packed;

/* UMAC Scan API */

/* The maximum of either of these cannot exceed 8, because we use an
 * 8-bit mask (see IWL_MVM_SCAN_MASK in mvm.h).
 */
#define IWL_MVM_MAX_UMAC_SCANS 8
#define IWL_MVM_MAX_LMAC_SCANS 1

enum scan_config_flags {
  SCAN_CONFIG_FLAG_ACTIVATE = BIT(0),
  SCAN_CONFIG_FLAG_DEACTIVATE = BIT(1),
  SCAN_CONFIG_FLAG_FORBID_CHUB_REQS = BIT(2),
  SCAN_CONFIG_FLAG_ALLOW_CHUB_REQS = BIT(3),
  SCAN_CONFIG_FLAG_SET_TX_CHAINS = BIT(8),
  SCAN_CONFIG_FLAG_SET_RX_CHAINS = BIT(9),
  SCAN_CONFIG_FLAG_SET_AUX_STA_ID = BIT(10),
  SCAN_CONFIG_FLAG_SET_ALL_TIMES = BIT(11),
  SCAN_CONFIG_FLAG_SET_EFFECTIVE_TIMES = BIT(12),
  SCAN_CONFIG_FLAG_SET_CHANNEL_FLAGS = BIT(13),
  SCAN_CONFIG_FLAG_SET_LEGACY_RATES = BIT(14),
  SCAN_CONFIG_FLAG_SET_MAC_ADDR = BIT(15),
  SCAN_CONFIG_FLAG_SET_FRAGMENTED = BIT(16),
  SCAN_CONFIG_FLAG_CLEAR_FRAGMENTED = BIT(17),
  SCAN_CONFIG_FLAG_SET_CAM_MODE = BIT(18),
  SCAN_CONFIG_FLAG_CLEAR_CAM_MODE = BIT(19),
  SCAN_CONFIG_FLAG_SET_PROMISC_MODE = BIT(20),
  SCAN_CONFIG_FLAG_CLEAR_PROMISC_MODE = BIT(21),
  SCAN_CONFIG_FLAG_SET_LMAC2_FRAGMENTED = BIT(22),
  SCAN_CONFIG_FLAG_CLEAR_LMAC2_FRAGMENTED = BIT(23),

/* Bits 26-31 are for num of channels in channel_array */
#define SCAN_CONFIG_N_CHANNELS(n) ((n) << 26)
};

enum scan_config_rates {
  /* OFDM basic rates */
  SCAN_CONFIG_RATE_6M = BIT(0),
  SCAN_CONFIG_RATE_9M = BIT(1),
  SCAN_CONFIG_RATE_12M = BIT(2),
  SCAN_CONFIG_RATE_18M = BIT(3),
  SCAN_CONFIG_RATE_24M = BIT(4),
  SCAN_CONFIG_RATE_36M = BIT(5),
  SCAN_CONFIG_RATE_48M = BIT(6),
  SCAN_CONFIG_RATE_54M = BIT(7),
  /* CCK basic rates */
  SCAN_CONFIG_RATE_1M = BIT(8),
  SCAN_CONFIG_RATE_2M = BIT(9),
  SCAN_CONFIG_RATE_5M = BIT(10),
  SCAN_CONFIG_RATE_11M = BIT(11),

/* Bits 16-27 are for supported rates */
#define SCAN_CONFIG_SUPPORTED_RATE(rate) ((rate) << 16)
};

enum iwl_channel_flags {
  IWL_CHANNEL_FLAG_EBS = BIT(0),
  IWL_CHANNEL_FLAG_ACCURATE_EBS = BIT(1),
  IWL_CHANNEL_FLAG_EBS_ADD = BIT(2),
  IWL_CHANNEL_FLAG_PRE_SCAN_PASSIVE2ACTIVE = BIT(3),
};

/**
 * struct iwl_scan_dwell
 * @active:     default dwell time for active scan
 * @passive:        default dwell time for passive scan
 * @fragmented:     default dwell time for fragmented scan
 * @extended:       default dwell time for channels 1, 6 and 11
 */
struct iwl_scan_dwell {
  uint8_t active;
  uint8_t passive;
  uint8_t fragmented;
  uint8_t extended;
} __packed;

/**
 * struct iwl_scan_config
 * @flags:          enum scan_config_flags
 * @tx_chains:          valid_tx antenna - ANT_* definitions
 * @rx_chains:          valid_rx antenna - ANT_* definitions
 * @legacy_rates:       default legacy rates - enum scan_config_rates
 * @out_of_channel_time:    default max out of serving channel time
 * @suspend_time:       default max suspend time
 * @dwell:          dwells for the scan
 * @mac_addr:           default mac address to be used in probes
 * @bcast_sta_id:       the index of the station in the fw
 * @channel_flags:      default channel flags - enum iwl_channel_flags
 *              scan_config_channel_flag
 * @channel_array:      default supported channels
 */
struct iwl_scan_config_v1 {
  __le32 flags;
  __le32 tx_chains;
  __le32 rx_chains;
  __le32 legacy_rates;
  __le32 out_of_channel_time;
  __le32 suspend_time;
  struct iwl_scan_dwell dwell;
  uint8_t mac_addr[ETH_ALEN];
  uint8_t bcast_sta_id;
  uint8_t channel_flags;
  uint8_t channel_array[];
} __packed; /* SCAN_CONFIG_DB_CMD_API_S */

#define SCAN_TWO_LMACS 2
#define SCAN_LB_LMAC_IDX 0
#define SCAN_HB_LMAC_IDX 1

struct iwl_scan_config {
  __le32 flags;
  __le32 tx_chains;
  __le32 rx_chains;
  __le32 legacy_rates;
  __le32 out_of_channel_time[SCAN_TWO_LMACS];
  __le32 suspend_time[SCAN_TWO_LMACS];
  struct iwl_scan_dwell dwell;
  uint8_t mac_addr[ETH_ALEN];
  uint8_t bcast_sta_id;
  uint8_t channel_flags;
  uint8_t channel_array[];
} __packed; /* SCAN_CONFIG_DB_CMD_API_S_3 */

/**
 * enum iwl_umac_scan_flags - UMAC scan flags
 * @IWL_UMAC_SCAN_FLAG_PREEMPTIVE: scan process triggered by this scan request
 *  can be preempted by other scan requests with higher priority.
 *  The low priority scan will be resumed when the higher proirity scan is
 *  completed.
 * @IWL_UMAC_SCAN_FLAG_START_NOTIF: notification will be sent to the driver
 *  when scan starts.
 */
enum iwl_umac_scan_flags {
  IWL_UMAC_SCAN_FLAG_PREEMPTIVE = BIT(0),
  IWL_UMAC_SCAN_FLAG_START_NOTIF = BIT(1),
};

enum iwl_umac_scan_uid_offsets {
  IWL_UMAC_SCAN_UID_TYPE_OFFSET = 0,
  IWL_UMAC_SCAN_UID_SEQ_OFFSET = 8,
};

enum iwl_umac_scan_general_flags {
  IWL_UMAC_SCAN_GEN_FLAGS_PERIODIC = BIT(0),
  IWL_UMAC_SCAN_GEN_FLAGS_OVER_BT = BIT(1),
  IWL_UMAC_SCAN_GEN_FLAGS_PASS_ALL = BIT(2),
  IWL_UMAC_SCAN_GEN_FLAGS_PASSIVE = BIT(3),
  IWL_UMAC_SCAN_GEN_FLAGS_PRE_CONNECT = BIT(4),
  IWL_UMAC_SCAN_GEN_FLAGS_ITER_COMPLETE = BIT(5),
  IWL_UMAC_SCAN_GEN_FLAGS_MULTIPLE_SSID = BIT(6),
  IWL_UMAC_SCAN_GEN_FLAGS_FRAGMENTED = BIT(7),
  IWL_UMAC_SCAN_GEN_FLAGS_RRM_ENABLED = BIT(8),
  IWL_UMAC_SCAN_GEN_FLAGS_MATCH = BIT(9),
  IWL_UMAC_SCAN_GEN_FLAGS_EXTENDED_DWELL = BIT(10),
  /* Extended dwell is obselete when adaptive dwell is used, making this
   * bit reusable. Hence, probe request defer is used only when adaptive
   * dwell is supported. */
  IWL_UMAC_SCAN_GEN_FLAGS_PROB_REQ_DEFER_SUPP = BIT(10),
  IWL_UMAC_SCAN_GEN_FLAGS_LMAC2_FRAGMENTED = BIT(11),
  IWL_UMAC_SCAN_GEN_FLAGS_ADAPTIVE_DWELL = BIT(13),
  IWL_UMAC_SCAN_GEN_FLAGS_MAX_CHNL_TIME = BIT(14),
  IWL_UMAC_SCAN_GEN_FLAGS_PROB_REQ_HIGH_TX_RATE = BIT(15),
};

/**
 * enum iwl_umac_scan_general_flags2 - UMAC scan general flags #2
 * @IWL_UMAC_SCAN_GEN_FLAGS2_NOTIF_PER_CHNL: Whether to send a complete
 *  notification per channel or not.
 * @IWL_UMAC_SCAN_GEN_FLAGS2_ALLOW_CHNL_REORDER: Whether to allow channel
 *  reorder optimization or not.
 */
enum iwl_umac_scan_general_flags2 {
  IWL_UMAC_SCAN_GEN_FLAGS2_NOTIF_PER_CHNL = BIT(0),
  IWL_UMAC_SCAN_GEN_FLAGS2_ALLOW_CHNL_REORDER = BIT(1),
};

/**
 * struct iwl_scan_channel_cfg_umac
 * @flags:      bitmap - 0-19:  directed scan to i'th ssid.
 * @channel_num:    channel number 1-13 etc.
 * @iter_count:     repetition count for the channel.
 * @iter_interval:  interval between two scan iterations on one channel.
 */
struct iwl_scan_channel_cfg_umac {
  __le32 flags;
  uint8_t channel_num;
  uint8_t iter_count;
  __le16 iter_interval;
} __packed; /* SCAN_CHANNEL_CFG_S_VER2 */

/**
 * struct iwl_scan_umac_schedule
 * @interval: interval in seconds between scan iterations
 * @iter_count: num of scan iterations for schedule plan, 0xff for infinite loop
 * @reserved: for alignment and future use
 */
struct iwl_scan_umac_schedule {
  __le16 interval;
  uint8_t iter_count;
  uint8_t reserved;
} __packed; /* SCAN_SCHED_PARAM_API_S_VER_1 */

/**
 * struct iwl_scan_req_umac_tail - the rest of the UMAC scan request command
 *      parameters following channels configuration array.
 * @schedule: two scheduling plans.
 * @delay: delay in TUs before starting the first scan iteration
 * @reserved: for future use and alignment
 * @preq: probe request with IEs blocks
 * @direct_scan: list of SSIDs for directed active scan
 */
struct iwl_scan_req_umac_tail {
  /* SCAN_PERIODIC_PARAMS_API_S_VER_1 */
  struct iwl_scan_umac_schedule schedule[IWL_MAX_SCHED_SCAN_PLANS];
  __le16 delay;
  __le16 reserved;
  /* SCAN_PROBE_PARAMS_API_S_VER_1 */
  struct iwl_scan_probe_req preq;
  struct iwl_ssid_ie direct_scan[PROBE_OPTION_MAX];
} __packed;

/**
 * struct iwl_scan_umac_chan_param
 * @flags: channel flags &enum iwl_scan_channel_flags
 * @count: num of channels in scan request
 * @reserved: for future use and alignment
 */
struct iwl_scan_umac_chan_param {
  uint8_t flags;
  uint8_t count;
  __le16 reserved;
} __packed; /*SCAN_CHANNEL_PARAMS_API_S_VER_1 */

/**
 * struct iwl_scan_req_umac
 * @flags: &enum iwl_umac_scan_flags
 * @uid: scan id, &enum iwl_umac_scan_uid_offsets
 * @ooc_priority: out of channel priority - &enum iwl_scan_priority
 * @general_flags: &enum iwl_umac_scan_general_flags
 * @scan_start_mac_id: report the scan start TSF time according to this mac TSF
 * @extended_dwell: dwell time for channels 1, 6 and 11
 * @active_dwell: dwell time for active scan per LMAC
 * @passive_dwell: dwell time for passive scan per LMAC
 * @fragmented_dwell: dwell time for fragmented passive scan
 * @adwell_default_n_aps: for adaptive dwell the default number of APs
 *  per channel
 * @adwell_default_n_aps_social: for adaptive dwell the default
 *  number of APs per social (1,6,11) channel
 * @general_flags2: &enum iwl_umac_scan_general_flags2
 * @adwell_max_budget: for adaptive dwell the maximal budget of TU to be added
 *  to total scan time
 * @max_out_time: max out of serving channel time, per LMAC - for CDB there
 *  are 2 LMACs
 * @suspend_time: max suspend time, per LMAC - for CDB there are 2 LMACs
 * @scan_priority: scan internal prioritization &enum iwl_scan_priority
 * @num_of_fragments: Number of fragments needed for full coverage per band.
 *  Relevant only for fragmented scan.
 * @channel: &struct iwl_scan_umac_chan_param
 * @reserved: for future use and alignment
 * @reserved3: for future use and alignment
 * @data: &struct iwl_scan_channel_cfg_umac and
 *  &struct iwl_scan_req_umac_tail
 */
struct iwl_scan_req_umac {
  __le32 flags;
  __le32 uid;
  __le32 ooc_priority;
  __le16 general_flags;
  uint8_t reserved;
  uint8_t scan_start_mac_id;
  union {
    struct {
      uint8_t extended_dwell;
      uint8_t active_dwell;
      uint8_t passive_dwell;
      uint8_t fragmented_dwell;
      __le32 max_out_time;
      __le32 suspend_time;
      __le32 scan_priority;
      struct iwl_scan_umac_chan_param channel;
      uint8_t data[];
    } v1; /* SCAN_REQUEST_CMD_UMAC_API_S_VER_1 */
    struct {
      uint8_t extended_dwell;
      uint8_t active_dwell;
      uint8_t passive_dwell;
      uint8_t fragmented_dwell;
      __le32 max_out_time[SCAN_TWO_LMACS];
      __le32 suspend_time[SCAN_TWO_LMACS];
      __le32 scan_priority;
      struct iwl_scan_umac_chan_param channel;
      uint8_t data[];
    } v6; /* SCAN_REQUEST_CMD_UMAC_API_S_VER_6 */
    struct {
      uint8_t active_dwell;
      uint8_t passive_dwell;
      uint8_t fragmented_dwell;
      uint8_t adwell_default_n_aps;
      uint8_t adwell_default_n_aps_social;
      uint8_t reserved3;
      __le16 adwell_max_budget;
      __le32 max_out_time[SCAN_TWO_LMACS];
      __le32 suspend_time[SCAN_TWO_LMACS];
      __le32 scan_priority;
      struct iwl_scan_umac_chan_param channel;
      uint8_t data[];
    } v7; /* SCAN_REQUEST_CMD_UMAC_API_S_VER_7 */
    struct {
      uint8_t active_dwell[SCAN_TWO_LMACS];
      uint8_t reserved2;
      uint8_t adwell_default_n_aps;
      uint8_t adwell_default_n_aps_social;
      uint8_t general_flags2;
      __le16 adwell_max_budget;
      __le32 max_out_time[SCAN_TWO_LMACS];
      __le32 suspend_time[SCAN_TWO_LMACS];
      __le32 scan_priority;
      uint8_t passive_dwell[SCAN_TWO_LMACS];
      uint8_t num_of_fragments[SCAN_TWO_LMACS];
      struct iwl_scan_umac_chan_param channel;
      uint8_t data[];
    } v8; /* SCAN_REQUEST_CMD_UMAC_API_S_VER_8 */
  };
} __packed;

#define IWL_SCAN_REQ_UMAC_SIZE_V8 sizeof(struct iwl_scan_req_umac)
#define IWL_SCAN_REQ_UMAC_SIZE_V7 48
#define IWL_SCAN_REQ_UMAC_SIZE_V6 44
#define IWL_SCAN_REQ_UMAC_SIZE_V1 36

/**
 * struct iwl_umac_scan_abort
 * @uid: scan id, &enum iwl_umac_scan_uid_offsets
 * @flags: reserved
 */
struct iwl_umac_scan_abort {
  __le32 uid;
  __le32 flags;
} __packed; /* SCAN_ABORT_CMD_UMAC_API_S_VER_1 */

/**
 * struct iwl_umac_scan_complete
 * @uid: scan id, &enum iwl_umac_scan_uid_offsets
 * @last_schedule: last scheduling line
 * @last_iter: last scan iteration number
 * @status: &enum iwl_scan_offload_complete_status
 * @ebs_status: &enum iwl_scan_ebs_status
 * @time_from_last_iter: time elapsed from last iteration
 * @reserved: for future use
 */
struct iwl_umac_scan_complete {
  __le32 uid;
  uint8_t last_schedule;
  uint8_t last_iter;
  uint8_t status;
  uint8_t ebs_status;
  __le32 time_from_last_iter;
  __le32 reserved;
} __packed; /* SCAN_COMPLETE_NTF_UMAC_API_S_VER_1 */

#define SCAN_OFFLOAD_MATCHING_CHANNELS_LEN 5
/**
 * struct iwl_scan_offload_profile_match - match information
 * @bssid: matched bssid
 * @reserved: reserved
 * @channel: channel where the match occurred
 * @energy: energy
 * @matching_feature: feature matches
 * @matching_channels: bitmap of channels that matched, referencing
 *  the channels passed in tue scan offload request
 */
struct iwl_scan_offload_profile_match {
  uint8_t bssid[ETH_ALEN];
  __le16 reserved;
  uint8_t channel;
  uint8_t energy;
  uint8_t matching_feature;
  uint8_t matching_channels[SCAN_OFFLOAD_MATCHING_CHANNELS_LEN];
} __packed; /* SCAN_OFFLOAD_PROFILE_MATCH_RESULTS_S_VER_1 */

/**
 * struct iwl_scan_offload_profiles_query - match results query response
 * @matched_profiles: bitmap of matched profiles, referencing the
 *  matches passed in the scan offload request
 * @last_scan_age: age of the last offloaded scan
 * @n_scans_done: number of offloaded scans done
 * @gp2_d0u: GP2 when D0U occurred
 * @gp2_invoked: GP2 when scan offload was invoked
 * @resume_while_scanning: not used
 * @self_recovery: obsolete
 * @reserved: reserved
 * @matches: array of match information, one for each match
 */
struct iwl_scan_offload_profiles_query {
  __le32 matched_profiles;
  __le32 last_scan_age;
  __le32 n_scans_done;
  __le32 gp2_d0u;
  __le32 gp2_invoked;
  uint8_t resume_while_scanning;
  uint8_t self_recovery;
  __le16 reserved;
  struct iwl_scan_offload_profile_match matches[IWL_SCAN_MAX_PROFILES];
} __packed; /* SCAN_OFFLOAD_PROFILES_QUERY_RSP_S_VER_2 */

/**
 * struct iwl_umac_scan_iter_complete_notif - notifies end of scanning iteration
 * @uid: scan id, &enum iwl_umac_scan_uid_offsets
 * @scanned_channels: number of channels scanned and number of valid elements in
 *  results array
 * @status: one of SCAN_COMP_STATUS_*
 * @bt_status: BT on/off status
 * @last_channel: last channel that was scanned
 * @start_tsf: TSF timer in usecs of the scan start time for the mac specified
 *  in &struct iwl_scan_req_umac.
 * @results: array of scan results, length in @scanned_channels
 */
struct iwl_umac_scan_iter_complete_notif {
  __le32 uid;
  uint8_t scanned_channels;
  uint8_t status;
  uint8_t bt_status;
  uint8_t last_channel;
  __le64 start_tsf;
  struct iwl_scan_results_notif results[];
} __packed; /* SCAN_ITER_COMPLETE_NTF_UMAC_API_S_VER_2 */

#endif  // SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_INTEL_IWLWIFI_FW_API_SCAN_H_
