#pragma once

#include <sstream>
#include <atomic>

#include "airmap/panorama.h"
#include "airmap/logger.h"

namespace airmap {
namespace stitcher {

/**
 * @brief is a baseclass that can be put to the task of stitching a 360 Panorama.
 * @details Stitcher is cancellable, which means a single instance is single use.
 */
class Stitcher {
public:

    /**
     * @brief The Report struct conveys metatada about the stitched image
     * @details the stitcher has decisions to make (how much do we scale the input?)
     * and stats to collect (how many features were detected), while the caller may
     * be intested in knowing what these were. The stitcher may report them via this
     * structure.
     */
    struct Report {
        /**
         * @brief inputScaled - the fraction to which the input was scaled.
         *  Stitcher has RAM requirements and it may decide that it can only succeed
         *  with the RAM it is given if it scales the input images.
         */
        double inputScaled = 1.0;
        size_t inputSizeMB = 0;
    };

    /**
     * @brief The RetriableError class conveys a stitching error that can be retried on.
     * @details The process of stitching (esp. feature extraction and matching) can be
     * indeterministic.
     */
    class RetriableError : public std::invalid_argument {
    public:
        RetriableError(const std::string &what)
            : std::invalid_argument(what)
        {
        }
    };

    using SharedPtr = std::shared_ptr<Stitcher>;
    virtual ~Stitcher() = default;

    /**
     * @brief stitch is the Stitcher's main entry - subclasses should
     * fill with the actual stitching.
     */
    virtual Report stitch() = 0;
    virtual void cancel() = 0;
};

/**
 * @brief The RetryingStitcher class is such that can retry the stitching based on
 * Parameters::retries
 */
class RetryingStitcher : public Stitcher
{
public:
    RetryingStitcher(SharedPtr underlying,
                     const Panorama::Parameters &parameters,
                     std::shared_ptr<Logger> logger)
        : _underlying(underlying)
        , _retries(parameters.retries)
        , _logger(logger)
    {
    }

    Report stitch() override {
        for (size_t i = 0; i < _retries; i++) {
            try {
                return _underlying->stitch();
            } catch (const RetriableError &e) {
                if (_retries == 0) {
                    throw;
                }
                std::stringstream ss;
                ss << "Stitching failed with " << e.what() << ", retrying, retries left " << _retries - i;
                _logger->log(Logger::Severity::error, ss.str().c_str(), "stitcher");
            }
        }
        return _underlying->stitch();
    }
    void cancel() override {
        _retries = 0;
        _underlying->cancel();
    }

private:
    Stitcher::SharedPtr _underlying;
    std::atomic<size_t> _retries;
    std::shared_ptr<Logger> _logger;
};


} // namespace stitchar
} // namespace airmap
