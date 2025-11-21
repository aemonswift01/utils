#include "utils/random.h"
#include "port/likely.h"
#include "utils/aligned_storage.h"

#include <thread>

#define STORAGE_DECL static thread_local

namespace utils::utils {

Random* Random::GetTLSInstance() {
    STORAGE_DECL Random* tls_instance;
    STORAGE_DECL utils::AlignedMemory<sizeof(Random), alignof(Random)>
        tls_instance_bytes;

    auto rv = tls_instance;  //discard static/ thread_local
    if (UNLIKELY(rv == nullptr)) {
        size_t seed = std::hash<std::thread::id>()(std::this_thread::get_id());
        rv = new (&tls_instance_bytes) Random((uint32_t)seed);
        tls_instance = rv;
    }
    return rv;
}

std::string Random::HumanReadableString(int len) {
    std::string ret;
    ret.resize(len);
    for (int i = 0; i < len; ++i) {
        ret[i] = static_cast<char>('a' + Uniform(26));
    }
    return ret;
}

std::string Random::RandomString(int len) {
    std::string ret;
    ret.resize(len);
    for (int i = 0; i < len; i++) {
        ret[i] = static_cast<char>(' ' + Uniform(95));  // ' ' .. '~'
    }
    return ret;
}

std::string Random::RandomBinaryString(int len) {
    std::string ret;
    ret.resize(len);
    for (int i = 0; i < len; i++) {
        ret[i] = static_cast<char>(Uniform(CHAR_MAX));
    }
    return ret;
}
}  // namespace utils::utils