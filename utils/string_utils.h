#include <string.h>
#include <string>

namespace utils::utils {

std::string errnoStr(int err) {
    char buf[1024];
    buf[0] = '\0';
    std::string result;
    result.assign(strerror_r(err, buf, sizeof(buf)));
    return result;
}

}  // namespace utils::utils