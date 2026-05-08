/*
 * Copyright 2016 Ecole Polytechnique Federale Lausanne (EPFL)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St., Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/pci.h>
#include <linux/uaccess.h>

#include "pcidma.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("control bus master (DMA) for PCI devices");

static DEFINE_SPINLOCK(pci_register_lock);

static int pci_registered;

static int pcidma_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	/* This driver responds always negatively to probe requests */
	return -EINVAL;
}

static void pcidma_remove(struct pci_dev *dev)
{
	pci_clear_master(dev);

	dev_info(&dev->dev, "released\n");
}

static struct pci_driver pcidma_driver = {
	.name		= "pcidma",
	.id_table	= NULL,
	.probe		= pcidma_probe,
	.remove		= pcidma_remove,
};

static int pci_register(void)
{
	int ret;

	ret = 0;
	spin_lock(&pci_register_lock);
	if (!pci_registered)
		ret = pci_register_driver(&pcidma_driver);
	if (!ret)
		pci_registered = 1;
	spin_unlock(&pci_register_lock);
	return ret;
}

static void pci_unregister(void)
{
	spin_lock(&pci_register_lock);
	if (pci_registered)
		pci_unregister_driver(&pcidma_driver);
	pci_registered = 0;
	spin_unlock(&pci_register_lock);
}

static int pcidma_enable(struct args_enable *args)
{
	int ret;
	struct pci_loc *loc;
	unsigned int devfn;
	struct pci_dev *dev;

	ret = pci_register();
	if (ret)
		return ret;

	loc = &args->pci_loc;

	devfn = PCI_DEVFN(loc->slot, loc->func);
	dev = pci_get_domain_bus_and_slot(loc->domain, loc->bus, devfn);
	if (!dev) {
		ret = -EINVAL;
		goto out_unregister;
	}

	if (dev->dev.driver) {
		ret = -EBUSY;
		goto out_unregister;
	}

	dev->driver = &pcidma_driver;
	dev->dev.driver = &pcidma_driver.driver;

	if (device_attach(&dev->dev) <= 0) {
		dev->driver = NULL;
		dev->dev.driver = NULL;
		ret = -EINVAL;
		goto out_unregister;
	}

	pci_set_master(dev);

	dev_info(&dev->dev, "claimed\n");

	return 0;

out_unregister:
	pci_unregister();

	return ret;
}

static int pcidma_release(struct inode *inode, struct file *file)
{
	pci_unregister();
	return 0;
}

static long pcidma_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct args_enable args_enable;
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case PCIDMA_ENABLE:
		if (copy_from_user(&args_enable, argp, sizeof(args_enable)))
			return -EFAULT;
		return pcidma_enable(&args_enable);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations pcidma_fops = {
	.release	= pcidma_release,
	.unlocked_ioctl	= pcidma_ioctl,
};

static struct miscdevice pcidma_dev = {
	MISC_DYNAMIC_MINOR,
	"pcidma",
	&pcidma_fops,
};

static int __init pcidma_init(void)
{
	return misc_register(&pcidma_dev);
}

static void __exit pcidma_exit(void)
{
	misc_deregister(&pcidma_dev);
	pci_unregister();
}

module_init(pcidma_init);
module_exit(pcidma_exit);
