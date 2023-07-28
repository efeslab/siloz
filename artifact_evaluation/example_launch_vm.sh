#!/bin/bash

# TODO: these vars may need to change based on your system cores, RAM, and VM name
socket_0_cpus="0-19,40-59"
cgroup_name="siloz_vm_00"
nr_hugepages_siloz="768"
nr_hugepages_baseline="81920"
upper_node_num=120
vm_name="jammy2"

# make a cgroup and confine this process's cpus to it
sudo mkdir -p /sys/fs/cgroup/${cgroup_name} && sleep 1 && echo "${socket_0_cpus}" | sudo tee /sys/fs/cgroup/${cgroup_name}/cpuset.cpus && sudo cgclassify -g cpu:${cgroup_name} $$

# get kernel version
kernel=$(uname -r)

# determine nodeset and alloc hugepages to specific nodes based on kernel
nodeset="0-1"
if [[ $kernel == *"siloz"* ]]; then
  echo "detected subarray kernel: ${kernel}"
  
  nodeset="0-1,2,6-${upper_node_num}"
  
  # confine this process's mems
  echo "${nodeset}" | sudo tee /sys/fs/cgroup/${cgroup_name}/cpuset.mems && sudo cgclassify -g memory:${cgroup_name} $$
  for (( i=6; i<=$upper_node_num; i++ ))
  do
	  echo ${nr_hugepages_siloz} |  sudo tee /sys/devices/system/node/node${i}/hugepages/hugepages-2048kB/nr_hugepages > /dev/null
  done
else
	echo "detected baseline kernel: ${kernel}"
	echo "${nodeset}" | sudo tee /sys/fs/cgroup/${cgroup_name}/cpuset.mems && sudo cgclassify -g memory:${cgroup_name} $$
	echo ${nr_hugepages_baseline} |  sudo tee /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages > /dev/null
fi

# make VM config match kernel version
virsh numatune ${vm_name} --nodeset "${nodeset}"

# start VM
virsh start ${vm_name}
