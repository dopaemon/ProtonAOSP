/*
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "audio_hw_primary"
/*#define LOG_NDEBUG 0*/
#define LOG_NDDEBUG 0

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/time.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <math.h>

#include <cutils/log.h>
#include <cutils/str_parms.h>
#include <cutils/properties.h>

#include "audio_hw.h"

#define LIB_ACDB_LOADER "/system/lib/libacdbloader.so"
#define LIB_CSD_CLIENT "/system/lib/libcsd-client.so"
#define MIXER_XML_PATH "/system/etc/mixer_paths.xml"
#define MIXER_CARD 0

#define STRING_TO_ENUM(string) { #string, string }

/* Flags used to initialize acdb_settings variable that goes to ACDB library */
#define DMIC_FLAG       0x00000002
#define TTY_MODE_OFF    0x00000010
#define TTY_MODE_FULL   0x00000020
#define TTY_MODE_VCO    0x00000040
#define TTY_MODE_HCO    0x00000080
#define TTY_MODE_CLEAR  0xFFFFFF0F

struct string_to_enum {
    const char *name;
    uint32_t value;
};

static const struct string_to_enum out_channels_name_to_enum_table[] = {
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_STEREO),
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_5POINT1),
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_7POINT1),
};

static const char * const use_case_table[AUDIO_USECASE_MAX] = {
    [USECASE_AUDIO_PLAYBACK_DEEP_BUFFER] = "deep-buffer-playback",
    [USECASE_AUDIO_PLAYBACK_LOW_LATENCY] = "low-latency-playback",
    [USECASE_AUDIO_PLAYBACK_MULTI_CH] = "multi-channel-playback",
    [USECASE_AUDIO_RECORD] = "audio-record",
    [USECASE_AUDIO_RECORD_LOW_LATENCY] = "low-latency-record",
    [USECASE_VOICE_CALL] = "voice-call",
};

static const int pcm_device_table[AUDIO_USECASE_MAX][2] = {
    [USECASE_AUDIO_PLAYBACK_DEEP_BUFFER] = {0, 0},
    [USECASE_AUDIO_PLAYBACK_LOW_LATENCY] = {14, 14},
    [USECASE_AUDIO_PLAYBACK_MULTI_CH] = {1, 1},
    [USECASE_AUDIO_RECORD] = {0, 0},
    [USECASE_AUDIO_RECORD_LOW_LATENCY] = {14, 14},
    [USECASE_VOICE_CALL] = {12, 12},
};

/* Array to store sound devices */
static const char * const device_table[SND_DEVICE_MAX] = {
    [SND_DEVICE_NONE] = "none",
    /* Playback sound devices */
    [SND_DEVICE_OUT_HANDSET] = "handset",
    [SND_DEVICE_OUT_SPEAKER] = "speaker",
    [SND_DEVICE_OUT_HEADPHONES] = "headphones",
    [SND_DEVICE_OUT_SPEAKER_AND_HEADPHONES] = "speaker-and-headphones",
    [SND_DEVICE_OUT_VOICE_SPEAKER] = "voice-speaker",
    [SND_DEVICE_OUT_VOICE_HEADPHONES] = "voice-headphones",
    [SND_DEVICE_OUT_HDMI] = "hdmi",
    [SND_DEVICE_OUT_SPEAKER_AND_HDMI] = "speaker-and-hdmi",
    [SND_DEVICE_OUT_BT_SCO] = "bt-sco-headset",
    [SND_DEVICE_OUT_VOICE_HANDSET_TMUS] = "voice-handset-tmus",
    [SND_DEVICE_OUT_VOICE_TTY_FULL_HEADPHONES] = "voice-tty-full-headphones",
    [SND_DEVICE_OUT_VOICE_TTY_VCO_HEADPHONES] = "voice-tty-vco-headphones",
    [SND_DEVICE_OUT_VOICE_TTY_HCO_HANDSET] = "voice-tty-hco-handset",

    /* Capture sound devices */
    [SND_DEVICE_IN_HANDSET_MIC] = "handset-mic",
    [SND_DEVICE_IN_SPEAKER_MIC] = "speaker-mic",
    [SND_DEVICE_IN_HEADSET_MIC] = "headset-mic",
    [SND_DEVICE_IN_VOICE_SPEAKER_MIC] = "voice-speaker-mic",
    [SND_DEVICE_IN_VOICE_HEADSET_MIC] = "voice-headset-mic",
    [SND_DEVICE_IN_HDMI_MIC] = "hdmi-mic",
    [SND_DEVICE_IN_BT_SCO_MIC] = "bt-sco-mic",
    [SND_DEVICE_IN_CAMCORDER_MIC] = "camcorder-mic",
    [SND_DEVICE_IN_VOICE_DMIC_EF] = "voice-dmic-ef",
    [SND_DEVICE_IN_VOICE_DMIC_BS] = "voice-dmic-bs",
    [SND_DEVICE_IN_VOICE_DMIC_EF_TMUS] = "voice-dmic-ef-tmus",
    [SND_DEVICE_IN_VOICE_SPEAKER_DMIC_EF] = "voice-speaker-dmic-ef",
    [SND_DEVICE_IN_VOICE_SPEAKER_DMIC_BS] = "voice-speaker-dmic-bs",
    [SND_DEVICE_IN_VOICE_TTY_FULL_HEADSET_MIC] = "voice-tty-full-headset-mic",
    [SND_DEVICE_IN_VOICE_TTY_VCO_HANDSET_MIC] = "voice-tty-vco-handset-mic",
    [SND_DEVICE_IN_VOICE_TTY_HCO_HEADSET_MIC] = "voice-tty-hco-headset-mic",
    [SND_DEVICE_IN_VOICE_REC_MIC] = "voice-rec-mic",
    [SND_DEVICE_IN_VOICE_REC_DMIC_EF] = "voice-rec-dmic-ef",
    [SND_DEVICE_IN_VOICE_REC_DMIC_BS] = "voice-rec-dmic-bs",
    [SND_DEVICE_IN_VOICE_REC_DMIC_EF_FLUENCE] = "voice-rec-dmic-ef-fluence",
    [SND_DEVICE_IN_VOICE_REC_DMIC_BS_FLUENCE] = "voice-rec-dmic-bs-fluence",
};

/* ACDB IDs (audio DSP path configuration IDs) for each sound device */
static const int acdb_device_table[SND_DEVICE_MAX] = {
    [SND_DEVICE_OUT_HANDSET] = 7,
    [SND_DEVICE_OUT_SPEAKER] = 14,
    [SND_DEVICE_OUT_HEADPHONES] = 10,
    [SND_DEVICE_OUT_SPEAKER_AND_HEADPHONES] = 10,
    [SND_DEVICE_OUT_VOICE_SPEAKER] = 14,
    [SND_DEVICE_OUT_VOICE_HEADPHONES] = 10,
    [SND_DEVICE_OUT_HDMI] = 18,
    [SND_DEVICE_OUT_SPEAKER_AND_HDMI] = 14,
    [SND_DEVICE_OUT_BT_SCO] = 22,
    [SND_DEVICE_OUT_VOICE_HANDSET_TMUS] = 81,
    [SND_DEVICE_OUT_VOICE_TTY_FULL_HEADPHONES] = 17,
    [SND_DEVICE_OUT_VOICE_TTY_VCO_HEADPHONES] = 17,
    [SND_DEVICE_OUT_VOICE_TTY_HCO_HANDSET] = 37,

    [SND_DEVICE_IN_HANDSET_MIC] = 4,
    [SND_DEVICE_IN_SPEAKER_MIC] = 4,
    [SND_DEVICE_IN_HEADSET_MIC] = 8,
    [SND_DEVICE_IN_VOICE_SPEAKER_MIC] = 11,
    [SND_DEVICE_IN_VOICE_HEADSET_MIC] = 8,
    [SND_DEVICE_IN_HDMI_MIC] = 4,
    [SND_DEVICE_IN_BT_SCO_MIC] = 21,
    [SND_DEVICE_IN_CAMCORDER_MIC] = 61,
    [SND_DEVICE_IN_VOICE_DMIC_EF] = 6,
    [SND_DEVICE_IN_VOICE_DMIC_BS] = 5,
    [SND_DEVICE_IN_VOICE_DMIC_EF_TMUS] = 91,
    [SND_DEVICE_IN_VOICE_SPEAKER_DMIC_EF] = 13,
    [SND_DEVICE_IN_VOICE_SPEAKER_DMIC_BS] = 12,
    [SND_DEVICE_IN_VOICE_TTY_FULL_HEADSET_MIC] = 16,
    [SND_DEVICE_IN_VOICE_TTY_VCO_HANDSET_MIC] = 36,
    [SND_DEVICE_IN_VOICE_TTY_HCO_HEADSET_MIC] = 16,
    [SND_DEVICE_IN_VOICE_REC_MIC] = 62,
    [SND_DEVICE_IN_VOICE_REC_DMIC_EF] = 62,
    [SND_DEVICE_IN_VOICE_REC_DMIC_BS] = 62,
    /* TODO: Update with proper acdb ids */
    [SND_DEVICE_IN_VOICE_REC_DMIC_EF_FLUENCE] = 62,
    [SND_DEVICE_IN_VOICE_REC_DMIC_BS_FLUENCE] = 62,
};

int edid_get_max_channels(void);

static pthread_once_t check_op_once_ctl = PTHREAD_ONCE_INIT;
static bool is_tmus = false;

static void check_operator()
{
    char value[PROPERTY_VALUE_MAX];
    int mccmnc;
    property_get("gsm.sim.operator.numeric",value,"0");
    mccmnc = atoi(value);
    ALOGD("%s: tmus mccmnc %d", __func__, mccmnc);
    switch(mccmnc) {
    /* TMUS MCC(310), MNC(490, 260, 026) */
    case 310490:
    case 310260:
    case 310026:
        is_tmus = true;
        break;
    }
}

static bool is_operator_tmus()
{
    pthread_once(&check_op_once_ctl, check_operator);
    return is_tmus;
}

static int get_pcm_device_id(struct audio_route *ar,
                             audio_usecase_t usecase,
                             int device_type)
{
    ALOGV("%s: enter: usecase(%d)", __func__, usecase);
    int device_id;
    if (device_type == PCM_PLAYBACK)
        device_id = pcm_device_table[usecase][0];
    else
        device_id = pcm_device_table[usecase][1];
    ALOGV("%s: exit: device_id(%d)", __func__, device_id);
    return device_id;
}

static int get_acdb_device_id(snd_device_t snd_device)
{
    ALOGV("%s: enter: snd_devie(%d)", __func__, snd_device);
    int acdb_dev_id = acdb_device_table[snd_device];
    ALOGV("%s: exit: acdb_dev_id(%d)", __func__, acdb_dev_id);
    return acdb_dev_id;
}

static void add_backend_name(char *mixer_path,
                             snd_device_t snd_device)
{
    if (snd_device == SND_DEVICE_OUT_HDMI ||
            snd_device == SND_DEVICE_IN_HDMI_MIC)
        strcat(mixer_path, " hdmi");
    else if(snd_device == SND_DEVICE_OUT_BT_SCO ||
            snd_device == SND_DEVICE_IN_BT_SCO_MIC)
        strcat(mixer_path, " bt-sco");
    else if (snd_device == SND_DEVICE_OUT_SPEAKER_AND_HDMI)
        strcat(mixer_path, " speaker-and-hdmi");
}

static int enable_audio_route(struct audio_route *ar,
                              audio_usecase_t usecase,
                              snd_device_t    snd_device)
{
    ALOGV("%s: enter: usecase(%d) snd_device(%d)",
          __func__, usecase, snd_device);
    char mixer_path[50];
    strcpy(mixer_path, use_case_table[usecase]);
    add_backend_name(mixer_path, snd_device);
    audio_route_apply_path(ar, mixer_path);
    ALOGV("%s: exit", __func__);
    return 0;
}

static int disable_audio_route(struct audio_route *ar,
                               audio_usecase_t usecase,
                               snd_device_t    snd_device)
{
    ALOGV("%s: enter: usecase(%d) snd_device(%d)",
          __func__, usecase, snd_device);
    char mixer_path[50];
    strcpy(mixer_path, use_case_table[usecase]);
    add_backend_name(mixer_path, snd_device);
    audio_route_reset_path(ar, mixer_path);
    ALOGV("%s: exit", __func__);
    return 0;
}

static int enable_snd_device(struct audio_device *adev,
                             snd_device_t snd_device)
{
    int acdb_dev_id, acdb_dev_type;

    ALOGD("%s: snd_device(%d: %s)", __func__,
          snd_device, device_table[snd_device]);
    if (snd_device < SND_DEVICE_MIN ||
        snd_device >= SND_DEVICE_MAX) {
        return -EINVAL;
    }
    acdb_dev_id = get_acdb_device_id(snd_device);
    if (acdb_dev_id < 0) {
        ALOGE("%s: Could not find acdb id for device(%d)",
              __func__, snd_device);
        return -EINVAL;
    }
    if (snd_device >= SND_DEVICE_OUT_BEGIN &&
            snd_device < SND_DEVICE_OUT_END) {
        acdb_dev_type = ACDB_DEV_TYPE_OUT;
    } else {
        acdb_dev_type = ACDB_DEV_TYPE_IN;
    }
    if (adev->acdb_send_audio_cal) {
        ALOGV("%s: sending audio calibration for snd_device(%d) acdb_id(%d)",
              __func__, snd_device, acdb_dev_id);
        adev->acdb_send_audio_cal(acdb_dev_id, acdb_dev_type);
    } else {
        ALOGW("%s: Could find the symbol acdb_send_audio_cal from %s",
              __func__, LIB_ACDB_LOADER);
    }

    audio_route_apply_path(adev->audio_route, device_table[snd_device]);
    return 0;
}

static int disable_snd_device(struct audio_route *ar,
                              snd_device_t    snd_device)
{
    ALOGD("%s: enter: snd_device(%d: %s)", __func__,
          snd_device, device_table[snd_device]);
    if (snd_device < SND_DEVICE_MIN ||
        snd_device >= SND_DEVICE_MAX) {
        return -EINVAL;
    }
    audio_route_reset_path(ar, device_table[snd_device]);
    return 0;
}

static int set_hdmi_channels(struct mixer *mixer,
                             int channel_count)
{
    struct mixer_ctl *ctl;
    const char *channel_cnt_str = NULL;
    const char *mixer_ctl_name = "HDMI_RX Channels";
    switch (channel_count) {
    case 8:
        channel_cnt_str = "Eight"; break;
    case 7:
        channel_cnt_str = "Seven"; break;
    case 6:
        channel_cnt_str = "Six"; break;
    case 5:
        channel_cnt_str = "Five"; break;
    case 4:
        channel_cnt_str = "Four"; break;
    case 3:
        channel_cnt_str = "Three"; break;
    default:
        channel_cnt_str = "Two"; break;
    }
    ctl = mixer_get_ctl_by_name(mixer, mixer_ctl_name);
    if (!ctl) {
        ALOGE("%s: Could not get ctl for mixer cmd - %s",
              __func__, mixer_ctl_name);
        return -EINVAL;
    }
    ALOGV("HDMI channel count: %s", channel_cnt_str);
    mixer_ctl_set_enum_by_string(ctl, channel_cnt_str);
    return 0;
}

/* must be called with hw device mutex locked */
static void read_hdmi_channel_masks(struct stream_out *out)
{
    int channels = edid_get_max_channels();
    ALOGV("%s: enter", __func__);

    switch (channels) {
        /*
         * Do not handle stereo output in Multi-channel cases
         * Stereo case is handled in normal playback path
         */
    case 6:
        ALOGV("%s: HDMI supports 5.1", __func__);
        out->supported_channel_masks[0] = AUDIO_CHANNEL_OUT_5POINT1;
        break;
    case 8:
        ALOGV("%s: HDMI supports 5.1 and 7.1 channels", __func__);
        out->supported_channel_masks[0] = AUDIO_CHANNEL_OUT_5POINT1;
        out->supported_channel_masks[1] = AUDIO_CHANNEL_OUT_7POINT1;
        break;
    default:
        ALOGE("Unsupported number of channels (%d)", channels);
        break;
    }

    ALOGV("%s: exit", __func__);
}

static snd_device_t get_output_snd_device(struct audio_device *adev)
{
    audio_source_t  source = (adev->active_input == NULL) ?
                                AUDIO_SOURCE_DEFAULT : adev->active_input->source;
    audio_mode_t    mode   = adev->mode;
    audio_devices_t devices = adev->out_device;
    snd_device_t    snd_device = SND_DEVICE_NONE;

    ALOGV("%s: enter: output devices(%#x)", __func__, devices);
    if (devices == AUDIO_DEVICE_NONE ||
        devices & AUDIO_DEVICE_BIT_IN) {
        ALOGV("%s: Invalid output devices (%#x)", __func__, devices);
        goto exit;
    }

    if (mode == AUDIO_MODE_IN_CALL) {
        if (devices & AUDIO_DEVICE_OUT_WIRED_HEADPHONE ||
            devices & AUDIO_DEVICE_OUT_WIRED_HEADSET) {
            if (adev->tty_mode == TTY_MODE_FULL)
                snd_device = SND_DEVICE_OUT_VOICE_TTY_FULL_HEADPHONES;
            else if (adev->tty_mode == TTY_MODE_VCO)
                snd_device = SND_DEVICE_OUT_VOICE_TTY_VCO_HEADPHONES;
            else if (adev->tty_mode == TTY_MODE_HCO)
                snd_device = SND_DEVICE_OUT_VOICE_TTY_HCO_HANDSET;
            else
                snd_device = SND_DEVICE_OUT_VOICE_HEADPHONES;
        } else if (devices & AUDIO_DEVICE_OUT_ALL_SCO) {
            snd_device = SND_DEVICE_OUT_BT_SCO;
        } else if (devices & AUDIO_DEVICE_OUT_SPEAKER) {
            snd_device = SND_DEVICE_OUT_VOICE_SPEAKER;
        } else if (devices & AUDIO_DEVICE_OUT_EARPIECE) {
            if (is_operator_tmus())
                snd_device = SND_DEVICE_OUT_VOICE_HANDSET_TMUS;
            else
                snd_device = SND_DEVICE_OUT_HANDSET;
        }
        if (snd_device != SND_DEVICE_NONE) {
            goto exit;
        }
    }

    if (popcount(devices) == 2) {
        if (devices == (AUDIO_DEVICE_OUT_WIRED_HEADPHONE |
                        AUDIO_DEVICE_OUT_SPEAKER)) {
            snd_device = SND_DEVICE_OUT_SPEAKER_AND_HEADPHONES;
        } else if (devices == (AUDIO_DEVICE_OUT_WIRED_HEADSET |
                               AUDIO_DEVICE_OUT_SPEAKER)) {
            snd_device = SND_DEVICE_OUT_SPEAKER_AND_HEADPHONES;
        } else if (devices == (AUDIO_DEVICE_OUT_AUX_DIGITAL |
                               AUDIO_DEVICE_OUT_SPEAKER)) {
            snd_device = SND_DEVICE_OUT_SPEAKER_AND_HDMI;
        } else {
            ALOGE("%s: Invalid combo device(%#x)", __func__, devices);
            goto exit;
        }
        if (snd_device != SND_DEVICE_NONE) {
            goto exit;
        }
    }

    if (popcount(devices) != 1) {
        ALOGE("%s: Invalid output devices(%#x)", __func__, devices);
        goto exit;
    }

    if (devices & AUDIO_DEVICE_OUT_WIRED_HEADPHONE ||
        devices & AUDIO_DEVICE_OUT_WIRED_HEADSET) {
        snd_device = SND_DEVICE_OUT_HEADPHONES;
    } else if (devices & AUDIO_DEVICE_OUT_SPEAKER) {
        snd_device = SND_DEVICE_OUT_SPEAKER;
    } else if (devices & AUDIO_DEVICE_OUT_ALL_SCO) {
        snd_device = SND_DEVICE_OUT_BT_SCO;
    } else if (devices & AUDIO_DEVICE_OUT_AUX_DIGITAL) {
        snd_device = SND_DEVICE_OUT_HDMI ;
    } else if (devices & AUDIO_DEVICE_OUT_EARPIECE) {
        snd_device = SND_DEVICE_OUT_HANDSET;
    } else {
        ALOGE("%s: Unknown device(s) %#x", __func__, devices);
    }
exit:
    ALOGV("%s: exit: snd_device(%s)", __func__, device_table[snd_device]);
    return snd_device;
}

static snd_device_t get_input_snd_device(struct audio_device *adev)
{
    audio_source_t  source = (adev->active_input == NULL) ?
                                AUDIO_SOURCE_DEFAULT : adev->active_input->source;

    audio_mode_t    mode   = adev->mode;
    audio_devices_t out_device = adev->out_device;
    audio_devices_t in_device = ((adev->active_input == NULL) ?
                                    AUDIO_DEVICE_NONE : adev->active_input->device)
                                & ~AUDIO_DEVICE_BIT_IN;
    audio_channel_mask_t channel_mask = (adev->active_input == NULL) ?
                                AUDIO_CHANNEL_IN_MONO : adev->active_input->channel_mask;
    snd_device_t snd_device = SND_DEVICE_NONE;

    ALOGV("%s: enter: out_device(%#x) in_device(%#x)",
          __func__, out_device, in_device);
    if (mode == AUDIO_MODE_IN_CALL) {
        if (out_device == AUDIO_DEVICE_NONE) {
            ALOGE("%s: No output device set for voice call", __func__);
            goto exit;
        }
        if (adev->tty_mode != TTY_MODE_OFF) {
            if (out_device & AUDIO_DEVICE_OUT_WIRED_HEADPHONE ||
                out_device & AUDIO_DEVICE_OUT_WIRED_HEADSET) {
                switch (adev->tty_mode) {
                case TTY_MODE_FULL:
                    snd_device = SND_DEVICE_IN_VOICE_TTY_FULL_HEADSET_MIC;
                    break;
                case TTY_MODE_VCO:
                    snd_device = SND_DEVICE_IN_VOICE_TTY_VCO_HANDSET_MIC;
                    break;
                case TTY_MODE_HCO:
                    snd_device = SND_DEVICE_IN_VOICE_TTY_HCO_HEADSET_MIC;
                    break;
                default:
                    ALOGE("%s: Invalid TTY mode (%#x)", __func__, adev->tty_mode);
                }
                goto exit;
            }
        }
        if (out_device & AUDIO_DEVICE_OUT_EARPIECE ||
            out_device & AUDIO_DEVICE_OUT_WIRED_HEADPHONE) {
            if (adev->mic_type_analog || adev->fluence_in_voice_call == false) {
                snd_device = SND_DEVICE_IN_HANDSET_MIC;
            } else {
                if (adev->dualmic_config == DUALMIC_CONFIG_ENDFIRE) {
                    if (is_operator_tmus())
                        snd_device = SND_DEVICE_IN_VOICE_DMIC_EF_TMUS;
                    else
                        snd_device = SND_DEVICE_IN_VOICE_DMIC_EF;
                } else if(adev->dualmic_config == DUALMIC_CONFIG_BROADSIDE)
                    snd_device = SND_DEVICE_IN_VOICE_DMIC_BS;
                else
                    snd_device = SND_DEVICE_IN_HANDSET_MIC;
            }
        } else if (out_device & AUDIO_DEVICE_OUT_WIRED_HEADSET) {
            snd_device = SND_DEVICE_IN_VOICE_HEADSET_MIC;
        } else if (out_device & AUDIO_DEVICE_OUT_ALL_SCO) {
            snd_device = SND_DEVICE_IN_BT_SCO_MIC ;
        } else if (out_device & AUDIO_DEVICE_OUT_SPEAKER) {
            if (adev->fluence_in_voice_call &&
                    adev->dualmic_config == DUALMIC_CONFIG_ENDFIRE) {
                snd_device = SND_DEVICE_IN_VOICE_SPEAKER_DMIC_EF;
            } else if (adev->fluence_in_voice_call &&
                       adev->dualmic_config == DUALMIC_CONFIG_BROADSIDE) {
                snd_device = SND_DEVICE_IN_VOICE_SPEAKER_DMIC_BS;
            } else {
                snd_device = SND_DEVICE_IN_VOICE_SPEAKER_MIC;
            }
        }
    } else if (source == AUDIO_SOURCE_CAMCORDER) {
        if (in_device & AUDIO_DEVICE_IN_BUILTIN_MIC ||
            in_device & AUDIO_DEVICE_IN_BACK_MIC) {
            snd_device = SND_DEVICE_IN_CAMCORDER_MIC;
        }
    } else if (source == AUDIO_SOURCE_VOICE_RECOGNITION) {
        if (in_device & AUDIO_DEVICE_IN_BUILTIN_MIC) {
            if (adev->dualmic_config == DUALMIC_CONFIG_ENDFIRE) {
                if (channel_mask == AUDIO_CHANNEL_IN_FRONT_BACK)
                    snd_device = SND_DEVICE_IN_VOICE_REC_DMIC_EF;
                else if (adev->fluence_in_voice_rec)
                    snd_device = SND_DEVICE_IN_VOICE_REC_DMIC_EF_FLUENCE;
                else
                    snd_device = SND_DEVICE_IN_VOICE_REC_MIC;
            } else if (adev->dualmic_config == DUALMIC_CONFIG_BROADSIDE) {
                if (channel_mask == AUDIO_CHANNEL_IN_FRONT_BACK)
                    snd_device = SND_DEVICE_IN_VOICE_REC_DMIC_BS;
                else if (adev->fluence_in_voice_rec)
                    snd_device = SND_DEVICE_IN_VOICE_REC_DMIC_BS_FLUENCE;
                else
                    snd_device = SND_DEVICE_IN_VOICE_REC_MIC;
            } else
                snd_device = SND_DEVICE_IN_VOICE_REC_MIC;
        }
    } else if (source == AUDIO_SOURCE_VOICE_COMMUNICATION) {
        if (out_device & AUDIO_DEVICE_OUT_SPEAKER)
            in_device = AUDIO_DEVICE_IN_BACK_MIC;
    } else if (source == AUDIO_SOURCE_DEFAULT) {
        goto exit;
    }

    if (snd_device != SND_DEVICE_NONE) {
        goto exit;
    }

    if (in_device != AUDIO_DEVICE_NONE &&
            !(in_device & AUDIO_DEVICE_IN_VOICE_CALL) &&
            !(in_device & AUDIO_DEVICE_IN_COMMUNICATION)) {
        if (in_device & AUDIO_DEVICE_IN_BUILTIN_MIC) {
            snd_device = SND_DEVICE_IN_HANDSET_MIC;
        } else if (in_device & AUDIO_DEVICE_IN_BACK_MIC) {
            if (adev->mic_type_analog)
                snd_device = SND_DEVICE_IN_HANDSET_MIC;
            else
                snd_device = SND_DEVICE_IN_SPEAKER_MIC;
        } else if (in_device & AUDIO_DEVICE_IN_WIRED_HEADSET) {
            snd_device = SND_DEVICE_IN_HEADSET_MIC;
        } else if (in_device & AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET) {
            snd_device = SND_DEVICE_IN_BT_SCO_MIC ;
        } else if (in_device & AUDIO_DEVICE_IN_AUX_DIGITAL) {
            snd_device = SND_DEVICE_IN_HDMI_MIC;
        } else {
            ALOGE("%s: Unknown input device(s) %#x", __func__, in_device);
            ALOGW("%s: Using default handset-mic", __func__);
            snd_device = SND_DEVICE_IN_HANDSET_MIC;
        }
    } else {
        if (out_device & AUDIO_DEVICE_OUT_EARPIECE) {
            snd_device = SND_DEVICE_IN_HANDSET_MIC;
        } else if (out_device & AUDIO_DEVICE_OUT_WIRED_HEADSET) {
            snd_device = SND_DEVICE_IN_HEADSET_MIC;
        } else if (out_device & AUDIO_DEVICE_OUT_SPEAKER) {
            snd_device = SND_DEVICE_IN_SPEAKER_MIC;
        } else if (out_device & AUDIO_DEVICE_OUT_WIRED_HEADPHONE) {
            snd_device = SND_DEVICE_IN_HANDSET_MIC;
        } else if (out_device & AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET) {
            snd_device = SND_DEVICE_IN_BT_SCO_MIC;
        } else if (out_device & AUDIO_DEVICE_OUT_AUX_DIGITAL) {
            snd_device = SND_DEVICE_IN_HDMI_MIC;
        } else {
            ALOGE("%s: Unknown output device(s) %#x", __func__, out_device);
            ALOGW("%s: Using default handset-mic", __func__);
            snd_device = SND_DEVICE_IN_HANDSET_MIC;
        }
    }
exit:
    ALOGV("%s: exit: in_snd_device(%s)", __func__, device_table[snd_device]);
    return snd_device;
}

static int select_devices(struct audio_device *adev)
{
    snd_device_t out_snd_device = SND_DEVICE_NONE;
    snd_device_t in_snd_device = SND_DEVICE_NONE;
    struct audio_usecase *usecase;
    int status = 0;
    int acdb_rx_id, acdb_tx_id;
    bool in_call_device_switch = false;

    ALOGV("%s: enter", __func__);
    out_snd_device = get_output_snd_device(adev);
    in_snd_device  = get_input_snd_device(adev);

    if (out_snd_device == adev->cur_out_snd_device && adev->out_snd_device_active &&
        in_snd_device == adev->cur_in_snd_device && adev->in_snd_device_active) {
        ALOGV("%s: exit: snd_devices (%d and %d) are already active",
              __func__, out_snd_device, in_snd_device);
        return 0;
    }

    ALOGD("%s: out_snd_device(%d: %s) in_snd_device(%d: %s)", __func__,
          out_snd_device, device_table[out_snd_device],
          in_snd_device,  device_table[in_snd_device]);

    /*
     * Limitation: While in call, to do a device switch we need to disable
     * and enable both RX and TX devices though one of them is same as current
     * device.
     */
    if (adev->in_call && adev->csd_client != NULL) {
        in_call_device_switch = true;
        /* This must be called before disabling the mixer controls on APQ side */
        if (adev->csd_disable_device == NULL) {
            ALOGE("%s: dlsym error for csd_client_disable_device", __func__);
        } else {
            status = adev->csd_disable_device();
            if (status < 0) {
                ALOGE("%s: csd_client_disable_device, failed, error %d",
                      __func__, status);
            }
        }
    }

    if ((out_snd_device != adev->cur_out_snd_device || in_call_device_switch)
            && adev->out_snd_device_active) {
        usecase = &adev->usecase_list;
        while (usecase->next != NULL) {
            usecase = usecase->next;
            if (usecase->type == PCM_PLAYBACK || usecase->type == VOICE_CALL) {
                disable_audio_route(adev->audio_route, usecase->id,
                                    adev->cur_out_snd_device);
            }
        }
        audio_route_update_mixer(adev->audio_route);
        /* Disable current rx device */
        disable_snd_device(adev->audio_route, adev->cur_out_snd_device);
        adev->out_snd_device_active = false;
    }

    if ((in_snd_device != adev->cur_in_snd_device || in_call_device_switch)
            && adev->in_snd_device_active) {
        usecase = &adev->usecase_list;
        while (usecase->next != NULL) {
            usecase = usecase->next;
            if (usecase->type == PCM_CAPTURE) {
                disable_audio_route(adev->audio_route, usecase->id,
                                    adev->cur_in_snd_device);
            }
        }
        audio_route_update_mixer(adev->audio_route);
        /* Disable current tx device */
        disable_snd_device(adev->audio_route, adev->cur_in_snd_device);
        adev->in_snd_device_active = false;
    }

    if (out_snd_device != SND_DEVICE_NONE && !adev->out_snd_device_active) {
        /* Enable new rx device */
        status = enable_snd_device(adev, out_snd_device);
        if (status != 0) {
            ALOGE("%s: Failed to set mixer ctls for snd_device(%d)",
                  __func__, out_snd_device);
            return status;
        }
        adev->out_snd_device_active = true;
        adev->cur_out_snd_device = out_snd_device;
    }

    if (in_snd_device != SND_DEVICE_NONE && !adev->in_snd_device_active) {
        /* Enable new tx device */
        status = enable_snd_device(adev, in_snd_device);
        if (status != 0) {
            ALOGE("%s: Failed to set mixer ctls for snd_device(%d)",
                  __func__, out_snd_device);
            return status;
        }
        adev->in_snd_device_active = true;
        adev->cur_in_snd_device = in_snd_device;
    }
    audio_route_update_mixer(adev->audio_route);

    usecase = &adev->usecase_list;
    while (usecase->next != NULL) {
        usecase = usecase->next;
        if (usecase->type == PCM_PLAYBACK || usecase->type == VOICE_CALL) {
            usecase->devices = adev->out_device; /* TODO: fix device logic */
            status = enable_audio_route(adev->audio_route, usecase->id,
                                        adev->cur_out_snd_device);
        } else {
            status = enable_audio_route(adev->audio_route, usecase->id,
                                        adev->cur_in_snd_device);
        }
    }
    audio_route_update_mixer(adev->audio_route);

    if (adev->mode == AUDIO_MODE_IN_CALL && adev->csd_client) {
        if (adev->csd_enable_device == NULL) {
            ALOGE("%s: dlsym error for csd_client_enable_device",
                  __func__);
        } else {
            acdb_rx_id = get_acdb_device_id(out_snd_device);
            acdb_tx_id = get_acdb_device_id(in_snd_device);

            status = adev->csd_enable_device(acdb_rx_id, acdb_tx_id,
                                             adev->acdb_settings);
            if (status < 0) {
                ALOGE("%s: csd_client_enable_device, failed, error %d",
                      __func__, status);
            }
        }
    }

    ALOGV("%s: exit: status(%d)", __func__, status);
    return status;
}

static void add_usecase_to_list(struct audio_device *adev,
                                struct audio_usecase *uc_info)
{
    struct audio_usecase *first_entry = adev->usecase_list.next;
    ALOGV("%s: enter: usecase(%d)", __func__, uc_info->id);
    /* Insert the new entry on the top of the list */
    adev->usecase_list.next = uc_info;
    uc_info->next = first_entry;
    ALOGV("%s: exit", __func__);
}

static void remove_usecase_from_list(struct audio_device *adev,
                                     audio_usecase_t uc_id)
{
    struct audio_usecase *uc_to_remove = NULL;
    struct audio_usecase *list_head = &adev->usecase_list;
    ALOGV("%s: enter: usecase(%d)", __func__, uc_id);
    while (list_head->next != NULL) {
        if (list_head->next->id == uc_id) {
            uc_to_remove = list_head->next;
            list_head->next = list_head->next->next;
            free(uc_to_remove);
            break;
        }
        list_head = list_head->next;
    }
    ALOGV("%s: exit", __func__);
}

static struct audio_usecase *get_usecase_from_list(struct audio_device *adev,
                                                   audio_usecase_t uc_id)
{
    struct audio_usecase *uc_info = NULL;
    struct audio_usecase *list_head = &adev->usecase_list;
    ALOGV("%s: enter: uc_id(%d)", __func__, uc_id);
    while (list_head->next != NULL) {
        list_head = list_head->next;
        if (list_head->id == uc_id) {
            uc_info = list_head;
            break;
        }
    }
    ALOGV("%s: exit: uc_info(%p)", __func__, uc_info);
    return uc_info;
}

static int get_num_active_usecases(struct audio_device *adev)
{
    int num_uc = 0;
    struct audio_usecase *list_head = &adev->usecase_list;
    while (list_head->next != NULL) {
        num_uc++;
        list_head = list_head->next;
    }
    return num_uc;
}

static audio_devices_t get_active_out_devices(struct audio_device *adev,
                                              audio_usecase_t usecase)
{
    audio_devices_t devices = 0;
    struct audio_usecase *list_head = &adev->usecase_list;
    /* Return the output devices of usecases other than given usecase */
    while (list_head->next != NULL) {
        list_head = list_head->next;
        if (list_head->type == PCM_PLAYBACK && list_head->id != usecase) {
            devices |= list_head->devices;
        }
    }
    return devices;
}

static audio_devices_t get_voice_call_out_device(struct audio_device *adev)
{
    audio_devices_t devices = 0;
    struct audio_usecase *list_head = &adev->usecase_list;
    /* Return the output devices of usecases other than VOICE_CALL usecase */
    while (list_head->next != NULL) {
        list_head = list_head->next;
        if (list_head->id == USECASE_VOICE_CALL) {
            devices = list_head->devices;
            break;
        }
    }
    return devices;
}

static int stop_input_stream(struct stream_in *in)
{
    int i, ret = 0;
    snd_device_t in_snd_device;
    struct audio_usecase *uc_info;
    struct audio_device *adev = in->dev;

    adev->active_input = NULL;

    ALOGD("%s: enter: usecase(%d)", __func__, in->usecase);
    uc_info = get_usecase_from_list(adev, in->usecase);
    if (uc_info == NULL) {
        ALOGE("%s: Could not find the usecase (%d) in the list",
              __func__, in->usecase);
        return -EINVAL;
    }

    /* 1. Disable stream specific mixer controls */
    in_snd_device = adev->cur_in_snd_device;
    disable_audio_route(adev->audio_route, in->usecase, in_snd_device);
    audio_route_update_mixer(adev->audio_route);

    remove_usecase_from_list(adev, in->usecase);

    /* 2. Disable the tx device */
    select_devices(adev);

    ALOGD("%s: exit: status(%d)", __func__, ret);
    return ret;
}

int start_input_stream(struct stream_in *in)
{
    /* 1. Enable output device and stream routing controls */
    int ret = 0;
    snd_device_t in_snd_device;
    struct audio_usecase *uc_info;
    struct audio_device *adev = in->dev;

    ALOGD("%s: enter: usecase(%d)", __func__, in->usecase);
    adev->active_input = in;
    in_snd_device = get_input_snd_device(adev);
    if (in_snd_device == SND_DEVICE_NONE) {
        ALOGE("%s: Could not get valid input sound device", __func__);
        ret = -EINVAL;
        goto error_config;
    }

    in->pcm_device_id = get_pcm_device_id(adev->audio_route,
                                          in->usecase,
                                          PCM_CAPTURE);
    if (in->pcm_device_id < 0) {
        ALOGE("%s: Could not find PCM device id for the usecase(%d)",
              __func__, in->usecase);
        ret = -EINVAL;
        goto error_config;
    }
    uc_info = (struct audio_usecase *)calloc(1, sizeof(struct audio_usecase));
    uc_info->id = in->usecase;
    uc_info->type = PCM_CAPTURE;
    uc_info->devices = in->device;

    /* 1. Enable the TX device */
    ret = select_devices(adev);
    if (ret) {
        ALOGE("%s: Failed to enable device(%#x)",
              __func__, in->device);
        free(uc_info);
        goto error_config;
    }
    in_snd_device = adev->cur_in_snd_device;

    /* 2. Enable the mixer controls for the audio route */
    enable_audio_route(adev->audio_route, in->usecase, in_snd_device);
    audio_route_update_mixer(adev->audio_route);

    /* 3. Add the usecase info to usecase list */
    add_usecase_to_list(adev, uc_info);

    /* 2. Open the pcm device */
    ALOGV("%s: Opening PCM device card_id(%d) device_id(%d), channels %d",
          __func__, SOUND_CARD, in->pcm_device_id, in->config.channels);
    in->pcm = pcm_open(SOUND_CARD, in->pcm_device_id,
                           PCM_IN, &in->config);
    if (in->pcm && !pcm_is_ready(in->pcm)) {
        ALOGE("%s: %s", __func__, pcm_get_error(in->pcm));
        pcm_close(in->pcm);
        in->pcm = NULL;
        ret = -EIO;
        goto error_open;
    }
    ALOGD("%s: exit", __func__);
    return ret;

error_open:
    stop_input_stream(in);

error_config:
    adev->active_input = NULL;
    ALOGV("%s: exit: status(%d)", __func__, ret);

    return ret;
}

static int stop_output_stream(struct stream_out *out)
{
    int i, ret = 0;
    snd_device_t out_snd_device;
    struct audio_usecase *uc_info;
    struct audio_device *adev = out->dev;

    ALOGD("%s: enter: usecase(%d)", __func__, out->usecase);
    uc_info = get_usecase_from_list(adev, out->usecase);
    if (uc_info == NULL) {
        ALOGE("%s: Could not find the usecase (%d) in the list",
              __func__, out->usecase);
        return -EINVAL;
    }

    /* 1. Get and set stream specific mixer controls */
    out_snd_device = adev->cur_out_snd_device;
    disable_audio_route(adev->audio_route, out->usecase, out_snd_device);
    audio_route_update_mixer(adev->audio_route);

    remove_usecase_from_list(adev, uc_info->id);

    /* 2. Disable the rx device */
    adev->out_device = get_active_out_devices(adev, out->usecase);
    adev->out_device |= get_voice_call_out_device(adev);
    ret = select_devices(adev);

    ALOGD("%s: exit: status(%d) adev->out_device(%#x)",
          __func__, ret, adev->out_device);
    return ret;
}

int start_output_stream(struct stream_out *out)
{
    int ret = 0;
    snd_device_t out_snd_device;
    struct audio_usecase *uc_info;
    struct audio_device *adev = out->dev;

    /* 1. Enable output device and stream routing controls */
    ALOGD("%s: enter: usecase(%d) devices(%#x)",
          __func__, out->usecase, out->devices);
    adev->out_device |= out->devices;
    out_snd_device = get_output_snd_device(adev);
    if (out_snd_device == SND_DEVICE_NONE) {
        ALOGE("%s: Could not get valid output sound device", __func__);
        ret = -EINVAL;
        goto error_config;
    }

    out->pcm_device_id = get_pcm_device_id(adev->audio_route,
                                           out->usecase,
                                           PCM_PLAYBACK);
    if (out->pcm_device_id < 0) {
        ALOGE("%s: Invalid PCM device id(%d) for the usecase(%d)",
              __func__, out->pcm_device_id, out->usecase);
        ret = -EINVAL;
        goto error_config;
    }

    uc_info = (struct audio_usecase *)calloc(1, sizeof(struct audio_usecase));
    uc_info->id = out->usecase;
    uc_info->type = PCM_PLAYBACK;
    uc_info->devices = out->devices;

    ret = select_devices(adev);
    if (ret) {
        ALOGE("%s: Failed to enable device(%#x)",
              __func__, adev->out_device);
        free(uc_info);
        goto error_config;
    }

    out_snd_device = adev->cur_out_snd_device;
    enable_audio_route(adev->audio_route, out->usecase, out_snd_device);
    audio_route_update_mixer(adev->audio_route);

    add_usecase_to_list(adev, uc_info);

    ALOGV("%s: Opening PCM device card_id(%d) device_id(%d)",
          __func__, 0, out->pcm_device_id);
    out->pcm = pcm_open(SOUND_CARD, out->pcm_device_id,
                           PCM_OUT, &out->config);
    if (out->pcm && !pcm_is_ready(out->pcm)) {
        ALOGE("%s: %s", __func__, pcm_get_error(out->pcm));
        pcm_close(out->pcm);
        out->pcm = NULL;
        ret = -EIO;
        goto error;
    }
    ALOGD("%s: exit", __func__);
    return 0;
error:
    stop_output_stream(out);
error_config:
    adev->out_device = get_active_out_devices(adev, out->usecase);
    adev->out_device |= get_voice_call_out_device(adev);
    return ret;
}

static int stop_voice_call(struct audio_device *adev)
{
    int i, ret = 0;
    snd_device_t out_snd_device;
    struct audio_usecase *uc_info;

    ALOGD("%s: enter", __func__);
    adev->in_call = false;
    if (adev->csd_client) {
        if (adev->csd_stop_voice == NULL) {
            ALOGE("dlsym error for csd_client_disable_device");
        } else {
            ret = adev->csd_stop_voice();
            if (ret < 0) {
                ALOGE("%s: csd_client error %d\n", __func__, ret);
            }
        }
    }

    /* 1. Close the PCM devices */
    if (adev->voice_call_rx) {
        pcm_close(adev->voice_call_rx);
        adev->voice_call_rx = NULL;
    }
    if (adev->voice_call_tx) {
        pcm_close(adev->voice_call_tx);
        adev->voice_call_tx = NULL;
    }

    uc_info = get_usecase_from_list(adev, USECASE_VOICE_CALL);
    if (uc_info == NULL) {
        ALOGE("%s: Could not find the usecase (%d) in the list",
              __func__, USECASE_VOICE_CALL);
        return -EINVAL;
    }
    out_snd_device = adev->cur_out_snd_device;

    /* 2. Get and set stream specific mixer controls */
    disable_audio_route(adev->audio_route, USECASE_VOICE_CALL, out_snd_device);
    audio_route_update_mixer(adev->audio_route);

    remove_usecase_from_list(adev, uc_info->id);

    /* 3. Disable the rx and tx devices */
    ret = select_devices(adev);

    ALOGD("%s: exit: status(%d)", __func__, ret);
    return ret;
}

static int start_voice_call(struct audio_device *adev)
{
    int i, ret = 0;
    snd_device_t out_snd_device;
    struct audio_usecase *uc_info;
    int pcm_dev_rx_id, pcm_dev_tx_id;

    ALOGD("%s: enter", __func__);

    uc_info = (struct audio_usecase *)calloc(1, sizeof(struct audio_usecase));
    uc_info->id = USECASE_VOICE_CALL;
    uc_info->type = VOICE_CALL;
    uc_info->devices = adev->out_device;

    ret = select_devices(adev);
    if (ret) {
        free(uc_info);
        return ret;
    }

    out_snd_device = adev->cur_out_snd_device;
    enable_audio_route(adev->audio_route, uc_info->id, out_snd_device);
    audio_route_update_mixer(adev->audio_route);

    add_usecase_to_list(adev, uc_info);

    pcm_dev_rx_id = get_pcm_device_id(adev->audio_route, uc_info->id,
                                      PCM_PLAYBACK);
    pcm_dev_tx_id = get_pcm_device_id(adev->audio_route, uc_info->id,
                                      PCM_CAPTURE);

    if (pcm_dev_rx_id < 0 || pcm_dev_tx_id < 0) {
        ALOGE("%s: Invalid PCM devices (rx: %d tx: %d) for the usecase(%d)",
              __func__, pcm_dev_rx_id, pcm_dev_tx_id, uc_info->id);
        ret = -EIO;
        goto error_start_voice;
    }

    ALOGV("%s: Opening PCM playback device card_id(%d) device_id(%d)",
          __func__, SOUND_CARD, pcm_dev_rx_id);
    adev->voice_call_rx = pcm_open(SOUND_CARD,
                                  pcm_dev_rx_id,
                                  PCM_OUT, &pcm_config_voice_call);
    if (adev->voice_call_rx && !pcm_is_ready(adev->voice_call_rx)) {
        ALOGE("%s: %s", __func__, pcm_get_error(adev->voice_call_rx));
        ret = -EIO;
        goto error_start_voice;
    }

    ALOGV("%s: Opening PCM capture device card_id(%d) device_id(%d)",
          __func__, SOUND_CARD, pcm_dev_tx_id);
    adev->voice_call_tx = pcm_open(SOUND_CARD,
                                   pcm_dev_tx_id,
                                   PCM_IN, &pcm_config_voice_call);
    if (adev->voice_call_tx && !pcm_is_ready(adev->voice_call_tx)) {
        ALOGE("%s: %s", __func__, pcm_get_error(adev->voice_call_tx));
        ret = -EIO;
        goto error_start_voice;
    }
    pcm_start(adev->voice_call_rx);
    pcm_start(adev->voice_call_tx);

    if (adev->csd_client) {
        if (adev->csd_start_voice == NULL) {
            ALOGE("dlsym error for csd_client_start_voice");
            goto error_start_voice;
        } else {
            ret = adev->csd_start_voice();
            if (ret < 0) {
                ALOGE("%s: csd_start_voice error %d\n", __func__, ret);
                goto error_start_voice;
            }
        }
    }

    adev->in_call = true;
    return 0;

error_start_voice:
    stop_voice_call(adev);

    ALOGD("%s: exit: status(%d)", __func__, ret);
    return ret;
}

static int check_input_parameters(uint32_t sample_rate,
                                  audio_format_t format,
                                  int channel_count)
{
    if (format != AUDIO_FORMAT_PCM_16_BIT) return -EINVAL;

    if ((channel_count < 1) || (channel_count > 2)) return -EINVAL;

    switch (sample_rate) {
    case 8000:
    case 11025:
    case 12000:
    case 16000:
    case 22050:
    case 24000:
    case 32000:
    case 44100:
    case 48000:
        break;
    default:
        return -EINVAL;
    }

    return 0;
}

static size_t get_input_buffer_size(uint32_t sample_rate,
                                    audio_format_t format,
                                    int channel_count)
{
    size_t size = 0;

    if (check_input_parameters(sample_rate, format, channel_count) != 0) return 0;

    if (sample_rate == 8000 || sample_rate == 16000 || sample_rate == 32000) {
        size = (sample_rate * 20) / 1000;
    } else if (sample_rate == 11025 || sample_rate == 12000) {
        size = 256;
    } else if (sample_rate == 22050 || sample_rate == 24000) {
        size = 512;
    } else if (sample_rate == 44100 || sample_rate == 48000) {
        size = 1024;
    }

    return size * sizeof(short) * channel_count;
}

static uint32_t out_get_sample_rate(const struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    return out->config.rate;
}

static int out_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    return -ENOSYS;
}

static size_t out_get_buffer_size(const struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    return out->config.period_size * audio_stream_frame_size(stream);
}

static uint32_t out_get_channels(const struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    return out->channel_mask;
}

static audio_format_t out_get_format(const struct audio_stream *stream)
{
    return AUDIO_FORMAT_PCM_16_BIT;
}

static int out_set_format(struct audio_stream *stream, audio_format_t format)
{
    return -ENOSYS;
}

static int out_standby(struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;
    ALOGD("%s: enter: usecase(%d)", __func__, out->usecase);
    pthread_mutex_lock(&out->lock);

    if (!out->standby) {
        out->standby = true;
        if (out->pcm) {
            pcm_close(out->pcm);
            out->pcm = NULL;
        }
        pthread_mutex_lock(&adev->lock);
        stop_output_stream(out);
        pthread_mutex_unlock(&adev->lock);
    }
    pthread_mutex_unlock(&out->lock);
    ALOGD("%s: exit", __func__);
    return 0;
}

static int out_dump(const struct audio_stream *stream, int fd)
{
    return 0;
}

static int out_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;
    struct str_parms *parms;
    char value[32];
    int ret, val = 0;

    ALOGD("%s: enter: usecase(%d) kvpairs: %s",
          __func__, out->usecase, kvpairs);
    parms = str_parms_create_str(kvpairs);
    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING, value, sizeof(value));
    if (ret >= 0) {
        val = atoi(value);
        pthread_mutex_lock(&out->lock);
        pthread_mutex_lock(&adev->lock);

        if (adev->mode == AUDIO_MODE_IN_CALL && !adev->in_call && (val != 0)) {
            adev->out_device = get_active_out_devices(adev, out->usecase) | val;
            out->devices = val;
            start_voice_call(adev);
        } else if (adev->mode != AUDIO_MODE_IN_CALL && adev->in_call) {
            if (val != 0) {
                adev->out_device = get_active_out_devices(adev, out->usecase) | val;
                out->devices = val;
            }
            stop_voice_call(adev);
        } else if ((out->devices != (audio_devices_t)val) && (val != 0)) {
            if (!out->standby || adev->in_call) {
                adev->out_device = get_active_out_devices(adev, out->usecase) | val;
                ret = select_devices(adev);
            }
            out->devices = val;
        }

        pthread_mutex_unlock(&adev->lock);
        pthread_mutex_unlock(&out->lock);
    }
    str_parms_destroy(parms);
    ALOGD("%s: exit: code(%d)", __func__, ret);
    return ret;
}

static char* out_get_parameters(const struct audio_stream *stream, const char *keys)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct str_parms *query = str_parms_create_str(keys);
    char *str;
    char value[256];
    struct str_parms *reply = str_parms_create();
    size_t i, j;
    int ret;
    bool first = true;
    ALOGD("%s: enter: keys - %s", __func__, keys);
    ret = str_parms_get_str(query, AUDIO_PARAMETER_STREAM_SUP_CHANNELS, value, sizeof(value));
    if (ret >= 0) {
        value[0] = '\0';
        i = 0;
        while (out->supported_channel_masks[i] != 0) {
            for (j = 0; j < ARRAY_SIZE(out_channels_name_to_enum_table); j++) {
                if (out_channels_name_to_enum_table[j].value == out->supported_channel_masks[i]) {
                    if (!first) {
                        strcat(value, "|");
                    }
                    strcat(value, out_channels_name_to_enum_table[j].name);
                    first = false;
                    break;
                }
            }
            i++;
        }
        str_parms_add_str(reply, AUDIO_PARAMETER_STREAM_SUP_CHANNELS, value);
        str = str_parms_to_str(reply);
    } else {
        str = strdup(keys);
    }
    str_parms_destroy(query);
    str_parms_destroy(reply);
    ALOGD("%s: exit: returns - %s", __func__, str);
    return str;
}

static uint32_t out_get_latency(const struct audio_stream_out *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    return (out->config.period_count * out->config.period_size * 1000) / (out->config.rate);
}

static int out_set_volume(struct audio_stream_out *stream, float left,
                          float right)
{
    return -ENOSYS;
}

static ssize_t out_write(struct audio_stream_out *stream, const void *buffer,
                         size_t bytes)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;
    int i, ret = -1;

    pthread_mutex_lock(&out->lock);
    if (out->standby) {
        pthread_mutex_lock(&adev->lock);
        ret = start_output_stream(out);
        pthread_mutex_unlock(&adev->lock);
        if (ret != 0) {
            goto exit;
        }
        out->standby = false;
    }

    if (out->pcm) {
        //ALOGV("%s: writing buffer (%d bytes) to pcm device", __func__, bytes);
        ret = pcm_write(out->pcm, (void *)buffer, bytes);
    }

exit:
    pthread_mutex_unlock(&out->lock);

    if (ret != 0) {
        out_standby(&out->stream.common);
        usleep(bytes * 1000000 / audio_stream_frame_size(&out->stream.common) /
               out_get_sample_rate(&out->stream.common));
    }
    return bytes;
}

static int out_get_render_position(const struct audio_stream_out *stream,
                                   uint32_t *dsp_frames)
{
    return -EINVAL;
}

static int out_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int out_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int out_get_next_write_timestamp(const struct audio_stream_out *stream,
                                        int64_t *timestamp)
{
    return -EINVAL;
}

/** audio_stream_in implementation **/
static uint32_t in_get_sample_rate(const struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    return in->config.rate;
}

static int in_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    return -ENOSYS;
}

static size_t in_get_buffer_size(const struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    return in->config.period_size * audio_stream_frame_size(stream);
}

static uint32_t in_get_channels(const struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    return in->channel_mask;
}

static audio_format_t in_get_format(const struct audio_stream *stream)
{
    return AUDIO_FORMAT_PCM_16_BIT;
}

static int in_set_format(struct audio_stream *stream, audio_format_t format)
{
    return -ENOSYS;
}

static int in_standby(struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;
    struct audio_device *adev = in->dev;
    int status = 0;
    ALOGD("%s: enter", __func__);
    pthread_mutex_lock(&in->lock);
    if (!in->standby) {
        in->standby = true;
        if (in->pcm) {
            pcm_close(in->pcm);
            in->pcm = NULL;
        }
        pthread_mutex_lock(&adev->lock);
        status = stop_input_stream(in);
        pthread_mutex_unlock(&adev->lock);
    }
    pthread_mutex_unlock(&in->lock);
    ALOGD("%s: exit:  status(%d)", __func__, status);
    return status;
}

static int in_dump(const struct audio_stream *stream, int fd)
{
    return 0;
}

static int in_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct stream_in *in = (struct stream_in *)stream;
    struct audio_device *adev = in->dev;
    struct str_parms *parms;
    char *str;
    char value[32];
    int ret, val = 0;

    ALOGD("%s: enter: kvpairs=%s", __func__, kvpairs);
    parms = str_parms_create_str(kvpairs);

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_INPUT_SOURCE, value, sizeof(value));

    pthread_mutex_lock(&in->lock);
    pthread_mutex_lock(&adev->lock);
    if (ret >= 0) {
        val = atoi(value);
        /* no audio source uses val == 0 */
        if ((in->source != val) && (val != 0)) {
            in->source = val;
        }
    }

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING, value, sizeof(value));
    if (ret >= 0) {
        val = atoi(value);
        if ((in->device != val) && (val != 0)) {
            in->device = val;
            /* If recording is in progress, change the tx device to new device */
            if (!in->standby) {
                ret = select_devices(adev);
            }
        }
    }

    pthread_mutex_unlock(&adev->lock);
    pthread_mutex_unlock(&in->lock);

    str_parms_destroy(parms);
    ALOGD("%s: exit: status(%d)", __func__, ret);
    return ret;
}

static char* in_get_parameters(const struct audio_stream *stream,
                               const char *keys)
{
    return strdup("");
}

static int in_set_gain(struct audio_stream_in *stream, float gain)
{
    return 0;
}

static ssize_t in_read(struct audio_stream_in *stream, void *buffer,
                       size_t bytes)
{
    struct stream_in *in = (struct stream_in *)stream;
    struct audio_device *adev = in->dev;
    int i, ret = -1;

    //ALOGV("%s: buffer(%p) bytes(%d)", __func__, buffer, bytes);
    pthread_mutex_lock(&in->lock);
    if (in->standby) {
      pthread_mutex_lock(&adev->lock);
        ret = start_input_stream(in);
        pthread_mutex_unlock(&adev->lock);
        if (ret != 0) {
            goto exit;
        }
        in->standby = 0;
    }

    if (in->pcm) {
        ret = pcm_read(in->pcm, buffer, bytes);
    }

    /*
     * Instead of writing zeroes here, we could trust the hardware
     * to always provide zeroes when muted.
     */
    if (ret == 0 && adev->mic_mute)
        memset(buffer, 0, bytes);

exit:
    pthread_mutex_unlock(&in->lock);

    if (ret != 0) {
        in_standby(&in->stream.common);
        ALOGV("%s: read failed - sleeping for buffer duration", __func__);
        usleep(bytes * 1000000 / audio_stream_frame_size(&in->stream.common) /
               in_get_sample_rate(&in->stream.common));
    }
    return bytes;
}

static uint32_t in_get_input_frames_lost(struct audio_stream_in *stream)
{
    return 0;
}

static int in_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int in_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int adev_open_output_stream(struct audio_hw_device *dev,
                                   audio_io_handle_t handle,
                                   audio_devices_t devices,
                                   audio_output_flags_t flags,
                                   struct audio_config *config,
                                   struct audio_stream_out **stream_out)
{
    struct audio_device *adev = (struct audio_device *)dev;
    struct stream_out *out;
    int i, ret;

    ALOGD("%s: enter: sample_rate(%d) channel_mask(%#x) devices(%#x) flags(%#x)",
          __func__, config->sample_rate, config->channel_mask, devices, flags);
    *stream_out = NULL;
    out = (struct stream_out *)calloc(1, sizeof(struct stream_out));

    if (devices == AUDIO_DEVICE_NONE)
        devices = AUDIO_DEVICE_OUT_SPEAKER;

    out->supported_channel_masks[0] = AUDIO_CHANNEL_OUT_STEREO;
    out->channel_mask = AUDIO_CHANNEL_OUT_STEREO;
    out->flags = flags;
    out->devices = devices;

    /* Init use case and pcm_config */
    if (out->flags & AUDIO_OUTPUT_FLAG_DIRECT &&
        out->devices & AUDIO_DEVICE_OUT_AUX_DIGITAL) {
        out->usecase = USECASE_AUDIO_PLAYBACK_MULTI_CH;
        out->config = pcm_config_hdmi_multi;

        pthread_mutex_lock(&adev->lock);
        read_hdmi_channel_masks(out);
        pthread_mutex_unlock(&adev->lock);

        if (config->sample_rate == 0) config->sample_rate = DEFAULT_OUTPUT_SAMPLING_RATE;
        if (config->channel_mask == 0) config->channel_mask = AUDIO_CHANNEL_OUT_5POINT1;
        out->channel_mask = config->channel_mask;
        out->config.rate = config->sample_rate;
        out->config.channels = popcount(out->channel_mask);
        out->config.period_size = HDMI_MULTI_PERIOD_BYTES / (out->config.channels * 2);
        set_hdmi_channels(adev->mixer, out->config.channels);
    } else if (out->flags & AUDIO_OUTPUT_FLAG_DEEP_BUFFER) {
        out->usecase = USECASE_AUDIO_PLAYBACK_DEEP_BUFFER;
        out->config = pcm_config_deep_buffer;
    } else {
        out->usecase = USECASE_AUDIO_PLAYBACK_LOW_LATENCY;
        out->config = pcm_config_low_latency;
    }

    /* Check if this usecase is already existing */
    pthread_mutex_lock(&adev->lock);
    if (get_usecase_from_list(adev, out->usecase) != NULL) {
        ALOGE("%s: Usecase (%d) is already present", __func__, out->usecase);
        free(out);
        *stream_out = NULL;
        pthread_mutex_unlock(&adev->lock);
        return -EEXIST;
    }
    pthread_mutex_unlock(&adev->lock);

    out->stream.common.get_sample_rate = out_get_sample_rate;
    out->stream.common.set_sample_rate = out_set_sample_rate;
    out->stream.common.get_buffer_size = out_get_buffer_size;
    out->stream.common.get_channels = out_get_channels;
    out->stream.common.get_format = out_get_format;
    out->stream.common.set_format = out_set_format;
    out->stream.common.standby = out_standby;
    out->stream.common.dump = out_dump;
    out->stream.common.set_parameters = out_set_parameters;
    out->stream.common.get_parameters = out_get_parameters;
    out->stream.common.add_audio_effect = out_add_audio_effect;
    out->stream.common.remove_audio_effect = out_remove_audio_effect;
    out->stream.get_latency = out_get_latency;
    out->stream.set_volume = out_set_volume;
    out->stream.write = out_write;
    out->stream.get_render_position = out_get_render_position;
    out->stream.get_next_write_timestamp = out_get_next_write_timestamp;

    out->dev = adev;
    out->standby = 1;

    config->format = out->stream.common.get_format(&out->stream.common);
    config->channel_mask = out->stream.common.get_channels(&out->stream.common);
    config->sample_rate = out->stream.common.get_sample_rate(&out->stream.common);

    *stream_out = &out->stream;
    ALOGD("%s: exit", __func__);
    return 0;
}

static void adev_close_output_stream(struct audio_hw_device *dev,
                                     struct audio_stream_out *stream)
{
    ALOGD("%s: enter", __func__);
    out_standby(&stream->common);
    free(stream);
    ALOGD("%s: exit", __func__);
}

static int adev_set_parameters(struct audio_hw_device *dev, const char *kvpairs)
{
    struct audio_device *adev = (struct audio_device *)dev;
    struct str_parms *parms;
    char *str;
    char value[32];
    int ret;

    ALOGD("%s: enter: %s", __func__, kvpairs);

    parms = str_parms_create_str(kvpairs);
    ret = str_parms_get_str(parms, AUDIO_PARAMETER_KEY_TTY_MODE, value, sizeof(value));
    if (ret >= 0) {
        int tty_mode;

        if (strcmp(value, AUDIO_PARAMETER_VALUE_TTY_OFF) == 0)
            tty_mode = TTY_MODE_OFF;
        else if (strcmp(value, AUDIO_PARAMETER_VALUE_TTY_VCO) == 0)
            tty_mode = TTY_MODE_VCO;
        else if (strcmp(value, AUDIO_PARAMETER_VALUE_TTY_HCO) == 0)
            tty_mode = TTY_MODE_HCO;
        else if (strcmp(value, AUDIO_PARAMETER_VALUE_TTY_FULL) == 0)
            tty_mode = TTY_MODE_FULL;
        else
            return -EINVAL;

        pthread_mutex_lock(&adev->lock);
        if (tty_mode != adev->tty_mode) {
            adev->tty_mode = tty_mode;
            adev->acdb_settings = (adev->acdb_settings & TTY_MODE_CLEAR) | tty_mode;
            if (adev->in_call)
                select_devices(adev);
        }
        pthread_mutex_unlock(&adev->lock);
    }

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_KEY_BT_NREC, value, sizeof(value));
    if (ret >= 0) {
        /* When set to false, HAL should disable EC and NS
         * But it is currently not supported.
         */
        if (strcmp(value, AUDIO_PARAMETER_VALUE_ON) == 0)
            adev->bluetooth_nrec = true;
        else
            adev->bluetooth_nrec = false;
    }

    ret = str_parms_get_str(parms, "screen_state", value, sizeof(value));
    if (ret >= 0) {
        if (strcmp(value, AUDIO_PARAMETER_VALUE_ON) == 0)
            adev->screen_off = false;
        else
            adev->screen_off = true;
    }

    str_parms_destroy(parms);
    ALOGD("%s: exit with code(%d)", __func__, ret);
    return ret;
}

static char* adev_get_parameters(const struct audio_hw_device *dev,
                                 const char *keys)
{
    return strdup("");
}

static int adev_init_check(const struct audio_hw_device *dev)
{
    return 0;
}

static int adev_set_voice_volume(struct audio_hw_device *dev, float volume)
{
    struct audio_device *adev = (struct audio_device *)dev;
    int vol, err = 0;

    pthread_mutex_lock(&adev->lock);
    adev->voice_volume = volume;
    if (adev->mode == AUDIO_MODE_IN_CALL) {
        if (volume < 0.0) {
            volume = 0.0;
        } else if (volume > 1.0) {
            volume = 1.0;
        }

        vol = lrint(volume * 100.0);

        // Voice volume levels from android are mapped to driver volume levels as follows.
        // 0 -> 5, 20 -> 4, 40 ->3, 60 -> 2, 80 -> 1, 100 -> 0
        // So adjust the volume to get the correct volume index in driver
        vol = 100 - vol;

        if (adev->csd_client) {
            if (adev->csd_volume == NULL) {
                ALOGE("%s: dlsym error for csd_client_volume", __func__);
            } else {
                err = adev->csd_volume(vol);
                if (err < 0) {
                    ALOGE("%s: csd_client error %d", __func__, err);
                }
            }
        } else {
            ALOGE("%s: No CSD Client present", __func__);
        }
    }
    pthread_mutex_unlock(&adev->lock);
    return err;
}

static int adev_set_master_volume(struct audio_hw_device *dev, float volume)
{
    return -ENOSYS;
}

static int adev_get_master_volume(struct audio_hw_device *dev,
                                  float *volume)
{
    return -ENOSYS;
}

static int adev_set_master_mute(struct audio_hw_device *dev, bool muted)
{
    return -ENOSYS;
}

static int adev_get_master_mute(struct audio_hw_device *dev, bool *muted)
{
    return -ENOSYS;
}

static int adev_set_mode(struct audio_hw_device *dev, audio_mode_t mode)
{
    struct audio_device *adev = (struct audio_device *)dev;

    pthread_mutex_lock(&adev->lock);
    if (adev->mode != mode) {
        adev->mode = mode;
    }
    pthread_mutex_unlock(&adev->lock);
    return 0;
}

static int adev_set_mic_mute(struct audio_hw_device *dev, bool state)
{
    struct audio_device *adev = (struct audio_device *)dev;
    int err = 0;

    adev->mic_mute = state;
    if (adev->mode == AUDIO_MODE_IN_CALL) {
        if (adev->csd_client) {
            if (adev->csd_mic_mute == NULL) {
                ALOGE("%s: dlsym error for csd_mic_mute", __func__);
            } else {
                err = adev->csd_mic_mute(state);
                if (err < 0) {
                    ALOGE("%s: csd_client error %d", __func__, err);
                }
            }
        } else {
            ALOGE("%s: No CSD Client present", __func__);
        }
    }
    return err;
}

static int adev_get_mic_mute(const struct audio_hw_device *dev, bool *state)
{
    struct audio_device *adev = (struct audio_device *)dev;

    *state = adev->mic_mute;

    return 0;
}

static size_t adev_get_input_buffer_size(const struct audio_hw_device *dev,
                                         const struct audio_config *config)
{
    int channel_count = popcount(config->channel_mask);

    return get_input_buffer_size(config->sample_rate, config->format, channel_count);
}

static int adev_open_input_stream(struct audio_hw_device *dev,
                                  audio_io_handle_t handle,
                                  audio_devices_t devices,
                                  struct audio_config *config,
                                  struct audio_stream_in **stream_in)
{
    struct audio_device *adev = (struct audio_device *)dev;
    struct stream_in *in;
    int ret, buffer_size, frame_size;
    int channel_count = popcount(config->channel_mask);

    ALOGD("%s: enter", __func__);
    *stream_in = NULL;
    if (check_input_parameters(config->sample_rate, config->format, channel_count) != 0)
        return -EINVAL;

    in = (struct stream_in *)calloc(1, sizeof(struct stream_in));

    in->stream.common.get_sample_rate = in_get_sample_rate;
    in->stream.common.set_sample_rate = in_set_sample_rate;
    in->stream.common.get_buffer_size = in_get_buffer_size;
    in->stream.common.get_channels = in_get_channels;
    in->stream.common.get_format = in_get_format;
    in->stream.common.set_format = in_set_format;
    in->stream.common.standby = in_standby;
    in->stream.common.dump = in_dump;
    in->stream.common.set_parameters = in_set_parameters;
    in->stream.common.get_parameters = in_get_parameters;
    in->stream.common.add_audio_effect = in_add_audio_effect;
    in->stream.common.remove_audio_effect = in_remove_audio_effect;
    in->stream.set_gain = in_set_gain;
    in->stream.read = in_read;
    in->stream.get_input_frames_lost = in_get_input_frames_lost;

    in->device = devices;
    in->source = AUDIO_SOURCE_DEFAULT;
    in->dev = adev;
    in->standby = 1;
    in->channel_mask = config->channel_mask;

    /* Update config params with the requested sample rate and channels */
    in->usecase = USECASE_AUDIO_RECORD;
    in->config = pcm_config_audio_capture;
    in->config.channels = channel_count;
    in->config.rate = config->sample_rate;

    frame_size = audio_stream_frame_size((struct audio_stream *)in);
    buffer_size = get_input_buffer_size(config->sample_rate,
                                        config->format,
                                        channel_count);
    in->config.period_size = buffer_size / frame_size;

    *stream_in = &in->stream;
    ALOGD("%s: exit", __func__);
    return 0;

err_open:
    free(in);
    *stream_in = NULL;
    return ret;
}

static void adev_close_input_stream(struct audio_hw_device *dev,
                                    struct audio_stream_in *stream)
{
    ALOGD("%s", __func__);

    in_standby(&stream->common);
    free(stream);

    return;
}

static int adev_dump(const audio_hw_device_t *device, int fd)
{
    return 0;
}

static int adev_close(hw_device_t *device)
{
    struct audio_device *adev = (struct audio_device *)device;
    audio_route_free(adev->audio_route);
    free(device);
    return 0;
}

static void init_platform_data(struct audio_device *adev)
{
    char platform[PROPERTY_VALUE_MAX];
    char baseband[PROPERTY_VALUE_MAX];
    char value[PROPERTY_VALUE_MAX];

    adev->dualmic_config = DUALMIC_CONFIG_NONE;
    adev->fluence_in_voice_call = false;
    adev->fluence_in_voice_rec = false;
    adev->mic_type_analog = false;

    property_get("persist.audio.handset.mic.type",value,"");
    if (!strncmp("analog", value, 6))
        adev->mic_type_analog = true;

    property_get("persist.audio.dualmic.config",value,"");
    if (!strncmp("broadside", value, 9)) {
        adev->dualmic_config = DUALMIC_CONFIG_BROADSIDE;
        adev->acdb_settings |= DMIC_FLAG;
    } else if (!strncmp("endfire", value, 7)) {
        adev->dualmic_config = DUALMIC_CONFIG_ENDFIRE;
        adev->acdb_settings |= DMIC_FLAG;
    }

    if (adev->dualmic_config != DUALMIC_CONFIG_NONE) {
        property_get("persist.audio.fluence.voicecall",value,"");
        if (!strncmp("true", value, 4)) {
            adev->fluence_in_voice_call = true;
        }

        property_get("persist.audio.fluence.voicerec",value,"");
        if (!strncmp("true", value, 4)) {
            adev->fluence_in_voice_rec = true;
        }
    }

    adev->acdb_handle = dlopen(LIB_ACDB_LOADER, RTLD_NOW);
    if (adev->acdb_handle == NULL) {
        ALOGE("%s: DLOPEN failed for %s", __func__, LIB_ACDB_LOADER);
    } else {
        ALOGV("%s: DLOPEN successful for %s", __func__, LIB_ACDB_LOADER);
        adev->acdb_deallocate = (acdb_deallocate_t)dlsym(adev->acdb_handle,
                                                    "acdb_loader_deallocate_ACDB");
        adev->acdb_send_audio_cal = (acdb_send_audio_cal_t)dlsym(adev->acdb_handle,
                                                    "acdb_loader_send_audio_cal");
        adev->acdb_send_voice_cal = (acdb_send_voice_cal_t)dlsym(adev->acdb_handle,
                                                    "acdb_loader_send_voice_cal");
        adev->acdb_init = (acdb_init_t)dlsym(adev->acdb_handle,
                                                    "acdb_loader_init_ACDB");
        if (adev->acdb_init == NULL)
            ALOGE("%s: dlsym error %s for acdb_loader_init_ACDB", __func__, dlerror());
        else
            adev->acdb_init();
    }

    /* If platform is Fusion3, load CSD Client specific symbols
     * Voice call is handled by MDM and apps processor talks to
     * MDM through CSD Client
     */
    property_get("ro.board.platform", platform, "");
    property_get("ro.baseband", baseband, "");
    if (!strcmp("msm8960", platform) && !strcmp("mdm", baseband)) {
        adev->csd_client = dlopen(LIB_CSD_CLIENT, RTLD_NOW);
        if (adev->csd_client == NULL)
            ALOGE("%s: DLOPEN failed for %s", __func__, LIB_CSD_CLIENT);
    }

    if (adev->csd_client) {
        ALOGV("%s: DLOPEN successful for %s", __func__, LIB_CSD_CLIENT);
        adev->csd_client_deinit = (csd_client_deinit_t)dlsym(adev->csd_client,
                                                    "csd_client_deinit");
        adev->csd_disable_device = (csd_disable_device_t)dlsym(adev->csd_client,
                                                    "csd_client_disable_device");
        adev->csd_enable_device = (csd_enable_device_t)dlsym(adev->csd_client,
                                                    "csd_client_enable_device");
        adev->csd_start_voice = (csd_start_voice_t)dlsym(adev->csd_client,
                                                    "csd_client_start_voice");
        adev->csd_stop_voice = (csd_stop_voice_t)dlsym(adev->csd_client,
                                                    "csd_client_stop_voice");
        adev->csd_volume = (csd_volume_t)dlsym(adev->csd_client,
                                                    "csd_client_volume");
        adev->csd_mic_mute = (csd_mic_mute_t)dlsym(adev->csd_client,
                                                    "csd_client_mic_mute");
        adev->csd_client_init = (csd_client_init_t)dlsym(adev->csd_client,
                                                    "csd_client_init");

        if (adev->csd_client_init == NULL) {
            ALOGE("%s: dlsym error %s for csd_client_init", __func__, dlerror());
        } else {
            adev->csd_client_init();
        }
    }
}

static int adev_open(const hw_module_t *module, const char *name,
                     hw_device_t **device)
{
    struct audio_device *adev;
    int ret;

    ALOGD("%s: enter", __func__);
    if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0) return -EINVAL;

    adev = calloc(1, sizeof(struct audio_device));

    adev->mixer = mixer_open(MIXER_CARD);
    if (!adev->mixer) {
        ALOGE("Unable to open the mixer, aborting.");
        return -ENOSYS;
    }

    adev->audio_route = audio_route_init(MIXER_CARD, MIXER_XML_PATH);
    if (!adev->audio_route) {
        free(adev);
        ALOGE("%s: Failed to init audio route controls, aborting.", __func__);
        *device = NULL;
        return -EINVAL;
    }

    adev->device.common.tag = HARDWARE_DEVICE_TAG;
    adev->device.common.version = AUDIO_DEVICE_API_VERSION_2_0;
    adev->device.common.module = (struct hw_module_t *)module;
    adev->device.common.close = adev_close;

    adev->device.init_check = adev_init_check;
    adev->device.set_voice_volume = adev_set_voice_volume;
    adev->device.set_master_volume = adev_set_master_volume;
    adev->device.get_master_volume = adev_get_master_volume;
    adev->device.set_master_mute = adev_set_master_mute;
    adev->device.get_master_mute = adev_get_master_mute;
    adev->device.set_mode = adev_set_mode;
    adev->device.set_mic_mute = adev_set_mic_mute;
    adev->device.get_mic_mute = adev_get_mic_mute;
    adev->device.set_parameters = adev_set_parameters;
    adev->device.get_parameters = adev_get_parameters;
    adev->device.get_input_buffer_size = adev_get_input_buffer_size;
    adev->device.open_output_stream = adev_open_output_stream;
    adev->device.close_output_stream = adev_close_output_stream;
    adev->device.open_input_stream = adev_open_input_stream;
    adev->device.close_input_stream = adev_close_input_stream;
    adev->device.dump = adev_dump;

    /* Set the default route before the PCM stream is opened */
    pthread_mutex_lock(&adev->lock);
    adev->mode = AUDIO_MODE_NORMAL;
    adev->active_input = NULL;
    adev->out_device = AUDIO_DEVICE_NONE;
    adev->voice_call_rx = NULL;
    adev->voice_call_tx = NULL;
    adev->voice_volume = 1.0f;
    adev->tty_mode = TTY_MODE_OFF;
    adev->bluetooth_nrec = true;
    adev->cur_out_snd_device = 0;
    adev->cur_in_snd_device = 0;
    adev->out_snd_device_active = false;
    adev->in_snd_device_active = false;
    adev->usecase_list.next = NULL;
    adev->usecase_list.id = USECASE_INVALID;
    adev->in_call = false;
    adev->acdb_settings = TTY_MODE_OFF;
    pthread_mutex_unlock(&adev->lock);

    /* Loads platform specific libraries dynamically */
    init_platform_data(adev);

    *device = &adev->device.common;

    ALOGD("%s: exit", __func__);
    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
    .open = adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "QCOM Audio HAL",
        .author = "Code Aurora Forum",
        .methods = &hal_module_methods,
    },
};
