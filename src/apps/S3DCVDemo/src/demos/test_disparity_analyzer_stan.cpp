/**
 * Compares s3d feature matching with a reference dataset
 */

#include "dataset_loader_vsg.h"
#include "demo_utils.h"

#include <s3d/cv/rectification/rectifier_cv.h>
#include <s3d/math.h>

int main(int argc, char* argv[]) {
  DatasetLoaderVSG dataset = createDatasetLoader(argc, argv);

  cv::Mat imgLeft = dataset.loadImageLeft();
  cv::Mat imgRight = dataset.loadImageRight();
  s3d::Size imageSize{imgLeft.cols, imgLeft.rows};

  // solve F with DisparityAnalyzerStan (feature, matching, ransac)
  s3d::DisparityAnalyzerSTAN analyzer;
  analyzer.analyze(imgLeft, imgRight);
  s3d::StanAlignment alignment = analyzer.results.stan.alignment;
  Eigen::Matrix3d F = s3d::StanFundamentalMatrixSolver::CenteredFundamentalMatrixFromAlignment(alignment, imageSize);

  // display matches
  std::vector<Eigen::Vector2f> ptsLeft = analyzer.results.stan.featuresLeft,
                               ptsRight = analyzer.results.stan.featuresRight;
  s3d::displayMatches("Feature Matches (After Ransac)", imgLeft, imgRight, ptsLeft, ptsRight);

  // display epilines
  cv::Mat imgLeftEpilines, imgRightEpilines;
  std::tie(imgLeftEpilines, imgRightEpilines) =
      s3d::drawEpipolarLines(s3d::eigenMatToCV(F),
                             imgLeft,
                             imgRight,
                             s3d::eigenPointsToCV(ptsLeft),
                             s3d::eigenPointsToCV(ptsRight));

  cv::Mat imgEpilines(imgLeftEpilines.rows, imgLeftEpilines.cols * 2, CV_8UC3);
  cv::hconcat(imgLeftEpilines, imgRightEpilines, imgEpilines);
  cv::imshow("Model Epilines", imgEpilines);

  // display model rectification
  Eigen::Matrix3f H1 = s3d::RectificationStan::centeredLeftImageMatrix(alignment, imageSize);
  Eigen::Matrix3f H2 = s3d::RectificationStan::centeredRightImageMatrix(alignment, imageSize);

  s3d::RectifierCV rectifier;
  cv::Mat imgLeftRect = rectifier.rectifyCV(imgLeftEpilines, s3d::eigenMatToCV(H1));
  cv::Mat imgRightRect = rectifier.rectifyCV(imgRightEpilines, s3d::eigenMatToCV(H2));

  cv::Mat imgRect(imgLeftRect.rows, imgLeftRect.cols * 2, CV_8UC3);
  cv::hconcat(imgLeftRect, imgRightRect, imgRect);
  cv::imshow("Rectified Pairs With Epilines", imgRect);

  cv::waitKey(0);
}
