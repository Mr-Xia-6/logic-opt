/**
 * opspace_controller.cc
 *
 * Copyright 2019. All Rights Reserved.
 *
 * Created: June 4, 2019
 * Authors: Toki Migimatsu
 */

#include "logic_opt/control/opspace_controller.h"

#include <ctrl_utils/control.h>
#include <ctrl_utils/euclidian.h>
#include <ctrl_utils/filesystem.h>
#include <ctrl_utils/json.h>
#include <ctrl_utils/redis_client.h>
#include <ctrl_utils/timer.h>
#include <spatial_dyn/algorithms/inverse_kinematics.h>

#include "logic_opt/control/throw_constraint_scp.h"

namespace {

// Robot
const std::string kNameRobot = "kuka_iiwa";
const std::string kPathUrdf = "../resources/" + kNameRobot + "/" + kNameRobot + ".urdf";

const std::string KEY_PREFIX         = "logic_opt::";
const std::string KEY_MODELS_PREFIX  = KEY_PREFIX + "model::";
const std::string KEY_OBJECTS_PREFIX = KEY_PREFIX + "object::";
const std::string KEY_OBJECT_MODELS_PREFIX = KEY_OBJECTS_PREFIX + "model::";
const std::string KEY_TRAJ_PREFIX    = KEY_PREFIX + "trajectory::";

// SET keys
const std::string KEY_SENSOR_Q       = KEY_PREFIX + kNameRobot + "::sensor::q";
const std::string KEY_CONTROL_POS    = KEY_PREFIX + kNameRobot + "::control::pos";
const std::string KEY_TRAJ_POS       = KEY_TRAJ_PREFIX + kNameRobot + "::pos";

// Webapp keys
const std::string kNameApp   = "simulator";
const std::string KEY_WEB_RESOURCES   = "webapp::resources";
const std::string KEY_WEB_ARGS        = "webapp::" + kNameApp + "::args";
const std::string KEY_WEB_INTERACTION = "webapp::" + kNameApp + "::interaction";

// Controller gains
const std::string KEY_KP_KV_POS   = KEY_PREFIX + "control::kp_kv_pos";
const std::string KEY_KP_KV_ORI   = KEY_PREFIX + "control::kp_kv_ori";
const std::string KEY_KP_KV_JOINT = KEY_PREFIX + "control::kp_kv_joint";

const Eigen::Vector2d kKpKvPos   = Eigen::Vector2d(10., 6.);
const Eigen::Vector2d kKpKvOri   = Eigen::Vector2d(40., 40.);
const Eigen::Vector2d kKpKvJoint = Eigen::Vector2d(16., 8.);
const double kMaxVel             = 0.1;
const double kTimerFreq          = 1000.;
const double kGainClickDrag      = 100.;

const Eigen::VectorXd kQHome     = (M_PI / 180.) * (Eigen::Matrix<double,7,1>() <<
                                   90., -30., 0., 60., 0., -90., 0.).finished();

const std::string kEeFrame = "ee";

void InitializeWebApp(ctrl_utils::RedisClient& redis_client, const spatial_dyn::ArticulatedBody& ab,
                      const std::map<std::string, logic_opt::Object3>& objects, size_t T);

std::pair<std::map<size_t, spatial_dyn::SpatialForced>, std::map<std::string, std::pair<Eigen::Vector3d, Eigen::Vector3d>>>
ComputeExternalForces(const spatial_dyn::ArticulatedBody& ab, const nlohmann::json& interaction);

void UpdateObjectStates(ctrl_utils::RedisClient& redis_client, const logic_opt::World3& world,
                        std::map<std::string, logic_opt::Object3>& sim_objects_abs,
                        size_t idx_trajectory, const Eigen::MatrixXd& X_optimal,
                        const Eigen::Isometry3d& T_ee_to_world,
                        const std::map<std::string, std::pair<Eigen::Vector3d, Eigen::Vector3d>>& f_ext);

}  // namespace

namespace logic_opt {

void ExecuteOpspaceController(spatial_dyn::ArticulatedBody& ab, const World3& world,
                              const std::shared_ptr<const std::map<std::string, Object3>> world_objects,
                              const Eigen::MatrixXd& X_optimal, volatile std::sig_atomic_t& g_runloop) {

  // End-effector parameters
  const Eigen::Isometry3d& T_ee = world_objects->at(kEeFrame).T_to_parent();
  Eigen::Quaterniond quat_ee(T_ee.linear());
  Eigen::Ref<const Eigen::Vector3d> ee_offset = T_ee.translation();

  std::map<size_t, std::pair<Eigen::MatrixXd, Eigen::MatrixXd>> throw_trajectories;

  // Optimize trajectory
  for (size_t t = 1; t < X_optimal.cols(); t++) {
    if (world.controller(t) == "throw") {
      // Controller frames
      const std::pair<std::string, std::string>& controller_frames = world.controller_frames(t);
      const std::string& control_frame = controller_frames.first;
      const std::string& target_frame = controller_frames.second;

      const Eigen::Isometry3d T_control_to_ee_0 = quat_ee * world.T_to_frame(control_frame, kEeFrame, X_optimal, t-1);
      const Eigen::Isometry3d T_target_to_world_0 = world.T_to_world(control_frame, X_optimal, t-1);
      const Eigen::Isometry3d T_control_to_target_0 = world.T_control_to_target(X_optimal, t-1);
      const Eigen::Isometry3d T_control_to_world_0 = T_target_to_world_0 * T_control_to_target_0;

      const Eigen::Isometry3d T_obj_to_world = T_target_to_world_0 * T_control_to_target_0;
      const Eigen::Isometry3d T_des_to_world_0 = T_target_to_world_0 * T_control_to_target_0 * T_control_to_ee_0.inverse();
      const Eigen::Quaterniond quat_0(T_des_to_world_0.linear());

      // TODO: from perception
      const Eigen::Isometry3d T_control_to_ee = quat_ee * world.T_to_frame(control_frame, kEeFrame, X_optimal, t);
      const Eigen::Isometry3d T_target_to_world = world.T_to_world(target_frame, X_optimal, t);
      const Eigen::Isometry3d T_control_to_target = world.T_control_to_target(X_optimal, t);
      const Eigen::Isometry3d T_control_to_world = T_target_to_world * T_control_to_target;

      // Prepare position-orientation task
      const Eigen::Isometry3d T_des_to_world = T_target_to_world * T_control_to_target * T_control_to_ee.inverse();

      const Eigen::Vector3d offset = T_control_to_ee.translation() + T_ee.translation();

      Eigen::VectorXd q_0 = spatial_dyn::InverseKinematics(ab, T_target_to_world_0.translation(), quat_0, -1, offset);

      throw_trajectories[t] = ThrowConstraintScp(ab, q_0, T_control_to_world.translation(), offset);
    }
  }

  // Set up timer and Redis
  ctrl_utils::RedisClient redis_client;
  redis_client.connect();

  // Initialize controller parameters
  Eigen::VectorXd q_des = kQHome;

  InitializeWebApp(redis_client, ab, *world_objects, world.num_timesteps());
  redis_client.set(KEY_SENSOR_Q, ab.q());
  redis_client.set(KEY_KP_KV_POS, kKpKvPos);
  redis_client.set(KEY_KP_KV_ORI, kKpKvOri);
  redis_client.set(KEY_KP_KV_JOINT, kKpKvJoint);
  redis_client.sync_commit();

  ctrl_utils::Timer timer(1000);

  size_t idx_trajectory = 0;
  std::map<std::string, Object3> sim_objects_abs = *world_objects;
  while (g_runloop) {
    timer.Sleep();

    // Get Redis values
    std::future<Eigen::Vector2d> fut_kp_kv_pos   = redis_client.get<Eigen::Vector2d>(KEY_KP_KV_POS);
    std::future<Eigen::Vector2d> fut_kp_kv_ori   = redis_client.get<Eigen::Vector2d>(KEY_KP_KV_ORI);
    std::future<Eigen::Vector2d> fut_kp_kv_joint = redis_client.get<Eigen::Vector2d>(KEY_KP_KV_JOINT);
    std::future<nlohmann::json> fut_interaction  = redis_client.get<nlohmann::json>(KEY_WEB_INTERACTION);
    redis_client.commit();

    // Controller frames
    const std::pair<std::string, std::string>& controller_frames = world.controller_frames(idx_trajectory);
    const std::string& control_frame = controller_frames.first;
    const std::string& target_frame = controller_frames.second;

    // TODO: from perception
    const Eigen::Isometry3d T_control_to_ee = quat_ee * world.T_to_frame(control_frame, kEeFrame, X_optimal, idx_trajectory);
    const Eigen::Isometry3d T_target_to_world = target_frame != World3::kWorldFrame
                                                  ? sim_objects_abs.at(target_frame).T_to_parent()
                                                  : Eigen::Isometry3d::Identity();//world.T_to_world(target_frame, X_optimal, idx_trajectory);
    const Eigen::Isometry3d T_control_to_target = world.T_control_to_target(X_optimal, idx_trajectory);

    // Prepare position-orientation task
    const Eigen::Isometry3d T_des_to_world = T_target_to_world * T_control_to_target * T_control_to_ee.inverse();
    const Eigen::Matrix6Xd& J = spatial_dyn::Jacobian(ab, -1, ee_offset);

    // Compute position PD control
    const Eigen::Vector3d x_des = T_des_to_world.translation();
    const Eigen::Vector3d x = spatial_dyn::Position(ab, -1, ee_offset);
    const Eigen::Vector3d dx = J.topRows<3>() * ab.dq();
    const Eigen::Vector3d ddx = ctrl_utils::PdControl(x, x_des, dx, fut_kp_kv_pos.get(), kMaxVel);

    // Compute orientation PD control
    const Eigen::Quaterniond quat = spatial_dyn::Orientation(ab);
    const Eigen::Quaterniond quat_des = ctrl_utils::NearQuaternion(T_des_to_world.linear(), quat);
    const Eigen::Vector3d w = J.bottomRows<3>() * ab.dq();
    const Eigen::Vector3d dw = ctrl_utils::PdControl(quat, quat_des, w, fut_kp_kv_ori.get());

    // Compute opspace torques
    Eigen::Vector6d ddx_dw;
    ddx_dw << ddx, dw;
    Eigen::MatrixXd N;
    Eigen::VectorXd tau = spatial_dyn::opspace::InverseDynamics(ab, J, ddx_dw, &N);

    // Add joint task in nullspace
    static const Eigen::MatrixXd I = Eigen::MatrixXd::Identity(ab.dof(), ab.dof());
    const Eigen::VectorXd ddq = ctrl_utils::PdControl(ab.q(), q_des, ab.dq(), fut_kp_kv_joint.get());
    tau += spatial_dyn::opspace::InverseDynamics(ab, I, ddq, &N);

    // Add gravity compensation
    tau += spatial_dyn::Gravity(ab);

    // Parse interaction from web app
    const nlohmann::json interaction = fut_interaction.get();
    const auto f_ext = ComputeExternalForces(ab, interaction);

    // Integrate
    spatial_dyn::Integrate(ab, tau, timer.dt(), f_ext.first);

    // Update object states
    const Eigen::Isometry3d& T_ee_to_world = ab.T_to_world(-1) * T_ee;
    UpdateObjectStates(redis_client, world, sim_objects_abs, idx_trajectory, X_optimal, T_ee_to_world, f_ext.second);

    redis_client.set(KEY_SENSOR_Q, ab.q());
    redis_client.set(KEY_CONTROL_POS, x_des);
    redis_client.set(KEY_TRAJ_POS, spatial_dyn::Position(ab));
    redis_client.commit();

    if (ddx_dw.norm() < 0.001) {
      if (idx_trajectory < X_optimal.cols() - 1) {
        idx_trajectory++;
        if (world.controller(idx_trajectory) == "throw") {
          // Controller frames
          const std::pair<std::string, std::string>& controller_frames = world.controller_frames(idx_trajectory);
          const std::string& control_frame = controller_frames.first;

          Eigen::MatrixXd Q, X;
          std::tie(Q, X) = throw_trajectories[idx_trajectory];

          redis_client.set(KEY_CONTROL_POS, T_des_to_world.translation());
          redis_client.commit();

          const Eigen::VectorXd q_des = Q.col(0);
          while (g_runloop) {
            timer.Sleep();

            std::future<Eigen::Vector2d> fut_kp_kv_joint = redis_client.get<Eigen::Vector2d>(KEY_KP_KV_JOINT);
            std::future<nlohmann::json> fut_interaction  = redis_client.get<nlohmann::json>(KEY_WEB_INTERACTION);
            redis_client.commit();

            Eigen::VectorXd q_err;
            const Eigen::VectorXd ddq = ctrl_utils::PdControl(ab.q(), q_des, ab.dq(), fut_kp_kv_joint.get(), 0, &q_err);
            if (q_err.norm() < 1e-3) break;
            static spatial_dyn::InverseDynamicsOptions options;
            options.centrifugal_coriolis = true;
            options.gravity = true;
            const Eigen::VectorXd tau = spatial_dyn::InverseDynamics(ab, ddq, {}, options);

            const nlohmann::json interaction = fut_interaction.get();
            const auto f_ext = ComputeExternalForces(ab, interaction);
            spatial_dyn::Integrate(ab, tau, timer.dt(), f_ext.first);

            // Update object states
            const Eigen::Isometry3d& T_ee_to_world = ab.T_to_world(-1) * T_ee;
            UpdateObjectStates(redis_client, world, sim_objects_abs, idx_trajectory, X_optimal, T_ee_to_world, f_ext.second);

            redis_client.set(KEY_SENSOR_Q, ab.q());
            redis_client.commit();
          }

          // ctrl_utils::Timer timer_throw(100);
          for (size_t t = 0; t < Q.cols(); t++) {
            if (t == Q.cols() - 1) {
              timer.Sleep();
              if (!g_runloop) break;
              ab.set_q(Q.col(t));

              const Eigen::Isometry3d& T_ee_to_world = ab.T_to_world(-1) * T_ee;
              UpdateObjectStates(redis_client, world, sim_objects_abs, idx_trajectory, X_optimal, T_ee_to_world, f_ext.second);

              redis_client.set(KEY_SENSOR_Q, ab.q());
              redis_client.set(KEY_TRAJ_POS, spatial_dyn::Position(ab));
              redis_client.commit();
              continue;
            }
            for (double dt = 0.; dt < 1.; dt += 0.1) {
              timer.Sleep();
              // timer_throw.Sleep();
              if (!g_runloop) break;

              ab.set_q((1 - dt) * Q.col(t) + dt * Q.col(t+1));

              const Eigen::Isometry3d& T_ee_to_world = ab.T_to_world(-1) * T_ee;
              UpdateObjectStates(redis_client, world, sim_objects_abs, idx_trajectory, X_optimal, T_ee_to_world, f_ext.second);

              redis_client.set(KEY_SENSOR_Q, ab.q());
              redis_client.set(KEY_TRAJ_POS, spatial_dyn::Position(ab));
              redis_client.commit();
            }
          }
          for (size_t t = 0; t < X.cols(); t++) {
            if (t == X.cols() - 1) {
              timer.Sleep();
              if (!g_runloop) break;

              spatial_dyn::RigidBody& rb = sim_objects_abs.at(control_frame);
              Eigen::Isometry3d T_to_world = rb.T_to_parent();
              T_to_world.translation() = X.col(t);
              rb.set_T_to_parent(T_to_world);

              redis_client.set(KEY_OBJECTS_PREFIX + control_frame + "::pos", rb.T_to_parent().translation());
              redis_client.set(KEY_OBJECTS_PREFIX + control_frame + "::ori", Eigen::Quaterniond(rb.T_to_parent().linear()).coeffs());
              redis_client.commit();
              continue;
            }
            for (double dt = 0.; dt < 1.; dt += 0.1) {
              timer.Sleep();
              // timer_throw.Sleep();
              if (!g_runloop) break;

              spatial_dyn::RigidBody& rb = sim_objects_abs.at(control_frame);
              Eigen::Isometry3d T_to_world = rb.T_to_parent();
              T_to_world.translation() = (1 - dt) * X.col(t) + dt * X.col(t+1);
              rb.set_T_to_parent(T_to_world);

              redis_client.set(KEY_OBJECTS_PREFIX + control_frame + "::pos", rb.T_to_parent().translation());
              redis_client.set(KEY_OBJECTS_PREFIX + control_frame + "::ori", Eigen::Quaterniond(rb.T_to_parent().linear()).coeffs());
              redis_client.commit();
            }
          }
          idx_trajectory++;
        }
      } else {
        break;
      }
    }

    if ((ab.q().array() != ab.q().array()).any()) break;
  }
  std::cout << "Simulated " << timer.time_sim() << "s in " << timer.time_elapsed() << "s." << std::endl;
  std::cout << std::endl;
}

}  // namespace logic_opt

namespace {

void InitializeWebApp(ctrl_utils::RedisClient& redis_client, const spatial_dyn::ArticulatedBody& ab,
                      const std::map<std::string, logic_opt::Object3>& objects, size_t T) {
  
  // Register the urdf path so the server knows it's safe to fulfill requests for files in that directory
  std::string path_urdf = ctrl_utils::AbsolutePath(ctrl_utils::CurrentPath() + "/" + kPathUrdf);
  redis_client.hset(KEY_WEB_RESOURCES, kNameApp, ctrl_utils::ParentPath(path_urdf));

  // Register key prefixes so the web app knows which models and objects to render.
  nlohmann::json web_keys;
  web_keys["key_models_prefix"]       = KEY_MODELS_PREFIX;
  web_keys["key_objects_prefix"]      = KEY_OBJECT_MODELS_PREFIX;
  web_keys["key_trajectories_prefix"] = KEY_TRAJ_PREFIX;
  redis_client.set(KEY_WEB_ARGS, web_keys);

  // Register the robot
  nlohmann::json web_model;
  web_model["model"] = ab;
  web_model["key_q"] = KEY_SENSOR_Q;
  redis_client.set(KEY_MODELS_PREFIX + ab.name, web_model);

  // Create objects
  for (const std::pair<std::string, spatial_dyn::RigidBody>& key_val : objects) {
    const std::string& frame = key_val.first;
    if (frame == kEeFrame) continue;
    const spatial_dyn::RigidBody& object = key_val.second;
    
    nlohmann::json web_object;
    web_object["graphics"] = object.graphics;
    web_object["key_pos"] = KEY_OBJECTS_PREFIX + frame + "::pos";
    web_object["key_ori"] = KEY_OBJECTS_PREFIX + frame + "::ori";
    redis_client.set(KEY_OBJECT_MODELS_PREFIX + frame, web_object);
    redis_client.set(KEY_OBJECTS_PREFIX + frame + "::pos", object.T_to_parent().translation());
    redis_client.set(KEY_OBJECTS_PREFIX + frame + "::ori", Eigen::Quaterniond(object.T_to_parent().linear()).coeffs());
  }

  // Create a sphere marker for x_des
  nlohmann::json web_object;
  spatial_dyn::Graphics x_des_marker("x_des_marker");
  x_des_marker.geometry.type = spatial_dyn::Graphics::Geometry::Type::kSphere;
  x_des_marker.geometry.radius = 0.01;
  web_object["graphics"] = std::vector<spatial_dyn::Graphics>{ x_des_marker };
  web_object["key_pos"] = KEY_CONTROL_POS;
  redis_client.set(KEY_OBJECT_MODELS_PREFIX + x_des_marker.name, web_object);

  // Create a sphere marker for each frame
  // for (size_t t = 0; t < T; t++) {
  //   nlohmann::json web_object;
  //   spatial_dyn::Graphics frame_marker("frame_marker::" + std::to_string(t));
  //   frame_marker.geometry.type = spatial_dyn::Graphics::Geometry::Type::kSphere;
  //   frame_marker.geometry.radius = 0.01;
  //   web_object["graphics"] = std::vector<spatial_dyn::Graphics>{ frame_marker };
  //   web_object["key_pos"] = KEY_TRAJ_PREFIX + "frame::" + std::to_string(t) + "::pos";
  //   redis_client.set(KEY_OBJECT_MODELS_PREFIX + frame_marker.name, web_object);
  // }
}

std::pair<std::map<size_t, spatial_dyn::SpatialForced>, std::map<std::string, std::pair<Eigen::Vector3d, Eigen::Vector3d>>>
ComputeExternalForces(const spatial_dyn::ArticulatedBody& ab, const nlohmann::json& interaction) {

  // Check the clicked object
  const std::string key_object = interaction["key_object"].get<std::string>();
  if (key_object.empty()) return {};

  // Extract the json fields
  const size_t idx_link = interaction["idx_link"].get<size_t>();
  const Eigen::Vector3d pos_mouse = interaction["pos_mouse_in_world"].get<Eigen::Vector3d>();
  const Eigen::Vector3d pos_click = interaction["pos_click_in_link"].get<Eigen::Vector3d>();

  // Clicked object
  if (key_object != KEY_MODELS_PREFIX + ab.name) {
    if (key_object.size() <= KEY_OBJECT_MODELS_PREFIX.size() ||
        key_object.substr(0, KEY_OBJECT_MODELS_PREFIX.size()) != KEY_OBJECT_MODELS_PREFIX) return {};
    // Click position in object frame
    return {{}, {{key_object.substr(KEY_OBJECT_MODELS_PREFIX.length()), {pos_mouse, pos_click}}}};
  }

  // Get the click position in world coordinates
  const Eigen::Vector3d pos_click_in_world = spatial_dyn::Position(ab, idx_link, pos_click);

  // Set the click force
  const Eigen::Vector3d f = kGainClickDrag * (pos_mouse - pos_click_in_world);
  spatial_dyn::SpatialForced f_click(f, Eigen::Vector3d::Zero());

  // Translate the spatial force to the world frame
  f_click = Eigen::Translation3d(pos_click_in_world) * f_click;

  // Clicked robot link
  return {{{idx_link, f_click}}, {}};
}

void UpdateObjectStates(ctrl_utils::RedisClient& redis_client, const logic_opt::World3& world,
                        std::map<std::string, logic_opt::Object3>& sim_objects_abs,
                        size_t idx_trajectory, const Eigen::MatrixXd& X_optimal,
                        const Eigen::Isometry3d& T_ee_to_world,
                        const std::map<std::string, std::pair<Eigen::Vector3d, Eigen::Vector3d>>& f_ext) {
  const std::string& control_frame = world.controller_frames(idx_trajectory).first;

  const ctrl_utils::Tree<std::string, logic_opt::Frame> frame_tree = world.frames(idx_trajectory);
  if (frame_tree.is_ancestor(kEeFrame, control_frame)) {
    for (const auto& key_val : frame_tree.ancestors(control_frame)) {
      // Only check frames between control frame and ee
      const std::string& frame = key_val.first;
      if (frame == kEeFrame) break;
      const Eigen::Isometry3d T_to_ee = world.T_to_frame(frame, kEeFrame, X_optimal, idx_trajectory);
      spatial_dyn::RigidBody& rb = sim_objects_abs.at(frame);
      rb.set_T_to_parent(T_ee_to_world * T_to_ee);

      redis_client.set(KEY_OBJECTS_PREFIX + frame + "::pos", rb.T_to_parent().translation());
      redis_client.set(KEY_OBJECTS_PREFIX + frame + "::ori", Eigen::Quaterniond(rb.T_to_parent().linear()).coeffs());
    }
  } else if (frame_tree.is_ancestor(control_frame, kEeFrame)) {
    for (const auto& key_val : frame_tree.ancestors(*frame_tree.parent(kEeFrame))) {
      // Only check frames between control frame and ee
      const std::string& frame = key_val.first;
      const Eigen::Isometry3d T_to_ee = world.T_to_frame(frame, kEeFrame, X_optimal, idx_trajectory);
      spatial_dyn::RigidBody& rb = sim_objects_abs.at(frame);
      rb.set_T_to_parent(T_ee_to_world * T_to_ee);

      redis_client.set(KEY_OBJECTS_PREFIX + frame + "::pos", rb.T_to_parent().translation());
      redis_client.set(KEY_OBJECTS_PREFIX + frame + "::ori", Eigen::Quaterniond(rb.T_to_parent().linear()).coeffs());
      if (frame == control_frame) break;
    }
  }

  // Handle external forces
  for (const auto& key_val : f_ext) {
    const std::string& frame = key_val.first;
    const Eigen::Vector3d& pos_mouse_in_world = key_val.second.first;
    const Eigen::Vector3d& pos_click_in_link = key_val.second.second;
    if (sim_objects_abs.find(frame) == sim_objects_abs.end()) break;
    // Ignore forces applied to frames attached to the robot
    if (frame_tree.contains(frame) && frame_tree.is_descendant(frame, kEeFrame)) break;

    // Compute click force
    const spatial_dyn::RigidBody& rb = sim_objects_abs.at(frame);
    const Eigen::Vector3d pos_mouse_in_link = pos_mouse_in_world - rb.T_to_parent().translation();
    const Eigen::Vector3d f_click = 1e-5 * kGainClickDrag * (pos_mouse_in_link - pos_click_in_link);

    // Update position of all descendant frames
    for (const auto& key_val : frame_tree.values()) {
      if (!frame_tree.is_descendant(key_val.first, frame)) continue;
      spatial_dyn::RigidBody& rb = sim_objects_abs.at(key_val.first);

      Eigen::Isometry3d T_to_parent = rb.T_to_parent();
      T_to_parent.translation().head<2>() += f_click.head<2>();
      rb.set_T_to_parent(T_to_parent);
      redis_client.set(KEY_OBJECTS_PREFIX + key_val.first + "::pos", rb.T_to_parent().translation());
    }

  }
}

}  // namespace