/*
 * Copyright (C) 2020 The Android Open Source Project
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

#define LOG_TAG "AidlConversion"
//#define LOG_NDEBUG 0
#include <system/audio.h>
#include <utils/Log.h>

#include "media/AidlConversion.h"

#include <media/ShmemCompat.h>

////////////////////////////////////////////////////////////////////////////////////////////////////
// Utilities

namespace android {

using base::unexpected;

namespace {

////////////////////////////////////////////////////////////////////////////////////////////////////
// The code below establishes:
// IntegralTypeOf<T>, which works for either integral types (in which case it evaluates to T), or
// enum types (in which case it evaluates to std::underlying_type_T<T>).

template<typename T, typename = std::enable_if_t<std::is_integral_v<T> || std::is_enum_v<T>>>
struct IntegralTypeOfStruct {
    using Type = T;
};

template<typename T>
struct IntegralTypeOfStruct<T, std::enable_if_t<std::is_enum_v<T>>> {
    using Type = std::underlying_type_t<T>;
};

template<typename T>
using IntegralTypeOf = typename IntegralTypeOfStruct<T>::Type;

////////////////////////////////////////////////////////////////////////////////////////////////////
// Utilities for handling bitmasks.

template<typename Enum>
Enum index2enum_index(int index) {
    static_assert(std::is_enum_v<Enum> || std::is_integral_v<Enum>);
    return static_cast<Enum>(index);
}

template<typename Enum>
Enum index2enum_bitmask(int index) {
    static_assert(std::is_enum_v<Enum> || std::is_integral_v<Enum>);
    return static_cast<Enum>(1 << index);
}

template<typename Mask, typename Enum>
Mask enumToMask_bitmask(Enum e) {
    static_assert(std::is_enum_v<Enum> || std::is_integral_v<Enum>);
    static_assert(std::is_enum_v<Mask> || std::is_integral_v<Mask>);
    return static_cast<Mask>(e);
}

template<typename Mask, typename Enum>
Mask enumToMask_index(Enum e) {
    static_assert(std::is_enum_v<Enum> || std::is_integral_v<Enum>);
    static_assert(std::is_enum_v<Mask> || std::is_integral_v<Mask>);
    return static_cast<Mask>(static_cast<std::make_unsigned_t<IntegralTypeOf<Mask>>>(1)
            << static_cast<int>(e));
}

template<typename DestMask, typename SrcMask, typename DestEnum, typename SrcEnum>
ConversionResult<DestMask> convertBitmask(
        SrcMask src, const std::function<ConversionResult<DestEnum>(SrcEnum)>& enumConversion,
        const std::function<SrcEnum(int)>& srcIndexToEnum,
        const std::function<DestMask(DestEnum)>& destEnumToMask) {
    using UnsignedDestMask = std::make_unsigned_t<IntegralTypeOf<DestMask>>;
    using UnsignedSrcMask = std::make_unsigned_t<IntegralTypeOf<SrcMask>>;

    UnsignedDestMask dest = static_cast<UnsignedDestMask>(0);
    UnsignedSrcMask usrc = static_cast<UnsignedSrcMask>(src);

    int srcBitIndex = 0;
    while (usrc != 0) {
        if (usrc & 1) {
            SrcEnum srcEnum = srcIndexToEnum(srcBitIndex);
            DestEnum destEnum = VALUE_OR_RETURN(enumConversion(srcEnum));
            DestMask destMask = destEnumToMask(destEnum);
            dest |= destMask;
        }
        ++srcBitIndex;
        usrc >>= 1;
    }
    return static_cast<DestMask>(dest);
}

template<typename Mask, typename Enum>
bool bitmaskIsSet(Mask mask, Enum index) {
    return (mask & enumToMask_index<Mask, Enum>(index)) != 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Utilities for working with AIDL unions.
// UNION_GET(obj, fieldname) returns a ConversionResult<T> containing either the strongly-typed
//   value of the respective field, or BAD_VALUE if the union is not set to the requested field.
// UNION_SET(obj, fieldname, value) sets the requested field to the given value.

template<typename T, typename T::Tag tag>
using UnionFieldType = std::decay_t<decltype(std::declval<T>().template get<tag>())>;

template<typename T, typename T::Tag tag>
ConversionResult<UnionFieldType<T, tag>> unionGetField(const T& u) {
    if (u.getTag() != tag) {
        return unexpected(BAD_VALUE);
    }
    return u.template get<tag>();
}

#define UNION_GET(u, field) \
    unionGetField<std::decay_t<decltype(u)>, std::decay_t<decltype(u)>::Tag::field>(u)

#define UNION_SET(u, field, value) \
    (u).set<std::decay_t<decltype(u)>::Tag::field>(value)

////////////////////////////////////////////////////////////////////////////////////////////////////

template<typename To, typename From>
ConversionResult<To> convertReinterpret(From from) {
    static_assert(sizeof(From) == sizeof(To));
    return static_cast<To>(from);
}

enum class Direction {
    INPUT, OUTPUT
};

ConversionResult<Direction> direction(media::AudioPortRole role, media::AudioPortType type) {
    switch (type) {
        case media::AudioPortType::DEVICE:
            switch (role) {
                case media::AudioPortRole::SOURCE:
                    return Direction::INPUT;
                case media::AudioPortRole::SINK:
                    return Direction::OUTPUT;
                default:
                    break;
            }
            break;
        case media::AudioPortType::MIX:
            switch (role) {
                case media::AudioPortRole::SOURCE:
                    return Direction::OUTPUT;
                case media::AudioPortRole::SINK:
                    return Direction::INPUT;
                default:
                    break;
            }
            break;
        default:
            break;
    }
    return unexpected(BAD_VALUE);
}

ConversionResult<Direction> direction(audio_port_role_t role, audio_port_type_t type) {
    switch (type) {
        case AUDIO_PORT_TYPE_DEVICE:
            switch (role) {
                case AUDIO_PORT_ROLE_SOURCE:
                    return Direction::INPUT;
                case AUDIO_PORT_ROLE_SINK:
                    return Direction::OUTPUT;
                default:
                    break;
            }
            break;
        case AUDIO_PORT_TYPE_MIX:
            switch (role) {
                case AUDIO_PORT_ROLE_SOURCE:
                    return Direction::OUTPUT;
                case AUDIO_PORT_ROLE_SINK:
                    return Direction::INPUT;
                default:
                    break;
            }
            break;
        default:
            break;
    }
    return unexpected(BAD_VALUE);
}

}  // namespace

////////////////////////////////////////////////////////////////////////////////////////////////////
// Converters

status_t aidl2legacy_string(std::string_view aidl, char* dest, size_t maxSize) {
    if (aidl.size() > maxSize - 1) {
        return BAD_VALUE;
    }
    aidl.copy(dest, aidl.size());
    dest[aidl.size()] = '\0';
    return OK;
}

ConversionResult<std::string> legacy2aidl_string(const char* legacy, size_t maxSize) {
    if (legacy == nullptr) {
        return unexpected(BAD_VALUE);
    }
    if (strnlen(legacy, maxSize) == maxSize) {
        // No null-terminator.
        return unexpected(BAD_VALUE);
    }
    return std::string(legacy);
}

ConversionResult<audio_module_handle_t> aidl2legacy_int32_t_audio_module_handle_t(int32_t aidl) {
    return convertReinterpret<audio_module_handle_t>(aidl);
}

ConversionResult<int32_t> legacy2aidl_audio_module_handle_t_int32_t(audio_module_handle_t legacy) {
    return convertReinterpret<int32_t>(legacy);
}

ConversionResult<audio_io_handle_t> aidl2legacy_int32_t_audio_io_handle_t(int32_t aidl) {
    return convertReinterpret<audio_io_handle_t>(aidl);
}

ConversionResult<int32_t> legacy2aidl_audio_io_handle_t_int32_t(audio_io_handle_t legacy) {
    return convertReinterpret<int32_t>(legacy);
}

ConversionResult<audio_port_handle_t> aidl2legacy_int32_t_audio_port_handle_t(int32_t aidl) {
    return convertReinterpret<audio_port_handle_t>(aidl);
}

ConversionResult<int32_t> legacy2aidl_audio_port_handle_t_int32_t(audio_port_handle_t legacy) {
    return convertReinterpret<int32_t>(legacy);
}

ConversionResult<audio_patch_handle_t> aidl2legacy_int32_t_audio_patch_handle_t(int32_t aidl) {
    return convertReinterpret<audio_patch_handle_t>(aidl);
}

ConversionResult<int32_t> legacy2aidl_audio_patch_handle_t_int32_t(audio_patch_handle_t legacy) {
    return convertReinterpret<int32_t>(legacy);
}

ConversionResult<audio_unique_id_t> aidl2legacy_int32_t_audio_unique_id_t(int32_t aidl) {
    return convertReinterpret<audio_unique_id_t>(aidl);
}

ConversionResult<int32_t> legacy2aidl_audio_unique_id_t_int32_t(audio_unique_id_t legacy) {
    return convertReinterpret<int32_t>(legacy);
}

ConversionResult<pid_t> aidl2legacy_int32_t_pid_t(int32_t aidl) {
    return convertReinterpret<pid_t>(aidl);
}

ConversionResult<int32_t> legacy2aidl_pid_t_int32_t(pid_t legacy) {
    return convertReinterpret<int32_t>(legacy);
}

ConversionResult<uid_t> aidl2legacy_int32_t_uid_t(int32_t aidl) {
    return convertReinterpret<uid_t>(aidl);
}

ConversionResult<int32_t> legacy2aidl_uid_t_int32_t(uid_t legacy) {
    return convertReinterpret<int32_t>(legacy);
}

ConversionResult<String16> aidl2legacy_string_view_String16(std::string_view aidl) {
    return String16(aidl.data(), aidl.size());
}

ConversionResult<std::string> legacy2aidl_String16_string(const String16& legacy) {
    return std::string(String8(legacy).c_str());
}

// The legacy enum is unnamed. Thus, we use int.
ConversionResult<int> aidl2legacy_AudioPortConfigType(media::AudioPortConfigType aidl) {
    switch (aidl) {
        case media::AudioPortConfigType::SAMPLE_RATE:
            return AUDIO_PORT_CONFIG_SAMPLE_RATE;
        case media::AudioPortConfigType::CHANNEL_MASK:
            return AUDIO_PORT_CONFIG_CHANNEL_MASK;
        case media::AudioPortConfigType::FORMAT:
            return AUDIO_PORT_CONFIG_FORMAT;
        case media::AudioPortConfigType::FLAGS:
            return AUDIO_PORT_CONFIG_FLAGS;
        default:
            return unexpected(BAD_VALUE);
    }
}

// The legacy enum is unnamed. Thus, we use int.
ConversionResult<media::AudioPortConfigType> legacy2aidl_AudioPortConfigType(int legacy) {
    switch (legacy) {
        case AUDIO_PORT_CONFIG_SAMPLE_RATE:
            return media::AudioPortConfigType::SAMPLE_RATE;
        case AUDIO_PORT_CONFIG_CHANNEL_MASK:
            return media::AudioPortConfigType::CHANNEL_MASK;
        case AUDIO_PORT_CONFIG_FORMAT:
            return media::AudioPortConfigType::FORMAT;
        case AUDIO_PORT_CONFIG_FLAGS:
            return media::AudioPortConfigType::FLAGS;
        default:
            return unexpected(BAD_VALUE);
    }
}

ConversionResult<unsigned int> aidl2legacy_int32_t_config_mask(int32_t aidl) {
    return convertBitmask<unsigned int, int32_t, int, media::AudioPortConfigType>(
            aidl, aidl2legacy_AudioPortConfigType,
            // AudioPortConfigType enum is index-based.
            index2enum_index<media::AudioPortConfigType>,
            // AUDIO_PORT_CONFIG_* flags are mask-based.
            enumToMask_bitmask<unsigned int, int>);
}

ConversionResult<int32_t> legacy2aidl_config_mask_int32_t(unsigned int legacy) {
    return convertBitmask<int32_t, unsigned int, media::AudioPortConfigType, int>(
            legacy, legacy2aidl_AudioPortConfigType,
            // AUDIO_PORT_CONFIG_* flags are mask-based.
            index2enum_bitmask<unsigned>,
            // AudioPortConfigType enum is index-based.
            enumToMask_index<int32_t, media::AudioPortConfigType>);
}

ConversionResult<audio_channel_mask_t> aidl2legacy_int32_t_audio_channel_mask_t(int32_t aidl) {
    // TODO(ytai): should we convert bit-by-bit?
    // One problem here is that the representation is both opaque and is different based on the
    // context (input vs. output). Can determine based on type and role, as per useInChannelMask().
    return convertReinterpret<audio_channel_mask_t>(aidl);
}

ConversionResult<int32_t> legacy2aidl_audio_channel_mask_t_int32_t(audio_channel_mask_t legacy) {
    // TODO(ytai): should we convert bit-by-bit?
    // One problem here is that the representation is both opaque and is different based on the
    // context (input vs. output). Can determine based on type and role, as per useInChannelMask().
    return convertReinterpret<int32_t>(legacy);
}

ConversionResult<audio_io_config_event> aidl2legacy_AudioIoConfigEvent_audio_io_config_event(
        media::AudioIoConfigEvent aidl) {
    switch (aidl) {
        case media::AudioIoConfigEvent::OUTPUT_REGISTERED:
            return AUDIO_OUTPUT_REGISTERED;
        case media::AudioIoConfigEvent::OUTPUT_OPENED:
            return AUDIO_OUTPUT_OPENED;
        case media::AudioIoConfigEvent::OUTPUT_CLOSED:
            return AUDIO_OUTPUT_CLOSED;
        case media::AudioIoConfigEvent::OUTPUT_CONFIG_CHANGED:
            return AUDIO_OUTPUT_CONFIG_CHANGED;
        case media::AudioIoConfigEvent::INPUT_REGISTERED:
            return AUDIO_INPUT_REGISTERED;
        case media::AudioIoConfigEvent::INPUT_OPENED:
            return AUDIO_INPUT_OPENED;
        case media::AudioIoConfigEvent::INPUT_CLOSED:
            return AUDIO_INPUT_CLOSED;
        case media::AudioIoConfigEvent::INPUT_CONFIG_CHANGED:
            return AUDIO_INPUT_CONFIG_CHANGED;
        case media::AudioIoConfigEvent::CLIENT_STARTED:
            return AUDIO_CLIENT_STARTED;
        default:
            return unexpected(BAD_VALUE);
    }
}

ConversionResult<media::AudioIoConfigEvent> legacy2aidl_audio_io_config_event_AudioIoConfigEvent(
        audio_io_config_event legacy) {
    switch (legacy) {
        case AUDIO_OUTPUT_REGISTERED:
            return media::AudioIoConfigEvent::OUTPUT_REGISTERED;
        case AUDIO_OUTPUT_OPENED:
            return media::AudioIoConfigEvent::OUTPUT_OPENED;
        case AUDIO_OUTPUT_CLOSED:
            return media::AudioIoConfigEvent::OUTPUT_CLOSED;
        case AUDIO_OUTPUT_CONFIG_CHANGED:
            return media::AudioIoConfigEvent::OUTPUT_CONFIG_CHANGED;
        case AUDIO_INPUT_REGISTERED:
            return media::AudioIoConfigEvent::INPUT_REGISTERED;
        case AUDIO_INPUT_OPENED:
            return media::AudioIoConfigEvent::INPUT_OPENED;
        case AUDIO_INPUT_CLOSED:
            return media::AudioIoConfigEvent::INPUT_CLOSED;
        case AUDIO_INPUT_CONFIG_CHANGED:
            return media::AudioIoConfigEvent::INPUT_CONFIG_CHANGED;
        case AUDIO_CLIENT_STARTED:
            return media::AudioIoConfigEvent::CLIENT_STARTED;
        default:
            return unexpected(BAD_VALUE);
    }
}

ConversionResult<audio_port_role_t> aidl2legacy_AudioPortRole_audio_port_role_t(
        media::AudioPortRole aidl) {
    switch (aidl) {
        case media::AudioPortRole::NONE:
            return AUDIO_PORT_ROLE_NONE;
        case media::AudioPortRole::SOURCE:
            return AUDIO_PORT_ROLE_SOURCE;
        case media::AudioPortRole::SINK:
            return AUDIO_PORT_ROLE_SINK;
        default:
            return unexpected(BAD_VALUE);
    }
}

ConversionResult<media::AudioPortRole> legacy2aidl_audio_port_role_t_AudioPortRole(
        audio_port_role_t legacy) {
    switch (legacy) {
        case AUDIO_PORT_ROLE_NONE:
            return media::AudioPortRole::NONE;
        case AUDIO_PORT_ROLE_SOURCE:
            return media::AudioPortRole::SOURCE;
        case AUDIO_PORT_ROLE_SINK:
            return media::AudioPortRole::SINK;
        default:
            return unexpected(BAD_VALUE);
    }
}

ConversionResult<audio_port_type_t> aidl2legacy_AudioPortType_audio_port_type_t(
        media::AudioPortType aidl) {
    switch (aidl) {
        case media::AudioPortType::NONE:
            return AUDIO_PORT_TYPE_NONE;
        case media::AudioPortType::DEVICE:
            return AUDIO_PORT_TYPE_DEVICE;
        case media::AudioPortType::MIX:
            return AUDIO_PORT_TYPE_MIX;
        case media::AudioPortType::SESSION:
            return AUDIO_PORT_TYPE_SESSION;
        default:
            return unexpected(BAD_VALUE);
    }
}

ConversionResult<media::AudioPortType> legacy2aidl_audio_port_type_t_AudioPortType(
        audio_port_type_t legacy) {
    switch (legacy) {
        case AUDIO_PORT_TYPE_NONE:
            return media::AudioPortType::NONE;
        case AUDIO_PORT_TYPE_DEVICE:
            return media::AudioPortType::DEVICE;
        case AUDIO_PORT_TYPE_MIX:
            return media::AudioPortType::MIX;
        case AUDIO_PORT_TYPE_SESSION:
            return media::AudioPortType::SESSION;
        default:
            return unexpected(BAD_VALUE);
    }
}

ConversionResult<audio_format_t> aidl2legacy_AudioFormat_audio_format_t(
        media::audio::common::AudioFormat aidl) {
    // This relies on AudioFormat being kept in sync with audio_format_t.
    static_assert(sizeof(media::audio::common::AudioFormat) == sizeof(audio_format_t));
    return static_cast<audio_format_t>(aidl);
}

ConversionResult<media::audio::common::AudioFormat> legacy2aidl_audio_format_t_AudioFormat(
        audio_format_t legacy) {
    // This relies on AudioFormat being kept in sync with audio_format_t.
    static_assert(sizeof(media::audio::common::AudioFormat) == sizeof(audio_format_t));
    return static_cast<media::audio::common::AudioFormat>(legacy);
}

ConversionResult<int> aidl2legacy_AudioGainMode_int(media::AudioGainMode aidl) {
    switch (aidl) {
        case media::AudioGainMode::JOINT:
            return AUDIO_GAIN_MODE_JOINT;
        case media::AudioGainMode::CHANNELS:
            return AUDIO_GAIN_MODE_CHANNELS;
        case media::AudioGainMode::RAMP:
            return AUDIO_GAIN_MODE_RAMP;
        default:
            return unexpected(BAD_VALUE);
    }
}

ConversionResult<media::AudioGainMode> legacy2aidl_int_AudioGainMode(int legacy) {
    switch (legacy) {
        case AUDIO_GAIN_MODE_JOINT:
            return media::AudioGainMode::JOINT;
        case AUDIO_GAIN_MODE_CHANNELS:
            return media::AudioGainMode::CHANNELS;
        case AUDIO_GAIN_MODE_RAMP:
            return media::AudioGainMode::RAMP;
        default:
            return unexpected(BAD_VALUE);
    }
}

ConversionResult<audio_gain_mode_t> aidl2legacy_int32_t_audio_gain_mode_t(int32_t aidl) {
    return convertBitmask<audio_gain_mode_t, int32_t, int, media::AudioGainMode>(
            aidl, aidl2legacy_AudioGainMode_int,
            // AudioGainMode is index-based.
            index2enum_index<media::AudioGainMode>,
            // AUDIO_GAIN_MODE_* constants are mask-based.
            enumToMask_bitmask<audio_gain_mode_t, int>);
}

ConversionResult<int32_t> legacy2aidl_audio_gain_mode_t_int32_t(audio_gain_mode_t legacy) {
    return convertBitmask<int32_t, audio_gain_mode_t, media::AudioGainMode, int>(
            legacy, legacy2aidl_int_AudioGainMode,
            // AUDIO_GAIN_MODE_* constants are mask-based.
            index2enum_bitmask<int>,
            // AudioGainMode is index-based.
            enumToMask_index<int32_t, media::AudioGainMode>);
}

ConversionResult<audio_devices_t> aidl2legacy_int32_t_audio_devices_t(int32_t aidl) {
    // TODO(ytai): bitfield?
    return convertReinterpret<audio_devices_t>(aidl);
}

ConversionResult<int32_t> legacy2aidl_audio_devices_t_int32_t(audio_devices_t legacy) {
    // TODO(ytai): bitfield?
    return convertReinterpret<int32_t>(legacy);
}

ConversionResult<audio_gain_config> aidl2legacy_AudioGainConfig_audio_gain_config(
        const media::AudioGainConfig& aidl, media::AudioPortRole role, media::AudioPortType type) {
    audio_gain_config legacy;
    legacy.index = VALUE_OR_RETURN(convertIntegral<int>(aidl.index));
    legacy.mode = VALUE_OR_RETURN(aidl2legacy_int32_t_audio_gain_mode_t(aidl.mode));
    legacy.channel_mask =
            VALUE_OR_RETURN(aidl2legacy_int32_t_audio_channel_mask_t(aidl.channelMask));
    const bool isInput = VALUE_OR_RETURN(direction(role, type)) == Direction::INPUT;
    const bool isJoint = bitmaskIsSet(aidl.mode, media::AudioGainMode::JOINT);
    size_t numValues = isJoint ? 1
                               : isInput ? audio_channel_count_from_in_mask(legacy.channel_mask)
                                         : audio_channel_count_from_out_mask(legacy.channel_mask);
    if (aidl.values.size() != numValues || aidl.values.size() > std::size(legacy.values)) {
        return unexpected(BAD_VALUE);
    }
    for (size_t i = 0; i < numValues; ++i) {
        legacy.values[i] = VALUE_OR_RETURN(convertIntegral<int>(aidl.values[i]));
    }
    legacy.ramp_duration_ms = VALUE_OR_RETURN(convertIntegral<unsigned int>(aidl.rampDurationMs));
    return legacy;
}

ConversionResult<media::AudioGainConfig> legacy2aidl_audio_gain_config_AudioGainConfig(
        const audio_gain_config& legacy, audio_port_role_t role, audio_port_type_t type) {
    media::AudioGainConfig aidl;
    aidl.index = VALUE_OR_RETURN(convertIntegral<int32_t>(legacy.index));
    aidl.mode = VALUE_OR_RETURN(legacy2aidl_audio_gain_mode_t_int32_t(legacy.mode));
    aidl.channelMask =
            VALUE_OR_RETURN(legacy2aidl_audio_channel_mask_t_int32_t(legacy.channel_mask));
    const bool isInput = VALUE_OR_RETURN(direction(role, type)) == Direction::INPUT;
    const bool isJoint = (legacy.mode & AUDIO_GAIN_MODE_JOINT) != 0;
    size_t numValues = isJoint ? 1
                               : isInput ? audio_channel_count_from_in_mask(legacy.channel_mask)
                                         : audio_channel_count_from_out_mask(legacy.channel_mask);
    aidl.values.resize(numValues);
    for (size_t i = 0; i < numValues; ++i) {
        aidl.values[i] = VALUE_OR_RETURN(convertIntegral<int32_t>(legacy.values[i]));
    }
    aidl.rampDurationMs = VALUE_OR_RETURN(convertIntegral<int32_t>(legacy.ramp_duration_ms));
    return aidl;
}

ConversionResult<audio_input_flags_t> aidl2legacy_AudioInputFlags_audio_input_flags_t(
        media::AudioInputFlags aidl) {
    switch (aidl) {
        case media::AudioInputFlags::FAST:
            return AUDIO_INPUT_FLAG_FAST;
        case media::AudioInputFlags::HW_HOTWORD:
            return AUDIO_INPUT_FLAG_HW_HOTWORD;
        case media::AudioInputFlags::RAW:
            return AUDIO_INPUT_FLAG_RAW;
        case media::AudioInputFlags::SYNC:
            return AUDIO_INPUT_FLAG_SYNC;
        case media::AudioInputFlags::MMAP_NOIRQ:
            return AUDIO_INPUT_FLAG_MMAP_NOIRQ;
        case media::AudioInputFlags::VOIP_TX:
            return AUDIO_INPUT_FLAG_VOIP_TX;
        case media::AudioInputFlags::HW_AV_SYNC:
            return AUDIO_INPUT_FLAG_HW_AV_SYNC;
        case media::AudioInputFlags::DIRECT:
            return AUDIO_INPUT_FLAG_DIRECT;
        default:
            return unexpected(BAD_VALUE);
    }
}

ConversionResult<media::AudioInputFlags> legacy2aidl_audio_input_flags_t_AudioInputFlags(
        audio_input_flags_t legacy) {
    switch (legacy) {
        case AUDIO_INPUT_FLAG_FAST:
            return media::AudioInputFlags::FAST;
        case AUDIO_INPUT_FLAG_HW_HOTWORD:
            return media::AudioInputFlags::HW_HOTWORD;
        case AUDIO_INPUT_FLAG_RAW:
            return media::AudioInputFlags::RAW;
        case AUDIO_INPUT_FLAG_SYNC:
            return media::AudioInputFlags::SYNC;
        case AUDIO_INPUT_FLAG_MMAP_NOIRQ:
            return media::AudioInputFlags::MMAP_NOIRQ;
        case AUDIO_INPUT_FLAG_VOIP_TX:
            return media::AudioInputFlags::VOIP_TX;
        case AUDIO_INPUT_FLAG_HW_AV_SYNC:
            return media::AudioInputFlags::HW_AV_SYNC;
        case AUDIO_INPUT_FLAG_DIRECT:
            return media::AudioInputFlags::DIRECT;
        default:
            return unexpected(BAD_VALUE);
    }
}

ConversionResult<audio_output_flags_t> aidl2legacy_AudioOutputFlags_audio_output_flags_t(
        media::AudioOutputFlags aidl) {
    switch (aidl) {
        case media::AudioOutputFlags::DIRECT:
            return AUDIO_OUTPUT_FLAG_DIRECT;
        case media::AudioOutputFlags::PRIMARY:
            return AUDIO_OUTPUT_FLAG_PRIMARY;
        case media::AudioOutputFlags::FAST:
            return AUDIO_OUTPUT_FLAG_FAST;
        case media::AudioOutputFlags::DEEP_BUFFER:
            return AUDIO_OUTPUT_FLAG_DEEP_BUFFER;
        case media::AudioOutputFlags::COMPRESS_OFFLOAD:
            return AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD;
        case media::AudioOutputFlags::NON_BLOCKING:
            return AUDIO_OUTPUT_FLAG_NON_BLOCKING;
        case media::AudioOutputFlags::HW_AV_SYNC:
            return AUDIO_OUTPUT_FLAG_HW_AV_SYNC;
        case media::AudioOutputFlags::TTS:
            return AUDIO_OUTPUT_FLAG_TTS;
        case media::AudioOutputFlags::RAW:
            return AUDIO_OUTPUT_FLAG_RAW;
        case media::AudioOutputFlags::SYNC:
            return AUDIO_OUTPUT_FLAG_SYNC;
        case media::AudioOutputFlags::IEC958_NONAUDIO:
            return AUDIO_OUTPUT_FLAG_IEC958_NONAUDIO;
        case media::AudioOutputFlags::DIRECT_PCM:
            return AUDIO_OUTPUT_FLAG_DIRECT_PCM;
        case media::AudioOutputFlags::MMAP_NOIRQ:
            return AUDIO_OUTPUT_FLAG_MMAP_NOIRQ;
        case media::AudioOutputFlags::VOIP_RX:
            return AUDIO_OUTPUT_FLAG_VOIP_RX;
        case media::AudioOutputFlags::INCALL_MUSIC:
            return AUDIO_OUTPUT_FLAG_INCALL_MUSIC;
        default:
            return unexpected(BAD_VALUE);
    }
}

ConversionResult<media::AudioOutputFlags> legacy2aidl_audio_output_flags_t_AudioOutputFlags(
        audio_output_flags_t legacy) {
    switch (legacy) {
        case AUDIO_OUTPUT_FLAG_DIRECT:
            return media::AudioOutputFlags::DIRECT;
        case AUDIO_OUTPUT_FLAG_PRIMARY:
            return media::AudioOutputFlags::PRIMARY;
        case AUDIO_OUTPUT_FLAG_FAST:
            return media::AudioOutputFlags::FAST;
        case AUDIO_OUTPUT_FLAG_DEEP_BUFFER:
            return media::AudioOutputFlags::DEEP_BUFFER;
        case AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD:
            return media::AudioOutputFlags::COMPRESS_OFFLOAD;
        case AUDIO_OUTPUT_FLAG_NON_BLOCKING:
            return media::AudioOutputFlags::NON_BLOCKING;
        case AUDIO_OUTPUT_FLAG_HW_AV_SYNC:
            return media::AudioOutputFlags::HW_AV_SYNC;
        case AUDIO_OUTPUT_FLAG_TTS:
            return media::AudioOutputFlags::TTS;
        case AUDIO_OUTPUT_FLAG_RAW:
            return media::AudioOutputFlags::RAW;
        case AUDIO_OUTPUT_FLAG_SYNC:
            return media::AudioOutputFlags::SYNC;
        case AUDIO_OUTPUT_FLAG_IEC958_NONAUDIO:
            return media::AudioOutputFlags::IEC958_NONAUDIO;
        case AUDIO_OUTPUT_FLAG_DIRECT_PCM:
            return media::AudioOutputFlags::DIRECT_PCM;
        case AUDIO_OUTPUT_FLAG_MMAP_NOIRQ:
            return media::AudioOutputFlags::MMAP_NOIRQ;
        case AUDIO_OUTPUT_FLAG_VOIP_RX:
            return media::AudioOutputFlags::VOIP_RX;
        case AUDIO_OUTPUT_FLAG_INCALL_MUSIC:
            return media::AudioOutputFlags::INCALL_MUSIC;
        default:
            return unexpected(BAD_VALUE);
    }
}

ConversionResult<audio_input_flags_t> aidl2legacy_audio_input_flags_mask(int32_t aidl) {
    using LegacyMask = std::underlying_type_t<audio_input_flags_t>;

    LegacyMask converted = VALUE_OR_RETURN(
            (convertBitmask<LegacyMask, int32_t, audio_input_flags_t, media::AudioInputFlags>(
                    aidl, aidl2legacy_AudioInputFlags_audio_input_flags_t,
                    index2enum_index<media::AudioInputFlags>,
                    enumToMask_bitmask<LegacyMask, audio_input_flags_t>)));
    return static_cast<audio_input_flags_t>(converted);
}

ConversionResult<int32_t> legacy2aidl_audio_input_flags_mask(audio_input_flags_t legacy) {
    using LegacyMask = std::underlying_type_t<audio_input_flags_t>;

    LegacyMask legacyMask = static_cast<LegacyMask>(legacy);
    return convertBitmask<int32_t, LegacyMask, media::AudioInputFlags, audio_input_flags_t>(
            legacyMask, legacy2aidl_audio_input_flags_t_AudioInputFlags,
            index2enum_bitmask<audio_input_flags_t>,
            enumToMask_index<int32_t, media::AudioInputFlags>);
}

ConversionResult<audio_output_flags_t> aidl2legacy_audio_output_flags_mask(int32_t aidl) {
    return convertBitmask<audio_output_flags_t,
                          int32_t,
                          audio_output_flags_t,
                          media::AudioOutputFlags>(
            aidl, aidl2legacy_AudioOutputFlags_audio_output_flags_t,
            index2enum_index<media::AudioOutputFlags>,
            enumToMask_bitmask<audio_output_flags_t, audio_output_flags_t>);
}

ConversionResult<int32_t> legacy2aidl_audio_output_flags_mask(audio_output_flags_t legacy) {
    using LegacyMask = std::underlying_type_t<audio_output_flags_t>;

    LegacyMask legacyMask = static_cast<LegacyMask>(legacy);
    return convertBitmask<int32_t, LegacyMask, media::AudioOutputFlags, audio_output_flags_t>(
            legacyMask, legacy2aidl_audio_output_flags_t_AudioOutputFlags,
            index2enum_bitmask<audio_output_flags_t>,
            enumToMask_index<int32_t, media::AudioOutputFlags>);
}

ConversionResult<audio_io_flags> aidl2legacy_AudioIoFlags_audio_io_flags(
        const media::AudioIoFlags& aidl, media::AudioPortRole role, media::AudioPortType type) {
    audio_io_flags legacy;
    Direction dir = VALUE_OR_RETURN(direction(role, type));
    switch (dir) {
        case Direction::INPUT: {
            legacy.input = VALUE_OR_RETURN(
                    aidl2legacy_audio_input_flags_mask(VALUE_OR_RETURN(UNION_GET(aidl, input))));
        }
            break;

        case Direction::OUTPUT: {
            legacy.output = VALUE_OR_RETURN(
                    aidl2legacy_audio_output_flags_mask(VALUE_OR_RETURN(UNION_GET(aidl, output))));
        }
            break;
    }

    return legacy;
}

ConversionResult<media::AudioIoFlags> legacy2aidl_audio_io_flags_AudioIoFlags(
        const audio_io_flags& legacy, audio_port_role_t role, audio_port_type_t type) {
    media::AudioIoFlags aidl;

    Direction dir = VALUE_OR_RETURN(direction(role, type));
    switch (dir) {
        case Direction::INPUT:
            UNION_SET(aidl, input,
                      VALUE_OR_RETURN(legacy2aidl_audio_input_flags_mask(legacy.input)));
            break;
        case Direction::OUTPUT:
            UNION_SET(aidl, output,
                      VALUE_OR_RETURN(legacy2aidl_audio_output_flags_mask(legacy.output)));
            break;
    }
    return aidl;
}

ConversionResult<audio_port_config_device_ext> aidl2legacy_AudioPortConfigDeviceExt(
        const media::AudioPortConfigDeviceExt& aidl) {
    audio_port_config_device_ext legacy;
    legacy.hw_module = VALUE_OR_RETURN(aidl2legacy_int32_t_audio_module_handle_t(aidl.hwModule));
    legacy.type = VALUE_OR_RETURN(aidl2legacy_int32_t_audio_devices_t(aidl.type));
    RETURN_IF_ERROR(aidl2legacy_string(aidl.address, legacy.address, AUDIO_DEVICE_MAX_ADDRESS_LEN));
    return legacy;
}

ConversionResult<media::AudioPortConfigDeviceExt> legacy2aidl_AudioPortConfigDeviceExt(
        const audio_port_config_device_ext& legacy) {
    media::AudioPortConfigDeviceExt aidl;
    aidl.hwModule = VALUE_OR_RETURN(legacy2aidl_audio_module_handle_t_int32_t(legacy.hw_module));
    aidl.type = VALUE_OR_RETURN(legacy2aidl_audio_devices_t_int32_t(legacy.type));
    aidl.address = VALUE_OR_RETURN(
            legacy2aidl_string(legacy.address, AUDIO_DEVICE_MAX_ADDRESS_LEN));
    return aidl;
}

ConversionResult<audio_stream_type_t> aidl2legacy_AudioStreamType_audio_stream_type_t(
        media::AudioStreamType aidl) {
    switch (aidl) {
        case media::AudioStreamType::DEFAULT:
            return AUDIO_STREAM_DEFAULT;
        case media::AudioStreamType::VOICE_CALL:
            return AUDIO_STREAM_VOICE_CALL;
        case media::AudioStreamType::SYSTEM:
            return AUDIO_STREAM_SYSTEM;
        case media::AudioStreamType::RING:
            return AUDIO_STREAM_RING;
        case media::AudioStreamType::MUSIC:
            return AUDIO_STREAM_MUSIC;
        case media::AudioStreamType::ALARM:
            return AUDIO_STREAM_ALARM;
        case media::AudioStreamType::NOTIFICATION:
            return AUDIO_STREAM_NOTIFICATION;
        case media::AudioStreamType::BLUETOOTH_SCO:
            return AUDIO_STREAM_BLUETOOTH_SCO;
        case media::AudioStreamType::ENFORCED_AUDIBLE:
            return AUDIO_STREAM_ENFORCED_AUDIBLE;
        case media::AudioStreamType::DTMF:
            return AUDIO_STREAM_DTMF;
        case media::AudioStreamType::TTS:
            return AUDIO_STREAM_TTS;
        case media::AudioStreamType::ACCESSIBILITY:
            return AUDIO_STREAM_ACCESSIBILITY;
        case media::AudioStreamType::ASSISTANT:
            return AUDIO_STREAM_ASSISTANT;
        case media::AudioStreamType::REROUTING:
            return AUDIO_STREAM_REROUTING;
        case media::AudioStreamType::PATCH:
            return AUDIO_STREAM_PATCH;
        case media::AudioStreamType::CALL_ASSISTANT:
            return AUDIO_STREAM_CALL_ASSISTANT;
        default:
            return unexpected(BAD_VALUE);
    }
}

ConversionResult<media::AudioStreamType> legacy2aidl_audio_stream_type_t_AudioStreamType(
        audio_stream_type_t legacy) {
    switch (legacy) {
        case AUDIO_STREAM_DEFAULT:
            return media::AudioStreamType::DEFAULT;
        case AUDIO_STREAM_VOICE_CALL:
            return media::AudioStreamType::VOICE_CALL;
        case AUDIO_STREAM_SYSTEM:
            return media::AudioStreamType::SYSTEM;
        case AUDIO_STREAM_RING:
            return media::AudioStreamType::RING;
        case AUDIO_STREAM_MUSIC:
            return media::AudioStreamType::MUSIC;
        case AUDIO_STREAM_ALARM:
            return media::AudioStreamType::ALARM;
        case AUDIO_STREAM_NOTIFICATION:
            return media::AudioStreamType::NOTIFICATION;
        case AUDIO_STREAM_BLUETOOTH_SCO:
            return media::AudioStreamType::BLUETOOTH_SCO;
        case AUDIO_STREAM_ENFORCED_AUDIBLE:
            return media::AudioStreamType::ENFORCED_AUDIBLE;
        case AUDIO_STREAM_DTMF:
            return media::AudioStreamType::DTMF;
        case AUDIO_STREAM_TTS:
            return media::AudioStreamType::TTS;
        case AUDIO_STREAM_ACCESSIBILITY:
            return media::AudioStreamType::ACCESSIBILITY;
        case AUDIO_STREAM_ASSISTANT:
            return media::AudioStreamType::ASSISTANT;
        case AUDIO_STREAM_REROUTING:
            return media::AudioStreamType::REROUTING;
        case AUDIO_STREAM_PATCH:
            return media::AudioStreamType::PATCH;
        case AUDIO_STREAM_CALL_ASSISTANT:
            return media::AudioStreamType::CALL_ASSISTANT;
        default:
            return unexpected(BAD_VALUE);
    }
}

ConversionResult<audio_source_t> aidl2legacy_AudioSourceType_audio_source_t(
        media::AudioSourceType aidl) {
    switch (aidl) {
        case media::AudioSourceType::INVALID:
            // This value does not have an enum
            return AUDIO_SOURCE_INVALID;
        case media::AudioSourceType::DEFAULT:
            return AUDIO_SOURCE_DEFAULT;
        case media::AudioSourceType::MIC:
            return AUDIO_SOURCE_MIC;
        case media::AudioSourceType::VOICE_UPLINK:
            return AUDIO_SOURCE_VOICE_UPLINK;
        case media::AudioSourceType::VOICE_DOWNLINK:
            return AUDIO_SOURCE_VOICE_DOWNLINK;
        case media::AudioSourceType::VOICE_CALL:
            return AUDIO_SOURCE_VOICE_CALL;
        case media::AudioSourceType::CAMCORDER:
            return AUDIO_SOURCE_CAMCORDER;
        case media::AudioSourceType::VOICE_RECOGNITION:
            return AUDIO_SOURCE_VOICE_RECOGNITION;
        case media::AudioSourceType::VOICE_COMMUNICATION:
            return AUDIO_SOURCE_VOICE_COMMUNICATION;
        case media::AudioSourceType::REMOTE_SUBMIX:
            return AUDIO_SOURCE_REMOTE_SUBMIX;
        case media::AudioSourceType::UNPROCESSED:
            return AUDIO_SOURCE_UNPROCESSED;
        case media::AudioSourceType::VOICE_PERFORMANCE:
            return AUDIO_SOURCE_VOICE_PERFORMANCE;
        case media::AudioSourceType::ECHO_REFERENCE:
            return AUDIO_SOURCE_ECHO_REFERENCE;
        case media::AudioSourceType::FM_TUNER:
            return AUDIO_SOURCE_FM_TUNER;
        case media::AudioSourceType::HOTWORD:
            return AUDIO_SOURCE_HOTWORD;
        default:
            return unexpected(BAD_VALUE);
    }
}

ConversionResult<media::AudioSourceType> legacy2aidl_audio_source_t_AudioSourceType(
        audio_source_t legacy) {
    switch (legacy) {
        case AUDIO_SOURCE_INVALID:
            return media::AudioSourceType::INVALID;
        case AUDIO_SOURCE_DEFAULT:
            return media::AudioSourceType::DEFAULT;
        case AUDIO_SOURCE_MIC:
            return media::AudioSourceType::MIC;
        case AUDIO_SOURCE_VOICE_UPLINK:
            return media::AudioSourceType::VOICE_UPLINK;
        case AUDIO_SOURCE_VOICE_DOWNLINK:
            return media::AudioSourceType::VOICE_DOWNLINK;
        case AUDIO_SOURCE_VOICE_CALL:
            return media::AudioSourceType::VOICE_CALL;
        case AUDIO_SOURCE_CAMCORDER:
            return media::AudioSourceType::CAMCORDER;
        case AUDIO_SOURCE_VOICE_RECOGNITION:
            return media::AudioSourceType::VOICE_RECOGNITION;
        case AUDIO_SOURCE_VOICE_COMMUNICATION:
            return media::AudioSourceType::VOICE_COMMUNICATION;
        case AUDIO_SOURCE_REMOTE_SUBMIX:
            return media::AudioSourceType::REMOTE_SUBMIX;
        case AUDIO_SOURCE_UNPROCESSED:
            return media::AudioSourceType::UNPROCESSED;
        case AUDIO_SOURCE_VOICE_PERFORMANCE:
            return media::AudioSourceType::VOICE_PERFORMANCE;
        case AUDIO_SOURCE_ECHO_REFERENCE:
            return media::AudioSourceType::ECHO_REFERENCE;
        case AUDIO_SOURCE_FM_TUNER:
            return media::AudioSourceType::FM_TUNER;
        case AUDIO_SOURCE_HOTWORD:
            return media::AudioSourceType::HOTWORD;
        default:
            return unexpected(BAD_VALUE);
    }
}

ConversionResult<audio_session_t> aidl2legacy_int32_t_audio_session_t(int32_t aidl) {
    return convertReinterpret<audio_session_t>(aidl);
}

ConversionResult<int32_t> legacy2aidl_audio_session_t_int32_t(audio_session_t legacy) {
    return convertReinterpret<int32_t>(legacy);
}

// This type is unnamed in the original definition, thus we name it here.
using audio_port_config_mix_ext_usecase = decltype(audio_port_config_mix_ext::usecase);

ConversionResult<audio_port_config_mix_ext_usecase> aidl2legacy_AudioPortConfigMixExtUseCase(
        const media::AudioPortConfigMixExtUseCase& aidl, media::AudioPortRole role) {
    audio_port_config_mix_ext_usecase legacy;

    switch (role) {
        case media::AudioPortRole::NONE:
            // Just verify that the union is empty.
            VALUE_OR_RETURN(UNION_GET(aidl, nothing));
            break;

        case media::AudioPortRole::SOURCE:
            // This is not a bug. A SOURCE role corresponds to the stream field.
            legacy.stream = VALUE_OR_RETURN(aidl2legacy_AudioStreamType_audio_stream_type_t(
                    VALUE_OR_RETURN(UNION_GET(aidl, stream))));
            break;

        case media::AudioPortRole::SINK:
            // This is not a bug. A SINK role corresponds to the source field.
            legacy.source = VALUE_OR_RETURN(aidl2legacy_AudioSourceType_audio_source_t(
                    VALUE_OR_RETURN(UNION_GET(aidl, source))));
            break;

        default:
            LOG_ALWAYS_FATAL("Shouldn't get here");
    }
    return legacy;
}

ConversionResult<media::AudioPortConfigMixExtUseCase> legacy2aidl_AudioPortConfigMixExtUseCase(
        const audio_port_config_mix_ext_usecase& legacy, audio_port_role_t role) {
    media::AudioPortConfigMixExtUseCase aidl;

    switch (role) {
        case AUDIO_PORT_ROLE_NONE:
            UNION_SET(aidl, nothing, false);
            break;
        case AUDIO_PORT_ROLE_SOURCE:
            // This is not a bug. A SOURCE role corresponds to the stream field.
            UNION_SET(aidl, stream, VALUE_OR_RETURN(
                    legacy2aidl_audio_stream_type_t_AudioStreamType(legacy.stream)));
            break;
        case AUDIO_PORT_ROLE_SINK:
            // This is not a bug. A SINK role corresponds to the source field.
            UNION_SET(aidl, source,
                      VALUE_OR_RETURN(legacy2aidl_audio_source_t_AudioSourceType(legacy.source)));
            break;
        default:
            LOG_ALWAYS_FATAL("Shouldn't get here");
    }
    return aidl;
}

ConversionResult<audio_port_config_mix_ext> aidl2legacy_AudioPortConfigMixExt(
        const media::AudioPortConfigMixExt& aidl, media::AudioPortRole role) {
    audio_port_config_mix_ext legacy;
    legacy.hw_module = VALUE_OR_RETURN(aidl2legacy_int32_t_audio_module_handle_t(aidl.hwModule));
    legacy.handle = VALUE_OR_RETURN(aidl2legacy_int32_t_audio_io_handle_t(aidl.handle));
    legacy.usecase = VALUE_OR_RETURN(aidl2legacy_AudioPortConfigMixExtUseCase(aidl.usecase, role));
    return legacy;
}

ConversionResult<media::AudioPortConfigMixExt> legacy2aidl_AudioPortConfigMixExt(
        const audio_port_config_mix_ext& legacy, audio_port_role_t role) {
    media::AudioPortConfigMixExt aidl;
    aidl.hwModule = VALUE_OR_RETURN(legacy2aidl_audio_module_handle_t_int32_t(legacy.hw_module));
    aidl.handle = VALUE_OR_RETURN(legacy2aidl_audio_io_handle_t_int32_t(legacy.handle));
    aidl.usecase = VALUE_OR_RETURN(legacy2aidl_AudioPortConfigMixExtUseCase(legacy.usecase, role));
    return aidl;
}

ConversionResult<audio_port_config_session_ext> aidl2legacy_AudioPortConfigSessionExt(
        const media::AudioPortConfigSessionExt& aidl) {
    audio_port_config_session_ext legacy;
    legacy.session = VALUE_OR_RETURN(aidl2legacy_int32_t_audio_session_t(aidl.session));
    return legacy;
}

ConversionResult<media::AudioPortConfigSessionExt> legacy2aidl_AudioPortConfigSessionExt(
        const audio_port_config_session_ext& legacy) {
    media::AudioPortConfigSessionExt aidl;
    aidl.session = VALUE_OR_RETURN(legacy2aidl_audio_session_t_int32_t(legacy.session));
    return aidl;
}

// This type is unnamed in the original definition, thus we name it here.
using audio_port_config_ext = decltype(audio_port_config::ext);

ConversionResult<audio_port_config_ext> aidl2legacy_AudioPortConfigExt(
        const media::AudioPortConfigExt& aidl, media::AudioPortType type,
        media::AudioPortRole role) {
    audio_port_config_ext legacy;
    // Our way of representing a union in AIDL is to have multiple vectors and require that at most
    // one of the them has size 1 and the rest are empty.
    switch (type) {
        case media::AudioPortType::NONE:
            // Just verify that the union is empty.
            VALUE_OR_RETURN(UNION_GET(aidl, nothing));
            break;
        case media::AudioPortType::DEVICE:
            legacy.device = VALUE_OR_RETURN(
                    aidl2legacy_AudioPortConfigDeviceExt(VALUE_OR_RETURN(UNION_GET(aidl, device))));
            break;
        case media::AudioPortType::MIX:
            legacy.mix = VALUE_OR_RETURN(
                    aidl2legacy_AudioPortConfigMixExt(VALUE_OR_RETURN(UNION_GET(aidl, mix)), role));
            break;
        case media::AudioPortType::SESSION:
            legacy.session = VALUE_OR_RETURN(aidl2legacy_AudioPortConfigSessionExt(
                    VALUE_OR_RETURN(UNION_GET(aidl, session))));
            break;
        default:
            LOG_ALWAYS_FATAL("Shouldn't get here");
    }
    return legacy;
}

ConversionResult<media::AudioPortConfigExt> legacy2aidl_AudioPortConfigExt(
        const audio_port_config_ext& legacy, audio_port_type_t type, audio_port_role_t role) {
    media::AudioPortConfigExt aidl;

    switch (type) {
        case AUDIO_PORT_TYPE_NONE:
            UNION_SET(aidl, nothing, false);
            break;
        case AUDIO_PORT_TYPE_DEVICE:
            UNION_SET(aidl, device,
                      VALUE_OR_RETURN(legacy2aidl_AudioPortConfigDeviceExt(legacy.device)));
            break;
        case AUDIO_PORT_TYPE_MIX:
            UNION_SET(aidl, mix,
                      VALUE_OR_RETURN(legacy2aidl_AudioPortConfigMixExt(legacy.mix, role)));
            break;
        case AUDIO_PORT_TYPE_SESSION:
            UNION_SET(aidl, session,
                      VALUE_OR_RETURN(legacy2aidl_AudioPortConfigSessionExt(legacy.session)));
            break;
        default:
            LOG_ALWAYS_FATAL("Shouldn't get here");
    }
    return aidl;
}

ConversionResult<audio_port_config> aidl2legacy_AudioPortConfig_audio_port_config(
        const media::AudioPortConfig& aidl) {
    audio_port_config legacy;
    legacy.id = VALUE_OR_RETURN(aidl2legacy_int32_t_audio_port_handle_t(aidl.id));
    legacy.role = VALUE_OR_RETURN(aidl2legacy_AudioPortRole_audio_port_role_t(aidl.role));
    legacy.type = VALUE_OR_RETURN(aidl2legacy_AudioPortType_audio_port_type_t(aidl.type));
    legacy.config_mask = VALUE_OR_RETURN(aidl2legacy_int32_t_config_mask(aidl.configMask));
    if (bitmaskIsSet(aidl.configMask, media::AudioPortConfigType::SAMPLE_RATE)) {
        legacy.sample_rate = VALUE_OR_RETURN(convertIntegral<unsigned int>(aidl.sampleRate));
    }
    if (bitmaskIsSet(aidl.configMask, media::AudioPortConfigType::CHANNEL_MASK)) {
        legacy.channel_mask =
                VALUE_OR_RETURN(aidl2legacy_int32_t_audio_channel_mask_t(aidl.channelMask));
    }
    if (bitmaskIsSet(aidl.configMask, media::AudioPortConfigType::FORMAT)) {
        legacy.format = VALUE_OR_RETURN(aidl2legacy_AudioFormat_audio_format_t(aidl.format));
    }
    if (bitmaskIsSet(aidl.configMask, media::AudioPortConfigType::GAIN)) {
        legacy.gain = VALUE_OR_RETURN(
                aidl2legacy_AudioGainConfig_audio_gain_config(aidl.gain, aidl.role, aidl.type));
    }
    if (bitmaskIsSet(aidl.configMask, media::AudioPortConfigType::FLAGS)) {
        legacy.flags = VALUE_OR_RETURN(
                aidl2legacy_AudioIoFlags_audio_io_flags(aidl.flags, aidl.role, aidl.type));
    }
    legacy.ext = VALUE_OR_RETURN(aidl2legacy_AudioPortConfigExt(aidl.ext, aidl.type, aidl.role));
    return legacy;
}

ConversionResult<media::AudioPortConfig> legacy2aidl_audio_port_config_AudioPortConfig(
        const audio_port_config& legacy) {
    media::AudioPortConfig aidl;
    aidl.id = VALUE_OR_RETURN(legacy2aidl_audio_port_handle_t_int32_t(legacy.id));
    aidl.role = VALUE_OR_RETURN(legacy2aidl_audio_port_role_t_AudioPortRole(legacy.role));
    aidl.type = VALUE_OR_RETURN(legacy2aidl_audio_port_type_t_AudioPortType(legacy.type));
    aidl.configMask = VALUE_OR_RETURN(legacy2aidl_config_mask_int32_t(legacy.config_mask));
    if (legacy.config_mask & AUDIO_PORT_CONFIG_SAMPLE_RATE) {
        aidl.sampleRate = VALUE_OR_RETURN(convertIntegral<int32_t>(legacy.sample_rate));
    }
    if (legacy.config_mask & AUDIO_PORT_CONFIG_CHANNEL_MASK) {
        aidl.channelMask =
                VALUE_OR_RETURN(legacy2aidl_audio_channel_mask_t_int32_t(legacy.channel_mask));
    }
    if (legacy.config_mask & AUDIO_PORT_CONFIG_FORMAT) {
        aidl.format = VALUE_OR_RETURN(legacy2aidl_audio_format_t_AudioFormat(legacy.format));
    }
    if (legacy.config_mask & AUDIO_PORT_CONFIG_GAIN) {
        aidl.gain = VALUE_OR_RETURN(legacy2aidl_audio_gain_config_AudioGainConfig(
                legacy.gain, legacy.role, legacy.type));
    }
    if (legacy.config_mask & AUDIO_PORT_CONFIG_FLAGS) {
        aidl.flags = VALUE_OR_RETURN(
                legacy2aidl_audio_io_flags_AudioIoFlags(legacy.flags, legacy.role, legacy.type));
    }
    aidl.ext =
            VALUE_OR_RETURN(legacy2aidl_AudioPortConfigExt(legacy.ext, legacy.type, legacy.role));
    return aidl;
}

ConversionResult<struct audio_patch> aidl2legacy_AudioPatch_audio_patch(
        const media::AudioPatch& aidl) {
    struct audio_patch legacy;
    legacy.id = VALUE_OR_RETURN(aidl2legacy_int32_t_audio_patch_handle_t(aidl.id));
    legacy.num_sinks = VALUE_OR_RETURN(convertIntegral<unsigned int>(aidl.sinks.size()));
    if (legacy.num_sinks > AUDIO_PATCH_PORTS_MAX) {
        return unexpected(BAD_VALUE);
    }
    for (size_t i = 0; i < legacy.num_sinks; ++i) {
        legacy.sinks[i] =
                VALUE_OR_RETURN(aidl2legacy_AudioPortConfig_audio_port_config(aidl.sinks[i]));
    }
    legacy.num_sources = VALUE_OR_RETURN(convertIntegral<unsigned int>(aidl.sources.size()));
    if (legacy.num_sources > AUDIO_PATCH_PORTS_MAX) {
        return unexpected(BAD_VALUE);
    }
    for (size_t i = 0; i < legacy.num_sources; ++i) {
        legacy.sources[i] =
                VALUE_OR_RETURN(aidl2legacy_AudioPortConfig_audio_port_config(aidl.sources[i]));
    }
    return legacy;
}

ConversionResult<media::AudioPatch> legacy2aidl_audio_patch_AudioPatch(
        const struct audio_patch& legacy) {
    media::AudioPatch aidl;
    aidl.id = VALUE_OR_RETURN(legacy2aidl_audio_patch_handle_t_int32_t(legacy.id));

    if (legacy.num_sinks > AUDIO_PATCH_PORTS_MAX) {
        return unexpected(BAD_VALUE);
    }
    for (unsigned int i = 0; i < legacy.num_sinks; ++i) {
        aidl.sinks.push_back(
                VALUE_OR_RETURN(legacy2aidl_audio_port_config_AudioPortConfig(legacy.sinks[i])));
    }
    if (legacy.num_sources > AUDIO_PATCH_PORTS_MAX) {
        return unexpected(BAD_VALUE);
    }
    for (unsigned int i = 0; i < legacy.num_sources; ++i) {
        aidl.sources.push_back(
                VALUE_OR_RETURN(legacy2aidl_audio_port_config_AudioPortConfig(legacy.sources[i])));
    }
    return aidl;
}

ConversionResult<sp<AudioIoDescriptor>> aidl2legacy_AudioIoDescriptor_AudioIoDescriptor(
        const media::AudioIoDescriptor& aidl) {
    sp<AudioIoDescriptor> legacy(new AudioIoDescriptor());
    legacy->mIoHandle = VALUE_OR_RETURN(aidl2legacy_int32_t_audio_io_handle_t(aidl.ioHandle));
    legacy->mPatch = VALUE_OR_RETURN(aidl2legacy_AudioPatch_audio_patch(aidl.patch));
    legacy->mSamplingRate = VALUE_OR_RETURN(convertIntegral<uint32_t>(aidl.samplingRate));
    legacy->mFormat = VALUE_OR_RETURN(aidl2legacy_AudioFormat_audio_format_t(aidl.format));
    legacy->mChannelMask =
            VALUE_OR_RETURN(aidl2legacy_int32_t_audio_channel_mask_t(aidl.channelMask));
    legacy->mFrameCount = VALUE_OR_RETURN(convertIntegral<size_t>(aidl.frameCount));
    legacy->mFrameCountHAL = VALUE_OR_RETURN(convertIntegral<size_t>(aidl.frameCountHAL));
    legacy->mLatency = VALUE_OR_RETURN(convertIntegral<uint32_t>(aidl.latency));
    legacy->mPortId = VALUE_OR_RETURN(aidl2legacy_int32_t_audio_port_handle_t(aidl.portId));
    return legacy;
}

ConversionResult<media::AudioIoDescriptor> legacy2aidl_AudioIoDescriptor_AudioIoDescriptor(
        const sp<AudioIoDescriptor>& legacy) {
    media::AudioIoDescriptor aidl;
    aidl.ioHandle = VALUE_OR_RETURN(legacy2aidl_audio_io_handle_t_int32_t(legacy->mIoHandle));
    aidl.patch = VALUE_OR_RETURN(legacy2aidl_audio_patch_AudioPatch(legacy->mPatch));
    aidl.samplingRate = VALUE_OR_RETURN(convertIntegral<int32_t>(legacy->mSamplingRate));
    aidl.format = VALUE_OR_RETURN(legacy2aidl_audio_format_t_AudioFormat(legacy->mFormat));
    aidl.channelMask = VALUE_OR_RETURN(
            legacy2aidl_audio_channel_mask_t_int32_t(legacy->mChannelMask));
    aidl.frameCount = VALUE_OR_RETURN(convertIntegral<int64_t>(legacy->mFrameCount));
    aidl.frameCountHAL = VALUE_OR_RETURN(convertIntegral<int64_t>(legacy->mFrameCountHAL));
    aidl.latency = VALUE_OR_RETURN(convertIntegral<int32_t>(legacy->mLatency));
    aidl.portId = VALUE_OR_RETURN(legacy2aidl_audio_port_handle_t_int32_t(legacy->mPortId));
    return aidl;
}

ConversionResult<AudioClient> aidl2legacy_AudioClient(const media::AudioClient& aidl) {
    AudioClient legacy;
    legacy.clientUid = VALUE_OR_RETURN(aidl2legacy_int32_t_uid_t(aidl.clientUid));
    legacy.clientPid = VALUE_OR_RETURN(aidl2legacy_int32_t_pid_t(aidl.clientPid));
    legacy.clientTid = VALUE_OR_RETURN(aidl2legacy_int32_t_pid_t(aidl.clientTid));
    legacy.packageName = VALUE_OR_RETURN(aidl2legacy_string_view_String16(aidl.packageName));
    return legacy;
}

ConversionResult<media::AudioClient> legacy2aidl_AudioClient(const AudioClient& legacy) {
    media::AudioClient aidl;
    aidl.clientUid = VALUE_OR_RETURN(legacy2aidl_uid_t_int32_t(legacy.clientUid));
    aidl.clientPid = VALUE_OR_RETURN(legacy2aidl_pid_t_int32_t(legacy.clientPid));
    aidl.clientTid = VALUE_OR_RETURN(legacy2aidl_pid_t_int32_t(legacy.clientTid));
    aidl.packageName = VALUE_OR_RETURN(legacy2aidl_String16_string(legacy.packageName));
    return aidl;
}

ConversionResult<audio_content_type_t>
aidl2legacy_AudioContentType_audio_content_type_t(media::AudioContentType aidl) {
    switch (aidl) {
        case media::AudioContentType::UNKNOWN:
            return AUDIO_CONTENT_TYPE_UNKNOWN;
        case media::AudioContentType::SPEECH:
            return AUDIO_CONTENT_TYPE_SPEECH;
        case media::AudioContentType::MUSIC:
            return AUDIO_CONTENT_TYPE_MUSIC;
        case media::AudioContentType::MOVIE:
            return AUDIO_CONTENT_TYPE_MOVIE;
        case media::AudioContentType::SONIFICATION:
            return AUDIO_CONTENT_TYPE_SONIFICATION;
    }
    return unexpected(BAD_VALUE);
}

ConversionResult<media::AudioContentType>
legacy2aidl_audio_content_type_t_AudioContentType(audio_content_type_t legacy) {
    switch (legacy) {
        case AUDIO_CONTENT_TYPE_UNKNOWN:
            return media::AudioContentType::UNKNOWN;
        case AUDIO_CONTENT_TYPE_SPEECH:
            return media::AudioContentType::SPEECH;
        case AUDIO_CONTENT_TYPE_MUSIC:
            return media::AudioContentType::MUSIC;
        case AUDIO_CONTENT_TYPE_MOVIE:
            return media::AudioContentType::MOVIE;
        case AUDIO_CONTENT_TYPE_SONIFICATION:
            return media::AudioContentType::SONIFICATION;
    }
    return unexpected(BAD_VALUE);
}

ConversionResult<audio_usage_t>
aidl2legacy_AudioUsage_audio_usage_t(media::AudioUsage aidl) {
    switch (aidl) {
        case media::AudioUsage::UNKNOWN:
            return AUDIO_USAGE_UNKNOWN;
        case media::AudioUsage::MEDIA:
            return AUDIO_USAGE_MEDIA;
        case media::AudioUsage::VOICE_COMMUNICATION:
            return AUDIO_USAGE_VOICE_COMMUNICATION;
        case media::AudioUsage::VOICE_COMMUNICATION_SIGNALLING:
            return AUDIO_USAGE_VOICE_COMMUNICATION_SIGNALLING;
        case media::AudioUsage::ALARM:
            return AUDIO_USAGE_ALARM;
        case media::AudioUsage::NOTIFICATION:
            return AUDIO_USAGE_NOTIFICATION;
        case media::AudioUsage::NOTIFICATION_TELEPHONY_RINGTONE:
            return AUDIO_USAGE_NOTIFICATION_TELEPHONY_RINGTONE;
        case media::AudioUsage::NOTIFICATION_COMMUNICATION_REQUEST:
            return AUDIO_USAGE_NOTIFICATION_COMMUNICATION_REQUEST;
        case media::AudioUsage::NOTIFICATION_COMMUNICATION_INSTANT:
            return AUDIO_USAGE_NOTIFICATION_COMMUNICATION_INSTANT;
        case media::AudioUsage::NOTIFICATION_COMMUNICATION_DELAYED:
            return AUDIO_USAGE_NOTIFICATION_COMMUNICATION_DELAYED;
        case media::AudioUsage::NOTIFICATION_EVENT:
            return AUDIO_USAGE_NOTIFICATION_EVENT;
        case media::AudioUsage::ASSISTANCE_ACCESSIBILITY:
            return AUDIO_USAGE_ASSISTANCE_ACCESSIBILITY;
        case media::AudioUsage::ASSISTANCE_NAVIGATION_GUIDANCE:
            return AUDIO_USAGE_ASSISTANCE_NAVIGATION_GUIDANCE;
        case media::AudioUsage::ASSISTANCE_SONIFICATION:
            return AUDIO_USAGE_ASSISTANCE_SONIFICATION;
        case media::AudioUsage::GAME:
            return AUDIO_USAGE_GAME;
        case media::AudioUsage::VIRTUAL_SOURCE:
            return AUDIO_USAGE_VIRTUAL_SOURCE;
        case media::AudioUsage::ASSISTANT:
            return AUDIO_USAGE_ASSISTANT;
        case media::AudioUsage::CALL_ASSISTANT:
            return AUDIO_USAGE_CALL_ASSISTANT;
        case media::AudioUsage::EMERGENCY:
            return AUDIO_USAGE_EMERGENCY;
        case media::AudioUsage::SAFETY:
            return AUDIO_USAGE_SAFETY;
        case media::AudioUsage::VEHICLE_STATUS:
            return AUDIO_USAGE_VEHICLE_STATUS;
        case media::AudioUsage::ANNOUNCEMENT:
            return AUDIO_USAGE_ANNOUNCEMENT;
    }
    return unexpected(BAD_VALUE);
}

ConversionResult<media::AudioUsage>
legacy2aidl_audio_usage_t_AudioUsage(audio_usage_t legacy) {
    switch (legacy) {
        case AUDIO_USAGE_UNKNOWN:
            return media::AudioUsage::UNKNOWN;
        case AUDIO_USAGE_MEDIA:
            return media::AudioUsage::MEDIA;
        case AUDIO_USAGE_VOICE_COMMUNICATION:
            return media::AudioUsage::VOICE_COMMUNICATION;
        case AUDIO_USAGE_VOICE_COMMUNICATION_SIGNALLING:
            return media::AudioUsage::VOICE_COMMUNICATION_SIGNALLING;
        case AUDIO_USAGE_ALARM:
            return media::AudioUsage::ALARM;
        case AUDIO_USAGE_NOTIFICATION:
            return media::AudioUsage::NOTIFICATION;
        case AUDIO_USAGE_NOTIFICATION_TELEPHONY_RINGTONE:
            return media::AudioUsage::NOTIFICATION_TELEPHONY_RINGTONE;
        case AUDIO_USAGE_NOTIFICATION_COMMUNICATION_REQUEST:
            return media::AudioUsage::NOTIFICATION_COMMUNICATION_REQUEST;
        case AUDIO_USAGE_NOTIFICATION_COMMUNICATION_INSTANT:
            return media::AudioUsage::NOTIFICATION_COMMUNICATION_INSTANT;
        case AUDIO_USAGE_NOTIFICATION_COMMUNICATION_DELAYED:
            return media::AudioUsage::NOTIFICATION_COMMUNICATION_DELAYED;
        case AUDIO_USAGE_NOTIFICATION_EVENT:
            return media::AudioUsage::NOTIFICATION_EVENT;
        case AUDIO_USAGE_ASSISTANCE_ACCESSIBILITY:
            return media::AudioUsage::ASSISTANCE_ACCESSIBILITY;
        case AUDIO_USAGE_ASSISTANCE_NAVIGATION_GUIDANCE:
            return media::AudioUsage::ASSISTANCE_NAVIGATION_GUIDANCE;
        case AUDIO_USAGE_ASSISTANCE_SONIFICATION:
            return media::AudioUsage::ASSISTANCE_SONIFICATION;
        case AUDIO_USAGE_GAME:
            return media::AudioUsage::GAME;
        case AUDIO_USAGE_VIRTUAL_SOURCE:
            return media::AudioUsage::VIRTUAL_SOURCE;
        case AUDIO_USAGE_ASSISTANT:
            return media::AudioUsage::ASSISTANT;
        case AUDIO_USAGE_CALL_ASSISTANT:
            return media::AudioUsage::CALL_ASSISTANT;
        case AUDIO_USAGE_EMERGENCY:
            return media::AudioUsage::EMERGENCY;
        case AUDIO_USAGE_SAFETY:
            return media::AudioUsage::SAFETY;
        case AUDIO_USAGE_VEHICLE_STATUS:
            return media::AudioUsage::VEHICLE_STATUS;
        case AUDIO_USAGE_ANNOUNCEMENT:
            return media::AudioUsage::ANNOUNCEMENT;
    }
    return unexpected(BAD_VALUE);
}

ConversionResult<audio_flags_mask_t>
aidl2legacy_AudioFlag_audio_flags_mask_t(media::AudioFlag aidl) {
    switch (aidl) {
        case media::AudioFlag::AUDIBILITY_ENFORCED:
            return AUDIO_FLAG_AUDIBILITY_ENFORCED;
        case media::AudioFlag::SECURE:
            return AUDIO_FLAG_SECURE;
        case media::AudioFlag::SCO:
            return AUDIO_FLAG_SCO;
        case media::AudioFlag::BEACON:
            return AUDIO_FLAG_BEACON;
        case media::AudioFlag::HW_AV_SYNC:
            return AUDIO_FLAG_HW_AV_SYNC;
        case media::AudioFlag::HW_HOTWORD:
            return AUDIO_FLAG_HW_HOTWORD;
        case media::AudioFlag::BYPASS_INTERRUPTION_POLICY:
            return AUDIO_FLAG_BYPASS_INTERRUPTION_POLICY;
        case media::AudioFlag::BYPASS_MUTE:
            return AUDIO_FLAG_BYPASS_MUTE;
        case media::AudioFlag::LOW_LATENCY:
            return AUDIO_FLAG_LOW_LATENCY;
        case media::AudioFlag::DEEP_BUFFER:
            return AUDIO_FLAG_DEEP_BUFFER;
        case media::AudioFlag::NO_MEDIA_PROJECTION:
            return AUDIO_FLAG_NO_MEDIA_PROJECTION;
        case media::AudioFlag::MUTE_HAPTIC:
            return AUDIO_FLAG_MUTE_HAPTIC;
        case media::AudioFlag::NO_SYSTEM_CAPTURE:
            return AUDIO_FLAG_NO_SYSTEM_CAPTURE;
        case media::AudioFlag::CAPTURE_PRIVATE:
            return AUDIO_FLAG_CAPTURE_PRIVATE;
    }
    return unexpected(BAD_VALUE);
}

ConversionResult<media::AudioFlag>
legacy2aidl_audio_flags_mask_t_AudioFlag(audio_flags_mask_t legacy) {
    switch (legacy) {
        case AUDIO_FLAG_NONE:
            return unexpected(BAD_VALUE);
        case AUDIO_FLAG_AUDIBILITY_ENFORCED:
            return media::AudioFlag::AUDIBILITY_ENFORCED;
        case AUDIO_FLAG_SECURE:
            return media::AudioFlag::SECURE;
        case AUDIO_FLAG_SCO:
            return media::AudioFlag::SCO;
        case AUDIO_FLAG_BEACON:
            return media::AudioFlag::BEACON;
        case AUDIO_FLAG_HW_AV_SYNC:
            return media::AudioFlag::HW_AV_SYNC;
        case AUDIO_FLAG_HW_HOTWORD:
            return media::AudioFlag::HW_HOTWORD;
        case AUDIO_FLAG_BYPASS_INTERRUPTION_POLICY:
            return media::AudioFlag::BYPASS_INTERRUPTION_POLICY;
        case AUDIO_FLAG_BYPASS_MUTE:
            return media::AudioFlag::BYPASS_MUTE;
        case AUDIO_FLAG_LOW_LATENCY:
            return media::AudioFlag::LOW_LATENCY;
        case AUDIO_FLAG_DEEP_BUFFER:
            return media::AudioFlag::DEEP_BUFFER;
        case AUDIO_FLAG_NO_MEDIA_PROJECTION:
            return media::AudioFlag::NO_MEDIA_PROJECTION;
        case AUDIO_FLAG_MUTE_HAPTIC:
            return media::AudioFlag::MUTE_HAPTIC;
        case AUDIO_FLAG_NO_SYSTEM_CAPTURE:
            return media::AudioFlag::NO_SYSTEM_CAPTURE;
        case AUDIO_FLAG_CAPTURE_PRIVATE:
            return media::AudioFlag::CAPTURE_PRIVATE;
    }
    return unexpected(BAD_VALUE);
}

ConversionResult<audio_flags_mask_t>
aidl2legacy_int32_t_audio_flags_mask_t_mask(int32_t aidl) {
    return convertBitmask<audio_flags_mask_t, int32_t, audio_flags_mask_t, media::AudioFlag>(
            aidl, aidl2legacy_AudioFlag_audio_flags_mask_t, index2enum_index<media::AudioFlag>,
            enumToMask_bitmask<audio_flags_mask_t, audio_flags_mask_t>);
}

ConversionResult<int32_t>
legacy2aidl_audio_flags_mask_t_int32_t_mask(audio_flags_mask_t legacy) {
    return convertBitmask<int32_t, audio_flags_mask_t, media::AudioFlag, audio_flags_mask_t>(
            legacy, legacy2aidl_audio_flags_mask_t_AudioFlag,
            index2enum_bitmask<audio_flags_mask_t>,
            enumToMask_index<int32_t, media::AudioFlag>);
}

ConversionResult<audio_attributes_t>
aidl2legacy_AudioAttributesInternal_audio_attributes_t(const media::AudioAttributesInternal& aidl) {
    audio_attributes_t legacy;
    legacy.content_type = VALUE_OR_RETURN(
            aidl2legacy_AudioContentType_audio_content_type_t(aidl.contentType));
    legacy.usage = VALUE_OR_RETURN(aidl2legacy_AudioUsage_audio_usage_t(aidl.usage));
    legacy.source = VALUE_OR_RETURN(aidl2legacy_AudioSourceType_audio_source_t(aidl.source));
    legacy.flags = VALUE_OR_RETURN(aidl2legacy_int32_t_audio_flags_mask_t_mask(aidl.flags));
    RETURN_IF_ERROR(aidl2legacy_string(aidl.tags, legacy.tags, sizeof(legacy.tags)));
    return legacy;
}

ConversionResult<media::AudioAttributesInternal>
legacy2aidl_audio_attributes_t_AudioAttributesInternal(const audio_attributes_t& legacy) {
    media::AudioAttributesInternal aidl;
    aidl.contentType = VALUE_OR_RETURN(
            legacy2aidl_audio_content_type_t_AudioContentType(legacy.content_type));
    aidl.usage = VALUE_OR_RETURN(legacy2aidl_audio_usage_t_AudioUsage(legacy.usage));
    aidl.source = VALUE_OR_RETURN(legacy2aidl_audio_source_t_AudioSourceType(legacy.source));
    aidl.flags = VALUE_OR_RETURN(legacy2aidl_audio_flags_mask_t_int32_t_mask(legacy.flags));
    aidl.tags = VALUE_OR_RETURN(legacy2aidl_string(legacy.tags, sizeof(legacy.tags)));
    return aidl;
}

ConversionResult<audio_encapsulation_mode_t>
aidl2legacy_audio_encapsulation_mode_t_AudioEncapsulationMode(media::AudioEncapsulationMode aidl) {
    switch (aidl) {
        case media::AudioEncapsulationMode::NONE:
            return AUDIO_ENCAPSULATION_MODE_NONE;
        case media::AudioEncapsulationMode::ELEMENTARY_STREAM:
            return AUDIO_ENCAPSULATION_MODE_ELEMENTARY_STREAM;
        case media::AudioEncapsulationMode::HANDLE:
            return AUDIO_ENCAPSULATION_MODE_HANDLE;
    }
    return unexpected(BAD_VALUE);
}

ConversionResult<media::AudioEncapsulationMode>
legacy2aidl_AudioEncapsulationMode_audio_encapsulation_mode_t(audio_encapsulation_mode_t legacy) {
    switch (legacy) {
        case AUDIO_ENCAPSULATION_MODE_NONE:
            return media::AudioEncapsulationMode::NONE;
        case AUDIO_ENCAPSULATION_MODE_ELEMENTARY_STREAM:
            return media::AudioEncapsulationMode::ELEMENTARY_STREAM;
        case AUDIO_ENCAPSULATION_MODE_HANDLE:
            return media::AudioEncapsulationMode::HANDLE;
    }
    return unexpected(BAD_VALUE);
}

ConversionResult<audio_offload_info_t>
aidl2legacy_AudioOffloadInfo_audio_offload_info_t(const media::AudioOffloadInfo& aidl) {
    audio_offload_info_t legacy;
    legacy.version = VALUE_OR_RETURN(convertIntegral<uint16_t>(aidl.version));
    legacy.size = sizeof(audio_offload_info_t);
    audio_config_base_t config = VALUE_OR_RETURN(
            aidl2legacy_AudioConfigBase_audio_config_base_t(aidl.config));
    legacy.sample_rate = config.sample_rate;
    legacy.channel_mask = config.channel_mask;
    legacy.format = config.format;
    legacy.stream_type = VALUE_OR_RETURN(
            aidl2legacy_AudioStreamType_audio_stream_type_t(aidl.streamType));
    legacy.bit_rate = VALUE_OR_RETURN(convertIntegral<uint32_t>(aidl.bitRate));
    legacy.duration_us = VALUE_OR_RETURN(convertIntegral<int64_t>(aidl.durationUs));
    legacy.has_video = aidl.hasVideo;
    legacy.is_streaming = aidl.isStreaming;
    legacy.bit_width = VALUE_OR_RETURN(convertIntegral<uint32_t>(aidl.bitWidth));
    legacy.offload_buffer_size = VALUE_OR_RETURN(convertIntegral<uint32_t>(aidl.offloadBufferSize));
    legacy.usage = VALUE_OR_RETURN(aidl2legacy_AudioUsage_audio_usage_t(aidl.usage));
    legacy.encapsulation_mode = VALUE_OR_RETURN(
            aidl2legacy_audio_encapsulation_mode_t_AudioEncapsulationMode(aidl.encapsulationMode));
    legacy.content_id = VALUE_OR_RETURN(convertReinterpret<int32_t>(aidl.contentId));
    legacy.sync_id = VALUE_OR_RETURN(convertReinterpret<int32_t>(aidl.syncId));
    return legacy;
}

ConversionResult<media::AudioOffloadInfo>
legacy2aidl_audio_offload_info_t_AudioOffloadInfo(const audio_offload_info_t& legacy) {
    media::AudioOffloadInfo aidl;
    // Version 0.1 fields.
    if (legacy.size < offsetof(audio_offload_info_t, usage) + sizeof(audio_offload_info_t::usage)) {
        return unexpected(BAD_VALUE);
    }
    aidl.version = VALUE_OR_RETURN(convertIntegral<int32_t>(legacy.version));
    aidl.config.sampleRate = VALUE_OR_RETURN(convertIntegral<int32_t>(legacy.sample_rate));
    aidl.config.channelMask = VALUE_OR_RETURN(
            legacy2aidl_audio_channel_mask_t_int32_t(legacy.channel_mask));
    aidl.config.format = VALUE_OR_RETURN(legacy2aidl_audio_format_t_AudioFormat(legacy.format));
    aidl.streamType = VALUE_OR_RETURN(
            legacy2aidl_audio_stream_type_t_AudioStreamType(legacy.stream_type));
    aidl.bitRate = VALUE_OR_RETURN(convertIntegral<int32_t>(legacy.bit_rate));
    aidl.durationUs = VALUE_OR_RETURN(convertIntegral<int64_t>(legacy.duration_us));
    aidl.hasVideo = legacy.has_video;
    aidl.isStreaming = legacy.is_streaming;
    aidl.bitWidth = VALUE_OR_RETURN(convertIntegral<int32_t>(legacy.bit_width));
    aidl.offloadBufferSize = VALUE_OR_RETURN(convertIntegral<int32_t>(legacy.offload_buffer_size));
    aidl.usage = VALUE_OR_RETURN(legacy2aidl_audio_usage_t_AudioUsage(legacy.usage));

    // Version 0.2 fields.
    if (legacy.version >= AUDIO_OFFLOAD_INFO_VERSION_0_2) {
        if (legacy.size <
            offsetof(audio_offload_info_t, sync_id) + sizeof(audio_offload_info_t::sync_id)) {
            return unexpected(BAD_VALUE);
        }
        aidl.encapsulationMode = VALUE_OR_RETURN(
                legacy2aidl_AudioEncapsulationMode_audio_encapsulation_mode_t(
                        legacy.encapsulation_mode));
        aidl.contentId = VALUE_OR_RETURN(convertReinterpret<int32_t>(legacy.content_id));
        aidl.syncId = VALUE_OR_RETURN(convertReinterpret<int32_t>(legacy.sync_id));
    }
    return aidl;
}

ConversionResult<audio_config_t>
aidl2legacy_AudioConfig_audio_config_t(const media::AudioConfig& aidl) {
    audio_config_t legacy;
    legacy.sample_rate = VALUE_OR_RETURN(convertIntegral<uint32_t>(aidl.sampleRate));
    legacy.channel_mask = VALUE_OR_RETURN(
            aidl2legacy_int32_t_audio_channel_mask_t(aidl.channelMask));
    legacy.format = VALUE_OR_RETURN(aidl2legacy_AudioFormat_audio_format_t(aidl.format));
    legacy.offload_info = VALUE_OR_RETURN(
            aidl2legacy_AudioOffloadInfo_audio_offload_info_t(aidl.offloadInfo));
    legacy.frame_count = VALUE_OR_RETURN(convertIntegral<uint32_t>(aidl.frameCount));
    return legacy;
}

ConversionResult<media::AudioConfig>
legacy2aidl_audio_config_t_AudioConfig(const audio_config_t& legacy) {
    media::AudioConfig aidl;
    aidl.sampleRate = VALUE_OR_RETURN(convertIntegral<int32_t>(legacy.sample_rate));
    aidl.channelMask = VALUE_OR_RETURN(
            legacy2aidl_audio_channel_mask_t_int32_t(legacy.channel_mask));
    aidl.format = VALUE_OR_RETURN(legacy2aidl_audio_format_t_AudioFormat(legacy.format));
    aidl.offloadInfo = VALUE_OR_RETURN(
            legacy2aidl_audio_offload_info_t_AudioOffloadInfo(legacy.offload_info));
    aidl.frameCount = VALUE_OR_RETURN(convertIntegral<int64_t>(legacy.frame_count));
    return aidl;
}

ConversionResult<audio_config_base_t>
aidl2legacy_AudioConfigBase_audio_config_base_t(const media::AudioConfigBase& aidl) {
    audio_config_base_t legacy;
    legacy.sample_rate = VALUE_OR_RETURN(convertIntegral<uint32_t>(aidl.sampleRate));
    legacy.channel_mask = VALUE_OR_RETURN(
            aidl2legacy_int32_t_audio_channel_mask_t(aidl.channelMask));
    legacy.format = VALUE_OR_RETURN(aidl2legacy_AudioFormat_audio_format_t(aidl.format));
    return legacy;
}

ConversionResult<media::AudioConfigBase>
legacy2aidl_audio_config_base_t_AudioConfigBase(const audio_config_base_t& legacy) {
    media::AudioConfigBase aidl;
    aidl.sampleRate = VALUE_OR_RETURN(convertIntegral<int32_t>(legacy.sample_rate));
    aidl.channelMask = VALUE_OR_RETURN(
            legacy2aidl_audio_channel_mask_t_int32_t(legacy.channel_mask));
    aidl.format = VALUE_OR_RETURN(legacy2aidl_audio_format_t_AudioFormat(legacy.format));
    return aidl;
}

ConversionResult<sp<IMemory>>
aidl2legacy_SharedFileRegion_IMemory(const media::SharedFileRegion& aidl) {
    sp<IMemory> legacy;
    if (!convertSharedFileRegionToIMemory(aidl, &legacy)) {
        return unexpected(BAD_VALUE);
    }
    return legacy;
}

ConversionResult<media::SharedFileRegion>
legacy2aidl_IMemory_SharedFileRegion(const sp<IMemory>& legacy) {
    media::SharedFileRegion aidl;
    if (!convertIMemoryToSharedFileRegion(legacy, &aidl)) {
        return unexpected(BAD_VALUE);
    }
    return aidl;
}

ConversionResult<sp<IMemory>>
aidl2legacy_NullableSharedFileRegion_IMemory(const std::optional<media::SharedFileRegion>& aidl) {
    sp<IMemory> legacy;
    if (!convertNullableSharedFileRegionToIMemory(aidl, &legacy)) {
        return unexpected(BAD_VALUE);
    }
    return legacy;
}

ConversionResult<std::optional<media::SharedFileRegion>>
legacy2aidl_NullableIMemory_SharedFileRegion(const sp<IMemory>& legacy) {
    std::optional<media::SharedFileRegion> aidl;
    if (!convertNullableIMemoryToSharedFileRegion(legacy, &aidl)) {
        return unexpected(BAD_VALUE);
    }
    return aidl;
}

}  // namespace android
