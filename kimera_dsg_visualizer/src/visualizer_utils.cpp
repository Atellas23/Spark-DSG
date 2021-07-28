#include "kimera_dsg_visualizer/visualizer_utils.h"
#include "kimera_dsg_visualizer/colormap_utils.h"

#include <kimera_dsg/scene_graph_layer.h>
#include <tf2_eigen/tf2_eigen.h>
#include <opencv2/imgproc.hpp>

namespace kimera {

using visualization_msgs::Marker;
using visualization_msgs::MarkerArray;
using Node = SceneGraphLayer::Node;
using dsg_utils::makeColorMsg;

namespace {

inline double getRatio(double min, double max, double value) {
  double ratio = (value - min) / (max - min);
  ratio = !std::isfinite(ratio) ? 0.0 : ratio;
  ratio = ratio > 1.0 ? 1.0 : ratio;
  ratio = ratio < 0.0 ? 0.0 : ratio;
  return ratio;
}

inline NodeColor getDistanceColor(const VisualizerConfig& config, double distance) {
  if (config.places_max_distance <= config.places_min_distance) {
    // TODO(nathan) consider warning
    return NodeColor::Zero();
  }

  dsg_utils::HlsColorMapConfig hls_config{config.places_min_hue,
                                          config.places_max_hue,
                                          config.places_min_saturation,
                                          config.places_max_saturation,
                                          config.places_min_luminance,
                                          config.places_max_luminance};

  double ratio =
      getRatio(config.places_min_distance, config.places_max_distance, distance);

  return dsg_utils::interpolateColorMap(hls_config, ratio);
}

inline void fillPoseWithIdentity(geometry_msgs::Pose& pose) {
  Eigen::Vector3d identity_pos = Eigen::Vector3d::Zero();
  tf2::convert(identity_pos, pose.position);
  tf2::convert(Eigen::Quaterniond::Identity(), pose.orientation);
}

}  // namespace

Marker makeBoundingBoxMarker(const LayerConfig& config,
                             const Node& node,
                             const VisualizerConfig& visualizer_config,
                             const std::string& marker_namespace) {
  Marker marker;
  marker.type = Marker::CUBE;
  marker.action = Marker::ADD;
  marker.id = node.id;
  marker.ns = marker_namespace;
  marker.color = makeColorMsg(node.attributes<SemanticNodeAttributes>().color,
                              config.bounding_box_alpha);

  BoundingBox bounding_box = node.attributes<ObjectNodeAttributes>().bounding_box;

  switch (bounding_box.type) {
    case BoundingBox::Type::OBB:
      marker.pose.position =
          tf2::toMsg(bounding_box.world_P_center.cast<double>().eval());
      tf2::convert(bounding_box.world_R_center.cast<double>(), marker.pose.orientation);
      marker.pose.position.z += getZOffset(config, visualizer_config);
      break;
    case BoundingBox::Type::AABB:
      marker.pose.position =
          tf2::toMsg(bounding_box.world_P_center.cast<double>().eval());
      tf2::convert(Eigen::Quaterniond::Identity(), marker.pose.orientation);
      marker.pose.position.z += getZOffset(config, visualizer_config);
      break;
    default:
      ROS_ERROR("Invalid bounding box encountered!");
      break;
  }

  tf2::toMsg((bounding_box.max - bounding_box.min).cast<double>().eval(), marker.scale);
  return marker;
}

Marker makeTextMarker(const LayerConfig& config,
                      const Node& node,
                      const VisualizerConfig& visualizer_config,
                      const std::string& marker_namespace) {
  Marker marker;
  marker.ns = marker_namespace;
  marker.id = node.id;
  marker.type = Marker::TEXT_VIEW_FACING;
  marker.action = Marker::ADD;
  marker.lifetime = ros::Duration(0);
  marker.text = NodeSymbol(node.id).getLabel();
  marker.scale.z = config.label_scale;
  marker.color = makeColorMsg(NodeColor::Zero());

  fillPoseWithIdentity(marker.pose);
  tf2::convert(node.attributes().position, marker.pose.position);
  marker.pose.position.z += getZOffset(config, visualizer_config) + config.label_height;

  return marker;
}

Marker makeCentroidMarkers(const LayerConfig& config,
                           const SceneGraphLayer& layer,
                           const VisualizerConfig& visualizer_config,
                           std::optional<NodeColor> layer_color,
                           const std::string& marker_namespace) {
  Marker marker;
  marker.type = config.use_sphere_marker ? Marker::SPHERE_LIST : Marker::CUBE_LIST;
  marker.action = visualization_msgs::Marker::ADD;
  marker.id = layer.id;
  marker.ns = marker_namespace;

  marker.scale.x = config.marker_scale;
  marker.scale.y = config.marker_scale;
  marker.scale.z = config.marker_scale;

  fillPoseWithIdentity(marker.pose);

  bool node_colors_valid = true;
  marker.points.reserve(layer.numNodes());
  marker.colors.reserve(layer.numNodes());
  for (const auto& id_node_pair : layer.nodes()) {
    geometry_msgs::Point node_centroid;
    tf2::convert(id_node_pair.second->attributes().position, node_centroid);
    node_centroid.z += getZOffset(config, visualizer_config);
    marker.points.push_back(node_centroid);

    // get the color of the node
    // TODO(nathan) refactor or pull out into function
    NodeColor desired_color;
    if (layer_color) {
      desired_color = *layer_color;
    } else if (!node_colors_valid) {
      desired_color << 1.0, 0.0, 0.0;
    } else if (visualizer_config.color_places_by_distance &&
               layer.id == to_underlying(KimeraDsgLayers::PLACES)) {
      desired_color = getDistanceColor(
          visualizer_config,
          id_node_pair.second->attributes<PlaceNodeAttributes>().distance);
    } else {
      try {
        desired_color = id_node_pair.second->attributes<SemanticNodeAttributes>().color;
      } catch (const std::bad_cast) {
        node_colors_valid = false;
        desired_color << 1.0, 0.0, 0.0;
      }
    }

    marker.colors.push_back(makeColorMsg(desired_color, config.marker_alpha));
  }

  return marker;
}

namespace {

inline Marker makeNewEdgeList(const LayerConfig& config, LayerId layer_id) {
  Marker marker;
  marker.type = Marker::LINE_LIST;
  if (config.visualize) {
    marker.action = Marker::ADD;
  } else {
    marker.action = Marker::DELETE;
  }
  marker.id = layer_id;
  marker.ns = "graph_edges";
  marker.scale.x = config.interlayer_edge_scale;
  fillPoseWithIdentity(marker.pose);
  return marker;
}

}  // namespace

MarkerArray makeGraphEdgeMarkers(const SceneGraph& graph,
                                 const std::map<LayerId, LayerConfig>& configs,
                                 const VisualizerConfig& visualizer_config) {
  MarkerArray layer_edges;
  std::map<LayerId, Marker> layer_markers;
  std::map<LayerId, size_t> num_since_last_insertion;

  for (const auto& edge : graph.inter_layer_edges()) {
    const Node& source = *(graph.getNode(edge.second.source));
    const Node& target = *(graph.getNode(edge.second.target));

    // parent is always source
    // TODO(nathan) make the above statement an invariant
    if (layer_markers.count(source.layer) == 0) {
      layer_markers[source.layer] =
          makeNewEdgeList(configs.at(source.layer), source.layer);
      if (!configs.at(target.layer).visualize) {
        // TODO(nathan) this assumes only adjacent layer edges
        layer_markers[source.layer].action = Marker::DELETE;
      }
      num_since_last_insertion[source.layer] = 0;
    }

    if (!configs.at(source.layer).visualize) {
      continue;
    }

    if (!configs.at(target.layer).visualize) {
      continue;
    }

    size_t num_between_insertions =
        configs.at(source.layer).interlayer_edge_insertion_skip;
    if (num_since_last_insertion[source.layer] >= num_between_insertions) {
      num_since_last_insertion[source.layer] = 0;
    } else {
      num_since_last_insertion[source.layer]++;
      continue;
    }

    Marker& marker = layer_markers.at(source.layer);
    geometry_msgs::Point source_point;
    tf2::convert(source.attributes().position, source_point);
    source_point.z += getZOffset(configs.at(source.layer), visualizer_config);
    marker.points.push_back(source_point);

    geometry_msgs::Point target_point;
    tf2::convert(target.attributes().position, target_point);
    target_point.z += getZOffset(configs.at(target.layer), visualizer_config);
    marker.points.push_back(target_point);

    NodeColor edge_color;
    if (configs.at(source.layer).interlayer_edge_use_color) {
      if (configs.at(source.layer).use_edge_source) {
        // TODO(nathan) this might not be a safe cast in general
        edge_color = source.attributes<SemanticNodeAttributes>().color;
      } else {
        // TODO(nathan) this might not be a safe cast in general
        edge_color = target.attributes<SemanticNodeAttributes>().color;
      }
    } else {
      edge_color = NodeColor::Zero();
    }

    marker.colors.push_back(
        makeColorMsg(edge_color, configs.at(source.layer).intralayer_edge_alpha));
    marker.colors.push_back(
        makeColorMsg(edge_color, configs.at(source.layer).intralayer_edge_alpha));
  }

  for (const auto& id_marker_pair : layer_markers) {
    layer_edges.markers.push_back(id_marker_pair.second);
  }
  return layer_edges;
}

Marker makeMeshEdgesMarker(const LayerConfig& config,
                           const VisualizerConfig& visualizer_config,
                           const DynamicSceneGraph& graph,
                           const SceneGraphLayer& layer,
                           const std::string& marker_namespace) {
  Marker marker;
  marker.type = Marker::LINE_LIST;
  marker.action = Marker::ADD;
  marker.id = layer.id;
  marker.ns = marker_namespace;

  marker.scale.x = config.interlayer_edge_scale;
  fillPoseWithIdentity(marker.pose);

  for (const auto& id_node_pair : layer.nodes()) {
    const Node& node = *id_node_pair.second;
    auto mesh_points = graph.getMeshCloudForNode(node.id);
    if (mesh_points == nullptr || mesh_points->size() == 0) {
      continue;
    }

    SemanticNodeAttributes attrs = node.attributes<SemanticNodeAttributes>();

    geometry_msgs::Point center_point;
    tf2::convert(attrs.position, center_point);
    center_point.z +=
        visualizer_config.mesh_edge_break_ratio * getZOffset(config, visualizer_config);

    geometry_msgs::Point centroid_location;
    tf2::convert(attrs.position, centroid_location);
    centroid_location.z += getZOffset(config, visualizer_config);

    // make first edge
    marker.points.push_back(centroid_location);
    marker.points.push_back(center_point);
    if (config.interlayer_edge_use_color) {
      marker.colors.push_back(makeColorMsg(attrs.color, config.interlayer_edge_alpha));
      marker.colors.push_back(makeColorMsg(attrs.color, config.interlayer_edge_alpha));
    } else {
      marker.colors.push_back(
          makeColorMsg(NodeColor::Zero(), config.interlayer_edge_alpha));
      marker.colors.push_back(
          makeColorMsg(NodeColor::Zero(), config.interlayer_edge_alpha));
    }

    for (size_t i = 0; i < mesh_points->size();
         i += config.interlayer_edge_insertion_skip + 1) {
      geometry_msgs::Point vertex;
      vertex.x = mesh_points->at(i).x;
      vertex.y = mesh_points->at(i).y;
      vertex.z = mesh_points->at(i).z;
      if (!visualizer_config.collapse_layers) {
        vertex.z += visualizer_config.mesh_layer_offset;
      }

      marker.points.push_back(center_point);
      marker.points.push_back(vertex);

      if (config.interlayer_edge_use_color) {
        marker.colors.push_back(
            makeColorMsg(attrs.color, config.interlayer_edge_alpha));
        marker.colors.push_back(
            makeColorMsg(attrs.color, config.interlayer_edge_alpha));
      } else {
        marker.colors.push_back(
            makeColorMsg(NodeColor::Zero(), config.interlayer_edge_alpha));
        marker.colors.push_back(
            makeColorMsg(NodeColor::Zero(), config.interlayer_edge_alpha));
      }
    }
  }

  return marker;
}

Marker makeLayerEdgeMarkers(const LayerConfig& config,
                            const SceneGraphLayer& layer,
                            const VisualizerConfig& visualizer_config,
                            const NodeColor& color) {
  Marker marker;
  marker.type = Marker::LINE_LIST;
  marker.id = 0;
  marker.ns = "layer_" + std::to_string(layer.id) + "_edges";

  if (!config.visualize) {
    marker.action = Marker::DELETE;
    return marker;
  }

  marker.action = Marker::ADD;
  marker.scale.x = config.intralayer_edge_scale;
  marker.color = makeColorMsg(color, config.intralayer_edge_alpha);
  fillPoseWithIdentity(marker.pose);

  auto edge_iter = layer.edges().begin();
  while (edge_iter != layer.edges().end()) {
    geometry_msgs::Point source;
    tf2::convert(layer.getPosition(edge_iter->second.source), source);
    source.z += getZOffset(config, visualizer_config);
    marker.points.push_back(source);

    geometry_msgs::Point target;
    tf2::convert(layer.getPosition(edge_iter->second.target), target);
    target.z += getZOffset(config, visualizer_config);
    marker.points.push_back(target);

    std::advance(edge_iter, config.intralayer_edge_insertion_skip + 1);
  }

  return marker;
}

}  // namespace kimera
