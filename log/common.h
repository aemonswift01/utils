#include <memory>
#include "logger.h"

namespace utils::log {
// siezeof(enum) =4 ,sizeof( enum: char)= 1
enum class InfoLogLevel : unsigned char {

    DEBUG_LEVEL = 0,
    INFO_LEVEL,
    WARN_LEVEL,
    ERROR_LEVEL,
    FATAL_LEVEL,
    HEADER_LEVEL,
    NUM_INFO_LOG_LEVELS,
};

void Log(const InfoLogLevel log_level, Logger* logger, const char* format,
         ...) {}

}  // namespace utils::log