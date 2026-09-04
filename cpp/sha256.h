// sha256.h — FIPS 180-4 SHA-256, matching Node's crypto.createHash('sha256').
#ifndef pocev_SHA256_H
#define pocev_SHA256_H

#include <cstdint>
#include <string>

namespace pocev {

// Hex digest (lowercase) of the byte string, like digest('hex').
std::string sha256Hex(const std::string& data);

}  // namespace pocev

#endif  // pocev_SHA256_H
