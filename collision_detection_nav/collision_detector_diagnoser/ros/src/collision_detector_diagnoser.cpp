#include "collision_detector_diagnoser/collision_detector_diagnoser.h"
#include <pluginlib/class_list_macros.h>
#include <iostream>

// register this class as a Fault Detector
PLUGINLIB_DECLARE_CLASS(collision_detector_diagnoser, CollisionDetectorDiagnoser,
                        collision_detector_diagnoser::CollisionDetectorDiagnoser,
                        fault_core::FaultDetector)

using namespace fault_core;
using namespace message_filters;
namespace collision_detector_diagnoser
{

  CollisionDetectorDiagnoser::CollisionDetectorDiagnoser(): isCollisionDetected(false),
                                                            time_of_collision_(),
                                                            mode_(0),
                                                            sensor_number_(4),
                                                            filter_(false),
                                                            percentage_threshold_(0.5),
                                                            age_penalty_(-1.0),
                                                            max_interval_(0.0),
                                                            queue_size_(10),
                                                            is_custom_filter_requested_(false),
                                                            debug_mode_(false),
                                                            is_collision_published_(false),
                                                            mean_collision_orientation_()
  {
    fault_.type_ =  FaultTopology::UNKNOWN_TYPE; //Default Value to Type
    fault_.cause_ = FaultTopology::UNKNOWN; //Default Value for cause
    ROS_INFO("Default Constructor CollisionDetectorDiagnoser");
  }

  CollisionDetectorDiagnoser::CollisionDetectorDiagnoser(int sensor_number): isCollisionDetected(false),
                                                                             time_of_collision_(),
                                                                             mode_(0),
                                                                             sensor_number_(sensor_number),
                                                                             filter_(false),
                                                                             age_penalty_(-1.0),
                                                                             max_interval_(0.0),
                                                                             queue_size_(10),
                                                                             is_custom_filter_requested_(false),
                                                                             debug_mode_(false),
                                                                             is_collision_published_(false),
                                                                             mean_collision_orientation_()
  {
    //Used In teh Node
    setDynamicReconfigureServer();
    ros::NodeHandle private_n("~");
    fault_.type_ =  FaultTopology::UNKNOWN_TYPE;
    fault_.cause_ = FaultTopology::UNKNOWN;

    //Init Service Client
    strength_srv_client_ = private_n.serviceClient<kinetic_energy_monitor::KineticEnergyMonitorMsg>("kinetic_energy_drop");
    orientations_srv_client_ = private_n.serviceClient<footprint_checker::CollisionCheckerMsg>("collision_checker");

    //this publisher is cob3 specific
    speak_pub_ = private_n.advertise<std_msgs::String>("/sound/say",1);
    //Wait for service
    ros::Duration(2).sleep();

    //Initialize Publishers
    orientation_pub_ = private_n.advertise<geometry_msgs::PoseArray>("measured_collision_orientations", 1);
    collision_pub_ = private_n.advertise<fusion_msgs::monitorStatusMsg>("overall_collision", 1);
    ROS_INFO("Constructor CollisionDetectorDiagnoser");
  }

  CollisionDetectorDiagnoser::~CollisionDetectorDiagnoser()
  {

  }

  void CollisionDetectorDiagnoser::instantiateServices(ros::NodeHandle nh){
    setDynamicReconfigureServer();
    nh.param("sensor_fusion/sensor_number", sensor_number_);
    nh.param("sensor_fusion/mode", mode_);
    nh.param("sensor_fusion/filter", filter_);
    ROS_DEBUG_STREAM(" Selected mode" << mode_);

    strength_srv_client_ = nh.serviceClient<kinetic_energy_monitor::KineticEnergyMonitorMsg>("/kinetic_energy_drop");
    orientations_srv_client_ = nh.serviceClient<footprint_checker::CollisionCheckerMsg>("/collision_checker");

    speak_pub_ = nh.advertise<std_msgs::String>("/sound/say",1);
    ros::Duration(2).sleep();
    orientation_pub_ = nh.advertise<geometry_msgs::PoseArray>("measured_collision_orientations", 1);
    collision_pub_ = nh.advertise<fusion_msgs::monitorStatusMsg>("overall_collision", 1);

  }

  void CollisionDetectorDiagnoser::setDynamicReconfigureServer(){
    //init a DynamicReconfigureServer
    std::string node_handler_name("collision_detector_diagnoser");
    ros::NodeHandle nh_for_dyn("~/"+ node_handler_name);
    dyn_server = new dynamic_reconfigure::Server<collision_detector_diagnoser::diagnoserConfig>(nh_for_dyn);
    dyn_server_cb = boost::bind(&CollisionDetectorDiagnoser::dyn_reconfigureCB, this, _1, _2);
    dyn_server->setCallback(dyn_server_cb);
  }

  void CollisionDetectorDiagnoser::dyn_reconfigureCB(collision_detector_diagnoser::diagnoserConfig &config, uint32_t level){
    mutex mu; //just one service call at the time
    mu.lock();
    ROS_INFO("Configuration Update Required");
    mode_ = config.mode;
    filter_ = config.allow_filter;
    percentage_threshold_ = config.percentage_threshold;
    sensor_number_ = config.sensor_sources;
    age_penalty_ = config.age_penalty;
    max_interval_ = std::numeric_limits< int32_t >::max() * config.max_interval; //Comming from Asycronous message filter api
    queue_size_ = config.queue_size;
    is_custom_filter_requested_ = config.custom_filter;
    debug_mode_ = config.request_debug_mode;
    setTimeOut(config.custom_timeout); //For Custom

    initialize(sensor_number_);
    isCollisionDetected = false;
    ROS_INFO("Configuration Update Complete");
    mu.unlock();
  }


  fault_core::FaultTopology CollisionDetectorDiagnoser::getFault()
  {
     return fault_;
  }

  void CollisionDetectorDiagnoser::listenTime(){

    int current_collisions = 0;
    ros::Duration(1).sleep();

    while(is_custom_filter_requested_){//thread end just when the filter is not more required
      current_collisions = std::count (collision_flags_, collision_flags_+getInputNumber(), true);//count collisons(true) on flags

      if(current_collisions >= ceil(getCustomThrehold()* getInputNumber())){ // check is collision thread is reached
        ROS_DEBUG_STREAM("CUSTOM COLLISION FOUND in "<< current_collisions <<" observers");
        isCollisionDetected = true; //set colliison
      }
      else{
        isCollisionDetected = false;//false colliison
      }

    }
  }

  void CollisionDetectorDiagnoser::timeoutReset(){
    while(is_custom_filter_requested_){//thread end just when the filter is not more required
       CustomMessageFilter::resetCollisionFlags(); //resetFlags
       CustomMessageFilter::clearCustomCollisionObserversIDS(); //clearSensorIds
       is_collision_published_ = false;
       ros::Duration(double(CustomMessageFilter::getTimeOut())/1000).sleep(); //Sleep for Timeout(milliseconds)/1000 seconds
    }
  }

  void CollisionDetectorDiagnoser::plotOrientation(list<fusion_msgs::sensorFusionMsg> v){
    geometry_msgs::PoseArray array_msg;
    array_msg.header.frame_id = "base_link";

    for (std::list<fusion_msgs::sensorFusionMsg>::iterator it=v.begin(); it != v.end(); ++it){
      geometry_msgs::Pose pose;
      pose.orientation = tf::createQuaternionMsgFromYaw (it->angle);
      array_msg.poses.push_back(pose); // add pose on origin with angle of observer
    }

    orientation_pub_.publish(array_msg);
  }

  void CollisionDetectorDiagnoser::twoSensorsCallBack(const fusion_msgs::sensorFusionMsgConstPtr& detector_1,
                                                      const fusion_msgs::sensorFusionMsgConstPtr& detector_2){
    ROS_DEBUG("TwoSensors");
    fusion_msgs::sensorFusionMsg array[] = {*detector_1,*detector_2};
    std::list<fusion_msgs::sensorFusionMsg> list(array, array + sizeof(array)/sizeof(fusion_msgs::sensorFusionMsg));//parseToList

    if(fusion_approach_->detect(list)){//current sf approach
      plotOrientation(list);
      time_of_collision_ = detector_1->header; //TODO
      isCollisionDetected = true;

    }
    else{
      isCollisionDetected = false;
    }
  }

  void CollisionDetectorDiagnoser::threeSensorsCallBack(const fusion_msgs::sensorFusionMsgConstPtr& detector_1,
                                                        const fusion_msgs::sensorFusionMsgConstPtr& detector_2,
                                                        const fusion_msgs::sensorFusionMsgConstPtr& detector_3){
    ROS_DEBUG("Three Sensors");
    fusion_msgs::sensorFusionMsg array[] = {*detector_1,*detector_2,*detector_3};
    std::list<fusion_msgs::sensorFusionMsg> list(array, array + sizeof(array)/sizeof(fusion_msgs::sensorFusionMsg));//parseToList

    if(fusion_approach_->detect(list)){//current sf approach
      plotOrientation(list);
      time_of_collision_ = detector_1->header; //TODO
      isCollisionDetected = true;

    }
    else{
      isCollisionDetected = false;
    }
  }

  void CollisionDetectorDiagnoser::fourSensorsCallBack(const fusion_msgs::sensorFusionMsgConstPtr& detector_1,
                                                        const fusion_msgs::sensorFusionMsgConstPtr& detector_2,
                                                        const fusion_msgs::sensorFusionMsgConstPtr& detector_3,
                                                        const fusion_msgs::sensorFusionMsgConstPtr& detector_4){
    ROS_DEBUG("Four Sensors");
    fusion_msgs::sensorFusionMsg array[] = {*detector_1,*detector_2,*detector_3, *detector_4};
    std::list<fusion_msgs::sensorFusionMsg> list(array, array + sizeof(array)/sizeof(fusion_msgs::sensorFusionMsg)); //parseToList

    if(fusion_approach_->detect(list)){//current sf approach
      plotOrientation(list);
      time_of_collision_ = detector_1->header; //TODO
      isCollisionDetected = true;

    }
    else{
      isCollisionDetected = false;
    }
  }

  void CollisionDetectorDiagnoser::fiveSensorsCallBack(const fusion_msgs::sensorFusionMsgConstPtr& detector_1,
                                                        const fusion_msgs::sensorFusionMsgConstPtr& detector_2,
                                                        const fusion_msgs::sensorFusionMsgConstPtr& detector_3,
                                                        const fusion_msgs::sensorFusionMsgConstPtr& detector_4,
                                                        const fusion_msgs::sensorFusionMsgConstPtr& detector_5){
    ROS_DEBUG("Five Sensors");

    fusion_msgs::sensorFusionMsg array[] = {*detector_1,*detector_2,*detector_3, *detector_4, *detector_5};
    std::list<fusion_msgs::sensorFusionMsg> list(array, array + sizeof(array)/sizeof(fusion_msgs::sensorFusionMsg));//parseToList

    if(fusion_approach_->detect(list)){//current sf approach
      plotOrientation(list);
      time_of_collision_ = detector_1->header; //TODO
      isCollisionDetected = true;

    }
    else{
      isCollisionDetected = false;
    }
  }


  void CollisionDetectorDiagnoser::simpleCallBack(const fusion_msgs::sensorFusionMsg msg){
    ROS_DEBUG("Simple Filtering");
    list <fusion_msgs::sensorFusionMsg> list;
    fusion_msgs::sensorFusionMsg tmp = msg;
    list.push_back(msg);

    //any collision detected plot orientation
    if(fusion_approach_->detect(list)){
      plotOrientation(list);
    }

    if (msg.msg == fusion_msgs::sensorFusionMsg::ERROR){
      ROS_INFO_STREAM ("Collision detected by " << msg.sensor_id);
      time_of_collision_ = msg.header;
      isCollisionDetected = true;
      mean_collision_orientation_ = tf::createQuaternionFromYaw(msg.angle); //calculate overall orientation
    }
    else{
      isCollisionDetected = false;
    }
  }

  void CollisionDetectorDiagnoser::unregisterCallbackForSyncronizers(){
    main_connection.disconnect(); //disconnect any syncronized CB
  }

  void CollisionDetectorDiagnoser::registerCallbackForSyncronizers(int sensor_number){
    ROS_DEBUG_STREAM("Set CB Sync");
    switch(sensor_number){ //syncronization procedure up to 5 observers using the policies provided at sync_policies.h
      case 2:
        syncronizer_for_two_ = new message_filters::Synchronizer<MySyncPolicy2>(MySyncPolicy2(queue_size_), *filtered_subscribers_.at(0),
                                                                                               *filtered_subscribers_.at(1));
        syncronizer_for_two_->setAgePenalty(age_penalty_);
        syncronizer_for_two_->setMaxIntervalDuration(ros::Duration(max_interval_));

        main_connection = syncronizer_for_two_->registerCallback(boost::bind(&CollisionDetectorDiagnoser::twoSensorsCallBack,this,_1, _2));
        break;
      case 3:
        syncronizer_for_three_ = new message_filters::Synchronizer<MySyncPolicy3>(MySyncPolicy3(queue_size_), *filtered_subscribers_.at(0),
                                                                                                     *filtered_subscribers_.at(1),
                                                                                                     *filtered_subscribers_.at(2));
        syncronizer_for_three_->setAgePenalty(age_penalty_);
        syncronizer_for_three_->setMaxIntervalDuration(ros::Duration(max_interval_));


        main_connection = syncronizer_for_three_->registerCallback(boost::bind(&CollisionDetectorDiagnoser::threeSensorsCallBack,this,_1, _2,_3));
        break;
      case 4:
        syncronizer_for_four_ = new message_filters::Synchronizer<MySyncPolicy4>(MySyncPolicy4(queue_size_), *filtered_subscribers_.at(0),
                                                                                               *filtered_subscribers_.at(1),
                                                                                               *filtered_subscribers_.at(2),
                                                                                               *filtered_subscribers_.at(3));
        syncronizer_for_four_->setMaxIntervalDuration(ros::Duration(max_interval_));

        main_connection = syncronizer_for_four_->registerCallback(boost::bind(&CollisionDetectorDiagnoser::fourSensorsCallBack,this,_1, _2,_3,_4));
        syncronizer_for_four_->setAgePenalty(age_penalty_);
        break;
      case 5:
        syncronizer_for_five_ = new message_filters::Synchronizer<MySyncPolicy5>(MySyncPolicy5(queue_size_), *filtered_subscribers_.at(0),
                                                                                             *filtered_subscribers_.at(1),
                                                                                             *filtered_subscribers_.at(2),
                                                                                             *filtered_subscribers_.at(3),
                                                                                             *filtered_subscribers_.at(4));
        syncronizer_for_five_->setAgePenalty(age_penalty_);
        syncronizer_for_five_->setMaxIntervalDuration(ros::Duration(max_interval_));

        main_connection = syncronizer_for_five_->registerCallback(boost::bind(&CollisionDetectorDiagnoser::fiveSensorsCallBack,this,_1, _2,_3,_4,_5));
        break;

      default:
      ROS_ERROR_STREAM("registerCallback Failure");
    }
  }


  void CollisionDetectorDiagnoser::resetUnFilteredSubscribers(){
    ROS_DEBUG("Reset Unfilter Subscribers");
    for (int i = 0; i< array_subscribers_.size();i++){//remove normal subscribers
      array_subscribers_[i].shutdown();
    }//endFor

    array_subscribers_.erase(array_subscribers_.begin(), array_subscribers_.end());

  }

  void CollisionDetectorDiagnoser::resetFilteredSubscribers(){
    ROS_DEBUG("Reset Filtered subscribers");
    for(int i=0; i< filtered_subscribers_.size(); i++){
      filtered_subscribers_.at(i)->unsubscribe();// unsubscribe all filtered messages
    }//endFor
    filtered_subscribers_.erase(filtered_subscribers_.begin(), filtered_subscribers_.end());
  }

  void CollisionDetectorDiagnoser::setUnfilteredSubscribers(int sensor_number, ros::NodeHandle nh){
    ROS_DEBUG("Set Unfiltered Publishers");
    for (int i = 0; i< sensor_number;i++){//add normal subscribers
      ros::Subscriber sub = nh.subscribe("collisions_"+std::to_string(i), 10, &CollisionDetectorDiagnoser::simpleCallBack, this);
      array_subscribers_.push_back(sub);
    }//
  }

  void CollisionDetectorDiagnoser::setFilteredSubscribers(int sensor_number, ros::NodeHandle nh){
    ROS_DEBUG("Set Filtered Publishers");

    for(int i=0; i< sensor_number; i++){// create subscribers to filter
      filtered_subscribers_.push_back(new message_filters::Subscriber<fusion_msgs::sensorFusionMsg>(nh, "collisions_"+std::to_string(i), 10));
    }
    registerCallbackForSyncronizers(sensor_number); //initiate the syncronizers which belongs to the selection
  }

  void CollisionDetectorDiagnoser::initialize(int sensor_number)
  {
    ros::NodeHandle nh;
    sensor_number_ = sensor_number;
    ROS_DEBUG_STREAM("initializing " << sensor_number << " sensors");
    ROS_DEBUG_STREAM("Method" << std::to_string(mode_) << " Selected");

    //EveryCycle all syncronization methods are stopped
    stop(); //Stop CustomFilter
    ROS_INFO("ASTO");

    if (is_custom_filter_requested_){
      isCollisionDetected = false;
      ROS_DEBUG("Custom Filtering Requested");
      //reset Syncronizers
      resetUnFilteredSubscribers();
      unregisterCallbackForSyncronizers();
      ROS_DEBUG("Starting Threads");
      start(sensor_number_);
      ROS_DEBUG("Custom Filtering Ready");

    }
    else {

      if(!filter_){//unfiltered
        //resetFilter
        resetFilteredSubscribers();
        //setUnfilter
        setUnfilteredSubscribers(sensor_number, nh);
      }//Endif
      else{// Switching to filtered messages
        //resetUnFiltered
        resetUnFilteredSubscribers();
        unregisterCallbackForSyncronizers();
        //setFilter
        setFilteredSubscribers(sensor_number, nh);
      }//endElse
    }
    //Swap betweenModes;
    selectMode();

    //Update Threshold
    setCustomThreshold(percentage_threshold_);
    fusion_approach_->setThreshold(percentage_threshold_);

  }

  void CollisionDetectorDiagnoser::selectMode(){
    switch(mode_){
      //Pointer overwritted for selected procedure
      case 0: fusion_approach_ = &default_approach_; break;
      case 1: fusion_approach_ = &voting_approach_; break;
      case 2: fusion_approach_ = &weighted_approach_; break;
      default: ROS_ERROR_STREAM("selecMode Error with code "<< mode_);break;
    }
  }
  bool CollisionDetectorDiagnoser::detectFault()
  {

    if (debug_mode_){//if debug mode then publish status_output_msg
      status_output_msg_.header.stamp = ros::Time::now();
      status_output_msg_.header.frame_id = "base_link";

      if (isCollisionDetected && !is_collision_published_){
        if (is_custom_filter_requested_){
          status_output_msg_.observers_ids = getCustomCollisionObservers();
        }
        else{
          status_output_msg_.observers_ids = fusion_approach_->getCollisionObservers();
        }
        collision_pub_.publish(status_output_msg_); //TODO

        if (is_custom_filter_requested_){
            is_collision_published_ = true;
        }
      }
    }

    return isCollisionDetected;
  }

  void CollisionDetectorDiagnoser::isolateFault(){

    //collision_output_msg_.header = time_of_collision_; //TODO
    fault_.type_ = FaultTopology::COLLISION;

    ROS_INFO("Isolating Platform Collision");
    isCollisionDetected = false;
    //Say Ouch
    std_msgs::String msg;
    msg.data ="ouch";
    speak_pub_.publish(msg);

    diagnoseFault();

  }

  void CollisionDetectorDiagnoser::diagnoseFault(){
    //Force
    kinetic_energy_monitor::KineticEnergyMonitorMsg kinetic_srv;
    footprint_checker::CollisionCheckerMsg orientation_srv;

    kinetic_srv.request.collision_time = time_of_collision_;

    if(strength_srv_client_.call(kinetic_srv)){
      ROS_INFO_STREAM("Strength: " << kinetic_srv.response.energy_lost);
    }

     tf::quaternionTFToMsg(tf::createQuaternionFromYaw(kinetic_srv.response.collision_angle), orientation_srv.request.collision_orientation);


    if(orientations_srv_client_.call(orientation_srv)){
      ROS_INFO("Orientations Computed Correctly");
      if (orientation_srv.response.is_static_collision){
        fault_.cause_ = FaultTopology::STATIC_OBSTACLE;
        ROS_ERROR("STATIC Collision FOUND");
      }
      else{
        fault_.cause_ = FaultTopology::DYNAMIC_OBSTACLE;
        ROS_ERROR("DYNAMIC Collision FOUND");
      }

      ros::Duration(2).sleep();//Give time to costmap to update
    }

    else{
      ROS_WARN("Error in orientations Server");
      fault_.cause_ = FaultTopology::UNKNOWN;
      ROS_ERROR("UNKNOWN Collision FOUND");
    }

  }
}  // namespace collision_detector_diagnoser
