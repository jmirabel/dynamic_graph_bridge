/*
 * Copyright 2016,
 * Olivier Stasse,
 *
 * CNRS
 *
 * This file is part of dynamic_graph_bridge.
 * dynamic_graph_bridge is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 * dynamic_graph_bridge is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.  You should
 * have received a copy of the GNU Lesser General Public License along
 * with dynamic_graph_bridge.  If not, see <http://www.gnu.org/licenses/>.
 */
/* -------------------------------------------------------------------------- */
/* --- INCLUDES ------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

#include <dynamic_graph_bridge/sot_loader.hh>
#include "dynamic_graph_bridge/ros_init.hh"

#include <dynamic-graph/pool.h>

// POSIX.1-2001
#include <dlfcn.h>

#include <boost/thread/thread.hpp>
#include <boost/thread/condition.hpp>

boost::mutex mut;

using namespace std;
using namespace dynamicgraph::sot; 
namespace po = boost::program_options;

SotLoaderBasic::SotLoaderBasic():
  dynamic_graph_stopped_(true),
  sotRobotControllerLibrary_(0)
{
  readSotVectorStateParam();
  initPublication();
}

int SotLoaderBasic::initPublication()
{
  ros::NodeHandle & n = dynamicgraph::rosInit(false);
 

  // Prepare message to be published
  joint_pub_ = n.advertise<sensor_msgs::JointState>("joint_states", 1);

  return 0;
}


void SotLoaderBasic::initializeRosNode(int , char *[])
{
  ROS_INFO("Ready to start dynamic graph.");
  ros::NodeHandle n;
  service_start_ = n.advertiseService("start_dynamic_graph",
                                     &SotLoaderBasic::start_dg,
                                     this);

  service_stop_  = n.advertiseService("stop_dynamic_graph",
                                     &SotLoaderBasic::stop_dg,
                                     this);
  
}   


void SotLoaderBasic::setDynamicLibraryName(std::string &afilename)
{
  dynamicLibraryName_ = afilename;
}

int SotLoaderBasic::readSotVectorStateParam()
{
  std::map<std::string,int> from_state_name_to_state_vector;
  std::map<std::string,std::string> from_parallel_name_to_state_vector_name;
  ros::NodeHandle n;

  if (!ros::param::has("/sot_controller/joint_names"))
    {
      std::cerr<< " Could not read param /sot_controller/joint_names" << std::endl;
      return 1;
    }

  n.getParam("/sot_controller/joint_names", stateVectorMap_);
  ROS_ASSERT(stateVectorMap_.getType() == XmlRpc::XmlRpcValue::TypeArray);
  nbOfJoints_ = stateVectorMap_.size();
  nbOfParallelJoints_ = 0;

  if (ros::param::has("/sot_controller/joint_state_parallel"))
    {
      XmlRpc::XmlRpcValue joint_state_parallel;
      n.getParam("/sot_controller/joint_state_parallel", joint_state_parallel);
      ROS_ASSERT(joint_state_parallel.getType() == XmlRpc::XmlRpcValue::TypeStruct);
      std::cout << "Type of joint_state_parallel:" << joint_state_parallel.getType() << std::endl;

      for(XmlRpc::XmlRpcValue::iterator it = joint_state_parallel.begin(); 
          it!= joint_state_parallel.end(); it++) 
        {
          XmlRpc::XmlRpcValue local_value=it->second;
          std::string final_expression, map_expression = static_cast<string>(local_value); 
          double final_coefficient = 1.0;
          // deal with sign 
          if (map_expression[0]=='-')
            {
              final_expression = map_expression.substr(1,map_expression.size()-1);
              final_coefficient = -1.0;
            }
          else 
            final_expression = map_expression;

          std::cout <<it->first.c_str() << ": " << final_coefficient << std::endl;
          from_parallel_name_to_state_vector_name[it->first.c_str()] = final_expression;
          coefficient_parallel_joints_.push_back(final_coefficient);

        }
      nbOfParallelJoints_ = from_parallel_name_to_state_vector_name.size();
    }

  // Prepare joint_state according to robot description.
  joint_state_.name.resize(nbOfJoints_+nbOfParallelJoints_);
  joint_state_.position.resize(nbOfJoints_+nbOfParallelJoints_);

  // Fill in the name of the joints from the state vector.
  // and build local map from state name to state vector
  for (int32_t i = 0; i < stateVectorMap_.size(); ++i) 
   {
     joint_state_.name[i]= static_cast<string>(stateVectorMap_[i]);

     from_state_name_to_state_vector[joint_state_.name[i]] = i;
   }

  // Fill in the name of the joints from the parallel joint vector.
  // and build map from parallel name to state vector
  int i=0;
  parallel_joints_to_state_vector_.resize(nbOfParallelJoints_);
  for (std::map<std::string,std::string>::iterator  it = from_parallel_name_to_state_vector_name.begin();
       it != from_parallel_name_to_state_vector_name.end();
       it++,i++)
    {
      joint_state_.name[i+nbOfJoints_]=it->first.c_str();
      parallel_joints_to_state_vector_[i] = from_state_name_to_state_vector[it->second];
    }

  return 0;
}


int SotLoaderBasic::parseOptions(int argc, char *argv[])
{ 
  po::options_description desc("Allowed options");
  desc.add_options()
    ("help", "produce help message")
    ("input-file", po::value<string>(), "library to load")
    ;
    
  
  po::store(po::parse_command_line(argc, argv, desc), vm_);
  po::notify(vm_);    
  
  if (vm_.count("help")) {
    cout << desc << "\n";
    return -1;
  }
  if (!vm_.count("input-file")) {
    cout << "No filename specified\n";
    return -1;
  }
  else
    dynamicLibraryName_= vm_["input-file"].as< string >();
  
  Initialization();
  return 0;
}

void SotLoaderBasic::Initialization()
{
  
  // Load the SotRobotBipedController library.
  sotRobotControllerLibrary_ = dlopen( dynamicLibraryName_.c_str(),
                                             RTLD_LAZY | RTLD_GLOBAL );
  if (!sotRobotControllerLibrary_) {
    std::cerr << "Cannot load library: " << dlerror() << '\n';
    return ;
  }
  
  // reset errors
  dlerror();
  
  // Load the symbols.
  createSotExternalInterface_t * createSot =
    reinterpret_cast<createSotExternalInterface_t *> 
    (reinterpret_cast<long> 
     (dlsym(sotRobotControllerLibrary_, 
	    "createSotExternalInterface")));
  const char* dlsym_error = dlerror();
  if (dlsym_error) {
    std::cerr << "Cannot load symbol create: " << dlsym_error << '\n';
    return ;
  }
  
  // Create robot-controller
  sotController_ = createSot();
  cout <<"Went out from Initialization." << endl;
}

void SotLoaderBasic::CleanUp()
{
  dynamicgraph::PoolStorage::destroy();
  // We do not destroy the FactoryStorage singleton because the module will not
  // be reloaded at next initialization (because Python C API cannot safely
  // unload a module...).
  // SignalCaster singleton could probably be destroyed.

  // Load the symbols.
  destroySotExternalInterface_t * destroySot =
    reinterpret_cast<destroySotExternalInterface_t *> 
    (reinterpret_cast<long> 
     (dlsym(sotRobotControllerLibrary_, 
	    "destroySotExternalInterface")));
  const char* dlsym_error = dlerror();
  if (dlsym_error) {
    std::cerr << "Cannot load symbol destroy: " << dlsym_error << '\n';
    return ;
  }

  destroySot (sotController_);
  sotController_ = NULL;

  /// Uncount the number of access to this library.
  dlclose(sotRobotControllerLibrary_);
}



bool SotLoaderBasic::start_dg(std_srvs::Empty::Request& , 
                         std_srvs::Empty::Response& )
{
  dynamic_graph_stopped_=false;    
  return true;
}

bool SotLoaderBasic::stop_dg(std_srvs::Empty::Request& , 
                         std_srvs::Empty::Response& )
{
  dynamic_graph_stopped_ = true;
  return true;
}

