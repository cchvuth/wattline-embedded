// SPDX-License-Identifier: MIT
// aa_proto.h — Android Auto wire-protocol constants, from the open aasdk project (f1xpl/aasdk,
// aasdk_proto/ + Messenger/ headers). Reference only: no vendor code, just the public numbers.
//
// Frame on the wire: [channel:1][flags:1][size], then payload.
//   flags = frameType | encryptionType | messageType
//   size  = SHORT: uint16 BE payload length; EXTENDED (FIRST frame of a split message only):
//           uint16 BE frame length then uint32 BE total length.
//   payload (one logical message) = [uint16 BE message id][protobuf]
//   messages larger than AA_MAX_FRAME_PAYLOAD are split across frames.
#pragma once

#define AA_PORT              5288
#define AA_MAX_FRAME_PAYLOAD 0x4000   /* 16384; larger messages are split */
#define AA_VERSION_MAJOR 1            /* aasdk-compatible protocol version */
#define AA_VERSION_MINOR 1

/* frame flags (byte 1) */
enum { AA_FRAME_MIDDLE = 0, AA_FRAME_FIRST = 1, AA_FRAME_LAST = 2, AA_FRAME_BULK = 3 };
enum { AA_ENC_PLAIN = 0, AA_ENC_ENCRYPTED = 8 };
enum { AA_MSG_SPECIFIC = 0, AA_MSG_CONTROL = 4 };

/* channel ids (Messenger/ChannelId.hpp) */
enum {
	AA_CH_CONTROL = 0, AA_CH_INPUT = 1, AA_CH_SENSOR = 2, AA_CH_VIDEO = 3, AA_CH_MEDIA_AUDIO = 4,
	AA_CH_SPEECH_AUDIO = 5, AA_CH_SYSTEM_AUDIO = 6, AA_CH_AV_INPUT = 7, AA_CH_BLUETOOTH = 8, AA_CH_NONE = 255,
};

/* control-channel message ids (ControlMessageIdsEnum.proto) */
enum {
	AA_CTL_VERSION_REQUEST = 1, AA_CTL_VERSION_RESPONSE = 2, AA_CTL_SSL_HANDSHAKE = 3, AA_CTL_AUTH_COMPLETE = 4,
	AA_CTL_SERVICE_DISCOVERY_REQUEST = 5, AA_CTL_SERVICE_DISCOVERY_RESPONSE = 6,
	AA_CTL_CHANNEL_OPEN_REQUEST = 7, AA_CTL_CHANNEL_OPEN_RESPONSE = 8,
	AA_CTL_PING_REQUEST = 11, AA_CTL_PING_RESPONSE = 12,
	AA_CTL_NAV_FOCUS_REQUEST = 13, AA_CTL_NAV_FOCUS_RESPONSE = 14,
	AA_CTL_SHUTDOWN_REQUEST = 15, AA_CTL_SHUTDOWN_RESPONSE = 16,
	AA_CTL_VOICE_SESSION_REQUEST = 17, AA_CTL_AUDIO_FOCUS_REQUEST = 18, AA_CTL_AUDIO_FOCUS_RESPONSE = 19,
};

/* AV-channel message ids (AVChannelMessageIdsEnum.proto) */
enum {
	AA_AV_MEDIA_WITH_TIMESTAMP = 0x0000, AA_AV_MEDIA = 0x0001,
	AA_AV_SETUP_REQUEST = 0x8000, AA_AV_START = 0x8001, AA_AV_STOP = 0x8002, AA_AV_SETUP_RESPONSE = 0x8003,
	AA_AV_MEDIA_ACK = 0x8004, AA_AV_INPUT_OPEN_REQUEST = 0x8005, AA_AV_INPUT_OPEN_RESPONSE = 0x8006,
	AA_AV_VIDEO_FOCUS_REQUEST = 0x8007, AA_AV_VIDEO_FOCUS_INDICATION = 0x8008,
};
/* input-channel message ids (InputChannelMessageIdsEnum.proto) */
enum { AA_IN_EVENT = 0x8001, AA_IN_BINDING_REQUEST = 0x8002, AA_IN_BINDING_RESPONSE = 0x8003 };
/* sensor-channel message ids (SensorChannelMessageIdsEnum.proto) */
enum { AA_SEN_START_REQUEST = 0x8001, AA_SEN_START_RESPONSE = 0x8002, AA_SEN_EVENT = 0x8003 };
/* SensorType (SensorType.proto): the phone tears down with "No sensors" unless the HU advertises a sensor source */
enum { AA_SENSOR_NIGHT_DATA = 10, AA_SENSOR_DRIVING_STATUS = 13 };

/* enums */
enum { AA_VER_MATCH = 0, AA_VER_MISMATCH = 0xFFFF };          /* VersionResponseStatus */
enum { AA_STATUS_OK = 0, AA_STATUS_FAIL = 1 };                /* StatusEnum (channel open) */
enum { AA_AVSTATUS_NONE = 0, AA_AVSTATUS_FAIL = 1, AA_AVSTATUS_OK = 2 }; /* AVChannelSetupStatus */
enum { AA_AVSTREAM_NONE = 0, AA_AVSTREAM_AUDIO = 1, AA_AVSTREAM_VIDEO = 3 };
enum { AA_RES_NONE = 0, AA_RES_480p = 1, AA_RES_720p = 2, AA_RES_1080p = 3 }; /* 800x480 / 1280x720 / 1920x1080 */
enum { AA_FPS_NONE = 0, AA_FPS_30 = 1, AA_FPS_60 = 2 };
/* TouchAction = Android's MotionEvent actions (opencardev/aasdk PointerAction.proto): a second finger is a
 * POINTER_DOWN / POINTER_UP with action_index naming it in the event's list of every finger */
enum { AA_TOUCH_PRESS = 0, AA_TOUCH_RELEASE = 1, AA_TOUCH_DRAG = 2, AA_TOUCH_POINTER_DOWN = 5, AA_TOUCH_POINTER_UP = 6 };

/* protobuf field numbers */
/* ServiceDiscoveryRequest */
#define AA_SDREQ_DEVICE_NAME 4
#define AA_SDREQ_DEVICE_BRAND 5
/* ServiceDiscoveryResponse */
#define AA_SDRESP_CHANNELS 1          /* repeated ChannelDescriptor */
#define AA_SDRESP_HEAD_UNIT_NAME 2
#define AA_SDRESP_CAR_MODEL 3
#define AA_SDRESP_CAR_YEAR 4
#define AA_SDRESP_CAR_SERIAL 5
#define AA_SDRESP_LHD 6
#define AA_SDRESP_HU_MANUFACTURER 7
#define AA_SDRESP_HU_MODEL 8
#define AA_SDRESP_SW_BUILD 9
#define AA_SDRESP_SW_VERSION 10
#define AA_SDRESP_NATIVE_MEDIA_VR 11
#define AA_SDRESP_HIDE_CLOCK 12
/* ChannelDescriptor */
#define AA_CHD_CHANNEL_ID 1
#define AA_CHD_SENSOR_CHANNEL 2
#define AA_CHD_AV_CHANNEL 3
#define AA_CHD_INPUT_CHANNEL 4
/* SensorChannel descriptor { repeated Sensor sensors = 1 }, Sensor { type = 1 } */
#define AA_SENCH_SENSORS 1
#define AA_SEN_TYPE 1
/* SensorRequest { sensor_type = 1 } / SensorResponse { status = 1 } */
#define AA_SENREQ_TYPE 1
#define AA_SENRESP_STATUS 1
/* SensorBatch (SensorEventIndication): repeated NightMode night_mode = 10, repeated DrivingStatus driving_status = 13 */
#define AA_SENEV_NIGHT_MODE 10
#define AA_SENEV_DRIVING_STATUS 13
/* DrivingStatus { status = 1 } (0 = unrestricted), NightMode { is_night = 1 } */
#define AA_DRV_STATUS 1
#define AA_NIGHT_IS_NIGHT 1
/* ChannelOpenRequest / Response */
#define AA_COREQ_PRIORITY 1
#define AA_COREQ_CHANNEL_ID 2
#define AA_CORESP_STATUS 1
/* AVChannel (in ChannelDescriptor.av_channel) */
#define AA_AVCH_STREAM_TYPE 1
#define AA_AVCH_AUDIO_TYPE 2
#define AA_AVCH_VIDEO_CONFIGS 4
#define AA_AVCH_AVAILABLE_IN_CALL 5
/* AudioConfig { sample_rate = 1, bit_depth = 2, channel_count = 3 }, in AVChannel.audio_configs = 3.
 * The phone tears down with "No audio playback service" unless the HU advertises audio sinks. */
#define AA_AVCH_AUDIO_CONFIGS 3
#define AA_ACFG_SAMPLE_RATE 1
#define AA_ACFG_BIT_DEPTH 2
#define AA_ACFG_CHANNELS 3
enum { AA_AUDIO_SPEECH = 1, AA_AUDIO_SYSTEM = 2, AA_AUDIO_MEDIA = 3 };   /* AudioType */
/* AVInputChannel (ChannelDescriptor.av_input_channel = 5): the microphone; AA refuses without it ("No audio/mic").
 * { stream_type = 1, audio_config = 2 } (no audio_type here, unlike AVChannel); AVInputOpenResponse { session = 1, value = 2 } */
#define AA_CHD_AV_INPUT_CHANNEL 5
#define AA_AVIN_STREAM_TYPE 1
#define AA_AVIN_AUDIO_CONFIG 2
#define AA_AVINRESP_SESSION 1
#define AA_AVINRESP_VALUE 2
/* TouchConfig { width = 1, height = 2 } (in InputChannel.touch_screen_config) */
#define AA_TC_WIDTH 1
#define AA_TC_HEIGHT 2
/* AudioFocusRequest { audio_focus_type = 1 } / AudioFocusResponse { audio_focus_state = 1 } */
#define AA_AFREQ_TYPE 1
#define AA_AFRESP_STATE 1
enum { AA_AF_RELEASE = 4 };                /* AudioFocusType */
enum { AA_AF_GAIN = 1, AA_AF_LOSS = 3 };   /* AudioFocusState */
/* NavigationFocusRequest / Response { type = 1 } (2 = projected) */
#define AA_NAVF_TYPE 1
/* PingRequest / PingResponse { timestamp = 1 } */
#define AA_PING_TS 1
/* BindingResponse { status = 1 } */
#define AA_BINDRESP_STATUS 1
/* VideoConfig */
#define AA_VC_RESOLUTION 1
#define AA_VC_FPS 2
#define AA_VC_MARGIN_W 3
#define AA_VC_MARGIN_H 4
#define AA_VC_DPI 5
/* InputChannel descriptor (in ChannelDescriptor.input_channel) */
#define AA_INCH_TOUCH_SCREEN 2       /* TouchConfig (width=1,height=2) nested; see note */
/* AVChannelSetupRequest / Response */
#define AA_AVSETUP_CONFIG_INDEX 1
#define AA_AVSETUPRESP_STATUS 1
#define AA_AVSETUPRESP_MAX_UNACKED 2
#define AA_AVSETUPRESP_CONFIGS 3
/* InputEventIndication / TouchEvent / TouchLocation */
#define AA_IEI_TIMESTAMP 1
#define AA_IEI_DISP_CHANNEL 2
#define AA_IEI_TOUCH_EVENT 3
#define AA_TE_TOUCH_LOCATION 1
#define AA_TE_ACTION_INDEX 2
#define AA_TE_TOUCH_ACTION 3
#define AA_TL_X 1
#define AA_TL_Y 2
#define AA_TL_POINTER_ID 3
/* AuthCompleteIndication */
#define AA_AC_STATUS 1               /* Status.Enum (OK=0) */
/* VideoFocusIndication + VideoFocusMode */
#define AA_VFI_FOCUS_MODE 1
#define AA_VFI_UNREQUESTED 2
enum { AA_FOCUS_NONE = 0, AA_FOCUS_FOCUSED = 1, AA_FOCUS_UNFOCUSED = 2 };
/* AVChannelStartIndication (phone->HU): session=1, config=2 */
#define AA_START_SESSION 1
#define AA_START_CONFIG 2
/* AVMediaAckIndication (HU->phone): session=1, value=2 */
#define AA_ACK_SESSION 1
#define AA_ACK_VALUE 2

/* --- wireless Bluetooth handshake (WirelessAndroidAutoDongle scheme) ---
 * The HU advertises an RFCOMM service, the phone connects, and over it the HU hands the phone the WiFi AP
 * credentials; the phone then joins the AP and opens TCP 5288. BT framing: [uint16 BE len][uint16 BE id][protobuf],
 * len excludes the 4-byte header. */
#define AA_BT_UUID "4de17a00-52cb-11e6-bdf4-0800200c9a66"
#define AA_BT_RFCOMM_CHANNEL 8
enum { AA_BT_WIFI_START_REQUEST = 1, AA_BT_WIFI_INFO_REQUEST = 2, AA_BT_WIFI_INFO_RESPONSE = 3,
       AA_BT_WIFI_VERSION_REQUEST = 4, AA_BT_WIFI_VERSION_RESPONSE = 5, AA_BT_WIFI_CONNECT_STATUS = 6, AA_BT_WIFI_START_RESPONSE = 7 };
/* WifiStartRequest */
#define AA_BT_WSR_IP 1               /* string */
#define AA_BT_WSR_PORT 2             /* int32 */
/* WifiInfoResponse */
#define AA_BT_WIR_SSID 1
#define AA_BT_WIR_KEY 2
#define AA_BT_WIR_BSSID 3
#define AA_BT_WIR_SECURITY 4
#define AA_BT_WIR_AP_TYPE 5
enum { AA_SEC_OPEN = 1, AA_SEC_WPA2_PERSONAL = 8 };
enum { AA_AP_STATIC = 0, AA_AP_DYNAMIC = 1 };
