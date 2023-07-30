# Siloz: Leveraging DRAM Isolation Domains to Prevent Inter-VM Rowhammer

This is the source code for our SOSP 2023 paper "Siloz: Leveraging DRAM Isolation Domains to Prevent Inter-VM Rowhammer". The Siloz prototype is implemented as extensions to the Linux/KVM hypervisor. Specifically, we extend the kernel distributed in Ubuntu 22.04 (tag: [Ubuntu-5.15.0-43.46](https://git.launchpad.net/~ubuntu-kernel/ubuntu/+source/linux/+git/jammy/commit/?id=b8509484e0bab1dd935e504fa1b1f65a3866f0f6)). When using code from this repository, please be sure to cite [the paper](https://www.kevinloughlin.org/siloz.pdf).

NOTE: the Siloz prototype includes implementated support for running natively on dual-socket Intel Skylake and Intel Cascade Lake servers using (1) the physical-to-media address mapping yielded by the default (adaptive) page policy and (2) presumed subarray sizes of 512, 1024, and 2048 rows. Other configurations are not yet supported in the prototype, would require alternate physical-to-media address translation drivers, and may require additional code modifications described in the paper (e.g., offlining potential cross-boundary pages).

This `README.md` provides an overview of Siloz's implementation, as well as instructions on building and launching Siloz and a guest VM. For information specific to the SOSP 2023 Artifact Evaluation, please see [artifact_evaluation/README.md](https://github.com/efeslab/siloz/tree/main/artifact_evaluation#readme).

## Implementation Overview

At a high level, Siloz's modifications to the Linux/KVM kernel+hypervisor divide the physical memory space into logical NUMA nodes (each residing on a portion of a single physical NUMA node) that serve as DRAM isolation domains. Each logical node corresponds to 1 or more complete subarray groups, except for special EPT and guard row logical nodes that carve out a protected sub-region of a host-reserved subarray group on each physical node (i.e., socket). A logical node can be translated to its corresponding physical node (socket 0 or 1) via the function `node_to_pxm(logical_node_number)`. The breakdown of the logical-to-physical node mapping is as follows:

- Logical nodes 0 and 1 are host-reserved and map to socket 0 and 1, respectively.
- Logical nodes `SOCKET_0_EPT_NODE` and `SOCKET_1_EPT_NODE` are used for EPT pages. They are currently node numbers 2 and 4, respectively.
- Logical nodes `SOCKET_0_GUARD_ROWS_NODE` and `SOCKET_1_GUARD_ROWS_NODE` are used for guard row pages. They are currently node numbers 3 and 5, respectively.
- Logical nodes `[6,first_remote_group)` are guest-reserved and map to socket 0.
- Logical nodes `[first_remote_group,siloz_total_nodes)` are also guest-reserved and map to socket 1.

A per-directory summary of changes made to implement Siloz is listed below; we additionally note that running `git grep -il kevlough` from the top-level directory will reveal most/all modified files.

`include/linux/`:
- Declares new GFP (Get Free Page) flags for allocating EPT pages (`__GFP_EPT`), reserving huge pages for guests (`__GFP_GUEST_RESERVATION`), and allocating the reserved huge pages to guests (`__GFP_GUEST_RAM`).
- Declares new `mmap` flag (`MAP_GUEST`) to indicate that a host process (e.g., QEMU) wishes to use guest-reserved memory.
- Declares a new VM (virtual memory) flag to denote a guest-reserved region (`VM_GUEST_RAM`).
- Declares nodemasks to ease masking host-reserved logical nodes (`NODE_MASK_HOST`), EPT logical nodes (`NODE_MASK_EPT`), or both (`NODE_MASK_HOST_AND_EPT`).
- Declares a wide variety of Siloz constants, variables, and functions in `include/linux/skx_edac.h`.

`arch/x86/`:
- Repurposes `numa=` boot param to include number of rows per subarray.
- Finalizes logical node to physical node mapping.
- Allocates all EPT pages on demand (i.e., at guest initialization).
- Includes `__GFP_EPT` flag in call for EPT page allocations.

`drivers/acpi/`:
- Calculates address ranges for each logical node and creates the nodes.
- Determines logical node distances based on firmware-reported physical NUMA distances (i.e., same socket vs different socket).

`drivers/edac/`:
- Supports use of physical-to-media address translation drivers during early boot to establish logical node address ranges.
- Calculates subarray groups for physical addresses.

`init/`
- Restricts host kernel and `init` process to use host-reserved nodes by default; this policy is also enforced by systemd slice/scope files placed in `/lib/systemd/system/` on the root filesystem. These files are provided in this repository in `artifact_evaluation/lib/systemd/system/`.

`kernel/cgroup/`
- Manages cgroups to restrict allocations to specific logical nodes.

`mm/`
- Sets up zonelist orderings for different logical nodes.
- Implements `MAP_GUEST` backend for `mmap`.
- Allows full range of guest-reserved nodes to be allocated to guests as HugeTLB pages.
- Offlines guard row pages.
- Places `memmap` for each logical node on host-reserved nodes.
- Ensures host kernel allocations are placed on host-reserved nodes; when possible, allocations host allocations associated with a guest context are placed on the same socket.
- Ensures cgroup/cpuset logic supersedes mempolicy and node restrictions are enforced.
- Manages memory stats for different VM regions.

`virt/kvm/`
- Uses `__GFP_EPT` flag for EPT allocation requests.

## Installing Dependencies

On the target machine, the following dependencies and tools should be installed to build, run, and manage Siloz:

`sudo apt install wget build-essential bison flex libncurses-dev libssl-dev libelf-dev libx11-dev x11proto-xext-dev libxext-dev libxt-dev libxmu-dev libxi-dev qemu-kvm virt-manager libvirt-daemon-system virtinst libvirt-clients bridge-utils numactl cpu-checker cgroup-tools`

More information on Linux kernel dependencies and possible issues is available [here](https://wiki.ubuntu.com/Kernel/BuildYourOwnKernel).

The following dependencies and tools should be installed build Siloz's custom version of QEMU:

`sudo apt install git libglib2.0-dev libfdt-dev libpixman-1-dev zlib1g-dev ninja-build`

More information on QEMU dependencies and possible issues is available [here](https://wiki.qemu.org/Hosts/Linux).

## Building and Installing Siloz

These instructions assume that Siloz is being built in Ubuntu 22.04 on the target server. See `README` (in contrast to this file) for the original Linux kernel README. Note that the build `.config` must include `CONFIG_EDAC_SKX=y` for built-in access to address translation drivers during early boot; the `.config` file provided in this repository is already configured accordingly.

First, clone this repository using `git clone git@github.com:efeslab/siloz.git`. `cd` into the created `siloz` top-level directory; all paths herein are relative to the `siloz` top-level directory unless otherwise noted.

To build Siloz (assuming dependencies are installed), run

`make -j$(nproc)`

Assuming the build environment described in these instructions, all build options should already be appropriately congigured; nonetheless, if you are prompted to configure additional options, the default selections should work, assuming you have not changed the provided `.config`.

To then install Siloz's modules and kernel, run

`sudo make -j$(nproc) modules_install INSTALL_MOD_STRIP=1 && sudo make install`

To additionally ensure that the host is restricted to logical nodes 0 and 1, backup the contents of `/lib/systemd/system/` on the target machine for easy reversion and then execute the following to copy systemd slice/scope files into the root filesystem

`sudo cp -- artifact_evaluation/lib/systemd/system/* /lib/systemd/system`

Then, copy `reserve_hugepages.sh` and `siloz.service` into place and enable the service in order to reserve guest huge pages upon boot:

`sudo cp artifact_evaluation/reserve_hugepages.sh /root; sudo cp artifact_evaluation/siloz.service /etc/systemd/system; sudo systemctl enable siloz`

Please note that the service assumes that the server boots in `multi-user` mode. The service additionally checks for the Siloz kernel and does not reserve huge pages if Siloz is not detected. If you wish to disable the service when booting a different kernel, you may do so via `sudo systemctl disable siloz`.

## Building and Installing Modified QEMU

Siloz uses a slightly-modified version of QEMU 6.2, the version distributed with Ubuntu 22.04. The modifications ensure that QEMU's `mmap` calls for unmediated memory use the `MAP_GUEST` flag and also support a greater number of (logical) NUMA nodes than QEMU's default via the use of `uint16_t` instead of `uint8_t`.

Assuming QEMU dependencies are installed, to build QEMU, first update the `qemu-siloz` submodule using `git submodule update qemu-siloz`. Then, `cd` into `qemu-siloz` and run

`./configure --target-list=x86_64-softmmu --enable-debug`

followed by (still in the `qemu-siloz` directory)

`make -j$(nproc)`

You can then install the built version of `qemu` by running `sudo make install` from the `qemu-siloz` directory, which will install this version of QEMU at `/usr/local/bin/qemu-system-x86_64`. You may need to adjust your `$PATH` variable to use this custom version of QEMU over others installed on your system, as well as modify or disable AppArmor (if enabled) to allow your QEMU binaries to execute.

## Creating a VM

The XML for an example virsh domain named `jammy2` is provided in `artifact_evaluation/jammy2.xml`. You will need to modify this script to be consistent with your server's core enumeration, RAM sizing, and path to your desired disk image. Once appropriately modified, you can define your domain using `virsh define jammy2.xml`.

## Booting Siloz

Ensure that your system has KVM support enabled in BIOS via `sudo kvm-ok`. Once enabled, on the target server, modify the line containing `GRUB_CMDLINE_LINUX_DEFAULT` in `/etc/default/grub` to the following

`GRUB_CMDLINE_LINUX_DEFAULT="numa=1024 kvm.nx_huge_pages=off"`

`numa=1024` tells Siloz that subarrays have 1024 rows per subarray; it is ignored by the baseline kernel. The Siloz prototype also supports booting with 512 rows per subarray and 2048 rows per subarray. `kvm.nx_huge_pages=off` ensures that KVM's executable huge pages are not broken into 4 KB pages due to the possible presence of [iTLB multihit](https://www.tacitosecurity.com/multihit.html) on both the baseline and in Siloz.

At this point, set grub to boot the Siloz kernel upon the next reboot with `sudo grub-reboot "Advanced options for Ubuntu>Ubuntu, with Linux 5.15.0-47-generic-siloz+"`. Then, update grub with `sudo update-grub`. At this point, executing `sudo reboot` will bring up the Siloz kernel upon reboot.

Please note that if your server configuration differs from Siloz's evaluation server in number of cores, core enumeration, total RAM, or memory geometry, you may need to update the number of reserved hugepages in `reserve_hugepages.sh` to match your desired configuration. You can grep the Siloz-specific information (e.g., `first_remote_group` and `siloz_total_nodes`) from kernel debug messages using `sudo dmesg | grep "ACPI: kevlough: slit_init"` after booting Siloz for the first time.

## Launching a VM in Siloz

After Siloz has booted, ensure that `siloz.service` has finished reserving hugepages by running `sudo systemctl status siloz`. Once this query returns that the service has deactivated, you may proceed to booting a VM.

An example script to launch the above-referenced VM named `jammy2` is at `artifact_evaluation/example_launch_vm.sh`. As with `reserve_hugepages.sh`, you will need to update the VM's cores and RAM configuration if your server differs from Siloz's evaluation server in these parameters. Please note that the example script uses virtio for IO virtualization.
