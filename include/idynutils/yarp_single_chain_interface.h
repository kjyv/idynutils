/*
 * Copyright (C) 2014 Walkman
 * Author: Mirko Ferrati, Enrico Mingo, Alessio Rocchi
 * email:  mirko.ferrati@gmail.com, enrico.mingo@iit.it, alessio.rocchi@iit.it
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>
*/

#ifndef YARP_SINGLE_CHAIN_INTERFACE_H
#define YARP_SINGLE_CHAIN_INTERFACE_H
#include <yarp/os/all.h>
#include <yarp/dev/all.h>
#include <vector>
#include <iostream>
#include <mutex>
#include <vector>
#include <math.h>
#include <yarp/os/RateThread.h>
#include <yarp/os/BufferedPort.h>
#include <yarp/dev/IInteractionMode.h>
#include <idynutils/ControlType.hpp>

#define WALKMAN_CM_NONE VOCAB3('d','i','o')
#define WALKMAN_CM_TORQUE VOCAB4('c','a','n','e')
#define WALKMAN_CM_POSITION_DIRECT VOCAB4('l','u','p','o')
#define WALKMAN_CM_POSITION VOCAB4('s','u','c','a')
#define WALKMAN_CM_IMPEDANCE_POS VOCAB4('g','e','s','u')
#define WALKMAN_CM_IDLE VOCAB4('b','o','i','a')


/**
 * These strings are supposed to be found into the SRDF of any robot we are working with
 */
namespace walkman{
    namespace robot{
        static const std::string left_hand = "left_hand";
        static const std::string right_hand = "right_hand";
        static const std::string left_arm = "left_arm";
        static const std::string right_arm = "right_arm";
        static const std::string left_leg = "left_leg";
        static const std::string right_leg = "right_leg";
        static const std::string torso = "torso";
        static const std::string chains = "chains";
        static const std::string sensors = "sensors";
        static const std::string base = "base";
    }
}

namespace walkman{

/**
 * @brief The yarp_single_chain_interface class
 */
class yarp_single_chain_interface
{
public:
    /**
     * @brief ControlType is a pair representing the new control schemes in YARP.
     * Each joint control type is identified by a pair <Control Mode, Interaction Mode>
     * which can be of the type < Position | Velocity | Torque | Idle, Compliant | Stiff >
     * The getControlMethod(), setPositionDirectMode(), setPositionMode(), setImpedanceMode(),
     * setIdleMode(), setTorqueMode(), and the relative isInPositionDirectMode(), isInPositionMode(),
     * isInImpedanceMode(), isInIdleMode(), isInTorqueMode() are simple wrappers that check and set
     * control types according to the old semantic, where only the five modes are available:
     * - position
     * - position direct (no trajectory interpolation between position references)
     * - torque
     * - impedance control
     * - idle
     */
    typedef std::pair<int, yarp::dev::InteractionModeEnum> ControlType;
    typedef std::vector<ControlType> ControlTypes;

    /**
     * @brief yarp_single_chain_interface is a simple interface for control of kinematic chains
     * @param kinematic_chain the name of the kinematic chain as defined in the robot srdf
     * @param robot_name the name of the robot, will be used to open polydrivers
     * @param module_prefix_with_no_slash the module name
     * @param useSI does the sense() and move() use SI units? defaults to false
     * @param controlModeVocab is the controlMode used to initialize the interface, default is IDLE
     */
    yarp_single_chain_interface(std::string kinematic_chain,
                                std::string module_prefix_with_no_slash,
                                std::string robot_name,
                                bool useSI = false,
                                const int controlModeVocab = WALKMAN_CM_IDLE
                                );

    /**
     * @brief sense returns joint positions
     * @return a \f$R^{n_\text{chain\_joints}}\f$ vector which is in
     * \f$[rad]\f$ if useSI is true
     * \f$[deg]\f$ is useSI is false
     */
    virtual yarp::sig::Vector sense();

    /**
     * @brief sense returns joint positions
     * @param q_sensed \f$R^{n_\text{chain\_joints}}\f$ vector which is in
     * \f$[rad]\f$ if useSI is true
     * \f$[deg]\f$ is useSI is false
     */
    virtual void sense(yarp::sig::Vector& q_sensed);

    /**
     * @brief sensePosition returns joint positions
     * @return a \f$R^{n_\text{chain\_joints}}\f$ vector which is in
     * \f$[rad]\f$ if useSI is true
     * \f$[deg]\f$ is useSI is false
     */
    yarp::sig::Vector sensePosition();

    /**
     * @brief sensePosition returns joint positions
     * @param q_sensed \f$R^{n_\text{chain\_joints}}\f$ vector which is in
     * \f$[rad]\f$ if useSI is true
     * \f$[deg]\f$ is useSI is false
     */
    void sensePosition(yarp::sig::Vector &q_sensed);

    /**
     * @brief senseVelocity returns joint velocities
     * @return a \f$R^{n_\text{chain\_joints}}\f$ vector which is in
     * \f$[\frac{rad}{s}]\f$ if useSI is true
     * \f$[\frac{deg}{s}]\f$ is useSI is false
     */
    yarp::sig::Vector senseVelocity();

    /**
     * @brief senseVelocity returns joint velocities
     * @param velocity_sensed \f$R^{n_\text{chain\_joints}}\f$ vector which is in
     * \f$[\frac{rad}{s}]\f$ if useSI is true
     * \f$[\frac{deg}{s}]\f$ is useSI is false
     */
    void senseVelocity(yarp::sig::Vector &velocity_sensed);

    /**
     * @brief senseTorque returns joint torques
     * @return a \f$R^{n_\text{chain\_joints}}\f$ vector which is in
     * \f$[\frac{rad}{s}]\f$ if useSI is true
     * \f$[\frac{deg}{s}]\f$ is useSI is false
     */
    yarp::sig::Vector senseTorque();

    /**
     * @brief senseTorque returns joint torques
     * @param tau_sensed \f$R^{n_\text{chain\_joints}}\f$ vector which is in
     * \f$[\frac{N}]\f$
     */
    void senseTorque(yarp::sig::Vector &tau_sensed);

    /**
     * @brief move moves all joints of the chain
     * @param u_d is a \f$R^{n_\text{chain\_joints}}\f$ which is in
     * \f$[rad]\f$ if useSI is true and control mode is position or position direct
     * \f$[deg]\f$ is useSI is false and control mode is position or position direct
     * \f$[N]\f$ is control mode is torque
     */
    virtual void move(const yarp::sig::Vector& u_d);

    /**
     * @brief setReferenceSpeed set a desired reference speed vector for all joints in the chain
     * when moving using position mode
     * @param maximum_velocity a reference velocity vector \f$R^{n_\text{chain\_joints}}\f$ which is
     * in \f$\frac{rad}{s}\f$ is useSI is true
     * in \f$\frac{deg}{s}\f$ is useSI is true
     * @return true if the chain is in position mode and
     * we are able to set reference velocity for all joints in the chain
     */
    bool setReferenceSpeeds(const yarp::sig::Vector& maximum_velocity);

    /**
     * @brief setReferenceSpeed set a desired reference speed for all joints in the chain
     * when moving using position mode
     * @param maximum_velocity a reference velocity which is
     * in \f$\frac{rad}{s}\f$ is useSI is true
     * in \f$\frac{deg}{s}\f$ is useSI is true
     * @return true if the chain is in position mode and
     * we are able to set reference velocity for all joints in the chain
     */
    bool setReferenceSpeed(const double& maximum_velocity);

    /**
     * @brief setImpedance sets joint impedance for all joints in the chain
     * @param Kq a \f$R^{n_\text{chain\_joints}}\f$ desired joint stiffness vector
     * in \f$\frac{Nm}{\text{rad}}\f$ if useSI is set to true,
     * in \f$\frac{Nm}{\text{deg}}\f$ othwerwise
     * @param Dq a \f$R^{n_\text{chain\_joints}}\f$ desired joint damping vector
     * in \f$\frac{Nm}{\text{rad}}\f$ if useSI is set to true,
     * in \f$\frac{Nm}{\text{deg}}\f$ othwerwise
     * @return true if the chain is in impedance mode and we are able
     * to set impedance for all joins
     */
    bool setImpedance(const yarp::sig::Vector& Kq,
                      const yarp::sig::Vector& Dq);

    /**
     * @brief getImpedance gets joint impedance for all joints in the chain
     * @param Kq a \f$R^{n_\text{chain\_joints}}\f$ actual joint stiffness vector
     * in \f$\frac{Nm}{\text{rad}}\f$ if useSI is set to true,
     * in \f$\frac{Nm}{\text{deg}}\f$ othwerwise
     * @param Dq a \f$R^{n_\text{chain\_joints}}\f$ actual joint damping vector
     * in \f$\frac{Nm}{\text{rad}}\f$ if useSI is set to true,
     * in \f$\frac{Nm}{\text{deg}}\f$ othwerwise
     * @return true if the chain is in impedance mode and we are able
     * to get impedance for all joins
     */
    bool getImpedance(yarp::sig::Vector& Kq,
                      yarp::sig::Vector& Dq);

    /**
     * @brief getControlTypes returns the pair <control mode, interaction mode> for each joint in the chain
     * @param controlTypes a vector of pairs <integer, InteractionModeEnum> representing control type for each joint
     * @return true if able to succesfully read control mode and interaction mode for each joint
     */
    bool getControlTypes(ControlTypes& controlTypes);

    /**
     * @brief setControlTypes sets the pair <control mode, interaction mode> for each joint in the chain
     * @param controlTypes a vector of pairs <integer, InteractionModeEnum> representing control type for each joint
     * @return true if able to successfully write control mode and interaction mode for each joint
     */
    bool setControlTypes(const ControlTypes& controlTypes);

    ControlTypes controlTypesFromVectors(const std::vector<int>& controlModes,
                                         const std::vector<yarp::dev::InteractionModeEnum>& interactionModes);

    void vectorsFromControlTypes(const ControlTypes& controlTypes,
                                 std::vector<int>& controlModes,
                                 std::vector<yarp::dev::InteractionModeEnum>& interactionModes);


    const int& getNumberOfJoints() const;

    const std::string &getChainName() const;

    /**
     * @brief setControlType sets the desired control type, if possible
     * @param controlType the desired control type for this kinematic chain
     * @return true on success
     */
    bool setControlType(const walkman::ControlType& controlType);

    /**
     * @brief getControlType returns the current control type for this kinematic chain
     * @param controlType the control type actually set for this kinematic chain
     * @return true if able to query a control type on the kinemait chain
     */
    bool getControlType(walkman::ControlType& controlType);

    bool setPositionMode();

    bool isInPositionMode() const;

    bool setPositionDirectMode();

    bool  isInPositionDirectMode() const;

    bool setTorqueMode();

    bool isInTorqueMode() const;

    bool setIdleMode();

    bool isInIdleMode() const;

    bool setImpedanceMode();

    bool isInImpedanceMode() const;

    const int getControlMode() const;

    bool useSI() const;

    ~yarp_single_chain_interface();

    const bool& isAvailable;

protected:

    /**
     * @brief getControlModes returns the control mode for each joint in the chain
     * @param controlModes a vector of integers representing control modes for each joint
     * @return true if able to succesfully read control mode for each joint
     */
    bool getControlModes(std::vector<int>& controlModes);

    /**
     * @brief getControlModes returns the control mode for each joint in the chain
     * @return a vector of integers representing control modes for each joint
     */
    std::vector<int> getControlModes();

    /**
     * @brief getInteractionModes returns the interaction mode for each joint in the chain
     * @param interactionModes a vector of InteractionModeEnum representing interaction mode for each joint
     * @return true if able to sucesfully read interaction mode for each joint
     */
    bool getInteractionModes(std::vector<yarp::dev::InteractionModeEnum>& interactionModes);

    /**
     * @brief getInteractionModes returns the interactino mode for each joint in the chain
     * @return a vector of InteractionModeEnum representing interaction mode for each joint
     */
    std::vector<yarp::dev::InteractionModeEnum> getInteractionModes();

private:

    bool createPolyDriver ( const std::string &kinematic_chain, const std::string &robot_name, yarp::dev::PolyDriver &polyDriver );
    std::string kinematic_chain;
    int joints_number;
    std::string module_prefix;
    yarp::sig::Vector q_buffer;
    yarp::sig::Vector qdot_buffer;
    yarp::sig::Vector tau_buffer;
    bool internal_isAvailable;
    yarp::dev::PolyDriver polyDriver;
    bool _useSI;
    ControlType _controlType;
    std::string _robot_name;

    void convertEncoderToSI(yarp::sig::Vector& vector);
    void convertImpedanceFromSI(yarp::sig::Vector &vector);
    double convertImpedanceFromSI(const double& in) const;
    void convertMotorCommandFromSI(yarp::sig::Vector& vector);
    yarp::sig::Vector convertMotorCommandFromSI(const yarp::sig::Vector& vector);
    double convertMotorCommandFromSI(const double& in) const;


    int computeControlMode();

    yarp::dev::IEncodersTimed *encodersMotor;
    yarp::dev::IControlMode2 *controlMode;
    yarp::dev::IInteractionMode *interactionMode;
    yarp::dev::IPositionControl2 *positionControl;
    yarp::dev::IPositionDirect *positionDirect;
    yarp::dev::IImpedanceControl *impedancePositionControl;
    yarp::dev::ITorqueControl *torqueControl;

};


}


#endif // YARP_SINGLE_CHAIN_INTERFACE_H
