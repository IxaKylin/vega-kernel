// SPDX-License-Identifier: GPL-2.0
/*
 * FiberHome Fisilink Switch Interface Driver
 *
 * Copyright (C) 2011-2012 FiberHome
 * Author: zhenxin.xie
 *
 * PCIE to PATA driver. This driver provides an IDE interface and
 * supports max two CF cards working together.
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/blkdev.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/vmalloc.h>

#include "fh_pcie2pata.h"

static DEFINE_MUTEX(mutex);

#define PRINT_ERROR		0
#define PRINT_NOTICE	2
#define PRINT_DEBUG		1
#define PRINT_INFO		2

static int print_level = PRINT_ERROR;

#define fhme_print(n, fmt, ...)					\
	do {							\
		if (print_level >= (n))				\
			printk(KERN_DEBUG fmt, ##__VA_ARGS__);	\
	} while (0)

static int driver_is_present;
static int open_counter;

static DMA_TypeDef __iomem *fh_ide_ctrl_base1;
static void __iomem *dma_reg_base;

static unsigned int _dma_mem_size;
static unsigned int *_dma_vbase;
static unsigned int _dma_pbase;

static DECLARE_WAIT_QUEUE_HEAD(_interrupt_wq);
static LIST_HEAD(_dma_buf);
static int dmabuf_list_num;
#define DMABUF_LIST_MAX_NUM 10
#define DMA_MAX_SIZE  0x20000
#define DMA_BASE_ADDR 0x40010000
static uint32_t dma_buffer_state;

typedef struct _dma_buffer {
	struct list_head list;
	unsigned long paddr;		 /* DMA phy addr */
	unsigned long vaddr;		 /* DMA virtual addr in kernel space*/
	unsigned long useraddr;	 /* DMA virtual addr in user space*/
	unsigned long size;		 /* buffer size */
} dma_buffer_t;

static LIST_HEAD(_dma_seg);

typedef struct _dma_segment {
	struct list_head list;
	unsigned long req_size;     /* Requested DMA segment size */
	unsigned long blk_size;     /* DMA block size */
	unsigned long blk_order;    /* DMA block size in alternate format */
	unsigned long seg_size;     /* Current DMA segment size */
	unsigned long seg_begin;    /* Logical address of segment */
	unsigned long seg_end;      /* Logical end address of segment */
	unsigned long *blk_ptr;     /* Array of logical DMA block addresses */
	int blk_cnt_max;            /* Maximum number of block to allocate */
	int blk_cnt;                /* Current number of blocks allocated */
} dma_segment_t;

/* DMA memory allocation */
#define ONE_KB 1024
#define ONE_MB (1024 * 1024)

/* Default DMA memory size */
#define DMA_MEM_DEFAULT (20 * ONE_MB)
#define DMA_MEM_DEFAULT_ROBO (20 * ONE_MB)

/* We try to assemble a contiguous segment from chunks of this size */
#define DMA_BLOCK_SIZE (512 * ONE_KB)

#define MEM_MAP_RESERVE SetPageReserved
#define MEM_MAP_UNRESERVE ClearPageReserved
#define IOREMAP(addr, size) ioremap_nocache(addr, size)
#define VIRT_TO_PAGE(p)     virt_to_page((void *)(p))

#define dma_reg_read(addr)		(*(volatile unsigned int *)(addr))
#define dma_reg_write(addr, value)	(*(volatile unsigned int *)(addr) = (value))

#define SOC_BLK_PCIEDMA (0x2000)

static int fh_pcie2pata_open(struct inode *inode, struct file *file)
{
	if (!driver_is_present) {
		fhme_print(PRINT_ERROR, "%s@%s: device is not present\n",
			   __func__, DRIVER_NAME);
		return -EIO;
	}
	return 0;
}

static int fh_pcie2pata_release(struct inode *inode, struct file *file)
{
	return 0;
}

static long fh_pcie2pata_ioctl(struct file *file, unsigned int cmd,
			       unsigned long arg)
{
	int ret = 0;
	struct pcie2pata_read_reg reg_read;
	struct pcie2pata_write_reg reg_write;
	struct pcie2pata_read_array_reg reg_array_read;
	struct pcie2pata_write_array_reg reg_array_write;
	struct pcie2pata_addr dmaaddr;
	dma_buffer_t *dma_buffers;
	struct list_head *pos;
	unsigned int i = 0;
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case fh_pcie2pata_ioctl_read_reg:
		ret = copy_from_user(&reg_read, (struct pcie2pata_read_reg *)arg,
				     sizeof(struct pcie2pata_read_reg));
		if (ret) {
			fhme_print(PRINT_ERROR,
				   "%s@%s: fh_pcie2pata_ioctl_read_reg: copy from user failed\n",
				   __func__, DRIVER_NAME);
			return -EFAULT;
		}

		reg_read.value = dma_reg_read(dma_reg_base + reg_read.addr);
		if (ret < 0) {
			fhme_print(PRINT_ERROR,
				   "%s@%s: fh_pcie2pata_ioctl_read_reg1: pci_read_config_dword failed\n",
				   __func__, DRIVER_NAME);
			return -EFAULT;
		}

#ifdef SOC_ENDIAN_NEED_SWAP
		cpu_to_le32s(&reg_read.value);
#endif
		ret = copy_to_user((unsigned long *)arg, &reg_read,
				   sizeof(struct pcie2pata_read_reg));
		if (ret) {
			fhme_print(PRINT_ERROR,
				   "%s@%s: fh_pcie2pata_ioctl_read_reg1: copy to user failed\n",
				   __func__, DRIVER_NAME);
			return -EFAULT;
		}
		ret = 0;
		break;

	case fh_pcie2pata_ioctl_write_reg:
		ret = copy_from_user(&reg_write,
				     (struct pcie2pata_write_reg *)arg,
				     sizeof(struct pcie2pata_write_reg));
		if (ret) {
			fhme_print(PRINT_ERROR,
				   "%s@%s: fh_pcie2pata_ioctl_write_reg: copy from user failed\n",
				   __func__, DRIVER_NAME);
			return -EFAULT;
		}

#ifdef SOC_ENDIAN_NEED_SWAP
		cpu_to_le32s(&reg_write.value);
#endif
		dma_reg_write(dma_reg_base + reg_write.addr, reg_write.value);
		ret = 0;
		break;

	case fh_pcie2pata_ioctl_read_array_ide_reg:
		ret = copy_from_user(&reg_array_read, argp,
				     sizeof(struct pcie2pata_read_array_reg));
		if (ret) {
			fhme_print(PRINT_ERROR,
				   "%s@%s: fh_pcie2pata_ioctl_read_ide_reg: copy from user failed\n",
				   __func__, DRIVER_NAME);
			return -EFAULT;
		}
		mutex_lock(&mutex);
		fh_ide_ctrl_base1->MSRCADDR = reg_array_read.addr + 0x60000000;
		fh_ide_ctrl_base1->MDSTADDR = reg_array_read.dma_addr.pbase;
		fh_ide_ctrl_base1->MSIZE = reg_array_read.read_num;
		fh_ide_ctrl_base1->MCTRL = 0 |
					   0 << 1 |
					   0 << 6 |
					   0 << 12 |
					   0 << 13 |
					   3 << 16 |
					   3 << 21 |
					   0 << 24 |
					   0 << 28;
		fh_ide_ctrl_base1->CHX_IRQ_EN |= ftrans_irq;
		fh_ide_ctrl_base1->MCTRL |= 1;

		i = 0;
		while (((fh_ide_ctrl_base1->CHX_IRQ_STAT & 0x1) == 0) &&
		       (i < 10000)) {
			ndelay(1000);
			i++;
		}

		if (i >= 10000) {
			mutex_unlock(&mutex);
			pr_err("DMA read error\n");
			return -EIO;
		}
		fh_ide_ctrl_base1->CHX_IRQ_CLR |= ftrans_irq;
		mutex_unlock(&mutex);

#ifdef SOC_ENDIAN_NEED_SWAP
		cpu_to_le32s(reg_array_read.dma_addr.vbase);
#endif
		ret = copy_to_user(argp, &reg_array_read,
				   sizeof(struct pcie2pata_read_array_reg));
		if (ret) {
			fhme_print(PRINT_ERROR,
				   "%s@%s: fh_pcie2pata_ioctl_read_ide_reg1: copy to user failed\n",
				   __func__, DRIVER_NAME);
			return -EFAULT;
		}
		ret = 0;
		break;

	case fh_pcie2pata_ioctl_write_array_ide_reg:
		ret = copy_from_user(&reg_array_write, argp,
				     sizeof(struct pcie2pata_write_array_reg));
		if (ret) {
			fhme_print(PRINT_ERROR,
				   "%s@%s: fh_pcie2pata_ioctl_write_ide_reg: copy from user failed\n",
				   __func__, DRIVER_NAME);
			return -EFAULT;
		}
		mutex_lock(&mutex);
		fh_ide_ctrl_base1->MSRCADDR = reg_array_write.dma_addr.pbase;
		fh_ide_ctrl_base1->MDSTADDR = reg_array_write.addr + 0x60000000;
		fh_ide_ctrl_base1->MSIZE = reg_array_write.write_num;
		fh_ide_ctrl_base1->MCTRL = 0 |
					   0 << 1 |
					   0 << 6 |
					   0 << 12 |
					   0 << 13 |
					   3 << 16 |
					   3 << 21 |
					   0 << 24 |
					   0 << 28;
		fh_ide_ctrl_base1->CHX_IRQ_EN |= ftrans_irq;
		fh_ide_ctrl_base1->MCTRL |= 1;
		i = 0;
		while (((fh_ide_ctrl_base1->CHX_IRQ_STAT & 0x1) == 0) &&
		       (i < 10000)) {
			ndelay(1000);
			i++;
		}
		if (i >= 10000) {
			mutex_unlock(&mutex);
			pr_err("DMA write error\n");
			return -EIO;
		}
		fh_ide_ctrl_base1->CHX_IRQ_CLR |= ftrans_irq;
		mutex_unlock(&mutex);
		ret = 0;
		break;

	case fh_pcie2pata_ioctl_get_dma:
		ret = copy_from_user(&dmaaddr, (struct pcie2pata_addr *)arg,
				     sizeof(struct pcie2pata_addr));
		if (ret) {
			fhme_print(PRINT_ERROR,
				   "%s@%s: fh_pcie2pata_ioctl_addr: copy from user failed\n",
				   __func__, DRIVER_NAME);
			return -EFAULT;
		}
		dma_buffers = kmalloc(sizeof(dma_buffer_t), GFP_ATOMIC);
		if (!dma_buffers) {
			pr_err("kmalloc dma_buffers error\n");
			return -ENOMEM;
		}
		memset(dma_buffers, 0, sizeof(dma_buffer_t));

		mutex_lock(&mutex);
		if (dmabuf_list_num >= DMABUF_LIST_MAX_NUM) {
			mutex_unlock(&mutex);
			kfree(dma_buffers);
			pr_err("There are too many buffers already, please free first!\n");
			return -EBUSY;
		}
		for (i = 0; i < DMABUF_LIST_MAX_NUM; i++) {
			if ((dma_buffer_state & (0x1 << i)) == 0)
				break;
		}
		if (i < DMABUF_LIST_MAX_NUM) {
			dmaaddr.pbase = (i * DMA_MAX_SIZE + DMA_BASE_ADDR);
			dmaaddr.vbase = ioremap(dmaaddr.pbase, dmaaddr.size);
			if (!dmaaddr.vbase) {
				mutex_unlock(&mutex);
				pr_err("%s:%d ioremap fail\n",
				       __func__, __LINE__);
				kfree(dma_buffers);
				return -ENOMEM;
			}
			list_add(&dma_buffers->list, &_dma_buf);
			dmabuf_list_num++;
			dma_buffer_state |= 0x1 << i;
			mutex_unlock(&mutex);
			dma_buffers->paddr = dmaaddr.pbase;
			dma_buffers->vaddr = dmaaddr.vbase;
			dma_buffers->size = dmaaddr.size;

		} else {
			mutex_unlock(&mutex);
			kfree(dma_buffers);
			pr_err("dma_buffers is full\n");
			return -EBUSY;
		}
		ret = copy_to_user((unsigned long *)arg, &dmaaddr,
				   sizeof(struct pcie2pata_addr));
		if (ret) {
			fhme_print(PRINT_ERROR,
				   "%s@%s: fh_pcie2pata_ioctl_addr: copy to user failed\n",
				   __func__, DRIVER_NAME);
			iounmap(dmaaddr.vbase);
			kfree(dma_buffers);
			mutex_lock(&mutex);
			list_del(&dma_buffers->list);
			dmabuf_list_num--;
			dma_buffer_state &= ~(0x1 << i);
			mutex_unlock(&mutex);
			return -EFAULT;
		}
		ret = 0;
		break;

	case fh_pcie2pata_ioctl_cleandma:
		ret = copy_from_user(&dmaaddr, (struct pcie2pata_addr *)arg,
				     sizeof(struct pcie2pata_addr));
		if (ret) {
			fhme_print(PRINT_ERROR,
				   "%s@%s: fh_pcie2pata_ioctl_addr: copy from user failed\n",
				   __func__, DRIVER_NAME);
			return -EFAULT;
		}
		ret = -EINVAL;
		mutex_lock(&mutex);
		list_for_each(pos, &_dma_buf) {
			dma_buffers = list_entry(pos, dma_buffer_t, list);
			if (dmaaddr.pbase == (unsigned int)dma_buffers->paddr) {
				iounmap(dma_buffers->vaddr);
				dma_buffer_state &=
					~(0x1 << ((dma_buffers->paddr - DMA_BASE_ADDR) /
						  DMA_MAX_SIZE));
				list_del(&dma_buffers->list);
				kfree(dma_buffers);
				dmabuf_list_num--;
				ret = 0;
				break;
			}
		}
		mutex_unlock(&mutex);
		if (ret != 0)
			pr_err("Can't find the buffer\n");
		break;

	case fh_pcie2pata_ioctl_get_info:
		ret = copy_from_user(&dmaaddr, argp,
				     sizeof(struct pcie2pata_addr));
		if (ret) {
			fhme_print(PRINT_ERROR,
				   "%s@%s: fh_pcie2pata_ioctl_get_info: copy from user failed\n",
				   __func__, DRIVER_NAME);
			return -EFAULT;
		}

		{
			unsigned long tmp = (unsigned long)dmaaddr.new_vbase;

			mutex_lock(&mutex);
			list_for_each(pos, &_dma_buf) {
				ret = -EINVAL;
				dma_buffers = list_entry(pos, dma_buffer_t, list);

				if (tmp == (dma_buffers->useraddr)) {
					dmaaddr.pbase = dma_buffers->paddr;
					dmaaddr.size = dma_buffers->size;
					dmaaddr.vbase = dma_buffers->vaddr;
					ret = 0;
					break;
				}
			}
			mutex_unlock(&mutex);
		}
		if (ret == -EINVAL) {
			pr_err("Can't find dma buffer\n");
			return -EINVAL;
		}
		ret = copy_to_user(argp, &dmaaddr,
				   sizeof(struct pcie2pata_addr));
		if (ret) {
			fhme_print(PRINT_ERROR,
				   "%s@%s: fh_pcie2pata_ioctl_get_info: copy to user failed\n",
				   __func__, DRIVER_NAME);
			return -EFAULT;
		}
		ret = 0;
		break;

	default:
		break;
	}

	return ret;
}

static int fh_pcie2pata_mmap(struct file *file, struct vm_area_struct *vma)
{
	int ret;
	unsigned long off = 0 << PAGE_SHIFT;
	unsigned long pos;
	struct list_head *buf_pos;
	dma_buffer_t *dma_buffers;
	unsigned long start = vma->vm_start;
	unsigned long size = vma->vm_end - vma->vm_start;

	mutex_lock(&mutex);
	list_for_each(buf_pos, &_dma_buf) {
		ret = -EINVAL;
		dma_buffers = list_entry(buf_pos, dma_buffer_t, list);
		if (vma->vm_pgoff == ((dma_buffers->paddr) >> PAGE_SHIFT)) {
			pos = dma_buffers->paddr + off;
			ret = 0;
			break;
		}
	}
	if (ret == -EINVAL) {
		pr_err("%s:Can't find dma buffer\n", __func__);
		mutex_unlock(&mutex);
		return -EINVAL;
	}

	vma->vm_pgoff = pos >> PAGE_SHIFT;

	vma->vm_flags |= VM_SHARED | VM_IO | VM_READ | VM_WRITE;
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	if (remap_pfn_range(vma, start, vma->vm_pgoff, size, vma->vm_page_prot)) {
		fhme_print(PRINT_ERROR, "Enter %s..remap_pfn_range.\n",
			   __func__);
		mutex_unlock(&mutex);
		return -EAGAIN;
	}
	dma_buffers->useraddr = start;
	mutex_unlock(&mutex);

	return 0;
}

static const struct file_operations fh_pcie2pata_fops = {
	.owner		= THIS_MODULE,
	.open		= fh_pcie2pata_open,
	.release	= fh_pcie2pata_release,
	.unlocked_ioctl	= fh_pcie2pata_ioctl,
	.mmap		= fh_pcie2pata_mmap
};

static struct miscdevice fh_pcie2pata_miscdev = {
	.minor		= FH_PCIE2PATA_MINOR,
	.name		= "fh_pcie2pata",
	.fops		= &fh_pcie2pata_fops
};

static int __init fh_pcie2pata_init(void)
{
	int ret;
	unsigned long base_addr = 0x60000000;
	unsigned long len = 0x10000000;
	unsigned long tabledma_base_addr = 0x10017000;
	unsigned long tabledma_len = 0x1000;

	pr_info("switch interface: localbus\n");

	ret = misc_register(&fh_pcie2pata_miscdev);
	if (ret < 0) {
		fhme_print(PRINT_ERROR, "%s@%s: misc register failed, minorID(%d)\n",
			   __func__, DRIVER_NAME, FH_PCIE2PATA_MINOR);
		return ret;
	}

	fh_ide_ctrl_base1 = ioremap(tabledma_base_addr, tabledma_len);
	if (!fh_ide_ctrl_base1) {
		fhme_print(PRINT_ERROR, "%s@%s: ioremap failed\n",
			   __func__, DRIVER_NAME);
		return -ENOMEM;
	}
	dma_reg_base = ioremap(base_addr, len);
	if (!dma_reg_base) {
		fhme_print(PRINT_ERROR, "%s@%s: ioremap failed\n",
			   __func__, DRIVER_NAME);
		return -ENOMEM;
	}

	mutex_init(&mutex);

	dma_buffer_state = 0x0;
	driver_is_present = 1;
	return 0;
}

static void __exit fh_pcie2pata_exit(void)
{
	struct list_head *buf_pos;
	dma_buffer_t *dma_buffers;

	misc_deregister(&fh_pcie2pata_miscdev);

	list_for_each(buf_pos, &_dma_buf) {
		dma_buffers = list_entry(buf_pos, dma_buffer_t, list);
		if (dma_buffers->vaddr != NULL) {
			iounmap(dma_buffers->vaddr);
			list_del(&dma_buffers->list);
			kfree(dma_buffers);
			break;
		}
	}
	dma_buffer_state = 0;

	driver_is_present = 0;
}

module_init(fh_pcie2pata_init);
module_exit(fh_pcie2pata_exit);

MODULE_AUTHOR("Pengfei.Liu@FiberHome");
MODULE_DESCRIPTION("FiberHome Switch Interface Driver");
MODULE_LICENSE("GPL");
