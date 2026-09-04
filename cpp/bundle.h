// bundle.h — mirrors lib/bundle.mjs.
#ifndef pocev_BUNDLE_H
#define pocev_BUNDLE_H

#include "jsjson.h"

#include <string>

namespace pocev {

struct BundleResult {
    std::string out;
    Json manifest;
};

BundleResult buildBundle(const std::string& cwd, const std::string& captureId,
                         const std::string& outDir);

}  // namespace pocev

#endif  // pocev_BUNDLE_H
