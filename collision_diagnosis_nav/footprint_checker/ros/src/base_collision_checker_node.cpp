#include <footprint_checker/base_collision_checker.h>

int main(int argc, char *argv[])
{
    ros::init(argc, argv, "base_collision_checker_server");

    ros::NodeHandle nh("~");
    //Init service
    BaseCollisionChecker base_collision_checker(nh);

    return 0;
}
