#include "feature_detector.hpp"

namespace vis
{
    void FeatureDetector::PredictAndDetectFeatures(
        const Image<u8> &image,
        const Image<Vec2f> &gradient_image,
        const Image<float> &gradmag_image,
        vector<FeatureDetection> &feature_predictions, // input
        vector<FeatureDetection> &feature_detections,  // output
        bool debug,
        bool debug_step_by_step,
        Vec3u8 debug_colors[8])
    {
        const int kIncrementalPredictionErrorThreshold = window_half_extent * 4 / 5.f; // In pixels. TODO: make configurable
        constexpr float kMinimumFeatureDistance = 5;                                   // in pixels; TODO: make configurable

        feature_detections.resize(feature_predictions.size());

        // Used to be a while loop here. Not necessary since we're simplifying the data structures.

        // Visualization for when in debug mode
        if (debug)
        {
            // Show all feature predictions in gray
            for (auto &item : feature_predictions)
            {
                d->debug_display->AddSubpixelDotPixelCornerConv(
                    item.position + Vec2f::Constant(0.5f), Vec3u8(127, 127, 127));
            }
            if (debug_step_by_step)
            {
                std::cout << "[INFO] Showing new predictions (and neighbor validations)" << std::endl;
                d->debug_display->Update();
                std::getchar();
            }
        }

        // Refine all feature predictions and convert them to detected features if
        // the refinement was successful. Submit all refinement requests at the same
        // time, such that they can be performed in parallel.
        vector<FeatureDetection> features_for_refinement;
        features_for_refinement.reserve(128);
        vector<FeatureDetection> refined_detections;
        refined_detections.reserve(128);
        for (auto feature : feature_predictions)
        {
            features_for_refinement.push_back(feature);
            refined_detections.emplace_back();
        }

        RefineFeatureDetections(
            image,
            gradient_image,
            gradmag_image,
            features_for_refinement.size(), // n_features
            features_for_refinement.data(), // features to be refined
            refined_detections.data(),      // output
            debug,
            debug_step_by_step);

        int index = 0; // Used to track index of refined_detections

        for (auto &predicted_feature : feature_predictions)
        {
            FeatureDetection &refined_feature = refined_detections[index];
            ++index;

            // For features discarded during refinement, the cost is set to a negative value.
            if (refined_feature.final_cost < 0)
                continue;

            // Check whether the returned position is within a reasonable range of the prediction.
            if (!((refined_feature.position - predicted_feature.position).squaredNorm() <=
                  kIncrementalPredictionErrorThreshold * kIncrementalPredictionErrorThreshold))
                continue;

            // Check whether the returned position is too close to an existing one.
            bool reject_detection = false;
            for (auto &existing_detection : feature_detections)
            {
                float squared_distance = (existing_detection.position - refined_feature.position).squaredNorm();
                if (!(squared_distance >= kMinimumFeatureDistance * kMinimumFeatureDistance))
                {
                    reject_detection = true;
                    break;
                }
            }
            if (reject_detection)
                continue;

            refined_feature.pattern_coordinate = predicted_feature.pattern_coordinate;
            refined_feature.local_pixel_tr_pattern = predicted_feature.local_pixel_tr_pattern;

            // Add the refined position as a new detection.
            feature_detections.push_back(refined_feature);

            if (debug)
            {
                d->debug_display->AddSubpixelDotPixelCornerConv(
                    refined_feature.position + Vec2f::Constant(0.5f),
                    debug_colors[0]); // Was supposed to use pattern.tags[0].index % 8, but I figured it doesn't really matter so I hardcoded it.
            }
        }

        feature_predictions.clear(); // cleanup

        if (debug && debug_step_by_step)
        {
            std::cout << "[INFO] Showing new refined detections" << std::endl;
            d->debug_display->Update();
            std::getchar();
        }
    }

    void FeatureDetector::RefineFeatureDetections(
        const Image<u8> &image,
        const Image<Vec2f> &gradient_image,
        const Image<float> &gradmag_image,
        int num_features,
        const FeatureDetection *predicted_features,
        const FeatureDetection *output,
        bool debug,
        bool debug_step_by_step)
    {
        vector<Vec2f> position_after_intensity_based_refinement(num_features);

        const int num_intensity_samples = (1 / 8.) * d->samples.size();
        const int num_gradient_samples = d->samples.size();

        for (usize i = 0; i < num_features; ++i)
        {
            FeatureDetection this_output = output[i];
            const Mat3f &local_pixel_tr_pattern = predicted_features[i].local_pixel_tr_pattern;
            Mat3f local_pattern_tr_pixel = local_pixel_tr_pattern.inverse();
            if (!RefineFeatureByMatching(
                    num_intensity_samples,
                    d->samples,
                    image,
                    window_half_extent,
                    predicted_features[i].position,
                    local_pattern_tr_pixel,
                    d->patterns[0], // TODO: Use the correct pattern here instead of always the one with index 0
                    &this_output.position,
                    nullptr,
                    debug))
            {
                // Could not find a corner here.
                if (debug)
                {
                    d->debug_display->AddSubpixelDotPixelCornerConv(predicted_features[i].position + Vec2f::Constant(0.5f), Vec3u8(255, 0, 0));
                    if (debug_step_by_step)
                    {
                        std::cout << "[WARNING] Failure during matching-based refinement" << std::endl;
                        d->debug_display->Update();
                        std::getchar();
                    }
                }
                this_output.final_cost = -1;
                continue;
            }

            position_after_intensity_based_refinement[i] = this_output.position;
            bool feature_found_from_symmetry = false;
            if (refinement_type == FeatureRefinement::GradientsXY)
            {
                feature_found_from_symmetry = RefineFeatureBySymmetry<SymmetryCostFunction_GradientsXY>(
                    num_gradient_samples,
                    d->samples,
                    gradient_image,
                    window_half_extent,
                    this_output.position,
                    local_pattern_tr_pixel,
                    local_pixel_tr_pattern,
                    &this_output.position,
                    &this_output.final_cost,
                    debug);
            }
            else if (refinement_type == FeatureRefinement::GradientMagnitude)
            {
                feature_found_from_symmetry = RefineFeatureBySymmetry<SymmetryCostFunction_SingleChannel>(
                    num_gradient_samples,
                    d->samples,
                    gradmag_image,
                    window_half_extent,
                    this_output.position,
                    local_pattern_tr_pixel,
                    local_pixel_tr_pattern,
                    &this_output.position,
                    &this_output.final_cost,
                    debug);
            }
            else if (refinement_type == FeatureRefinement::Intensities)
            {
                feature_found_from_symmetry = RefineFeatureBySymmetry<SymmetryCostFunction_SingleChannel>(
                    num_gradient_samples,
                    d->samples,
                    image,
                    window_half_extent,
                    this_output.position,
                    local_pattern_tr_pixel,
                    local_pixel_tr_pattern,
                    &this_output.position,
                    &this_output.final_cost,
                    debug);
            }
            else if (refinement_type == FeatureRefinement::NoRefinement)
            {
                // Use the output of the matching-based feature detection as-is.
                this_output.final_cost = 0;
                feature_found_from_symmetry = true;
            }
            else
            {
                std::cout << "[FATAL] Unsupported feature refinement type" << std::endl;
            }

            if (!feature_found_from_symmetry)
            {
                // Could not find a feature here.
                if (debug)
                {
                    d->debug_display->AddSubpixelDotPixelCornerConv(predicted_features[i].position + Vec2f::Constant(0.5f), Vec3u8(255, 0, 0));
                    if (debug_step_by_step)
                    {
                        std::cout << "[WARNING] Failure during symmetry-based refinement" << std::endl;
                        d->debug_display->Update();
                        std::getchar();
                    }
                }
                this_output.final_cost = -1;
                continue;
            }
        }
    }

    void FeatureDetector::computeGradientGradmagImages(
        const Image<u8> &image,
        Image<Vec2f> &gradient_image,
        Image<float> &gradmag_image)
    {
        gradient_image.SetSize(image.size());
        gradmag_image.SetSize(image.size());
        int width = image.width();
        int height = image.height();

        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                int mx = std::max<int>(0, x - 1);
                int px = std::min<int>(width - 1, x + 1);

                int my = std::max<int>(0, y - 1);
                int py = std::min<int>(height - 1, y + 1);

                float dx = (image(px, y) - static_cast<float>(image(mx, y))) / (px - mx);
                float dy = (image(x, py) - static_cast<float>(image(x, my))) / (py - my);

                gradient_image(x, y) = Vec2f(dx, dy);
                gradmag_image(x, y) = gradient_image(x, y).norm();
            }
        }
    }

    void FeatureDetector::DetectFeatures(
        const Image<Vec3u8> &image,
        std::vector<Vec2f> &features,
        Image<Vec3u8> *detection_visualization)
    {
        // Setting up image to be visualized
        detection_visualization->SetSize(image.size());
        detection_visualization->SetTo(image);

        // Prepare sample positions.
        int max_sample_count = static_cast<int>(8.0 * (2 * window_half_extent + 1) * (2 * window_half_extent + 1) + 0.5);
        if (d->samples.empty() || d->samples.size() < max_sample_count)
        {
            d->samples.resize(max_sample_count);
            srand(0); // setting seed.
            for (usize i = 0; i < d->samples.size(); ++i)
            {
                d->samples[i] = Vec2f::Random();
            }
        }

        // Convert the image to grayscale.
        Image<u8> gray_image;
        image.ConvertToGrayscale(&gray_image);

        // Initialize variables and compute gradient image.
        Image<Vec2f> gradient_image;
        Image<float> gradmag_image;
        computeGradientGradmagImages(gray_image, gradient_image, gradmag_image);

        static Vec3u8 debug_colors[8] = {Vec3u8(255, 80, 80),
                                         Vec3u8(255, 80, 255),
                                         Vec3u8(80, 255, 255),
                                         Vec3u8(0, 255, 0),
                                         Vec3u8(80, 80, 255),
                                         Vec3u8(127, 255, 127),
                                         Vec3u8(255, 160, 0),
                                         Vec3u8(255, 255, 0)};

        // TODO: Read features into this format.
        vector<FeatureDetection> feature_predictions;

        /// Mapping: feature_predictions[pattern_array_index][Vec2i(pattern_x, pattern_y)] -> final feature detection.
        vector<FeatureDetection> feature_detections;

        PredictAndDetectFeatures(
            gray_image,
            gradient_image,
            gradmag_image,
            feature_predictions,
            feature_detections,
            false,
            false,
            debug_colors);

        for (auto &pattern_feature_detections : feature_detections)
        {
        }
    }
}