#include <gtest/gtest.h>
#include <idynutils/collision_utils.h>
#include <idynutils/idynutils.h>
#include <idynutils/tests_utils.h>
#include <iCub/iDynTree/yarp_kdl.h>
#include <yarp/math/Math.h>
#include <yarp/math/SVD.h>
#include <yarp/sig/Vector.h>
#include <yarp/os/all.h>
#include <cmath>
#include <fcl/distance.h>
#include <fcl/shape/geometric_shapes.h>
#include <eigen_conversions/eigen_kdl.h>
#include <iostream>
#include <fstream>

#include "svm.h"


#define  s                1.0
#define  dT               0.001* s
#define  m_s              1.0
#define toRad(X) (X * M_PI/180.0)
#define SMALL_NUM 1e-5

KDL::Frame fcl2KDL(const fcl::Transform3f &in)
{
    fcl::Quaternion3f q = in.getQuatRotation();
    fcl::Vec3f t = in.getTranslation();

    KDL::Frame f;
    f.p = KDL::Vector(t[0],t[1],t[2]);
    f.M = KDL::Rotation::Quaternion(q.getX(), q.getY(), q.getZ(), q.getW());

    return f;
}

yarp::sig::Vector getGoodInitialPosition(iDynUtils& idynutils) {
    yarp::sig::Vector q(idynutils.iDyn3_model.getNrOfDOFs(), 0.0);
    yarp::sig::Vector leg(idynutils.left_leg.getNrOfDOFs(), 0.0);
    leg[0] = -25.0 * M_PI/180.0;
    leg[3] =  50.0 * M_PI/180.0;
    leg[5] = -25.0 * M_PI/180.0;
    idynutils.fromRobotToIDyn(leg, q, idynutils.left_leg);
    idynutils.fromRobotToIDyn(leg, q, idynutils.right_leg);
    yarp::sig::Vector arm(idynutils.left_arm.getNrOfDOFs(), 0.0);
    arm[0] = 20.0 * M_PI/180.0;
    arm[1] = 10.0 * M_PI/180.0;
    arm[3] = -80.0 * M_PI/180.0;
    idynutils.fromRobotToIDyn(arm, q, idynutils.left_arm);
    arm[1] = -arm[1];
    idynutils.fromRobotToIDyn(arm, q, idynutils.right_arm);
    return q;
}

double dist3D_Segment_to_Segment (const Eigen::Vector3d & segment_A_endpoint_1,
                                  const Eigen::Vector3d & segment_A_endpoint_2,
                                  const Eigen::Vector3d & segment_B_endpoint_1,
                                  const Eigen::Vector3d & segment_B_endpoint_2,
                                  Eigen::Vector3d & closest_point_on_segment_A,
                                  Eigen::Vector3d & closest_point_on_segment_B)
{

    using namespace Eigen;

    Vector3d   u = segment_A_endpoint_2 - segment_A_endpoint_1;
    Vector3d   v = segment_B_endpoint_2 - segment_B_endpoint_1;
    Vector3d   w = segment_A_endpoint_1 - segment_B_endpoint_1;
    double    a = u.dot(u);         // always >= 0
    double    b = u.dot(v);
    double    c = v.dot(v);         // always >= 0
    double    d = u.dot(w);
    double    e = v.dot(w);
    double    D = a*c - b*b;        // always >= 0
    double    sc, sN, sD = D;       // sc = sN / sD, default sD = D >= 0
    double    tc, tN, tD = D;       // tc = tN / tD, default tD = D >= 0

    // compute the line parameters of the two closest points
    if (D < SMALL_NUM) { // the lines are almost parallel
        sN = 0.0;         // force using point P0 on segment S1
        sD = 1.0;         // to prevent possible division by 0.0 later
        tN = e;
        tD = c;
    }
    else {                 // get the closest points on the infinite lines
        sN = (b*e - c*d);
        tN = (a*e - b*d);
        if (sN < 0.0) {        // sc < 0 => the s=0 edge is visible
            sN = 0.0;
            tN = e;
            tD = c;
        }
        else if (sN > sD) {  // sc > 1  => the s=1 edge is visible
            sN = sD;
            tN = e + b;
            tD = c;
        }
    }

    if (tN < 0.0) {            // tc < 0 => the t=0 edge is visible
        tN = 0.0;
        // recompute sc for this edge
        if (-d < 0.0)
            sN = 0.0;
        else if (-d > a)
            sN = sD;
        else {
            sN = -d;
            sD = a;
        }
    }
    else if (tN > tD) {      // tc > 1  => the t=1 edge is visible
        tN = tD;
        // recompute sc for this edge
        if ((-d + b) < 0.0)
            sN = 0;
        else if ((-d + b) > a)
            sN = sD;
        else {
            sN = (-d + b);
            sD = a;
        }
    }
    // finally do the division to get sc and tc
    sc = (std::fabs(sN) < SMALL_NUM ? 0.0 : sN / sD);
    tc = (std::fabs(tN) < SMALL_NUM ? 0.0 : tN / tD);

    closest_point_on_segment_A = segment_A_endpoint_1 + sc * u;
    closest_point_on_segment_B = segment_B_endpoint_1 + tc * v;

//    std::cout << "CP1: " << std::endl << CP1 << std::endl;
//    std::cout << "CP2: " << std::endl << CP2 << std::endl;

    // get the difference of the two closest points
    Vector3d   dP = closest_point_on_segment_A - closest_point_on_segment_B;  // =  S1(sc) - S2(tc)

    double Dm = dP.norm();   // return the closest distance

    // I leave the line here for observing the minimum distance between the inner line segments of the corresponding capsule pair
    //std::cout << "Dm: " << std::endl << Dm << std::endl;

    return Dm;
}

yarp::sig::Vector getRandomConfiguration (iDynUtils& robot_model, random_numbers::RandomNumberGenerator &rng)
{
    yarp::sig::Vector q(robot_model.iDyn3_model.getNrOfDOFs(), 0.0);

    yarp::sig::Vector q_bound_max, q_bound_min;
    q_bound_min = robot_model.iDyn3_model.getJointBoundMin();
    q_bound_max = robot_model.iDyn3_model.getJointBoundMax();

    for (unsigned int i = 0 ; i < robot_model.iDyn3_model.getNrOfDOFs() ; ++i)
    {
        q(i) = rng.uniformReal( q_bound_min(i), q_bound_max(i) );
    }

//    std::cout<<"whole q: "<<q.toString()<<std::endl;

    return q;
}

class TestCapsuleLinksDistance
{

private:
    ComputeLinksDistance& _computeDistance;

public:

    TestCapsuleLinksDistance(ComputeLinksDistance& computeDistance)
        :_computeDistance(computeDistance)
    {

    }

    std::map<std::string,boost::shared_ptr<fcl::CollisionGeometry> > getShapes()
    {
        return _computeDistance.shapes_;
    }

    std::map<std::string,boost::shared_ptr<fcl::CollisionObject> > getcollision_objects()
    {
        return _computeDistance.collision_objects_;
    }

    std::map<std::string,KDL::Frame> getlink_T_shape()
    {
        return _computeDistance.link_T_shape;
    }

    std::map<std::string,boost::shared_ptr<ComputeLinksDistance::Capsule> > getcustom_capsules()
    {
        return _computeDistance.custom_capsules_;
    }

    bool updateCollisionObjects()
    {
        return _computeDistance.updateCollisionObjects();
    }

    bool globalToLinkCoordinates(const std::string& linkName,
                                 const fcl::Transform3f &fcl_w_T_f,
                                 KDL::Frame &link_T_f)
    {

        return _computeDistance.globalToLinkCoordinates(linkName, fcl_w_T_f, link_T_f);
    }

    bool globalToLinkCoordinatesKDL(const std::string& linkName,
                                    const fcl::Transform3f &fcl_w_T_f,
                                    KDL::Frame &link_T_f)
    {

        KDL::Frame w_T_f = fcl2KDL(fcl_w_T_f);

        fcl::Transform3f fcl_w_T_shape = _computeDistance.collision_objects_[linkName]->getTransform();
        KDL::Frame w_T_shape = fcl2KDL(fcl_w_T_shape);

        KDL::Frame shape_T_f = w_T_shape.Inverse()*w_T_f;

        link_T_f = _computeDistance.link_T_shape[linkName] * shape_T_f;

        return true;
    }

};

namespace {

class testCollisionUtils : public ::testing::Test{
 protected:

  testCollisionUtils():
      robot("bigman",
            std::string(IDYNUTILS_TESTS_ROBOTS_DIR)+"bigman/bigman.urdf",
            std::string(IDYNUTILS_TESTS_ROBOTS_DIR)+"bigman/bigman.srdf"),
      q(robot.iDyn3_model.getNrOfDOFs(), 0.0),
      compute_distance(robot)
  {

       /*//////////////////whiteList_L_R_Arms////////////////////*/

      std::string linkA1 = "LSoftHandLink";
      std::string linkA2 = "LWrMot3";
      std::string linkA3 = "LWrMot2";
      std::string linkA4 = "LForearm";
      std::string linkA5 = "LElb";
      std::string linkA6 = "LShy";
      std::string linkA7 = "LShr";
      std::string linkA8 = "LShp";

      std::string linkB1 = "RSoftHandLink";
      std::string linkB2 = "RWrMot3";
      std::string linkB3 = "RWrMot2";
      std::string linkB4 = "RForearm";
      std::string linkB5 = "RElb";
      std::string linkB6 = "RShy";
      std::string linkB7 = "RShr";
      std::string linkB8 = "RShp";

      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA1,linkB1));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA1,linkB2));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA1,linkB3));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA1,linkB4));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA1,linkB5));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA1,linkB6));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA1,linkB7));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA1,linkB8));

      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA2,linkB1));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA2,linkB2));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA2,linkB3));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA2,linkB4));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA2,linkB5));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA2,linkB6));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA2,linkB7));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA2,linkB8));

      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA3,linkB1));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA3,linkB2));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA3,linkB3));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA3,linkB4));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA3,linkB5));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA3,linkB6));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA3,linkB7));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA3,linkB8));

      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA4,linkB1));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA4,linkB2));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA4,linkB3));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA4,linkB4));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA4,linkB5));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA4,linkB6));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA4,linkB7));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA4,linkB8));

      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA5,linkB1));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA5,linkB2));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA5,linkB3));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA5,linkB4));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA5,linkB5));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA5,linkB6));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA5,linkB7));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA5,linkB8));

      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA6,linkB1));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA6,linkB2));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA6,linkB3));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA6,linkB4));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA6,linkB5));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA6,linkB6));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA6,linkB7));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA6,linkB8));

      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA7,linkB1));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA7,linkB2));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA7,linkB3));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA7,linkB4));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA7,linkB5));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA7,linkB6));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA7,linkB7));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA7,linkB8));

      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA8,linkB1));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA8,linkB2));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA8,linkB3));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA8,linkB4));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA8,linkB5));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA8,linkB6));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA8,linkB7));
      whiteList_L_R_Arms.push_back(std::pair<std::string,std::string>(linkA8,linkB8));

      /*//////////////////whiteList_L_Arm_Torso////////////////////*/

      linkA1 = "LSoftHandLink";
      linkA2 = "LWrMot3";
      linkA3 = "LWrMot2";
      linkA4 = "LForearm";
      linkA5 = "LElb";
      linkA6 = "LShy";
      linkA7 = "LShr";
      linkA8 = "LShp";

      linkB1 = "Waist";
      linkB2 = "DWL";
      linkB3 = "DWS";
      linkB4 = "DWYTorso";
      linkB5 = "TorsoProtections";

      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA1,linkB1));
      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA1,linkB2));
      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA1,linkB3));
      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA1,linkB4));
      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA1,linkB5));

      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA2,linkB1));
      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA2,linkB2));
      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA2,linkB3));
      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA2,linkB4));
      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA2,linkB5));

      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA3,linkB1));
      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA3,linkB2));
      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA3,linkB3));
      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA3,linkB4));
      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA3,linkB5));

      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA4,linkB1));
      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA4,linkB2));
      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA4,linkB3));
      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA4,linkB4));
      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA4,linkB5));

      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA5,linkB1));
      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA5,linkB2));
      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA5,linkB3));
      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA5,linkB4));
      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA5,linkB5));

      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA6,linkB1));
      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA6,linkB2));
      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA6,linkB3));
      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA6,linkB4));
      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA6,linkB5));

      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA7,linkB1));
      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA7,linkB2));
      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA7,linkB3));
      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA7,linkB4));
      //whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA7,linkB5));

      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA8,linkB1));
      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA8,linkB2));
      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA8,linkB3));
      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA8,linkB4));
      whitelist_L_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA8,linkB5));

      /*//////////////////whiteList_R_Arm_Torso////////////////////*/

      linkA1 = "RSoftHandLink";
      linkA2 = "RWrMot3";
      linkA3 = "RWrMot2";
      linkA4 = "RForearm";
      linkA5 = "RElb";
      linkA6 = "RShy";
      linkA7 = "RShr";
      linkA8 = "RShp";

      linkB1 = "Waist";
      linkB2 = "DWL";
      linkB3 = "DWS";
      linkB4 = "DWYTorso";
      linkB5 = "TorsoProtections";

      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA1,linkB1));
      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA1,linkB2));
      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA1,linkB3));
      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA1,linkB4));
      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA1,linkB5));

      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA2,linkB1));
      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA2,linkB2));
      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA2,linkB3));
      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA2,linkB4));
      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA2,linkB5));

      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA3,linkB1));
      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA3,linkB2));
      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA3,linkB3));
      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA3,linkB4));
      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA3,linkB5));

      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA4,linkB1));
      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA4,linkB2));
      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA4,linkB3));
      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA4,linkB4));
      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA4,linkB5));

      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA5,linkB1));
      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA5,linkB2));
      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA5,linkB3));
      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA5,linkB4));
      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA5,linkB5));

      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA6,linkB1));
      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA6,linkB2));
      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA6,linkB3));
      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA6,linkB4));
      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA6,linkB5));

      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA7,linkB1));
      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA7,linkB2));
      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA7,linkB3));
      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA7,linkB4));
      //whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA7,linkB5));

      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA8,linkB1));
      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA8,linkB2));
      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA8,linkB3));
      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA8,linkB4));
      whitelist_R_Arm_Torso.push_back(std::pair<std::string,std::string>(linkA8,linkB5));

      /*//////////////////whiteList_L_Arm_L_Leg////////////////////*/

      linkA1 = "LSoftHandLink";
      linkA2 = "LWrMot3";
      linkA3 = "LWrMot2";
      linkA4 = "LForearm";
      linkA5 = "LElb";
      linkA6 = "LShy";
      linkA7 = "LShr";
      linkA8 = "LShp";

      linkB1 = "LHipMot";
      linkB2 = "LThighUpLeg";
      linkB3 = "LThighLowLeg";
      linkB4 = "LLowLeg";
      linkB5 = "LFootmot";

      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA1,linkB1));
      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA1,linkB2));
      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA1,linkB3));
      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA1,linkB4));
      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA1,linkB5));

      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA2,linkB1));
      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA2,linkB2));
      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA2,linkB3));
      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA2,linkB4));
      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA2,linkB5));

      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA3,linkB1));
      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA3,linkB2));
      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA3,linkB3));
      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA3,linkB4));
      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA3,linkB5));

      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA4,linkB1));
      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA4,linkB2));
      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA4,linkB3));
      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA4,linkB4));
      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA4,linkB5));

      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA5,linkB1));
      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA5,linkB2));
      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA5,linkB3));
      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA5,linkB4));
      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA5,linkB5));

      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA6,linkB1));
      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA6,linkB2));
      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA6,linkB3));
      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA6,linkB4));
      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA6,linkB5));

      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA7,linkB1));
      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA7,linkB2));
      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA7,linkB3));
      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA7,linkB4));
      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA7,linkB5));

      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA8,linkB1));
      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA8,linkB2));
      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA8,linkB3));
      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA8,linkB4));
      whiteList_L_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA8,linkB5));

      /*//////////////////whiteList_R_Arm_R_Leg////////////////////*/

      linkA1 = "RSoftHandLink";
      linkA2 = "RWrMot3";
      linkA3 = "RWrMot2";
      linkA4 = "RForearm";
      linkA5 = "RElb";
      linkA6 = "RShy";
      linkA7 = "RShr";
      linkA8 = "RShp";

      linkB1 = "RHipMot";
      linkB2 = "RThighUpLeg";
      linkB3 = "RThighLowLeg";
      linkB4 = "RLowLeg";
      linkB5 = "RFootmot";

      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA1,linkB1));
      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA1,linkB2));
      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA1,linkB3));
      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA1,linkB4));
      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA1,linkB5));

      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA2,linkB1));
      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA2,linkB2));
      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA2,linkB3));
      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA2,linkB4));
      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA2,linkB5));

      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA3,linkB1));
      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA3,linkB2));
      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA3,linkB3));
      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA3,linkB4));
      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA3,linkB5));

      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA4,linkB1));
      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA4,linkB2));
      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA4,linkB3));
      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA4,linkB4));
      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA4,linkB5));

      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA5,linkB1));
      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA5,linkB2));
      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA5,linkB3));
      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA5,linkB4));
      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA5,linkB5));

      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA6,linkB1));
      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA6,linkB2));
      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA6,linkB3));
      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA6,linkB4));
      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA6,linkB5));

      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA7,linkB1));
      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA7,linkB2));
      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA7,linkB3));
      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA7,linkB4));
      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA7,linkB5));

      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA8,linkB1));
      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA8,linkB2));
      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA8,linkB3));
      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA8,linkB4));
      whiteList_R_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA8,linkB5));

      /*//////////////////whiteList_L_Arm_R_Leg////////////////////*/

      linkA1 = "LSoftHandLink";
      linkA2 = "LWrMot3";
      linkA3 = "LWrMot2";
      linkA4 = "LForearm";
      linkA5 = "LElb";
      linkA6 = "LShy";
      linkA7 = "LShr";
      linkA8 = "LShp";

      linkB1 = "RHipMot";
      linkB2 = "RThighUpLeg";
      linkB3 = "RThighLowLeg";
      linkB4 = "RLowLeg";
      linkB5 = "RFootmot";

      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA1,linkB1));
      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA1,linkB2));
      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA1,linkB3));
      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA1,linkB4));
      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA1,linkB5));

      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA2,linkB1));
      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA2,linkB2));
      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA2,linkB3));
      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA2,linkB4));
      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA2,linkB5));

      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA3,linkB1));
      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA3,linkB2));
      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA3,linkB3));
      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA3,linkB4));
      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA3,linkB5));

      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA4,linkB1));
      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA4,linkB2));
      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA4,linkB3));
      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA4,linkB4));
      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA4,linkB5));

      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA5,linkB1));
      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA5,linkB2));
      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA5,linkB3));
      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA5,linkB4));
      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA5,linkB5));

      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA6,linkB1));
      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA6,linkB2));
      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA6,linkB3));
      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA6,linkB4));
      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA6,linkB5));

      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA7,linkB1));
      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA7,linkB2));
      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA7,linkB3));
      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA7,linkB4));
      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA7,linkB5));

      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA8,linkB1));
      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA8,linkB2));
      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA8,linkB3));
      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA8,linkB4));
      whiteList_L_Arm_R_Leg.push_back(std::pair<std::string,std::string>(linkA8,linkB5));

      /*//////////////////whiteList_R_Arm_L_Leg////////////////////*/

      linkA1 = "RSoftHandLink";
      linkA2 = "RWrMot3";
      linkA3 = "RWrMot2";
      linkA4 = "RForearm";
      linkA5 = "RElb";
      linkA6 = "RShy";
      linkA7 = "RShr";
      linkA8 = "RShp";

      linkB1 = "LHipMot";
      linkB2 = "LThighUpLeg";
      linkB3 = "LThighLowLeg";
      linkB4 = "LLowLeg";
      linkB5 = "LFootmot";

      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA1,linkB1));
      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA1,linkB2));
      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA1,linkB3));
      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA1,linkB4));
      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA1,linkB5));

      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA2,linkB1));
      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA2,linkB2));
      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA2,linkB3));
      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA2,linkB4));
      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA2,linkB5));

      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA3,linkB1));
      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA3,linkB2));
      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA3,linkB3));
      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA3,linkB4));
      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA3,linkB5));

      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA4,linkB1));
      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA4,linkB2));
      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA4,linkB3));
      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA4,linkB4));
      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA4,linkB5));

      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA5,linkB1));
      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA5,linkB2));
      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA5,linkB3));
      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA5,linkB4));
      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA5,linkB5));

      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA6,linkB1));
      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA6,linkB2));
      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA6,linkB3));
      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA6,linkB4));
      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA6,linkB5));

      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA7,linkB1));
      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA7,linkB2));
      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA7,linkB3));
      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA7,linkB4));
      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA7,linkB5));

      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA8,linkB1));
      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA8,linkB2));
      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA8,linkB3));
      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA8,linkB4));
      whiteList_R_Arm_L_Leg.push_back(std::pair<std::string,std::string>(linkA8,linkB5));

  }

  virtual ~testCollisionUtils() {
  }

  virtual void SetUp() {
  }

  virtual void TearDown() {
  }

  iDynUtils robot;
  yarp::sig::Vector q;
  ComputeLinksDistance compute_distance;
  std::list<std::pair<std::string,std::string>> whiteList_L_R_Arms, whitelist_L_Arm_Torso, whitelist_R_Arm_Torso,
  whiteList_L_Arm_L_Leg, whiteList_R_Arm_R_Leg, whiteList_L_Arm_R_Leg, whiteList_R_Arm_L_Leg;

};

TEST_F(testCollisionUtils, testDistanceChecksAreInvariant) {

      std::list<std::pair<std::string,std::string>> whiteList;
      whiteList.push_back(std::pair<std::string,std::string>("LSoftHandLink","RSoftHandLink"));
      compute_distance.setCollisionWhiteList(whiteList);

      q = getGoodInitialPosition(robot);
      robot.updateiDyn3Model(q, false);

      std::list<LinkPairDistance> results = compute_distance.getLinkDistances();
      LinkPairDistance result1 = results.front();

      robot.updateiDyn3Model(q, false);
      results.clear();
      results = compute_distance.getLinkDistances();
      LinkPairDistance result2 = results.front();
      ASSERT_EQ(result1.getDistance(), result2.getDistance());
      ASSERT_EQ(result1.getLinkNames(), result2.getLinkNames());
      ASSERT_EQ(result1.getLink_T_closestPoint(), result2.getLink_T_closestPoint());

}

TEST_F(testCollisionUtils, testCapsuleDistance) {

    q = getGoodInitialPosition(robot);
    robot.updateiDyn3Model(q, false);

    std::string linkA = "LSoftHandLink";
    std::string linkB = "RSoftHandLink";

    std::list<std::pair<std::string,std::string>> whiteList;
    whiteList.push_back(std::pair<std::string,std::string>(linkA,linkB));
    compute_distance.setCollisionWhiteList(whiteList);

    std::list<LinkPairDistance> results = compute_distance.getLinkDistances();
    LinkPairDistance result = results.front();
    double actual_distance;
    actual_distance = result.getDistance();
    ASSERT_EQ(result.getLinkNames().first, linkA);
    ASSERT_EQ(result.getLinkNames().second, linkB);

    TestCapsuleLinksDistance compute_distance_observer(compute_distance);
    std::map<std::string,boost::shared_ptr<fcl::CollisionGeometry> > shapes_test;
    std::map<std::string,boost::shared_ptr<fcl::CollisionObject> > collision_objects_test;
    std::map<std::string,KDL::Frame> link_T_shape_test;

    shapes_test = compute_distance_observer.getShapes();
    collision_objects_test = compute_distance_observer.getcollision_objects();
    link_T_shape_test = compute_distance_observer.getlink_T_shape();

    boost::shared_ptr<fcl::CollisionObject> collision_geometry_l = collision_objects_test[linkA];
    boost::shared_ptr<fcl::CollisionObject> collision_geometry_r = collision_objects_test[linkB];

    int left_hand_index = robot.iDyn3_model.getLinkIndex(linkA);
    if(left_hand_index == -1)
        std::cout << "Failed to get lefthand_index" << std::endl;

    int right_hand_index = robot.iDyn3_model.getLinkIndex(linkB);
    if(right_hand_index == -1)
        std::cout << "Failed to get righthand_index" << std::endl;

    KDL::Frame w_T_link_left_hand = robot.iDyn3_model.getPositionKDL(left_hand_index);
    KDL::Frame w_T_link_right_hand = robot.iDyn3_model.getPositionKDL(right_hand_index);

    double actual_distance_check =
        (   ( w_T_link_left_hand *
              result.getLink_T_closestPoint().first ).p -
            ( w_T_link_right_hand *
              result.getLink_T_closestPoint().second ).p
        ).Norm();

    fcl::DistanceRequest distance_request;
    distance_request.gjk_solver_type = fcl::GST_INDEP;
    distance_request.enable_nearest_points = true;

    fcl::DistanceResult distance_result;

    fcl::CollisionObject* left_hand_collision_object =
        collision_geometry_l.get();
    fcl::CollisionObject* right_hand_collision_object =
        collision_geometry_r.get();

    fcl::distance(left_hand_collision_object, right_hand_collision_object,
                  distance_request,
                  distance_result);

    double actual_distance_check_original =
        (distance_result.nearest_points[0] - distance_result.nearest_points[1]).norm();

    KDL::Vector lefthand_capsule_ep1, lefthand_capsule_ep2,
                righthand_capsule_ep1, righthand_capsule_ep2;

    boost::shared_ptr<ComputeLinksDistance::Capsule> capsuleA = compute_distance_observer.getcustom_capsules()[linkA];
    boost::shared_ptr<ComputeLinksDistance::Capsule> capsuleB = compute_distance_observer.getcustom_capsules()[linkB];
    capsuleA->getEndPoints(lefthand_capsule_ep1, lefthand_capsule_ep2);
    capsuleB->getEndPoints(righthand_capsule_ep1, righthand_capsule_ep2);
    lefthand_capsule_ep1 = w_T_link_left_hand * lefthand_capsule_ep1;
    lefthand_capsule_ep2 = w_T_link_left_hand * lefthand_capsule_ep2;
    righthand_capsule_ep1 = w_T_link_right_hand * righthand_capsule_ep1;
    righthand_capsule_ep2 = w_T_link_right_hand * righthand_capsule_ep2;

    Eigen::Vector3d lefthand_capsule_ep1_eigen, lefthand_capsule_ep2_eigen,
                    righthand_capsule_ep1_eigen, righthand_capsule_ep2_eigen;

    Eigen::Vector3d lefthand_CP, righthand_CP;
    double reference_distance;

    tf::vectorKDLToEigen(lefthand_capsule_ep1, lefthand_capsule_ep1_eigen);
    tf::vectorKDLToEigen(lefthand_capsule_ep2, lefthand_capsule_ep2_eigen);
    tf::vectorKDLToEigen(righthand_capsule_ep1, righthand_capsule_ep1_eigen);
    tf::vectorKDLToEigen(righthand_capsule_ep2, righthand_capsule_ep2_eigen);

    reference_distance = dist3D_Segment_to_Segment (lefthand_capsule_ep1_eigen,
                                                    lefthand_capsule_ep2_eigen,
                                                    righthand_capsule_ep1_eigen,
                                                    righthand_capsule_ep2_eigen,
                                                    lefthand_CP,
                                                    righthand_CP);

    reference_distance = reference_distance
        - capsuleA->getRadius()
        - capsuleB->getRadius();
    double reference_distance_check = (lefthand_CP - righthand_CP).norm()
        - capsuleA->getRadius()
        - capsuleB->getRadius();


    // we compute the distance by knowing the two hands are parallel (but not the capsules!) and the capsules have the same radii
    double hand_computed_distance_estimate = (w_T_link_left_hand.p - w_T_link_right_hand.p).Norm()
        - capsuleA->getRadius()
        - capsuleB->getRadius();

    EXPECT_NEAR(actual_distance, actual_distance_check, 1E-8);
    EXPECT_NEAR(actual_distance_check, actual_distance_check_original, 1E-8);
    EXPECT_NEAR(reference_distance, reference_distance_check, 1E-8);
    EXPECT_NEAR(actual_distance, reference_distance, 1E-4) << "estimate was " << hand_computed_distance_estimate;

}

TEST_F(testCollisionUtils, testGenerateTrainingData)
{

    std::ofstream output, record_1;
    output.open("fc.train", std::ofstream::trunc);
//    record_1.open("chosen", std::ofstream::app);

    this->robot.iDyn3_model.setFloatingBaseLink(this->robot.left_leg.index);
    random_numbers::RandomNumberGenerator rng_fc;

    std::string linkA = "LSoftHandLink";
    std::string linkB = "RSoftHandLink";

    std::list<std::pair<std::string,std::string>> whiteList;
    whiteList.push_back(std::pair<std::string,std::string>(linkA,linkB));
    compute_distance.setCollisionWhiteList(whiteList);

    int counter = 0;
//    std::vector<double> identifier;
//    std::vector<double>::iterator iter;

    for (unsigned int k = 0; k < 5000; k++)
    {

        this->q = getRandomConfiguration (robot, rng_fc);
        this->robot.updateiDyn3Model(this->q, true);

        std::list<LinkPairDistance> results = compute_distance.getLinkDistances();
        LinkPairDistance result = results.front();
        double actual_distance;
        actual_distance = result.getDistance();

        //    std::cout<<"actual_distance: "<<actual_distance<<std::endl;


        if ( actual_distance < 0.8 )
        {
            output << "1" << " ";
            for(unsigned int i = 0; i < robot.left_arm.joint_numbers.size(); i++)
            {
                output << i+1 << ":" << q[robot.left_arm.joint_numbers[i]] << " ";
            }
            for(unsigned int j = 0; j < robot.right_arm.joint_numbers.size(); j++)
            {
                output << robot.left_arm.joint_numbers.size() + j + 1 << ":" << q[robot.right_arm.joint_numbers[j]] << " ";
            }
            output <<"\n";

            counter++;

//            identifier.push_back(k+1);

//            record_1 << "1" << " ";
//            for(unsigned int i = 0; i < robot.left_arm.joint_numbers.size(); i++)
//            {
//                record_1 << i+1 << ":" << q[robot.left_arm.joint_numbers[i]] << " ";
//            }
//            for(unsigned int j = 0; j < robot.right_arm.joint_numbers.size(); j++)
//            {
//                record_1 << robot.left_arm.joint_numbers.size() + j + 1 << ":" << q[robot.right_arm.joint_numbers[j]] << " ";
//            }
//            record_1 <<"\n";
        }
        else
        {
            output << "0" << " ";
            for(unsigned int i = 0; i < robot.left_arm.joint_numbers.size(); i++)
            {
                output << i+1 << ":" << q[robot.left_arm.joint_numbers[i]] << " ";
            }
            for(unsigned int j = 0; j < robot.right_arm.joint_numbers.size(); j++)
            {
                output << robot.left_arm.joint_numbers.size() + j + 1 << ":" << q[robot.right_arm.joint_numbers[j]] << " ";
            }
            output <<"\n";
        }

    }
    output.close();
//    record_1.close();

//    for (iter = identifier.begin(); iter != identifier.end(); iter++ )
//    {
//        std::cout<<*iter<<std::endl;
//    }

    std::cout<<"counter: "<<counter<<std::endl;

    ASSERT_EQ(robot.iDyn3_model.getNrOfDOFs(), 31);

}

TEST_F(testCollisionUtils, testGenerateRealTrainingData)
{

    std::ofstream output;
    output.open("fc.train", std::ofstream::trunc);
//    record_1.open("chosen", std::ofstream::app);

    this->robot.iDyn3_model.setFloatingBaseLink(this->robot.left_leg.index);
    random_numbers::RandomNumberGenerator rng_fc;

    compute_distance.setCollisionWhiteList(whiteList_R_Arm_L_Leg);

    int counter1 = 0;
    int counter2 = 0;
//      int counter = 0;
//    std::vector<double> identifier;
//    std::vector<double>::iterator iter;

//    for (unsigned int k = 0; k < 10000; k++)
    while (counter1 < 2000 || counter2 < 4000 )
//    while (counter1 < 1000)
    {

        this->q = getRandomConfiguration (robot, rng_fc);
        this->robot.updateiDyn3Model(this->q, true);

//        double tic = yarp::os::SystemClock::nowSystem();
        std::list<LinkPairDistance> results = compute_distance.getLinkDistances();
//        std::cout << "getLinkDistances() t: " << yarp::os::SystemClock::nowSystem() - tic << std::endl;


//            std::list<LinkPairDistance>::iterator iter_results;

        LinkPairDistance result = results.front();
        double actual_distance;
        actual_distance = result.getDistance();

//            std::cout<<"actual_distance: "<<actual_distance<<std::endl;
//            std::cout<<"first link: "<<result.getLinkNames().first << std::endl;
//            std::cout<<"second link: "<<result.getLinkNames().second << std::endl;
//            std::cout<<"actual_distance_end: "<<actual_distance_end<<std::endl;
//            std::cout<<"results_size: "<<results.size()<<std::endl;

//            for (iter_results = results.begin(); iter_results != results.end(); iter_results++)
//            {
//                std::cout<<(*iter_results).getLinkNames().first<<" "<<(*iter_results).getLinkNames().second<<" "<<(*iter_results).getDistance()<<std::endl;
//            }

        if ( actual_distance > 0.00 && actual_distance < 0.15 )
        {
            if (counter1 < 2000){
            output << "1" << " ";
//            output << actual_distance << " ";
            for(unsigned int i = 0; i < robot.right_arm.joint_numbers.size(); i++)
            {
                output << i+1 << ":" << q[robot.right_arm.joint_numbers[i]] << " ";
            }
            for(unsigned int k = 0; k < robot.torso.joint_numbers.size(); k++)
            {
                output << robot.right_arm.joint_numbers.size() + k + 1 << ":" << q[robot.torso.joint_numbers[k]] << " ";
            }
            for(unsigned int j = 0; j < robot.left_leg.joint_numbers.size()-1; j++)
            {
                output << robot.right_arm.joint_numbers.size() + robot.torso.joint_numbers.size() + j + 1 << ":" << q[robot.left_leg.joint_numbers[j]] << " ";
            }
            output <<"\n";

            counter1++;
            }

//            identifier.push_back(k+1);

//            record_1 << "1" << " ";
//            for(unsigned int i = 0; i < robot.left_arm.joint_numbers.size(); i++)
//            {
//                record_1 << i+1 << ":" << q[robot.left_arm.joint_numbers[i]] << " ";
//            }
//            for(unsigned int j = 0; j < robot.right_arm.joint_numbers.size(); j++)
//            {
//                record_1 << robot.left_arm.joint_numbers.size() + j + 1 << ":" << q[robot.right_arm.joint_numbers[j]] << " ";
//            }
//            record_1 <<"\n";
        }
        else if ( actual_distance > 0.20 && actual_distance < 0.50 )
        {
            if (counter2 < 4000){
            output << "0" << " ";
//            output << actual_distance << " ";
            for(unsigned int i = 0; i < robot.right_arm.joint_numbers.size(); i++)
            {
                output << i+1 << ":" << q[robot.right_arm.joint_numbers[i]] << " ";
            }
            for(unsigned int k = 0; k < robot.torso.joint_numbers.size(); k++)
            {
                output << robot.right_arm.joint_numbers.size() + k + 1 << ":" << q[robot.torso.joint_numbers[k]] << " ";
            }
            for(unsigned int j = 0; j < robot.left_leg.joint_numbers.size()-1; j++)
            {
                output << robot.right_arm.joint_numbers.size() + robot.torso.joint_numbers.size() + j + 1 << ":" << q[robot.left_leg.joint_numbers[j]] << " ";
            }
            output <<"\n";

            counter2++;
            }
        }
        else
        {}

    }
    output.close();
//    record_1.close();

//    for (iter = identifier.begin(); iter != identifier.end(); iter++ )
//    {
//        std::cout<<*iter<<std::endl;
//    }

//    std::cout<<"counter: "<<counter<<std::endl;

    std::cout<<"counter1: "<<counter1<<std::endl;
    std::cout<<"counter2: "<<counter2<<std::endl;

    ASSERT_EQ(robot.iDyn3_model.getNrOfDOFs(), 31);

}

TEST_F(testCollisionUtils, testGenerateRealTrainingData_temp)
{

    std::ofstream output;
    output.open("fc.test", std::ofstream::trunc);
//    record_1.open("chosen", std::ofstream::app);

    this->robot.iDyn3_model.setFloatingBaseLink(this->robot.left_leg.index);
    random_numbers::RandomNumberGenerator rng_fc;

    compute_distance.setCollisionWhiteList(whiteList_L_Arm_R_Leg);

    int counter1 = 0;
//    int counter2 = 0;
//      int counter = 0;
//    std::vector<double> identifier;
//    std::vector<double>::iterator iter;

//    for (unsigned int k = 0; k < 10000; k++)
//    while (counter1 < 2000 || counter2 < 4000 )
    while (counter1 < 1000)
    {

        this->q = getRandomConfiguration (robot, rng_fc);
        this->robot.updateiDyn3Model(this->q, true);

//        double tic = yarp::os::SystemClock::nowSystem();
        std::list<LinkPairDistance> results = compute_distance.getLinkDistances();
//        std::cout << "getLinkDistances() t: " << yarp::os::SystemClock::nowSystem() - tic << std::endl;


//            std::list<LinkPairDistance>::iterator iter_results;

        LinkPairDistance result = results.front();
        double actual_distance;
        actual_distance = result.getDistance();

//            std::cout<<"actual_distance: "<<actual_distance<<std::endl;
//            std::cout<<"first link: "<<result.getLinkNames().first << std::endl;
//            std::cout<<"second link: "<<result.getLinkNames().second << std::endl;
//            std::cout<<"actual_distance_end: "<<actual_distance_end<<std::endl;
//            std::cout<<"results_size: "<<results.size()<<std::endl;

//            for (iter_results = results.begin(); iter_results != results.end(); iter_results++)
//            {
//                std::cout<<(*iter_results).getLinkNames().first<<" "<<(*iter_results).getLinkNames().second<<" "<<(*iter_results).getDistance()<<std::endl;
//            }

        if ( actual_distance > 0.29 && actual_distance < 0.30 )
        {
            if (counter1 < 1000){
            output << "0" << " ";
//            output << actual_distance << " ";
            for(unsigned int i = 0; i < robot.left_arm.joint_numbers.size(); i++)
            {
                output << i+1 << ":" << q[robot.left_arm.joint_numbers[i]] << " ";
            }
            for(unsigned int k = 0; k < robot.torso.joint_numbers.size(); k++)
            {
                output << robot.left_arm.joint_numbers.size() + k + 1 << ":" << q[robot.torso.joint_numbers[k]] << " ";
            }
            for(unsigned int j = 0; j < robot.right_leg.joint_numbers.size()-1; j++)
            {
                output << robot.left_arm.joint_numbers.size() + robot.torso.joint_numbers.size() + j + 1 << ":" << q[robot.right_leg.joint_numbers[j]] << " ";
            }
            output <<"\n";

            counter1++;
            }

//            identifier.push_back(k+1);

//            record_1 << "1" << " ";
//            for(unsigned int i = 0; i < robot.left_arm.joint_numbers.size(); i++)
//            {
//                record_1 << i+1 << ":" << q[robot.left_arm.joint_numbers[i]] << " ";
//            }
//            for(unsigned int j = 0; j < robot.right_arm.joint_numbers.size(); j++)
//            {
//                record_1 << robot.left_arm.joint_numbers.size() + j + 1 << ":" << q[robot.right_arm.joint_numbers[j]] << " ";
//            }
//            record_1 <<"\n";
        }
//        else if ( actual_distance > 0.20 && actual_distance < 0.50 )
//        {
//            if (counter2 < 4000){
//            output << "0" << " ";
//            output << actual_distance << " ";
//            for(unsigned int i = 0; i < robot.left_arm.joint_numbers.size(); i++)
//            {
//                output << i+1 << ":" << q[robot.left_arm.joint_numbers[i]] << " ";
//            }
//            for(unsigned int k = 0; k < robot.torso.joint_numbers.size(); k++)
//            {
//                output << robot.left_arm.joint_numbers.size() + k + 1 << ":" << q[robot.torso.joint_numbers[k]] << " ";
//            }
//            for(unsigned int j = 0; j < robot.right_leg.joint_numbers.size()-1; j++)
//            {
//                output << robot.left_arm.joint_numbers.size() + robot.torso.joint_numbers.size() + j + 1 << ":" << q[robot.right_leg.joint_numbers[j]] << " ";
//            }
//            output <<"\n";

//            counter2++;
//            }
//        }
        else
        {}

    }
    output.close();
//    record_1.close();

//    for (iter = identifier.begin(); iter != identifier.end(); iter++ )
//    {
//        std::cout<<*iter<<std::endl;
//    }

//    std::cout<<"counter: "<<counter<<std::endl;

    std::cout<<"counter1: "<<counter1<<std::endl;
//    std::cout<<"counter2: "<<counter2<<std::endl;

    ASSERT_EQ(robot.iDyn3_model.getNrOfDOFs(), 31);

}


TEST_F(testCollisionUtils, testPredictbyModelFile)
{
    struct svm_node * x;
    struct svm_model * fc_model;
    double predict_label;
    int predict_label_int;

    double max[14], min[14], temp[14];

    x = (struct svm_node *) malloc(20*sizeof(struct svm_node));

    std::ifstream input, range;
    std::ofstream output;
    input.open("original.data");
    range.open("range1519");
    output.open("fc.predict", std::ofstream::trunc);
    fc_model = svm_load_model("fc.train.scale.model.1519");

    for (unsigned int j = 0; j<14; j++)
    {
        range>>min[j];
        range>>max[j];
        temp[j] = 2 / (max[j]-min[j]);
    }

    double input_get;

    for (unsigned int k = 0; k < 1000; k++)
    {

        for(unsigned int i = 0; i < 14; i++)
        {
            x[i].index = i+1;
            input>>input_get;
            x[i].value = ( input_get - min[i] ) * temp[i] - 1;
        }
        x[14].index = -1;


        double tic = yarp::os::SystemClock::nowSystem();
        predict_label = svm_predict(fc_model,x);
        std::cout << "svm_predict() t: " << yarp::os::SystemClock::nowSystem() - tic << std::endl;


        predict_label_int = (int)predict_label;


        output << predict_label_int<< " ";
        for(unsigned int i = 0; i < 14; i++)
        {
           output << i+1 << ":" << x[i].value << " ";
        }
        output <<"\n";


    }

    free(x);

    input.close();
    range.close();
    output.close();

    ASSERT_EQ(robot.iDyn3_model.getNrOfDOFs(), 31);

}

TEST_F(testCollisionUtils, testClassifyOriginalData)
{

    std::ifstream input;
    std::ofstream output;
    input.open("original.data");
    output.open("fc.test", std::ofstream::trunc);

    this->robot.iDyn3_model.setFloatingBaseLink(this->robot.left_leg.index);
    random_numbers::RandomNumberGenerator rng_fc;

    compute_distance.setCollisionWhiteList(whiteList_L_R_Arms);

    for (unsigned int k = 0; k < 1000; k++)
    {

        this->q = getRandomConfiguration (robot, rng_fc);

        for(unsigned int i = 0; i < robot.left_arm.joint_numbers.size(); i++)
        {
            input>>q[robot.left_arm.joint_numbers[i]];
        }
        for(unsigned int j = 0; j < robot.right_arm.joint_numbers.size(); j++)
        {
            input>>q[robot.right_arm.joint_numbers[j]];
        }

        this->robot.updateiDyn3Model(this->q, true);

        std::list<LinkPairDistance> results = compute_distance.getLinkDistances();

        LinkPairDistance result = results.front();
        double actual_distance;
        actual_distance = result.getDistance();

        if ( actual_distance < 0.2 )
        {
//            output << actual_distance << " ";
            output << "1" << " ";
            for(unsigned int i = 0; i < robot.left_arm.joint_numbers.size(); i++)
            {
                output << i+1 << ":" << q[robot.left_arm.joint_numbers[i]] << " ";
            }
            for(unsigned int j = 0; j < robot.right_arm.joint_numbers.size(); j++)
            {
                output << robot.left_arm.joint_numbers.size() + j + 1 << ":" << q[robot.right_arm.joint_numbers[j]] << " ";
            }
            output <<"\n";
        }
        else
        {
//            output << actual_distance << " ";
            output << "0" << " ";
            for(unsigned int i = 0; i < robot.left_arm.joint_numbers.size(); i++)
            {
                output << i+1 << ":" << q[robot.left_arm.joint_numbers[i]] << " ";
            }
            for(unsigned int j = 0; j < robot.right_arm.joint_numbers.size(); j++)
            {
                output << robot.left_arm.joint_numbers.size() + j + 1 << ":" << q[robot.right_arm.joint_numbers[j]] << " ";
            }
            output <<"\n";
        }

    }
    input.close();
    output.close();

    ASSERT_EQ(robot.iDyn3_model.getNrOfDOFs(), 31);

}


TEST_F(testCollisionUtils, checkTimings)
{
    q = getGoodInitialPosition(robot);
    robot.updateiDyn3Model(q, false);

    std::string linkA = "LSoftHandLink";
    std::string linkB = "RSoftHandLink";

    TestCapsuleLinksDistance compute_distance_observer(compute_distance);

    std::map<std::string,boost::shared_ptr<fcl::CollisionGeometry> > shapes_test;
    std::map<std::string,boost::shared_ptr<fcl::CollisionObject> > collision_objects_test;
    std::map<std::string,KDL::Frame> link_T_shape_test;

    shapes_test = compute_distance_observer.getShapes();
    collision_objects_test = compute_distance_observer.getcollision_objects();
    link_T_shape_test = compute_distance_observer.getlink_T_shape();

    boost::shared_ptr<fcl::CollisionObject> collision_geometry_l = collision_objects_test[linkA];
    boost::shared_ptr<fcl::CollisionObject> collision_geometry_r = collision_objects_test[linkB];

    int left_hand_index = robot.iDyn3_model.getLinkIndex(linkA);
    if(left_hand_index == -1)
        std::cout << "Failed to get lefthand_index" << std::endl;

    int right_hand_index = robot.iDyn3_model.getLinkIndex(linkB);
    if(right_hand_index == -1)
        std::cout << "Failed to get righthand_index" << std::endl;

    double tic = yarp::os::SystemClock::nowSystem();
    compute_distance_observer.updateCollisionObjects();
    std::cout << "updateCollisionObjects t: " << yarp::os::SystemClock::nowSystem() - tic << std::endl;

    tic = yarp::os::SystemClock::nowSystem();
    compute_distance.getLinkDistances();
    std::cout << "getLinkDistances() t: " << yarp::os::SystemClock::nowSystem() - tic << std::endl;

    tic = yarp::os::SystemClock::nowSystem();
    compute_distance.getLinkDistances(0.05);
    std::cout << "getLinkDistances(0.05) t: " << yarp::os::SystemClock::nowSystem() - tic << std::endl;

    {
        tic = yarp::os::SystemClock::nowSystem();
        fcl::DistanceRequest distance_request;
        distance_request.gjk_solver_type = fcl::GST_INDEP;
        distance_request.enable_nearest_points = true;

        fcl::DistanceResult distance_result;

        fcl::CollisionObject* left_hand_collision_object =
            collision_geometry_l.get();
        fcl::CollisionObject* right_hand_collision_object =
            collision_geometry_r.get();

        fcl::distance(left_hand_collision_object, right_hand_collision_object,
                      distance_request,
                      distance_result);
        std::cout << "fcl capsule-capsule t: " << yarp::os::SystemClock::nowSystem() - tic << std::endl;
    }

    {
        tic = yarp::os::SystemClock::nowSystem();
        fcl::DistanceRequest distance_request;
        distance_request.gjk_solver_type = fcl::GST_INDEP;
        distance_request.enable_nearest_points = false;

        fcl::DistanceResult distance_result;

        fcl::CollisionObject* left_hand_collision_object =
            collision_geometry_l.get();
        fcl::CollisionObject* right_hand_collision_object =
            collision_geometry_r.get();

        fcl::distance(left_hand_collision_object, right_hand_collision_object,
                      distance_request,
                      distance_result);
        std::cout << "fcl capsule-capsule without closest-point query t: " << yarp::os::SystemClock::nowSystem() - tic << std::endl;
    }

    tic = yarp::os::SystemClock::nowSystem();
    KDL::Vector lefthand_capsule_ep1, lefthand_capsule_ep2,
                righthand_capsule_ep1, righthand_capsule_ep2;

    boost::shared_ptr<ComputeLinksDistance::Capsule> capsuleA = compute_distance_observer.getcustom_capsules()[linkA];
    boost::shared_ptr<ComputeLinksDistance::Capsule> capsuleB = compute_distance_observer.getcustom_capsules()[linkB];

    capsuleA->getEndPoints(lefthand_capsule_ep1, lefthand_capsule_ep2);
    capsuleB->getEndPoints(righthand_capsule_ep1, righthand_capsule_ep2);

    Eigen::Vector3d lefthand_capsule_ep1_eigen, lefthand_capsule_ep2_eigen,
                    righthand_capsule_ep1_eigen, righthand_capsule_ep2_eigen;

    Eigen::Vector3d lefthand_CP, righthand_CP;
    double reference_distance;

    tf::vectorKDLToEigen(lefthand_capsule_ep1, lefthand_capsule_ep1_eigen);
    tf::vectorKDLToEigen(lefthand_capsule_ep2, lefthand_capsule_ep2_eigen);
    tf::vectorKDLToEigen(righthand_capsule_ep1, righthand_capsule_ep1_eigen);
    tf::vectorKDLToEigen(righthand_capsule_ep2, righthand_capsule_ep2_eigen);

    reference_distance = dist3D_Segment_to_Segment (lefthand_capsule_ep1_eigen,
                                                    lefthand_capsule_ep2_eigen,
                                                    righthand_capsule_ep1_eigen,
                                                    righthand_capsule_ep2_eigen,
                                                    lefthand_CP,
                                                    righthand_CP);
    std::cout << "inline capsule-capsule t: " << yarp::os::SystemClock::nowSystem() - tic << std::endl;
}

TEST_F(testCollisionUtils, testGlobalToLinkCoordinates)
{
    q = getGoodInitialPosition(robot);
    robot.updateiDyn3Model(q, false);
    std::string linkA = "LSoftHandLink";
    std::string linkB = "RSoftHandLink";

    TestCapsuleLinksDistance compute_distance_observer(compute_distance);
    std::map<std::string,boost::shared_ptr<fcl::CollisionObject> > collision_objects_test =
        compute_distance_observer.getcollision_objects();
    boost::shared_ptr<fcl::CollisionObject> collision_geometry_l = collision_objects_test[linkA];
    boost::shared_ptr<fcl::CollisionObject> collision_geometry_r = collision_objects_test[linkB];

    fcl::DistanceRequest distance_request;
    distance_request.gjk_solver_type = fcl::GST_INDEP;
    distance_request.enable_nearest_points = true;

    fcl::DistanceResult distance_result;

    fcl::CollisionObject* left_hand_collision_object =
        collision_geometry_l.get();
    fcl::CollisionObject* right_hand_collision_object =
        collision_geometry_r.get();

    fcl::distance(left_hand_collision_object, right_hand_collision_object,
                  distance_request,
                  distance_result);

    KDL::Frame lA_T_pA_KDL;
    KDL::Frame lA_T_pA;
    compute_distance_observer.globalToLinkCoordinatesKDL(linkA,distance_result.nearest_points[0],lA_T_pA_KDL);
    compute_distance_observer.globalToLinkCoordinates(linkA,distance_result.nearest_points[0],lA_T_pA);

    EXPECT_EQ(lA_T_pA_KDL, lA_T_pA);
}

}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

