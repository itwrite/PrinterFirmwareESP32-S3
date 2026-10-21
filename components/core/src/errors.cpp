// ============================================================================
// errors.cpp —— 错误码唯一字符串映射
// ============================================================================
#include "core/errors.h"

namespace ijpd {

const char* err_name(Err e) noexcept {
    switch (e) {
        case Err::OK:                           return "OK";
        case Err::INVALID_JSON:                 return "INVALID_JSON";
        case Err::INVALID_ARGUMENT:             return "INVALID_ARGUMENT";
        case Err::UNKNOWN_COMMAND:              return "UNKNOWN_COMMAND";
        case Err::FRAME_TOO_LARGE:              return "FRAME_TOO_LARGE";
        case Err::FRAME_TIMEOUT:                return "FRAME_TIMEOUT";
        case Err::PACKET_OUT_OF_ORDER:          return "PACKET_OUT_OF_ORDER";
        case Err::PACKET_LENGTH_INVALID:        return "PACKET_LENGTH_INVALID";
        case Err::PACKET_CRC_MISMATCH:          return "PACKET_CRC_MISMATCH";
        case Err::DECOMPRESSION_FAILED:         return "DECOMPRESSION_FAILED";
        case Err::DECOMPRESSED_LENGTH_MISMATCH: return "DECOMPRESSED_LENGTH_MISMATCH";
        case Err::QUEUE_TIMEOUT:                return "QUEUE_TIMEOUT";
        case Err::PSRAM_UNAVAILABLE:            return "PSRAM_UNAVAILABLE";
        case Err::OUT_OF_MEMORY:                return "OUT_OF_MEMORY";
        case Err::JOB_ALREADY_RUNNING:          return "JOB_ALREADY_RUNNING";
        case Err::NO_ACTIVE_JOB:                return "NO_ACTIVE_JOB";
        case Err::DATA_CONNECTION_LOST:         return "DATA_CONNECTION_LOST";
        case Err::DATA_HANDSHAKE_FAILED:        return "DATA_HANDSHAKE_FAILED";
        case Err::HAL_NOT_CONFIGURED:           return "HAL_NOT_CONFIGURED";
        case Err::HAL_FAULT:                    return "HAL_FAULT";
        case Err::UNSUPPORTED:                  return "UNSUPPORTED";
        case Err::JOB_CANCELLED:                return "JOB_CANCELLED";
        case Err::JOB_TIMEOUT:                  return "JOB_TIMEOUT";
        case Err::INTERNAL_ERROR:               return "INTERNAL_ERROR";
        case Err::STALE_GENERATION:             return "STALE_GENERATION";
        default:                                return "UNKNOWN";
    }
}

const char* err_default_message(Err e) noexcept {
    switch (e) {
        case Err::OK:                           return "ok";
        case Err::INVALID_JSON:                 return "malformed JSON or JSON subset constraint violated";
        case Err::INVALID_ARGUMENT:             return "invalid argument";
        case Err::UNKNOWN_COMMAND:              return "command is not in the whitelist";
        case Err::FRAME_TOO_LARGE:              return "control frame exceeds the maximum size";
        case Err::FRAME_TIMEOUT:                return "control frame reassembly timed out";
        case Err::PACKET_OUT_OF_ORDER:          return "packet index duplicated, out of order or missing";
        case Err::PACKET_LENGTH_INVALID:        return "packet length fields invalid or total bytes mismatch";
        case Err::PACKET_CRC_MISMATCH:          return "packet CRC32 check failed";
        case Err::DECOMPRESSION_FAILED:         return "LZ4 stream is corrupted";
        case Err::DECOMPRESSED_LENGTH_MISMATCH: return "decompressed length does not match expected length";
        case Err::QUEUE_TIMEOUT:                return "timed out waiting for slot or buffer";
        case Err::PSRAM_UNAVAILABLE:            return "PSRAM is unavailable or insufficient";
        case Err::OUT_OF_MEMORY:                return "memory allocation failed";
        case Err::JOB_ALREADY_RUNNING:          return "another job is already running";
        case Err::NO_ACTIVE_JOB:                return "no active job";
        case Err::DATA_CONNECTION_LOST:         return "data connection lost unexpectedly";
        case Err::DATA_HANDSHAKE_FAILED:        return "data connection handshake failed";
        case Err::HAL_NOT_CONFIGURED:           return "HAL is not configured";
        case Err::HAL_FAULT:                    return "HAL fault";
        case Err::UNSUPPORTED:                  return "capability is not supported by this device";
        case Err::JOB_CANCELLED:                return "job was cancelled";
        case Err::JOB_TIMEOUT:                  return "job timed out";
        case Err::INTERNAL_ERROR:               return "internal error";
        case Err::STALE_GENERATION:             return "slot submission belongs to a superseded generation";
        default:                                return "unknown error";
    }
}

}  // namespace ijpd
