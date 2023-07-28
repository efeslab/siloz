// SPDX-License-Identifier: GPL-2.0
/*
 * EDAC driver for Intel(R) Xeon(R) Skylake processors
 * Copyright (c) 2016, Intel Corporation.
 */

#include <linux/kernel.h>
#include <linux/cdev.h>
#include <linux/processor.h>
#include <asm/cpu_device_id.h>
#include <asm/intel-family.h>
#include <asm/mce.h>
#include <asm/pci-direct.h>

#include "edac_module.h"
#include <linux/skx_common.h>
#include <linux/acpi.h>
#include <linux/hashtable.h>

#define EDAC_MOD_STR    "skx_edac"
#define SAD_DEV_START 0x0e
#define SAD_FUNC_START 0x0
#define MAX_DEV 1

// kevlough: default is 512, boot param can update
unsigned long num_rows_per_subarray = 512;
EXPORT_SYMBOL_GPL(num_rows_per_subarray);

// kevlough: indicates whether huge pages have been reserved on guest nodes
bool siloz_init_complete = false;
EXPORT_SYMBOL_GPL(siloz_init_complete);

static int skx_edac_open(struct inode *inode, struct file *file);
static int skx_edac_release(struct inode *inode, struct file *file);
static long skx_edac_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
static ssize_t skx_edac_read(struct file *file, char __user *buf, size_t count, loff_t *offset);
static ssize_t skx_edac_write(struct file *file, const char __user *buf, size_t count, loff_t *offset);
static int second_socket_empty = 0;

// kevlough: row number of 1st row >= 4 GB in phys addr space
static u32 himem_subarray_boundary_row = 0;

// kevlough: make edac a char dev to communicate with processes
static const struct file_operations skx_edac_fops = {
    .owner      = THIS_MODULE,
    .open       = skx_edac_open,
    .release    = skx_edac_release,
    .unlocked_ioctl = skx_edac_ioctl,
    .read       = skx_edac_read,
    .write       = skx_edac_write
};

struct mychar_device_data {
    struct cdev cdev;
};

static int dev_major = 0;
static struct class *skx_edac_class = NULL;
static struct mychar_device_data skx_edac_data[MAX_DEV];

static int skx_edac_uevent(struct device *dev, struct kobj_uevent_env *env)
{
    add_uevent_var(env, "DEVMODE=%#o", 0666);
    return 0;
}

static int skx_edac_open(struct inode *inode, struct file *file)
{
    printk("skx_edac: Device open\n");
    return 0;
}

static int skx_edac_release(struct inode *inode, struct file *file)
{
    printk("skx_edac: Device close\n");
    return 0;
}

static long skx_edac_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    printk("skx_edac: Device ioctl\n");
    return 0;
}

static ssize_t skx_edac_write(struct file *file, const char __user *buf, size_t count, loff_t *offset)
{
	// kevlough: Siloz initialization process writes to device once init is complete
	siloz_init_complete = true;
	printk("kevlough: skx_edac: siloz init complete\n");
    return count;
}

/*
 * Debug macros
 */
#define skx_printk(level, fmt, arg...)			\
	edac_printk(level, "skx", fmt, ##arg)

#define skx_mc_printk(mci, level, fmt, arg...)		\
	edac_mc_chipset_printk(mci, level, "skx", fmt, ##arg)

static struct list_head *skx_edac_list;
#define MAX_SOCKETS 8
static struct skx_dev skx_sock_devs[MAX_SOCKETS];

static int skx_num_sockets;
static unsigned int nvdimm_count;

#define	MASK26	0x3FFFFFF		/* Mask for 2^26 */
#define MASK29	0x1FFFFFFF		/* Mask for 2^29 */

static struct skx_dev *get_skx_dev(struct pci_bus *bus, u8 idx)
{
	struct skx_dev *d;

	list_for_each_entry(d, skx_edac_list, list) {
		if (d->seg == pci_domain_nr(bus) && d->bus[idx] == bus->number)
			return d;
	}

	return NULL;
}

enum munittype {
	CHAN0, CHAN1, CHAN2, SAD_ALL, UTIL_ALL, SAD,
	ERRCHAN0, ERRCHAN1, ERRCHAN2,
};

struct munit {
	u16	did;
	u16	devfn[SKX_NUM_IMC];
	u8	busidx;
	u8	per_socket;
	enum munittype mtype;
	u8 bus_numbers[2]; // TODO: hardcoded to support max 2 sockets
};

/*
 * List of PCI device ids that we need together with some device
 * number and function numbers to tell which memory controller the
 * device belongs to.
 */
static const struct munit skx_all_munits[] = {
	{ 0x2054, { PCI_DEVFN(0x1d, 0) }, 1, 1, SAD_ALL, {0x17, 0x85} },
	{ 0x2055, { PCI_DEVFN(0x1d, 1) }, 1, 1, UTIL_ALL, {0x17, 0x85} },
	{ 0x2040, { PCI_DEVFN(10, 0), PCI_DEVFN(12, 0) }, 2, 2, CHAN0, {0x3a, 0xae} },
	{ 0x2044, { PCI_DEVFN(10, 4), PCI_DEVFN(12, 4) }, 2, 2, CHAN1, {0x3a, 0xae} },
	{ 0x2048, { PCI_DEVFN(11, 0), PCI_DEVFN(13, 0) }, 2, 2, CHAN2, {0x3a, 0xae} },
	// TODO check errchan busno
	{ 0x2043, { PCI_DEVFN(10, 3), PCI_DEVFN(12, 3) }, 2, 2, ERRCHAN0, {0x3a, 0xae} },
	{ 0x2047, { PCI_DEVFN(10, 7), PCI_DEVFN(12, 7) }, 2, 2, ERRCHAN1, {0x3a, 0xae} },
	{ 0x204b, { PCI_DEVFN(11, 3), PCI_DEVFN(13, 3) }, 2, 2, ERRCHAN2, {0x3a, 0xae} },
	{ 0x208e, { }, 1, 0, SAD, {0x17, 0x85} },
	{ }
};

static int early_get_all_munits(const struct munit *m, struct skx_dev *skx_sock_devs)
{
	struct skx_dev *d;
	u32 reg;
	int iter;

	if (skx_num_sockets > 2) {
		printk(KERN_INFO "early_skx: more than 2 sockets found, currently unsupported\n");
		return 1;
	}

	if (m->per_socket == 0) {
		for (iter = 0; iter < skx_num_sockets; iter++) {
			d = skx_sock_devs + iter;
			switch (m->mtype) {
			case SAD:
				/*
				* one of these devices per core, including cores
				* that don't exist on this SKU. Ignore any that
				* read a route table of zero, make sure all the
				* non-zero values match.
				*/
				// kevlough: we only need one of these, as they should all match; let's do error checking later
				if (early_read_pci_config(m->bus_numbers[iter], SAD_DEV_START, SAD_FUNC_START, 0xB4, &reg)) {
					printk(KERN_INFO "early_skx: Invalid mcroute BDF\n");
					return 1;
				}
				if (reg != 0) {
					d->mcroute = reg;
				} else {
					d->mcroute = 0;
					skx_num_sockets--;
				}
				break;
			default:
				printk(KERN_INFO "early_skx: Unupported dev type for 0 devs!\n");
				return 1;
			}
		}
	} else {
		for (iter = 0; iter < m->per_socket * skx_num_sockets; iter++) {
			d = skx_sock_devs + (iter / m->per_socket);

			switch (m->mtype) {
			case CHAN0: case CHAN1: case CHAN2:
				d->imc[iter % m->per_socket].chan[m->mtype].cdev_bus = m->bus_numbers[iter / m->per_socket];
				d->imc[iter % m->per_socket].chan[m->mtype].cdev_dev = PCI_SLOT(m->devfn[iter % m->per_socket]);
				d->imc[iter % m->per_socket].chan[m->mtype].cdev_func = PCI_FUNC(m->devfn[iter % m->per_socket]);
				break;
			case SAD_ALL:
				d->sad_all_bus = m->bus_numbers[iter / m->per_socket];
				d->sad_all_dev = PCI_SLOT(m->devfn[iter % m->per_socket]);
				d->sad_all_func = PCI_FUNC(m->devfn[iter % m->per_socket]);
				break;
			case UTIL_ALL:
				d->util_all_bus = m->bus_numbers[iter / m->per_socket];
				d->util_all_dev = PCI_SLOT(m->devfn[iter % m->per_socket]);
				d->util_all_func = PCI_FUNC(m->devfn[iter % m->per_socket]);
				break;
			case ERRCHAN0: case ERRCHAN1: case ERRCHAN2:
				/* don't care for early boot */
				break;
			default:
				printk(KERN_INFO "early_skx: Unupported dev type!\n");
				return 1;
			}
		}
	}
	return m->per_socket * skx_num_sockets;
}

static int get_all_munits(const struct munit *m)
{
	struct pci_dev *pdev, *prev;
	struct skx_dev *d;
	u32 reg;
	int i = 0, ndev = 0;

	prev = NULL;
	for (;;) {
		pdev = pci_get_device(PCI_VENDOR_ID_INTEL, m->did, prev);
		if (!pdev)
			break;
		ndev++;
		if (m->per_socket == SKX_NUM_IMC) {
			for (i = 0; i < SKX_NUM_IMC; i++)
				if (m->devfn[i] == pdev->devfn)
					break;
			if (i == SKX_NUM_IMC)
				goto fail;
		}
		d = get_skx_dev(pdev->bus, m->busidx);
		if (!d)
			goto fail;

		/* Be sure that the device is enabled */
		if (unlikely(pci_enable_device(pdev) < 0)) {
			skx_printk(KERN_ERR, "Couldn't enable device %04x:%04x\n",
				   PCI_VENDOR_ID_INTEL, m->did);
			goto fail;
		}

		switch (m->mtype) {
		case CHAN0:
		case CHAN1:
		case CHAN2:
			pci_dev_get(pdev);
			d->imc[i].chan[m->mtype].cdev = pdev;
			break;
		case ERRCHAN0:
		case ERRCHAN1:
		case ERRCHAN2:
			pci_dev_get(pdev);
			d->imc[i].chan[m->mtype - ERRCHAN0].edev = pdev;
			break;
		case SAD_ALL:
			pci_dev_get(pdev);
			d->sad_all = pdev;
			break;
		case UTIL_ALL:
			pci_dev_get(pdev);
			d->util_all = pdev;
			break;
		case SAD:
			/*
			 * one of these devices per core, including cores
			 * that don't exist on this SKU. Ignore any that
			 * read a route table of zero, make sure all the
			 * non-zero values match.
			 */
			pci_read_config_dword(pdev, 0xB4, &reg);
			if (reg != 0) {
				if (d->mcroute == 0) {
					d->mcroute = reg;
				} else if (d->mcroute != reg) {
					skx_printk(KERN_ERR, "mcroute mismatch\n");
					goto fail;
				}
			}
			ndev--;
			break;
		}

		prev = pdev;
	}

	return ndev;
fail:
	pci_dev_put(pdev);
	return -ENODEV;
}

static struct res_config skx_cfg = {
	.type			= SKX,
	.decs_did		= 0x2016,
	.busno_cfg_offset	= 0xcc,
};

static const struct x86_cpu_id skx_cpuids[] = {
	X86_MATCH_INTEL_FAM6_MODEL_STEPPINGS(SKYLAKE_X, X86_STEPPINGS(0x0, 0xf), &skx_cfg),
	{ }
};
MODULE_DEVICE_TABLE(x86cpu, skx_cpuids);

static bool skx_check_ecc(u32 mcmtr)
{
	return !!GET_BITFIELD(mcmtr, 2, 2);
}

static int early_skx_get_dimm_config(struct skx_imc *imc)
{
	u32 mtr, mcmtr, amap;//, mcddrtcfg; // assume no NVDIMMs
	int i, j;

	/* Only the mcmtr on the first channel is effective */
	if (early_init_read_pci_config(imc->chan[0].cdev_bus, imc->chan[0].cdev_dev, imc->chan[0].cdev_func, 0x87c, &mcmtr)) {
		printk(KERN_INFO "early_skx: Invalid mcmtr BDF\n");
		return 1;
	}

	for (i = 0; i < SKX_NUM_CHANNELS; i++) {
		if (early_read_pci_config(imc->chan[i].cdev_bus, imc->chan[i].cdev_dev, imc->chan[i].cdev_func, 0x8C, &amap)) {
			printk(KERN_INFO "early_skx: Invalid amap BDF\n");
			return 1;
		}
		for (j = 0; j < SKX_NUM_DIMMS; j++) {
			if (early_read_pci_config(imc->chan[i].cdev_bus, imc->chan[i].cdev_dev, imc->chan[i].cdev_func, 
					      0x80 + 4 * j, &mtr)) {
				printk(KERN_INFO "early_skx: Invalid mtr BDF\n");
				return 1;
			}
			if (IS_DIMM_PRESENT(mtr)) {
				// kevlough: assume same geometry for all dimms, exit here
				pr_info("kevlough: found DIMM\n");
				early_skx_get_dimm_info(mtr, mcmtr, amap, imc, i, j);
				break;
			}
		}
	}

	return 0;
}

static int skx_get_dimm_config(struct mem_ctl_info *mci, struct res_config *cfg)
{
	struct skx_pvt *pvt = mci->pvt_info;
	u32 mtr, mcmtr, amap, mcddrtcfg;
	struct skx_imc *imc = pvt->imc;
	struct dimm_info *dimm;
	int i, j;
	int ndimms;

	/* Only the mcmtr on the first channel is effective */
	pci_read_config_dword(imc->chan[0].cdev, 0x87c, &mcmtr);

	for (i = 0; i < SKX_NUM_CHANNELS; i++) {
		ndimms = 0;
		pci_read_config_dword(imc->chan[i].cdev, 0x8C, &amap);
		pci_read_config_dword(imc->chan[i].cdev, 0x400, &mcddrtcfg);
		for (j = 0; j < SKX_NUM_DIMMS; j++) {
			dimm = edac_get_dimm(mci, i, j, 0);
			pci_read_config_dword(imc->chan[i].cdev,
					      0x80 + 4 * j, &mtr);
			if (IS_DIMM_PRESENT(mtr)) {
				ndimms += skx_get_dimm_info(mtr, mcmtr, amap, dimm, imc, i, j, cfg);
			} else if (IS_NVDIMM_PRESENT(mcddrtcfg, j)) {
				ndimms += skx_get_nvdimm_info(dimm, imc, i, j,
							      EDAC_MOD_STR);
				nvdimm_count++;
			}
		}
		if (ndimms && !skx_check_ecc(mcmtr)) {
			skx_printk(KERN_ERR, "ECC is disabled on imc %d\n", imc->mc);
			return -ENODEV;
		}
	}

	return 0;
}

#define	SKX_MAX_SAD 24

#define SKX_GET_SAD(d, i, reg)	\
	pci_read_config_dword((d)->sad_all, 0x60 + 8 * (i), &(reg))
#define SKX_GET_ILV(d, i, reg)	\
	pci_read_config_dword((d)->sad_all, 0x64 + 8 * (i), &(reg))

#define EARLY_SKX_GET_SAD(d, i, reg)	\
	early_read_pci_config((d)->sad_all_bus, (d)->sad_all_dev, (d)->sad_all_func, 0x60 + 8 * (i), &reg)
#define EARLY_SKX_GET_ILV(d, i, reg)	\
	early_read_pci_config((d)->sad_all_bus, (d)->sad_all_dev, (d)->sad_all_func, 0x64 + 8 * (i), &reg)

#define	SKX_SAD_MOD3MODE(sad)	GET_BITFIELD((sad), 30, 31)
#define	SKX_SAD_MOD3(sad)	GET_BITFIELD((sad), 27, 27)
#define SKX_SAD_LIMIT(sad)	(((u64)GET_BITFIELD((sad), 7, 26) << 26) | MASK26)
#define	SKX_SAD_MOD3ASMOD2(sad)	GET_BITFIELD((sad), 5, 6)
#define	SKX_SAD_ATTR(sad)	GET_BITFIELD((sad), 3, 4)
#define	SKX_SAD_INTERLEAVE(sad)	GET_BITFIELD((sad), 1, 2)
#define SKX_SAD_ENABLE(sad)	GET_BITFIELD((sad), 0, 0)

#define SKX_ILV_REMOTE(tgt)	(((tgt) & 8) == 0)
#define SKX_ILV_TARGET(tgt)	((tgt) & 7)

static void early_skx_show_retry_rd_err_log(struct decoded_addr *res,
				      char *msg, int len,
				      bool scrub_err)
{
	panic("Early skx show retry rd err log not yet supported!\n");
}

static void skx_show_retry_rd_err_log(struct decoded_addr *res,
				      char *msg, int len,
				      bool scrub_err)
{
	u32 log0, log1, log2, log3, log4;
	u32 corr0, corr1, corr2, corr3;
	struct pci_dev *edev;
	int n;

	edev = res->dev->imc[res->imc].chan[res->channel].edev;

	pci_read_config_dword(edev, 0x154, &log0);
	pci_read_config_dword(edev, 0x148, &log1);
	pci_read_config_dword(edev, 0x150, &log2);
	pci_read_config_dword(edev, 0x15c, &log3);
	pci_read_config_dword(edev, 0x114, &log4);

	n = snprintf(msg, len, " retry_rd_err_log[%.8x %.8x %.8x %.8x %.8x]",
		     log0, log1, log2, log3, log4);

	pci_read_config_dword(edev, 0x104, &corr0);
	pci_read_config_dword(edev, 0x108, &corr1);
	pci_read_config_dword(edev, 0x10c, &corr2);
	pci_read_config_dword(edev, 0x110, &corr3);

	if (len - n > 0)
		snprintf(msg + n, len - n,
			 " correrrcnt[%.4x %.4x %.4x %.4x %.4x %.4x %.4x %.4x]",
			 corr0 & 0xffff, corr0 >> 16,
			 corr1 & 0xffff, corr1 >> 16,
			 corr2 & 0xffff, corr2 >> 16,
			 corr3 & 0xffff, corr3 >> 16);
}

static bool early_skx_sad_decode(struct decoded_addr *res)
{
	struct skx_dev *d = &skx_sock_devs[0];
	u64 addr = res->addr;
	int i, idx, tgt, lchan, shift;
	u32 sad, ilv;
	u64 limit, prev_limit;
	int remote = 0;

	/* Simple sanity check for I/O space or out of range */
	if (addr >= skx_tohm || (addr >= skx_tolm && addr < BIT_ULL(32))) {
		printk(KERN_INFO "early_skx: Address 0x%llx out of range\n", addr);
		// return false;
	}

restart:
	prev_limit = 0;
	for (i = 0; i < SKX_MAX_SAD; i++) {
		if (EARLY_SKX_GET_SAD(d, i, sad)) {
			printk(KERN_INFO "early_skx: Invalid sad BDF\n");
			return false;
		}
		limit = SKX_SAD_LIMIT(sad);
		if (SKX_SAD_ENABLE(sad)) {
			if (addr >= prev_limit && addr <= limit)
				goto sad_found;
		}
		prev_limit = limit + 1;
	}
	printk(KERN_INFO "early_skx: No SAD entry for 0x%llx\n", addr);
	return false;

sad_found:
	// determine the ilv val based on SAD
	// i think it should be same for all
	if (EARLY_SKX_GET_ILV(d, i, ilv)) {
		printk(KERN_INFO "early_skx: Invalid ilv BDF\n");
		return false;
	}

	// bits 1-2 of sad determine interleave
	// index for interleave determined by amount of intlv
	switch (SKX_SAD_INTERLEAVE(sad)) {
	case 0:
		idx = GET_BITFIELD(addr, 6, 8);
		break;
	case 1:
		idx = GET_BITFIELD(addr, 8, 10);
		break;
	case 2:
		idx = GET_BITFIELD(addr, 12, 14);
		break;
	case 3:
		idx = GET_BITFIELD(addr, 30, 32);
		break;
	}

	// tgt determined by index, its 4 * idx to + 3 from this
	tgt = GET_BITFIELD(ilv, 4 * idx, 4 * idx + 3);

	/* If point to another node, find it and start over */
	if (SKX_ILV_REMOTE(tgt)) {
		if (remote) {
			printk(KERN_INFO "early_skx: early_skx: Double remote!\n");
			return false;
		}
		remote = 1;
		for (i = 0; i < skx_num_sockets; i++) {
			if (skx_sock_devs[i].imc[0].src_id == SKX_ILV_TARGET(tgt)) {
				d = skx_sock_devs + i;
				goto restart;
			}
		}
		printk(KERN_INFO "early_skx: early_skx: Can't find node %d\n", SKX_ILV_TARGET(tgt));
		return false;
	}

	if (SKX_SAD_MOD3(sad) == 0) {
		// lchan is lower 3 bits of tgt
		lchan = SKX_ILV_TARGET(tgt);
	} else {
		switch (SKX_SAD_MOD3MODE(sad)) {
		case 0:
			shift = 6;
			break;
		case 1:
			shift = 8;
			break;
		case 2:
			shift = 12;
			break;
		default:
			printk(KERN_INFO "early_skx: illegal mod3mode\n");
			return false;
		}
		switch (SKX_SAD_MOD3ASMOD2(sad)) {
		case 0:
			lchan = (addr >> shift) % 3;
			break;
		case 1:
			lchan = (addr >> shift) % 2;
			break;
		case 2:
			lchan = (addr >> shift) % 2;
			lchan = (lchan << 1) | !lchan;
			break;
		case 3:
			lchan = ((addr >> shift) % 2) << 1;
			break;
		}
		lchan = (lchan << 1) | (SKX_ILV_TARGET(tgt) & 1);
	}

	res->dev = d;
	res->socket = d->imc[0].src_id;
	res->imc = GET_BITFIELD(d->mcroute, lchan * 3, lchan * 3 + 2);
	res->channel = GET_BITFIELD(d->mcroute, lchan * 2 + 18, lchan * 2 + 19);
	return true;
}

static bool skx_sad_decode(struct decoded_addr *res)
{
	struct skx_dev *d = list_first_entry(skx_edac_list, typeof(*d), list);
	u64 addr = res->addr;
	int i, idx, tgt, lchan, shift;
	u32 sad, ilv;
	u64 limit, prev_limit;
	int remote = 0;

	/* Simple sanity check for I/O space or out of range */
	if (addr >= skx_tohm || (addr >= skx_tolm && addr < BIT_ULL(32))) {
		edac_dbg(0, "Address 0x%llx out of range\n", addr);
		return false;
	}

restart:
	prev_limit = 0;
	for (i = 0; i < SKX_MAX_SAD; i++) {
		SKX_GET_SAD(d, i, sad);
		limit = SKX_SAD_LIMIT(sad);
		if (SKX_SAD_ENABLE(sad)) {
			if (addr >= prev_limit && addr <= limit)
				goto sad_found;
		}
		prev_limit = limit + 1;
	}
	edac_dbg(0, "No SAD entry for 0x%llx\n", addr);
	return false;

sad_found:
	SKX_GET_ILV(d, i, ilv);

	switch (SKX_SAD_INTERLEAVE(sad)) {
	case 0:
		idx = GET_BITFIELD(addr, 6, 8);
		break;
	case 1:
		idx = GET_BITFIELD(addr, 8, 10);
		break;
	case 2:
		idx = GET_BITFIELD(addr, 12, 14);
		break;
	case 3:
		idx = GET_BITFIELD(addr, 30, 32);
		break;
	}

	tgt = GET_BITFIELD(ilv, 4 * idx, 4 * idx + 3);

	/* If point to another node, find it and start over */
	if (SKX_ILV_REMOTE(tgt)) {
		if (remote) {
			edac_dbg(0, "Double remote!\n");
			return false;
		}
		remote = 1;
		list_for_each_entry(d, skx_edac_list, list) {
			if (d->imc[0].src_id == SKX_ILV_TARGET(tgt))
				goto restart;
		}
		edac_dbg(0, "Can't find node %d\n", SKX_ILV_TARGET(tgt));
		return false;
	}

	if (SKX_SAD_MOD3(sad) == 0) {
		lchan = SKX_ILV_TARGET(tgt);
	} else {
		switch (SKX_SAD_MOD3MODE(sad)) {
		case 0:
			shift = 6;
			break;
		case 1:
			shift = 8;
			break;
		case 2:
			shift = 12;
			break;
		default:
			edac_dbg(0, "illegal mod3mode\n");
			return false;
		}
		switch (SKX_SAD_MOD3ASMOD2(sad)) {
		case 0:
			lchan = (addr >> shift) % 3;
			break;
		case 1:
			lchan = (addr >> shift) % 2;
			break;
		case 2:
			lchan = (addr >> shift) % 2;
			lchan = (lchan << 1) | !lchan;
			break;
		case 3:
			lchan = ((addr >> shift) % 2) << 1;
			break;
		}
		lchan = (lchan << 1) | (SKX_ILV_TARGET(tgt) & 1);
	}

	res->dev = d;
	res->socket = d->imc[0].src_id;
	res->imc = GET_BITFIELD(d->mcroute, lchan * 3, lchan * 3 + 2);
	res->channel = GET_BITFIELD(d->mcroute, lchan * 2 + 18, lchan * 2 + 19);

	edac_dbg(2, "0x%llx: socket=%d imc=%d channel=%d\n",
		 res->addr, res->socket, res->imc, res->channel);
	return true;
}

#define	SKX_MAX_TAD 8

#define SKX_GET_TADBASE(d, mc, i, reg)			\
	pci_read_config_dword((d)->imc[mc].chan[0].cdev, 0x850 + 4 * (i), &(reg))
#define SKX_GET_TADWAYNESS(d, mc, i, reg)		\
	pci_read_config_dword((d)->imc[mc].chan[0].cdev, 0x880 + 4 * (i), &(reg))
#define SKX_GET_TADCHNILVOFFSET(d, mc, ch, i, reg)	\
	pci_read_config_dword((d)->imc[mc].chan[ch].cdev, 0x90 + 4 * (i), &(reg))

// kevlough: max 8 sockets in SKX; use the TADs from chan 0 in each socket
static u32 early_skx_tadbases[8][SKX_NUM_IMC][SKX_MAX_TAD];
static u32 early_skx_tadwayness[8][SKX_NUM_IMC][SKX_MAX_TAD];
static u32 early_skx_tadchnilvoffset[8][SKX_NUM_IMC][SKX_NUM_CHANNELS][SKX_MAX_TAD];

#define EARLY_SKX_GET_TADBASE(d, mc, i, reg)			\
	early_init_read_pci_config((d)->imc[mc].chan[0].cdev_bus, (d)->imc[mc].chan[0].cdev_dev, (d)->imc[mc].chan[0].cdev_func, 0x850 + 4 * (i), &reg)
#define EARLY_SKX_GET_TADWAYNESS(d, mc, i, reg)		\
	early_init_read_pci_config((d)->imc[mc].chan[0].cdev_bus, (d)->imc[mc].chan[0].cdev_dev, (d)->imc[mc].chan[0].cdev_func, 0x880 + 4 * (i), &reg)
#define EARLY_SKX_GET_TADCHNILVOFFSET(d, mc, ch, i, reg)	\
	early_read_pci_config((d)->imc[mc].chan[ch].cdev_bus, (d)->imc[mc].chan[ch].cdev_dev, (d)->imc[mc].chan[ch].cdev_func, 0x90 + 4 * (i), &reg)

#define	SKX_TAD_BASE(b)		((u64)GET_BITFIELD((b), 12, 31) << 26)
#define SKX_TAD_SKT_GRAN(b)	GET_BITFIELD((b), 4, 5)
#define SKX_TAD_CHN_GRAN(b)	GET_BITFIELD((b), 6, 7)
#define	SKX_TAD_LIMIT(b)	(((u64)GET_BITFIELD((b), 12, 31) << 26) | MASK26)
#define	SKX_TAD_OFFSET(b)	((u64)GET_BITFIELD((b), 4, 23) << 26)
#define	SKX_TAD_SKTWAYS(b)	(1 << GET_BITFIELD((b), 10, 11))
#define	SKX_TAD_CHNWAYS(b)	(GET_BITFIELD((b), 8, 9) + 1)

/* which bit used for both socket and channel interleave */
static int skx_granularity[] = { 6, 8, 12, 30 };

static u64 skx_do_interleave(u64 addr, int shift, int ways, u64 lowbits)
{
	addr >>= shift;
	addr /= ways;
	addr <<= shift;

	return addr | (lowbits & ((1ull << shift) - 1));
}

static bool early_skx_tad_decode(struct decoded_addr *res)
{
	int i;
	u32 base, wayness, chnilvoffset;
	int skt_interleave_bit, chn_interleave_bit;
	u64 channel_addr;

	for (i = 0; i < SKX_MAX_TAD; i++) {
		base = early_skx_tadbases[res->socket][res->imc][i];
		wayness = early_skx_tadwayness[res->socket][res->imc][i];

		if (SKX_TAD_BASE(base) <= res->addr && res->addr <= SKX_TAD_LIMIT(wayness))
			goto tad_found;
	}
	printk(KERN_INFO "early_skx: No TAD entry for 0x%llx\n", res->addr);
	return false;

tad_found:
	res->sktways = SKX_TAD_SKTWAYS(wayness);
	res->chanways = SKX_TAD_CHNWAYS(wayness);

	skt_interleave_bit = skx_granularity[SKX_TAD_SKT_GRAN(base)];
	chn_interleave_bit = skx_granularity[SKX_TAD_CHN_GRAN(base)];

	chnilvoffset = early_skx_tadchnilvoffset[res->socket][res->imc][res->channel][i];
	channel_addr = res->addr - SKX_TAD_OFFSET(chnilvoffset);

	if (res->chanways == 3 && skt_interleave_bit > chn_interleave_bit) {
		/* Must handle channel first, then socket */
		channel_addr = skx_do_interleave(channel_addr, chn_interleave_bit,
						 res->chanways, channel_addr);
		channel_addr = skx_do_interleave(channel_addr, skt_interleave_bit,
						 res->sktways, channel_addr);
	} else {
		/* Handle socket then channel. Preserve low bits from original address */
		channel_addr = skx_do_interleave(channel_addr, skt_interleave_bit,
						 res->sktways, res->addr);
		channel_addr = skx_do_interleave(channel_addr, chn_interleave_bit,
						 res->chanways, res->addr);
	}

	res->chan_addr = channel_addr;
	return true;
}

static bool skx_tad_decode(struct decoded_addr *res)
{
	int i;
	u32 base, wayness, chnilvoffset;
	int skt_interleave_bit, chn_interleave_bit;
	u64 channel_addr;

	for (i = 0; i < SKX_MAX_TAD; i++) {
		SKX_GET_TADBASE(res->dev, res->imc, i, base);
		SKX_GET_TADWAYNESS(res->dev, res->imc, i, wayness);
		if (SKX_TAD_BASE(base) <= res->addr && res->addr <= SKX_TAD_LIMIT(wayness))
			goto tad_found;
	}
	edac_dbg(0, "No TAD entry for 0x%llx\n", res->addr);
	return false;

tad_found:
	res->sktways = SKX_TAD_SKTWAYS(wayness);
	res->chanways = SKX_TAD_CHNWAYS(wayness);
	skt_interleave_bit = skx_granularity[SKX_TAD_SKT_GRAN(base)];
	chn_interleave_bit = skx_granularity[SKX_TAD_CHN_GRAN(base)];

	SKX_GET_TADCHNILVOFFSET(res->dev, res->imc, res->channel, i, chnilvoffset);
	channel_addr = res->addr - SKX_TAD_OFFSET(chnilvoffset);

	if (res->chanways == 3 && skt_interleave_bit > chn_interleave_bit) {
		/* Must handle channel first, then socket */
		channel_addr = skx_do_interleave(channel_addr, chn_interleave_bit,
						 res->chanways, channel_addr);
		channel_addr = skx_do_interleave(channel_addr, skt_interleave_bit,
						 res->sktways, channel_addr);
	} else {
		/* Handle socket then channel. Preserve low bits from original address */
		channel_addr = skx_do_interleave(channel_addr, skt_interleave_bit,
						 res->sktways, res->addr);
		channel_addr = skx_do_interleave(channel_addr, chn_interleave_bit,
						 res->chanways, res->addr);
	}

	res->chan_addr = channel_addr;

	edac_dbg(2, "0x%llx: chan_addr=0x%llx sktways=%d chanways=%d\n",
		 res->addr, res->chan_addr, res->sktways, res->chanways);
	return true;
}

#define SKX_MAX_RIR 4

#define SKX_GET_RIRWAYNESS(d, mc, ch, i, reg)		\
	pci_read_config_dword((d)->imc[mc].chan[ch].cdev,	\
			      0x108 + 4 * (i), &(reg))
#define SKX_GET_RIRILV(d, mc, ch, idx, i, reg)		\
	pci_read_config_dword((d)->imc[mc].chan[ch].cdev,	\
			      0x120 + 16 * (idx) + 4 * (i), &(reg))

static u32 early_skx_rirwayness[8][SKX_NUM_IMC][SKX_NUM_CHANNELS][SKX_MAX_RIR];
// TODO what is max number of rir_wayness? need that here; believe 4 based on bit field
static u32 early_skx_ririlv[8][SKX_NUM_IMC][SKX_NUM_CHANNELS][SKX_MAX_RIR][4];

#define EARLY_SKX_GET_RIRWAYNESS(d, mc, ch, i, reg)		\
	early_init_read_pci_config((d)->imc[mc].chan[ch].cdev_bus,	(d)->imc[mc].chan[ch].cdev_dev,	(d)->imc[mc].chan[ch].cdev_func,	\
			      0x108 + 4 * (i), &(reg))
#define EARLY_SKX_GET_RIRILV(d, mc, ch, idx, i, reg)		\
	early_init_read_pci_config((d)->imc[mc].chan[ch].cdev_bus,	(d)->imc[mc].chan[ch].cdev_dev,	(d)->imc[mc].chan[ch].cdev_func,	\
			      0x120 + 16 * (idx) + 4 * (i), &(reg))

#define	SKX_RIR_VALID(b) GET_BITFIELD((b), 31, 31)
#define	SKX_RIR_LIMIT(b) (((u64)GET_BITFIELD((b), 1, 11) << 29) | MASK29)
#define	SKX_RIR_WAYS(b) (1 << GET_BITFIELD((b), 28, 29))
#define	SKX_RIR_CHAN_RANK(b) GET_BITFIELD((b), 16, 19)
#define	SKX_RIR_OFFSET(b) ((u64)(GET_BITFIELD((b), 2, 15) << 26))

static bool early_skx_rir_decode(struct decoded_addr *res)
{
	int i, idx, chan_rank;
	int shift;
	u32 rirway, rirlv;
	u64 rank_addr, prev_limit = 0, limit;

	if (res->dev->imc[res->imc].close_pg)
		shift = 6;
	else
		shift = 13;

	for (i = 0; i < SKX_MAX_RIR; i++) {
		rirway = early_skx_rirwayness[res->socket][res->imc][res->channel][i];
		limit = SKX_RIR_LIMIT(rirway);
		if (SKX_RIR_VALID(rirway)) {
			if (prev_limit <= res->chan_addr &&
			    res->chan_addr <= limit)
				goto rir_found;
		}
		prev_limit = limit;
	}
	printk(KERN_INFO "early_skx: No RIR entry for 0x%llx\n", res->addr);
	return false;

rir_found:
	rank_addr = res->chan_addr >> shift;
	rank_addr /= SKX_RIR_WAYS(rirway);
	rank_addr <<= shift;
	rank_addr |= res->chan_addr & GENMASK_ULL(shift - 1, 0);

	res->rank_address = rank_addr;
	idx = (res->chan_addr >> shift) % SKX_RIR_WAYS(rirway);


	rirlv = early_skx_ririlv[res->socket][res->imc][res->channel][i][idx];
	res->rank_address = rank_addr - SKX_RIR_OFFSET(rirlv);
	chan_rank = SKX_RIR_CHAN_RANK(rirlv);
	res->channel_rank = chan_rank;
	res->dimm = chan_rank / 4;
	res->rank = chan_rank % 4;
	return true;
}

static bool skx_rir_decode(struct decoded_addr *res)
{
	int i, idx, chan_rank;
	int shift;
	u32 rirway, rirlv;
	u64 rank_addr, prev_limit = 0, limit;

	if (res->dev->imc[res->imc].chan[res->channel].dimms[0].close_pg)
		shift = 6;
	else
		shift = 13;

	for (i = 0; i < SKX_MAX_RIR; i++) {
		SKX_GET_RIRWAYNESS(res->dev, res->imc, res->channel, i, rirway);
		limit = SKX_RIR_LIMIT(rirway);
		if (SKX_RIR_VALID(rirway)) {
			if (prev_limit <= res->chan_addr &&
			    res->chan_addr <= limit)
				goto rir_found;
		}
		prev_limit = limit;
	}
	edac_dbg(0, "No RIR entry for 0x%llx\n", res->addr);
	return false;

rir_found:
	rank_addr = res->chan_addr >> shift;
	rank_addr /= SKX_RIR_WAYS(rirway);
	rank_addr <<= shift;
	rank_addr |= res->chan_addr & GENMASK_ULL(shift - 1, 0);

	res->rank_address = rank_addr;
	idx = (res->chan_addr >> shift) % SKX_RIR_WAYS(rirway);

	SKX_GET_RIRILV(res->dev, res->imc, res->channel, idx, i, rirlv);
	res->rank_address = rank_addr - SKX_RIR_OFFSET(rirlv);
	chan_rank = SKX_RIR_CHAN_RANK(rirlv);
	res->channel_rank = chan_rank;
	res->dimm = chan_rank / 4;
	res->rank = chan_rank % 4;

	edac_dbg(2, "0x%llx: dimm=%d rank=%d chan_rank=%d rank_addr=0x%llx\n",
		 res->addr, res->dimm, res->rank,
		 res->channel_rank, res->rank_address);
	return true;
}

static u8 skx_close_row[] = {
	15, 16, 17, 18, 20, 21, 22, 28, 10, 11, 12, 13, 29, 30, 31, 32, 33
};

static u8 skx_close_column[] = {
	3, 4, 5, 14, 19, 23, 24, 25, 26, 27
};

static u8 skx_open_row[] = {
	14, 15, 16, 20, 28, 21, 22, 23, 24, 25, 26, 27, 29, 30, 31, 32, 33
};

static u8 skx_open_column[] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 12
};

static u8 skx_open_fine_column[] = {
	3, 4, 5, 7, 8, 9, 10, 11, 12, 13
};

static int skx_bits(u64 addr, int nbits, u8 *bits)
{
	int i, res = 0;

	for (i = 0; i < nbits; i++)
		res |= ((addr >> bits[i]) & 1) << i;
	return res;
}

static int skx_bank_bits(u64 addr, int b0, int b1, int do_xor, int x0, int x1)
{
	int ret = GET_BITFIELD(addr, b0, b0) | (GET_BITFIELD(addr, b1, b1) << 1);

	if (do_xor)
		ret ^= GET_BITFIELD(addr, x0, x0) | (GET_BITFIELD(addr, x1, x1) << 1);

	return ret;
}

static bool early_skx_mad_decode(struct decoded_addr *r)
{
	struct skx_imc *imc = &r->dev->imc[r->imc];
	int bg0 = imc->fine_grain_bank ? 6 : 13;

	int num_subarrays_touched_per_huge_page;

	if (imc->close_pg) {
		r->row = skx_bits(r->rank_address, imc->rowbits, skx_close_row);
		r->column = skx_bits(r->rank_address, imc->colbits, skx_close_column);
		r->column |= 0x400; /* C10 is autoprecharge, always set */
		r->bank_address = skx_bank_bits(r->rank_address, 8, 9, imc->bank_xor_enable, 22, 28);
		r->bank_group = skx_bank_bits(r->rank_address, 6, 7, imc->bank_xor_enable, 20, 21);
		
		// kevlough: close_pg support not tested
		if (num_rows_per_subarray) {
			num_subarrays_touched_per_huge_page = (4096 + num_rows_per_subarray - 1) / num_rows_per_subarray;
		}
		
	} else {
		r->row = skx_bits(r->rank_address, imc->rowbits, skx_open_row);
		if (imc->fine_grain_bank)
			r->column = skx_bits(r->rank_address, imc->colbits, skx_open_fine_column);
		else
			r->column = skx_bits(r->rank_address, imc->colbits, skx_open_column);
		r->bank_address = skx_bank_bits(r->rank_address, 18, 19, imc->bank_xor_enable, 22, 23);
		r->bank_group = skx_bank_bits(r->rank_address, bg0, 17, imc->bank_xor_enable, 20, 21);
		num_subarrays_touched_per_huge_page = 1;
	}
	r->row &= (1u << imc->rowbits) - 1;

	// kevlough: calculate subarray group from translation and subarray group size
	if (num_rows_per_subarray) {
		r->subarray_group = (r->socket * (1 << imc->rowbits) + r->row) / (num_rows_per_subarray * num_subarrays_touched_per_huge_page);
	} else {
		// if we haven't set num_rows_per_subarray yet, we're on group 0
		r->subarray_group = 0;
	}

	// kevlough: handle EPT+guard (reserved) nodes
	if (r->socket) {
		if (r->row < 32) {
			if (r->row != 0xc) {
				r->subarray_group = SOCKET_1_GUARD_ROWS_NODE;
			} else {
				r->subarray_group = SOCKET_1_EPT_NODE;
			}
		} else {
			r->subarray_group += SOCKET_1_GUARD_ROWS_NODE - SOCKET_0_EPT_NODE + 1;
		}
	} else if (himem_subarray_boundary_row) {
		if (r->row >= himem_subarray_boundary_row && r->row < himem_subarray_boundary_row + 32) {
			if (r->row != himem_subarray_boundary_row + 0xc) {
				r->subarray_group = SOCKET_0_GUARD_ROWS_NODE;
			} else {
				r->subarray_group = SOCKET_0_EPT_NODE;
			}
		} else if (r->subarray_group >= SOCKET_0_EPT_NODE) {
			r->subarray_group += SOCKET_1_GUARD_ROWS_NODE - SOCKET_0_EPT_NODE + 1;
		}
	} else if (r->subarray_group >= SOCKET_0_EPT_NODE) {
		r->subarray_group += SOCKET_1_GUARD_ROWS_NODE - SOCKET_0_EPT_NODE + 1;
	}

	return true;
}

static bool skx_mad_decode(struct decoded_addr *r)
{
	struct skx_dimm *dimm = &r->dev->imc[r->imc].chan[r->channel].dimms[r->dimm];
	int bg0 = dimm->fine_grain_bank ? 6 : 13;

	if (dimm->close_pg) {
		r->row = skx_bits(r->rank_address, dimm->rowbits, skx_close_row);
		r->column = skx_bits(r->rank_address, dimm->colbits, skx_close_column);
		r->column |= 0x400; /* C10 is autoprecharge, always set */
		r->bank_address = skx_bank_bits(r->rank_address, 8, 9, dimm->bank_xor_enable, 22, 28);
		r->bank_group = skx_bank_bits(r->rank_address, 6, 7, dimm->bank_xor_enable, 20, 21);
	} else {
		r->row = skx_bits(r->rank_address, dimm->rowbits, skx_open_row);
		if (dimm->fine_grain_bank)
			r->column = skx_bits(r->rank_address, dimm->colbits, skx_open_fine_column);
		else
			r->column = skx_bits(r->rank_address, dimm->colbits, skx_open_column);
		r->bank_address = skx_bank_bits(r->rank_address, 18, 19, dimm->bank_xor_enable, 22, 23);
		r->bank_group = skx_bank_bits(r->rank_address, bg0, 17, dimm->bank_xor_enable, 20, 21);
	}
	r->row &= (1u << dimm->rowbits) - 1;

	edac_dbg(2, "0x%llx: row=0x%x col=0x%x bank_addr=%d bank_group=%d\n",
		 r->addr, r->row, r->column, r->bank_address,
		 r->bank_group);
	return true;
}

static bool skx_decode_local(struct decoded_addr *res)
{
	return skx_sad_decode(res) && skx_tad_decode(res) &&
		skx_rir_decode(res) && skx_mad_decode(res);
}

static bool early_skx_decode_local(struct decoded_addr *res)
{
	return early_skx_sad_decode(res) && early_skx_tad_decode(res) &&
		early_skx_rir_decode(res) && early_skx_mad_decode(res);
}

static ssize_t skx_edac_read(struct file *file, char __user *buf, size_t count, loff_t *offset)
{
	struct decoded_addr res;
	u64 addr;
	size_t i;

	if (count > 0 && offset && *offset >= 0) {
		memset(&res, 0, sizeof(res));
		addr = (u64) ((*offset >> 6) << 6);
		for (i = 0; i < count; i++) {
			res.addr = addr + i * 0x40;
			if (skx_decode_local(&res)) {
				printk("0x%llx,%u,%u,%u,%u,%u,%u,%u,0x%x,0x%x\n", res.addr, res.socket, res.imc, res.channel, res.dimm, res.rank, res.bank_group, res.bank_address, res.row, res.column);
			} else {
				printk("skx_edac: Error on addr %llx\n", addr);
				break;
			}
		}
	}
    return 0;
}

static struct notifier_block skx_mce_dec = {
	.notifier_call	= skx_mce_check_error,
	.priority	= MCE_PRIO_EDAC,
};

#ifdef CONFIG_EDAC_DEBUG
/*
 * Debug feature.
 * Exercise the address decode logic by writing an address to
 * /sys/kernel/debug/edac/skx_test/addr.
 */
static struct dentry *skx_test;

static int debugfs_u64_set(void *data, u64 val)
{
	struct mce m;

	pr_warn_once("Fake error to 0x%llx injected via debugfs\n", val);

	memset(&m, 0, sizeof(m));
	/* ADDRV + MemRd + Unknown channel */
	m.status = MCI_STATUS_ADDRV + 0x90;
	/* One corrected error */
	m.status |= BIT_ULL(MCI_STATUS_CEC_SHIFT);
	m.addr = val;
	skx_mce_check_error(NULL, 0, &m);

	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(fops_u64_wo, NULL, debugfs_u64_set, "%llu\n");

static void setup_skx_debug(void)
{
	skx_test = edac_debugfs_create_dir("skx_test");
	if (!skx_test)
		return;

	if (!edac_debugfs_create_file("addr", 0200, skx_test,
				      NULL, &fops_u64_wo)) {
		debugfs_remove(skx_test);
		skx_test = NULL;
	}
}

static void teardown_skx_debug(void)
{
	debugfs_remove_recursive(skx_test);
}
#else
static inline void setup_skx_debug(void) {}
static inline void teardown_skx_debug(void) {}
#endif /*CONFIG_EDAC_DEBUG*/

int early_skx_init(u64 *himem_subarray_boundary_addr, unsigned int *first_remote_group, int *num_banks_per_socket)
{
	// const struct x86_cpu_id *id;
    int rc = 0, off[3] = {0xd0, 0xd4, 0xd8};
	const struct munit *m;
	int sock_idx, imc, chan, i, idx;
	u8 mc = 0, src_id, node_id;
	struct decoded_addr res;
	u64 addr;
	int color;
	bool lomem_subarray_groups_map[MAX_SUBARRAY_GROUPS] = {false};

	if (!himem_subarray_boundary_addr || !first_remote_group || !num_banks_per_socket) {
		return 1;
	}

	*num_banks_per_socket = 0;

    if (early_skx_get_hi_lo(0x2034, off, &skx_tolm, &skx_tohm)) {
		printk(KERN_INFO "early_skx: Can't get TOLM/TOHM\n");
        return -1;
    }

	printk(KERN_INFO "early_skx: TOLM: 0x%llx, TOHM: 0x%llx\n", skx_tolm, skx_tohm);

	// kevlough: rounded down to 64 MB boundary, so give a 64 MB cushion
	skx_tolm += 0x4000000;
	skx_tohm += 0x4000000;

	if (skx_tohm < 0x3200000000) {
		second_socket_empty = 1;
	}

    rc = early_skx_get_all_bus_mappings(0x2016, 0xcc, SKX, skx_sock_devs);
    if (rc < 0) {
		printk(KERN_INFO "early_skx: Can't get bus mappings\n");
        return -1;
    }
    if (rc == 0) {
        printk(KERN_INFO "early_skx: Can't get mem ctrls\n");
        return -1;
    }
    skx_num_sockets = rc;

	printk(KERN_INFO "early_skx: we have %d sockets\n", skx_num_sockets);

    for (m = skx_all_munits; m->did; m++) {
        rc = early_get_all_munits(m, skx_sock_devs);
        if (rc < 0) {
			printk(KERN_INFO "early_skx: Can't get munits\n");
            return -1;
        }
        if (rc != m->per_socket * skx_num_sockets) {
            printk(KERN_INFO "early_skx: Expected %d, got %d of 0x%x\n",
                    m->per_socket * skx_num_sockets, rc, m->did);
            return -1;
        }
    }

    for (sock_idx = 0;
            sock_idx < skx_num_sockets;
            ++sock_idx) {
        struct skx_dev *d = &skx_sock_devs[sock_idx];
        rc = early_skx_get_src_id(d, 0xf0, &src_id);
        if (rc < 0) {
            printk(KERN_INFO "early_skx: [!!] fail skx_get_src_id\n");
            return -1;
        }
        rc = early_skx_get_node_id(d, &node_id);
        if (rc < 0) {
            printk(KERN_INFO "early_skx: [!!] fail skx_get_node_id\n");
            return -1;
        }
        for (imc = 0; imc < SKX_NUM_IMC; imc++) {
            d->imc[imc].mc = mc++;
            d->imc[imc].lmc = imc;
            d->imc[imc].src_id = src_id;
            d->imc[imc].node_id = node_id;

			pr_info("kevlough: checking for imc %d DIMMs\n", imc);
            rc = early_skx_get_dimm_config(&d->imc[imc]);
            if (rc < 0) {
                printk(KERN_INFO "early_skx: Failed to skx_get_dimm_config\n");
                return -1;
            }
			for (i = 0; i < SKX_MAX_TAD; i++) {
				if (EARLY_SKX_GET_TADBASE(d, imc, i, early_skx_tadbases[sock_idx][imc][i])) {
					printk(KERN_INFO "early_skx: Invalid base BDF\n");
					return 1;
				}
				if (EARLY_SKX_GET_TADWAYNESS(d, imc, i, early_skx_tadwayness[sock_idx][imc][i])) {
					printk(KERN_INFO "early_skx: Invalid wayness BDF\n");
					return 1;
				}
				for (chan = 0; chan < SKX_NUM_CHANNELS; chan++) {
					if (EARLY_SKX_GET_TADCHNILVOFFSET(d, imc, chan, i, early_skx_tadchnilvoffset[sock_idx][imc][chan][i])) {
						printk(KERN_INFO "early_skx: Invalid TADCHNILVOFFSET BDF\n");
						return 1;
					}
				}
			}


			for (i = 0; i < SKX_MAX_RIR; i++) {
				for (chan = 0; chan < SKX_NUM_CHANNELS; chan++) {
					if (EARLY_SKX_GET_RIRWAYNESS(d, imc, chan, i, early_skx_rirwayness[sock_idx][imc][chan][i])) {
						printk(KERN_INFO "early_skx: Invalid rirwayness BDF\n");
						return 1;
					}
					if (SKX_RIR_VALID(early_skx_rirwayness[sock_idx][imc][chan][i])) {
						// printk(KERN_INFO "early_skx: rirwayness[sock=%d][imc=%d][chan=%d][rir=%d]: %d\n", sock_idx, imc, chan, i, SKX_RIR_WAYS(early_skx_rirwayness[sock_idx][imc][chan][i]));
						for (idx = 0; idx < SKX_RIR_WAYS(early_skx_rirwayness[sock_idx][imc][chan][i]); idx++) {
							if (EARLY_SKX_GET_RIRILV(d, imc, chan, idx, i, early_skx_ririlv[sock_idx][imc][chan][i][idx])) {
								printk(KERN_INFO "early_skx: Invalid rirwayness BDF\n");
								return 1;
							}
							if (sock_idx == 0) {
								*num_banks_per_socket = *num_banks_per_socket + 16;
							}
						}
					}	
				}
			}
		}
	}

	printk(KERN_INFO "early_skx: Num banks/socket: %d\n", *num_banks_per_socket);

	if (skx_decode != NULL) {
		printk(KERN_INFO "early_skx: Houston we have a problem! Func is not NULL\n");
	}
	// early_skx_show_retry_rd_err_log not yet supported, but v unlikely to be called
	// for now, would just panic if called
	skx_set_decode(early_skx_decode_local, early_skx_show_retry_rd_err_log);

	rc = 0;

	color = 0;

	for (addr = 0x0; addr < skx_tolm; addr += 0x1000) {
		res.addr = addr;
		if (!skx_decode(&res) || res.socket != 0) {
			printk(KERN_INFO "early_skx: failed early lomem decode on addr 0x%llx\n", addr);
			rc = 1;
			break;
		}
		if (!lomem_subarray_groups_map[res.subarray_group]) {
			printk(KERN_INFO "early_skx: lomem subarray group for color %d (addr 0x%llx, row %d)\n", res.subarray_group, addr, res.row);
			lomem_subarray_groups_map[res.subarray_group] = 1;
		}
	}

	if (rc == 0) {	
		for (addr = 0x100000000; addr < skx_tohm; addr += 0x1000) {
			res.addr = addr;
			if (!skx_decode(&res)) {
				printk(KERN_INFO "early_skx: failed early himem decode on addr 0x%llx\n", addr);
				rc = 1;
				break;
			}
			if (!lomem_subarray_groups_map[res.subarray_group]) {
				printk(KERN_INFO "early_skx: himem subarray bound is color %d (addr 0x%llx, row %d)\n", res.subarray_group, addr, res.row);
				*himem_subarray_boundary_addr = addr;
				himem_subarray_boundary_row = res.row + num_rows_per_subarray;
				res.subarray_group = SOCKET_0_GUARD_ROWS_NODE;
				break;
			}
		}

		if (rc == 0 && !second_socket_empty) {
			// shorten search to just top half of physical memory
			for (addr = skx_tohm / 2; addr < skx_tohm; addr += 0x1000) {
				res.addr = addr;
				if (!skx_decode(&res)) {
					printk(KERN_INFO "early_skx: failed early himem decode on addr 0x%llx\n", addr);
					rc = 1;
					break;
				}
				if (res.socket != 0 && res.subarray_group != SOCKET_1_EPT_NODE && res.subarray_group != SOCKET_1_GUARD_ROWS_NODE) {
					printk(KERN_INFO "early_skx: first remote subarray group is color %d (addr 0x%llx, row %d)\n", res.subarray_group, addr, res.row);
					*first_remote_group = (unsigned int) res.subarray_group;
					break;
				}
			}
		}
	}

	printk(KERN_INFO "early_skx: returned %s. %ld rows/subarray. himem subarray bound: 0x%llx\n",
		rc ? "FAIL" : "SUCCESS", num_rows_per_subarray, *himem_subarray_boundary_addr);
    return rc;
}

/*
 * skx_init:
 *	make sure we are running on the correct cpu model
 *	search for all the devices we need
 *	check which DIMMs are present.
 */
static int __init skx_init(void)
{
	const struct x86_cpu_id *id;
	struct res_config *cfg;
	const struct munit *m;
	const char *owner;
	int rc = 0, i, off[3] = {0xd0, 0xd4, 0xd8};
	u8 mc = 0, src_id, node_id;
	struct skx_dev *d;
	int err;
    dev_t dev;

	edac_dbg(2, "\n");

	skx_printk(KERN_INFO, "early_skx: end: LOADING KEVIN'S MODIFIED SKX_EDAC\n");

	owner = edac_get_owner();
	if (owner && strncmp(owner, EDAC_MOD_STR, sizeof(EDAC_MOD_STR)))
		return -EBUSY;

	if (cpu_feature_enabled(X86_FEATURE_HYPERVISOR))
		return -ENODEV;

	id = x86_match_cpu(skx_cpuids);
	if (!id)
		return -ENODEV;

	cfg = (struct res_config *)id->driver_data;

	rc = skx_get_hi_lo(0x2034, off, &skx_tolm, &skx_tohm);
	if (rc)
		return rc;
	
	// kevlough: rounded down to 64 MB boundary, so give a 64 MB cushion
	skx_tolm += 0x4000000;
	skx_tohm += 0x4000000;

	rc = skx_get_all_bus_mappings(cfg, &skx_edac_list);
	if (rc < 0)
		goto fail;
	if (rc == 0) {
		edac_dbg(2, "No memory controllers found\n");
		return -ENODEV;
	}
	skx_num_sockets = rc;

	for (m = skx_all_munits; m->did; m++) {
		rc = get_all_munits(m);
		if (rc < 0)
			goto fail;
		if (rc != m->per_socket * skx_num_sockets) {
			edac_dbg(2, "Expected %d, got %d of 0x%x\n",
				 m->per_socket * skx_num_sockets, rc, m->did);
			rc = -ENODEV;
			goto fail;
		}
	}

	list_for_each_entry(d, skx_edac_list, list) {
		rc = skx_get_src_id(d, 0xf0, &src_id);
		if (rc < 0)
			goto fail;
		rc = skx_get_node_id(d, &node_id);
		if (rc < 0)
			goto fail;
		edac_dbg(2, "src_id=%d node_id=%d\n", src_id, node_id);
		for (i = 0; i < SKX_NUM_IMC; i++) {
			d->imc[i].mc = mc++;
			d->imc[i].lmc = i;
			d->imc[i].src_id = src_id;
			d->imc[i].node_id = node_id;
			rc = skx_register_mci(&d->imc[i], d->imc[i].chan[0].cdev,
					      "Skylake Socket", EDAC_MOD_STR,
					      skx_get_dimm_config, cfg);
			if (rc < 0)
				goto fail;
		}
	}

	if (skx_decode == NULL) {
		skx_printk(KERN_INFO, "early_skx: Houston we have a problem! Func is NULL\n");
	}

	skx_set_decode(skx_decode_local, skx_show_retry_rd_err_log);

	if (nvdimm_count && skx_adxl_get() == -ENODEV)
		skx_printk(KERN_NOTICE, "Only decoding DDR4 address!\n");

	/* Ensure that the OPSTATE is set correctly for POLL or NMI */
	opstate_init();

	setup_skx_debug();

	mce_register_decode_chain(&skx_mce_dec);

	err = alloc_chrdev_region(&dev, 0, MAX_DEV, "skx_edac");

    dev_major = MAJOR(dev);

    skx_edac_class = class_create(THIS_MODULE, "skx_edac");
    skx_edac_class->dev_uevent = skx_edac_uevent;

    for (i = 0; i < MAX_DEV; i++) {
        cdev_init(&skx_edac_data[i].cdev, &skx_edac_fops);
        skx_edac_data[i].cdev.owner = THIS_MODULE;

        cdev_add(&skx_edac_data[i].cdev, MKDEV(dev_major, i), 1);

        device_create(skx_edac_class, NULL, MKDEV(dev_major, i), NULL, "skx_edac");
    }

	return 0;
fail:
	skx_remove();
	return rc;
}

static void __exit skx_exit(void)
{
	int i;
	skx_printk(KERN_INFO, "\n");

	for (i = 0; i < MAX_DEV; i++) {
        device_destroy(skx_edac_class, MKDEV(dev_major, i));
    }

    class_unregister(skx_edac_class);
    class_destroy(skx_edac_class);

    unregister_chrdev_region(MKDEV(dev_major, 0), MINORMASK);

	mce_unregister_decode_chain(&skx_mce_dec);
	teardown_skx_debug();
	if (nvdimm_count)
		skx_adxl_put();
	skx_remove();
}

module_init(skx_init);
module_exit(skx_exit);

module_param(edac_op_state, int, 0444);
MODULE_PARM_DESC(edac_op_state, "EDAC Error Reporting state: 0=Poll,1=NMI");

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Tony Luck");
MODULE_DESCRIPTION("MC Driver for Intel Skylake server processors");
