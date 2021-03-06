#include "airmap/opencv_stitcher.h"
#include "cropper.h"
#include "cubemap.h"

namespace airmap {
namespace stitcher {

//
//
// OpenCVStitcher
//
//
Stitcher::Report OpenCVStitcher::stitch()
{
    Stitcher::Report report;

    SourceImages source_images(_panorama, _logger);
    source_images.scaleToAvailableMemory(_parameters.memoryBudgetMB,
                                         _parameters.maxInputImageSize,
                                         report.inputSizeMB,
                                         report.inputScaled);

    cv::Mat result;
    cv::Ptr<cv::Stitcher> stitcher = cv::Stitcher::create(cv::Stitcher::PANORAMA);
    cv::Stitcher::Status status;
    try {
        status = stitcher->stitch(source_images.images, result);
    } catch (const std::exception &e) {
        // can indeed throw, e.g.:
        //.../OpenCV/modules/flann/src/miniflann.cpp:487: error: (-215:Assertion
        // failed) (size_t)knn <= index_->size() in function 'runKnnSearch_' but
        // that's
        // nothing we shouldn't want to retry on.
        throw RetriableError(e.what());
    }
    if (status != cv::Stitcher::OK) {
        std::stringstream ss;
        ss << "Can't stitch; ";
        if (status == cv::Stitcher::ERR_NEED_MORE_IMGS) {
            ss << "need more images";
            throw std::invalid_argument(ss.str());
        } else if (status == cv::Stitcher::ERR_HOMOGRAPHY_EST_FAIL) {
            ss << "ERR_HOMOGRAPHY_EST_FAIL";
        } else if (status == cv::Stitcher::ERR_CAMERA_PARAMS_ADJUST_FAIL) {
            ss << "ERR_CAMERA_PARAMS_ADJUST_FAIL";
        }
        throw RetriableError(ss.str());
    }

    postprocess(std::move(result));
    return report;
}

void OpenCVStitcher::cancel() { }

void OpenCVStitcher::postprocess(cv::Mat &&result)
{
    // Crop any null regions from the sides or bottoms.
    // This will also crop null regions from the sky too, but that will be added back in
    // below.
    if (_parameters.maximumCropRatio < 1.0) {
        result = Cropper {}.cropNullEdges(result, _parameters.maximumCropRatio);
    }

    // force the image to 2x1 aspect ratio (consistent with a full equirectangular-ly
    // projected sphere) by adding artificial sky.
    int leftPadding = result.cols % 2;
    int topPadding = (result.cols + leftPadding) / 2 - result.rows;
    if (topPadding >= 0) { // pad
        cv::copyMakeBorder(result, result, topPadding, 0, leftPadding, 0,
                           cv::BORDER_REPLICATE);
    } else { // crop
        int cols = result.cols - leftPadding;
        int rows = cols / 2;
        assert(result.rows > rows);
        result = result(cv::Rect { 0, result.rows - rows, cols, rows });
    }
    assert(result.rows == result.cols / 2);

    cv::imwrite(_outputPath, result);
    LOG(info) << "Written stitched image to " << _outputPath << std::endl;
    if (_parameters.alsoCreateCubeMap) {
        std::string base_path = (path(_outputPath).parent_path()
                                 / path(_outputPath).stem())
                                        .string();
        CubeMap::write(
                result,
                CubeMap::Paths { { CubeMap::Face::Front, base_path + ".front.jpg" },
                                 { CubeMap::Face::Right, base_path + ".right.jpg" },
                                 { CubeMap::Face::Back, base_path + ".back.jpg" },
                                 { CubeMap::Face::Left, base_path + ".left.jpg" },
                                 { CubeMap::Face::Top, base_path + ".top.jpg" },
                                 { CubeMap::Face::Bottom, base_path + ".bottom.jpg" } });
        LOG(info) << "Written cubemap of the stitched image to " << base_path
                  << std::endl;
    }
}

//! stitcher::stitcher class

LowLevelOpenCVStitcher::LowLevelOpenCVStitcher(const Configuration &config,
                                               const Panorama &panorama,
                                               const Panorama::Parameters &parameters,
                                               const std::string &outputPath,
                                               std::shared_ptr<Logger> logger,
                                               bool debug, path debugPath)
    : OpenCVStitcher(panorama, parameters, outputPath, logger, debug, debugPath)
    , config(config) {};

void LowLevelOpenCVStitcher::adjustCameraParameters(
        std::vector<cv::detail::ImageFeatures> &features,
        std::vector<cv::detail::MatchesInfo> &matches,
        std::vector<cv::detail::CameraParams> &cameras)
{
    LOG(debug) << "Adjusting camera parameters.";
    auto bundle_adjuster = getBundleAdjuster();
    if (!(*bundle_adjuster)(features, matches, cameras)) {
        std::string message = "Failed to adjust camera parameters.";
        LOG(debug) << message.c_str();
        throw std::invalid_argument(message);
    }
    LOG(debug) << "Finished adjusting camera parameters.";
}

void LowLevelOpenCVStitcher::compose(
        SourceImages &source_images, std::vector<cv::detail::CameraParams> &cameras,
        cv::Ptr<cv::detail::ExposureCompensator> &exposure_compensator,
        LowLevelOpenCVStitcher::WarpResults &warp_results, double work_scale,
        double compose_scale, float warped_image_scale, cv::Mat &result)
{
    LOG(debug) << "Composing stitched image.";

    // compute relative scales
    float compose_work_aspect =
            static_cast<float>(compose_scale / static_cast<double>(work_scale));
    // update warped image scale
    float compose_work_scale = warped_image_scale * compose_work_aspect;

    auto warp_creator = getWarperCreator();
    auto warper = warp_creator->create(compose_work_scale);

    // update corners and sizes
    for (size_t i = 0; i < source_images.images.size(); ++i) {
        // update intrinsics
        double intrinsic_scale = static_cast<double>(compose_work_aspect);
        cameras[i].focal *= intrinsic_scale;
        cameras[i].ppx *= intrinsic_scale;
        cameras[i].ppy *= intrinsic_scale;

        // update corner and size
        cv::Size sz = source_images.images[i].size();

        cv::Mat K;
        cameras[i].K().convertTo(K, CV_32F);
        cv::Rect roi = warper->warpRoi(sz, K, cameras[i].R);
        warp_results.corners[i] = roi.tl();
        warp_results.sizes[i] = roi.size();
    }

    auto blender = prepareBlender(warp_results);

    cv::Mat image_warped, image_warped_s;
    cv::Mat dilated_mask, seam_mask, mask, mask_warped;

    for (size_t i = 0; i < source_images.images.size(); ++i) {
        cv::Size image_size = source_images.images[i].size();

        cv::Mat K;
        cameras[i].K().convertTo(K, CV_32F);

        // warp the current image
        warper->warp(source_images.images[i], K, cameras[i].R, cv::INTER_LINEAR, cv::BORDER_REFLECT,
                     image_warped);
        source_images.images[i].release();

        // warp the current image mask
        mask.create(image_size, CV_8U);
        mask.setTo(cv::Scalar::all(255));
        warper->warp(mask, K, cameras[i].R, cv::INTER_NEAREST, cv::BORDER_CONSTANT,
                     mask_warped);

        // compensate exposure
        exposure_compensator->apply(static_cast<int>(i), warp_results.corners[i],
                                    image_warped, mask_warped);

        image_warped.convertTo(image_warped_s, CV_16S);
        image_warped.release();
        mask.release();

        cv::dilate(warp_results.masks_warped[i], dilated_mask, cv::Mat());
        cv::resize(dilated_mask, seam_mask, mask_warped.size(), 0, 0,
                   cv::INTER_LINEAR_EXACT);
        mask_warped = seam_mask & mask_warped;

        // blend the current image
        blender->feed(image_warped_s, mask_warped, warp_results.corners[i]);
    }

    cv::Mat result_mask;
    blender->blend(result, result_mask);
    LOG(debug) << "Finished composing stitched image.";
}

void LowLevelOpenCVStitcher::debugFeatures(SourceImages &source_images,
        std::vector<cv::detail::ImageFeatures> &features,
        cv::DrawMatchesFlags flags)
{
    if (!_debug) { return; }

    path image_path_base = _debugPath / "features";
    boost::filesystem::create_directories(image_path_base.string());

    cv::Mat source_image, features_image;
    for (size_t i = 0; i < features.size(); ++i) {
        cv::drawKeypoints(source_images.images[features[i].img_idx],
                          features[i].keypoints, features_image,
                          cv::Scalar::all(-1), flags);

        std::string image_name = (boost::format("%1%.jpg") % std::to_string(features[i].img_idx)).str();
        std::string image_path = (image_path_base / image_name).string();

        cv::imwrite(image_path, features_image);
    }
}

void LowLevelOpenCVStitcher::debugImages(SourceImages &source_images,
                                         path debug_path)
{
    boost::filesystem::create_directories(debug_path.string());

    for (size_t i = 0; i < source_images.images.size(); ++i) {
        std::string image_name = (boost::format("%1%.jpg") % std::to_string(i)).str();
        std::string image_path = (debug_path / image_name).string();
        cv::imwrite(image_path, source_images.images[i]);
    }
}

void LowLevelOpenCVStitcher::debugMatches(SourceImages &source_images,
        std::vector<cv::detail::ImageFeatures> &features,
        std::vector<cv::detail::MatchesInfo> &matches,
        float conf_threshold, cv::DrawMatchesFlags flags)
{
    if (!_debug) { return; }

    path image_path_base = _debugPath / "matches";
    boost::filesystem::create_directories(image_path_base.string());

    const int num_matches = static_cast<int>(matches.size());
    for (int i = 0; i < num_matches; ++i) {
        cv::detail::MatchesInfo match = matches[i];
        if (match.confidence < conf_threshold) {
            continue;
        }
        
        cv::Mat img_matches;
        cv::Mat src_img = source_images.images[match.src_img_idx];
        cv::Mat dst_img = source_images.images[match.dst_img_idx];
        std::vector<cv::KeyPoint> src_keypoints = features[match.src_img_idx].getKeypoints();
        std::vector<cv::KeyPoint> dst_keypoints = features[match.dst_img_idx].getKeypoints();
        cv::drawMatches(src_img, src_keypoints, dst_img, dst_keypoints, match.matches, img_matches, cv::Scalar::all(-1),
            cv::Scalar::all(-1), std::vector<char>(), flags);

        std::string image_name = (boost::format("%1%_%2%.jpg") % std::to_string(match.src_img_idx) % std::to_string(match.dst_img_idx)).str();
        std::string image_path = (image_path_base / image_name).string();

        cv::imwrite(image_path, img_matches);
    }

    // Create and save matches graph.
    std::vector<std::string> image_names;
    image_names.reserve(source_images.images.size());
    for (size_t i = 0; i < source_images.images.size(); ++i) {
        image_names.push_back(std::to_string(i));
    }
    std::string matches_graph_path = (_debugPath / "matches.dot").string();
    std::ofstream matchesGraph(matches_graph_path);
    matchesGraph << cv::detail::matchesGraphAsString(image_names, matches, conf_threshold);
}

void LowLevelOpenCVStitcher::debugWarpResults(WarpResults &warp_results)
{
    if (!_debug) { return; }

    path image_path_base = _debugPath / "warp";
    boost::filesystem::create_directories(image_path_base.string());

    for (size_t i = 0; i < warp_results.images_warped.size(); ++i) {
        std::string image_name = (boost::format("%1%.jpg") % std::to_string(i)).str();
        std::string image_path = (image_path_base / image_name).string();

        cv::imwrite(image_path, warp_results.images_warped[i]);
    }
}

std::vector<cv::detail::CameraParams> LowLevelOpenCVStitcher::estimateCameraParameters(
        std::vector<cv::detail::ImageFeatures> &features,
        std::vector<cv::detail::MatchesInfo> &matches)
{
    LOG(debug) << "Estimating camera parameters.";
    std::vector<cv::detail::CameraParams> cameras;

    auto estimator = getEstimator();
    if (!(*estimator)(features, matches, cameras)) {
        std::string message = "Failed to estimate camera parameters.";
        LOG(debug) << message.c_str();
        throw std::invalid_argument(message);
    }

    for (size_t i = 0; i < cameras.size(); i++) {
        cv::Mat R;
        cameras[i].R.convertTo(R, CV_32F);
        cameras[i].R = R;
    }

    LOG(debug) << "Finished camera parameters estimation.";
    return cameras;
}

std::vector<cv::detail::ImageFeatures>
LowLevelOpenCVStitcher::findFeatures(SourceImages &source_images)
{
    LOG(debug) << "Finding features.";
    std::vector<cv::detail::ImageFeatures> features(source_images.images.size());
    cv::Ptr<cv::Feature2D> finder = getFeaturesFinder();

    for (size_t i = 0; i < source_images.images.size(); i++) {
        cv::detail::computeImageFeatures(finder, source_images.images[i],
                                         features[i]);

        features[i].img_idx = static_cast<int>(i);
    }

    LOG(debug) << "Finished finding features.";
    return features;
}

double LowLevelOpenCVStitcher::findMedianFocalLength(
        std::vector<cv::detail::CameraParams> &cameras)
{
    std::vector<double> focal_lengths;
    for (size_t i = 0; i < cameras.size(); i++) {
        focal_lengths.push_back(cameras[i].focal);
    }

    sort(focal_lengths.begin(), focal_lengths.end());
    if (focal_lengths.size() % 2 == 1) {
        return focal_lengths[focal_lengths.size() / 2];
    } else {
        return focal_lengths[focal_lengths.size() / 2 - 1]
                + focal_lengths[focal_lengths.size() / 2] * 0.5;
    }
}

void LowLevelOpenCVStitcher::findSeams(LowLevelOpenCVStitcher::WarpResults &warp_results)
{
    LOG(debug) << "Finding seams.";
    auto seam_finder = getSeamFinder();
    seam_finder->find(warp_results.images_warped_f, warp_results.corners,
                      warp_results.masks_warped);
    LOG(debug) << "Finished finding seams.";
}

cv::Ptr<cv::detail::BundleAdjusterBase> LowLevelOpenCVStitcher::getBundleAdjuster()
{
    cv::Ptr<cv::detail::BundleAdjusterBase> bundle_adjuster;

    switch (config.bundle_adjuster_type) {
    case BundleAdjusterType::Reproj:
        bundle_adjuster = cv::makePtr<cv::detail::BundleAdjusterReproj>();
        break;
    case BundleAdjusterType::Ray:
        bundle_adjuster = cv::makePtr<cv::detail::BundleAdjusterRay>();
        break;
    case BundleAdjusterType::AffinePartial:
        bundle_adjuster = cv::makePtr<cv::detail::BundleAdjusterAffinePartial>();
        break;
    case BundleAdjusterType::No:
        bundle_adjuster = cv::makePtr<cv::detail::NoBundleAdjuster>();
        break;
    }

    bundle_adjuster->setConfThresh(config.match_conf_thresh);

    // TODO(bkd): get from config
    cv::Mat_<uchar> refine_mask = (cv::Mat_<uchar>(3, 3) << 1, 1, 1, 1, 1, 1, 1);

    bundle_adjuster->setRefinementMask(refine_mask);
    return bundle_adjuster;
}

double LowLevelOpenCVStitcher::getComposeScale(SourceImages &source_images)
{
    if (config.compose_megapix < 0) {
        return 1.0;
    }

    return cv::min(
            1.0,
            sqrt(config.compose_megapix * 1e6 / source_images.images[0].size().area()));
}

cv::Ptr<cv::detail::Estimator> LowLevelOpenCVStitcher::getEstimator()
{
    cv::Ptr<cv::detail::Estimator> estimator;

    switch (config.estimator_type) {
    case EstimatorType::Affine:
        estimator = cv::makePtr<cv::detail::AffineBasedEstimator>();
        break;
    case EstimatorType::Homography:
        estimator = cv::makePtr<cv::detail::HomographyBasedEstimator>();
        break;
    }

    return estimator;
}

cv::Ptr<cv::detail::ExposureCompensator> LowLevelOpenCVStitcher::getExposureCompensator()
{
    cv::Ptr<cv::detail::ExposureCompensator> compensator;

    switch (config.exposure_compensator_type) {
    case ExposureCompensatorType::Channels:
        compensator = cv::detail::ExposureCompensator::createDefault(
                cv::detail::ExposureCompensator::CHANNELS);
        break;
    case ExposureCompensatorType::ChannelsBlocks:
        compensator = cv::detail::ExposureCompensator::createDefault(
                cv::detail::ExposureCompensator::CHANNELS_BLOCKS);
        break;
    case ExposureCompensatorType::Gain:
        compensator = cv::detail::ExposureCompensator::createDefault(
                cv::detail::ExposureCompensator::GAIN);
        break;
    case ExposureCompensatorType::GainBlocks:
        compensator = cv::detail::ExposureCompensator::createDefault(
                cv::detail::ExposureCompensator::GAIN_BLOCKS);
        break;
    case ExposureCompensatorType::No:
        compensator = cv::detail::ExposureCompensator::createDefault(
                cv::detail::ExposureCompensator::NO);
        break;
    }

    if (dynamic_cast<cv::detail::GainCompensator *>(compensator.get())) {
        cv::detail::GainCompensator *gain_compensator =
                dynamic_cast<cv::detail::GainCompensator *>(compensator.get());
        gain_compensator->setNrFeeds(config.exposure_compensation_nr_feeds);
    }

    if (dynamic_cast<cv::detail::ChannelsCompensator *>(compensator.get())) {
        cv::detail::ChannelsCompensator *channels_compensator =
                dynamic_cast<cv::detail::ChannelsCompensator *>(compensator.get());
        channels_compensator->setNrFeeds(config.exposure_compensation_nr_feeds);
    }

    if (dynamic_cast<cv::detail::BlocksCompensator *>(compensator.get())) {
        cv::detail::BlocksCompensator *blocks_compensator =
                dynamic_cast<cv::detail::BlocksCompensator *>(compensator.get());
        blocks_compensator->setNrFeeds(config.exposure_compensation_nr_feeds);
        blocks_compensator->setNrGainsFilteringIterations(
                config.exposure_compensation_nr_filtering);
        blocks_compensator->setBlockSize(config.exposure_compensation_block_size,
                                         config.exposure_compensation_block_size);
    }

    return compensator;
}

cv::Ptr<cv::Feature2D> LowLevelOpenCVStitcher::getFeaturesFinder()
{
    cv::Ptr<cv::Feature2D> features_finder;

    switch (config.features_finder_type) {
    case FeaturesFinderType::Orb:
        features_finder = cv::ORB::create(config.features_maximum);
        break;
    case FeaturesFinderType::Akaze:
        features_finder = cv::AKAZE::create();
        break;
    case FeaturesFinderType::Sift:
        features_finder = cv::ORB::create(); // TODO: compile flag and dependencies
        break;
    case FeaturesFinderType::Surf:
        features_finder = cv::ORB::create(); // TODO: compile flag and dependencies
        break;
    }

    return features_finder;
}

cv::Ptr<cv::detail::FeaturesMatcher> LowLevelOpenCVStitcher::getFeaturesMatcher()
{
    cv::Ptr<cv::detail::FeaturesMatcher> features_matcher;

    switch (config.features_matcher_type) {
    case FeaturesMatcherType::Affine:
        features_matcher = cv::makePtr<cv::detail::AffineBestOf2NearestMatcher>(
                false, config.try_cuda, config.match_conf);
        break;
    case FeaturesMatcherType::Homography:
        if (config.range_width == -1) {
            features_matcher = cv::makePtr<cv::detail::BestOf2NearestMatcher>(
                    config.try_cuda, config.match_conf);
        } else {
            features_matcher = cv::makePtr<cv::detail::BestOf2NearestRangeMatcher>(
                    config.range_width, config.try_cuda, config.match_conf);
        }
        break;
    }

    return features_matcher;
}

cv::Ptr<cv::detail::SeamFinder> LowLevelOpenCVStitcher::getSeamFinder()
{
    cv::Ptr<cv::detail::SeamFinder> seam_finder;

    switch (config.seam_finder_type) {
    case SeamFinderType::DpColor:
        seam_finder =
                cv::makePtr<cv::detail::DpSeamFinder>(cv::detail::DpSeamFinder::COLOR);
        break;
    case SeamFinderType::DpColorGrad:
        seam_finder = cv::makePtr<cv::detail::DpSeamFinder>(
                cv::detail::DpSeamFinder::COLOR_GRAD);
        break;
    case SeamFinderType::GraphCutColor: // TODO(bkd): optional GPU support
        seam_finder = cv::makePtr<cv::detail::GraphCutSeamFinder>(
                cv::detail::GraphCutSeamFinder::COST_COLOR);
        break;
    case SeamFinderType::GraphCutColorGrad: // TODO(bkd): optional GPU support
        seam_finder = cv::makePtr<cv::detail::GraphCutSeamFinder>(
                cv::detail::GraphCutSeamFinder::COST_COLOR_GRAD,
                config.seam_finder_graph_cut_terminal_cost,
                config.seam_finder_graph_cut_bad_region_penalty);
        break;
    case SeamFinderType::Voronoi:
        seam_finder = cv::makePtr<cv::detail::VoronoiSeamFinder>();
        break;
    case SeamFinderType::No:
        seam_finder = cv::makePtr<cv::detail::NoSeamFinder>();
        break;
    }

    return seam_finder;
}

double LowLevelOpenCVStitcher::getSeamScale(SourceImages &source_images)
{
    if (config.seam_megapix < 0) {
        return 1.0;
    }

    return cv::min(
            1.0, sqrt(config.seam_megapix * 1e6 / source_images.images[0].size().area()));
}

cv::Ptr<cv::WarperCreator> LowLevelOpenCVStitcher::getWarperCreator()
{
    cv::Ptr<cv::WarperCreator> warper_creator;

    // TODO(bkd): optional GPU support
    switch (config.warper_type) {
    case WarperType::Affine:
        warper_creator = cv::makePtr<cv::AffineWarper>();
        break;
    case WarperType::CompressedPlaneA2B1:
        warper_creator = cv::makePtr<cv::CompressedRectilinearWarper>(2.0f, 1.0f);
        break;
    case WarperType::CompressedPlaneA1_5B1:
        warper_creator = cv::makePtr<cv::CompressedRectilinearWarper>(1.5f, 1.0f);
        break;
    case WarperType::CompressedPlanePortraitA2B1:
        warper_creator = cv::makePtr<cv::CompressedRectilinearPortraitWarper>(2.0f, 1.0f);
        break;
    case WarperType::CompressedPlanePortraitA1_5B1:
        warper_creator = cv::makePtr<cv::CompressedRectilinearPortraitWarper>(1.5f, 1.0f);
        break;
    case WarperType::Cylindrical:
        warper_creator = cv::makePtr<cv::CylindricalWarper>();
        break;
    case WarperType::Fisheye:
        warper_creator = cv::makePtr<cv::FisheyeWarper>();
        break;
    case WarperType::Mercator:
        warper_creator = cv::makePtr<cv::MercatorWarper>();
        break;
    case WarperType::Planer:
        warper_creator = cv::makePtr<cv::PlaneWarper>();
        break;
    case WarperType::PaniniA2B1:
        warper_creator = cv::makePtr<cv::PaniniWarper>(2.0f, 1.0f);
        break;
    case WarperType::PaniniA1_5B1:
        warper_creator = cv::makePtr<cv::PaniniWarper>(1.5f, 1.0f);
        break;
    case WarperType::PaniniPortraitA2B1:
        warper_creator = cv::makePtr<cv::PaniniPortraitWarper>(2.0f, 1.0f);
        break;
    case WarperType::PaniniPortraitA1_5B1:
        warper_creator = cv::makePtr<cv::PaniniPortraitWarper>(1.5f, 1.0f);
        break;
    case WarperType::Spherical:
        warper_creator = cv::makePtr<cv::SphericalWarper>();
        break;
    case WarperType::Stereographic:
        warper_creator = cv::makePtr<cv::StereographicWarper>();
        break;
    case WarperType::TransverseMercator:
        warper_creator = cv::makePtr<cv::TransverseMercatorWarper>();
        break;
    }

    return warper_creator;
}

cv::detail::WaveCorrectKind LowLevelOpenCVStitcher::getWaveCorrect()
{
    cv::detail::WaveCorrectKind wave_correct;

    switch (config.wave_correct_type) {
    case WaveCorrectType::Horizontal:
        wave_correct = cv::detail::WAVE_CORRECT_HORIZ;
        break;
    case WaveCorrectType::Vertical:
        wave_correct = cv::detail::WAVE_CORRECT_VERT;
        break;
    }

    return wave_correct;
}

double LowLevelOpenCVStitcher::getWorkScale(SourceImages &source_images)
{
    if (config.work_megapix < 0) {
        return 1.0;
    }

    return cv::min(
            1.0, sqrt(config.work_megapix * 1e6 / source_images.images[0].size().area()));
}

std::vector<cv::detail::MatchesInfo>
LowLevelOpenCVStitcher::matchFeatures(std::vector<cv::detail::ImageFeatures> &features)
{
    LOG(debug) << "Matching features.";
    std::vector<cv::detail::MatchesInfo> matches;
    cv::Ptr<cv::detail::FeaturesMatcher> matcher = getFeaturesMatcher();
    (*matcher)(features, matches);
    matcher->collectGarbage();
    LOG(debug) << "Finished matching features.";
    return matches;
}

cv::Ptr<cv::detail::Blender>
LowLevelOpenCVStitcher::prepareBlender(WarpResults &warp_results)
{
    cv::Ptr<cv::detail::Blender> blender =
            cv::detail::Blender::createDefault(config.blender_type, config.try_cuda);
    cv::Size destination_size =
            cv::detail::resultRoi(warp_results.corners, warp_results.sizes).size();
    float blend_width = cv::sqrt(static_cast<float>(destination_size.area()))
            * config.blend_strength / 100.f;

    if (blend_width < 1.f) {
        blender = cv::detail::Blender::createDefault(cv::detail::Blender::NO,
                                                     config.try_cuda);
    } else if (config.blender_type == cv::detail::Blender::MULTI_BAND) {
        auto *multiband_blender =
                dynamic_cast<cv::detail::MultiBandBlender *>(blender.get());
        multiband_blender->setNumBands(static_cast<int>(
                ceil(log(static_cast<double>(blend_width)) / log(2.)) - 1.));
        LOG(debug) << "Multi-band blender prepared with " << multiband_blender->numBands()
                   << " bands.";
    } else if (config.blender_type == cv::detail::Blender::FEATHER) {
        auto *feather_blender = dynamic_cast<cv::detail::FeatherBlender *>(blender.get());
        feather_blender->setSharpness(1.f / blend_width);
        LOG(debug) << "Feather blender prepared with " << feather_blender->sharpness()
                   << " sharpness.";
    }

    blender->prepare(warp_results.corners, warp_results.sizes);
    return blender;
}

cv::Ptr<cv::detail::ExposureCompensator>
LowLevelOpenCVStitcher::prepareExposureCompensation(
        LowLevelOpenCVStitcher::WarpResults &warp_results)
{
    LOG(debug) << "Preparing exposure compensation.";
    auto compensator = getExposureCompensator();
    compensator->feed(warp_results.corners, warp_results.images_warped,
                      warp_results.masks_warped);
    LOG(debug) << "Finished exposure compensation preparation.";
    return compensator;
}

Stitcher::Report LowLevelOpenCVStitcher::stitch()
{
    cv::Mat result;
    Stitcher::Report report;
    
    try {
        report = stitch(result);
    } catch (const std::exception &e) {
        // can indeed throw, e.g.:
        //.../OpenCV/modules/flann/src/miniflann.cpp:487: error: (-215:Assertion
        // failed) (size_t)knn <= index_->size() in function 'runKnnSearch_' but
        // that's
        // nothing we shouldn't want to retry on.
        throw RetriableError(e.what());
    }

    postprocess(std::move(result));
    return report;
}

void LowLevelOpenCVStitcher::cancel() { }

Stitcher::Report LowLevelOpenCVStitcher::stitch(cv::Mat &result)
{
    Stitcher::Report report;
    std::list<std::string> sourceImagePaths = _panorama.inputPaths();

    // Load images and determine scales.
    SourceImages source_images(_panorama, _logger);
    source_images.ensureImageCount();
    source_images.scaleToAvailableMemory(_parameters.memoryBudgetMB,
                                         _parameters.maxInputImageSize,
                                         report.inputSizeMB,
                                         report.inputScaled);
    double seam_scale = getSeamScale(source_images);
    double work_scale = getWorkScale(source_images);
    double compose_scale = getComposeScale(source_images);

    // Optionally undistort images for detected cameras with a
    // known distortion model.
    undistortImages(source_images);

    // Scale images down for feature detection and matching.
    source_images.scale(work_scale);

    // Find features and matches.
    auto features = findFeatures(source_images);
    debugFeatures(source_images, features);
    auto matches = matchFeatures(features);
    debugMatches(source_images, features, matches, config.match_conf_thresh);
    source_images.clear();

    // Prepare to filter images with poor matching.
    auto keep_indices = cv::detail::leaveBiggestComponent(
            features, matches, static_cast<float>(config.match_conf_thresh));

    // Estimate and refine camera parameters.
    auto cameras = estimateCameraParameters(features, matches);
    adjustCameraParameters(features, matches, cameras);

    // Perform wave correction.
    waveCorrect(cameras);

    // Reload images at seam scale.
    source_images.reload();
    source_images.filter(keep_indices);
    source_images.scale(report.inputScaled * seam_scale);

    // Warp images.
    double median_focal_length = findMedianFocalLength(cameras);
    float seam_work_aspect = static_cast<float>(seam_scale / work_scale);
    float warped_image_scale = static_cast<float>(median_focal_length);
    auto warp_results =
            warpImages(source_images, cameras, warped_image_scale, seam_work_aspect);
    debugWarpResults(warp_results);

    // Prepare exposure compensation.
    auto exposure_compensator = prepareExposureCompensation(warp_results);

    // Release memory.
    source_images.clear();
    warp_results.images_warped.clear();
    warp_results.masks.clear();

    // Find seams.
    findSeams(warp_results);

    // Release memory.
    warp_results.images_warped_f.clear();

    // Reload images at compose scale.
    source_images.reload();
    source_images.filter(keep_indices);
    source_images.scale(report.inputScaled * compose_scale);

    // Compose the final panorama.
    compose(source_images, cameras, exposure_compensator, warp_results,
            work_scale, compose_scale, warped_image_scale, result);

    return report;
}

void LowLevelOpenCVStitcher::undistortImages(SourceImages &source_images)
{
    boost::optional<Camera> camera_ = CameraModels().detect(_panorama.front());
    if (!camera_.has_value()) {
        LOG(debug) << "Camera model not identified.";
        return;
    }

    Camera camera = camera_.get();
    if (!camera.distortion_model) {
        LOG(debug) << "Camera model doesn't have a distortion model.  Skipping undistortion.";
        return;
    }

    if (camera.distortion_model->enabled()) {
        LOG(debug) << "Undistorting images.";

        cv::Mat K = camera.K();
        camera.distortion_model->undistort(source_images.images, K);

        if (_debug) {
            path undistorted_image_path = _debugPath / "undistorted";
            debugImages(source_images, undistorted_image_path);
        }

        LOG(debug) << "Finished undistorting images.";
    }
}

LowLevelOpenCVStitcher::WarpResults
LowLevelOpenCVStitcher::warpImages(SourceImages &source_images,
                                   std::vector<cv::detail::CameraParams> &cameras,
                                   float warped_image_scale, float seam_work_aspect)
{
    LOG(debug) << "Warping images.";

    auto warper_creator = getWarperCreator();
    auto warper = warper_creator->create(warped_image_scale * seam_work_aspect);

    size_t image_count = source_images.images.size();
    WarpResults warp_results(image_count);

    // Prepare image masks
    for (size_t i = 0; i < image_count; ++i) {
        warp_results.masks[i].create(source_images.images[i].size(), CV_8U);
        warp_results.masks[i].setTo(cv::Scalar::all(255));
    }

    for (size_t i = 0; i < image_count; ++i) {
        cv::Mat_<float> K;
        cameras[i].K().convertTo(K, CV_32F);
        K(0, 0) *= seam_work_aspect;
        K(0, 2) *= seam_work_aspect;
        K(1, 1) *= seam_work_aspect;
        K(1, 2) *= seam_work_aspect;

        warp_results.corners[i] =
                warper->warp(source_images.images[i], K, cameras[i].R, cv::INTER_LINEAR,
                             cv::BORDER_REFLECT, warp_results.images_warped[i]);
        warp_results.sizes[i] = warp_results.images_warped[i].size();
        warper->warp(warp_results.masks[i], K, cameras[i].R, cv::INTER_NEAREST,
                     cv::BORDER_CONSTANT, warp_results.masks_warped[i]);
        warp_results.images_warped[i].convertTo(warp_results.images_warped_f[i], CV_32F);
    }

    LOG(debug) << "Finished warping images.";
    return warp_results;
}

void LowLevelOpenCVStitcher::waveCorrect(std::vector<cv::detail::CameraParams> &cameras)
{
    if (!config.wave_correct) { return; }

    std::vector<cv::Mat> rmats;
    for (auto camera : cameras) {
        rmats.push_back(camera.R.clone());
    }
    cv::detail::waveCorrect(rmats, getWaveCorrect());

    for (size_t i = 0; i < cameras.size(); ++i) {
        cameras[i].R = rmats[i];
    }
}

} // namespace stitcher
} // namespace airmap
