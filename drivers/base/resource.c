// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * (C) Copyright 2000
 * Wolfgang Denk, DENX Software Engineering, wd@denx.de.
 */

#include <common.h>
#include <driver.h>
#include <xfuncs.h>
#include <malloc.h>

struct device *device_alloc(const char *devname, int id)
{
	struct device *dev;

	dev = xzalloc(sizeof(*dev));
	dev_set_name(dev, "%s", devname);
	dev->id = id;

	return dev;
}

int device_add_data(struct device *dev, const void *data, size_t size)
{
	free(dev->platform_data);

	if (data)
		dev->platform_data = xmemdup(data, size);
	else
		dev->platform_data = NULL;

	return 0;
}

int device_add_resources(struct device *dev, const struct resource *res,
			 int num)
{
	dev->resource = xmemdup(res, sizeof(*res) * num);
	dev->num_resources = num;

	return 0;
}

int device_add_resource(struct device *dev, const char *resname,
			resource_size_t start, resource_size_t size,
			unsigned int flags)
{
	struct resource res = {
		.start = start,
		.end = start + size - 1,
		.flags = flags,
	};

	if (resname)
		res.name = xstrdup(resname);

	return device_add_resources(dev, &res, 1);
}

struct device *add_child_device(struct device *parent,
		const char* devname, int id, const char *resname,
		resource_size_t start, resource_size_t size, unsigned int flags,
		void *pdata)
{
	struct device *dev;

	dev = device_alloc(devname, id);
	dev->parent = parent;
	dev->platform_data = pdata;
	device_add_resource(dev, resname, start, size, flags);

	platform_device_register(dev);

	return dev;
}
EXPORT_SYMBOL(add_child_device);

struct device *add_generic_device_res(const char* devname, int id,
		struct resource *res, int nb, void *pdata)
{
	struct device *dev;

	dev = device_alloc(devname, id);
	dev->platform_data = pdata;
	device_add_resources(dev, res, nb);

	platform_device_register(dev);

	return dev;
}
EXPORT_SYMBOL(add_generic_device_res);

#ifdef CONFIG_DRIVER_NET_DM9K
struct device *add_dm9000_device(int id, resource_size_t base,
		resource_size_t data, int flags, void *pdata)
{
	struct resource *res;
	resource_size_t size;

	res = xzalloc(sizeof(struct resource) * 2);

	switch (flags) {
	case IORESOURCE_MEM_32BIT:
		size = 4;
		break;
	case IORESOURCE_MEM_16BIT:
		size = 2;
		break;
	case IORESOURCE_MEM_8BIT:
		size = 1;
		break;
	default:
		printf("dm9000: memory width flag missing\n");
		return NULL;
	}

	res[0].start = base;
	res[0].end = base + size - 1;
	res[0].flags = IORESOURCE_MEM | flags;
	res[1].start = data;
	res[1].end = data + size - 1;
	res[1].flags = IORESOURCE_MEM | flags;

	return add_generic_device_res("dm9000", id, res, 2, pdata);
}
EXPORT_SYMBOL(add_dm9000_device);
#endif

#ifdef CONFIG_USB_EHCI
struct device *add_usb_ehci_device(int id, resource_size_t hccr,
		resource_size_t hcor, void *pdata)
{
	struct resource *res;

	res = xzalloc(sizeof(struct resource) * 2);
	res[0].start = hccr;
	res[0].end = hccr + 0x10 - 1;
	res[0].flags = IORESOURCE_MEM;
	res[1].start = hcor;
	res[1].end = hcor + 0xc0 - 1;
	res[1].flags = IORESOURCE_MEM;

	return add_generic_device_res("ehci", id, res, 2, pdata);
}
EXPORT_SYMBOL(add_usb_ehci_device);
#endif

#ifdef CONFIG_DRIVER_NET_KS8851_MLL
struct device *add_ks8851_device(int id, resource_size_t addr,
		resource_size_t addr_cmd, int flags, void *pdata)
{
	struct resource *res;
	resource_size_t size;

	switch (flags) {
	case IORESOURCE_MEM_16BIT:
		size = 2;
		break;
	case IORESOURCE_MEM_8BIT:
		size = 1;
		break;
	default:
		printf("ks8851: memory width flag missing\n");
		return NULL;
	}

	res = xzalloc(sizeof(struct resource) * 2);

	res[0].start = addr;
	res[0].end = addr + size - 1;
	res[0].flags = IORESOURCE_MEM | flags;
	res[1].start = addr_cmd;
	res[1].end = addr_cmd + size - 1;
	res[1].flags = IORESOURCE_MEM | flags;

	return add_generic_device_res("ks8851_mll", id, res, 2, pdata);
}
EXPORT_SYMBOL(add_ks8851_device);
#endif
