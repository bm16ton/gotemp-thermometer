/*
 * USB GoTemp driver
 *
 * Copyright (C) 2005 Greg Kroah-Hartman (greg@kroah.com)
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License as
 *	published by the Free Software Foundation, version 2.
 *
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/uaccess.h> //for copy_to/from_user
#include <linux/device.h>

#include <linux/fs.h>
#include <linux/spinlock.h>

#define DRIVER_AUTHOR "Greg Kroah-Hartman, greg@kroah.com"
#define DRIVER_DESC "USB GoTemp driver"

#define VENDOR_ID	0x08f7
#define PRODUCT_ID	0x0002

static spinlock_t mylock;

static int     dev_open(struct inode *, struct file *);
static int     dev_release(struct inode *, struct file *);
static ssize_t dev_read(struct file *, char *, size_t, loff_t *);

#define USB_GOTEMP_MINOR_BASE	192

//check linux/fs.h for full details of below struct.
static struct file_operations gotemp_fops =
{
   .open = dev_open,
   .read = dev_read,
   .release = dev_release
};

static struct usb_class_driver gotemp_class = {
	.name =		"gotemp%d",
	.fops =		&gotemp_fops,
	.minor_base =	USB_GOTEMP_MINOR_BASE,
};

/* table of devices that work with this driver */
static struct usb_device_id id_table[] = {
	{ USB_DEVICE(VENDOR_ID, PRODUCT_ID) },
	{ },
};
MODULE_DEVICE_TABLE(usb, id_table);

struct gotemp {
	struct usb_device *udev;
	struct device *dev;
	int temperature;
	unsigned char *int_in_buffer;
	__u8 int_in_endpointAddr;
	struct urb *int_in_urb;
	struct file_operations gotemp_fops;
};

#define CMD_ID_GET_STATUS			0x10
#define CMD_ID_WRITE_LOCAL_NV_MEM_1BYTE		0x11
#define CMD_ID_WRITE_LOCAL_NV_MEM_2BYTES	0x12
#define CMD_ID_WRITE_LOCAL_NV_MEM_3BYTES	0x13
#define CMD_ID_WRITE_LOCAL_NV_MEM_4BYTES	0x14
#define CMD_ID_WRITE_LOCAL_NV_MEM_5BYTES	0x15
#define CMD_ID_WRITE_LOCAL_NV_MEM_6BYTES	0x16
#define CMD_ID_READ_LOCAL_NV_MEM		0x17
#define CMD_ID_START_MEASUREMENTS		0x18
#define CMD_ID_STOP_MEASUREMENTS		0x19
#define CMD_ID_INIT				0x1A
#define CMD_ID_SET_MEASUREMENT_PERIOD		0x1B
#define CMD_ID_GET_MEASUREMENT_PERIOD		0x1C
#define CMD_ID_SET_LED_STATE			0x1D
#define CMD_ID_GET_LED_STATE			0x1E
#define CMD_ID_GET_SERIAL_NUMBER		0x20

char test[] = {"00000"};

uint32_t btemp = 0;
int charToInt(const char *s);

struct output_packet {
	u8	cmd;
	u8	params[7];
} __attribute__ ((packed));

struct measurement_packet {
	u8	measurements_in_packet;
	u8	rolling_counter;
	__le16	measurement0;
	__le16	measurement1;
	__le16	measurement2;
} __attribute__ ((packed));

static int send_cmd(struct gotemp *gdev, u8 cmd)
{
	struct output_packet *pkt;
	int retval;

	pkt = kzalloc(sizeof(*pkt), GFP_KERNEL);
	if (!pkt)
		return -ENOMEM;
	pkt->cmd = cmd;

	retval = usb_control_msg(gdev->udev,
				 usb_sndctrlpipe(gdev->udev, 0),
				 0x09,		/* bRequest = SET_REPORT */
				 0x21,		/* bRequestType = 00100001 */
				 0x0200,	/* or is it 0x0002? */
				 0x0000,	/* interface 0 */
				 pkt, sizeof(*pkt), 10000);
	dev_dbg(&gdev->udev->dev, "retval=%d\n", retval);
	if (retval == sizeof(*pkt))
		retval = 0;

	kfree(pkt);
	return retval;
}

static void init_dev(struct gotemp *gdev)
{
	int retval;

	/* First send an init message */
	send_cmd(gdev, CMD_ID_INIT);

	/* hack hack hack */
	/* problem is, we want a usb_interrupt_msg() call to read the interrupt
	 * endpoint right now.  only after it is flushed, can we properly start
	 * up the measurements.  */
	msleep(1000);

	/* kick off interrupt urb */
	retval = usb_submit_urb(gdev->int_in_urb, GFP_KERNEL);
	if (retval)
		dev_err(&gdev->udev->dev,
			"%s - Error %d submitting interrupt urb\n",
			__func__, retval);

	msleep(3000);
	send_cmd(gdev, CMD_ID_START_MEASUREMENTS);
}

static ssize_t show_temp(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct usb_interface *intf = to_usb_interface(dev);
	struct gotemp *gdev = usb_get_intfdata(intf);
//    int yuptemp = (gdev->temperature / 128);
//	btemp = (gdev->temperature / 128);
    printk("from probe raw data %d\n", gdev->temperature); 
	btemp = (((gdev->temperature / 128) * 9) / 5) + 32;
	return sprintf(buf, "%d\n", btemp);
}

static DEVICE_ATTR(temperature, S_IRUGO, show_temp, NULL);

#include "linux/ctype.h"

int charToInt(const char *s)
{
  int n;
  unsigned char sign = 0;

  while (isspace(*s))
  {
    s++;
  }

  if (*s == '-')
  {
    sign = 1;
    s++;
  }
  else if (*s == '+')
  {
    s++;
  }

  n=0;

  while (isdigit(*s))
  {
    n = n * 10 + *s++ - '0';
  }

  return sign ? -n : n;
}


static void read_int_callback(struct urb *urb)
{
	struct gotemp *gdev = urb->context;
//	unsigned char *data = urb->transfer_buffer;
	struct measurement_packet *measurement = urb->transfer_buffer;
	int retval;
//	int i;
	uint32_t btemp2;
	uint32_t btemp3;
	uint32_t btemp4;

//	char poo[] = {"00000"};
	switch (urb->status) {
	case 0:
		/* success */
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		/* this urb is terminated, clean up */
		dev_dbg(&urb->dev->dev, "%s - urb shutting down with status: %d",
		    __func__, urb->status);
		return;
	default:
		dev_dbg(&urb->dev->dev, "%s - nonzero urb status received: %d",
		    __func__, urb->status);
		goto exit;
	}

//	dev_info(&urb->dev->dev, "int read data: ");
//	for (i = 0; i < urb->actual_length; ++i)
	//	printk("%02x ", data[i]);

	//printk("\n");

//	dev_dbg(&urb->dev->dev, "counter %d, temperature=%d\n",
//		 measurement->rolling_counter,
//		 measurement->measurement0);
	btemp = measurement->measurement0;
//	printk("btemp %d\n", btemp);
	gdev->temperature = le16_to_cpu(measurement->measurement0);
//	printk("gdev->temperature %d\n", gdev->temperature);
	btemp2 = (((gdev->temperature / 128) * 9) / 5) + 32;
	btemp3 = (btemp * 100);
//	printk("btemp3 %d\n", btemp3);
	btemp4 = (((btemp3 / 128) * 9) / 5) + 3200;
	uint32_t btemp42 = btemp4;
//	printk("btemp4 %d\n", btemp4);
//	printk("btemp2 %d\n", btemp2);

    int mod = btemp4 % 10;  //split last digit from number
	btemp4 = btemp4 / 10;
	int mod2 = btemp4 % 10;
	int mod3 = ((mod2 * 10) + mod);
	int mod4 = (btemp42 / 100);
//		printk("after dec %d\n", mod3);
	sprintf(test, "%d.%d\n", mod4, mod3);
//	sprintf(test, "%d\n", btemp2);
    
exit:
	retval = usb_submit_urb(urb, GFP_ATOMIC);
	if (retval)
		dev_err(&urb->dev->dev,
			"%s - Error %d submitting interrupt urb\n",
			__func__, retval);
}

static int gotemp_probe(struct usb_interface *interface,
			const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(interface);
	struct gotemp *gdev = NULL;
	int retval = -ENOMEM;
	int i;
	struct usb_host_interface *iface_desc;
	struct usb_endpoint_descriptor *endpoint = NULL;
	size_t buffer_size = 0;

   spin_lock_init(&mylock);

	gdev = kzalloc(sizeof(*gdev), GFP_KERNEL);
	if (gdev == NULL) {
		dev_err(&interface->dev, "Out of memory\n");
		goto error;
	}

	gdev->udev = usb_get_dev(udev);

	/* find the one control endpoint of this device */
	iface_desc = interface->cur_altsetting;
	for (i = 0; i < iface_desc->desc.bNumEndpoints; ++i) {
		endpoint = &iface_desc->endpoint[i].desc;

		if (usb_endpoint_is_int_in(endpoint)) {
			buffer_size = le16_to_cpu(endpoint->wMaxPacketSize);
			gdev->int_in_endpointAddr = endpoint->bEndpointAddress;
			gdev->int_in_buffer = kmalloc(buffer_size, GFP_KERNEL);
			if (!gdev->int_in_buffer) {
				dev_err(&interface->dev,
					"Could not allocate buffer");
				goto error;
			}
			break;
		}
	}
	if (!gdev->int_in_endpointAddr) {
		dev_err(&interface->dev, "Could not find int-in endpoint");
		retval = -ENODEV;
		goto error;
	}

	gdev->int_in_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!gdev->int_in_urb) {
		dev_err(&interface->dev, "No free urbs available\n");
		goto error;
	}
	usb_fill_int_urb(gdev->int_in_urb, udev,
			 usb_rcvintpipe(udev,
					endpoint->bEndpointAddress),
			 gdev->int_in_buffer, buffer_size,
			 read_int_callback, gdev,
			 endpoint->bInterval);

	usb_set_intfdata(interface, gdev);

	/* we can register the device now, as it is ready */
	retval = usb_register_dev(interface, &gotemp_class);
	if (retval) {
		/* something prevented us from registering this driver */
		dev_err(&interface->dev,
			"Not able to get a minor for this device.\n");
		usb_set_intfdata(interface, NULL);
		goto error;
	}

	/* let the user know what node this device is now attached to */
	dev_info(&interface->dev,
		 "USB gotemp device now attached to gotemp-%d",
		 interface->minor);
	init_dev(gdev);

	/*
	 * this must come last - after this call the device is active
	 * if we delayed any initialization until after this, the user
	 * would read garbage
	 */
	retval = device_create_file(&interface->dev, &dev_attr_temperature);
	if (retval)
		goto error;

    gdev->dev = &interface->dev;

	dev_info(&interface->dev, "USB GoTemp device now attached\n");
	return 0;

error:
	usb_set_intfdata(interface, NULL);
	if (gdev) {
		usb_free_urb(gdev->int_in_urb);
		kfree(gdev->int_in_buffer);
	}
	kfree(gdev);
	return retval;
}

static void gotemp_disconnect(struct usb_interface *interface)
{
	struct gotemp *gdev;

	gdev = usb_get_intfdata(interface);

	device_remove_file(&interface->dev, &dev_attr_temperature);
	/* intfdata must remain valid while reads are under way */
	usb_set_intfdata(interface, NULL);

	usb_put_dev(gdev->udev);

	usb_kill_urb(gdev->int_in_urb);
	usb_free_urb(gdev->int_in_urb);
	kfree(gdev->int_in_buffer);
	kfree(gdev);


   usb_deregister_dev(interface, &gotemp_class);
//   unregister_chrdev(Major, gotemp);

	dev_info(&interface->dev, "USB GoTemp now disconnected\n");
}

static struct usb_driver gotemp_driver = {
	.name =		"gotemp",
	.probe =	gotemp_probe,
	.disconnect =	gotemp_disconnect,
	.id_table =	id_table,
};

static int __init gotemp_init(void)
{
	int retval = 0;

	retval = usb_register(&gotemp_driver);
	if (retval)
		printk(KERN_ERR "usb_register failed. Error number %d", retval);
	return retval;
}

static int
dev_open(struct inode *in, struct file *fl)
{
   printk(KERN_INFO "go-temp is opened\n");

   return 0;
}

static int
dev_release(struct inode *in, struct file *fl)
{
   printk(KERN_INFO "go-temp is closed, applied close(fd)?\n");

   return 0;
}

static ssize_t
dev_read(struct file *f, char *buf, size_t len, loff_t *offset)
{
	
    if (copy_to_user(buf, test, sizeof(test)) != 0)
     {
        printk(KERN_ALERT "Copy to user error!");
        msleep(1000);
        return -ENOMEM;
     }

    msleep(1000);
    return sizeof(test);
}

static void __exit gotemp_exit(void)
{
	usb_deregister(&gotemp_driver);
}

module_init(gotemp_init);
module_exit(gotemp_exit);

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
