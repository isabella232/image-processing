#include "gtest/gtest.h"
#include "airmap/images.h"
#include "airmap/logger.h"
#include "airmap/panorama.h"

using airmap::stitcher::GeoImage;
using airmap::stitcher::Panorama;
using airmap::stitcher::SourceImages;
using airmap::filesystem::path;
using airmap::logging::Logger;
using airmap::logging::stdoe_logger;

class SourceImagesTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        path image_directory = (path("../") / path("test") / path("fixtures") /
                                path("panorama_aus_1"));

        std::list<GeoImage> input {
            GeoImage::fromExif((image_directory / path("P5050970.JPG")).string()),
            GeoImage::fromExif((image_directory / path("P5060971.JPG")).string()),
            GeoImage::fromExif((image_directory / path("P5060972.JPG")).string()),
            GeoImage::fromExif((image_directory / path("P5070973.JPG")).string()),
            GeoImage::fromExif((image_directory / path("P5070974.JPG")).string()),
            GeoImage::fromExif((image_directory / path("P5080975.JPG")).string()),
            GeoImage::fromExif((image_directory / path("P5080976.JPG")).string()),
            GeoImage::fromExif((image_directory / path("P5090977.JPG")).string()),
            GeoImage::fromExif((image_directory / path("P5090978.JPG")).string()),
            GeoImage::fromExif((image_directory / path("P5100979.JPG")).string()),
            GeoImage::fromExif((image_directory / path("P5100980.JPG")).string()),
            GeoImage::fromExif((image_directory / path("P5110981.JPG")).string()),
            GeoImage::fromExif((image_directory / path("P5110982.JPG")).string()),
            GeoImage::fromExif((image_directory / path("P5120983.JPG")).string()),
            GeoImage::fromExif((image_directory / path("P5120984.JPG")).string()),
            GeoImage::fromExif((image_directory / path("P5130985.JPG")).string()),
            GeoImage::fromExif((image_directory / path("P5130986.JPG")).string()),
            GeoImage::fromExif((image_directory / path("P5130987.JPG")).string()),
            GeoImage::fromExif((image_directory / path("P5140988.JPG")).string()),
            GeoImage::fromExif((image_directory / path("P5140989.JPG")).string()),
            GeoImage::fromExif((image_directory / path("P5150990.JPG")).string()),
            GeoImage::fromExif((image_directory / path("P5150991.JPG")).string()),
            GeoImage::fromExif((image_directory / path("P5160992.JPG")).string()),
            GeoImage::fromExif((image_directory / path("P5160993.JPG")).string()),
            GeoImage::fromExif((image_directory / path("P5160994.JPG")).string())
        };

        Panorama panorama = Panorama(input);
        logger = std::make_shared<stdoe_logger>();
        source_images = std::make_shared<SourceImages>(std::move(panorama), logger);
    }

    std::shared_ptr<Logger> logger;
    std::shared_ptr<SourceImages> source_images;
};

TEST_F(SourceImagesTest, sourceImagesStruct)
{
    EXPECT_EQ(source_images->gimbal_orientations.size(), 25);
    EXPECT_EQ(source_images->images.size(), 25);
    EXPECT_EQ(source_images->sizes.size(), 25);
}

TEST_F(SourceImagesTest, sourceImagesClear)
{
    EXPECT_EQ(source_images->gimbal_orientations.size(), 25);
    EXPECT_EQ(source_images->images.size(), 25);
    EXPECT_EQ(source_images->sizes.size(), 25);

    source_images->clear();

    EXPECT_EQ(source_images->gimbal_orientations.size(), 0);
    EXPECT_EQ(source_images->images.size(), 0);
    EXPECT_EQ(source_images->sizes.size(), 0);
}

TEST_F(SourceImagesTest, sourceImagesEnsureImageCount)
{
    std::vector<int> keep_indices;
    EXPECT_NO_THROW(source_images->ensureImageCount());

    keep_indices = { 0, 1 };
    source_images->filter(keep_indices);
    EXPECT_NO_THROW(source_images->ensureImageCount());

    keep_indices = { 0 };
    EXPECT_THROW(source_images->filter(keep_indices), std::invalid_argument);
    EXPECT_THROW(source_images->ensureImageCount(), std::invalid_argument);

    source_images->clear();
    EXPECT_THROW(source_images->ensureImageCount(), std::invalid_argument);
}

TEST_F(SourceImagesTest, sourceImagesFilter)
{
    int remove_index = 5;
    cv::Size image_size = source_images->images[remove_index].size();
    cv::Point2i principal_point(image_size.width / 2, image_size.height / 2);
    cv::Vec3b principal_point_values_removed =
        source_images->images[remove_index].at<cv::Vec3b>(
            principal_point.y, principal_point.x);


    /**
     * Make sure that no other images have the same values at the
     * principal point.
     */
    for (size_t i = 0; i < source_images->images.size(); ++i) {
        cv::Vec3b principal_point_values = source_images->images[i].at<cv::Vec3b>(
            principal_point.y, principal_point.x);

        if (i == remove_index) {
            EXPECT_TRUE(principal_point_values_removed[0] == principal_point_values[0] &&
                        principal_point_values_removed[1] == principal_point_values[1] &&
                        principal_point_values_removed[2] == principal_point_values[2]);
        } else {
            EXPECT_FALSE(principal_point_values_removed[0] == principal_point_values[0] &&
                        principal_point_values_removed[1] == principal_point_values[1] &&
                        principal_point_values_removed[2] == principal_point_values[2]);
        }
    }

    // Remove image at remove_index.
    std::vector<int> keep_indices;
    for (size_t i = 0; i < source_images->images.size(); ++i) {
        if (i != remove_index) {
            keep_indices.push_back(i);
        }
    }
    source_images->filter(keep_indices);
    EXPECT_EQ(source_images->images.size(), 24);

    /**
     * Make sure that no remaining images have the same values at the
     * principal point.
     */
    for (size_t i = 0; i < source_images->images.size(); ++i) {
        cv::Vec3b principal_point_values = source_images->images[i].at<cv::Vec3b>(
            principal_point.y, principal_point.x);

        EXPECT_FALSE(principal_point_values_removed[0] == principal_point_values[0] &&
                     principal_point_values_removed[1] == principal_point_values[1] &&
                     principal_point_values_removed[2] == principal_point_values[2]);
    }
}

/**
 * This currently throws an exception during reload.  It only
 * happpens as part of these tests.  valgrind shows invalid reads
 * during std::list<GeoImage> deallocation.
 */
// TEST_F(SourceImagesTest, sourceImagesReload)
// {
//     EXPECT_EQ(source_images->images.size(), 25);

//     source_images->clear();
//     EXPECT_EQ(source_images->images.size(), 0);

//     source_images->reload();
//     EXPECT_EQ(source_images->images.size(), 25);
// }

TEST_F(SourceImagesTest, sourceImagesResize)
{
    source_images->resize(10);
    EXPECT_EQ(source_images->gimbal_orientations.size(), 10);
    EXPECT_EQ(source_images->images.size(), 10);
    EXPECT_EQ(source_images->sizes.size(), 10);

    source_images->resize(5);
    EXPECT_EQ(source_images->gimbal_orientations.size(), 5);
    EXPECT_EQ(source_images->images.size(), 5);
    EXPECT_EQ(source_images->sizes.size(), 5);
}

TEST_F(SourceImagesTest, sourceImagesScale)
{
    cv::Size image_size = source_images->images[0].size();
    for (size_t i = 0; i < source_images->images.size(); ++i) {
        EXPECT_EQ(source_images->images[i].size(), image_size);
    }

    double scale = 0.8;
    source_images->scale(scale);
    image_size.width = image_size.width * scale + 1;
    image_size.height  = image_size.height * scale;
    for (size_t i = 0; i < source_images->images.size(); ++i) {
        EXPECT_EQ(source_images->images[i].size().width, image_size.width);
        EXPECT_EQ(source_images->images[i].size().height, image_size.height);
    }

    scale = 0.31;
    source_images->scale(scale);
    image_size.width = image_size.width * scale + 1;
    image_size.height  = image_size.height * scale;
    for (size_t i = 0; i < source_images->images.size(); ++i) {
        EXPECT_EQ(source_images->images[i].size().width, image_size.width);
        EXPECT_EQ(source_images->images[i].size().height, image_size.height);
    }
}