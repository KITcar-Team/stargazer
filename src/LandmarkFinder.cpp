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
#include <numeric>
#include <boost/range/adaptor/reversed.hpp>

using namespace std;
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
    fwLengthTriangle = 0.6;
    fwProjectedSecantLength = 30.0;
    fwSecantsLengthDiff = 3.0;
    hypotenuseTolerance = 0.8;

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
    for (auto& el : landmarks)
        valid_ids_.push_back(el.first);
}

///--------------------------------------------------------------------------------------///
/// default destructor
///
///--------------------------------------------------------------------------------------///
LandmarkFinder::~LandmarkFinder() {
}

///--------------------------------------------------------------------------------------///
/// FindMarker processing method
/// Handles the complete processing
///--------------------------------------------------------------------------------------///
int LandmarkFinder::DetectLandmarks(const cv::Mat& img, std::vector<ImgLandmark>& detected_landmarks) {
    clusteredPixels_.clear();
    clusteredPoints_.clear();

    /// check if input is valid
    // Explanation for CV_ Codes :
    // CV_[The number of bits per item][Signed or Unsigned][Type Prefix]C[The channel number]
    img.assignTo(grayImage_, CV_8UC1); // 8bit unsigned with 3 channels
    if (!grayImage_.data) {            /// otherwise: return with error
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
void LandmarkFinder::FindClusters(const std::vector<cv::Point>& points_in, std::vector<Cluster>& clusters,
                                  const float radiusThreshold, const unsigned int minPointsThreshold,
                                  const unsigned int maxPointsThreshold) {

    for (auto& thisPoint : points_in) /// go thru all points
    {
        bool clusterFound = 0; /// set flag that not used yet

        /// the last created cluster is most liley the one we are looking for
        for (auto& cluster : boost::adaptors::reverse(clusters)) { /// go thru all clusters
            for (auto& clusterPoint : cluster) {                   /// go thru all points in this cluster
                /// if distance is smaller than threshold, add point to cluster
                if (cv::norm(clusterPoint - thisPoint) <= radiusThreshold) {
                    cluster.push_back(thisPoint);
                    clusterFound = true;
                    break; /// because point has been added to cluster, no further search is neccessary
                }
            }

            if (clusterFound) /// because point has been added to cluster, no further search is neccessary
                break;
        }

        if (!clusterFound) /// not assigned to any cluster
        {
            Cluster newCluster;              /// create new cluster
            newCluster.push_back(thisPoint); /// put this point in this new cluster
            clusters.push_back(newCluster);  /// add this cluster to the list
        }
    }

    /// second rule: check for minimum and maximum of points per cluster
    clusters.erase(std::remove_if(clusters.begin(), clusters.end(),
                                  [&](Cluster& cluster) {
                                      return (minPointsThreshold > cluster.size() ||
                                              maxPointsThreshold < cluster.size());
                                  }),
                   clusters.end());
}

std::vector<cv::Point> LandmarkFinder::FindBlobs(cv::Mat& img_in) {

    //cv::Mat img;
    //bitwise_not (img_in, img);

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
bool LandmarkFinder::FindCorners(std::vector<cv::Point>& point_list, std::vector<cv::Point>& corner_points) {

    float best_score = std::numeric_limits<float>::lowest(); // Score for best combination of points

    /*  Numbering of corners and coordinate frame FOR THIS FUNCTION ONLY // TODO use normal numbering
     *       ---> y
     *  |   1   .   .   .
     *  |   .   .   .   .
     *  V   .   .   .   .
     *  x   3   .   .   2
     */

    /// Try all combinations of three points
    bool corners_found = false;

    cv::Point pS, pH1, pH2; // corner hypothesis where S ist not part of hypotenuse
    cv::Point cornerS, cornerH1, cornerH2; // current most probable corner hypothesis
    cv::Point corner1, corner2, corner3; // most probable corner hypothesis (as right hand system)
    for (size_t i = 0; i < point_list.size(); i++) {
        pS = point_list[i];
        for (size_t j = 0; j < point_list.size(); j++) {
            pH1 = point_list[j];
            for (size_t k = j+1; k < point_list.size(); k++) {
                pH2 = point_list[k];

                if (i==j || i==k) {
                    // Skip double assignments
                    continue;
                }

                cv::Point s1 = pS - pH1;
                cv::Point s2 = pS - pH2;
                cv::Point h = pH2 - pH1;

                float normS1 = cv::norm(s1);
                float normS2 = cv::norm(s2);
                float nH = cv::norm(h);

                /// Check if vH12 is reasonable hypotenuse
                if (normS1 > hypotenuseTolerance * nH || normS2 > hypotenuseTolerance * nH) {
                    // Skip unprobable hypotenuse
                    continue;
                }

                float lengthTriangle = normS1 + normS2 + nH;
                float projectedSecantLength, secantsLengthDiff;
                {
                    cv::Point s1 = pS - pH1;
                    cv::Point s2 = pS - pH2;
                    // Project s1 onto s2 -> resulting length should be close to zero
                    // TODO use cross product?
                    projectedSecantLength = std::abs(s1.dot(s2)) / (normS1 * normS2);
                    secantsLengthDiff = fabs(normS1 - normS2);
                }

                float score = fwLengthTriangle * lengthTriangle -
                              fwProjectedSecantLength * projectedSecantLength -
                              fwSecantsLengthDiff * secantsLengthDiff;
                if (best_score < score) {
                    /// remember their addresses and distance
                    corners_found = true;
                    cornerS = pS;
                    cornerH1 = pH1;
                    cornerH2 = pH2;
                    best_score = score;
                }
            }
        }
    }
    if (!corners_found) {
        return false;
    }

    // Check whether H1 is corner 1 or 2
    cv::Point vSH1 = cornerH1 - cornerS;
    cv::Point vSH2 = cornerH2 - cornerS;
    corner3 = cornerS;
    if(0. < vSH1.cross(vSH2)) {
        corner1 = cornerH1;
        corner2 = cornerH2;
    } else {
        corner1 = cornerH2;
        corner2 = cornerH1;
    }

    /// Move corner points from point list to empty corner point list

    /// Store in output container (order is important)
    corner_points.push_back(corner1);
    corner_points.push_back(corner3);
    corner_points.push_back(corner2);

    /// Remove from input list
    point_list.erase(std::remove(point_list.begin(), point_list.end(), corner1), point_list.end());
    point_list.erase(std::remove(point_list.begin(), point_list.end(), corner2), point_list.end());
    point_list.erase(std::remove(point_list.begin(), point_list.end(), corner3), point_list.end());

    return true;
}

///--------------------------------------------------------------------------------------///
/// FindLandmarks identifies landmark inside a point cluster
///
///--------------------------------------------------------------------------------------///
std::vector<ImgLandmark> LandmarkFinder::FindLandmarks(const std::vector<Cluster>& clusteredPoints) {

    std::vector<ImgLandmark> OutputLandmarks;

    for (auto& cluster : clusteredPoints) { /// go thru all clusters

        /// since most probably each cluster represents a landmark, create one
        ImgLandmark newLandmark;
        newLandmark.nID = 0; /// we have not identified anything, so default ID is zero
        newLandmark.voIDPoints =
            cluster; /// all points in this cluster are copied to the ID point vector for further examination

        /// FindCorners will move the three corner points into the corners vector
        if (!FindCorners(newLandmark.voIDPoints, newLandmark.voCorners))
            continue;

        /// add this landmark to the landmark vector
        OutputLandmarks.push_back(newLandmark);
    }
    landmarkHypotheses_ = OutputLandmarks;
    GetIDs(OutputLandmarks);

    /// done and return landmarks
    return OutputLandmarks;
}

///--------------------------------------------------------------------------------------///
/// CalculateIdForward sorts the given idPoints and calculates the id
///
///--------------------------------------------------------------------------------------///
bool LandmarkFinder::CalculateIdForward(ImgLandmark& landmark, std::vector<uint16_t>& valid_ids) {
    // TODO clean up this function
    /// first of all: get the three corner points
    const cv::Point* oCornerOne = &landmark.voCorners.at(0);
    const cv::Point* oCornerTwo = &landmark.voCorners.at(1);
    const cv::Point* oCornerThree = &landmark.voCorners.at(2);

    cv::Point oTwoOne = *oCornerOne - *oCornerTwo;
    cv::Point oTwoThree = *oCornerThree - *oCornerTwo;

    /// at this point we have a right hand system in image coordinates
    /// now, we find the affine transformation which maps the landmark from
    /// image coordinate in a landmark-related coordinate frame
    /// with the corners defined as (0,0), (1,0) and (0,1)

    /// for this, we just compute the inverse of the two side vectors
    cv::Mat Transform(2, 2, CV_32FC1);
    Transform.at<float>(0, 0) = float(oTwoOne.x);
    Transform.at<float>(1, 0) = float(oTwoOne.y);
    Transform.at<float>(0, 1) = float(oTwoThree.x);
    Transform.at<float>(1, 1) = float(oTwoThree.y);

    Transform = Transform.inv();

    /// now we have a transform which maps [0,1028]x[0,1280] -> [0,1]x[0,1],
    /// i.e. our landmark is in the latter do
    ///

    /// next, the ID points are transformed accordingly and then matched to
    /// their binary values

    /// the point under examination
    cv::Mat ThisPoint(2, 1, CV_32FC1);

    /// the total ID
    uint16_t ID = 0;

    /// go thru all ID points in this landmark structure
    std::vector<uint16_t> pPointsIDs;
    for (const auto& pPoints : landmark.voIDPoints) {
        /// first step: bring the ID point in relation to the origin of the
        /// landmark
        ThisPoint.at<float>(0, 0) = float(pPoints.x - oCornerTwo->x);
        ThisPoint.at<float>(1, 0) = float(pPoints.y - oCornerTwo->y);

        /// apply transfrom
        ThisPoint = Transform * ThisPoint;

        /// next step is the quantization in values between 0 and 3
        float x = ThisPoint.at<float>(0, 0);
        float y = ThisPoint.at<float>(1, 0);

        /// it's 1-y because in the definition of the landmark ID the x axis runs
        /// down
        int nY = floor((y) / 0.25);
        int nX = floor((1 - x) / 0.25);

        nX = nX < 0 ? 0 : nX;
        nX = nX > 3 ? 3 : nX;
        nY = nY < 0 ? 0 : nY;
        nY = nY > 3 ? 3 : nY;

        /// the binary values ar coded: x steps are binary shifts within 4 bit
        /// blocks
        ///                             y steps are binary shifts of 4 bit blocks
        ///                             see http://hagisonic.com/ for more
        ///                             information on this
        uint16_t ThisPointID = static_cast<uint16_t>((1 << nX) << 4 * nY);
        pPointsIDs.push_back(ThisPointID);

        /// add this point's contribution to the landmark ID
        ID += ThisPointID;
    }

    /// Sort points
    /* The order of id points
    *      x   3   7   .
    *      1   4   8   12
    *      2   5   9   13
    *      x   6   10  x
    */
    parallel_vector_sort(pPointsIDs, landmark.voIDPoints);

    /// assign ID to landmark
    landmark.nID = ID;

    /// validate with the vector of available IDs
    std::vector<uint16_t>::iterator idIterator = std::find(valid_ids.begin(), valid_ids.end(), landmark.nID);
    if (idIterator != valid_ids.end()) { /// ID matches one which is available:
        valid_ids.erase(idIterator);     /// remove this ID
        return true;
    } else { /// no ID match
        return false;
    }
}

///--------------------------------------------------------------------------------------///
/// CalculateIdBackward searches in the filtered image for id points, given the corners.
///
///--------------------------------------------------------------------------------------///
bool LandmarkFinder::CalculateIdBackward(ImgLandmark& landmark, std::vector<uint16_t>& valid_ids) {
    // TOD clean up this function
    uint16_t nThisID = 0;
    const float threshold = 128.f;

    /// same as before: finde affine transformation, but this time from landmark
    /// coordinates to image coordinates
    const cv::Point* oCornerOne = &landmark.voCorners.at(0);
    const cv::Point* oCornerTwo = &landmark.voCorners.at(1);
    const cv::Point* oCornerThree = &landmark.voCorners.at(2);

    cv::Point oTwoOne = *oCornerOne - *oCornerTwo;
    cv::Point oTwoThree = *oCornerThree - *oCornerTwo;

    /// make it a right hand system
    if (0 > oTwoOne.cross(oTwoThree)) {
        std::swap(oCornerOne, oCornerThree);
        std::swap(oTwoOne, oTwoThree);
        std::swap(landmark.voCorners.at(0), landmark.voCorners.at(2));
    }

    /// now we delete the previously detected points and go the other way around
    landmark.voIDPoints.clear();

    cv::Mat Transform(2, 2, CV_32FC1);
    Transform.at<float>(0, 0) = float(oTwoOne.x);
    Transform.at<float>(0, 1) = float(oTwoThree.x);
    Transform.at<float>(1, 0) = float(oTwoOne.y);
    Transform.at<float>(1, 1) = float(oTwoThree.y);

    cv::Mat ThisPoint(2, 1, CV_32FC1);
    std::vector<uint16_t> pPointsIDs;

    /// go thru all possible ID points and see if the image has a high gray
    /// value there, i.e. there's light
    for (int nX = 0; nX < 4; nX++) {
        for (int nY = 0; nY < 4; nY++) {
            /// this must not be done for the three corner points of course
            if ((nX != 0 || nY != 0) && (nX != 0 || nY != 3) && (nX != 3 || nY != 0)) {
                uint16_t ThisPointID = 0;
                /// since we know the corners, we can go in thirds between them to see
                /// if theres a light
                ThisPoint.at<float>(0, 0) = float(nX) * 0.333;
                ThisPoint.at<float>(1, 0) = float(nY) * 0.333;

                ThisPoint = Transform * ThisPoint;

                ThisPoint.at<float>(0, 0) += float(oCornerTwo->x);
                ThisPoint.at<float>(1, 0) += float(oCornerTwo->y);

                cv::Point Index(int(ThisPoint.at<float>(0, 0)), int(ThisPoint.at<float>(1, 0)));

                /// same as for the pixel detection: see if the gray value at the
                /// point where the light should be exceeds a threshold and thus
                /// supports the light hypothesis
                if (0 > Index.x || 0 > Index.y || grayImage_.cols <= Index.x || grayImage_.rows <= Index.y) {
                    continue;
                }

                if (threshold < grayImage_.at<uint8_t>(Index.y,
                                                       Index.x)) { /// todo: this might be extended to some area
                    ThisPointID = static_cast<uint16_t>((1 << (3 - nX)) << 4 * nY);
                    landmark.voIDPoints.push_back(Index);
                    pPointsIDs.push_back(ThisPointID);
                }

                /// add the contribution to the total ID
                nThisID += ThisPointID;
            }
        }
    }

    /// Sort points
    /* The order of id points
    *      x   3   7   .
    *      1   4   8   12
    *      2   5   9   13
    *      x   6   10  x
    */
    parallel_vector_sort(pPointsIDs, landmark.voIDPoints);

    landmark.nID = nThisID;

    /// now, same as before, validate with available IDs
    std::vector<uint16_t>::iterator pIDLUTIt = std::find(valid_ids.begin(), valid_ids.end(), nThisID);
    /// if the new ID is valid, enqueue the landmark again
    if (pIDLUTIt != valid_ids.end()) {
        valid_ids.erase(pIDLUTIt);
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
    /*  Numbering of corners and coordinate frame
     *       ---> y
     *  |   1   .   .   .
     *  |   .   .   .   .
     *  V   .   .   .   .
     *  x   2   .   .   3
     */
    /// get vector of possible IDs
    std::vector<uint16_t> validIDs = valid_ids_;

    /// vector of iterators to Landmarks which where not identified correctly
    /// once we've been through all landmarks, we can look up available IDs in the
    /// vector define above.
    /// this is why we remember errors but don't correct them right away.
    std::vector<ImgLandmark> landmarksInQueue;

    /// First, try to use the id points given to determine landmark id.
    std::vector<ImgLandmark>::iterator pLandmarkIt = landmarks.begin();
    while (pLandmarkIt != landmarks.end()) {

        if (CalculateIdForward(*pLandmarkIt, validIDs)) {
            ++pLandmarkIt; /// go to next landmark
        } else {
            landmarksInQueue.push_back(*pLandmarkIt); /// put this landmark in queue for second processing run
            landmarks.erase(pLandmarkIt); /// delete it from valid landmark list. This also is a step to next landmark
        }
    }

    /// now, go thru all landmarks which did not match a valid ID and try to match them to one of the remaining
    for (auto& landmark : landmarksInQueue) {
        if (CalculateIdBackward(landmark, validIDs)) {
            landmarks.push_back(landmark);
        } else {
            ; /// go to next landmark
        }
    }

    return 0;
}

void LandmarkFinder::parallel_vector_sort(std::vector<uint16_t>& ids, std::vector<cv::Point>& points) {
    size_t len = ids.size();
    size_t stepsize = len / 2; // Zu Beginn ist die Lücke über den halben Array.
    bool b = true;
    while (b) {
        b = false; // b bleibt auf false, wenn kein einziges Mal etwas falsch ist.
        for (size_t i = 0; i < len; i++) {
            if (stepsize + i >= len) // Schutz vor Speicherfehlern
            {
                break;
            }
            if (ids[i] > ids[i + stepsize]) // überprüft ob die zwei Elemente falsch herum sind
            {
                std::swap(ids[i], ids[i + stepsize]); // wenn ja -> vertauschen
                std::swap(points[i], points[i + stepsize]);
                b = true;
            }
        }
        stepsize = stepsize / 1.3; // Lücke verkleinern für nächsten Durchlauf
        if (stepsize < 1) {
            stepsize = 1;
        }
    }
}
