#!/bin/bash

cgroup_name="siloz_service"

# TODO: may need to change this var based on your system
nr_hugepages="1536"

# get kernel version
kernel=$(uname -r)
if [[ $kernel == *"siloz"* ]]; then
  echo "detected siloz kernel: ${kernel}; reserving huge pages"
  # make a cgroup and confine this process's cpus to it
  mkdir -p /sys/fs/cgroup/${cgroup_name}
  num_nodes=$(lscpu | grep -i numa | head -n 1 |  awk '{print $NF}')
  last_node=$(( $num_nodes - 1 ))
  nodeset="0-1,6-${last_node}"
  # confine this process's mems
  echo "${nodeset}" | tee /sys/fs/cgroup/${cgroup_name}/cpuset.mems > /dev/null && cgclassify -g memory:${cgroup_name} $$
  i=6
  while [ "$i" -le "$last_node" ]
  do
          # requesting more than we need/can allocate per node will just fill up node
	  echo ${nr_hugepages} | tee /sys/devices/system/node/node${i}/hugepages/hugepages-2048kB/nr_hugepages > /dev/null
          i=$(( $i+1 ))
  done
  # make sure changes propagate before we disable stat updates for static nodes
  sleep 10
  echo "" | tee /dev/skx_edac > /dev/null
else
	echo "detected kernel: ${kernel}; NOT reserving huge pages"
fi
exit 0
