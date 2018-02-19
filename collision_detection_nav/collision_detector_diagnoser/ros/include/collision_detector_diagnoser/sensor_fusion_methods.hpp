#include <list>
#include <ros/ros.h>
#include <fusion_msgs/sensorFusionMsg.h>
#include <std_msgs/Float32.h>

using namespace std;

class SensorFusionApproach {
  public:
    virtual bool detect(list<fusion_msgs::sensorFusionMsg> v) {
      ROS_DEBUG("Default");
      for (std::list<fusion_msgs::sensorFusionMsg>::iterator it=v.begin(); it != v.end(); ++it){
        if(it->msg == fusion_msgs::sensorFusionMsg::ERROR){
          ROS_WARN_STREAM("Collision DETECTED on " << it->sensor_id);
          return true;
        }
      }
      return false;
    };
    void setThreshold(double thr){
      ROS_DEBUG("Threshold update");
      threshold = thr;
    }
  protected:
    double threshold = 0.5; //TODO
};

class VotingApproach : public SensorFusionApproach {
    public:
      bool detect(list<fusion_msgs::sensorFusionMsg> v){
        ROS_DEBUG("Consensus");
        int counter = 0;
        for (std::list<fusion_msgs::sensorFusionMsg>::iterator it=v.begin(); it != v.end(); ++it){
          if(it->msg == fusion_msgs::sensorFusionMsg::ERROR){
            ROS_WARN_STREAM("Collision DETECTED on " << it->sensor_id);
            counter ++;
          }
        }
        if (counter >= v.size()*threshold){
          ROS_ERROR_STREAM("Voting Collision Detected");
          ROS_DEBUG_STREAM("Counter " << counter);
          return true;
        }

        return false;
      };
};

class WeightedApproach : public SensorFusionApproach {
    public:
      bool detect(list<fusion_msgs::sensorFusionMsg> v){
        ROS_DEBUG("Weighted");
        double max_value = 0; //TODO
        double count = 0;

        for (std::list<fusion_msgs::sensorFusionMsg>::iterator it=v.begin(); it != v.end(); ++it){
          max_value+= fusion_msgs::sensorFusionMsg::ERROR * it->weight;
          count += it->msg * it->weight;
        }

        ROS_DEBUG_STREAM("Count " << count);
        if ((count/max_value) >= threshold){
          ROS_ERROR_STREAM("Weighted Collision Detected: " << count << " Of " << max_value << " Percentage:" << (count/max_value));
          return true;

        }

        return false;
      };
};
