/*
 * Copyright (c) 2016, 2018, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * *    * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define LOG_NIDEBUG 0

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define LOG_TAG "QTI PowerHAL"
#include <hardware/hardware.h>
#include <hardware/power.h>
#include <log/log.h>

#include "hint-data.h"
#include "metadata-defs.h"
#include "performance.h"
#include "power-common.h"
#include "utils.h"

static int video_encode_hint_sent;

const int kMinInteractiveDuration = 500;  /* ms */
const int kMaxInteractiveDuration = 5000; /* ms */
const int kMaxLaunchDuration = 5000;      /* ms */

/**
 * Returns true if the target is SDM632.
 */
static bool is_target_SDM632(void) {
    static int is_SDM632 = -1;
    int soc_id;

    if (is_SDM632 >= 0) return is_SDM632;

    soc_id = get_soc_id();
    is_SDM632 = soc_id == 349 || soc_id == 350;

    return is_SDM632;
}

static int process_video_encode_hint(void* metadata) {
    char governor[80];
    struct video_encode_metadata_t video_encode_metadata;

    if (!metadata) return HINT_NONE;

    if (get_scaling_governor(governor, sizeof(governor)) == -1) {
        ALOGE("Can't obtain scaling governor.");
        return HINT_NONE;
    }

    /* Initialize encode metadata struct fields */
    memset(&video_encode_metadata, 0, sizeof(struct video_encode_metadata_t));
    video_encode_metadata.state = -1;
    video_encode_metadata.hint_id = DEFAULT_VIDEO_ENCODE_HINT_ID;

    if (parse_video_encode_metadata((char*)metadata, &video_encode_metadata) == -1) {
        ALOGE("Error occurred while parsing metadata.");
        return HINT_NONE;
    }

    if (video_encode_metadata.state == 1) {
        if (is_schedutil_governor(governor)) {
            if (is_target_SDM632()) {
                /* sample_ms = 10mS
                 * SLB for Core0 = -6
                 * SLB for Core1 = -6
                 * SLB for Core2 = -6
                 * SLB for Core3 = -6
                 * hispeed load = 95
                 * hispeed freq = 1036 */
                int resource_values[] = {CPUBW_HWMON_SAMPLE_MS,
                                         0xa,
                                         0x40c68100,
                                         0xfffffffa,
                                         0x40c68110,
                                         0xfffffffa,
                                         0x40c68120,
                                         0xfffffffa,
                                         0x40c68130,
                                         0xfffffffa,
                                         0x41440100,
                                         0x5f,
                                         0x4143c100,
                                         0x40c};
                if (!video_encode_hint_sent) {
                    perform_hint_action(video_encode_metadata.hint_id, resource_values,
                                        ARRAY_SIZE(resource_values));
                    video_encode_hint_sent = 1;
                    return HINT_HANDLED;
                }
            } else {
                /* sample_ms = 10mS */
                int resource_values[] = {CPUBW_HWMON_SAMPLE_MS, 0xa};
                if (!video_encode_hint_sent) {
                    perform_hint_action(video_encode_metadata.hint_id, resource_values,
                                        ARRAY_SIZE(resource_values));
                    video_encode_hint_sent = 1;
                    return HINT_HANDLED;
                }
            }
        } else if (is_interactive_governor(governor)) {
            /* Sched_load and migration_notification disable
             * timer rate - 40mS*/
            int resource_values[] = {INT_OP_CLUSTER0_USE_SCHED_LOAD,      0x1,
                                     INT_OP_CLUSTER0_USE_MIGRATION_NOTIF, 0x1,
                                     INT_OP_CLUSTER0_TIMER_RATE,          BIG_LITTLE_TR_MS_40};
            if (!video_encode_hint_sent) {
                perform_hint_action(video_encode_metadata.hint_id, resource_values,
                                    ARRAY_SIZE(resource_values));
                video_encode_hint_sent = 1;
                return HINT_HANDLED;
            }
        }
    } else if (video_encode_metadata.state == 0) {
        if (is_interactive_governor(governor) || is_schedutil_governor(governor)) {
            undo_hint_action(video_encode_metadata.hint_id);
            video_encode_hint_sent = 0;
            return HINT_HANDLED;
        }
    }
    return HINT_NONE;
}

static void process_interaction_hint(void* data) {
    static struct timespec s_previous_boost_timespec;
    static int s_previous_duration = 0;
    static int interaction_handle = -1;

    struct timespec cur_boost_timespec;
    long long elapsed_time;
    int duration = kMinInteractiveDuration;

    if (data) {
        int input_duration = *((int*)data);
        if (input_duration > duration) {
            duration = (input_duration > kMaxInteractiveDuration) ? kMaxInteractiveDuration
                                                                  : input_duration;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &cur_boost_timespec);

    elapsed_time = calc_timespan_us(s_previous_boost_timespec, cur_boost_timespec);
    // don't hint if it's been less than 250ms since last boost
    // also detect if we're doing anything resembling a fling
    // support additional boosting in case of flings
    if (elapsed_time < 250000 && duration <= 750) {
        return;
    }
    s_previous_boost_timespec = cur_boost_timespec;
    s_previous_duration = duration;

    if (CHECK_HANDLE(interaction_handle)) {
        release_request(interaction_handle);
    }

    interaction_handle =
            perf_hint_enable_with_type(VENDOR_HINT_SCROLL_BOOST, duration, SCROLL_VERTICAL);
}

static int process_activity_launch_hint(void* data) {
    static int launch_handle = -1;
    static int launch_mode = 0;

    // release lock early if launch has finished
    if (!data) {
        if (CHECK_HANDLE(launch_handle)) {
            release_request(launch_handle);
            launch_handle = -1;
        }
        launch_mode = 0;
        return HINT_HANDLED;
    }

    if (!launch_mode) {
        launch_handle = perf_hint_enable_with_type(VENDOR_HINT_FIRST_LAUNCH_BOOST,
                                                   kMaxLaunchDuration, LAUNCH_BOOST_V1);
        if (!CHECK_HANDLE(launch_handle)) {
            ALOGE("Failed to perform launch boost");
            return HINT_NONE;
        }
        launch_mode = 1;
    }
    return HINT_HANDLED;
}

int power_hint_override(power_hint_t hint, void* data) {
    int ret_val = HINT_NONE;
    switch (hint) {
        case POWER_HINT_VIDEO_ENCODE:
            ret_val = process_video_encode_hint(data);
            break;
        case POWER_HINT_INTERACTION:
            process_interaction_hint(data);
            ret_val = HINT_HANDLED;
            break;
        case POWER_HINT_LAUNCH:
            ret_val = process_activity_launch_hint(data);
            break;
        default:
            break;
    }
    return ret_val;
}

int set_interactive_override(int on) {
    char governor[80];

    if (get_scaling_governor(governor, sizeof(governor)) == -1) {
        ALOGE("Can't obtain scaling governor.");
        return HINT_NONE;
    }

    if (!on) {
        /* Display off */
        if (is_interactive_governor(governor)) {
            int resource_values[] = {INT_OP_CLUSTER0_TIMER_RATE, BIG_LITTLE_TR_MS_40};
            perform_hint_action(DISPLAY_STATE_HINT_ID, resource_values,
                                ARRAY_SIZE(resource_values));
        }
    } else {
        /* Display on */
        if (is_interactive_governor(governor)) {
            undo_hint_action(DISPLAY_STATE_HINT_ID);
        }
    }

    return HINT_HANDLED;
}
