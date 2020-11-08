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

#include "CeresLocalizer.h"

#include <ceres/ceres.h>

#include <limits>

#include "internal/CostFunction.h"

using namespace stargazer;

CeresLocalizer::CeresLocalizer(const std::string& cam_cfgfile,
                               const std::string& map_cfgfile,
                               bool estimate_2d_pose)
    : Localizer(cam_cfgfile, map_cfgfile),
      estimate_2d_pose(estimate_2d_pose),
      z_upper_bound(std::numeric_limits<double>::max()) {

  // Convert landmark points to worldcoordinates once.
  for (auto& el : landmarks) {
    for (auto& pt : el.second.points) {
      double x, y, z;
      transformLandMarkToWorld(pt[(int)POINT::X],
                               pt[(int)POINT::Y],
                               el.second.pose.position.data(),
                               el.second.pose.orientation.data(),
                               &x,
                               &y,
                               &z);
      pt = {x, y, z};
      z_upper_bound = std::min(z_upper_bound, z);
    }
  }
  z_upper_bound -= 1.;  // Assumption: Camera is at least 1m below the stargazer landmarks

  is_initialized = false;
}

void CeresLocalizer::UpdatePose(std::vector<ImgLandmark>& img_landmarks, float dt) {
  if (img_landmarks.empty()) {
    std::cout << "Localizer received empty landmarks vector" << std::endl;
    return;
  }

  if (!is_initialized) {
    // TODO std::accumulate
    for (auto& el : img_landmarks) {
      ego_pose.position[(int)POINT::X] += landmarks[el.nID].pose.position[(int)POINT::X];
      ego_pose.position[(int)POINT::Y] += landmarks[el.nID].pose.position[(int)POINT::Y];
    }
    ego_pose.position[(int)POINT::X] /= img_landmarks.size();
    ego_pose.position[(int)POINT::Y] /= img_landmarks.size();
    // is_initialized = true;
  }

  // Delete old data
  ClearResidualBlocks();

  // Add new data
  AddResidualBlocks(img_landmarks);

  // Prevents local minimum with all points behind camera (allowed by camera model)
  // Assumes that camera is approximately looking into positive z direction (map)
  if (problem.HasParameterBlock(ego_pose.position.data()))
    problem.SetParameterUpperBound(ego_pose.position.data(), (int)POINT::Z, z_upper_bound);

  // Set Quaternion Paramterization (4 variables but 3 dof)
  static bool is_local_parametrization_set = false;
  if (problem.HasParameterBlock(ego_pose.orientation.data()) && !is_local_parametrization_set) {
    problem.SetParameterization(ego_pose.orientation.data(),
                                new ceres::QuaternionParameterization());
    is_local_parametrization_set = true;
  }

  // Set Camera Parameters Constant
  if (problem.HasParameterBlock(camera_intrinsics.data()))
    problem.SetParameterBlockConstant(camera_intrinsics.data());

  // Optimize
  Optimize();
}

void CeresLocalizer::ClearResidualBlocks() {
  std::vector<ceres::ResidualBlockId> residual_blocks;
  problem.GetResidualBlocks(&residual_blocks);
  for (auto& block : residual_blocks) {
    problem.RemoveResidualBlock(block);
  }
}

void CeresLocalizer::AddResidualBlocks(std::vector<ImgLandmark> img_landmarks) {
  for (auto& img_lm : img_landmarks) {

    if (img_lm.idPoints.size() + img_lm.corners.size() !=
        landmarks[img_lm.nID].points.size()) {
      std::cerr << "point count does not match! "
                << img_lm.idPoints.size() + img_lm.corners.size()
                << "(observed) vs. " << landmarks[img_lm.nID].points.size()
                << "(map)\t ID: " << img_lm.nID << std::endl;
      return;
    };

    // Add residual block, for every one of the seen points.
    for (size_t k = 0; k < NUM_CORNERS; k++) {
      ceres::CostFunction* cost_function;
      // if (k < NUM_CORNERS) {
      cost_function = WorldToImageReprojectionFunctor::Create(
          img_lm.corners[k].x,
          img_lm.corners[k].y,
          landmarks[img_lm.nID].points[k][(int)POINT::X],
          landmarks[img_lm.nID].points[k][(int)POINT::Y],
          landmarks[img_lm.nID].points[k][(int)POINT::Z]);
      // Don't use inner points for localization (speeds up optimization by
      // approximately by factor 2) } else {
      //   cost_function = WorldToImageReprojectionFunctor::Create(
      //       img_lm.idPoints[k - NUM_CORNERS].x,
      //       img_lm.idPoints[k - NUM_CORNERS].y,
      //       landmarks[img_lm.nID].points[k][(int)POINT::X],
      //       landmarks[img_lm.nID].points[k][(int)POINT::Y],
      //       landmarks[img_lm.nID].points[k][(int)POINT::Z]);
      // }
      // CauchyLoss(9): a pixel-error of 3 is still considered as inlayer
      problem.AddResidualBlock(cost_function,
                               new ceres::CauchyLoss(9),
                               ego_pose.position.data(),
                               ego_pose.orientation.data(),
                               camera_intrinsics.data());
    }
  }
  if (!is_initialized) {
    if (estimate_2d_pose) {

      problem.SetParameterization(ego_pose.position.data(),
                                  new ceres::SubsetParameterization(
                                      (int)POINT::N_PARAMS, {{(int)POINT::Z}}));

      problem.SetParameterization(
          ego_pose.orientation.data(),
          new ceres::SubsetParameterization((int)QUAT::N_PARAMS,
                                            {{(int)QUAT::X, (int)QUAT::Y}}));

      ego_pose.position[(int)POINT::Z] = 0.;
      ego_pose.orientation[(int)QUAT::X] = 0.;
      ego_pose.orientation[(int)QUAT::Y] = 0.;
    }
    is_initialized = true;
  }
}

void CeresLocalizer::Optimize() {
  ceres::Solver::Options options;
  // set optimization settings
  options.linear_solver_type = ceres::SPARSE_NORMAL_CHOLESKY;
  options.minimizer_progress_to_stdout = false;
  // options.max_num_iterations = 20;
  // options.function_tolerance = 0.0000000000000001;
  // options.gradient_tolerance = 0.0000000000000001;
  // options.parameter_tolerance = 0.0000000000000001;
  // options.min_relative_decrease = 0.0000000000000001;

  ceres::Solve(options, &problem, &summary);
}
