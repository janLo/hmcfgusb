/* HM-CFG-USB libusb-driver
 *
 * Copyright (c) 2013 Michael Gernoth <michael@gernoth.net>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <math.h>
#include <poll.h>
#include <errno.h>
#include <libusb-1.0/libusb.h>

#include "hexdump.h"
#include "hmcfgusb.h"

#define USB_TIMEOUT	10000

#define ID_VENDOR	0x1b1f
#define ID_PRODUCT	0xc00f

/* TODO: dynamic */
#define ASYNC_SIZE	0x0040
#define ASYNC_INTERVAL	32

#define EP_OUT		0x02
#define EP_IN		0x83

#define INTERFACE	0

static int quit = 0;
static int debug = 0;

/* Not in all libusb-1.0 versions, so we have to roll our own :-( */
static char * usb_strerror(int e)
{
	static char unknerr[256];

	switch (e) {
		case LIBUSB_SUCCESS:
			return "Success";
		case LIBUSB_ERROR_IO:
			return "Input/output error";
		case LIBUSB_ERROR_INVALID_PARAM:
			return "Invalid parameter";
		case LIBUSB_ERROR_ACCESS:
			return "Access denied (insufficient permissions)";
		case LIBUSB_ERROR_NO_DEVICE:
			return "No such device (it may have been disconnected)";
		case LIBUSB_ERROR_NOT_FOUND:
			return "Entity not found";
		case LIBUSB_ERROR_BUSY:
			return "Resource busy";
		case LIBUSB_ERROR_TIMEOUT:
			return "Operation timed out";
		case LIBUSB_ERROR_OVERFLOW:
			return "Overflow";
		case LIBUSB_ERROR_PIPE:
			return "Pipe error";
		case LIBUSB_ERROR_INTERRUPTED:
			return "System call interrupted (perhaps due to signal)";
		case LIBUSB_ERROR_NO_MEM:
			return "Insufficient memory";
		case LIBUSB_ERROR_NOT_SUPPORTED:
			return "Operation not supported or unimplemented on this platform";
		case LIBUSB_ERROR_OTHER:
			return "Other error";
	};
	snprintf(unknerr, sizeof(unknerr), "Unknown error code %d / 0x%02x", e, e);
	return unknerr;
}

static libusb_device_handle *hmcfgusb_find() {
	libusb_device_handle *devh = NULL;
	libusb_device **list;
	ssize_t cnt;
	ssize_t i;
	int err;

	cnt = libusb_get_device_list(NULL, &list);
	if (cnt < 0) {
		fprintf(stderr, "Can't get USB device list: %d\n", (int)cnt);
		return NULL;
	}

	for (i = 0; i < cnt; i++){
		struct libusb_device_descriptor desc;

		err = libusb_get_device_descriptor(list[i], &desc);
		if (err)
			continue;

		if ((desc.idVendor == ID_VENDOR) && (desc.idProduct == ID_PRODUCT)) {
			libusb_device *dev = list[i];

			err = libusb_open(dev, &devh);
			if (err) {
				fprintf(stderr, "Can't open device: %s\n", usb_strerror(err));
				return NULL;
			}

			err = libusb_detach_kernel_driver(devh, INTERFACE);
			if ((err != 0) && (err != LIBUSB_ERROR_NOT_FOUND)) {
				fprintf(stderr, "Can't detach kernel driver: %s\n", usb_strerror(err));
				return NULL;
			}

			err = libusb_claim_interface(devh, INTERFACE);
			if ((err != 0)) {
				fprintf(stderr, "Can't claim interface: %s\n", usb_strerror(err));
				return NULL;
			}

			return devh;
		}

	}

	return NULL;
}

int hmcfgusb_send(struct hmcfgusb_dev *usbdev, unsigned char* send_data, int len, int done)
{
	int err;
	int cnt;

	if (debug)
		hexdump(send_data, len, "USB < ");
	err = libusb_interrupt_transfer(usbdev->usb_devh, EP_OUT, send_data, len, &cnt, USB_TIMEOUT);
	if (err) {
		fprintf(stderr, "Can't send data: %s\n", usb_strerror(err));
		return 0;
	}

	if (done) {
		err = libusb_interrupt_transfer(usbdev->usb_devh, EP_OUT, send_data, 0, &cnt, USB_TIMEOUT);
		if (err) {
			fprintf(stderr, "Can't send data: %s\n", usb_strerror(err));
			return 0;
		}
	}

	return 1;
}

static struct libusb_transfer *hmcfgusb_prepare_int(libusb_device_handle *devh, libusb_transfer_cb_fn cb, void *data)
{
	unsigned char *data_buf;
	struct libusb_transfer *transfer;
	int err;

	data_buf = malloc(ASYNC_SIZE);
	if (!data_buf) {
		fprintf(stderr, "Can't allocate memory for data-buffer!\n");
		return NULL;
	}

	transfer = libusb_alloc_transfer(0);
	if (!transfer) {
		fprintf(stderr, "Can't allocate memory for usb-transfer!\n");
		free(data_buf);
		return NULL;
	}

	libusb_fill_interrupt_transfer(transfer, devh, EP_IN,
			data_buf, ASYNC_SIZE, cb, data, USB_TIMEOUT);

	transfer->flags = LIBUSB_TRANSFER_SHORT_NOT_OK | LIBUSB_TRANSFER_FREE_BUFFER;

	err = libusb_submit_transfer(transfer);
	if (err != 0) {
		fprintf(stderr, "Can't submit transfer: %s\n", usb_strerror(err));
		libusb_free_transfer(transfer);
		return NULL;
	}

	return transfer;
}

struct hmcfgusb_cb_data {
	struct hmcfgusb_dev *dev;
	hmcfgusb_cb_fn cb;
	void *data;
};

static void LIBUSB_CALL hmcfgusb_interrupt(struct libusb_transfer *transfer)
{
	int err;
	struct hmcfgusb_cb_data *cb_data;

	cb_data = transfer->user_data;

	if (transfer->status != LIBUSB_TRANSFER_COMPLETED) {
		if (transfer->status != LIBUSB_TRANSFER_TIMED_OUT) {
			fprintf(stderr, "Interrupt transfer not completed: %d!\n", transfer->status);
			quit = EIO;

			if (cb_data && cb_data->dev && cb_data->dev->transfer) {
				libusb_free_transfer(cb_data->dev->transfer);
				cb_data->dev->transfer = NULL;
			}
			return;
		}
	} else {
		if (cb_data && cb_data->cb) {
			if (debug)
				hexdump(transfer->buffer, transfer->actual_length, "USB > ");

			if (!cb_data->cb(transfer->buffer, transfer->actual_length, cb_data->data)) {
				quit = EIO;

				if (cb_data && cb_data->dev && cb_data->dev->transfer) {
					libusb_free_transfer(cb_data->dev->transfer);
					cb_data->dev->transfer = NULL;
				}

				return;
			}
		} else {
			hexdump(transfer->buffer, transfer->actual_length, "> ");
		}
	}

	err = libusb_submit_transfer(transfer);
	if (err != 0) {
		fprintf(stderr, "Can't re-submit transfer: %s\n", usb_strerror(err));
		libusb_free_transfer(transfer);
	}
}

struct hmcfgusb_dev *hmcfgusb_init(hmcfgusb_cb_fn cb, void *data)
{
	libusb_device_handle *devh = NULL;
	const struct libusb_pollfd **usb_pfd = NULL;
	struct hmcfgusb_dev *dev = NULL;
	struct hmcfgusb_cb_data *cb_data = NULL;
	int err;
	int i;

	err = libusb_init(NULL);
	if (err != 0) {
		fprintf(stderr, "Can't initialize libusb: %s\n", usb_strerror(err));
		return NULL;
	}

	devh = hmcfgusb_find();
	if (!devh) {
		fprintf(stderr, "Can't find/open hmcfgusb!\n");
		return NULL;
	}

	dev = malloc(sizeof(struct hmcfgusb_dev));
	if (!dev) {
		perror("Can't allocate memory for hmcfgusb_dev");
		return NULL;
	}

	memset(dev, 0, sizeof(struct hmcfgusb_dev));
	dev->usb_devh = devh;

	cb_data = malloc(sizeof(struct hmcfgusb_cb_data));
	if (!cb_data) {
		perror("Can't allocate memory for hmcfgusb_cb_data");
		return NULL;
	}

	memset(cb_data, 0, sizeof(struct hmcfgusb_cb_data));

	cb_data->dev = dev;
	cb_data->cb = cb;
	cb_data->data = data;

	dev->transfer = hmcfgusb_prepare_int(devh, hmcfgusb_interrupt, cb_data);
	if (!dev->transfer) {
		fprintf(stderr, "Can't prepare async device io!\n");
		return NULL;
	}

	usb_pfd = libusb_get_pollfds(NULL);
	if (!usb_pfd) {
		fprintf(stderr, "Can't get FDset from libusb!\n");
		free(dev);
		return NULL;
	}

	dev->n_usb_pfd = 0;
	for(i = 0; usb_pfd[i]; i++)
		dev->n_usb_pfd++;

	dev->pfd = malloc(dev->n_usb_pfd * sizeof(struct pollfd));
	if (!dev->pfd) {
		perror("Can't allocate memory for poll-fds");
		return NULL;
	}

	memset(dev->pfd, 0, dev->n_usb_pfd * sizeof(struct pollfd));

	for (i = 0; i < dev->n_usb_pfd; i++) {
		dev->pfd[i].fd = usb_pfd[i]->fd;
		dev->pfd[i].events = usb_pfd[i]->events;
		dev->pfd[i].revents = 0;
	}

	free(usb_pfd);

	dev->n_pfd = dev->n_usb_pfd;

	quit = 0;

	return dev;
}

int hmcfgusb_add_pfd(struct hmcfgusb_dev *dev, int fd, short events)
{
	dev->n_pfd++;
	dev->pfd = realloc(dev->pfd, dev->n_pfd * sizeof(struct pollfd));
	if (!dev->pfd) {
		perror("Can't realloc poll-fds");
		return 0;
	}

	dev->pfd[dev->n_pfd-1].fd = fd;
	dev->pfd[dev->n_pfd-1].events = events;
	dev->pfd[dev->n_pfd-1].revents = 0;

	return 1;
}

int hmcfgusb_poll(struct hmcfgusb_dev *dev, int timeout)
{
	struct timeval tv;
	int usb_event = 0;
	int i;
	int n;
	int fd_n;
	int err;

	errno = 0;

	memset(&tv, 0, sizeof(tv));
	err = libusb_get_next_timeout(NULL, &tv);
	if (err < 0) {
		fprintf(stderr, "libusb_get_next_timeout: %s\n", usb_strerror(err));
		errno = EIO;
		return -1;
	} else if (err == 0) {
		/* No pending timeout or a sane platform */
		tv.tv_sec = timeout;
	} else {
		if ((tv.tv_sec == 0) && (tv.tv_usec == 0)) {
			usb_event = 1;
		}
	}

	if (!usb_event) {
		for (i = 0; i < dev->n_pfd; i++) {
			dev->pfd[i].revents = 0;
		}

		n = poll(dev->pfd, dev->n_pfd, tv.tv_sec * 1000);
		if (n < 0) {
			perror("poll");
			errno = 0;
			return -1;
		} else if (n == 0) {
			usb_event = 1;
		} else {
			for (fd_n = 0; fd_n < dev->n_pfd; fd_n++) {
				if (dev->pfd[fd_n].revents) {
					if (fd_n < dev->n_usb_pfd) {
						usb_event = 1;
						break;
					} else {
						errno = 0;
						return dev->pfd[fd_n].fd;
					}
				}
			}
		}
	}

	if (usb_event) {
		memset(&tv, 0, sizeof(tv));
		err = libusb_handle_events_timeout_completed(NULL, &tv, NULL);
		if (err < 0) {
			fprintf(stderr, "libusb_handle_events_timeout_completed: %s\n", usb_strerror(err));
			errno = EIO;
			return -1;
		}
	}

	errno = 0;
	if (quit) {
		fprintf(stderr, "closing device-connection due to error %d\n", quit);
		errno = quit;
	}

	return -1;
}

void hmcfgusb_close(struct hmcfgusb_dev *dev)
{
	int err;

	if (dev->transfer) {
		libusb_cancel_transfer(dev->transfer);
	}

	err = libusb_release_interface(dev->usb_devh, INTERFACE);
	if ((err != 0)) {
		fprintf(stderr, "Can't release interface: %s\n", usb_strerror(err));
	}

	libusb_close(dev->usb_devh);
	free(dev->pfd);
	free(dev);

	libusb_exit(NULL);
}

void hmcfgusb_set_debug(int d)
{
	debug = d;
}
