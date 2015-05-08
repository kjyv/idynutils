#include <boost/filesystem.hpp>
#include <idynutils/collision_utils.h>
#include <kdl_parser/kdl_parser.hpp>
#include <fcl/BV/OBBRSS.h>
#include <fcl/BVH/BVH_model.h>
#include <fcl/distance.h>
#include <fcl/shape/geometric_shapes.h>
#include <geometric_shapes/shapes.h>
#include <geometric_shapes/shape_operations.h>


// construct vector
KDL::Vector toKdl(urdf::Vector3 v)
{
  return KDL::Vector(v.x, v.y, v.z);
}

// construct rotation
KDL::Rotation toKdl(urdf::Rotation r)
{
  return KDL::Rotation::Quaternion(r.x, r.y, r.z, r.w);
}

// construct pose
KDL::Frame toKdl(urdf::Pose p)
{
  return KDL::Frame(toKdl(p.rotation), toKdl(p.position));
}

bool ComputeLinksDistance::parseCollisionObjects(const std::string &robot_urdf_path)
{
    urdf::Model robot_model;
    robot_model.initFile(robot_urdf_path);

    std::vector<boost::shared_ptr<urdf::Link> > links;
    robot_model.getLinks(links);
    typedef std::vector<boost::shared_ptr<urdf::Link> >::iterator it_type;

    for (it_type iterator = links.begin();
            iterator != links.end(); iterator++) {

        boost::shared_ptr<urdf::Link> link = *iterator;

        if (link->collision) {
            if (link->collision->geometry->type == urdf::Geometry::CYLINDER ||
                link->collision->geometry->type == urdf::Geometry::SPHERE   ||
                link->collision->geometry->type == urdf::Geometry::BOX      ||
                link->collision->geometry->type == urdf::Geometry::MESH) {

                boost::shared_ptr<fcl::CollisionGeometry> shape;

                if (link->collision->geometry->type == urdf::Geometry::CYLINDER) {
                    std::cout << "adding capsule for " << link->name;

                    boost::shared_ptr<urdf::Cylinder> collisionGeometry =
                            boost::dynamic_pointer_cast<urdf::Cylinder>(
                                    link->collision->geometry);

                    shape.reset(new fcl::Capsule(collisionGeometry->radius,
                                                 collisionGeometry->length));
                } else if (link->collision->geometry->type == urdf::Geometry::SPHERE) {
                    std::cout << "adding sphere for " << link->name;

                    boost::shared_ptr<urdf::Sphere> collisionGeometry =
                            boost::dynamic_pointer_cast<urdf::Sphere>(
                                    link->collision->geometry);

                    shape.reset(new fcl::Sphere(collisionGeometry->radius));
                } else if (link->collision->geometry->type == urdf::Geometry::BOX) {
                    std::cout << "adding box for " << link->name;

                    boost::shared_ptr<urdf::Box> collisionGeometry =
                            boost::dynamic_pointer_cast<urdf::Box>(
                                    link->collision->geometry);

                    shape.reset(new fcl::Box(collisionGeometry->dim.x,
                                             collisionGeometry->dim.y,
                                             collisionGeometry->dim.z));
                }
                else if(link->collision->geometry->type == urdf::Geometry::MESH){
                    std::cout << "adding mesh for " << link->name;

                    boost::shared_ptr< ::urdf::Mesh> collisionGeometry = boost::dynamic_pointer_cast< ::urdf::Mesh> (link->collision->geometry);

                    shapes::Mesh *mesh = shapes::createMeshFromResource(collisionGeometry->filename);

                    std::vector<fcl::Vec3f> vertices;
                    std::vector<fcl::Triangle> triangles;

                    for(unsigned int i=0; i < mesh->vertex_count; ++i){
                        fcl::Vec3f v(mesh->vertices[3*i]*collisionGeometry->scale.x,
                                     mesh->vertices[3*i + 1]*collisionGeometry->scale.y,
                                     mesh->vertices[3*i + 2]*collisionGeometry->scale.z);

                        vertices.push_back(v);
                    }

                    for(unsigned int i=0; i< mesh->triangle_count; ++i){
                        fcl::Triangle t(mesh->triangles[3*i],
                                        mesh->triangles[3*i + 1],
                                        mesh->triangles[3*i + 2]);
                        triangles.push_back(t);
                    }

                    // add the mesh data into the BVHModel structure
                    shape.reset(new fcl::BVHModel<fcl::OBBRSS>);
                    fcl::BVHModel<fcl::OBBRSS>* bvhModel = (fcl::BVHModel<fcl::OBBRSS>*)shape.get();
                    bvhModel->beginModel();
                    bvhModel->addSubModel(vertices, triangles);
                    bvhModel->endModel();
                }

                boost::shared_ptr<fcl::CollisionObject> collision_object(
                        new fcl::CollisionObject(shape));

                collision_objects_[link->name] = collision_object;
                shapes_[link->name] = shape;

                /* Store the transformation of the CollisionShape from URDF
                 * that is, we store link_T_shape for the actual link */
                link_T_shape[link->name] = toKdl(link->collision->origin);
            } else {
                std::cout << "Collision type unknown for link " << link->name;
            }
        } else {
            std::cout << "Collision not defined for link " << link->name;
        }
    }
    return true;
}

bool ComputeLinksDistance::updateCollisionObjects()
{
    typedef std::map<std::string,boost::shared_ptr<fcl::CollisionObject> >::iterator it_type;
    for(it_type it = collision_objects_.begin();
        it != collision_objects_.end(); ++it)
    {
        std::string link_name = it->first;
        KDL::Frame w_T_link, w_T_shape;
        w_T_link = model.iDyn3_model.getPositionKDL(model.iDyn3_model.getLinkIndex(link_name));
        w_T_shape = w_T_link * link_T_shape[link_name];

        fcl::Transform3f fcl_w_T_shape = KDL2fcl(w_T_shape);
        fcl::CollisionObject* collObj_shape = collision_objects_[link_name].get();
        collObj_shape->setTransform(fcl_w_T_shape);
    }
    return true;
}


fcl::Transform3f ComputeLinksDistance::KDL2fcl(const KDL::Frame &in){
    fcl::Transform3f out;
    double x,y,z,w;
    in.M.GetQuaternion(x, y, z, w);
    fcl::Vec3f t(in.p[0], in.p[1], in.p[2]);
    fcl::Quaternion3f q(w, x, y, z);
    out.setQuatRotation(q);
    out.setTranslation(t);
    return out;
}

KDL::Frame ComputeLinksDistance::fcl2KDL(const fcl::Transform3f &in)
{
    fcl::Quaternion3f q = in.getQuatRotation();
    fcl::Vec3f t = in.getTranslation();

    KDL::Frame f;
    f.p = KDL::Vector(t[0],t[1],t[2]);
    f.M = KDL::Rotation::Quaternion(q.getX(), q.getY(), q.getZ(), q.getW());

    return f;
}

ComputeLinksDistance::ComputeLinksDistance(iDynUtils &model) : model(model)
{
    boost::filesystem::path original_model(model.getRobotURDFPath());
    std::string capsule_model_urdf_filename = std::string(original_model.stem().c_str()) + std::string("_capsules.urdf");
    boost::filesystem::path capsule_model(original_model.parent_path() /
                                          capsule_model_urdf_filename);
    if(boost::filesystem::exists(capsule_model))
        this->parseCollisionObjects(capsule_model.c_str());
    else
        this->parseCollisionObjects(original_model.c_str());

    this->setCollisionBlackList(std::list<LinkPairDistance::LinksPair>());
}

std::list<LinkPairDistance> ComputeLinksDistance::getLinkDistances(double detectionThreshold)
{
    std::list<LinkPairDistance> results;

    updateCollisionObjects();

    std::vector<std::string> collisionEntries;
    allowed_collision_matrix->getAllEntryNames(collisionEntries);
    typedef std::vector<std::string>::iterator iter_link;
    typedef std::list<std::pair<std::string,std::string> >::iterator iter_pair;

    std::list<std::pair<std::string,std::string> > pairsToCheck;
    for(iter_link it_A = collisionEntries.begin();
        it_A != collisionEntries.end();
        ++it_A)
    {
        for(iter_link it_B = collisionEntries.begin();
            it_B != collisionEntries.end();
            ++it_B)
        {
            if(it_B <= it_A)
                continue;
            else
                pairsToCheck.push_back(std::pair<std::string,std::string>(*it_A,*it_B));
        }
    }

    for(iter_pair it = pairsToCheck.begin();
        it != pairsToCheck.end();
        ++it)
    {
        collision_detection::AllowedCollision::Type collisionType;
        if(allowed_collision_matrix->getAllowedCollision(it->first,it->second,collisionType) &&
           collisionType == collision_detection::AllowedCollision::NEVER)
        {
            std::string linkA = it->first;
            std::string linkB = it->second;

            fcl::CollisionObject* collObj_shapeA = collision_objects_[linkA].get();
            fcl::CollisionObject* collObj_shapeB = collision_objects_[linkB].get();

            fcl::DistanceRequest request;
            request.gjk_solver_type = fcl::GST_INDEP;
            request.enable_nearest_points = true;

            // result will be returned via the collision result structure
            fcl::DistanceResult result;

            // perform distance test
            fcl::distance(collObj_shapeA, collObj_shapeB, request, result);

            // p1Homo, p2Homo newly computed points by FCL
            // absolutely computed w.r.t. base-frame
            fcl::Transform3f w_pAHomo(result.nearest_points[0]);
            fcl::Transform3f w_pBHomo(result.nearest_points[1]);
            fcl::Transform3f fcl_w_T_shapeA, fcl_w_T_shapeB;

            fcl_w_T_shapeA = collObj_shapeA->getTransform();
            fcl_w_T_shapeB = collObj_shapeB->getTransform();

            fcl::Transform3f fcl_shapeA_pA, fcl_shapeB_pB;

            fcl_shapeA_pA = fcl_w_T_shapeA.inverseTimes(w_pAHomo);
            fcl_shapeB_pB = fcl_w_T_shapeB.inverseTimes(w_pBHomo);

            KDL::Frame shapeA_pA, shapeB_pB;

            shapeA_pA = fcl2KDL(fcl_shapeA_pA);
            shapeB_pB = fcl2KDL(fcl_shapeB_pB);

            KDL::Frame linkA_pA, linkB_pB;
            linkA_pA = link_T_shape[linkA] * shapeA_pA;
            linkB_pB = link_T_shape[linkB] * shapeB_pB;

            if(result.min_distance < detectionThreshold)
                results.push_back(LinkPairDistance(linkA, linkB,
                                                   linkA_pA, linkB_pB,
                                                   result.min_distance));
        }
    }

    results.sort();

    return results;
}

bool ComputeLinksDistance::setCollisionWhiteList(std::list<LinkPairDistance::LinksPair> whiteList)
{
    allowed_collision_matrix.reset(
        new collision_detection::AllowedCollisionMatrix(
            model.moveit_robot_model->getLinkModelNames(), true));

    typedef std::list<LinkPairDistance::LinksPair>::iterator iter_pairs;
    for(iter_pairs it = whiteList.begin(); it != whiteList.end(); ++it)
    {
        if( collision_objects_.count(it->first) > 0 &&
            collision_objects_.count(it->second) > 0)
        allowed_collision_matrix->setEntry(it->first, it->second, false);
    }

    model.loadDisabledCollisionsFromSRDF(allowed_collision_matrix);
}

bool ComputeLinksDistance::setCollisionBlackList(std::list<LinkPairDistance::LinksPair> blackList)
{
    allowed_collision_matrix.reset(
        new collision_detection::AllowedCollisionMatrix(
            model.moveit_robot_model->getLinkModelNames(), true));

    std::vector<std::string> linksWithCollisionObjects;

    typedef std::map<std::string,boost::shared_ptr<fcl::CollisionObject> >::iterator iter_collision;
    for(iter_collision it = collision_objects_.begin(); it != collision_objects_.end(); ++it)
        linksWithCollisionObjects.push_back(it->first);

    allowed_collision_matrix->setEntry(linksWithCollisionObjects, linksWithCollisionObjects, false);

    typedef std::list<LinkPairDistance::LinksPair>::iterator iter_pairs;
    for(iter_pairs it = blackList.begin(); it != blackList.end(); ++it)
        allowed_collision_matrix->setEntry(it->first, it->second, true);

    model.loadDisabledCollisionsFromSRDF(allowed_collision_matrix);
}


LinkPairDistance::LinkPairDistance(const std::string &link1, const std::string &link2,
                                   const KDL::Frame &link1_T_closestPoint1,
                                   const KDL::Frame &link2_T_closestPoint2,
                                   const double &distance) :
    linksPair(link1 < link2 ? link1:link2,
             link1 < link2 ? link2:link1),
    link_T_closestPoint(link1 < link2 ? link1_T_closestPoint1:link2_T_closestPoint2,
                        link1 < link2 ? link2_T_closestPoint2 :link1_T_closestPoint1),
    distance(distance)
{

}

const double &LinkPairDistance::getDistance() const
{
    return distance;
}

const std::pair<KDL::Frame, KDL::Frame> &LinkPairDistance::getTransforms() const
{
    return link_T_closestPoint;
}

const std::pair<std::string, std::string> &LinkPairDistance::getLinkNames() const
{
    return linksPair;
}

bool LinkPairDistance::operator <(const LinkPairDistance &second) const
{
    if(this->distance < second.distance) return true;
    else return (this->linksPair.first < second.linksPair.first);
}