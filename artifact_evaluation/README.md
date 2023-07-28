# Artifact Evaluation

This `README.md` provides instructions for the SOSP 2023 Artifact Evaluation Program Committee on how to verify the availability and functionality of the Siloz prototype.

Please consult the bidding instructions, which contain information on how to obtain credentials for accessing the host and the guest VM on the evaluation server. For the rest of this document, we will give examples with host username `hostuser`, fully-qualified host name `server`, guest username `guestuser`, and guest machine name `vm`.

After obtaining credentials, please verify that you can access the evaluation machine via SSH, replacing `hostuser` and `server` in `ssh -A hostuser@server` with the host credentials that you have been provided. Note that we have already booted the Siloz hypervisor and a guest VM atop Siloz, as these require `root` permission.

Once on the host machine, you can verify that the host is running the Siloz kernel with `uname -r`, which should return `5.15.0-47-generic-siloz+`. You can also confirm that the system is aware of Siloz's logical NUMA nodes via `numactl --show`, which will show all of Siloz's logical nodes as options for `membind`. You can see more information on the memory in some logical node `X` using `cat /sys/devices/system/node/nodeX/meminfo`, replacing `X` with the number of your node. Per the node mapping provided in the top-level directory's `README.md`, nodes numbered 6 and higher are guest-reserved, filled entirely with hugepages (`HugePages_Total`). Because the running VM is using huge pages from nodes [6, 114], you can verify that there are few-if-any `HugePages_Free` on these nodes via the node's `meminfo`. In contrast, all huge pages remain free on higher-numbered nodes.

Assuming you included the `-A` option when `ssh`-ing into the host, you can `ssh` into the guest from the host without a password using `ssh guestuser@vm`, replacing `guestuser` and `vm` with the credentials from the bidding instructions. Inside the guest, you can verify the VM's number of cores are consistent with the paper's evaluation parameters using `nproc` and that the DRAM quantity is also as reported using `free -g`. Note that the DRAM quantitied reported in the VM may be slightly less than the 160 GB allocated to the VM (per `example_launch_vm.sh` in this directory) due to reserved memory.

The primary script that we used to run profiling is in the VM at `~/git/subarray-benchmarking/profile.sh`. However, executing this script takes many hours to run and requires `root` privileges. A quick (< 5 minutes) benchmark example that can instead be run from the guest command line to verify functionality is `~/mlc_v3.9a/Linux/mlc --max_bandwidth`.

When you have finished your exploration of the guest VM, you can exit the guest ssh session with `exit`, which will return you to the Siloz host. The same command can be used to exit the ssh session on the host.

If you have any questions, please contact the corresponding author listed in HotCRP.