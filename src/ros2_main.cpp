#include <csignal>
#include <unistd.h>
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>
#include <sensor_msgs/msg/imu.hpp>
#include <sensor_msgs/msg/nav_sat_fix.hpp>
#include <nav_msgs/msg/odometry.hpp>
#include <nav_msgs/msg/path.hpp>
#include <pcl_conversions/pcl_conversions.h>
#include <tf2_ros/transform_broadcaster.h>

#include <geometry_msgs/msg/pose_with_covariance_stamped.hpp>
#include <visualization_msgs/msg/marker.hpp>
#include <visualization_msgs/msg/marker_array.hpp>
#include "pgo/Backend.hpp"
#include "slam_interfaces/srv/backend_opt.hpp"
#include "ParametersRos2.h"

FILE *location_log = nullptr;
bool showOptimizedPose = true;
double globalMapVisualizationSearchRadius = 1000;
double globalMapVisualizationPoseDensity = 10;
double globalMapVisualizationLeafSize = 1;
double lidar_end_time = 0;
bool path_en = true, scan_pub_en = false, dense_pub_en = false;
Backend backend;

rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr pubLaserCloudFull;
rclcpp::Publisher<nav_msgs::msg::Odometry>::SharedPtr pubOdomAftMapped;
rclcpp::Publisher<nav_msgs::msg::Path>::SharedPtr pubLidarPath;
rclcpp::Publisher<nav_msgs::msg::Odometry>::SharedPtr pubOdomNotFix;
rclcpp::Publisher<visualization_msgs::msg::MarkerArray>::SharedPtr pubLoopConstraintEdge;
std::shared_ptr<tf2_ros::TransformBroadcaster> broadcaster;
std::string map_frame;
std::string lidar_frame;

bool flg_exit = false;
void SigHandle(int sig)
{
    flg_exit = true;
    LOG_WARN("catch sig %d", sig);
}

void gnss_cbk(const sensor_msgs::msg::NavSatFix::SharedPtr msg)
{
    backend.gnss->gnss_handler(GnssPose(msg->header.stamp.sec + msg->header.stamp.nanosec * 1.0e-9, V3D(msg->latitude, msg->longitude, msg->altitude)));
    backend.relocalization->gnss_pose = GnssPose(msg->header.stamp.sec + msg->header.stamp.nanosec * 1.0e-9, V3D(msg->latitude, msg->longitude, msg->altitude));
}

#ifdef UrbanLoco
void UrbanLoco_cbk(const nav_msgs::msg::Odometry::SharedPtr msg)
{
    backend.gnss->UrbanLoco_handler(GnssPose(msg->header.stamp.sec + msg->header.stamp.nanosec * 1.0e-9,
                                             V3D(msg->pose.pose.position.x, msg->pose.pose.position.y, msg->pose.pose.position.z),
                                             QD(msg->pose.pose.orientation.w, msg->pose.pose.orientation.x, msg->pose.pose.orientation.y, msg->pose.pose.orientation.z),
                                             V3D(msg->pose.covariance[21], msg->pose.covariance[28], msg->pose.covariance[35])));
    backend.relocalization->gnss_pose = GnssPose(msg->header.stamp.sec + msg->header.stamp.nanosec * 1.0e-9,
                                                 V3D(msg->pose.pose.position.x, msg->pose.pose.position.y, msg->pose.pose.position.z),
                                                 QD(msg->pose.pose.orientation.w, msg->pose.pose.orientation.x, msg->pose.pose.orientation.y, msg->pose.pose.orientation.z));
}
#endif

void publish_cloud(rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr &pubCloud, PointCloudType::Ptr cloud, const double& lidar_end_time, const std::string& frame_id)
{
    sensor_msgs::msg::PointCloud2 cloud_msg;
    pcl::toROSMsg(*cloud, cloud_msg);
    cloud_msg.header.stamp = rclcpp::Time(lidar_end_time * 1e9);
    cloud_msg.header.frame_id = frame_id;
    pubCloud->publish(cloud_msg);
}

void publish_cloud_world(rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr &pubLaserCloudFull, PointCloudType::Ptr laserCloud, const PointXYZIRPYT &state, const double& lidar_end_time)
{
    PointCloudType::Ptr laserCloudWorld(new PointCloudType(laserCloud->size(), 1));
    pointcloudLidarToWorld(laserCloud, laserCloudWorld, state);
    publish_cloud(pubLaserCloudFull, laserCloudWorld, lidar_end_time, map_frame);
}

template <typename T>
void set_posestamp(T &out, const QD &rot, const V3D &pos)
{
    out.pose.position.x = pos(0);
    out.pose.position.y = pos(1);
    out.pose.position.z = pos(2);
    out.pose.orientation.x = rot.coeffs()[0];
    out.pose.orientation.y = rot.coeffs()[1];
    out.pose.orientation.z = rot.coeffs()[2];
    out.pose.orientation.w = rot.coeffs()[3];
}

void publish_tf(const geometry_msgs::msg::Pose &pose, const double &lidar_end_time)
{
    geometry_msgs::msg::TransformStamped transform_stamped;
    transform_stamped.header.stamp = rclcpp::Time(lidar_end_time * 1e9);
    // lidar -> map
    transform_stamped.header.frame_id = map_frame;
    transform_stamped.child_frame_id = lidar_frame;
    transform_stamped.transform.translation.x = pose.position.x;
    transform_stamped.transform.translation.y = pose.position.y;
    transform_stamped.transform.translation.z = pose.position.z;
    transform_stamped.transform.rotation.x = pose.orientation.x;
    transform_stamped.transform.rotation.y = pose.orientation.y;
    transform_stamped.transform.rotation.z = pose.orientation.z;
    transform_stamped.transform.rotation.w = pose.orientation.w;
    broadcaster->sendTransform(transform_stamped);
}

// 发布里程计
void publish_odometry(rclcpp::Publisher<nav_msgs::msg::Odometry>::SharedPtr &pubOdomAftMapped,
                      const PointXYZIRPYT &state, const double &lidar_end_time, bool need_publish_tf = true)
{
    nav_msgs::msg::Odometry odomAftMapped;
    odomAftMapped.header.frame_id = map_frame;
    odomAftMapped.child_frame_id = lidar_frame;
    odomAftMapped.header.stamp = rclcpp::Time(lidar_end_time * 1e9);
    const QD &lidar_rot = EigenMath::RPY2Quaternion(V3D(state.roll, state.pitch, state.yaw));
    const V3D &lidar_pos = V3D(state.x, state.y, state.z);
    set_posestamp(odomAftMapped.pose, lidar_rot, lidar_pos);
    pubOdomAftMapped->publish(odomAftMapped);
    if (need_publish_tf)
        publish_tf(odomAftMapped.pose.pose, lidar_end_time);
}

void publish_lidar_keyframe_trajectory(rclcpp::Publisher<nav_msgs::msg::Path>::SharedPtr &pubPath, const pcl::PointCloud<PointXYZIRPYT> &trajectory, const double &lidar_end_time)
{
    nav_msgs::msg::Path path;
    path.header.stamp = rclcpp::Time(lidar_end_time * 1e9);
    path.header.frame_id = map_frame;

    geometry_msgs::msg::PoseStamped msg_lidar_pose;
    for (const auto &point : trajectory)
    {
        msg_lidar_pose.pose.position.x = point.x;
        msg_lidar_pose.pose.position.y = point.y;
        msg_lidar_pose.pose.position.z = point.z;
        auto quat = EigenMath::RPY2Quaternion(V3D(point.roll, point.pitch, point.yaw));
        msg_lidar_pose.pose.orientation.x = quat.x();
        msg_lidar_pose.pose.orientation.y = quat.y();
        msg_lidar_pose.pose.orientation.z = quat.z();
        msg_lidar_pose.pose.orientation.w = quat.w();

        msg_lidar_pose.header.stamp = rclcpp::Time(lidar_end_time * 1e9);
        msg_lidar_pose.header.frame_id = map_frame;

        path.poses.push_back(msg_lidar_pose);
    }

    pubPath->publish(path);
}

void visualize_loop_closure_constraints(rclcpp::Publisher<visualization_msgs::msg::MarkerArray>::SharedPtr &pubLoopConstraintEdge, const double &timestamp,
                                        const unordered_map<int, int> &loop_constraint_records,
                                        const pcl::PointCloud<PointXYZIRPYT>::Ptr keyframe_pose6d)
{
    if (loop_constraint_records.empty())
        return;

    visualization_msgs::msg::MarkerArray markerArray;
    // loop nodes
    visualization_msgs::msg::Marker markerNode;
    markerNode.header.frame_id = map_frame;
    markerNode.header.stamp = rclcpp::Time(timestamp * 1e9);
    markerNode.action = visualization_msgs::msg::Marker::ADD;
    markerNode.type = visualization_msgs::msg::Marker::SPHERE_LIST;
    markerNode.ns = "loop_nodes";
    markerNode.id = 0;
    markerNode.pose.orientation.w = 1;
    markerNode.scale.x = 0.3;
    markerNode.scale.y = 0.3;
    markerNode.scale.z = 0.3;
    markerNode.color.r = 0;
    markerNode.color.g = 0.8;
    markerNode.color.b = 1;
    markerNode.color.a = 1;
    // loop edges
    visualization_msgs::msg::Marker markerEdge;
    markerEdge.header.frame_id = map_frame;
    markerEdge.header.stamp = rclcpp::Time(timestamp * 1e9);
    markerEdge.action = visualization_msgs::msg::Marker::ADD;
    markerEdge.type = visualization_msgs::msg::Marker::LINE_LIST;
    markerEdge.ns = "loop_edges";
    markerEdge.id = 1;
    markerEdge.pose.orientation.w = 1;
    markerEdge.scale.x = 0.1;
    markerEdge.color.r = 0.9;
    markerEdge.color.g = 0.9;
    markerEdge.color.b = 0;
    markerEdge.color.a = 1;

    for (auto it = loop_constraint_records.begin(); it != loop_constraint_records.end(); ++it)
    {
        int key_cur = it->first;
        int key_pre = it->second;
        geometry_msgs::msg::Point p;
        p.x = keyframe_pose6d->points[key_cur].x;
        p.y = keyframe_pose6d->points[key_cur].y;
        p.z = keyframe_pose6d->points[key_cur].z;
        markerNode.points.push_back(p);
        markerEdge.points.push_back(p);
        p.x = keyframe_pose6d->points[key_pre].x;
        p.y = keyframe_pose6d->points[key_pre].y;
        p.z = keyframe_pose6d->points[key_pre].z;
        markerNode.points.push_back(p);
        markerEdge.points.push_back(p);
    }

    markerArray.markers.push_back(markerNode);
    markerArray.markers.push_back(markerEdge);
    pubLoopConstraintEdge->publish(markerArray);
}

void visualize_globalmap_thread(rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr pubGlobalmap)
{
    while (!flg_exit)
    {
        this_thread::sleep_for(std::chrono::seconds(1));
        auto submap_visual = backend.get_submap_visual(globalMapVisualizationSearchRadius, globalMapVisualizationPoseDensity, globalMapVisualizationLeafSize, showOptimizedPose);
        if (submap_visual == nullptr)
            continue;
        publish_cloud(pubGlobalmap, submap_visual, lidar_end_time, map_frame);
    }
}

void initialPoseCallback(const geometry_msgs::msg::PoseWithCovarianceStamped::SharedPtr msg)
{
    const geometry_msgs::msg::Pose &pose = msg->pose.pose;
    const auto &ori = msg->pose.pose.orientation;
    Eigen::Quaterniond quat(ori.w, ori.x, ori.y, ori.z);
    auto rpy = EigenMath::Quaternion2RPY(quat);
    // prior pose in map(imu pose)
    Pose init_pose;
    init_pose.x = pose.position.x;
    init_pose.y = pose.position.y;
    init_pose.z = pose.position.z;
    init_pose.roll = rpy.x();
    init_pose.pitch = rpy.y();
    init_pose.yaw = rpy.z();
    backend.relocalization->set_init_pose(init_pose);
}

bool pgo_callback(const slam_interfaces::srv::BackendOpt::Request::SharedPtr request, slam_interfaces::srv::BackendOpt::Response::SharedPtr response)
{
    PointXYZIRPYT this_pose6d;
    PointCloudType::Ptr feats_undistort(new PointCloudType());
    PointCloudType::Ptr submap_fix(new PointCloudType());

    this_pose6d.x = request->pose[0];
    this_pose6d.y = request->pose[1];
    this_pose6d.z = request->pose[2];
    this_pose6d.roll = request->pose[3];
    this_pose6d.pitch = request->pose[4];
    this_pose6d.yaw = request->pose[5];
    this_pose6d.time = request->pose[6];
    publish_odometry(pubOdomAftMapped, this_pose6d, this_pose6d.time);

    pcl::fromROSMsg(request->cloud_undistort, *feats_undistort);
    backend.run(this_pose6d, feats_undistort, submap_fix);
    if (submap_fix->size())
    {
        response->pose_fix.emplace_back(this_pose6d.x);
        response->pose_fix.emplace_back(this_pose6d.y);
        response->pose_fix.emplace_back(this_pose6d.z);
        response->pose_fix.emplace_back(this_pose6d.roll);
        response->pose_fix.emplace_back(this_pose6d.pitch);
        response->pose_fix.emplace_back(this_pose6d.yaw);
        response->pose_fix.emplace_back(this_pose6d.time);
        pcl::toROSMsg(*submap_fix, response->submap_fix);
    }

    lidar_end_time = this_pose6d.time;

    /******* Publish odometry *******/
    publish_odometry(pubOdomNotFix, this_pose6d, this_pose6d.time, false);

    /******* Publish points *******/
    if (path_en)
    {
        publish_lidar_keyframe_trajectory(pubLidarPath, *backend.keyframe_pose6d_optimized, this_pose6d.time);
    }
    if (scan_pub_en)
        if (dense_pub_en)
            publish_cloud_world(pubLaserCloudFull, feats_undistort, this_pose6d, this_pose6d.time);
        // else
        //     publish_cloud(pubLaserCloudFull, frontend.feats_down_world, this_pose6d.time, map_frame);

    visualize_loop_closure_constraints(pubLoopConstraintEdge, this_pose6d.time, backend.loopClosure->loop_constraint_records, backend.loopClosure->copy_keyframe_pose6d);
    return true;
}

int main(int argc, char **argv)
{
    rclcpp::init(argc, argv);
    auto node = std::make_shared<rclcpp::Node>("backend_optimization");
    string gnss_topic;

    bool save_globalmap_en = false;
    bool save_pgm = false;
    double pgm_resolution;
    float min_z, max_z;
    // location_log = fopen(DEBUG_FILE_DIR("location.log").c_str(), "a");

    node->declare_parameter("showOptimizedPose", true);
    node->declare_parameter("globalMapVisualizationSearchRadius", 1000.);
    node->declare_parameter("globalMapVisualizationPoseDensity", 10.);
    node->declare_parameter("globalMapVisualizationLeafSize", 1.);
    node->get_parameter("showOptimizedPose", showOptimizedPose);
    node->get_parameter("globalMapVisualizationSearchRadius", globalMapVisualizationSearchRadius);
    node->get_parameter("globalMapVisualizationPoseDensity", globalMapVisualizationPoseDensity);
    node->get_parameter("globalMapVisualizationLeafSize", globalMapVisualizationLeafSize);

    load_ros_parameters(node, path_en, scan_pub_en, dense_pub_en, gnss_topic, map_frame, lidar_frame);
    load_parameters(node, backend);
    load_pgm_parameters(node, save_globalmap_en, save_pgm, pgm_resolution, min_z, max_z);

    /*** ROS subscribe initialization ***/
    // 发布当前正在扫描的点云，topic名字为/cloud_registered
    auto pubLaserCloudFull = node->create_publisher<sensor_msgs::msg::PointCloud2>("/cloud_registered", 1000);
    auto pubOdomAftMapped = node->create_publisher<nav_msgs::msg::Odometry>("/odom_fix", 1000);
    auto pubLidarPath = node->create_publisher<nav_msgs::msg::Path>("/lidar_keyframe_trajectory", 1000);
    auto pubOdomNotFix = node->create_publisher<nav_msgs::msg::Odometry>("/odom_not_fix", 1000);
    auto pubLoopConstraintEdge = node->create_publisher<visualization_msgs::msg::MarkerArray>("/loop_closure_constraints", 1);

    auto pubGlobalmap = node->create_publisher<sensor_msgs::msg::PointCloud2>("/map_global", 1);
    std::thread visualizeMapThread = std::thread(&visualize_globalmap_thread, pubGlobalmap);
    auto sub_initpose = node->create_subscription<geometry_msgs::msg::PoseWithCovarianceStamped>("/initialpose", 1, initialPoseCallback);
    broadcaster = std::make_shared<tf2_ros::TransformBroadcaster>(node);
    auto service = node->create_service<slam_interfaces::srv::BackendOpt>("/pgo_service", &pgo_callback);
    RCLCPP_INFO(rclcpp::get_logger("rclcpp"), "pgo service ready!");

    //------------------------------------------------------------------------------------------------------
    signal(SIGINT, SigHandle);
    rclcpp::Rate rate(5000);
    while (rclcpp::ok())
    {
        if (flg_exit)
            break;
        rclcpp::spin_some(node);
        rate.sleep();
    }

    backend.save_trajectory();
    backend.save_factor_graph();
    // backend.save_trajectory_to_other_frame(frontend.get_state().offset_R_L_I, frontend.get_state().offset_T_L_I, "imu");
    // backend.save_trajectory_to_other_frame(QD(M3D(backend.gnss->extrinsic_lidar2gnss.topLeftCorner(3, 3))), backend.gnss->extrinsic_lidar2gnss.topLeftCorner(3, 1), "gnss");

    if (save_globalmap_en)
        backend.save_globalmap();

    if (save_pgm)
        backend.save_pgm(pgm_resolution, min_z, max_z);

    return 0;
}
