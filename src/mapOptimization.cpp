#include <jsk_topic_tools/color_utils.h>
#include "factor.h"
#include "lio_segmot/Diagnosis.h"
#include "lio_segmot/ObjectStateArray.h"
#include "lio_segmot/cloud_info.h"
#include "lio_segmot/detection.h"
#include "lio_segmot/flags.h"
#include "lio_segmot/save_estimation_result.h"
#include "lio_segmot/save_map.h"
#include "solver.h"
#include "utility.h"

#include <visualization_msgs/MarkerArray.h>

#include <gtsam/geometry/Pose3.h>
#include <gtsam/geometry/Rot3.h>
#include <gtsam/inference/Symbol.h>
#include <gtsam/navigation/CombinedImuFactor.h>
#include <gtsam/navigation/GPSFactor.h>
#include <gtsam/navigation/ImuFactor.h>
#include <gtsam/nonlinear/LevenbergMarquardtOptimizer.h>
#include <gtsam/nonlinear/Marginals.h>
#include <gtsam/nonlinear/NonlinearFactorGraph.h>
#include <gtsam/nonlinear/Values.h>
#include <gtsam/slam/BetweenFactor.h>
#include <gtsam/slam/PriorFactor.h>

// #define ENABLE_COMPACT_VERSION_OF_FACTOR_GRAPH
// #define MAP_OPTIMIZATION_DEBUG
#define ENABLE_SIMULTANEOUS_LOCALIZATION_AND_TRACKING
#define ENABLE_ASYNCHRONOUS_STATE_ESTIMATE_FOR_SLOT
// #define ENABLE_MINIMAL_MEMORY_USAGE

using namespace gtsam;

using symbol_shorthand::B;  // Bias  (ax,ay,az,gx,gy,gz)
using symbol_shorthand::G;  // GPS pose
using symbol_shorthand::V;  // Vel   (xdot,ydot,zdot)
using symbol_shorthand::X;  // Pose3 (x,y,z,r,p,y)

using BoundingBox              = jsk_recognition_msgs::BoundingBox;
using BoundingBoxPtr           = jsk_recognition_msgs::BoundingBoxPtr;
using BoundingBoxConstPtr      = jsk_recognition_msgs::BoundingBoxConstPtr;
using BoundingBoxArray         = jsk_recognition_msgs::BoundingBoxArray;
using BoundingBoxArrayPtr      = jsk_recognition_msgs::BoundingBoxArrayPtr;
using BoundingBoxArrayConstPtr = jsk_recognition_msgs::BoundingBoxArrayConstPtr;
/*
 * A point cloud type that has 6D pose info ([x,y,z,roll,pitch,yaw] intensity is time stamp)
 */
struct PointXYZIRPYT {
  PCL_ADD_POINT4D
  PCL_ADD_INTENSITY;  // preferred way of adding a XYZ+padding
  float roll;
  float pitch;
  float yaw;
  double time;
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW  // make sure our new allocators are aligned
} EIGEN_ALIGN16;                   // enforce SSE padding for correct memory alignment

POINT_CLOUD_REGISTER_POINT_STRUCT(PointXYZIRPYT,
                                  (float, x, x)(float, y, y)(float, z, z)(float, intensity, intensity)(float, roll, roll)(float, pitch, pitch)(float, yaw, yaw)(double, time, time))

typedef PointXYZIRPYT PointTypePose;

geometry_msgs::Pose gtsamPose2ROSPose(const Pose3& pose) {
  geometry_msgs::Pose p;
  auto trans   = pose.translation();
  p.position.x = trans.x();
  p.position.y = trans.y();
  p.position.z = trans.z();

  auto quat       = pose.rotation().toQuaternion();
  p.orientation.w = quat.w();
  p.orientation.x = quat.x();
  p.orientation.y = quat.y();
  p.orientation.z = quat.z();

  return p;
}

class ObjectState {
 public:
  Pose3 pose                      = Pose3::identity();
  Pose3 velocity                  = Pose3::identity();
  uint64_t poseNodeIndex          = 0;
  uint64_t velocityNodeIndex      = 0;
  uint64_t objectIndex            = 0;
  uint64_t objectIndexForTracking = 0;
  int lostCount                   = 0;
  int trackScore                  = 0;
  ros::Time timestamp             = ros::Time();

  BoundingBox box       = BoundingBox();
  BoundingBox detection = BoundingBox();
  double confidence     = 0;

  bool isTightlyCoupled = false;
  bool isFirst          = false;

  TightlyCoupledDetectionFactor::shared_ptr tightlyCoupledDetectionFactorPtr = nullptr;
  LooselyCoupledDetectionFactor::shared_ptr looselyCoupledDetectionFactorPtr = nullptr;
  StablePoseFactor::shared_ptr motionFactorPtr                               = nullptr;

  double initialDetectionError = 0;
  double initialMotionError    = 0;

  std::vector<uint64_t> previousVelocityNodeIndices;

  ObjectState(Pose3 pose                                        = Pose3::identity(),
              Pose3 velocity                                    = Pose3::identity(),
              uint64_t poseNodeIndex                            = 0,
              uint64_t velocityNodeIndex                        = 0,
              uint64_t objectIndex                              = 0,
              uint64_t objectIndexForTracking                   = 0,
              int lostCount                                     = 0,
              int trackScore                                    = 0,
              ros::Time timestamp                               = ros::Time(),
              BoundingBox box                                   = BoundingBox(),
              BoundingBox detection                             = BoundingBox(),
              double confidence                                 = 0,
              bool isTightlyCoupled                             = false,
              bool isFirst                                      = false,
              std::vector<uint64_t> previousVelocityNodeIndices = std::vector<uint64_t>())
      : pose(pose),
        velocity(velocity),
        poseNodeIndex(poseNodeIndex),
        velocityNodeIndex(velocityNodeIndex),
        objectIndex(objectIndex),
        objectIndexForTracking(objectIndexForTracking),
        lostCount(lostCount),
        trackScore(trackScore),
        timestamp(timestamp),
        box(box),
        detection(detection),
        confidence(confidence),
        isTightlyCoupled(isTightlyCoupled),
        isFirst(isFirst),
        previousVelocityNodeIndices(previousVelocityNodeIndices) {
  }

  ObjectState clone() const {
    return ObjectState(pose,
                       velocity,
                       poseNodeIndex,
                       velocityNodeIndex,
                       objectIndex,
                       objectIndexForTracking,
                       lostCount,
                       trackScore,
                       timestamp,
                       box,
                       detection,
                       confidence,
                       isTightlyCoupled,
                       isFirst,
                       previousVelocityNodeIndices);
  }

  bool isTurning(float threshold) const {
    auto rot = gtsam::traits<gtsam::Rot3>::Local(gtsam::Rot3::identity(), this->velocity.rotation());
    return rot.maxCoeff() > threshold;
  }

  bool isMovingFast(float threshold) const {
    auto v = gtsam::traits<gtsam::Pose3>::Local(gtsam::Pose3::identity(), this->velocity);
    return sqrt(pow(v(3), 2) + pow(v(4), 2) + pow(v(5), 2)) > threshold;
  }

  bool velocityIsConsistent(int samplingSize,
                            Values& currentEstimates,
                            double angleThreshold,
                            double velocityThreshold) const {
    int size = previousVelocityNodeIndices.size();

    if (size < samplingSize) return false;

    Eigen::VectorXd angles     = Eigen::VectorXd::Zero(samplingSize);
    Eigen::VectorXd velocities = Eigen::VectorXd::Zero(samplingSize);
    std::vector<gtsam::Vector6> vs;
    gtsam::Vector6 vMean = gtsam::Vector6::Zero();
    for (int i = 0; i < samplingSize; ++i) {
      auto vi       = currentEstimates.at<gtsam::Pose3>(previousVelocityNodeIndices[size - i - 1]);
      auto v        = gtsam::traits<gtsam::Pose3>::Local(gtsam::Pose3::identity(), vi);
      angles(i)     = sqrt(pow(v(0), 2) + pow(v(1), 2) + pow(v(2), 2));
      velocities(i) = sqrt(pow(v(3), 2) + pow(v(4), 2) + pow(v(5), 2));
      vs.push_back(v);
      vMean += v;
    }
    vMean /= samplingSize;
    gtsam::Matrix6 covariance = gtsam::Matrix6::Zero();
    covariance(0, 0) = covariance(1, 1) = covariance(2, 2) = angleThreshold;
    covariance(3, 3) = covariance(4, 4) = covariance(5, 5) = velocityThreshold;
    auto covarianceInverse                                 = covariance.inverse();
    double error                                           = 0.0;
    for (int i = 0; i < samplingSize; ++i) {
      auto v = vs[i] - vMean;
      error += v.transpose() * covarianceInverse * v;
    }
    error /= samplingSize;

    double angleVar    = (angles.array() - angles.mean()).pow(2).mean();
    double velocityVar = (velocities.array() - velocities.mean()).pow(2).mean();

    // return angleVar < angleThreshold && velocityVar < velocityThreshold;

    return error < 1.0 * 1.0;
  }
};

class Timer {
 private:
  std::chrono::time_point<std::chrono::high_resolution_clock> start;
  std::chrono::time_point<std::chrono::high_resolution_clock> end;

 public:
  Timer() {
    start = std::chrono::high_resolution_clock::now();
  }

  void reset() {
    start = std::chrono::high_resolution_clock::now();
  }

  void stop() {
    end = std::chrono::high_resolution_clock::now();
  }

  double elapsed() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  }
};

class mapOptimization : public ParamServer {
 public:
  // gtsam
  NonlinearFactorGraph gtSAMgraph;
  NonlinearFactorGraph gtSAMgraphForLooselyCoupledObjects;
  Values initialEstimate;
  Values initialEstimateForLooselyCoupledObjects;
  Values initialEstimateForAnalysis;
  Values optimizedEstimate;
  MaxMixtureISAM2* isam;
  Values isamCurrentEstimate;
  Eigen::MatrixXd poseCovariance;

  ros::Publisher pubLaserCloudSurround;
  ros::Publisher pubLaserOdometryGlobal;
  ros::Publisher pubLaserOdometryIncremental;
  ros::Publisher pubKeyPoses;
  ros::Publisher pubPath;
  ros::Publisher pubKeyFrameCloud;

  ros::Publisher pubHistoryKeyFrames;
  ros::Publisher pubIcpKeyFrames;
  ros::Publisher pubRecentKeyFrames;
  ros::Publisher pubRecentKeyFrame;
  ros::Publisher pubCloudRegisteredRaw;
  ros::Publisher pubLoopConstraintEdge;

  ros::Subscriber subCloud;
  ros::Subscriber subGPS;
  ros::Subscriber subLoop;

  ros::Publisher pubDetection;
  ros::Publisher pubLaserCloudDeskewed;
  ros::Publisher pubObjects;
  ros::Publisher pubObjectPaths;
  ros::Publisher pubTightlyCoupledObjectPoints;
  ros::Publisher pubObjectLabels;
  ros::Publisher pubObjectVelocities;
  ros::Publisher pubObjectVelocityArrows;
  ros::Publisher pubObjectStates;
  ros::Publisher pubTrackingObjects;
  ros::Publisher pubTrackingObjectPaths;
  ros::Publisher pubTrackingObjectLabels;
  ros::Publisher pubTrackingObjectVelocities;
  ros::Publisher pubTrackingObjectVelocityArrows;

  ros::Publisher pubDiagnosis;

  ros::Publisher pubReady;

  ros::ServiceServer srvSaveMap;
  ros::ServiceServer srvSaveEstimationResult;

  ros::ServiceClient detectionClient;
  lio_segmot::detection detectionSrv;

  std::deque<nav_msgs::Odometry> gpsQueue;
  lio_segmot::cloud_info cloudInfo;

  vector<pcl::PointCloud<PointType>::Ptr> cornerCloudKeyFrames;
  vector<pcl::PointCloud<PointType>::Ptr> surfCloudKeyFrames;

  pcl::PointCloud<PointType>::Ptr cloudKeyPoses3D;
  pcl::PointCloud<PointTypePose>::Ptr cloudKeyPoses6D;
  pcl::PointCloud<PointType>::Ptr copy_cloudKeyPoses3D;
  pcl::PointCloud<PointTypePose>::Ptr copy_cloudKeyPoses6D;
  std::vector<uint64_t> keyPoseIndices;

  pcl::PointCloud<PointType>::Ptr laserCloudCornerLast;    // corner feature set from odoOptimization
  pcl::PointCloud<PointType>::Ptr laserCloudSurfLast;      // surf feature set from odoOptimization
  pcl::PointCloud<PointType>::Ptr laserCloudCornerLastDS;  // downsampled corner featuer set from odoOptimization
  pcl::PointCloud<PointType>::Ptr laserCloudSurfLastDS;    // downsampled surf featuer set from odoOptimization

  pcl::PointCloud<PointType>::Ptr laserCloudOri;
  pcl::PointCloud<PointType>::Ptr coeffSel;

  std::vector<PointType> laserCloudOriCornerVec;  // corner point holder for parallel computation
  std::vector<PointType> coeffSelCornerVec;
  std::vector<bool> laserCloudOriCornerFlag;
  std::vector<PointType> laserCloudOriSurfVec;  // surf point holder for parallel computation
  std::vector<PointType> coeffSelSurfVec;
  std::vector<bool> laserCloudOriSurfFlag;

  map<int, pair<pcl::PointCloud<PointType>, pcl::PointCloud<PointType>>> laserCloudMapContainer;
  pcl::PointCloud<PointType>::Ptr laserCloudCornerFromMap;
  pcl::PointCloud<PointType>::Ptr laserCloudSurfFromMap;
  pcl::PointCloud<PointType>::Ptr laserCloudCornerFromMapDS;
  pcl::PointCloud<PointType>::Ptr laserCloudSurfFromMapDS;

  pcl::KdTreeFLANN<PointType>::Ptr kdtreeCornerFromMap;
  pcl::KdTreeFLANN<PointType>::Ptr kdtreeSurfFromMap;

  pcl::KdTreeFLANN<PointType>::Ptr kdtreeSurroundingKeyPoses;
  pcl::KdTreeFLANN<PointType>::Ptr kdtreeHistoryKeyPoses;

  pcl::VoxelGrid<PointType> downSizeFilterCorner;
  pcl::VoxelGrid<PointType> downSizeFilterSurf;
  pcl::VoxelGrid<PointType> downSizeFilterICP;
  pcl::VoxelGrid<PointType> downSizeFilterSurroundingKeyPoses;  // for surrounding key poses of scan-to-map optimization

  ros::Time timeLaserInfoStamp;
  double timeLaserInfoCur;
  double deltaTime;

  float transformTobeMapped[6];

  std::mutex mtx;
  std::mutex mtxLoopInfo;

  bool isDegenerate = false;
  cv::Mat matP;

  int laserCloudCornerFromMapDSNum = 0;
  int laserCloudSurfFromMapDSNum   = 0;
  int laserCloudCornerLastDSNum    = 0;
  int laserCloudSurfLastDSNum      = 0;

  bool aLoopIsClosed = false;
  map<int, int> loopIndexContainer;  // from new to old
  vector<pair<int, int>> loopIndexQueue;
  vector<gtsam::Pose3> loopPoseQueue;
  vector<gtsam::noiseModel::Diagonal::shared_ptr> loopNoiseQueue;
  deque<std_msgs::Float64MultiArray> loopInfoVec;

  nav_msgs::Path globalPath;

  Eigen::Affine3f transPointAssociateToMap;
  Eigen::Affine3f incrementalOdometryAffineFront;
  Eigen::Affine3f incrementalOdometryAffineBack;

  BoundingBoxArrayPtr detections;
  std::vector<Detection> detectionVector;
  std::vector<Detection> tightlyCoupledDetectionVector;
  std::vector<Detection> earlyLooselyCoupledMatchingVector;
  std::vector<Detection> looselyCoupledMatchingVector;
  std::vector<Detection> tightlyCoupledMatchingVector;
  std::vector<Detection> dataAssociationVector;
  bool detectionIsActive = false;
  std::vector<std::map<uint64_t, ObjectState>> objects;
  visualization_msgs::MarkerArray objectPaths;
  visualization_msgs::Marker tightlyCoupledObjectPoints;
  visualization_msgs::MarkerArray objectLabels;
  visualization_msgs::MarkerArray objectVelocities;
  visualization_msgs::MarkerArray objectVelocityArrows;
  visualization_msgs::MarkerArray trackingObjectPaths;
  visualization_msgs::MarkerArray trackingObjectLabels;
  visualization_msgs::MarkerArray trackingObjectVelocities;
  visualization_msgs::MarkerArray trackingObjectVelocityArrows;
  lio_segmot::ObjectStateArray objectStates;
  uint64_t numberOfRegisteredObjects = 0;
  uint64_t numberOfTrackingObjects   = 0;
  bool anyObjectIsTightlyCoupled     = false;

  uint64_t numberOfNodes = 0;

  Timer timer;
  int numberOfTightlyCoupledObjectsAtThisMoment = 0;

  mapOptimization() {
    ISAM2Params parameters;
    parameters.relinearizeThreshold = 0.1;
    parameters.relinearizeSkip      = 1;
    isam                            = new MaxMixtureISAM2(parameters);

    pubKeyPoses                 = nh.advertise<sensor_msgs::PointCloud2>("lio_segmot/mapping/trajectory", 1);
    pubLaserCloudSurround       = nh.advertise<sensor_msgs::PointCloud2>("lio_segmot/mapping/map_global", 1);
    pubLaserOdometryGlobal      = nh.advertise<nav_msgs::Odometry>("lio_segmot/mapping/odometry", 1);
    pubLaserOdometryIncremental = nh.advertise<nav_msgs::Odometry>("lio_segmot/mapping/odometry_incremental", 1);
    pubPath                     = nh.advertise<nav_msgs::Path>("lio_segmot/mapping/path", 1);

    subCloud = nh.subscribe<lio_segmot::cloud_info>("lio_segmot/feature/cloud_info", 1, &mapOptimization::laserCloudInfoHandler, this, ros::TransportHints().tcpNoDelay());
    subGPS   = nh.subscribe<nav_msgs::Odometry>(gpsTopic, 200, &mapOptimization::gpsHandler, this, ros::TransportHints().tcpNoDelay());
    subLoop  = nh.subscribe<std_msgs::Float64MultiArray>("lio_loop/loop_closure_detection", 1, &mapOptimization::loopInfoHandler, this, ros::TransportHints().tcpNoDelay());

    srvSaveMap              = nh.advertiseService("lio_segmot/save_map", &mapOptimization::saveMapService, this);
    srvSaveEstimationResult = nh.advertiseService("lio_segmot/save_estimation_result", &mapOptimization::saveEstimationResultService, this);
    detectionClient         = nh.serviceClient<lio_segmot::detection>("lio_segmot_detector");

    pubHistoryKeyFrames   = nh.advertise<sensor_msgs::PointCloud2>("lio_segmot/mapping/icp_loop_closure_history_cloud", 1);
    pubIcpKeyFrames       = nh.advertise<sensor_msgs::PointCloud2>("lio_segmot/mapping/icp_loop_closure_corrected_cloud", 1);
    pubLoopConstraintEdge = nh.advertise<visualization_msgs::MarkerArray>("/lio_segmot/mapping/loop_closure_constraints", 1);

    pubRecentKeyFrames    = nh.advertise<sensor_msgs::PointCloud2>("lio_segmot/mapping/map_local", 1);
    pubRecentKeyFrame     = nh.advertise<sensor_msgs::PointCloud2>("lio_segmot/mapping/cloud_registered", 1);
    pubCloudRegisteredRaw = nh.advertise<sensor_msgs::PointCloud2>("lio_segmot/mapping/cloud_registered_raw", 1);

    pubDetection                  = nh.advertise<BoundingBoxArray>("lio_segmot/mapping/detections", 1);
    pubLaserCloudDeskewed         = nh.advertise<sensor_msgs::PointCloud2>("lio_segmot/mapping/cloud_deskewed", 1);
    pubObjects                    = nh.advertise<BoundingBoxArray>("lio_segmot/mapping/objects", 1);
    pubObjectPaths                = nh.advertise<visualization_msgs::MarkerArray>("lio_segmot/mapping/object_paths", 1);
    pubTightlyCoupledObjectPoints = nh.advertise<visualization_msgs::Marker>("lio_segmot/mapping/tightly_coupled_object_points", 1);
    pubObjectLabels               = nh.advertise<visualization_msgs::MarkerArray>("lio_segmot/mapping/object_labels", 1);
    pubObjectVelocities           = nh.advertise<visualization_msgs::MarkerArray>("lio_segmot/mapping/object_velocities", 1);
    pubObjectVelocityArrows       = nh.advertise<visualization_msgs::MarkerArray>("lio_segmot/mapping/object_velocity_arrows", 1);
    pubObjectStates               = nh.advertise<lio_segmot::ObjectStateArray>("lio_segmot/mapping/object_states", 1);

    pubTrackingObjects              = nh.advertise<BoundingBoxArray>("lio_segmot/tracking/objects", 1);
    pubTrackingObjectPaths          = nh.advertise<visualization_msgs::MarkerArray>("lio_segmot/tracking/object_paths", 1);
    pubTrackingObjectLabels         = nh.advertise<visualization_msgs::MarkerArray>("lio_segmot/tracking/object_labels", 1);
    pubTrackingObjectVelocities     = nh.advertise<visualization_msgs::MarkerArray>("lio_segmot/tracking/object_velocities", 1);
    pubTrackingObjectVelocityArrows = nh.advertise<visualization_msgs::MarkerArray>("lio_segmot/tracking/object_velocity_arrows", 1);

    pubDiagnosis = nh.advertise<lio_segmot::Diagnosis>("lio_segmot/diagnosis", 1);

    pubReady = nh.advertise<std_msgs::Empty>("lio_segmot/ready", 1);

    downSizeFilterCorner.setLeafSize(mappingCornerLeafSize, mappingCornerLeafSize, mappingCornerLeafSize);
    downSizeFilterSurf.setLeafSize(mappingSurfLeafSize, mappingSurfLeafSize, mappingSurfLeafSize);
    downSizeFilterICP.setLeafSize(mappingSurfLeafSize, mappingSurfLeafSize, mappingSurfLeafSize);
    downSizeFilterSurroundingKeyPoses.setLeafSize(surroundingKeyframeDensity, surroundingKeyframeDensity, surroundingKeyframeDensity);  // for surrounding key poses of scan-to-map optimization

    allocateMemory();
  }

  void allocateMemory() {
    cloudKeyPoses3D.reset(new pcl::PointCloud<PointType>());
    cloudKeyPoses6D.reset(new pcl::PointCloud<PointTypePose>());
    copy_cloudKeyPoses3D.reset(new pcl::PointCloud<PointType>());
    copy_cloudKeyPoses6D.reset(new pcl::PointCloud<PointTypePose>());

    kdtreeSurroundingKeyPoses.reset(new pcl::KdTreeFLANN<PointType>());
    kdtreeHistoryKeyPoses.reset(new pcl::KdTreeFLANN<PointType>());

    laserCloudCornerLast.reset(new pcl::PointCloud<PointType>());    // corner feature set from odoOptimization
    laserCloudSurfLast.reset(new pcl::PointCloud<PointType>());      // surf feature set from odoOptimization
    laserCloudCornerLastDS.reset(new pcl::PointCloud<PointType>());  // downsampled corner featuer set from odoOptimization
    laserCloudSurfLastDS.reset(new pcl::PointCloud<PointType>());    // downsampled surf featuer set from odoOptimization

    laserCloudOri.reset(new pcl::PointCloud<PointType>());
    coeffSel.reset(new pcl::PointCloud<PointType>());

    laserCloudOriCornerVec.resize(N_SCAN * Horizon_SCAN);
    coeffSelCornerVec.resize(N_SCAN * Horizon_SCAN);
    laserCloudOriCornerFlag.resize(N_SCAN * Horizon_SCAN);
    laserCloudOriSurfVec.resize(N_SCAN * Horizon_SCAN);
    coeffSelSurfVec.resize(N_SCAN * Horizon_SCAN);
    laserCloudOriSurfFlag.resize(N_SCAN * Horizon_SCAN);

    std::fill(laserCloudOriCornerFlag.begin(), laserCloudOriCornerFlag.end(), false);
    std::fill(laserCloudOriSurfFlag.begin(), laserCloudOriSurfFlag.end(), false);

    laserCloudCornerFromMap.reset(new pcl::PointCloud<PointType>());
    laserCloudSurfFromMap.reset(new pcl::PointCloud<PointType>());
    laserCloudCornerFromMapDS.reset(new pcl::PointCloud<PointType>());
    laserCloudSurfFromMapDS.reset(new pcl::PointCloud<PointType>());

    kdtreeCornerFromMap.reset(new pcl::KdTreeFLANN<PointType>());
    kdtreeSurfFromMap.reset(new pcl::KdTreeFLANN<PointType>());

    for (int i = 0; i < 6; ++i) {
      transformTobeMapped[i] = 0;
    }

    matP = cv::Mat(6, 6, CV_32F, cv::Scalar::all(0));

    detections.reset(new BoundingBoxArray());

    tightlyCoupledObjectPoints.action             = visualization_msgs::Marker::ADD;
    tightlyCoupledObjectPoints.type               = visualization_msgs::Marker::SPHERE_LIST;
    tightlyCoupledObjectPoints.color.a            = 0.4;
    tightlyCoupledObjectPoints.color.r            = 1.0;
    tightlyCoupledObjectPoints.color.g            = 1.0;
    tightlyCoupledObjectPoints.color.b            = 1.0;
    tightlyCoupledObjectPoints.scale.x            = 1.0;
    tightlyCoupledObjectPoints.scale.y            = 1.0;
    tightlyCoupledObjectPoints.scale.z            = 1.0;
    tightlyCoupledObjectPoints.pose.orientation.w = 1.0;
  }

  void laserCloudInfoHandler(const lio_segmot::cloud_infoConstPtr& msgIn) {
    // extract time stamp
    timeLaserInfoStamp = msgIn->header.stamp;
    timeLaserInfoCur   = msgIn->header.stamp.toSec();

    // extract info and feature cloud
    cloudInfo = *msgIn;
    pcl::fromROSMsg(msgIn->cloud_corner, *laserCloudCornerLast);
    pcl::fromROSMsg(msgIn->cloud_surface, *laserCloudSurfLast);

    std::lock_guard<std::mutex> lock(mtx);

    timer.reset();
    numberOfTightlyCoupledObjectsAtThisMoment = 0;

    static double timeLastProcessing = -1;
    if (timeLaserInfoCur - timeLastProcessing >= mappingProcessInterval) {
#ifdef ENABLE_SIMULTANEOUS_LOCALIZATION_AND_TRACKING
      std::thread t(&mapOptimization::getDetections, this);

      deltaTime          = timeLaserInfoCur - timeLastProcessing;
      timeLastProcessing = timeLaserInfoCur;
#endif

      updateInitialGuess();

      extractSurroundingKeyFrames();

      downsampleCurrentScan();

      scan2MapOptimization();

#ifdef ENABLE_SIMULTANEOUS_LOCALIZATION_AND_TRACKING
      t.join();
#endif

      saveKeyFramesAndFactor();

      correctPoses();

      timer.stop();

      publishOdometry();

      publishFrames();
    }

    pubReady.publish(std_msgs::Empty());
  }

  void getDetections() {
    detectionIsActive          = false;
    detectionSrv.request.cloud = cloudInfo.cloud_raw;
    if (detectionClient.call(detectionSrv)) {
      *detections       = detectionSrv.response.detections;
      detectionIsActive = true;
    }
  }

  void gpsHandler(const nav_msgs::Odometry::ConstPtr& gpsMsg) {
    gpsQueue.push_back(*gpsMsg);
  }

  void pointAssociateToMap(PointType const* const pi, PointType* const po) {
    po->x         = transPointAssociateToMap(0, 0) * pi->x + transPointAssociateToMap(0, 1) * pi->y + transPointAssociateToMap(0, 2) * pi->z + transPointAssociateToMap(0, 3);
    po->y         = transPointAssociateToMap(1, 0) * pi->x + transPointAssociateToMap(1, 1) * pi->y + transPointAssociateToMap(1, 2) * pi->z + transPointAssociateToMap(1, 3);
    po->z         = transPointAssociateToMap(2, 0) * pi->x + transPointAssociateToMap(2, 1) * pi->y + transPointAssociateToMap(2, 2) * pi->z + transPointAssociateToMap(2, 3);
    po->intensity = pi->intensity;
  }

  pcl::PointCloud<PointType>::Ptr transformPointCloud(pcl::PointCloud<PointType>::Ptr cloudIn, PointTypePose* transformIn) {
    pcl::PointCloud<PointType>::Ptr cloudOut(new pcl::PointCloud<PointType>());

    int cloudSize = cloudIn->size();
    cloudOut->resize(cloudSize);

    Eigen::Affine3f transCur = pcl::getTransformation(transformIn->x, transformIn->y, transformIn->z, transformIn->roll, transformIn->pitch, transformIn->yaw);

#pragma omp parallel for num_threads(numberOfCores)
    for (int i = 0; i < cloudSize; ++i) {
      const auto& pointFrom         = cloudIn->points[i];
      cloudOut->points[i].x         = transCur(0, 0) * pointFrom.x + transCur(0, 1) * pointFrom.y + transCur(0, 2) * pointFrom.z + transCur(0, 3);
      cloudOut->points[i].y         = transCur(1, 0) * pointFrom.x + transCur(1, 1) * pointFrom.y + transCur(1, 2) * pointFrom.z + transCur(1, 3);
      cloudOut->points[i].z         = transCur(2, 0) * pointFrom.x + transCur(2, 1) * pointFrom.y + transCur(2, 2) * pointFrom.z + transCur(2, 3);
      cloudOut->points[i].intensity = pointFrom.intensity;
    }
    return cloudOut;
  }

  gtsam::Pose3 pclPointTogtsamPose3(PointTypePose thisPoint) {
    return gtsam::Pose3(gtsam::Rot3::RzRyRx(double(thisPoint.roll), double(thisPoint.pitch), double(thisPoint.yaw)),
                        gtsam::Point3(double(thisPoint.x), double(thisPoint.y), double(thisPoint.z)));
  }

  gtsam::Pose3 trans2gtsamPose(float transformIn[]) {
    return gtsam::Pose3(gtsam::Rot3::RzRyRx(transformIn[0], transformIn[1], transformIn[2]),
                        gtsam::Point3(transformIn[3], transformIn[4], transformIn[5]));
  }

  Eigen::Affine3f pclPointToAffine3f(PointTypePose thisPoint) {
    return pcl::getTransformation(thisPoint.x, thisPoint.y, thisPoint.z, thisPoint.roll, thisPoint.pitch, thisPoint.yaw);
  }

  Eigen::Affine3f trans2Affine3f(float transformIn[]) {
    return pcl::getTransformation(transformIn[3], transformIn[4], transformIn[5], transformIn[0], transformIn[1], transformIn[2]);
  }

  PointTypePose trans2PointTypePose(float transformIn[]) {
    PointTypePose thisPose6D;
    thisPose6D.x     = transformIn[3];
    thisPose6D.y     = transformIn[4];
    thisPose6D.z     = transformIn[5];
    thisPose6D.roll  = transformIn[0];
    thisPose6D.pitch = transformIn[1];
    thisPose6D.yaw   = transformIn[2];
    return thisPose6D;
  }

  bool saveMapService(lio_segmot::save_mapRequest& req, lio_segmot::save_mapResponse& res) {
    string saveMapDirectory;

    cout << "****************************************************" << endl;
    cout << "Saving map to pcd files ..." << endl;
    if (req.destination.empty())
      saveMapDirectory = std::getenv("HOME") + savePCDDirectory;
    else
      saveMapDirectory = std::getenv("HOME") + req.destination;
    cout << "Save destination: " << saveMapDirectory << endl;
    // create directory and remove old files;
    int unused = system((std::string("exec rm -r ") + saveMapDirectory).c_str());
    unused     = system((std::string("mkdir -p ") + saveMapDirectory).c_str());
    // save key frame transformations
    pcl::io::savePCDFileBinary(saveMapDirectory + "/trajectory.pcd", *cloudKeyPoses3D);
    pcl::io::savePCDFileBinary(saveMapDirectory + "/transformations.pcd", *cloudKeyPoses6D);
    // extract global point cloud map
    pcl::PointCloud<PointType>::Ptr globalCornerCloud(new pcl::PointCloud<PointType>());
    pcl::PointCloud<PointType>::Ptr globalCornerCloudDS(new pcl::PointCloud<PointType>());
    pcl::PointCloud<PointType>::Ptr globalSurfCloud(new pcl::PointCloud<PointType>());
    pcl::PointCloud<PointType>::Ptr globalSurfCloudDS(new pcl::PointCloud<PointType>());
    pcl::PointCloud<PointType>::Ptr globalMapCloud(new pcl::PointCloud<PointType>());
    for (int i = 0; i < (int)cloudKeyPoses3D->size(); i++) {
      *globalCornerCloud += *transformPointCloud(cornerCloudKeyFrames[i], &cloudKeyPoses6D->points[i]);
      *globalSurfCloud += *transformPointCloud(surfCloudKeyFrames[i], &cloudKeyPoses6D->points[i]);
      cout << "\r" << std::flush << "Processing feature cloud " << i << " of " << cloudKeyPoses6D->size() << " ...";
    }

    if (req.resolution != 0) {
      cout << "\n\nSave resolution: " << req.resolution << endl;

      // down-sample and save corner cloud
      downSizeFilterCorner.setInputCloud(globalCornerCloud);
      downSizeFilterCorner.setLeafSize(req.resolution, req.resolution, req.resolution);
      downSizeFilterCorner.filter(*globalCornerCloudDS);
      pcl::io::savePCDFileBinary(saveMapDirectory + "/CornerMap.pcd", *globalCornerCloudDS);
      // down-sample and save surf cloud
      downSizeFilterSurf.setInputCloud(globalSurfCloud);
      downSizeFilterSurf.setLeafSize(req.resolution, req.resolution, req.resolution);
      downSizeFilterSurf.filter(*globalSurfCloudDS);
      pcl::io::savePCDFileBinary(saveMapDirectory + "/SurfMap.pcd", *globalSurfCloudDS);
    } else {
      // save corner cloud
      pcl::io::savePCDFileBinary(saveMapDirectory + "/CornerMap.pcd", *globalCornerCloud);
      // save surf cloud
      pcl::io::savePCDFileBinary(saveMapDirectory + "/SurfMap.pcd", *globalSurfCloud);
    }

    // save global point cloud map
    *globalMapCloud += *globalCornerCloud;
    *globalMapCloud += *globalSurfCloud;

    int ret     = pcl::io::savePCDFileBinary(saveMapDirectory + "/GlobalMap.pcd", *globalMapCloud);
    res.success = ret == 0;

    downSizeFilterCorner.setLeafSize(mappingCornerLeafSize, mappingCornerLeafSize, mappingCornerLeafSize);
    downSizeFilterSurf.setLeafSize(mappingSurfLeafSize, mappingSurfLeafSize, mappingSurfLeafSize);

    cout << "****************************************************" << endl;
    cout << "Saving map to pcd files completed\n"
         << endl;

    return true;
  }

  bool saveEstimationResultService(lio_segmot::save_estimation_resultRequest& req, lio_segmot::save_estimation_resultResponse& res) {
    res.robotTrajectory            = globalPath;
    res.objectTrajectories         = std::vector<nav_msgs::Path>(numberOfRegisteredObjects, nav_msgs::Path());
    res.objectVelocities           = std::vector<nav_msgs::Path>(numberOfRegisteredObjects, nav_msgs::Path());
    res.trackingObjectTrajectories = std::vector<nav_msgs::Path>(numberOfTrackingObjects, nav_msgs::Path());
    res.trackingObjectVelocities   = std::vector<nav_msgs::Path>(numberOfTrackingObjects, nav_msgs::Path());
    res.trackingObjectStates       = std::vector<lio_segmot::ObjectStateArray>(numberOfTrackingObjects, lio_segmot::ObjectStateArray());
    res.objectFlags                = std::vector<lio_segmot::flags>(numberOfRegisteredObjects, lio_segmot::flags());
    res.trackingObjectFlags        = std::vector<lio_segmot::flags>(numberOfTrackingObjects, lio_segmot::flags());
    for (int t = 0; t < objects.size(); ++t) {
      for (const auto& pairedObject : objects[t]) {
        const auto& object = pairedObject.second;

        if (object.lostCount > 0) continue;

        geometry_msgs::PoseStamped ps;
        ps.header.frame_id = odometryFrame;
        ps.header.stamp    = object.timestamp;
        ps.pose            = gtsamPose2ROSPose(isamCurrentEstimate.at<Pose3>(object.poseNodeIndex));
        res.objectTrajectories[object.objectIndex].poses.push_back(ps);
        res.trackingObjectTrajectories[object.objectIndexForTracking].poses.push_back(ps);

        ps.pose = gtsamPose2ROSPose(isamCurrentEstimate.at<Pose3>(object.velocityNodeIndex));
        res.objectVelocities[object.objectIndex].poses.push_back(ps);
        res.trackingObjectVelocities[object.objectIndexForTracking].poses.push_back(ps);

        res.objectFlags[object.objectIndex].flags.push_back(object.isTightlyCoupled ? 1 : 0);
        res.trackingObjectFlags[object.objectIndexForTracking].flags.push_back(object.isTightlyCoupled ? 1 : 0);

        lio_segmot::ObjectState state;
        state.header.frame_id = odometryFrame;
        state.header.stamp    = object.timestamp;
        state.pose            = gtsamPose2ROSPose(isamCurrentEstimate.at<Pose3>(object.poseNodeIndex));
        state.velocity        = gtsamPose2ROSPose(isamCurrentEstimate.at<Pose3>(object.velocityNodeIndex));
        state.detection       = object.detection;
        res.trackingObjectStates[object.objectIndexForTracking].objects.push_back(state);
      }
    }
    return true;
  }

  void visualizeGlobalMapThread() {
    ros::Rate rate(0.2);
    while (ros::ok()) {
      rate.sleep();
      publishGlobalMap();
    }

    if (savePCD == false)
      return;

    lio_segmot::save_mapRequest req;
    lio_segmot::save_mapResponse res;

    if (!saveMapService(req, res)) {
      cout << "Fail to save map" << endl;
    }
  }

  void publishGlobalMap() {
    if (pubLaserCloudSurround.getNumSubscribers() == 0)
      return;

    if (cloudKeyPoses3D->points.empty() == true)
      return;

    pcl::KdTreeFLANN<PointType>::Ptr kdtreeGlobalMap(new pcl::KdTreeFLANN<PointType>());
    ;
    pcl::PointCloud<PointType>::Ptr globalMapKeyPoses(new pcl::PointCloud<PointType>());
    pcl::PointCloud<PointType>::Ptr globalMapKeyPosesDS(new pcl::PointCloud<PointType>());
    pcl::PointCloud<PointType>::Ptr globalMapKeyFrames(new pcl::PointCloud<PointType>());
    pcl::PointCloud<PointType>::Ptr globalMapKeyFramesDS(new pcl::PointCloud<PointType>());

    // kd-tree to find near key frames to visualize
    std::vector<int> pointSearchIndGlobalMap;
    std::vector<float> pointSearchSqDisGlobalMap;
    // search near key frames to visualize
    mtx.lock();
    kdtreeGlobalMap->setInputCloud(cloudKeyPoses3D);
    kdtreeGlobalMap->radiusSearch(cloudKeyPoses3D->back(), globalMapVisualizationSearchRadius, pointSearchIndGlobalMap, pointSearchSqDisGlobalMap, 0);
    mtx.unlock();

    for (int i = 0; i < (int)pointSearchIndGlobalMap.size(); ++i)
      globalMapKeyPoses->push_back(cloudKeyPoses3D->points[pointSearchIndGlobalMap[i]]);
    // downsample near selected key frames
    pcl::VoxelGrid<PointType> downSizeFilterGlobalMapKeyPoses;                                                                                             // for global map visualization
    downSizeFilterGlobalMapKeyPoses.setLeafSize(globalMapVisualizationPoseDensity, globalMapVisualizationPoseDensity, globalMapVisualizationPoseDensity);  // for global map visualization
    downSizeFilterGlobalMapKeyPoses.setInputCloud(globalMapKeyPoses);
    downSizeFilterGlobalMapKeyPoses.filter(*globalMapKeyPosesDS);
    for (auto& pt : globalMapKeyPosesDS->points) {
      kdtreeGlobalMap->nearestKSearch(pt, 1, pointSearchIndGlobalMap, pointSearchSqDisGlobalMap);
      pt.intensity = cloudKeyPoses3D->points[pointSearchIndGlobalMap[0]].intensity;
    }

    // extract visualized and downsampled key frames
    for (int i = 0; i < (int)globalMapKeyPosesDS->size(); ++i) {
      if (pointDistance(globalMapKeyPosesDS->points[i], cloudKeyPoses3D->back()) > globalMapVisualizationSearchRadius)
        continue;
      int thisKeyInd = (int)globalMapKeyPosesDS->points[i].intensity;
      *globalMapKeyFrames += *transformPointCloud(cornerCloudKeyFrames[thisKeyInd], &cloudKeyPoses6D->points[thisKeyInd]);
      *globalMapKeyFrames += *transformPointCloud(surfCloudKeyFrames[thisKeyInd], &cloudKeyPoses6D->points[thisKeyInd]);
    }
    // downsample visualized points
    pcl::VoxelGrid<PointType> downSizeFilterGlobalMapKeyFrames;                                                                                    // for global map visualization
    downSizeFilterGlobalMapKeyFrames.setLeafSize(globalMapVisualizationLeafSize, globalMapVisualizationLeafSize, globalMapVisualizationLeafSize);  // for global map visualization
    downSizeFilterGlobalMapKeyFrames.setInputCloud(globalMapKeyFrames);
    downSizeFilterGlobalMapKeyFrames.filter(*globalMapKeyFramesDS);
    publishCloud(&pubLaserCloudSurround, globalMapKeyFramesDS, timeLaserInfoStamp, odometryFrame);
  }

  void loopClosureThread() {
    if (loopClosureEnableFlag == false)
      return;

    ros::Rate rate(loopClosureFrequency);
    while (ros::ok()) {
      rate.sleep();
      performLoopClosure();
      visualizeLoopClosure();
    }
  }

  void loopInfoHandler(const std_msgs::Float64MultiArray::ConstPtr& loopMsg) {
    std::lock_guard<std::mutex> lock(mtxLoopInfo);
    if (loopMsg->data.size() != 2)
      return;

    loopInfoVec.push_back(*loopMsg);

    while (loopInfoVec.size() > 5)
      loopInfoVec.pop_front();
  }

  void performLoopClosure() {
    if (cloudKeyPoses3D->points.empty() == true)
      return;

    mtx.lock();
    *copy_cloudKeyPoses3D = *cloudKeyPoses3D;
    *copy_cloudKeyPoses6D = *cloudKeyPoses6D;
    mtx.unlock();

    // find keys
    int loopKeyCur;
    int loopKeyPre;
    if (detectLoopClosureExternal(&loopKeyCur, &loopKeyPre) == false)
      if (detectLoopClosureDistance(&loopKeyCur, &loopKeyPre) == false)
        return;

    // extract cloud
    pcl::PointCloud<PointType>::Ptr cureKeyframeCloud(new pcl::PointCloud<PointType>());
    pcl::PointCloud<PointType>::Ptr prevKeyframeCloud(new pcl::PointCloud<PointType>());
    {
      loopFindNearKeyframes(cureKeyframeCloud, loopKeyCur, 0);
      loopFindNearKeyframes(prevKeyframeCloud, loopKeyPre, historyKeyframeSearchNum);
      if (cureKeyframeCloud->size() < 300 || prevKeyframeCloud->size() < 1000)
        return;
      if (pubHistoryKeyFrames.getNumSubscribers() != 0)
        publishCloud(&pubHistoryKeyFrames, prevKeyframeCloud, timeLaserInfoStamp, odometryFrame);
    }

    // ICP Settings
    static pcl::IterativeClosestPoint<PointType, PointType> icp;
    icp.setMaxCorrespondenceDistance(historyKeyframeSearchRadius * 2);
    icp.setMaximumIterations(100);
    icp.setTransformationEpsilon(1e-6);
    icp.setEuclideanFitnessEpsilon(1e-6);
    icp.setRANSACIterations(0);

    // Align clouds
    icp.setInputSource(cureKeyframeCloud);
    icp.setInputTarget(prevKeyframeCloud);
    pcl::PointCloud<PointType>::Ptr unused_result(new pcl::PointCloud<PointType>());
    icp.align(*unused_result);

    if (icp.hasConverged() == false || icp.getFitnessScore() > historyKeyframeFitnessScore)
      return;

    // publish corrected cloud
    if (pubIcpKeyFrames.getNumSubscribers() != 0) {
      pcl::PointCloud<PointType>::Ptr closed_cloud(new pcl::PointCloud<PointType>());
      pcl::transformPointCloud(*cureKeyframeCloud, *closed_cloud, icp.getFinalTransformation());
      publishCloud(&pubIcpKeyFrames, closed_cloud, timeLaserInfoStamp, odometryFrame);
    }

    // Get pose transformation
    float x, y, z, roll, pitch, yaw;
    Eigen::Affine3f correctionLidarFrame;
    correctionLidarFrame = icp.getFinalTransformation();
    // transform from world origin to wrong pose
    Eigen::Affine3f tWrong = pclPointToAffine3f(copy_cloudKeyPoses6D->points[loopKeyCur]);
    // transform from world origin to corrected pose
    Eigen::Affine3f tCorrect = correctionLidarFrame * tWrong;  // pre-multiplying -> successive rotation about a fixed frame
    pcl::getTranslationAndEulerAngles(tCorrect, x, y, z, roll, pitch, yaw);
    gtsam::Pose3 poseFrom = Pose3(Rot3::RzRyRx(roll, pitch, yaw), Point3(x, y, z));
    gtsam::Pose3 poseTo   = pclPointTogtsamPose3(copy_cloudKeyPoses6D->points[loopKeyPre]);
    gtsam::Vector Vector6(6);
    float noiseScore = icp.getFitnessScore();
    Vector6 << noiseScore, noiseScore, noiseScore, noiseScore, noiseScore, noiseScore;
    noiseModel::Diagonal::shared_ptr constraintNoise = noiseModel::Diagonal::Variances(Vector6);

    // Add pose constraint
    mtx.lock();
    loopIndexQueue.push_back(make_pair(loopKeyCur, loopKeyPre));
    loopPoseQueue.push_back(poseFrom.between(poseTo));
    loopNoiseQueue.push_back(constraintNoise);
    mtx.unlock();

    // add loop constriant
    loopIndexContainer[loopKeyCur] = loopKeyPre;
  }

  bool detectLoopClosureDistance(int* latestID, int* closestID) {
    int loopKeyCur = copy_cloudKeyPoses3D->size() - 1;
    int loopKeyPre = -1;

    // check loop constraint added before
    auto it = loopIndexContainer.find(loopKeyCur);
    if (it != loopIndexContainer.end())
      return false;

    // find the closest history key frame
    std::vector<int> pointSearchIndLoop;
    std::vector<float> pointSearchSqDisLoop;
    kdtreeHistoryKeyPoses->setInputCloud(copy_cloudKeyPoses3D);
    kdtreeHistoryKeyPoses->radiusSearch(copy_cloudKeyPoses3D->back(), historyKeyframeSearchRadius, pointSearchIndLoop, pointSearchSqDisLoop, 0);

    for (int i = 0; i < (int)pointSearchIndLoop.size(); ++i) {
      int id = pointSearchIndLoop[i];
      if (abs(copy_cloudKeyPoses6D->points[id].time - timeLaserInfoCur) > historyKeyframeSearchTimeDiff) {
        loopKeyPre = id;
        break;
      }
    }

    if (loopKeyPre == -1 || loopKeyCur == loopKeyPre)
      return false;

    *latestID  = loopKeyCur;
    *closestID = loopKeyPre;

    return true;
  }

  bool detectLoopClosureExternal(int* latestID, int* closestID) {
    // this function is not used yet, please ignore it
    int loopKeyCur = -1;
    int loopKeyPre = -1;

    std::lock_guard<std::mutex> lock(mtxLoopInfo);
    if (loopInfoVec.empty())
      return false;

    double loopTimeCur = loopInfoVec.front().data[0];
    double loopTimePre = loopInfoVec.front().data[1];
    loopInfoVec.pop_front();

    if (abs(loopTimeCur - loopTimePre) < historyKeyframeSearchTimeDiff)
      return false;

    int cloudSize = copy_cloudKeyPoses6D->size();
    if (cloudSize < 2)
      return false;

    // latest key
    loopKeyCur = cloudSize - 1;
    for (int i = cloudSize - 1; i >= 0; --i) {
      if (copy_cloudKeyPoses6D->points[i].time >= loopTimeCur)
        loopKeyCur = round(copy_cloudKeyPoses6D->points[i].intensity);
      else
        break;
    }

    // previous key
    loopKeyPre = 0;
    for (int i = 0; i < cloudSize; ++i) {
      if (copy_cloudKeyPoses6D->points[i].time <= loopTimePre)
        loopKeyPre = round(copy_cloudKeyPoses6D->points[i].intensity);
      else
        break;
    }

    if (loopKeyCur == loopKeyPre)
      return false;

    auto it = loopIndexContainer.find(loopKeyCur);
    if (it != loopIndexContainer.end())
      return false;

    *latestID  = loopKeyCur;
    *closestID = loopKeyPre;

    return true;
  }

  void loopFindNearKeyframes(pcl::PointCloud<PointType>::Ptr& nearKeyframes, const int& key, const int& searchNum) {
    // extract near keyframes
    nearKeyframes->clear();
    int cloudSize = copy_cloudKeyPoses6D->size();
    for (int i = -searchNum; i <= searchNum; ++i) {
      int keyNear = key + i;
      if (keyNear < 0 || keyNear >= cloudSize)
        continue;
      *nearKeyframes += *transformPointCloud(cornerCloudKeyFrames[keyNear], &copy_cloudKeyPoses6D->points[keyNear]);
      *nearKeyframes += *transformPointCloud(surfCloudKeyFrames[keyNear], &copy_cloudKeyPoses6D->points[keyNear]);
    }

    if (nearKeyframes->empty())
      return;

    // downsample near keyframes
    pcl::PointCloud<PointType>::Ptr cloud_temp(new pcl::PointCloud<PointType>());
    downSizeFilterICP.setInputCloud(nearKeyframes);
    downSizeFilterICP.filter(*cloud_temp);
    *nearKeyframes = *cloud_temp;
  }

  void visualizeLoopClosure() {
    if (loopIndexContainer.empty())
      return;

    visualization_msgs::MarkerArray markerArray;
    // loop nodes
    visualization_msgs::Marker markerNode;
    markerNode.header.frame_id    = odometryFrame;
    markerNode.header.stamp       = timeLaserInfoStamp;
    markerNode.action             = visualization_msgs::Marker::ADD;
    markerNode.type               = visualization_msgs::Marker::SPHERE_LIST;
    markerNode.ns                 = "loop_nodes";
    markerNode.id                 = 0;
    markerNode.pose.orientation.w = 1;
    markerNode.scale.x            = 0.3;
    markerNode.scale.y            = 0.3;
    markerNode.scale.z            = 0.3;
    markerNode.color.r            = 0;
    markerNode.color.g            = 0.8;
    markerNode.color.b            = 1;
    markerNode.color.a            = 1;
    // loop edges
    visualization_msgs::Marker markerEdge;
    markerEdge.header.frame_id    = odometryFrame;
    markerEdge.header.stamp       = timeLaserInfoStamp;
    markerEdge.action             = visualization_msgs::Marker::ADD;
    markerEdge.type               = visualization_msgs::Marker::LINE_LIST;
    markerEdge.ns                 = "loop_edges";
    markerEdge.id                 = 1;
    markerEdge.pose.orientation.w = 1;
    markerEdge.scale.x            = 0.1;
    markerEdge.color.r            = 0.9;
    markerEdge.color.g            = 0.9;
    markerEdge.color.b            = 0;
    markerEdge.color.a            = 1;

    for (auto it = loopIndexContainer.begin(); it != loopIndexContainer.end(); ++it) {
      int key_cur = it->first;
      int key_pre = it->second;
      geometry_msgs::Point p;
      p.x = copy_cloudKeyPoses6D->points[key_cur].x;
      p.y = copy_cloudKeyPoses6D->points[key_cur].y;
      p.z = copy_cloudKeyPoses6D->points[key_cur].z;
      markerNode.points.push_back(p);
      markerEdge.points.push_back(p);
      p.x = copy_cloudKeyPoses6D->points[key_pre].x;
      p.y = copy_cloudKeyPoses6D->points[key_pre].y;
      p.z = copy_cloudKeyPoses6D->points[key_pre].z;
      markerNode.points.push_back(p);
      markerEdge.points.push_back(p);
    }

    markerArray.markers.push_back(markerNode);
    markerArray.markers.push_back(markerEdge);
    pubLoopConstraintEdge.publish(markerArray);
  }

  void updateInitialGuess() {
    // save current transformation before any processing
    incrementalOdometryAffineFront = trans2Affine3f(transformTobeMapped);

    static Eigen::Affine3f lastImuTransformation;
    // initialization
    if (cloudKeyPoses3D->points.empty()) {
      transformTobeMapped[0] = cloudInfo.imuRollInit;
      transformTobeMapped[1] = cloudInfo.imuPitchInit;
      transformTobeMapped[2] = cloudInfo.imuYawInit;

      if (!useImuHeadingInitialization)
        transformTobeMapped[2] = 0;

      lastImuTransformation = pcl::getTransformation(0, 0, 0, cloudInfo.imuRollInit, cloudInfo.imuPitchInit, cloudInfo.imuYawInit);  // save imu before return;
      return;
    }

    // use imu pre-integration estimation for pose guess
    static bool lastImuPreTransAvailable = false;
    static Eigen::Affine3f lastImuPreTransformation;
    if (cloudInfo.odomAvailable == true) {
      Eigen::Affine3f transBack = pcl::getTransformation(cloudInfo.initialGuessX, cloudInfo.initialGuessY, cloudInfo.initialGuessZ,
                                                         cloudInfo.initialGuessRoll, cloudInfo.initialGuessPitch, cloudInfo.initialGuessYaw);
      if (lastImuPreTransAvailable == false) {
        lastImuPreTransformation = transBack;
        lastImuPreTransAvailable = true;
      } else {
        Eigen::Affine3f transIncre = lastImuPreTransformation.inverse() * transBack;
        Eigen::Affine3f transTobe  = trans2Affine3f(transformTobeMapped);
        Eigen::Affine3f transFinal = transTobe * transIncre;
        pcl::getTranslationAndEulerAngles(transFinal, transformTobeMapped[3], transformTobeMapped[4], transformTobeMapped[5],
                                          transformTobeMapped[0], transformTobeMapped[1], transformTobeMapped[2]);

        lastImuPreTransformation = transBack;

        lastImuTransformation = pcl::getTransformation(0, 0, 0, cloudInfo.imuRollInit, cloudInfo.imuPitchInit, cloudInfo.imuYawInit);  // save imu before return;
        return;
      }
    }

    // use imu incremental estimation for pose guess (only rotation)
    if (cloudInfo.imuAvailable == true) {
      Eigen::Affine3f transBack  = pcl::getTransformation(0, 0, 0, cloudInfo.imuRollInit, cloudInfo.imuPitchInit, cloudInfo.imuYawInit);
      Eigen::Affine3f transIncre = lastImuTransformation.inverse() * transBack;

      Eigen::Affine3f transTobe  = trans2Affine3f(transformTobeMapped);
      Eigen::Affine3f transFinal = transTobe * transIncre;
      pcl::getTranslationAndEulerAngles(transFinal, transformTobeMapped[3], transformTobeMapped[4], transformTobeMapped[5],
                                        transformTobeMapped[0], transformTobeMapped[1], transformTobeMapped[2]);

      lastImuTransformation = pcl::getTransformation(0, 0, 0, cloudInfo.imuRollInit, cloudInfo.imuPitchInit, cloudInfo.imuYawInit);  // save imu before return;
      return;
    }
  }

  void extractForLoopClosure() {
    pcl::PointCloud<PointType>::Ptr cloudToExtract(new pcl::PointCloud<PointType>());
    int numPoses = cloudKeyPoses3D->size();
    for (int i = numPoses - 1; i >= 0; --i) {
      if ((int)cloudToExtract->size() <= surroundingKeyframeSize)
        cloudToExtract->push_back(cloudKeyPoses3D->points[i]);
      else
        break;
    }

    extractCloud(cloudToExtract);
  }

  void extractNearby() {
    pcl::PointCloud<PointType>::Ptr surroundingKeyPoses(new pcl::PointCloud<PointType>());
    pcl::PointCloud<PointType>::Ptr surroundingKeyPosesDS(new pcl::PointCloud<PointType>());
    std::vector<int> pointSearchInd;
    std::vector<float> pointSearchSqDis;

    // extract all the nearby key poses and downsample them
    kdtreeSurroundingKeyPoses->setInputCloud(cloudKeyPoses3D);  // create kd-tree
    kdtreeSurroundingKeyPoses->radiusSearch(cloudKeyPoses3D->back(), (double)surroundingKeyframeSearchRadius, pointSearchInd, pointSearchSqDis);
    for (int i = 0; i < (int)pointSearchInd.size(); ++i) {
      int id = pointSearchInd[i];
      surroundingKeyPoses->push_back(cloudKeyPoses3D->points[id]);
    }

    downSizeFilterSurroundingKeyPoses.setInputCloud(surroundingKeyPoses);
    downSizeFilterSurroundingKeyPoses.filter(*surroundingKeyPosesDS);
    for (auto& pt : surroundingKeyPosesDS->points) {
      kdtreeSurroundingKeyPoses->nearestKSearch(pt, 1, pointSearchInd, pointSearchSqDis);
      pt.intensity = cloudKeyPoses3D->points[pointSearchInd[0]].intensity;
    }

    // also extract some latest key frames in case the robot rotates in one position
    int numPoses = cloudKeyPoses3D->size();
    for (int i = numPoses - 1; i >= 0; --i) {
      if (timeLaserInfoCur - cloudKeyPoses6D->points[i].time < 10.0)
        surroundingKeyPosesDS->push_back(cloudKeyPoses3D->points[i]);
      else
        break;
    }

    extractCloud(surroundingKeyPosesDS);
  }

  void extractCloud(pcl::PointCloud<PointType>::Ptr cloudToExtract) {
    // fuse the map
    laserCloudCornerFromMap->clear();
    laserCloudSurfFromMap->clear();
    for (int i = 0; i < (int)cloudToExtract->size(); ++i) {
      if (pointDistance(cloudToExtract->points[i], cloudKeyPoses3D->back()) > surroundingKeyframeSearchRadius)
        continue;

      int thisKeyInd = (int)cloudToExtract->points[i].intensity;
      if (laserCloudMapContainer.find(thisKeyInd) != laserCloudMapContainer.end()) {
        // transformed cloud available
        *laserCloudCornerFromMap += laserCloudMapContainer[thisKeyInd].first;
        *laserCloudSurfFromMap += laserCloudMapContainer[thisKeyInd].second;
      } else {
        // transformed cloud not available
        pcl::PointCloud<PointType> laserCloudCornerTemp = *transformPointCloud(cornerCloudKeyFrames[thisKeyInd], &cloudKeyPoses6D->points[thisKeyInd]);
        pcl::PointCloud<PointType> laserCloudSurfTemp   = *transformPointCloud(surfCloudKeyFrames[thisKeyInd], &cloudKeyPoses6D->points[thisKeyInd]);
        *laserCloudCornerFromMap += laserCloudCornerTemp;
        *laserCloudSurfFromMap += laserCloudSurfTemp;
        laserCloudMapContainer[thisKeyInd] = make_pair(laserCloudCornerTemp, laserCloudSurfTemp);
      }
    }

    // Downsample the surrounding corner key frames (or map)
    downSizeFilterCorner.setInputCloud(laserCloudCornerFromMap);
    downSizeFilterCorner.filter(*laserCloudCornerFromMapDS);
    laserCloudCornerFromMapDSNum = laserCloudCornerFromMapDS->size();
    // Downsample the surrounding surf key frames (or map)
    downSizeFilterSurf.setInputCloud(laserCloudSurfFromMap);
    downSizeFilterSurf.filter(*laserCloudSurfFromMapDS);
    laserCloudSurfFromMapDSNum = laserCloudSurfFromMapDS->size();

    // clear map cache if too large
    if (laserCloudMapContainer.size() > 1000)
      laserCloudMapContainer.clear();
  }

  void extractSurroundingKeyFrames() {
    if (cloudKeyPoses3D->points.empty() == true)
      return;

    // if (loopClosureEnableFlag == true)
    // {
    //     extractForLoopClosure();
    // } else {
    //     extractNearby();
    // }

    extractNearby();
  }

  void downsampleCurrentScan() {
    // Downsample cloud from current scan
    laserCloudCornerLastDS->clear();
    downSizeFilterCorner.setInputCloud(laserCloudCornerLast);
    downSizeFilterCorner.filter(*laserCloudCornerLastDS);
    laserCloudCornerLastDSNum = laserCloudCornerLastDS->size();

    laserCloudSurfLastDS->clear();
    downSizeFilterSurf.setInputCloud(laserCloudSurfLast);
    downSizeFilterSurf.filter(*laserCloudSurfLastDS);
    laserCloudSurfLastDSNum = laserCloudSurfLastDS->size();
  }

  void updatePointAssociateToMap() {
    transPointAssociateToMap = trans2Affine3f(transformTobeMapped);
  }

  void cornerOptimization() {
    updatePointAssociateToMap();

#pragma omp parallel for num_threads(numberOfCores)
    for (int i = 0; i < laserCloudCornerLastDSNum; i++) {
      PointType pointOri, pointSel, coeff;
      std::vector<int> pointSearchInd;
      std::vector<float> pointSearchSqDis;

      pointOri = laserCloudCornerLastDS->points[i];
      pointAssociateToMap(&pointOri, &pointSel);
      kdtreeCornerFromMap->nearestKSearch(pointSel, 5, pointSearchInd, pointSearchSqDis);

      cv::Mat matA1(3, 3, CV_32F, cv::Scalar::all(0));
      cv::Mat matD1(1, 3, CV_32F, cv::Scalar::all(0));
      cv::Mat matV1(3, 3, CV_32F, cv::Scalar::all(0));

      if (pointSearchSqDis[4] < 1.0) {
        float cx = 0, cy = 0, cz = 0;
        for (int j = 0; j < 5; j++) {
          cx += laserCloudCornerFromMapDS->points[pointSearchInd[j]].x;
          cy += laserCloudCornerFromMapDS->points[pointSearchInd[j]].y;
          cz += laserCloudCornerFromMapDS->points[pointSearchInd[j]].z;
        }
        cx /= 5;
        cy /= 5;
        cz /= 5;

        float a11 = 0, a12 = 0, a13 = 0, a22 = 0, a23 = 0, a33 = 0;
        for (int j = 0; j < 5; j++) {
          float ax = laserCloudCornerFromMapDS->points[pointSearchInd[j]].x - cx;
          float ay = laserCloudCornerFromMapDS->points[pointSearchInd[j]].y - cy;
          float az = laserCloudCornerFromMapDS->points[pointSearchInd[j]].z - cz;

          a11 += ax * ax;
          a12 += ax * ay;
          a13 += ax * az;
          a22 += ay * ay;
          a23 += ay * az;
          a33 += az * az;
        }
        a11 /= 5;
        a12 /= 5;
        a13 /= 5;
        a22 /= 5;
        a23 /= 5;
        a33 /= 5;

        matA1.at<float>(0, 0) = a11;
        matA1.at<float>(0, 1) = a12;
        matA1.at<float>(0, 2) = a13;
        matA1.at<float>(1, 0) = a12;
        matA1.at<float>(1, 1) = a22;
        matA1.at<float>(1, 2) = a23;
        matA1.at<float>(2, 0) = a13;
        matA1.at<float>(2, 1) = a23;
        matA1.at<float>(2, 2) = a33;

        cv::eigen(matA1, matD1, matV1);

        if (matD1.at<float>(0, 0) > 3 * matD1.at<float>(0, 1)) {
          float x0 = pointSel.x;
          float y0 = pointSel.y;
          float z0 = pointSel.z;
          float x1 = cx + 0.1 * matV1.at<float>(0, 0);
          float y1 = cy + 0.1 * matV1.at<float>(0, 1);
          float z1 = cz + 0.1 * matV1.at<float>(0, 2);
          float x2 = cx - 0.1 * matV1.at<float>(0, 0);
          float y2 = cy - 0.1 * matV1.at<float>(0, 1);
          float z2 = cz - 0.1 * matV1.at<float>(0, 2);

          // clang-format off
          float a012 = sqrt(((x0 - x1) * (y0 - y2) - (x0 - x2) * (y0 - y1)) * ((x0 - x1) * (y0 - y2) - (x0 - x2) * (y0 - y1))
                          + ((x0 - x1) * (z0 - z2) - (x0 - x2) * (z0 - z1)) * ((x0 - x1) * (z0 - z2) - (x0 - x2) * (z0 - z1))
                          + ((y0 - y1) * (z0 - z2) - (y0 - y2) * (z0 - z1)) * ((y0 - y1) * (z0 - z2) - (y0 - y2) * (z0 - z1)));

          float l12 = sqrt((x1 - x2) * (x1 - x2) + (y1 - y2) * (y1 - y2) + (z1 - z2) * (z1 - z2));

          float la = ((y1 - y2) * ((x0 - x1) * (y0 - y2) - (x0 - x2) * (y0 - y1))
                    + (z1 - z2) * ((x0 - x1) * (z0 - z2) - (x0 - x2) * (z0 - z1))) / a012 / l12;

          float lb = -((x1 - x2) * ((x0 - x1) * (y0 - y2) - (x0 - x2) * (y0 - y1)) 
                     - (z1 - z2) * ((y0 - y1) * (z0 - z2) - (y0 - y2) * (z0 - z1))) / a012 / l12;

          float lc = -((x1 - x2) * ((x0 - x1) * (z0 - z2) - (x0 - x2) * (z0 - z1))
                     + (y1 - y2) * ((y0 - y1) * (z0 - z2) - (y0 - y2) * (z0 - z1))) / a012 / l12;
          // clang-format on

          float ld2 = a012 / l12;

          float s = 1 - 0.9 * fabs(ld2);

          coeff.x         = s * la;
          coeff.y         = s * lb;
          coeff.z         = s * lc;
          coeff.intensity = s * ld2;

          if (s > 0.1) {
            laserCloudOriCornerVec[i]  = pointOri;
            coeffSelCornerVec[i]       = coeff;
            laserCloudOriCornerFlag[i] = true;
          }
        }
      }
    }
  }

  void surfOptimization() {
    updatePointAssociateToMap();

#pragma omp parallel for num_threads(numberOfCores)
    for (int i = 0; i < laserCloudSurfLastDSNum; i++) {
      PointType pointOri, pointSel, coeff;
      std::vector<int> pointSearchInd;
      std::vector<float> pointSearchSqDis;

      pointOri = laserCloudSurfLastDS->points[i];
      pointAssociateToMap(&pointOri, &pointSel);
      kdtreeSurfFromMap->nearestKSearch(pointSel, 5, pointSearchInd, pointSearchSqDis);

      Eigen::Matrix<float, 5, 3> matA0;
      Eigen::Matrix<float, 5, 1> matB0;
      Eigen::Vector3f matX0;

      matA0.setZero();
      matB0.fill(-1);
      matX0.setZero();

      if (pointSearchSqDis[4] < 1.0) {
        for (int j = 0; j < 5; j++) {
          matA0(j, 0) = laserCloudSurfFromMapDS->points[pointSearchInd[j]].x;
          matA0(j, 1) = laserCloudSurfFromMapDS->points[pointSearchInd[j]].y;
          matA0(j, 2) = laserCloudSurfFromMapDS->points[pointSearchInd[j]].z;
        }

        matX0 = matA0.colPivHouseholderQr().solve(matB0);

        float pa = matX0(0, 0);
        float pb = matX0(1, 0);
        float pc = matX0(2, 0);
        float pd = 1;

        float ps = sqrt(pa * pa + pb * pb + pc * pc);
        pa /= ps;
        pb /= ps;
        pc /= ps;
        pd /= ps;

        bool planeValid = true;
        for (int j = 0; j < 5; j++) {
          if (fabs(pa * laserCloudSurfFromMapDS->points[pointSearchInd[j]].x +
                   pb * laserCloudSurfFromMapDS->points[pointSearchInd[j]].y +
                   pc * laserCloudSurfFromMapDS->points[pointSearchInd[j]].z + pd) > 0.2) {
            planeValid = false;
            break;
          }
        }

        if (planeValid) {
          float pd2 = pa * pointSel.x + pb * pointSel.y + pc * pointSel.z + pd;

          float s = 1 - 0.9 * fabs(pd2) / sqrt(sqrt(pointSel.x * pointSel.x + pointSel.y * pointSel.y + pointSel.z * pointSel.z));

          coeff.x         = s * pa;
          coeff.y         = s * pb;
          coeff.z         = s * pc;
          coeff.intensity = s * pd2;

          if (s > 0.1) {
            laserCloudOriSurfVec[i]  = pointOri;
            coeffSelSurfVec[i]       = coeff;
            laserCloudOriSurfFlag[i] = true;
          }
        }
      }
    }
  }

  void combineOptimizationCoeffs() {
    // combine corner coeffs
    for (int i = 0; i < laserCloudCornerLastDSNum; ++i) {
      if (laserCloudOriCornerFlag[i] == true) {
        laserCloudOri->push_back(laserCloudOriCornerVec[i]);
        coeffSel->push_back(coeffSelCornerVec[i]);
      }
    }
    // combine surf coeffs
    for (int i = 0; i < laserCloudSurfLastDSNum; ++i) {
      if (laserCloudOriSurfFlag[i] == true) {
        laserCloudOri->push_back(laserCloudOriSurfVec[i]);
        coeffSel->push_back(coeffSelSurfVec[i]);
      }
    }
    // reset flag for next iteration
    std::fill(laserCloudOriCornerFlag.begin(), laserCloudOriCornerFlag.end(), false);
    std::fill(laserCloudOriSurfFlag.begin(), laserCloudOriSurfFlag.end(), false);
  }

  bool LMOptimization(int iterCount) {
    // This optimization is from the original loam_velodyne by Ji Zhang, need to cope with coordinate transformation
    // lidar <- camera      ---     camera <- lidar
    // x = z                ---     x = y
    // y = x                ---     y = z
    // z = y                ---     z = x
    // roll = yaw           ---     roll = pitch
    // pitch = roll         ---     pitch = yaw
    // yaw = pitch          ---     yaw = roll

    // lidar -> camera
    float srx = sin(transformTobeMapped[1]);
    float crx = cos(transformTobeMapped[1]);
    float sry = sin(transformTobeMapped[2]);
    float cry = cos(transformTobeMapped[2]);
    float srz = sin(transformTobeMapped[0]);
    float crz = cos(transformTobeMapped[0]);

    int laserCloudSelNum = laserCloudOri->size();
    if (laserCloudSelNum < 50) {
      return false;
    }

    cv::Mat matA(laserCloudSelNum, 6, CV_32F, cv::Scalar::all(0));
    cv::Mat matAt(6, laserCloudSelNum, CV_32F, cv::Scalar::all(0));
    cv::Mat matAtA(6, 6, CV_32F, cv::Scalar::all(0));
    cv::Mat matB(laserCloudSelNum, 1, CV_32F, cv::Scalar::all(0));
    cv::Mat matAtB(6, 1, CV_32F, cv::Scalar::all(0));
    cv::Mat matX(6, 1, CV_32F, cv::Scalar::all(0));

    PointType pointOri, coeff;

    for (int i = 0; i < laserCloudSelNum; i++) {
      // lidar -> camera
      pointOri.x = laserCloudOri->points[i].y;
      pointOri.y = laserCloudOri->points[i].z;
      pointOri.z = laserCloudOri->points[i].x;
      // lidar -> camera
      coeff.x         = coeffSel->points[i].y;
      coeff.y         = coeffSel->points[i].z;
      coeff.z         = coeffSel->points[i].x;
      coeff.intensity = coeffSel->points[i].intensity;
      // in camera
      float arx = (crx * sry * srz * pointOri.x + crx * crz * sry * pointOri.y - srx * sry * pointOri.z) * coeff.x + (-srx * srz * pointOri.x - crz * srx * pointOri.y - crx * pointOri.z) * coeff.y + (crx * cry * srz * pointOri.x + crx * cry * crz * pointOri.y - cry * srx * pointOri.z) * coeff.z;

      float ary = ((cry * srx * srz - crz * sry) * pointOri.x + (sry * srz + cry * crz * srx) * pointOri.y + crx * cry * pointOri.z) * coeff.x + ((-cry * crz - srx * sry * srz) * pointOri.x + (cry * srz - crz * srx * sry) * pointOri.y - crx * sry * pointOri.z) * coeff.z;

      float arz = ((crz * srx * sry - cry * srz) * pointOri.x + (-cry * crz - srx * sry * srz) * pointOri.y) * coeff.x + (crx * crz * pointOri.x - crx * srz * pointOri.y) * coeff.y + ((sry * srz + cry * crz * srx) * pointOri.x + (crz * sry - cry * srx * srz) * pointOri.y) * coeff.z;
      // lidar -> camera
      matA.at<float>(i, 0) = arz;
      matA.at<float>(i, 1) = arx;
      matA.at<float>(i, 2) = ary;
      matA.at<float>(i, 3) = coeff.z;
      matA.at<float>(i, 4) = coeff.x;
      matA.at<float>(i, 5) = coeff.y;
      matB.at<float>(i, 0) = -coeff.intensity;
    }

    cv::transpose(matA, matAt);
    matAtA = matAt * matA;
    matAtB = matAt * matB;
    cv::solve(matAtA, matAtB, matX, cv::DECOMP_QR);

    if (iterCount == 0) {
      cv::Mat matE(1, 6, CV_32F, cv::Scalar::all(0));
      cv::Mat matV(6, 6, CV_32F, cv::Scalar::all(0));
      cv::Mat matV2(6, 6, CV_32F, cv::Scalar::all(0));

      cv::eigen(matAtA, matE, matV);
      matV.copyTo(matV2);

      isDegenerate      = false;
      float eignThre[6] = {100, 100, 100, 100, 100, 100};
      for (int i = 5; i >= 0; i--) {
        if (matE.at<float>(0, i) < eignThre[i]) {
          for (int j = 0; j < 6; j++) {
            matV2.at<float>(i, j) = 0;
          }
          isDegenerate = true;
        } else {
          break;
        }
      }
      matP = matV.inv() * matV2;
    }

    if (isDegenerate) {
      cv::Mat matX2(6, 1, CV_32F, cv::Scalar::all(0));
      matX.copyTo(matX2);
      matX = matP * matX2;
    }

    transformTobeMapped[0] += matX.at<float>(0, 0);
    transformTobeMapped[1] += matX.at<float>(1, 0);
    transformTobeMapped[2] += matX.at<float>(2, 0);
    transformTobeMapped[3] += matX.at<float>(3, 0);
    transformTobeMapped[4] += matX.at<float>(4, 0);
    transformTobeMapped[5] += matX.at<float>(5, 0);

    float deltaR = sqrt(
        pow(pcl::rad2deg(matX.at<float>(0, 0)), 2) +
        pow(pcl::rad2deg(matX.at<float>(1, 0)), 2) +
        pow(pcl::rad2deg(matX.at<float>(2, 0)), 2));
    float deltaT = sqrt(
        pow(matX.at<float>(3, 0) * 100, 2) +
        pow(matX.at<float>(4, 0) * 100, 2) +
        pow(matX.at<float>(5, 0) * 100, 2));

    if (deltaR < 0.05 && deltaT < 0.05) {
      return true;  // converged
    }
    return false;  // keep optimizing
  }

  void scan2MapOptimization() {
    if (cloudKeyPoses3D->points.empty())
      return;

    if (laserCloudCornerLastDSNum > edgeFeatureMinValidNum && laserCloudSurfLastDSNum > surfFeatureMinValidNum) {
      kdtreeCornerFromMap->setInputCloud(laserCloudCornerFromMapDS);
      kdtreeSurfFromMap->setInputCloud(laserCloudSurfFromMapDS);

      for (int iterCount = 0; iterCount < 30; iterCount++) {
        laserCloudOri->clear();
        coeffSel->clear();

        cornerOptimization();
        surfOptimization();

        combineOptimizationCoeffs();

        if (LMOptimization(iterCount) == true)
          break;
      }

      transformUpdate();
    } else {
      ROS_WARN("Not enough features! Only %d edge and %d planar features available.", laserCloudCornerLastDSNum, laserCloudSurfLastDSNum);
    }
  }

  void transformUpdate() {
    if (cloudInfo.imuAvailable == true) {
      if (std::abs(cloudInfo.imuPitchInit) < 1.4) {
        double imuWeight = imuRPYWeight;
        tf::Quaternion imuQuaternion;
        tf::Quaternion transformQuaternion;
        double rollMid, pitchMid, yawMid;

        // slerp roll
        transformQuaternion.setRPY(transformTobeMapped[0], 0, 0);
        imuQuaternion.setRPY(cloudInfo.imuRollInit, 0, 0);
        tf::Matrix3x3(transformQuaternion.slerp(imuQuaternion, imuWeight)).getRPY(rollMid, pitchMid, yawMid);
        transformTobeMapped[0] = rollMid;

        // slerp pitch
        transformQuaternion.setRPY(0, transformTobeMapped[1], 0);
        imuQuaternion.setRPY(0, cloudInfo.imuPitchInit, 0);
        tf::Matrix3x3(transformQuaternion.slerp(imuQuaternion, imuWeight)).getRPY(rollMid, pitchMid, yawMid);
        transformTobeMapped[1] = pitchMid;
      }
    }

    transformTobeMapped[0] = constraintTransformation(transformTobeMapped[0], rotation_tollerance);
    transformTobeMapped[1] = constraintTransformation(transformTobeMapped[1], rotation_tollerance);
    transformTobeMapped[5] = constraintTransformation(transformTobeMapped[5], z_tollerance);

    incrementalOdometryAffineBack = trans2Affine3f(transformTobeMapped);
  }

  float constraintTransformation(float value, float limit) {
    if (value < -limit)
      value = -limit;
    if (value > limit)
      value = limit;

    return value;
  }

  bool saveFrame() {
    if (cloudKeyPoses3D->points.empty())
      return true;

    Eigen::Affine3f transStart   = pclPointToAffine3f(cloudKeyPoses6D->back());
    Eigen::Affine3f transFinal   = pcl::getTransformation(transformTobeMapped[3], transformTobeMapped[4], transformTobeMapped[5],
                                                          transformTobeMapped[0], transformTobeMapped[1], transformTobeMapped[2]);
    Eigen::Affine3f transBetween = transStart.inverse() * transFinal;
    float x, y, z, roll, pitch, yaw;
    pcl::getTranslationAndEulerAngles(transBetween, x, y, z, roll, pitch, yaw);

    if (abs(roll) < surroundingkeyframeAddingAngleThreshold &&
        abs(pitch) < surroundingkeyframeAddingAngleThreshold &&
        abs(yaw) < surroundingkeyframeAddingAngleThreshold &&
        sqrt(x * x + y * y + z * z) < surroundingkeyframeAddingDistThreshold)
      return false;

    return true;
  }

  void addOdomFactor() {
    if (cloudKeyPoses3D->points.empty()) {
      auto currentKeyIndex = numberOfNodes++;  // 0
      keyPoseIndices.push_back(currentKeyIndex);

      noiseModel::Diagonal::shared_ptr priorNoise = noiseModel::Diagonal::Variances(priorOdometryDiagonalVarianceEigenVector);  // rad*rad, meter*meter
      gtSAMgraph.add(PriorFactor<Pose3>(0, trans2gtsamPose(transformTobeMapped), priorNoise));
      initialEstimate.insert(currentKeyIndex, trans2gtsamPose(transformTobeMapped));
      initialEstimateForAnalysis.insert(currentKeyIndex, trans2gtsamPose(transformTobeMapped));
    } else {
      auto previousKeyIndex = keyPoseIndices.back();
      auto currentKeyIndex  = numberOfNodes++;
      keyPoseIndices.push_back(currentKeyIndex);

      noiseModel::Diagonal::shared_ptr odometryNoise = noiseModel::Diagonal::Variances(odometryDiagonalVarianceEigenVector);
      gtsam::Pose3 poseFrom                          = pclPointTogtsamPose3(cloudKeyPoses6D->points.back());
      gtsam::Pose3 poseTo                            = trans2gtsamPose(transformTobeMapped);
      gtSAMgraph.add(BetweenFactor<Pose3>(previousKeyIndex,
                                          currentKeyIndex,
                                          poseFrom.between(poseTo),
                                          odometryNoise));
      initialEstimate.insert(currentKeyIndex, poseTo);
      initialEstimateForAnalysis.insert(currentKeyIndex, poseTo);
    }
  }

  void addGPSFactor() {
    if (gpsQueue.empty())
      return;

    // wait for system initialized and settles down
    if (cloudKeyPoses3D->points.empty())
      return;
    else {
      if (pointDistance(cloudKeyPoses3D->front(), cloudKeyPoses3D->back()) < 5.0)
        return;
    }

    // pose covariance small, no need to correct
    if (poseCovariance(3, 3) < poseCovThreshold && poseCovariance(4, 4) < poseCovThreshold)
      return;

    // last gps position
    static PointType lastGPSPoint;

    while (!gpsQueue.empty()) {
      if (gpsQueue.front().header.stamp.toSec() < timeLaserInfoCur - 0.2) {
        // message too old
        gpsQueue.pop_front();
      } else if (gpsQueue.front().header.stamp.toSec() > timeLaserInfoCur + 0.2) {
        // message too new
        break;
      } else {
        nav_msgs::Odometry thisGPS = gpsQueue.front();
        gpsQueue.pop_front();

        // GPS too noisy, skip
        float noise_x = thisGPS.pose.covariance[0];
        float noise_y = thisGPS.pose.covariance[7];
        float noise_z = thisGPS.pose.covariance[14];
        if (noise_x > gpsCovThreshold || noise_y > gpsCovThreshold)
          continue;

        float gps_x = thisGPS.pose.pose.position.x;
        float gps_y = thisGPS.pose.pose.position.y;
        float gps_z = thisGPS.pose.pose.position.z;
        if (!useGpsElevation) {
          gps_z   = transformTobeMapped[5];
          noise_z = 0.01;
        }

        // GPS not properly initialized (0,0,0)
        if (abs(gps_x) < 1e-6 && abs(gps_y) < 1e-6)
          continue;

        // Add GPS every a few meters
        PointType curGPSPoint;
        curGPSPoint.x = gps_x;
        curGPSPoint.y = gps_y;
        curGPSPoint.z = gps_z;
        if (pointDistance(curGPSPoint, lastGPSPoint) < 5.0)
          continue;
        else
          lastGPSPoint = curGPSPoint;

        gtsam::Vector Vector3(3);
        Vector3 << max(noise_x, 1.0f), max(noise_y, 1.0f), max(noise_z, 1.0f);
        noiseModel::Diagonal::shared_ptr gps_noise = noiseModel::Diagonal::Variances(Vector3);
        gtsam::GPSFactor gps_factor(cloudKeyPoses3D->size(), gtsam::Point3(gps_x, gps_y, gps_z), gps_noise);
        gtSAMgraph.add(gps_factor);

        aLoopIsClosed = true;
        break;
      }
    }
  }

  void addLoopFactor() {
    if (loopIndexQueue.empty())
      return;

    for (int i = 0; i < (int)loopIndexQueue.size(); ++i) {
      int indexFrom                                        = loopIndexQueue[i].first;
      int indexTo                                          = loopIndexQueue[i].second;
      gtsam::Pose3 poseBetween                             = loopPoseQueue[i];
      gtsam::noiseModel::Diagonal::shared_ptr noiseBetween = loopNoiseQueue[i];
      gtSAMgraph.add(BetweenFactor<Pose3>(indexFrom, indexTo, poseBetween, noiseBetween));
    }

    loopIndexQueue.clear();
    loopPoseQueue.clear();
    loopNoiseQueue.clear();
    aLoopIsClosed = true;
  }

  void propagateObjectPoses() {
    std::map<uint64_t, ObjectState> nextObjects;

    // Only triggered in the beginning.
    if (objects.empty()) {
      objects.push_back(nextObjects);
      return;
    }

#ifdef MAP_OPTIMIZATION_DEBUG
    std::cout << "DELTA_TIME :: " << deltaTime << "\n\n";
#endif

    for (const auto& pairedObject : objects.back()) {
      // Drop the object if it is lost for a long time
      if (pairedObject.second.lostCount > trackingStepsForLostObject) continue;

      // Propagate the object using the constant velocity model as well as
      // register a new variable node for the object
      auto identity     = Pose3::identity();
      auto nextObject   = pairedObject.second.clone();
      auto deltaPoseVec = gtsam::traits<Pose3>::Local(identity, nextObject.velocity) * deltaTime;
      auto deltaPose    = gtsam::traits<Pose3>::Retract(identity, deltaPoseVec);
      nextObject.pose   = nextObject.pose * deltaPose;

      nextObject.isFirst   = false;
      nextObject.timestamp = timeLaserInfoStamp;
      if (pairedObject.second.lostCount == 0) {
        nextObject.poseNodeIndex     = numberOfNodes++;
        nextObject.velocityNodeIndex = numberOfNodes++;

        initialEstimate.insert(nextObject.poseNodeIndex, nextObject.pose);
        initialEstimate.insert(nextObject.velocityNodeIndex, nextObject.velocity);

        initialEstimateForAnalysis.insert(nextObject.poseNodeIndex, nextObject.pose);
        initialEstimateForAnalysis.insert(nextObject.velocityNodeIndex, nextObject.velocity);

        nextObject.previousVelocityNodeIndices.push_back(pairedObject.second.velocityNodeIndex);
      } else {
        nextObject.poseNodeIndex     = -1;
        nextObject.velocityNodeIndex = -1;
      }

      nextObjects[pairedObject.first] = nextObject;
    }
    objects.push_back(nextObjects);

#ifdef ENABLE_MINIMAL_MEMORY_USAGE
    if (objects.size() > 2 &&
        objects.size() > numberOfPreLooseCouplingSteps + 1 &&
        objects.size() > numberOfVelocityConsistencySteps + 1) {
      objects.erase(objects.begin());
    }
#endif
  }

  void addConstantVelocityFactor() {
    // Skip the process if there is no enough time stamps for adding constant
    // velocity factors
    if (objects.size() < 2) return;

    for (const auto& pairedObject : objects.back()) {
      // Skip if the object is lost at this stamp or is a new object
      if (pairedObject.second.isFirst) continue;
      if (pairedObject.second.lostCount > 0) continue;

      auto noiseModel      = noiseModel::Diagonal::Variances(constantVelocityDiagonalVarianceEigenVector);
      auto earlyNoiseModel = noiseModel::Diagonal::Variances(earlyConstantVelocityDiagonalVarianceEigenVector);
      auto currentObject   = pairedObject.second;
      auto objectIndex     = currentObject.objectIndex;
      auto previousObject  = objects[objects.size() - 2][objectIndex];
      if (pairedObject.second.isTightlyCoupled) {
        gtSAMgraph.add(ConstantVelocityFactor(previousObject.velocityNodeIndex,
                                              currentObject.velocityNodeIndex,
                                              noiseModel));
      } else {
        if (objectPaths.markers[pairedObject.second.objectIndex].points.size() <= numberOfEarlySteps) {
          gtSAMgraphForLooselyCoupledObjects.add(ConstantVelocityFactor(previousObject.velocityNodeIndex,
                                                                        currentObject.velocityNodeIndex,
                                                                        earlyNoiseModel));
        } else {
          gtSAMgraphForLooselyCoupledObjects.add(ConstantVelocityFactor(previousObject.velocityNodeIndex,
                                                                        currentObject.velocityNodeIndex,
                                                                        noiseModel));
        }
      }
    }
  }

  void addStablePoseFactor() {
    // Skip the process if there is no enough time stamps for adding constant
    // velocity factors
    if (objects.size() < 2) return;

    for (auto& pairedObject : objects.back()) {
      // Skip if the object is lost at this stamp or is a new object
      if (pairedObject.second.isFirst) continue;
      if (pairedObject.second.lostCount > 0) continue;

      auto noise          = noiseModel::Diagonal::Variances(motionDiagonalVarianceEigenVector);
      auto& currentObject = pairedObject.second;
      auto objectIndex    = currentObject.objectIndex;
      auto previousObject = objects[objects.size() - 2][objectIndex];
      if (pairedObject.second.isTightlyCoupled) {
        gtSAMgraph.add(StablePoseFactor(previousObject.poseNodeIndex,
#ifdef ENABLE_COMPACT_VERSION_OF_FACTOR_GRAPH
                                        currentObject.velocityNodeIndex,
#else
                                        previousObject.velocityNodeIndex,
#endif
                                        currentObject.poseNodeIndex,
                                        deltaTime,
                                        noise));
      } else {
        gtSAMgraphForLooselyCoupledObjects.add(StablePoseFactor(previousObject.poseNodeIndex,
#ifdef ENABLE_COMPACT_VERSION_OF_FACTOR_GRAPH
                                                                currentObject.velocityNodeIndex,
#else
                                                                previousObject.velocityNodeIndex,
#endif
                                                                currentObject.poseNodeIndex,
                                                                deltaTime,
                                                                noise));
      }
      currentObject.motionFactorPtr = boost::make_shared<StablePoseFactor>(previousObject.poseNodeIndex,
#ifdef ENABLE_COMPACT_VERSION_OF_FACTOR_GRAPH
                                                                           currentObject.velocityNodeIndex,
#else
                                                                           previousObject.velocityNodeIndex,
#endif
                                                                           currentObject.poseNodeIndex,
                                                                           deltaTime,
                                                                           noise);
      initialEstimateForAnalysis.insert(previousObject.poseNodeIndex, previousObject.pose);
      initialEstimateForAnalysis.insert(previousObject.velocityNodeIndex, previousObject.velocity);
      currentObject.initialMotionError = currentObject.motionFactorPtr->error(initialEstimateForAnalysis);
    }
  }

  void addDetectionFactor(bool requiredMockDetection = false) {
    anyObjectIsTightlyCoupled = false;

    if (detections->boxes.size() == 0 && objects.size() == 0) {
      // Skip the process if there is no detection and no active moving objects
      return;
    } else if (detections->boxes.size() == 0 && objects.size() > 0) {
      // Set every moving object as lost if there is no detection
      for (auto& pairedObject : objects.back()) {
        auto& object = pairedObject.second;
        ++object.lostCount;
        object.confidence                               = 0.0;
        objectPaths.markers[object.objectIndex].scale.x = 0.3;
        objectPaths.markers[object.objectIndex].scale.y = 0.3;
        objectPaths.markers[object.objectIndex].scale.z = 0.3;
      }
      return;
    }
    // After the above condition, the following process will suppose that there
    // exist at least one detection in this step

    // Initialize an indicator matrix for data association of objects and
    // detections, where we secretly add a row to the matrix in case of no
    // active objects at the current stamp
    Eigen::MatrixXi indicator = Eigen::MatrixXi::Zero(objects.back().size() + 1,
                                                      detections->boxes.size());
    std::vector<int> trackingObjectIndices(detections->boxes.size(), -1);

    // Create a vector of Detections.
    auto smallEgoMotion = Pose3(Rot3::RzRyRx(0, 0, 0), Point3(0, 0, 0));
    if (requiredMockDetection) {
      Eigen::Affine3f transStart   = pclPointToAffine3f(cloudKeyPoses6D->back());
      Eigen::Affine3f transFinal   = pcl::getTransformation(transformTobeMapped[3], transformTobeMapped[4], transformTobeMapped[5],
                                                            transformTobeMapped[0], transformTobeMapped[1], transformTobeMapped[2]);
      Eigen::Affine3f transBetween = transStart.inverse() * transFinal;
      float x, y, z, roll, pitch, yaw;
      pcl::getTranslationAndEulerAngles(transBetween, x, y, z, roll, pitch, yaw);
      smallEgoMotion = Pose3(Rot3::RzRyRx(roll, pitch, yaw), Point3(x, y, z));
    }
    detectionVector.clear();
    tightlyCoupledDetectionVector.clear();
    earlyLooselyCoupledMatchingVector.clear();
    looselyCoupledMatchingVector.clear();
    tightlyCoupledMatchingVector.clear();
    dataAssociationVector.clear();
    for (auto box : detections->boxes) {
      if (requiredMockDetection) {
        auto pose = Pose3(Rot3::Quaternion(box.pose.orientation.w, box.pose.orientation.x, box.pose.orientation.y, box.pose.orientation.z),
                          Point3(box.pose.position.x, box.pose.position.y, box.pose.position.z));
        pose      = smallEgoMotion * pose;

        auto quat              = pose.rotation().toQuaternion();
        box.pose.orientation.w = quat.w();
        box.pose.orientation.x = quat.x();
        box.pose.orientation.y = quat.y();
        box.pose.orientation.z = quat.z();

        auto pos            = pose.translation();
        box.pose.position.x = pos.x();
        box.pose.position.y = pos.y();
        box.pose.position.z = pos.z();
      }
      detectionVector.emplace_back(box, looselyCoupledDetectionVarianceEigenVector);
      tightlyCoupledDetectionVector.emplace_back(box, tightlyCoupledDetectionVarianceEigenVector);
      earlyLooselyCoupledMatchingVector.emplace_back(box, earlyLooselyCoupledMatchingVarianceEigenVector);
      looselyCoupledMatchingVector.emplace_back(box, looselyCoupledMatchingVarianceEigenVector);
      tightlyCoupledMatchingVector.emplace_back(box, tightlyCoupledMatchingVarianceEigenVector);
      dataAssociationVector.emplace_back(box, dataAssociationVarianceEigenVector);
    }

    auto egoPoseKey = keyPoseIndices.back();
    auto egoPose    = initialEstimate.at<Pose3>(egoPoseKey);
    auto invEgoPose = egoPose.inverse();

    // Perform the data association for each object, and determine if this
    // object is lost at this stamp
#ifdef MAP_OPTIMIZATION_DEBUG
    std::cout << "OBJECT_INDICES ::\n";
#endif
    size_t i = 0;  // object index with respect to the indicator matrix
    for (auto& pairedObject : objects.back()) {
      auto& object = pairedObject.second;
      size_t j;  // detection index with respect to the indicator matrix
      double error;

      size_t dataAssociationJ;
      double dataAssociationError;

#ifdef MAP_OPTIMIZATION_DEBUG
      std::cout << object.objectIndex << ' ';
#endif

      auto&& predictedPose = invEgoPose * object.pose;
      if (object.trackScore >= numberOfPreLooseCouplingSteps) {
        // std::tie(j, error) = getDetectionIndexAndError(predictedPose, tightlyCoupledMatchingVector);
        std::tie(j, error) = getDetectionIndexAndError(predictedPose, looselyCoupledMatchingVector);
      } else if (objectPaths.markers[object.objectIndex].points.size() <= numberOfEarlySteps) {
        // Set the detection factor error to the early-loosely-coupled detection
        // error for new objects, as they need some chances tp obtain velocity
        std::tie(j, error) = getDetectionIndexAndError(predictedPose, earlyLooselyCoupledMatchingVector);
      } else {
        std::tie(j, error) = getDetectionIndexAndError(predictedPose, looselyCoupledMatchingVector);
      }
      std::tie(dataAssociationJ, dataAssociationError) = getDetectionIndexAndError(predictedPose, dataAssociationVector);

      // TODO: Increase code readability
      if (error < detectionMatchThreshold) {  // found
        // If the object is lost, we still need to create a new moving object
        // for it. In this way we would temporarily skip it and later we will
        // add the object properly
        if (object.lostCount > 0) {
          trackingObjectIndices[j] = object.objectIndexForTracking;
          // A tricky part: set the lostCount to a large number so that the
          // system will subtly remove this ``retired'' object
          object.lostCount = std::numeric_limits<int>::max();
        } else {
          indicator(i, j)  = 1;
          object.lostCount = 0;
          // The maximum of `trackScore` is `numberOfPreLooseCouplingSteps` + 1
          if (object.trackScore <= numberOfPreLooseCouplingSteps) {
            ++object.trackScore;
          }
          object.detection = detections->boxes[j];

          if (object.trackScore >= numberOfPreLooseCouplingSteps + 1) {
            // Use a tighter threshold to determine if this detection is good
            // for coupling. If this detection does not meet the requirement,
            // the object will be loosely-coupled, and the corresponding
            // `trackScore` will be deducted.
            auto tightlyCoupledDetectionFactorPtr = boost::make_shared<TightlyCoupledDetectionFactor>(egoPoseKey,
                                                                                                      object.poseNodeIndex,
                                                                                                      tightlyCoupledDetectionVector);
            // auto&& detectionError                 = tightlyCoupledDetectionFactorPtr->error(initialEstimateForAnalysis);
            double detectionError;
            std::tie(j, detectionError) = getDetectionIndexAndError(predictedPose, tightlyCoupledMatchingVector);

            auto spatialConsistencyTest  = detectionError <= tightCouplingDetectionErrorThreshold;
            auto temporalConsistencyTest = object.velocityIsConsistent(numberOfVelocityConsistencySteps, isamCurrentEstimate,
                                                                       objectAngularVelocityConsistencyVarianceThreshold,
                                                                       objectLinearVelocityConsistencyVarianceThreshold);
            if (spatialConsistencyTest && temporalConsistencyTest) {
              ++numberOfTightlyCoupledObjectsAtThisMoment;
              anyObjectIsTightlyCoupled = true;
              object.isTightlyCoupled   = true;
              gtSAMgraph.add(TightlyCoupledDetectionFactor(egoPoseKey,
                                                           object.poseNodeIndex,
                                                           tightlyCoupledDetectionVector,
                                                           j));
              object.tightlyCoupledDetectionFactorPtr = tightlyCoupledDetectionFactorPtr;
              object.initialDetectionError            = detectionError;
            } else {
              object.trackScore -= numberOfInterLooseCouplingSteps;
              object.isTightlyCoupled = false;
              initialEstimateForLooselyCoupledObjects.insert(object.poseNodeIndex, initialEstimate.at(object.poseNodeIndex));
              initialEstimateForLooselyCoupledObjects.insert(object.velocityNodeIndex, initialEstimate.at(object.velocityNodeIndex));
              initialEstimate.erase(object.poseNodeIndex);
              initialEstimate.erase(object.velocityNodeIndex);
              gtSAMgraphForLooselyCoupledObjects.add(LooselyCoupledDetectionFactor(egoPoseKey,
                                                                                   object.poseNodeIndex,
                                                                                   detectionVector,
                                                                                   j));
              object.looselyCoupledDetectionFactorPtr = boost::make_shared<LooselyCoupledDetectionFactor>(egoPoseKey,
                                                                                                          object.poseNodeIndex,
                                                                                                          detectionVector);
              object.initialDetectionError            = object.looselyCoupledDetectionFactorPtr->error(initialEstimateForAnalysis);
            }
          } else {
            // Pre-loosely coupled detection to stabilize the object velocity
            object.isTightlyCoupled = false;
            initialEstimateForLooselyCoupledObjects.insert(object.poseNodeIndex, initialEstimate.at(object.poseNodeIndex));
            initialEstimateForLooselyCoupledObjects.insert(object.velocityNodeIndex, initialEstimate.at(object.velocityNodeIndex));
            initialEstimate.erase(object.poseNodeIndex);
            initialEstimate.erase(object.velocityNodeIndex);
            gtSAMgraphForLooselyCoupledObjects.add(LooselyCoupledDetectionFactor(egoPoseKey,
                                                                                 object.poseNodeIndex,
                                                                                 detectionVector,
                                                                                 j));
            object.looselyCoupledDetectionFactorPtr = boost::make_shared<LooselyCoupledDetectionFactor>(egoPoseKey,
                                                                                                        object.poseNodeIndex,
                                                                                                        detectionVector);
            object.initialDetectionError            = object.looselyCoupledDetectionFactorPtr->error(initialEstimateForAnalysis);
          }
        }
      } else {  // lost
        ++object.lostCount;
        object.confidence                               = 0.0;
        object.trackScore                               = 0.0;
        objectPaths.markers[object.objectIndex].scale.x = 0.3;
        objectPaths.markers[object.objectIndex].scale.y = 0.3;
        objectPaths.markers[object.objectIndex].scale.z = 0.3;

        // Use a larger threshold to track object without association in the
        // factor graph. They are different objects in the factor graph, but in
        // the tracking (visualization) system, they are the same object.
        if (dataAssociationError < detectionMatchThreshold) {
          trackingObjectIndices[dataAssociationJ] = object.objectIndexForTracking;
          // A tricky part: set the lostCount to a large number so that the
          // system will subtly remove this ``retired'' object
          object.lostCount = std::numeric_limits<int>::max();
        } else {
          trackingObjectPaths.markers[object.objectIndexForTracking].scale.x = 0.3;
          trackingObjectPaths.markers[object.objectIndexForTracking].scale.y = 0.3;
          trackingObjectPaths.markers[object.objectIndexForTracking].scale.z = 0.3;
        }
      }

      ++i;
    }

#ifdef MAP_OPTIMIZATION_DEBUG
    cout << "\n\n"
         << "INDICATOR ::\n"
         << indicator << "\n\n";
#endif

    // Register a new dynamic object if the detection does not belong to any
    // current active objects
    for (size_t idx = 0; idx < detectionVector.size(); ++idx) {
      if (indicator.col(idx).sum() == 0) {
        // Initialize the object
        ObjectState object;
        object.detection         = detections->boxes[idx];
        object.pose              = egoPose * detectionVector[idx].getPose();
        object.velocity          = Pose3::identity();
        object.poseNodeIndex     = numberOfNodes++;
        object.velocityNodeIndex = numberOfNodes++;
        object.objectIndex       = numberOfRegisteredObjects++;
        object.isFirst           = true;
        object.timestamp         = timeLaserInfoStamp;
        if (trackingObjectIndices[idx] < 0) {
          object.objectIndexForTracking = numberOfTrackingObjects++;

          // Initialize a path object (marker) for visualizing path
          visualization_msgs::Marker marker;
          marker.id                 = object.objectIndexForTracking;
          marker.type               = visualization_msgs::Marker::SPHERE_LIST;
          std_msgs::ColorRGBA color = jsk_topic_tools::colorCategory20(object.objectIndexForTracking);
          marker.color.a            = 1.0;
          marker.color.r            = color.r;
          marker.color.g            = color.g;
          marker.color.b            = color.b;
          marker.scale.x            = 0.6;
          marker.scale.y            = 0.6;
          marker.scale.z            = 0.6;
          marker.pose.orientation   = tf::createQuaternionMsgFromYaw(0);
          trackingObjectPaths.markers.push_back(marker);

          visualization_msgs::Marker labelMarker;
          labelMarker.id      = object.objectIndexForTracking;
          labelMarker.type    = visualization_msgs::Marker::TEXT_VIEW_FACING;
          labelMarker.color.a = 1.0;
          labelMarker.color.r = color.r;
          labelMarker.color.g = color.g;
          labelMarker.color.b = color.b;
          labelMarker.scale.z = 1.2;
          labelMarker.text    = "Object " + std::to_string(object.objectIndexForTracking);
          trackingObjectLabels.markers.push_back(labelMarker);

          visualization_msgs::Marker velocityMarker;
          velocityMarker.id               = object.objectIndexForTracking;
          velocityMarker.type             = visualization_msgs::Marker::LINE_STRIP;
          velocityMarker.color.a          = 0.7;
          velocityMarker.color.r          = color.r;
          velocityMarker.color.g          = color.g;
          velocityMarker.color.b          = color.b;
          velocityMarker.scale.x          = 0.4;
          velocityMarker.scale.y          = 0.4;
          velocityMarker.scale.z          = 0.4;
          velocityMarker.pose.orientation = tf::createQuaternionMsgFromYaw(0);
          trackingObjectVelocities.markers.push_back(velocityMarker);

          velocityMarker.type = visualization_msgs::Marker::ARROW;
          trackingObjectVelocityArrows.markers.push_back(velocityMarker);
        } else {
          object.objectIndexForTracking                                      = trackingObjectIndices[idx];
          trackingObjectPaths.markers[object.objectIndexForTracking].scale.x = 0.6;
          trackingObjectPaths.markers[object.objectIndexForTracking].scale.y = 0.6;
          trackingObjectPaths.markers[object.objectIndexForTracking].scale.z = 0.6;
          trackingObjectLabels.markers[object.objectIndexForTracking].text   = "Object " + std::to_string(object.objectIndexForTracking);
        }

        // TODO: Propagate the bounding box in the post-processing
        object.box = detectionVector[idx].getBoundingBox();

        objects.back()[object.objectIndex] = object;

        // Initialize a path object (marker) for visualizing path
        visualization_msgs::Marker marker;
        marker.id                 = object.objectIndex;
        marker.type               = visualization_msgs::Marker::SPHERE_LIST;
        std_msgs::ColorRGBA color = jsk_topic_tools::colorCategory20(object.objectIndex);
        marker.color.a            = 1.0;
        marker.color.r            = color.r;
        marker.color.g            = color.g;
        marker.color.b            = color.b;
        marker.scale.x            = 0.6;
        marker.scale.y            = 0.6;
        marker.scale.z            = 0.6;
        marker.pose.orientation   = tf::createQuaternionMsgFromYaw(0);
        objectPaths.markers.push_back(marker);

        visualization_msgs::Marker labelMarker;
        labelMarker.id      = object.objectIndex;
        labelMarker.type    = visualization_msgs::Marker::TEXT_VIEW_FACING;
        labelMarker.color.a = 1.0;
        labelMarker.color.r = color.r;
        labelMarker.color.g = color.g;
        labelMarker.color.b = color.b;
        labelMarker.scale.z = 1.2;
        labelMarker.text    = "Object " + std::to_string(object.objectIndex);
        objectLabels.markers.push_back(labelMarker);

        visualization_msgs::Marker velocityMarker;
        velocityMarker.id               = object.objectIndex;
        velocityMarker.type             = visualization_msgs::Marker::LINE_STRIP;
        velocityMarker.color.a          = 0.7;
        velocityMarker.color.r          = color.r;
        velocityMarker.color.g          = color.g;
        velocityMarker.color.b          = color.b;
        velocityMarker.scale.x          = 0.4;
        velocityMarker.scale.y          = 0.4;
        velocityMarker.scale.z          = 0.4;
        velocityMarker.pose.orientation = tf::createQuaternionMsgFromYaw(0);
        objectVelocities.markers.push_back(velocityMarker);

        velocityMarker.type = visualization_msgs::Marker::ARROW;
        objectVelocityArrows.markers.push_back(velocityMarker);

        initialEstimateForLooselyCoupledObjects.insert(object.poseNodeIndex, object.pose);
        initialEstimateForLooselyCoupledObjects.insert(object.velocityNodeIndex, object.velocity);

        initialEstimateForAnalysis.insert(object.poseNodeIndex, object.pose);
        initialEstimateForAnalysis.insert(object.velocityNodeIndex, object.velocity);

        // detection factor
        gtSAMgraphForLooselyCoupledObjects.add(LooselyCoupledDetectionFactor(egoPoseKey,
                                                                             object.poseNodeIndex,
                                                                             detectionVector,
                                                                             idx));
        objects.back()[object.objectIndex].looselyCoupledDetectionFactorPtr = boost::make_shared<LooselyCoupledDetectionFactor>(egoPoseKey,
                                                                                                                                object.poseNodeIndex,
                                                                                                                                detectionVector);

#ifndef ENABLE_COMPACT_VERSION_OF_FACTOR_GRAPH
        // prior velocity factor (the noise should be large)
        auto noise = noiseModel::Diagonal::Variances((Vector(6) << 1e-2, 1e-2, 1e0, 1e8, 1e2, 1e2).finished());
        gtSAMgraphForLooselyCoupledObjects.add(PriorFactor<Pose3>(object.velocityNodeIndex, object.velocity, noise));
#endif
      }
    }
  }

  void saveKeyFramesAndFactor() {
    bool requiredSaveFrame = saveFrame();

#ifdef ENABLE_SIMULTANEOUS_LOCALIZATION_AND_TRACKING
    if (requiredSaveFrame) {
      // odom factor
      addOdomFactor();

      // gps factor
      addGPSFactor();

      // loop factor
      addLoopFactor();
    } else {
#ifdef ENABLE_ASYNCHRONOUS_STATE_ESTIMATE_FOR_SLOT
      // add the latest ego-pose to the initial guess set
      auto egoPose6D      = cloudKeyPoses6D->back();
      Pose3 latestEgoPose = Pose3(Rot3::RzRyRx((Vector3() << egoPose6D.roll, egoPose6D.pitch, egoPose6D.yaw).finished()),
                                  Point3((Vector3() << egoPose6D.x, egoPose6D.y, egoPose6D.z).finished()));
      initialEstimate.insert(keyPoseIndices.back(), latestEgoPose);
      initialEstimateForAnalysis.insert(keyPoseIndices.back(), latestEgoPose);
#else
      return;
#endif
    }
#else  // LIO-SAM
    if (!requiredSaveFrame)
      return;

    // odom factor
    addOdomFactor();

    // gps factor
    addGPSFactor();

    // loop factor
    addLoopFactor();
#endif

#ifdef ENABLE_SIMULTANEOUS_LOCALIZATION_AND_TRACKING
    // perform dynamic object propagation
    propagateObjectPoses();

    // detection factor (for multi-object tracking tracking)
    addDetectionFactor(!requiredSaveFrame);

    // // constant velocity factor (for multi-object tracking)
    addConstantVelocityFactor();

    // stable pose factor (for multi-object tracking)
    addStablePoseFactor();
#endif

#ifdef MAP_OPTIMIZATION_DEBUG
    std::cout << "****************************************************" << endl;
    gtSAMgraph.print("GTSAM Graph:\n");
#endif

    if (!requiredSaveFrame) {
      initialEstimate.erase(keyPoseIndices.back());
      initialEstimateForAnalysis.erase(keyPoseIndices.back());
    }

    // update iSAM
    isam->update(gtSAMgraph, initialEstimate);
    isam->update();

    if (aLoopIsClosed == true) {
      isam->update();
      isam->update();
      isam->update();
      isam->update();
      isam->update();
    }

    gtSAMgraph.resize(0);
    initialEstimate.clear();
    initialEstimateForAnalysis.clear();

#ifdef ENABLE_SIMULTANEOUS_LOCALIZATION_AND_TRACKING
    if (!gtSAMgraphForLooselyCoupledObjects.empty()) {
      isam->update(gtSAMgraphForLooselyCoupledObjects, initialEstimateForLooselyCoupledObjects);
      isam->update();
    }

    gtSAMgraphForLooselyCoupledObjects.resize(0);
    initialEstimateForLooselyCoupledObjects.clear();
#endif

    isamCurrentEstimate = isam->calculateEstimate();

    if (requiredSaveFrame) {
      // save key poses
      PointType thisPose3D;
      PointTypePose thisPose6D;
      Pose3 latestEstimate;

      latestEstimate = isamCurrentEstimate.at<Pose3>(keyPoseIndices.back());
      // cout << "****************************************************" << endl;
      // isamCurrentEstimate.print("Current estimate: ");

      thisPose3D.x         = latestEstimate.translation().x();
      thisPose3D.y         = latestEstimate.translation().y();
      thisPose3D.z         = latestEstimate.translation().z();
      thisPose3D.intensity = cloudKeyPoses3D->size();  // this can be used as index
      cloudKeyPoses3D->push_back(thisPose3D);

      thisPose6D.x         = thisPose3D.x;
      thisPose6D.y         = thisPose3D.y;
      thisPose6D.z         = thisPose3D.z;
      thisPose6D.intensity = thisPose3D.intensity;  // this can be used as index
      thisPose6D.roll      = latestEstimate.rotation().roll();
      thisPose6D.pitch     = latestEstimate.rotation().pitch();
      thisPose6D.yaw       = latestEstimate.rotation().yaw();
      thisPose6D.time      = timeLaserInfoCur;
      cloudKeyPoses6D->push_back(thisPose6D);

      // cout << "****************************************************" << endl;
      // cout << "Pose covariance:" << endl;
      // cout << isam->marginalCovariance(isamCurrentEstimate.size()-1) << endl << endl;
      poseCovariance = isam->marginalCovariance(keyPoseIndices.back());

      // save updated transform
      transformTobeMapped[0] = latestEstimate.rotation().roll();
      transformTobeMapped[1] = latestEstimate.rotation().pitch();
      transformTobeMapped[2] = latestEstimate.rotation().yaw();
      transformTobeMapped[3] = latestEstimate.translation().x();
      transformTobeMapped[4] = latestEstimate.translation().y();
      transformTobeMapped[5] = latestEstimate.translation().z();

      // save all the received edge and surf points
      pcl::PointCloud<PointType>::Ptr thisCornerKeyFrame(new pcl::PointCloud<PointType>());
      pcl::PointCloud<PointType>::Ptr thisSurfKeyFrame(new pcl::PointCloud<PointType>());
      pcl::copyPointCloud(*laserCloudCornerLastDS, *thisCornerKeyFrame);
      pcl::copyPointCloud(*laserCloudSurfLastDS, *thisSurfKeyFrame);

      // save key frame cloud
      cornerCloudKeyFrames.push_back(thisCornerKeyFrame);
      surfCloudKeyFrames.push_back(thisSurfKeyFrame);

      // save path for visualization
      updatePath(thisPose6D);
    }

    // save dynamic objects
    if (objects.size() > 0) {
      size_t i = 0;
      for (auto& pairedObject : objects.back()) {
        auto& object = pairedObject.second;

        // Since lost objects do not join the optimization, skip the value
        // update for them
        if (object.lostCount > 0) continue;

#ifdef MAP_OPTIMIZATION_DEBUG
        std::cout << "(OBJECT " << object.objectIndex << ") [BEFORE]\nPOSITION ::\n"
                  << object.pose << "\n"
                  << "VELOCITY ::\n"
                  << object.velocity << "\n";
#endif

        object.pose = isamCurrentEstimate.at<Pose3>(object.poseNodeIndex);
#ifdef ENABLE_COMPACT_VERSION_OF_FACTOR_GRAPH
        if (!object.isFirst) {
          object.velocity = isamCurrentEstimate.at<Pose3>(object.velocityNodeIndex);
        }
#else
        object.velocity = isamCurrentEstimate.at<Pose3>(object.velocityNodeIndex);
#endif

#ifdef MAP_OPTIMIZATION_DEBUG
        std::cout << "(OBJECT " << object.objectIndex << ") [AFTER ]\nPOSITION ::\n"
                  << object.pose << "\n"
                  << "VELOCITY ::\n"
                  << object.velocity << "\n\n";
#endif

        // TODO: Reproduce the post-processing of tracking (refer to FG-3DMOT)
        // // Check if there is any detection belongs to the object.
        // int index;
        // double error;
        // std::tie(index, error) = getDetectionIndexAndError(object.pose, detectionVector);

        // if (error < detectionMatchThreshold) {  // found
        // object.box = detections->boxes[index];

        auto p                     = object.pose.translation();
        object.box.pose.position.x = p.x();
        object.box.pose.position.y = p.y();
        object.box.pose.position.z = p.z();

        auto r                      = object.pose.rotation();
        object.box.pose.orientation = tf::createQuaternionMsgFromRollPitchYaw(r.roll(), r.pitch(), r.yaw());

        object.box.header.frame_id = odometryFrame;
        object.box.label           = object.objectIndex;
        // object.confidence          = object.box.value;

        // } else {  // lost
        //   object.confidence = 0;
        // }

        ++i;
      }
    }
  }

  void correctPoses() {
    if (cloudKeyPoses3D->points.empty())
      return;

    if (aLoopIsClosed || anyObjectIsTightlyCoupled) {
      // clear map cache
      laserCloudMapContainer.clear();
      // clear path
      globalPath.poses.clear();
      // update key poses
      int numPoses = keyPoseIndices.size();
      for (int i = 0; i < numPoses; ++i) {
        auto poseIndex               = keyPoseIndices[i];
        cloudKeyPoses3D->points[i].x = isamCurrentEstimate.at<Pose3>(poseIndex).translation().x();
        cloudKeyPoses3D->points[i].y = isamCurrentEstimate.at<Pose3>(poseIndex).translation().y();
        cloudKeyPoses3D->points[i].z = isamCurrentEstimate.at<Pose3>(poseIndex).translation().z();

        cloudKeyPoses6D->points[i].x     = cloudKeyPoses3D->points[i].x;
        cloudKeyPoses6D->points[i].y     = cloudKeyPoses3D->points[i].y;
        cloudKeyPoses6D->points[i].z     = cloudKeyPoses3D->points[i].z;
        cloudKeyPoses6D->points[i].roll  = isamCurrentEstimate.at<Pose3>(poseIndex).rotation().roll();
        cloudKeyPoses6D->points[i].pitch = isamCurrentEstimate.at<Pose3>(poseIndex).rotation().pitch();
        cloudKeyPoses6D->points[i].yaw   = isamCurrentEstimate.at<Pose3>(poseIndex).rotation().yaw();

        updatePath(cloudKeyPoses6D->points[i]);
      }

      aLoopIsClosed = false;
    }
  }

  void updatePath(const PointTypePose& pose_in) {
    geometry_msgs::PoseStamped pose_stamped;
    pose_stamped.header.stamp       = ros::Time().fromSec(pose_in.time);
    pose_stamped.header.frame_id    = odometryFrame;
    pose_stamped.pose.position.x    = pose_in.x;
    pose_stamped.pose.position.y    = pose_in.y;
    pose_stamped.pose.position.z    = pose_in.z;
    tf::Quaternion q                = tf::createQuaternionFromRPY(pose_in.roll, pose_in.pitch, pose_in.yaw);
    pose_stamped.pose.orientation.x = q.x();
    pose_stamped.pose.orientation.y = q.y();
    pose_stamped.pose.orientation.z = q.z();
    pose_stamped.pose.orientation.w = q.w();

    globalPath.poses.push_back(pose_stamped);
  }

  void publishOdometry() {
    // Publish odometry for ROS (global)
    nav_msgs::Odometry laserOdometryROS;
    laserOdometryROS.header.stamp          = timeLaserInfoStamp;
    laserOdometryROS.header.frame_id       = odometryFrame;
    laserOdometryROS.child_frame_id        = "odom_mapping";
    laserOdometryROS.pose.pose.position.x  = transformTobeMapped[3];
    laserOdometryROS.pose.pose.position.y  = transformTobeMapped[4];
    laserOdometryROS.pose.pose.position.z  = transformTobeMapped[5];
    laserOdometryROS.pose.pose.orientation = tf::createQuaternionMsgFromRollPitchYaw(transformTobeMapped[0], transformTobeMapped[1], transformTobeMapped[2]);
    pubLaserOdometryGlobal.publish(laserOdometryROS);

    // Publish TF
    static tf::TransformBroadcaster br;
    tf::Transform t_odom_to_lidar            = tf::Transform(tf::createQuaternionFromRPY(transformTobeMapped[0], transformTobeMapped[1], transformTobeMapped[2]),
                                                             tf::Vector3(transformTobeMapped[3], transformTobeMapped[4], transformTobeMapped[5]));
    tf::StampedTransform trans_odom_to_lidar = tf::StampedTransform(t_odom_to_lidar, timeLaserInfoStamp, odometryFrame, "lidar_link");
    br.sendTransform(trans_odom_to_lidar);

    // Publish odometry for ROS (incremental)
    static bool lastIncreOdomPubFlag = false;
    static nav_msgs::Odometry laserOdomIncremental;  // incremental odometry msg
    static Eigen::Affine3f increOdomAffine;          // incremental odometry in affine
    if (lastIncreOdomPubFlag == false) {
      lastIncreOdomPubFlag = true;
      laserOdomIncremental = laserOdometryROS;
      increOdomAffine      = trans2Affine3f(transformTobeMapped);
    } else {
      Eigen::Affine3f affineIncre = incrementalOdometryAffineFront.inverse() * incrementalOdometryAffineBack;
      increOdomAffine             = increOdomAffine * affineIncre;
      float x, y, z, roll, pitch, yaw;
      pcl::getTranslationAndEulerAngles(increOdomAffine, x, y, z, roll, pitch, yaw);
      if (cloudInfo.imuAvailable == true) {
        if (std::abs(cloudInfo.imuPitchInit) < 1.4) {
          double imuWeight = 0.1;
          tf::Quaternion imuQuaternion;
          tf::Quaternion transformQuaternion;
          double rollMid, pitchMid, yawMid;

          // slerp roll
          transformQuaternion.setRPY(roll, 0, 0);
          imuQuaternion.setRPY(cloudInfo.imuRollInit, 0, 0);
          tf::Matrix3x3(transformQuaternion.slerp(imuQuaternion, imuWeight)).getRPY(rollMid, pitchMid, yawMid);
          roll = rollMid;

          // slerp pitch
          transformQuaternion.setRPY(0, pitch, 0);
          imuQuaternion.setRPY(0, cloudInfo.imuPitchInit, 0);
          tf::Matrix3x3(transformQuaternion.slerp(imuQuaternion, imuWeight)).getRPY(rollMid, pitchMid, yawMid);
          pitch = pitchMid;
        }
      }
      laserOdomIncremental.header.stamp          = timeLaserInfoStamp;
      laserOdomIncremental.header.frame_id       = odometryFrame;
      laserOdomIncremental.child_frame_id        = "odom_mapping";
      laserOdomIncremental.pose.pose.position.x  = x;
      laserOdomIncremental.pose.pose.position.y  = y;
      laserOdomIncremental.pose.pose.position.z  = z;
      laserOdomIncremental.pose.pose.orientation = tf::createQuaternionMsgFromRollPitchYaw(roll, pitch, yaw);
      if (isDegenerate)
        laserOdomIncremental.pose.covariance[0] = 1;
      else
        laserOdomIncremental.pose.covariance[0] = 0;
    }
    pubLaserOdometryIncremental.publish(laserOdomIncremental);
  }

  void publishFrames() {
    if (cloudKeyPoses3D->points.empty())
      return;
    // publish key poses
    publishCloud(&pubKeyPoses, cloudKeyPoses3D, timeLaserInfoStamp, odometryFrame);
    // Publish surrounding key frames
    publishCloud(&pubRecentKeyFrames, laserCloudSurfFromMapDS, timeLaserInfoStamp, odometryFrame);
    // publish registered key frame
    if (pubRecentKeyFrame.getNumSubscribers() != 0) {
      pcl::PointCloud<PointType>::Ptr cloudOut(new pcl::PointCloud<PointType>());
      PointTypePose thisPose6D = trans2PointTypePose(transformTobeMapped);
      *cloudOut += *transformPointCloud(laserCloudCornerLastDS, &thisPose6D);
      *cloudOut += *transformPointCloud(laserCloudSurfLastDS, &thisPose6D);
      publishCloud(&pubRecentKeyFrame, cloudOut, timeLaserInfoStamp, odometryFrame);
    }
    // publish registered high-res raw cloud
    if (pubCloudRegisteredRaw.getNumSubscribers() != 0) {
      pcl::PointCloud<PointType>::Ptr cloudOut(new pcl::PointCloud<PointType>());
      pcl::fromROSMsg(cloudInfo.cloud_deskewed, *cloudOut);
      PointTypePose thisPose6D = trans2PointTypePose(transformTobeMapped);
      *cloudOut                = *transformPointCloud(cloudOut, &thisPose6D);
      publishCloud(&pubCloudRegisteredRaw, cloudOut, timeLaserInfoStamp, odometryFrame);
    }
    // publish path
    if (pubPath.getNumSubscribers() != 0) {
      globalPath.header.stamp    = timeLaserInfoStamp;
      globalPath.header.frame_id = odometryFrame;
      pubPath.publish(globalPath);
    }
    // publish detections
    if (pubDetection.getNumSubscribers() != 0 && detectionIsActive) {
      pubDetection.publish(detections);
    }
    if (pubLaserCloudDeskewed.getNumSubscribers() != 0) {
      cloudInfo.header.stamp = timeLaserInfoStamp;
      pubLaserCloudDeskewed.publish(cloudInfo.cloud_deskewed);
    }
    // public dynamic objects
    if (detectionIsActive) {
      BoundingBoxArray objectMessage;
      BoundingBoxArray trackingObjectMessage;
      objectMessage.header          = detections->header;
      objectMessage.header.frame_id = odometryFrame;
      objectMessage.header.stamp    = timeLaserInfoStamp;

      trackingObjectMessage.header          = detections->header;
      trackingObjectMessage.header.frame_id = odometryFrame;
      trackingObjectMessage.header.stamp    = timeLaserInfoStamp;

      tightlyCoupledObjectPoints.header.frame_id = odometryFrame;
      tightlyCoupledObjectPoints.header.stamp    = timeLaserInfoStamp;

      for (auto& marker : objectVelocities.markers) {
        marker.points.clear();
      }
      for (auto& marker : objectVelocityArrows.markers) {
        marker.points.clear();
        marker.scale.x = 0;
        marker.scale.y = 0;
        marker.scale.z = 0;
      }
      for (auto& marker : trackingObjectVelocities.markers) {
        marker.points.clear();
      }
      for (auto& marker : trackingObjectVelocityArrows.markers) {
        marker.points.clear();
        marker.scale.x = 0;
        marker.scale.y = 0;
        marker.scale.z = 0;
      }

      objectStates.objects.clear();
      objectStates.header.frame_id = odometryFrame;
      objectStates.header.stamp    = timeLaserInfoStamp;

      std::vector<bool> trackingObjectIsActive(numberOfTrackingObjects, false);
      for (auto& pairedObject : objects.back()) {
        auto& object = pairedObject.second;
        // Only publish active objects
        if (object.lostCount == 0) {
          // TODO: A better data structure to present moving object with path
          // Mark the tracking object as an active instance
          trackingObjectIsActive[object.objectIndexForTracking] = true;

          // Bounding box
          object.box.header.stamp = timeLaserInfoStamp;
          objectMessage.boxes.push_back(object.box);
          trackingObjectMessage.boxes.push_back(object.box);
          trackingObjectMessage.boxes.back().label = object.objectIndexForTracking;

          // Path
          geometry_msgs::Point point;
          point.x = object.box.pose.position.x;
          point.y = object.box.pose.position.y;
          point.z = object.box.pose.position.z;
          objectPaths.markers[object.objectIndex].points.push_back(point);
          objectPaths.markers[object.objectIndex].header.frame_id = odometryFrame;
          objectPaths.markers[object.objectIndex].header.stamp    = timeLaserInfoStamp;

          trackingObjectPaths.markers[object.objectIndexForTracking].points.push_back(point);
          trackingObjectPaths.markers[object.objectIndexForTracking].header.frame_id = odometryFrame;
          trackingObjectPaths.markers[object.objectIndexForTracking].header.stamp    = timeLaserInfoStamp;

          // Tightly-coupled nodes (poses)
          if (object.isTightlyCoupled) {
            tightlyCoupledObjectPoints.points.push_back(point);
          }

          // Label
          objectLabels.markers[object.objectIndex].pose.position.x = object.box.pose.position.x;
          objectLabels.markers[object.objectIndex].pose.position.y = object.box.pose.position.y;
          objectLabels.markers[object.objectIndex].pose.position.z = object.box.pose.position.z + 2.0;
          objectLabels.markers[object.objectIndex].header.frame_id = odometryFrame;
          objectLabels.markers[object.objectIndex].header.stamp    = timeLaserInfoStamp;

          trackingObjectLabels.markers[object.objectIndexForTracking].pose.position.x = object.box.pose.position.x;
          trackingObjectLabels.markers[object.objectIndexForTracking].pose.position.y = object.box.pose.position.y;
          trackingObjectLabels.markers[object.objectIndexForTracking].pose.position.z = object.box.pose.position.z + 2.0;
          trackingObjectLabels.markers[object.objectIndexForTracking].header.frame_id = odometryFrame;
          trackingObjectLabels.markers[object.objectIndexForTracking].header.stamp    = timeLaserInfoStamp;

          // Velocity (prediction of path)
          objectVelocities.markers[object.objectIndex].header.frame_id     = odometryFrame;
          objectVelocities.markers[object.objectIndex].header.stamp        = timeLaserInfoStamp;
          objectVelocityArrows.markers[object.objectIndex].header.frame_id = odometryFrame;
          objectVelocityArrows.markers[object.objectIndex].header.stamp    = timeLaserInfoStamp;
          objectVelocityArrows.markers[object.objectIndex].scale.x         = 0.4;
          objectVelocityArrows.markers[object.objectIndex].scale.y         = 0.8;
          objectVelocityArrows.markers[object.objectIndex].scale.z         = 1.0;

          trackingObjectVelocities.markers[object.objectIndexForTracking].header.frame_id     = odometryFrame;
          trackingObjectVelocities.markers[object.objectIndexForTracking].header.stamp        = timeLaserInfoStamp;
          trackingObjectVelocityArrows.markers[object.objectIndexForTracking].header.frame_id = odometryFrame;
          trackingObjectVelocityArrows.markers[object.objectIndexForTracking].header.stamp    = timeLaserInfoStamp;
          trackingObjectVelocityArrows.markers[object.objectIndexForTracking].scale.x         = 0.4;
          trackingObjectVelocityArrows.markers[object.objectIndexForTracking].scale.y         = 0.8;
          trackingObjectVelocityArrows.markers[object.objectIndexForTracking].scale.z         = 1.0;

          // .. Compute the delta pose with respect to the delta time
          auto identity     = gtsam::Pose3::identity();
          auto deltaPoseVec = gtsam::traits<gtsam::Pose3>::Local(identity, object.velocity) * 0.1;
          auto deltaPose    = gtsam::traits<gtsam::Pose3>::Retract(identity, deltaPoseVec);
          auto nextPose     = object.pose;

          // .. Object position at relative time stamp from 1 to 5
          for (int timeStamp = 1; timeStamp <= 5; ++timeStamp) {
            nextPose = nextPose * deltaPose;
            point.x  = nextPose.translation().x();
            point.y  = nextPose.translation().y();
            point.z  = nextPose.translation().z();
            if (timeStamp <= 4) {
              objectVelocities.markers[object.objectIndex].points.push_back(point);
              trackingObjectVelocities.markers[object.objectIndexForTracking].points.push_back(point);
            }
            if (timeStamp >= 4) {
              objectVelocityArrows.markers[object.objectIndex].points.push_back(point);
              trackingObjectVelocityArrows.markers[object.objectIndexForTracking].points.push_back(point);
            }
          }

          // Diagnosis
          lio_segmot::ObjectState state;
          state.header.frame_id        = odometryFrame;
          state.header.stamp           = timeLaserInfoStamp;
          state.detection              = object.detection;
          state.pose                   = object.box.pose;
          state.velocity.position.x    = object.velocity.translation().x();
          state.velocity.position.y    = object.velocity.translation().y();
          state.velocity.position.z    = object.velocity.translation().z();
          state.velocity.orientation.x = object.velocity.rotation().quaternion().x();
          state.velocity.orientation.y = object.velocity.rotation().quaternion().y();
          state.velocity.orientation.z = object.velocity.rotation().quaternion().z();
          state.velocity.orientation.w = object.velocity.rotation().quaternion().w();
          state.index                  = object.objectIndex;
          state.lostCount              = object.lostCount;
          state.confidence             = object.confidence;
          state.isTightlyCoupled       = object.isTightlyCoupled;
          state.isFirst                = object.isFirst;

          state.hasTightlyCoupledDetectionError = false;
          if (object.tightlyCoupledDetectionFactorPtr) {
            state.hasTightlyCoupledDetectionError     = true;
            state.tightlyCoupledDetectionError        = object.tightlyCoupledDetectionFactorPtr->error(isamCurrentEstimate);
            state.initialTightlyCoupledDetectionError = object.initialDetectionError;
          }

          state.hasLooselyCoupledDetectionError = false;
          if (object.looselyCoupledDetectionFactorPtr) {
            state.hasLooselyCoupledDetectionError     = true;
            state.looselyCoupledDetectionError        = object.looselyCoupledDetectionFactorPtr->error(isamCurrentEstimate);
            state.initialLooselyCoupledDetectionError = object.initialDetectionError;
          }

          state.hasMotionError = false;
          if (object.motionFactorPtr) {
            state.hasMotionError     = true;
            state.motionError        = object.motionFactorPtr->error(isamCurrentEstimate);
            state.initialMotionError = object.initialMotionError;
          }

          objectStates.objects.push_back(state);
        }
      }

      // Hide moving object if it only appears one time and it is not active
      for (auto& pairedObject : objects.back()) {
        auto& object = pairedObject.second;
        auto& index  = object.objectIndexForTracking;
        if (!trackingObjectIsActive[index] && object.lostCount != 0) {
          if (trackingObjectPaths.markers[index].points.size() <= 1) {
            trackingObjectPaths.markers[index].points.clear();
            trackingObjectLabels.markers[index].text = "";
          }
        }
      }

      pubObjects.publish(objectMessage);
      pubObjectPaths.publish(objectPaths);
      pubTightlyCoupledObjectPoints.publish(tightlyCoupledObjectPoints);
      pubObjectLabels.publish(objectLabels);
      pubObjectVelocities.publish(objectVelocities);
      pubObjectVelocityArrows.publish(objectVelocityArrows);
      pubTrackingObjects.publish(trackingObjectMessage);
      pubTrackingObjectPaths.publish(trackingObjectPaths);
      pubTrackingObjectLabels.publish(trackingObjectLabels);
      pubTrackingObjectVelocities.publish(trackingObjectVelocities);
      pubTrackingObjectVelocityArrows.publish(trackingObjectVelocityArrows);
      pubObjectStates.publish(objectStates);
    }

    lio_segmot::Diagnosis diagnosis;
    diagnosis.header.frame_id               = odometryFrame;
    diagnosis.header.stamp                  = timeLaserInfoStamp;
    diagnosis.numberOfDetections            = detections ? detections->boxes.size() : 0;
    diagnosis.computationalTime             = timer.elapsed();
    diagnosis.numberOfTightlyCoupledObjects = numberOfTightlyCoupledObjectsAtThisMoment;
    pubDiagnosis.publish(diagnosis);
  }
};

int main(int argc, char** argv) {
  ros::init(argc, argv, "lio_segmot");

  mapOptimization MO;

  ROS_INFO("\033[1;32m----> Map Optimization Started.\033[0m");

  std::thread loopthread(&mapOptimization::loopClosureThread, &MO);
  std::thread visualizeMapThread(&mapOptimization::visualizeGlobalMapThread, &MO);

  ros::spin();

  loopthread.join();
  visualizeMapThread.join();

  return 0;
}
