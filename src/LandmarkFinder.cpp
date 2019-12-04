//
// This file is part of the stargazer library.
//
// Copyright 2016 Claudio Bandera <claudio.bandera@kit.edu (Karlsruhe Institute of Technology)
//
// The stargazer library is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// The stargazer library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>.

#include "LandmarkFinder.h"

#include <algorithm>
#include <limits>

#include <boost/range/adaptor/reversed.hpp>

using namespace stargazer;

///--------------------------------------------------------------------------------------///
/// Default constructor
///--------------------------------------------------------------------------------------///
LandmarkFinder::LandmarkFinder(std::string cfgfile) {

  /// set parameters

  maxRadiusForCluster = 40;
  minPointsPerLandmark = 5;
  maxPointsPerLandmark = 9;

  // Weight factors for corner detection
  cornerHypothesesCutoff = 1.0;
  maxCornerHypotheses = 10;
  fwLengthTriangle = 1.0;
  fwCrossProduct = 1.0;
  cornerAngleTolerance = 1.0;
  pointInsideTolerance = 1.0;

  blobParams.filterByArea = false;
  blobParams.filterByCircularity = false;
  blobParams.filterByColor = false;
  blobParams.filterByConvexity = false;
  blobParams.filterByInertia = false;
  blobParams.maxArea = std::numeric_limits<float>::max();
  blobParams.maxCircularity = 1.f;
  blobParams.maxConvexity = 1.f;
  blobParams.maxInertiaRatio = 1.f;
  blobParams.maxThreshold = 255.f;
  blobParams.minArea = 0.f;
  blobParams.minCircularity = 0.f;
  blobParams.minConvexity = 0.f;
  blobParams.minDistBetweenBlobs = 0.f;
  blobParams.minInertiaRatio = 0.f;
  blobParams.minRepeatability = 0;
  blobParams.minThreshold = 0.f;
  blobParams.thresholdStep = 50.f;

  /// Read in Landmark ids
  landmark_map_t landmarks;
  readMapConfig(cfgfile, landmarks);
  for (auto& el : landmarks) valid_ids_.push_back(el.first);
}

///--------------------------------------------------------------------------------------///
/// default destructor
///
///--------------------------------------------------------------------------------------///
LandmarkFinder::~LandmarkFinder() {}

///--------------------------------------------------------------------------------------///
/// FindMarker processing method
/// Handles the complete processing
///--------------------------------------------------------------------------------------///
int LandmarkFinder::DetectLandmarks(const cv::Mat& img,
                                    std::vector<ImgLandmark>& detected_landmarks) {
  clusteredPixels_.clear();
  clusteredPoints_.clear();

  /// check if input is valid
  // Explanation for CV_ Codes :
  // CV_[The number of bits per item][Signed or Unsigned][Type Prefix]C[The channel number]
  img.assignTo(grayImage_, CV_8UC1);  // 8bit unsigned with 3 channels
  if (!grayImage_.data) {             /// otherwise: return with error
    std::cerr << "Input data is invalid" << std::endl;
    return -1;
  }
  detected_landmarks.clear();

  /// This method finds bright points in image
  /// returns vector of center points of pixel groups
  clusteredPixels_ = FindBlobs(grayImage_);

  /// cluster points to groups which could be landmarks
  /// returns a vector of clusters which themselves are vectors of points
  FindClusters(clusteredPixels_, clusteredPoints_, maxRadiusForCluster, minPointsPerLandmark, maxPointsPerLandmark);

  /// on the clustered points, extract corners
  /// output is of type landmark, because now you can almost be certain that
  /// what you have is a landmark
  detected_landmarks = FindLandmarks(clusteredPoints_);
  //  std::cout << "Number of preliminary landmarks found: "<<
  //  detected_landmarks.size() << std::endl;

  return 0;
}

///--------------------------------------------------------------------------------------///
/// FindClusters groups points from input vector into groups
///
///--------------------------------------------------------------------------------------///
void LandmarkFinder::FindClusters(const std::vector<cv::Point>& points_in,
                                  std::vector<Cluster>& clusters,
                                  const float radiusThreshold,
                                  const unsigned int minPointsThreshold,
                                  const unsigned int maxPointsThreshold) {

  for (auto& thisPoint : points_in)  /// go thru all points
  {
    bool clusterFound = 0;  /// set flag that not used yet

    /// the last created cluster is most liley the one we are looking for
    for (auto& cluster : boost::adaptors::reverse(clusters)) {  /// go thru all clusters
      for (auto& clusterPoint : cluster) {  /// go thru all points in this cluster
        /// if distance is smaller than threshold, add point to cluster
        if (cv::norm(clusterPoint - thisPoint) <= radiusThreshold) {
          cluster.push_back(thisPoint);
          clusterFound = true;
          break;  /// because point has been added to cluster, no further search is neccessary
        }
      }

      if (clusterFound)  /// because point has been added to cluster, no further search is neccessary
        break;
    }

    if (!clusterFound)  /// not assigned to any cluster
    {
      Cluster newCluster;               /// create new cluster
      newCluster.push_back(thisPoint);  /// put this point in this new cluster
      clusters.push_back(newCluster);   /// add this cluster to the list
    }
  }

  /// second rule: check for minimum and maximum of points per cluster
  clusters.erase(std::remove_if(clusters.begin(),
                                clusters.end(),
                                [&](Cluster& cluster) {
                                  return (minPointsThreshold > cluster.size() ||
                                          maxPointsThreshold < cluster.size());
                                }),
                 clusters.end());
}

std::vector<cv::Point> LandmarkFinder::FindBlobs(cv::Mat& img_in) {

  // cv::Mat img;
  // bitwise_not (img_in, img);

  // BlobDetector with latest parameters
  std::vector<cv::KeyPoint> keypoints;
  cv::Ptr<cv::SimpleBlobDetector> detector = cv::SimpleBlobDetector::create(blobParams);
  detector->detect(img_in, keypoints);

  // two step conversion
  std::vector<cv::Point2f> points2f;
  cv::KeyPoint::convert(keypoints, points2f);
  std::vector<cv::Point> points;
  cv::Mat(points2f).convertTo(points, cv::Mat(points).type());

  return points;
}

///--------------------------------------------------------------------------------------///
/// FindCorners identifies the three corner points and sorts them into output vector
/// -> find three points which maximize a certain score for corner points
///--------------------------------------------------------------------------------------///
void LandmarkFinder::FindCorners(const std::vector<cv::Point>& point_list,
                                 std::vector<ImgLandmark>& hypotheses) {

  typedef std::pair<double, ImgLandmark> LmHypothesis;
  std::vector<LmHypothesis> scored_hypotheses;
  double best_score = std::numeric_limits<double>::lowest();  // Score for best combination of points

  /*  Numbering of corners and coordinate frame FOR THIS FUNCTION ONLY
   *       ---> y
   *  |   A   .   .   .
   *  |   .   .   .   .
   *  V   .   .   .   .
   *  x   S   .   .   B
   */

  /// Try all combinations of three points
  cv::Point pS, pA, pB, pH1, pH2;
  for (size_t i = 0; i < point_list.size(); i++) {
    pS = point_list[i];
    for (size_t j = 0; j < point_list.size(); j++) {
      pH1 = point_list[j];
      for (size_t k = j + 1; k < point_list.size(); k++) {
        pH2 = point_list[k];

        if (i == j || i == k) {
          // Skip double assignments
          continue;
        }

        // ensure rhs
        if (0. > (pH1 - pS).cross(pH2 - pS)) {
          pA = pH2;
          pB = pH1;
        } else {
          pA = pH1;
          pB = pH2;
        }

        cv::Point vSA = pA - pS;
        cv::Point vSB = pB - pS;
        cv::Point vBA = pB - pA;

        double normSA = cv::norm(vSA);
        double normSB = cv::norm(vSB);
        double normBA = cv::norm(vBA);

        // check angle between secants
        double cosangle = vSA.dot(vSB) / (normSA * normSB);
        if (std::abs(cosangle) > cornerAngleTolerance) {
          continue;
        }

        // check if each point is on correct side of assumed secant
        std::vector<cv::Point2f> local_points;
        local_points.reserve(point_list.size());
        cv::Mat(point_list).convertTo(local_points, cv::Mat(local_points).type());
        TransformToLocalPoints(pA, pS, pB, local_points);

        bool is_point_invalid = false;
        for (cv::Point2f pL : local_points) {
          if (!isInside<float>(pL.x, 0.0f, 1.0f, pointInsideTolerance) ||
              !isInside<float>(pL.y, 0.0f, 1.0f, pointInsideTolerance)) {
            is_point_invalid = true;
            break;
          }
        }
        if (is_point_invalid) {
          // skip corner hypothesis
          continue;
        }

        double score;
        {
          // The weight of the score components should be invariant to scaling
          // e.g. circumference linear vs area quadratic

          // Cross product (approximate area of stargazer pcb)
          double crossProduct = std::fabs(vSA.cross(vBA));

          // Triangle circumference
          double lengthTriangle = normSA + normSB + normBA;

          // Total score
          score = fwCrossProduct * crossProduct +
                  fwLengthTriangle * lengthTriangle * lengthTriangle;
        }


        // Keep hypothesis if its considered to be good (relative to current best)
        if (score < cornerHypothesesCutoff * best_score) {
          continue;
        } else {
          ImgLandmark lm;
          // corners
          lm.voCorners.push_back(pA);
          lm.voCorners.push_back(pS);
          lm.voCorners.push_back(pB);
          // id points
          for (size_t n = 0; n < point_list.size(); n++) {
            if (n == i || n == j || n == k) {
              continue;
            }
            lm.voIDPoints.push_back(point_list[n]);
          }
          scored_hypotheses.push_back(LmHypothesis(score, lm));
          if (score > best_score) {
            best_score = score;
          }
        }
      }
    }
  }
  // Sort out bad hypotheses (relative to total best)
  auto bad_end = std::remove_if(
      scored_hypotheses.begin(), scored_hypotheses.end(), [this, best_score](LmHypothesis lmh) {
        return lmh.first < cornerHypothesesCutoff * best_score;
      });

  // Sort by score
  std::sort(scored_hypotheses.begin(), bad_end, [](LmHypothesis a, LmHypothesis b) {
    return a.first > b.first;
  });

  // Append set of hypotheses
  for (auto it = scored_hypotheses.begin();
       it != bad_end && it != scored_hypotheses.begin() + maxCornerHypotheses;
       it++) {
    hypotheses.push_back(it->second);
  }
}

///--------------------------------------------------------------------------------------///
/// FindLandmarks identifies landmark inside a point cluster
///
///--------------------------------------------------------------------------------------///
std::vector<ImgLandmark> LandmarkFinder::FindLandmarks(const std::vector<Cluster>& clusteredPoints) {
  landmarkHypotheses_.clear();
  for (auto& cluster : clusteredPoints) {  /// go thru all clusters

    /// since most probably each cluster represents a landmark, create one
    ImgLandmark newLandmark;
    newLandmark.nID = 0;  /// we have not identified anything, so default ID is zero
    newLandmark.voIDPoints =
        cluster;  /// all points in this cluster are copied to the ID point vector for further examination

    /// FindCorners will move the three corner points into the corners vector
    FindCorners(cluster, landmarkHypotheses_);
  }
  std::vector<ImgLandmark> OutputLandmarks(landmarkHypotheses_);
  GetIDs(OutputLandmarks);

  /// done and return landmarks
  return OutputLandmarks;
}

///--------------------------------------------------------------------------------------///
/// CalculateIdForward sorts the given idPoints and calculates the id
///
///--------------------------------------------------------------------------------------///
bool LandmarkFinder::CalculateIdForward(ImgLandmark& landmark,
                                        std::vector<uint16_t>& valid_ids) {
  std::vector<cv::Point2f> local_points;
  cv::Mat(landmark.voIDPoints).convertTo(local_points, cv::Mat(local_points).type());
  TransformToLocalPoints(
      landmark.voCorners.at(0), landmark.voCorners.at(1), landmark.voCorners.at(2), local_points);

  /// the total ID
  uint16_t ID = 0;

  /// go thru all ID points in this landmark structure
  for (const auto& p : local_points) {
    int nX = static_cast<int>(0.5f + (DIM - 1) * p.x);
    int nY = static_cast<int>(0.5f + (DIM - 1) * p.y);

    nX = std::clamp(nX, 0, DIM - 1);
    nY = std::clamp(nY, 0, DIM - 1);

    /// the binary values ar coded:
    /// x steps are binary shifts within 4 bit blocks
    /// y steps are binary shifts of 4 bit blocks
    /// see http://hagisonic.com/ for more information on this
    ID += static_cast<uint16_t>((1 << nX) << DIM * nY);
  }

  /// validate with the vector of available IDs
  std::vector<uint16_t>::iterator idIterator =
      std::find(valid_ids.begin(), valid_ids.end(), ID);
  if (idIterator != valid_ids.end()) {  /// ID matches one which is available:
    valid_ids.erase(idIterator);        /// remove this ID
    landmark.nID = ID;
    return true;
  } else {  /// no ID match
    return false;
  }
}

///--------------------------------------------------------------------------------------///
/// CalculateIdBackward searches in the filtered image for id points, given the corners.
///
///--------------------------------------------------------------------------------------///
bool LandmarkFinder::CalculateIdBackward(ImgLandmark& landmark,
                                         std::vector<uint16_t>& valid_ids) {
  uint16_t ID = 0;
  const float threshold = 128.f;

  /// now we delete the previously detected points and go the other way around
  landmark.voIDPoints.clear();

  const cv::Point& x0y0 = landmark.voCorners.at(0);
  const cv::Point& x1y0 = landmark.voCorners.at(1);
  const cv::Point& x1y1 = landmark.voCorners.at(2);

  std::vector<cv::Point2f> id_points;
  std::vector<uint16_t> id_point_values;
  id_points.reserve(DIM * DIM - 3);
  id_point_values.reserve(DIM * DIM - 3);

  // setup id points in local coordinates and their encoded value
  for (int nX = 0; nX < DIM; nX++) {
    for (int nY = 0; nY < DIM; nY++) {
      /// skip corner points
      if ((nX == 0 && nY == 0) || (nX == 0 && nY == DIM - 1) || (nX == DIM - 1 && nY == 0)) {
        continue;
      }
      id_points.push_back(cv::Point2f(float(nX) / (DIM - 1), float(nY) / (DIM - 1)));
      id_point_values.push_back(static_cast<uint16_t>((1 << (DIM - 1 - nX)) << DIM * nY));
    }
  }

  /// same as before: finde affine transformation, but this time from landmark
  /// coordinates to image coordinates
  TransformToGlobalPoints(x0y0, x1y0, x1y1, id_points);

  /// check image for bright spots
  for (size_t n = 0; n < id_points.size(); n++) {
    cv::Point img_point(id_points[n].x, id_points[n].y);
    if (0 > img_point.x || 0 > img_point.y || grayImage_.cols <= img_point.x ||
        grayImage_.rows <= img_point.y) {
      // corner hypothesis suggest id points outside of visible area. No safe detection possible.
      return false;
    }
    if (threshold < grayImage_.at<uint8_t>(img_point.y,
                                           img_point.x)) {  /// todo: this might be extended to some area
      ID += id_point_values[n];
    }
  }

  /// now, same as before, validate with available IDs
  std::vector<uint16_t>::iterator idIt =
      std::find(valid_ids.begin(), valid_ids.end(), ID);
  if (idIt != valid_ids.end()) {
    valid_ids.erase(idIt);
    landmark.nID = ID;
    return true;
  } else {
    return false;
  }
}

///--------------------------------------------------------------------------------------///
/// GetIDs is to identify the ID of a landmark according to the point pattern
/// see http://hagisonic.com/ for information on pattern
///--------------------------------------------------------------------------------------///
int LandmarkFinder::GetIDs(std::vector<ImgLandmark>& landmarks) {
  /// get vector of possible IDs
  std::vector<uint16_t> validIDs = valid_ids_;

  auto unknownLandmarksBegin = std::remove_if(
      landmarks.begin(), landmarks.end(), [this, &validIDs](ImgLandmark& lm) {
        return !CalculateIdForward(lm, validIDs);
      });

  unknownLandmarksBegin = std::remove_if(
      unknownLandmarksBegin, landmarks.end(), [this, &validIDs](ImgLandmark& lm) {
        return !CalculateIdBackward(lm, validIDs);
      });

  landmarks.erase(unknownLandmarksBegin, landmarks.end());
  return 0;  // TODO What defines success?
}

void LandmarkFinder::TransformToLocalPoints(const cv::Point2f& x0y0,
                                            const cv::Point2f& x1y0,
                                            const cv::Point2f& x1y1,
                                            std::vector<cv::Point2f>& p) {
  std::transform(
      p.begin(), p.end(), p.begin(), [x0y0](cv::Point2f p) { return p - x0y0; });

  cv::Point2f vX = x1y0 - x0y0;
  cv::Point2f vY = x1y1 - x1y0;

  cv::Matx22f transform(vX.x, vY.x, vX.y, vY.y);
  cv::transform(p, p, transform.inv());
}

void LandmarkFinder::TransformToGlobalPoints(const cv::Point2f& x0y0,
                                             const cv::Point2f& x1y0,
                                             const cv::Point2f& x1y1,
                                             std::vector<cv::Point2f>& p) {
  cv::Point2f vX = x1y0 - x0y0;
  cv::Point2f vY = x1y1 - x1y0;
  std::transform(p.begin(), p.end(), p.begin(), [x0y0, vX, vY](cv::Point2f p) {
    return x0y0 + p.x * vX + p.y * vY;
  });
}
