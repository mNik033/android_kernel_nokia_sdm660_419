// SPDX-License-Identifier: GPL-2.0
/* mm/ashmem.c
 *
 * Anonymous Shared Memory Subsystem, ashmem
 *
 * Copyright (C) 2008 Google, Inc.
 *
 * Robert Love <rlove@google.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "ashmem: " fmt

#include <linux/init.h>
#include <linux/export.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/uaccess.h>
#include <linux/personality.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/shmem_fs.h>
#include <linux/lockdep.h>
#include <linux/seq_file.h>
#include <linux/capability.h>
#include <linux/slab.h>
#include "ashmem.h"

#define ASHMEM_NAME_PREFIX "dev/ashmem/"
#define ASHMEM_NAME_PREFIX_LEN (sizeof(ASHMEM_NAME_PREFIX) - 1)
#define ASHMEM_FULL_NAME_LEN (ASHMEM_NAME_LEN + ASHMEM_NAME_PREFIX_LEN)

/**
 * struct ashmem_area - The anonymous shared memory area
 * @name:		The optional name in /proc/pid/maps
 * @lock:		The per-area mutex lock
 * @file:		The shmem-based backing file
 * @size:		The size of the mapping, in bytes
 * @prot_mask:		The allowed protection bits, as vm_flags
 *
 * The lifecycle of this structure is from our parent file's open() until
 * its release().
 *
 * Warning: Mappings do NOT pin this structure; It dies on close()
 */
struct ashmem_area {
	char name[ASHMEM_FULL_NAME_LEN];
	struct mutex lock;
	struct file *file;
	size_t size;
	unsigned long prot_mask;
};

static struct kmem_cache *ashmem_area_cachep __read_mostly;

/*
 * Lockdep class key for the backing shmem inode's i_rwsem. Ashmem does not
 * support writing into the backing shmem via write syscalls, so fs_reclaim
 * cannot deadlock against i_rwsem on ashmem backing inodes.
 */
static struct lock_class_key backing_shmem_inode_class;

#define PROT_MASK		(PROT_EXEC | PROT_READ | PROT_WRITE)

/**
 * ashmem_open() - Opens an Anonymous Shared Memory structure
 * @inode:	   The backing file's index node(?)
 * @file:	   The backing file
 *
 * Please note that the ashmem_area is not returned by this function - It is
 * instead written to "file->private_data".
 *
 * Return: 0 if successful, or another code if unsuccessful.
 */
static int ashmem_open(struct inode *inode, struct file *file)
{
	struct ashmem_area *asma;
	int ret;

	ret = generic_file_open(inode, file);
	if (unlikely(ret))
		return ret;

	asma = kmem_cache_alloc(ashmem_area_cachep, GFP_KERNEL);
	if (unlikely(!asma))
		return -ENOMEM;

	mutex_init(&asma->lock);
	asma->file = NULL;
	asma->size = 0;
	asma->prot_mask = PROT_MASK;
	memcpy(asma->name, ASHMEM_NAME_PREFIX, ASHMEM_NAME_PREFIX_LEN);
	asma->name[ASHMEM_NAME_PREFIX_LEN] = '\0';

	file->private_data = asma;

	return 0;
}

/**
 * ashmem_release() - Releases an Anonymous Shared Memory structure
 * @ignored:	      The backing file's Index Node(?) - It is ignored here.
 * @file:	      The backing file
 *
 * Return: 0 if successful. If it is anything else, go have a coffee and
 * try again.
 */
static int ashmem_release(struct inode *ignored, struct file *file)
{
	struct ashmem_area *asma = file->private_data;

	if (asma->file)
		fput(asma->file);
	kmem_cache_free(ashmem_area_cachep, asma);

	return 0;
}

static ssize_t ashmem_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	struct ashmem_area *asma = iocb->ki_filp->private_data;
	struct file *vmfile;
	ssize_t ret;

	/* If size is not set, or set to 0, always return EOF. */
	if (!READ_ONCE(asma->size))
		return 0;

	vmfile = smp_load_acquire(&asma->file);
	if (!vmfile)
		return -EBADF;

	/*
	 * asma and asma->file are used outside the lock here.  We assume
	 * asma->file is only set once and will not be destroyed until all
	 * references to the file are dropped and ashmem_release is called.
	 */
	ret = vfs_iter_read(vmfile, iter, &iocb->ki_pos, 0);
	if (ret > 0)
		vmfile->f_pos = iocb->ki_pos;
	return ret;
}

static loff_t ashmem_llseek(struct file *file, loff_t offset, int origin)
{
	struct ashmem_area *asma = file->private_data;
	struct file *vmfile;
	loff_t ret;

	if (!READ_ONCE(asma->size))
		return -EINVAL;

	vmfile = smp_load_acquire(&asma->file);
	if (!vmfile)
		return -EBADF;

	ret = vfs_llseek(vmfile, offset, origin);
	if (ret < 0)
		return ret;

	/** Copy f_pos from backing file, since f_ops->llseek() sets it */
	file->f_pos = vmfile->f_pos;
	return ret;
}

static inline vm_flags_t calc_vm_may_flags(unsigned long prot)
{
	return _calc_vm_trans(prot, PROT_READ,  VM_MAYREAD) |
	       _calc_vm_trans(prot, PROT_WRITE, VM_MAYWRITE) |
	       _calc_vm_trans(prot, PROT_EXEC,  VM_MAYEXEC);
}

static int ashmem_vmfile_mmap(struct file *file, struct vm_area_struct *vma)
{
	/* do not allow to mmap ashmem backing shmem file directly */
	return -EPERM;
}

static unsigned long
ashmem_vmfile_get_unmapped_area(struct file *file, unsigned long addr,
				unsigned long len, unsigned long pgoff,
				unsigned long flags)
{
	return current->mm->get_unmapped_area(file, addr, len, pgoff, flags);
}

static int ashmem_file_setup(struct ashmem_area *asma,
			     struct vm_area_struct *vma)
{
	static struct file_operations vmfile_fops;
	static DEFINE_SPINLOCK(vmfile_fops_lock);
	const char *name = ASHMEM_NAME_DEF;
	struct file *vmfile;
	struct inode *inode;

	if (asma->name[ASHMEM_NAME_PREFIX_LEN] != '\0')
		name = asma->name;

	/* ... and allocate the backing shmem file */
	vmfile = shmem_file_setup(name, asma->size, vma->vm_flags);
	if (IS_ERR(vmfile))
		return PTR_ERR(vmfile);

	vmfile->f_mode |= FMODE_LSEEK;
	inode = file_inode(vmfile);
	lockdep_set_class(&inode->i_rwsem, &backing_shmem_inode_class);

	/*
	 * override mmap operation of the vmfile so that it can't be
	 * remapped which would lead to creation of a new vma with no
	 * asma permission checks. Have to override get_unmapped_area
	 * as well to prevent VM_BUG_ON check for f_ops modification.
	 */
	if (!smp_load_acquire(&vmfile_fops.mmap)) {
		spin_lock(&vmfile_fops_lock);
		if (!vmfile_fops.mmap) {
			vmfile_fops = *vmfile->f_op;
			vmfile_fops.get_unmapped_area =
				ashmem_vmfile_get_unmapped_area;
			smp_store_release(&vmfile_fops.mmap,
					  ashmem_vmfile_mmap);
		}
		spin_unlock(&vmfile_fops_lock);
	}
	vmfile->f_op = &vmfile_fops;

	smp_store_release(&asma->file, vmfile);
	return 0;
}

static int ashmem_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct ashmem_area *asma = file->private_data;
	int ret = 0;

	mutex_lock(&asma->lock);

	/* user needs to SET_SIZE before mapping */
	if (unlikely(!asma->size)) {
		ret = -EINVAL;
		goto out;
	}

	/* requested mapping size larger than object size */
	if (vma->vm_end - vma->vm_start > PAGE_ALIGN(asma->size)) {
		ret = -EINVAL;
		goto out;
	}

	/* requested protection bits must match our allowed protection mask */
	if (unlikely((vma->vm_flags & ~calc_vm_prot_bits(asma->prot_mask, 0)) &
		     calc_vm_prot_bits(PROT_MASK, 0))) {
		ret = -EPERM;
		goto out;
	}
	vma->vm_flags &= ~calc_vm_may_flags(~asma->prot_mask);

	if (!asma->file) {
		ret = ashmem_file_setup(asma, vma);
		if (ret)
			goto out;
	}
	get_file(asma->file);

	if (vma->vm_flags & VM_SHARED) {
		shmem_set_file(vma, asma->file);
	} else {
		vma_set_anonymous(vma);
		if (vma->vm_file)
			fput(vma->vm_file);
		vma->vm_file = asma->file;
	}

out:
	mutex_unlock(&asma->lock);
	return ret;
}

static int set_prot_mask(struct ashmem_area *asma, unsigned long prot)
{
	int ret = 0;

	mutex_lock(&asma->lock);

	/* the user can only remove, not add, protection bits */
	if (unlikely((asma->prot_mask & prot) != prot)) {
		ret = -EINVAL;
		goto out;
	}

	/* does the application expect PROT_READ to imply PROT_EXEC? */
	if ((prot & PROT_READ) && (current->personality & READ_IMPLIES_EXEC))
		prot |= PROT_EXEC;

	WRITE_ONCE(asma->prot_mask, prot);

out:
	mutex_unlock(&asma->lock);
	return ret;
}

static int set_name(struct ashmem_area *asma, void __user *name)
{
	int len;
	int ret = 0;
	char local_name[ASHMEM_NAME_LEN];

	/*
	 * Holding asma->lock while doing a copy_from_user might cause
	 * a data abort which would try to access mmap_sem. If another
	 * thread has invoked ashmem_mmap then it will be holding the
	 * semaphore and will be waiting for asma->lock, thereby leading to
	 * deadlock. Copy into a stack buffer before taking the lock.
	 */
	len = strncpy_from_user(local_name, name, ASHMEM_NAME_LEN);
	if (len < 0)
		return len;
	if (len == ASHMEM_NAME_LEN)
		local_name[ASHMEM_NAME_LEN - 1] = '\0';

	mutex_lock(&asma->lock);
	/* cannot change an existing mapping's name */
	if (unlikely(asma->file))
		ret = -EINVAL;
	else
		strcpy(asma->name + ASHMEM_NAME_PREFIX_LEN, local_name);

	mutex_unlock(&asma->lock);
	return ret;
}

static int get_name(struct ashmem_area *asma, void __user *name)
{
	int ret = 0;
	size_t len;
	char local_name[ASHMEM_NAME_LEN];

	mutex_lock(&asma->lock);
	if (asma->name[ASHMEM_NAME_PREFIX_LEN] != '\0') {
		/*
		 * Copying only `len', instead of ASHMEM_NAME_LEN, bytes
		 * prevents us from revealing one user's stack to another.
		 */
		len = strlen(asma->name + ASHMEM_NAME_PREFIX_LEN) + 1;
		memcpy(local_name, asma->name + ASHMEM_NAME_PREFIX_LEN, len);
	} else {
		len = sizeof(ASHMEM_NAME_DEF);
		memcpy(local_name, ASHMEM_NAME_DEF, len);
	}
	mutex_unlock(&asma->lock);

	/*
	 * Now we are just copying from the stack variable to userland
	 * No lock held
	 */
	if (unlikely(copy_to_user(name, local_name, len)))
		ret = -EFAULT;
	return ret;
}

static long ashmem_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct ashmem_area *asma = file->private_data;
	long ret = -EINVAL;

	switch (cmd) {
	case ASHMEM_SET_NAME:
		return set_name(asma, (void __user *)arg);
	case ASHMEM_GET_NAME:
		return get_name(asma, (void __user *)arg);
	case ASHMEM_SET_SIZE:
		mutex_lock(&asma->lock);
		if (!asma->file) {
			ret = 0;
			WRITE_ONCE(asma->size, (size_t)arg);
		}
		mutex_unlock(&asma->lock);
		return ret;
	case ASHMEM_GET_SIZE:
		return READ_ONCE(asma->size);
	case ASHMEM_SET_PROT_MASK:
		return set_prot_mask(asma, arg);
	case ASHMEM_GET_PROT_MASK:
		return READ_ONCE(asma->prot_mask);
	case ASHMEM_PIN:
	case ASHMEM_UNPIN:
		return 0;
	case ASHMEM_GET_PIN_STATUS:
		return ASHMEM_IS_PINNED;
	case ASHMEM_PURGE_ALL_CACHES:
		return capable(CAP_SYS_ADMIN) ? 0 : -EPERM;
	}

	return -ENOTTY;
}

/* support of 32bit userspace on 64bit platforms */
#ifdef CONFIG_COMPAT
static long compat_ashmem_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	switch (cmd) {
	case COMPAT_ASHMEM_SET_SIZE:
		cmd = ASHMEM_SET_SIZE;
		break;
	case COMPAT_ASHMEM_SET_PROT_MASK:
		cmd = ASHMEM_SET_PROT_MASK;
		break;
	}
	return ashmem_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}
#endif

#ifdef CONFIG_PROC_FS
static void ashmem_show_fdinfo(struct seq_file *m, struct file *file)
{
	struct ashmem_area *asma = file->private_data;

	mutex_lock(&asma->lock);

	if (asma->file)
		seq_printf(m, "inode:\t%ld\n", file_inode(asma->file)->i_ino);

	if (asma->name[ASHMEM_NAME_PREFIX_LEN] != '\0')
		seq_printf(m, "name:\t%s\n", asma->name);

	mutex_unlock(&asma->lock);
}
#endif

static const struct file_operations ashmem_fops = {
	.owner = THIS_MODULE,
	.open = ashmem_open,
	.release = ashmem_release,
	.read_iter = ashmem_read_iter,
	.llseek = ashmem_llseek,
	.mmap = ashmem_mmap,
	.unlocked_ioctl = ashmem_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = compat_ashmem_ioctl,
#endif
#ifdef CONFIG_PROC_FS
	.show_fdinfo = ashmem_show_fdinfo,
#endif
};

static struct miscdevice ashmem_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ashmem",
	.fops = &ashmem_fops,
};

static int __init ashmem_init(void)
{
	int ret;

	ashmem_area_cachep = kmem_cache_create("ashmem_area_cache",
					       sizeof(struct ashmem_area),
					       0, 0, NULL);
	if (unlikely(!ashmem_area_cachep)) {
		pr_err("failed to create slab cache\n");
		ret = -ENOMEM;
		goto out;
	}

	ret = misc_register(&ashmem_misc);
	if (unlikely(ret)) {
		pr_err("failed to register misc device!\n");
		goto out_free;
	}

	pr_info("initialized\n");

	return 0;

out_free:
	kmem_cache_destroy(ashmem_area_cachep);
out:
	return ret;
}
device_initcall(ashmem_init);
