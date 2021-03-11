## Kimera-DSG

Requirements (you likely have them):

```
sudo apt install python-rosdep python-wstool python-catkin-tools
# if you haven't yet:
# sudo rosdep init
# rosdep update
```

To get started (YMMV):

```
mkdir -p catkin_ws/src
cd catkin_ws
catkin init
catkin config -DCMAKE_BUILD_TYPE=Release -DGTSAM_TANGENT_PREINTEGRATION=OFF

wstool init -t src
wstool merge -t src https://github.mit.edu/SPARK/Kimera-DSG/blob/master/install/kimera_scene_graph.rosinstall
wstool up

rosdep install --from-paths src --ignore-src -r -y

catkin build
```

File any and all build errors as an issue
