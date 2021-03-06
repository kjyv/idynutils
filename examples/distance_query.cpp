/**
 @author Karsten Knese
 @author Alessio Rocchi
 */

#include <geometry_msgs/Transform.h>
#include <idynutils/collision_utils.h>
#include <ros/ros.h>
#include <sensor_msgs/JointState.h>
#include <tf/transform_broadcaster.h>
#include <visualization_msgs/Marker.h>
#include <visualization_msgs/MarkerArray.h>

static const std::string JOINT_STATE_TOPIC = "/joint_states";
static const std::string RESULT_MARKER_TOPIC = "distance_query/result_marker";

int id_counter = 1;
std::string base_frame = "base_link";

bool draw_point(const double x, const double y, const double z,
                const std::string frame, visualization_msgs::Marker& marker, float color=1.0) {
    //DRAW REFERENCE
    marker.header.frame_id = frame;
    marker.header.stamp = ros::Time().now();
    marker.ns = "goal";
    marker.id = ++id_counter;
    marker.type = visualization_msgs::Marker::SPHERE;
    marker.action = visualization_msgs::Marker::ADD;

    marker.pose.position.x = x;
    marker.pose.position.y = y;
    marker.pose.position.z = z;
    marker.pose.orientation.x = 0.0;
    marker.pose.orientation.y = 0.0;
    marker.pose.orientation.z = 0.0;
    marker.pose.orientation.w = 1.0;
    marker.scale.x = 0.02;
    marker.scale.y = 0.02;
    marker.scale.z = 0.02;

    marker.color.r = color;
    marker.color.g = 0.0;
    marker.color.b = 0.0;
    marker.color.a = 1.0;
    return true;
}

void createMarkerArray(std::list<LinkPairDistance>& results,
                       const boost::shared_ptr<visualization_msgs::MarkerArray>& markers) {
    typedef std::list<LinkPairDistance>::iterator iter_pairs;
    unsigned int indicator = 0;
    for(iter_pairs it = results.begin(); it != results.end(); ++it)
    {
        visualization_msgs::Marker m1;
        std::pair<std::string, std::string> linkNames = it->getLinkNames();
        std::pair<KDL::Frame, KDL::Frame> transforms = it->getLink_T_closestPoint();
        draw_point( transforms.first.p.x(),
                    transforms.first.p.y(),
                    transforms.first.p.z(),
                    linkNames.first, m1, indicator++);

        visualization_msgs::Marker m2;
        draw_point( transforms.second.p.x(),
                    transforms.second.p.y(),
                    transforms.second.p.z(),
                    linkNames.second, m2, indicator++);

        markers->markers.push_back(m1);
        markers->markers.push_back(m2);
    }
}

int main(int argc, char** argv) {
    iDynUtils bigman("bigman",
                     std::string(IDYNUTILS_TESTS_ROBOTS_DIR)+"bigman/bigman.urdf",
                     std::string(IDYNUTILS_TESTS_ROBOTS_DIR)+"bigman/bigman.srdf");

    ros::init(argc, argv, "distance_computation");
    ros::NodeHandle nh;
    double rate = 100;
    ros::Rate loopRate(rate);

    const sensor_msgs::JointStateConstPtr initJoints =
            ros::topic::waitForMessage<sensor_msgs::JointState>(
                    JOINT_STATE_TOPIC, nh);

    ROS_INFO("Joint States published");
    ROS_INFO("length of joint states %d", initJoints->name.size());

    boost::shared_ptr<ComputeLinksDistance> distance_comp(
            new ComputeLinksDistance(bigman));

    /*
    std::list<std::pair<std::string,std::string>> whiteList;
    whiteList.push_back(std::pair<std::string,std::string>("LSoftHandLink","RSoftHandLink"));
    whiteList.push_back(std::pair<std::string,std::string>("backpack","LSoftHandLink"));
    whiteList.push_back(std::pair<std::string,std::string>("RSoftHandLink","backpack"));
    distance_comp->setCollisionWhiteList(whiteList);*/


    ros::Subscriber joint_states_subscriber = nh.subscribe<
            sensor_msgs::JointState>(JOINT_STATE_TOPIC, 1, // Buffer size
            &iDynUtils::updateiDyn3ModelFromJoinStateMsg, &bigman);

    ros::Publisher resultMarkerPub = nh.advertise<
            visualization_msgs::MarkerArray>(RESULT_MARKER_TOPIC, 10);
    boost::shared_ptr<visualization_msgs::MarkerArray> markers(
            new visualization_msgs::MarkerArray);

    while (ros::ok()) {
        ROS_INFO("looping");
        ros::Time tic = ros::Time::now();
        std::list<LinkPairDistance> results = distance_comp->getLinkDistances();
        ros::Time toc = ros::Time::now();

        ROS_INFO("minimum_distance computed, results found %d distances in %fs", results.size(), toc.toSec()-tic.toSec());

        while(results.size() > 15) results.pop_back();

        std::list<LinkPairDistance>::iterator it = results.begin();
        ROS_INFO("first distance result: %f, p0={%f, %f, %f} p1={%f, %f, %f}",
                 it->getDistance(),
                 it->getLink_T_closestPoint().first.p.x(),
                 it->getLink_T_closestPoint().first.p.y(),
                 it->getLink_T_closestPoint().first.p.z(),
                 it->getLink_T_closestPoint().second.p.x(),
                 it->getLink_T_closestPoint().second.p.y(),
                 it->getLink_T_closestPoint().second.p.z());

        if (results.size() > 0) {
            markers->markers.clear();
            id_counter = 0;

            createMarkerArray(results, markers);
            resultMarkerPub.publish(markers);
        }

        ros::spinOnce();
        loopRate.sleep();
    }

    return EXIT_SUCCESS;
}
