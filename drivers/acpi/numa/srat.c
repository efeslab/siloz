// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  acpi_numa.c - ACPI NUMA support
 *
 *  Copyright (C) 2002 Takayoshi Kochi <t-kochi@bq.jp.nec.com>
 */

#define pr_fmt(fmt) "ACPI: " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/acpi.h>
#include <linux/memblock.h>
#include <linux/numa.h>
#include <linux/nodemask.h>
#include <linux/topology.h>
#include <linux/skx_common.h>
#include <asm/numa.h>

static nodemask_t nodes_found_map = NODE_MASK_NONE;

/* maps to convert between proximity domain and logical node ID */
static int pxm_to_node_map[MAX_PXM_DOMAINS]
			= { [0 ... MAX_PXM_DOMAINS - 1] = NUMA_NO_NODE };
static int node_to_pxm_map[MAX_NUMNODES]
			= { [0 ... MAX_NUMNODES - 1] = PXM_INVAL };

unsigned char acpi_srat_revision __initdata;
static int acpi_numa __initdata;

void __init disable_srat(void)
{
	acpi_numa = -1;
}

int pxm_to_node(int pxm)
{
	if (pxm < 0 || pxm >= MAX_PXM_DOMAINS || numa_off)
		return NUMA_NO_NODE;
	return pxm_to_node_map[pxm];
}
EXPORT_SYMBOL(pxm_to_node);

/**
 * kevlough: we use this function to translate logical->physical node.
 * In our current support for dual socket machines, physical node 0
 * is socket 0, and physical node 1 is socket 1. Logical nodes 0 and 1
 * are host-reserved and map to regions on socket 0 and socket 1, respectively.
 * 
 * Preserving logical nodes 0 and 1 as host-reserved allows for easier integration
 * with existing NUMA code.
 */
int node_to_pxm(int node)
{
	if (node < 0)
		return PXM_INVAL;
	return node_to_pxm_map[node];
}

static void __acpi_map_pxm_to_node(int pxm, int node)
{
	if (pxm_to_node_map[pxm] == NUMA_NO_NODE || node < pxm_to_node_map[pxm])
		pxm_to_node_map[pxm] = node;
	if (node_to_pxm_map[node] == PXM_INVAL || pxm < node_to_pxm_map[node])
		node_to_pxm_map[node] = pxm;
}

int acpi_map_pxm_to_node(int pxm)
{
	int node;

	if (pxm < 0 || pxm >= MAX_PXM_DOMAINS || numa_off)
		return NUMA_NO_NODE;

	node = pxm_to_node_map[pxm];

	if (node == NUMA_NO_NODE) {
		if (nodes_weight(nodes_found_map) >= MAX_NUMNODES)
			return NUMA_NO_NODE;
		node = first_unset_node(nodes_found_map);
		__acpi_map_pxm_to_node(pxm, node);
		node_set(node, nodes_found_map);
	}

	return node;
}
EXPORT_SYMBOL(acpi_map_pxm_to_node);

static void __init
acpi_table_print_srat_entry(struct acpi_subtable_header *header)
{
	switch (header->type) {
	case ACPI_SRAT_TYPE_CPU_AFFINITY:
		{
			struct acpi_srat_cpu_affinity *p =
			    (struct acpi_srat_cpu_affinity *)header;
			pr_debug("SRAT Processor (id[0x%02x] eid[0x%02x]) in proximity domain %d %s\n",
				 p->apic_id, p->local_sapic_eid,
				 p->proximity_domain_lo,
				 (p->flags & ACPI_SRAT_CPU_ENABLED) ?
				 "enabled" : "disabled");
		}
		break;

	case ACPI_SRAT_TYPE_MEMORY_AFFINITY:
		{
			struct acpi_srat_mem_affinity *p =
			    (struct acpi_srat_mem_affinity *)header;
			pr_debug("SRAT Memory (0x%llx length 0x%llx) in proximity domain %d %s%s%s\n",
				 (unsigned long long)p->base_address,
				 (unsigned long long)p->length,
				 p->proximity_domain,
				 (p->flags & ACPI_SRAT_MEM_ENABLED) ?
				 "enabled" : "disabled",
				 (p->flags & ACPI_SRAT_MEM_HOT_PLUGGABLE) ?
				 " hot-pluggable" : "",
				 (p->flags & ACPI_SRAT_MEM_NON_VOLATILE) ?
				 " non-volatile" : "");
		}
		break;

	case ACPI_SRAT_TYPE_X2APIC_CPU_AFFINITY:
		{
			struct acpi_srat_x2apic_cpu_affinity *p =
			    (struct acpi_srat_x2apic_cpu_affinity *)header;
			pr_debug("SRAT Processor (x2apicid[0x%08x]) in proximity domain %d %s\n",
				 p->apic_id,
				 p->proximity_domain,
				 (p->flags & ACPI_SRAT_CPU_ENABLED) ?
				 "enabled" : "disabled");
		}
		break;

	case ACPI_SRAT_TYPE_GICC_AFFINITY:
		{
			struct acpi_srat_gicc_affinity *p =
			    (struct acpi_srat_gicc_affinity *)header;
			pr_debug("SRAT Processor (acpi id[0x%04x]) in proximity domain %d %s\n",
				 p->acpi_processor_uid,
				 p->proximity_domain,
				 (p->flags & ACPI_SRAT_GICC_ENABLED) ?
				 "enabled" : "disabled");
		}
		break;

	case ACPI_SRAT_TYPE_GENERIC_AFFINITY:
	{
		struct acpi_srat_generic_affinity *p =
			(struct acpi_srat_generic_affinity *)header;

		if (p->device_handle_type == 0) {
			/*
			 * For pci devices this may be the only place they
			 * are assigned a proximity domain
			 */
			pr_debug("SRAT Generic Initiator(Seg:%u BDF:%u) in proximity domain %d %s\n",
				 *(u16 *)(&p->device_handle[0]),
				 *(u16 *)(&p->device_handle[2]),
				 p->proximity_domain,
				 (p->flags & ACPI_SRAT_GENERIC_AFFINITY_ENABLED) ?
				"enabled" : "disabled");
		} else {
			/*
			 * In this case we can rely on the device having a
			 * proximity domain reference
			 */
			pr_debug("SRAT Generic Initiator(HID=%.8s UID=%.4s) in proximity domain %d %s\n",
				(char *)(&p->device_handle[0]),
				(char *)(&p->device_handle[8]),
				p->proximity_domain,
				(p->flags & ACPI_SRAT_GENERIC_AFFINITY_ENABLED) ?
				"enabled" : "disabled");
		}
	}
	break;
	default:
		pr_warn("Found unsupported SRAT entry (type = 0x%x)\n",
			header->type);
		break;
	}
}

/*
 * A lot of BIOS fill in 10 (= no distance) everywhere. This messes
 * up the NUMA heuristics which wants the local node to have a smaller
 * distance than the others.
 * Do some quick checks here and only use the SLIT if it passes.
 */
static int __init slit_valid(struct acpi_table_slit *slit)
{
	int i, j;
	int d = slit->locality_count;
	for (i = 0; i < d; i++) {
		for (j = 0; j < d; j++)  {
			u8 val = slit->entry[d*i + j];
			if (i == j) {
				if (val != LOCAL_DISTANCE)
					return 0;
			} else if (val <= LOCAL_DISTANCE)
				return 0;
		}
	}
	return 1;
}

void __init bad_srat(void)
{
	pr_err("SRAT: SRAT not used.\n");
	disable_srat();
}

int __init srat_disabled(void)
{
	return acpi_numa < 0;
}

#if defined(CONFIG_X86) || defined(CONFIG_ARM64) || defined(CONFIG_LOONGARCH)
static int siloz_total_nodes = 0;
static int local_distance = 0;
static int remote_distance = 0;
static unsigned int first_remote_group = -1;
/*
 * Callback for SLIT parsing.  pxm_to_node() returns NUMA_NO_NODE for
 * I/O localities since SRAT does not list them.  I/O localities are
 * not supported at this point.
 */
void __init acpi_numa_slit_init(struct acpi_table_slit *slit)
{
	int i, j;
	int from_node;
	int to_node;

	pr_info("kevlough: slit_init. locality_count=%llu, first_remote_group=%u, siloz_total_nodes=%d\n",
			slit->locality_count, first_remote_group, siloz_total_nodes);

	// kevlough: replace actual SLIT table (physical node distances) with Siloz logical node
	// distances. Preserve physical semantics.
	if (first_remote_group == (unsigned int) -1) {
		// if we don't have remote physical nodes, every logical node is local_distance.
		for (i = 0; i < siloz_total_nodes; i++) {
			for (j = 0; j < siloz_total_nodes; j++) {
				numa_set_distance(i, j, local_distance);
			}
		}
	} else {
		for (i = 0; i < siloz_total_nodes; i++) {
			if (i < slit->locality_count) {
				from_node = pxm_to_node(i);
			} else {
				from_node = i;
			}
			if (from_node == NUMA_NO_NODE)
				continue;

			for (j = 0; j < siloz_total_nodes; j++) {
				if (j < slit->locality_count) {
					to_node = pxm_to_node(j);
				} else {
					to_node = j;
				}

				if (to_node == NUMA_NO_NODE)
					continue;

				if (i < slit->locality_count && j < slit->locality_count) {
					if (i == j) {
						local_distance = slit->entry[slit->locality_count * i + j];
					} else {
						remote_distance = slit->entry[slit->locality_count * i + j];
					}
					numa_set_distance(from_node, to_node,
						slit->entry[slit->locality_count * i + j]);
				} else if (i == j) {
					numa_set_distance(from_node, to_node, local_distance);
				} else {
					// kevlough: special cases (e.g., reserved nodes, EPT npdes, and nodes 0 and 1)
					// are handled outside of simple loop logic to simplify integration with
					// physical NUMA semantics.
					if (i == SOCKET_0_GUARD_ROWS_NODE || i == SOCKET_1_GUARD_ROWS_NODE || j == SOCKET_0_GUARD_ROWS_NODE || j == SOCKET_1_GUARD_ROWS_NODE) {
						numa_set_distance(from_node, to_node, (1 << DISTANCE_BITS) - 1);
					} else if (i == 1 || i == SOCKET_1_EPT_NODE) {
						if (j < first_remote_group || j == siloz_total_nodes - 1 || j == SOCKET_0_EPT_NODE) {
							numa_set_distance(from_node, to_node, remote_distance);
						} else {
							numa_set_distance(from_node, to_node, local_distance);
						}
					} else if (j == 1 || j == SOCKET_1_EPT_NODE) {
						if (i < first_remote_group || i == siloz_total_nodes - 1 || i == SOCKET_0_EPT_NODE) {
							numa_set_distance(from_node, to_node, remote_distance);
						} else {
							numa_set_distance(from_node, to_node, local_distance);
						}
					} else if (i == siloz_total_nodes - 1 || i == SOCKET_0_EPT_NODE) {
						if ((j < siloz_total_nodes - 1 && j >= first_remote_group) || j == 1 || j == SOCKET_1_EPT_NODE) {
							numa_set_distance(from_node, to_node, remote_distance);
						} else {
							numa_set_distance(from_node, to_node, local_distance);
						}
					} else if (j == siloz_total_nodes - 1 || j == SOCKET_0_EPT_NODE || j == SOCKET_0_GUARD_ROWS_NODE) {
						if ((i < siloz_total_nodes - 1 && i >= first_remote_group) || i == 1 || i == SOCKET_1_EPT_NODE || i == SOCKET_1_GUARD_ROWS_NODE) {
							numa_set_distance(from_node, to_node, remote_distance);
						} else {
							numa_set_distance(from_node, to_node, local_distance);
						}
					} else if ((i < first_remote_group && j <= first_remote_group)
					|| (i > first_remote_group && j > first_remote_group)) {
						numa_set_distance(from_node, to_node, local_distance);
					} else {
						numa_set_distance(from_node, to_node, remote_distance);
					}
				}
			}
		}
	}
}

static int __initdata parsed_numa_memblks;

// kevlough: start addr of first subarray group above 4 GB phys addr
// helps manage x86-64 memory "holes" below 4 GB
static u64 himem_subarray_boundary_addr = 0;

// kevlough: subarray group size in bytes, currently assumed to be identical for all groups
static u64 full_subarray_group_size_bytes = 0;

/*
 * Default callback for parsing of the Proximity Domain <-> Memory
 * Area mappings
 */
int __init
acpi_numa_memory_affinity_init(struct acpi_srat_mem_affinity *ma)
{
	u64 start, end, subarray_boundary;
	u32 hotpluggable;
	int node, pxm;
	int last_subarray_group;
	struct decoded_addr res;
	int num_banks_per_socket;
	int himem_increment = 0x1000;

	if (unlikely(!skx_decode)) {
		// kevlough: extract topology info to determine subarray group size
		if (early_skx_init(&himem_subarray_boundary_addr, &first_remote_group, &num_banks_per_socket)) {
			panic("Failed SKX init!\n");
		}
		// kevlough: each row is 8 KiB
		full_subarray_group_size_bytes = num_banks_per_socket * num_rows_per_subarray * (8ULL * 1024ULL);
		pr_info("Full subarray group size (MiB): 0x%llx\n", full_subarray_group_size_bytes / 1024ULL / 1024ULL);
	}

	if (srat_disabled())
		goto out_err;
	if (ma->header.length < sizeof(struct acpi_srat_mem_affinity)) {
		pr_err("SRAT: Unexpected header length: %d\n",
		       ma->header.length);
		goto out_err_bad_srat;
	}
	if ((ma->flags & ACPI_SRAT_MEM_ENABLED) == 0)
		goto out_err;
	hotpluggable = ma->flags & ACPI_SRAT_MEM_HOT_PLUGGABLE;
	if (hotpluggable && !IS_ENABLED(CONFIG_MEMORY_HOTPLUG))
		goto out_err;

	start = ma->base_address;
	end = start + ma->length;
	pxm = ma->proximity_domain;
	if (acpi_srat_revision <= 1)
		pxm &= 0xff;

	node = acpi_map_pxm_to_node(pxm);
	if (node == NUMA_NO_NODE || node >= MAX_NUMNODES) {
		pr_err("SRAT: Too many proximity domains.\n");
		goto out_err_bad_srat;
	}

	// kevlough: need to handle start of physical node ranges specially
	if (node != 0 || start == 0) {
		res.addr = start;
		res.subarray_group = -1;
		if (unlikely(!skx_decode(&res))) {
			panic("Decode failed for start\n");
		}
		last_subarray_group = res.subarray_group;

		if (node == 0) {
			if (node != res.subarray_group) {
				panic("Unexpected node 0 layout!\n");
			}
		} else {
			if (SOCKET_1_GUARD_ROWS_NODE != res.subarray_group) {
				panic("Unexpected node 1 layout!. First remote is %d but got res.subarray_group=%d\n", SOCKET_1_EPT_NODE, res.subarray_group);
			}
		}
			
		// kevlough: setup first subarray group on this physical node
		// increment by 4 KB pages (slower) since lomem boundary may not be 2 MB aligned
		for (subarray_boundary = start + 0x1000; subarray_boundary < end; subarray_boundary += 0x1000) {
			res.addr = subarray_boundary;
			res.subarray_group = -1;
			if (unlikely(!skx_decode(&res))) {
				panic("Decode failed for start\n");
			}
			// kevlough: if we've reached a new subarray group, add the last group as a memblk
			if (res.subarray_group != last_subarray_group) {
				if (numa_add_memblk(last_subarray_group, start, subarray_boundary) < 0) {
					pr_err("SRAT: Failed to add memblk to node %u [mem %#010Lx-%#010Lx]\n",
						node, (unsigned long long) start,
						(unsigned long long) end - 1);
					goto out_err_bad_srat;
				}
				pr_info("SRAT: Node %u PXM %u [mem %#010Lx-%#010Lx]%s%s\n",
					last_subarray_group, pxm,
					(unsigned long long) start, (unsigned long long) subarray_boundary - 1,
					hotpluggable ? " hotplug (Force disabled)" : "",
					ma->flags & ACPI_SRAT_MEM_NON_VOLATILE ? " non-volatile" : "");
				start = subarray_boundary;
				last_subarray_group = res.subarray_group;
				// node_set(node, numa_nodes_parsed);
				parsed_numa_memblks++;
				break;
			}	
		}
	} else {
		if (himem_subarray_boundary_addr == 0) {
			panic("Failed to detect himem subarray bound addr!\n");
		}
		subarray_boundary = himem_subarray_boundary_addr;
 
		if (subarray_boundary != start) {
			res.addr = skx_tolm - 0x1000;
			res.subarray_group = -1;
			if (unlikely(!skx_decode(&res))) {
				panic("Decode failed for himem\n");
			}
			if (numa_add_memblk(res.subarray_group, start, subarray_boundary) < 0) {
				pr_err("SRAT: Failed to add memblk to node %u [mem %#010Lx-%#010Lx]\n",
					res.subarray_group, (unsigned long long) start,
					(unsigned long long) end - 1);
				goto out_err_bad_srat;
			}
			parsed_numa_memblks++;
			// node_set(res.subarray_group, numa_nodes_parsed);
			pr_info("SRAT: Node %u PXM %u [mem %#010Lx-%#010Lx]%s%s\n",
				res.subarray_group, pxm,
				(unsigned long long) start, (unsigned long long) subarray_boundary - 1,
				hotpluggable ? " hotplug (Force disabled)" : "",
				ma->flags & ACPI_SRAT_MEM_NON_VOLATILE ? " non-volatile" : "");
		}
		res.addr = subarray_boundary;
		res.subarray_group = -1;
		if (unlikely(!skx_decode(&res))) {
			panic("Decode failed for end here\n");
		}

		// kevlough: update last subarray group and start of this region
		last_subarray_group = res.subarray_group;
		start = subarray_boundary;
	}

	// kevlough: rest of subarray groups as remaining nodes
	// to speed up boot process on currently-supported subarray sizes, increment by 2 MB instead of 4 KB 
	if (num_rows_per_subarray % 512 == 0) {
		// increment by 4 KB on EPT/reserved nodes since 2 MB can touch multiple row groups
		if (last_subarray_group < SOCKET_0_EPT_NODE || last_subarray_group > SOCKET_1_GUARD_ROWS_NODE) {
			himem_increment = 0x200000;
		}
	}
	for (subarray_boundary = start + himem_increment; subarray_boundary < end; subarray_boundary += himem_increment) {
		res.addr = subarray_boundary;
		res.subarray_group = -1;
		if (unlikely(!skx_decode(&res))) {
			panic("Decode failed for rest\n");
		}
		if (res.subarray_group != last_subarray_group) {
			node = last_subarray_group;
			if (numa_add_memblk(node, start, subarray_boundary) < 0) {
				pr_err("SRAT: Failed to add memblk to node %u [mem %#010Lx-%#010Lx]\n",
					node, (unsigned long long) start,
					(unsigned long long) end - 1);
				goto out_err_bad_srat;
			}
			siloz_total_nodes = max(siloz_total_nodes, node + 1);
			// node_set(node, numa_nodes_parsed);
			pr_info("SRAT: Node %u PXM %u [mem %#010Lx-%#010Lx]%s%s\n",
				node, pxm,
				(unsigned long long) start, (unsigned long long) subarray_boundary - 1,
				hotpluggable ? " hotplug (Force disabled)" : "",
				ma->flags & ACPI_SRAT_MEM_NON_VOLATILE ? " non-volatile" : "");
			parsed_numa_memblks++;
			last_subarray_group = res.subarray_group;
			start = subarray_boundary;
			if (num_rows_per_subarray % 512 == 0) {
				if (res.subarray_group < SOCKET_0_EPT_NODE || res.subarray_group > SOCKET_1_GUARD_ROWS_NODE) {
					himem_increment = 0x200000;
				} else {
					himem_increment = 0x1000;
				}
			}
		}
	}

	if (subarray_boundary != end) {
		panic("kevlough: something is very wrong!\n");
	}

	// kevlough: need to add last group
	node = res.subarray_group;
	if (numa_add_memblk(node, start, subarray_boundary) < 0) {
		pr_err("SRAT: Failed to add memblk to node %u [mem %#010Lx-%#010Lx]\n",
						node, (unsigned long long) start,
						(unsigned long long) end - 1);
					goto out_err_bad_srat;
	}
	siloz_total_nodes = max(siloz_total_nodes, node + 1);

	pr_info("SRAT: Node %u PXM %u [mem %#010Lx-%#010Lx]%s%s\n",
		node, pxm,
		(unsigned long long) start, (unsigned long long) subarray_boundary - 1,
		hotpluggable ? " hotplug (Force disabled)" : "",
		ma->flags & ACPI_SRAT_MEM_NON_VOLATILE ? " non-volatile" : "");

	max_possible_pfn = max(max_possible_pfn, PFN_UP(end - 1));
	return 0;
out_err_bad_srat:
	bad_srat();
out_err:
	return -EINVAL;
}
#endif /* defined(CONFIG_X86) || defined (CONFIG_ARM64) */

static int __init acpi_parse_slit(struct acpi_table_header *table)
{
	struct acpi_table_slit *slit = (struct acpi_table_slit *)table;

	if (!slit_valid(slit)) {
		pr_info("SLIT table looks invalid. Not used.\n");
		return -EINVAL;
	}
	acpi_numa_slit_init(slit);

	return 0;
}

void __init __weak
acpi_numa_x2apic_affinity_init(struct acpi_srat_x2apic_cpu_affinity *pa)
{
	pr_warn("Found unsupported x2apic [0x%08x] SRAT entry\n", pa->apic_id);
}

static int __init
acpi_parse_x2apic_affinity(union acpi_subtable_headers *header,
			   const unsigned long end)
{
	struct acpi_srat_x2apic_cpu_affinity *processor_affinity;

	processor_affinity = (struct acpi_srat_x2apic_cpu_affinity *)header;

	acpi_table_print_srat_entry(&header->common);

	/* let architecture-dependent part to do it */
	acpi_numa_x2apic_affinity_init(processor_affinity);

	return 0;
}

static int __init
acpi_parse_processor_affinity(union acpi_subtable_headers *header,
			      const unsigned long end)
{
	struct acpi_srat_cpu_affinity *processor_affinity;

	processor_affinity = (struct acpi_srat_cpu_affinity *)header;

	acpi_table_print_srat_entry(&header->common);

	/* let architecture-dependent part to do it */
	acpi_numa_processor_affinity_init(processor_affinity);

	return 0;
}

static int __init
acpi_parse_gicc_affinity(union acpi_subtable_headers *header,
			 const unsigned long end)
{
	struct acpi_srat_gicc_affinity *processor_affinity;

	processor_affinity = (struct acpi_srat_gicc_affinity *)header;

	acpi_table_print_srat_entry(&header->common);

	/* let architecture-dependent part to do it */
	acpi_numa_gicc_affinity_init(processor_affinity);

	return 0;
}

#if defined(CONFIG_X86) || defined(CONFIG_ARM64)
static int __init
acpi_parse_gi_affinity(union acpi_subtable_headers *header,
		       const unsigned long end)
{
	struct acpi_srat_generic_affinity *gi_affinity;
	int node;

	gi_affinity = (struct acpi_srat_generic_affinity *)header;
	if (!gi_affinity)
		return -EINVAL;
	acpi_table_print_srat_entry(&header->common);

	if (!(gi_affinity->flags & ACPI_SRAT_GENERIC_AFFINITY_ENABLED))
		return -EINVAL;

	node = acpi_map_pxm_to_node(gi_affinity->proximity_domain);
	if (node == NUMA_NO_NODE || node >= MAX_NUMNODES) {
		pr_err("SRAT: Too many proximity domains.\n");
		return -EINVAL;
	}
	node_set(node, numa_nodes_parsed);
	node_set_state(node, N_GENERIC_INITIATOR);

	return 0;
}
#else
static int __init
acpi_parse_gi_affinity(union acpi_subtable_headers *header,
		       const unsigned long end)
{
	return 0;
}
#endif /* defined(CONFIG_X86) || defined (CONFIG_ARM64) */

static int __initdata parsed_numa_memblks;

static int __init
acpi_parse_memory_affinity(union acpi_subtable_headers * header,
			   const unsigned long end)
{
	struct acpi_srat_mem_affinity *memory_affinity;

	memory_affinity = (struct acpi_srat_mem_affinity *)header;

	acpi_table_print_srat_entry(&header->common);

	/* let architecture-dependent part to do it */
	if (!acpi_numa_memory_affinity_init(memory_affinity))
		parsed_numa_memblks++;
	return 0;
}

static int __init acpi_parse_srat(struct acpi_table_header *table)
{
	struct acpi_table_srat *srat = (struct acpi_table_srat *)table;

	acpi_srat_revision = srat->header.revision;

	/* Real work done in acpi_table_parse_srat below. */

	return 0;
}

static int __init
acpi_table_parse_srat(enum acpi_srat_type id,
		      acpi_tbl_entry_handler handler, unsigned int max_entries)
{
	return acpi_table_parse_entries(ACPI_SIG_SRAT,
					    sizeof(struct acpi_table_srat), id,
					    handler, max_entries);
}

int __init acpi_numa_init(void)
{
	int cnt = 0;
	int i;

	if (acpi_disabled)
		return -EINVAL;

	/*
	 * Should not limit number with cpu num that is from NR_CPUS or nr_cpus=
	 * SRAT cpu entries could have different order with that in MADT.
	 * So go over all cpu entries in SRAT to get apicid to node mapping.
	 */

	/* SRAT: System Resource Affinity Table */
	if (!acpi_table_parse(ACPI_SIG_SRAT, acpi_parse_srat)) {
		struct acpi_subtable_proc srat_proc[4];

		memset(srat_proc, 0, sizeof(srat_proc));
		srat_proc[0].id = ACPI_SRAT_TYPE_CPU_AFFINITY;
		srat_proc[0].handler = acpi_parse_processor_affinity;
		srat_proc[1].id = ACPI_SRAT_TYPE_X2APIC_CPU_AFFINITY;
		srat_proc[1].handler = acpi_parse_x2apic_affinity;
		srat_proc[2].id = ACPI_SRAT_TYPE_GICC_AFFINITY;
		srat_proc[2].handler = acpi_parse_gicc_affinity;
		srat_proc[3].id = ACPI_SRAT_TYPE_GENERIC_AFFINITY;
		srat_proc[3].handler = acpi_parse_gi_affinity;

		acpi_table_parse_entries_array(ACPI_SIG_SRAT,
					sizeof(struct acpi_table_srat),
					srat_proc, ARRAY_SIZE(srat_proc), 0);

		cnt = acpi_table_parse_srat(ACPI_SRAT_TYPE_MEMORY_AFFINITY,
					    acpi_parse_memory_affinity, 0);
	}

	// kevlough: ranges with reserved memory appear as partial subarray groups and should be
	// merged into host-reserved nodes
	numa_cleanup_partial_subarray_groups(full_subarray_group_size_bytes, &siloz_total_nodes, &first_remote_group);

	// kevlough: with groups setup, need to adjust node to pxm mapping
	if (first_remote_group == (unsigned int) -1) {
		for (i = 0; i < siloz_total_nodes; i++) {
			node_to_pxm_map[i] = 0;
		}
	} else {
		for (i = 0; i < siloz_total_nodes; i++) {
			// kevlough: account for "corner" case (e.g., low-numbered logical nodes on socket 1,
			// such as node 1 and reserved nodes.
			// Otherwise, logical nodes >= first_remote group are on socket 1, else on socket 0.
			if (i == 1 || i == SOCKET_1_EPT_NODE || i == SOCKET_1_GUARD_ROWS_NODE || (i >= first_remote_group && i != siloz_total_nodes - 1)) {
				node_to_pxm_map[i] = 1;
			} else {
				node_to_pxm_map[i] = 0;
			}
		}
	}
		

	/* SLIT: System Locality Information Table */
	acpi_table_parse(ACPI_SIG_SLIT, acpi_parse_slit);

	if (cnt < 0)
		return cnt;
	else if (!parsed_numa_memblks)
		return -ENOENT;
	return 0;
}

static int acpi_get_pxm(acpi_handle h)
{
	unsigned long long pxm;
	acpi_status status;
	acpi_handle handle;
	acpi_handle phandle = h;

	do {
		handle = phandle;
		status = acpi_evaluate_integer(handle, "_PXM", NULL, &pxm);
		if (ACPI_SUCCESS(status))
			return pxm;
		status = acpi_get_parent(handle, &phandle);
	} while (ACPI_SUCCESS(status));
	return -1;
}

int acpi_get_node(acpi_handle handle)
{
	int pxm;

	pxm = acpi_get_pxm(handle);

	return pxm_to_node(pxm);
}
EXPORT_SYMBOL(acpi_get_node);
