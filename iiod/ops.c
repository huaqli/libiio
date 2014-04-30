/*
 * libiio - Library for interfacing industrial I/O (IIO) devices
 *
 * Copyright (C) 2014 Analog Devices, Inc.
 * Author: Paul Cercueil <paul.cercueil@analog.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * */

#include "ops.h"
#include "parser.h"
#include "../debug.h"
#include "../iio-private.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <sys/queue.h>
#include <time.h>

int yyparse(yyscan_t scanner);

/* Corresponds to a thread reading from a device */
struct ThdEntry {
	SLIST_ENTRY(ThdEntry) next;
	pthread_cond_t cond;
	pthread_mutex_t cond_lock;
	unsigned int nb, sample_size, samples_count;
	ssize_t err;
	struct parser_pdata *pdata;

	uint32_t *mask;
	bool active, is_writer, new_client, wait_for_open;
};

/* Corresponds to an opened device */
struct DevEntry {
	SLIST_ENTRY(DevEntry) next;

	struct iio_device *dev;
	struct iio_buffer *buf;
	unsigned int sample_size, nb_clients;
	bool update_mask;

	/* Linked list of ThdEntry structures corresponding
	 * to all the threads who opened the device */
	SLIST_HEAD(ThdHead, ThdEntry) thdlist_head;
	pthread_mutex_t thdlist_lock;

	pthread_t thd;
	pthread_attr_t attr;

	uint32_t *mask;
	size_t nb_words;
};

struct send_sample_cb_info {
	FILE *out;
	unsigned int cpt;
	uint32_t *mask;
};

struct receive_sample_cb_info {
	FILE *in;
	unsigned int nb_bytes, cpt;
	uint32_t *mask;
};

/* This is a linked list of DevEntry structures corresponding to
 * all the devices which have threads trying to read them */
static SLIST_HEAD(DevHead, DevEntry) devlist_head =
	    SLIST_HEAD_INITIALIZER(DevHead);
static pthread_mutex_t devlist_lock = PTHREAD_MUTEX_INITIALIZER;

static ssize_t write_all(const void *src, size_t len, FILE *out)
{
	const void *ptr = src;
	while (len) {
		ssize_t ret = send(fileno(out), ptr, len, 0);
		if (ret < 0)
			return -errno;
		if (!ret)
			return -EPIPE;
		ptr += ret;
		len -= ret;
	}
	return ptr - src;
}

static ssize_t read_all(void *dst, size_t len, FILE *in)
{
	void *ptr = dst;
	while (len) {
		ssize_t ret = recv(fileno(in), ptr, len, 0);
		if (ret < 0)
			return -errno;
		if (!ret)
			return -EPIPE;
		ptr += ret;
		len -= ret;
	}
	return ptr - dst;
}

static void print_value(struct parser_pdata *pdata, long value)
{
	if (pdata->verbose && value < 0) {
		char buf[1024];
		strerror_r(-value, buf, sizeof(buf));
		output(pdata->out, "ERROR: ");
		output(pdata->out, buf);
		output(pdata->out, "\n");
	} else {
		char buf[128];
		sprintf(buf, "%li\n", value);
		output(pdata->out, buf);
	}
}

static ssize_t send_sample(const struct iio_channel *chn,
		void *src, size_t length, void *d)
{
	struct send_sample_cb_info *info = d;
	if (chn->index < 0 || !TEST_BIT(info->mask, chn->index))
		return 0;

	if (info->cpt % length) {
		unsigned int i, goal = length - info->cpt % length;
		char zero = 0;
		for (i = 0; i < goal; i++)
			send(fileno(info->out), &zero, 1, 0);
		info->cpt += goal;
	}

	info->cpt += length;
	return write_all(src, length, info->out);
}

static ssize_t receive_sample(const struct iio_channel *chn,
		void *dst, size_t length, void *d)
{
	struct receive_sample_cb_info *info = d;
	if (chn->index < 0 || !TEST_BIT(info->mask, chn->index))
		return 0;
	if (info->cpt == info->nb_bytes)
		return 0;

	/* Skip the padding if needed */
	if (info->cpt % length) {
		unsigned int i, goal = length - info->cpt % length;
		char foo;
		for (i = 0; i < goal; i++)
			recv(fileno(info->in), &foo, 1, 0);
		info->cpt += goal;
	}

	info->cpt += length;
	return read_all(dst, length, info->in);
}

static ssize_t send_data(struct DevEntry *dev, struct ThdEntry *thd, size_t len)
{
	FILE *out = thd->pdata->out;
	bool demux = server_demux && dev->sample_size != thd->sample_size;

	if (demux)
		len = (len / dev->sample_size) * thd->sample_size;

	print_value(thd->pdata, len);

	if (thd->new_client) {
		unsigned int i;
		char buf[128];

		/* Send the current mask */
		if (demux)
			for (i = dev->nb_words; i > 0; i--) {
				sprintf(buf, "%08x", thd->mask[i - 1]);
				output(out, buf);
			}
		else
			for (i = dev->nb_words; i > 0; i--) {
				sprintf(buf, "%08x", dev->mask[i - 1]);
				output(out, buf);
			}
		output(out, "\n");
		thd->new_client = false;
	}

	if (!demux) {
		/* Short path */
		return write_all(dev->buf->buffer, len, out);
	} else {
		struct send_sample_cb_info info = {
			.out = out,
			.cpt = 0,
			.mask = thd->mask,
		};

		return iio_buffer_foreach_sample(dev->buf, send_sample, &info);
	}
}

static ssize_t receive_data(struct DevEntry *dev, struct ThdEntry *thd)
{
	struct receive_sample_cb_info info = {
		.in = thd->pdata->in,
		.cpt = 0,
		.nb_bytes = thd->nb,
		.mask = thd->mask,
	};

	/* Inform that no error occured, and that we'll start reading data */
	if (thd->new_client) {
		print_value(thd->pdata, 0);
		thd->new_client = false;
	}

	return iio_buffer_foreach_sample(dev->buf, receive_sample, &info);
}

static void signal_thread(struct ThdEntry *thd, ssize_t ret)
{
	/* Ensure that the client thread is already waiting */
	pthread_mutex_lock(&thd->cond_lock);
	pthread_mutex_unlock(&thd->cond_lock);

	thd->err = ret;
	thd->nb = 0;
	pthread_cond_signal(&thd->cond);
	thd->active = false;
}

static void * read_thd(void *d)
{
	struct DevEntry *entry = d;
	struct ThdEntry *thd;
	struct iio_device *dev = entry->dev;
	unsigned int nb_words = entry->nb_words;
	ssize_t ret = 0;

	DEBUG("Read thread started\n");

	while (true) {
		struct ThdEntry *next_thd;
		bool has_readers = false, has_writers = false,
		     mask_updated = false;
		unsigned int sample_size;

		/* NOTE: this while loop must exit with
		 * devlist_lock and thdlist_lock locked. */
		pthread_mutex_lock(&devlist_lock);
		pthread_mutex_lock(&entry->thdlist_lock);

		if (SLIST_EMPTY(&entry->thdlist_head))
			break;

		if (entry->update_mask) {
			unsigned int i;
			unsigned int samples_count = UINT_MAX;

			memset(entry->mask, 0, nb_words * sizeof(*entry->mask));
			SLIST_FOREACH(thd, &entry->thdlist_head, next) {
				for (i = 0; i < nb_words; i++)
					entry->mask[i] |= thd->mask[i];

				if (thd->samples_count < samples_count)
					samples_count = thd->samples_count;
			}

			if (entry->buf)
				iio_buffer_destroy(entry->buf);

			for (i = 0; i < dev->nb_channels; i++) {
				struct iio_channel *chn = dev->channels[i];
				long index = chn->index;
				if (index >= 0 && TEST_BIT(entry->mask, index))
					iio_channel_enable(chn);
				else
					iio_channel_disable(chn);
			}

			entry->buf = iio_device_create_buffer(dev,
					samples_count);
			if (!entry->buf) {
				ret = -errno;
				ERROR("Unable to create buffer\n");
				break;
			}

			/* Signal the threads that we opened the device */
			SLIST_FOREACH(thd, &entry->thdlist_head, next) {
				if (thd->wait_for_open) {
					signal_thread(thd, 0);
					thd->wait_for_open = false;
				}
			}

			DEBUG("IIO device %s reopened with new mask:\n",
					dev->id);
			for (i = 0; i < nb_words; i++)
				DEBUG("Mask[%i] = 0x%08x\n", i, entry->mask[i]);
			entry->update_mask = false;

			entry->sample_size = iio_device_get_sample_size(dev);
			mask_updated = true;
		}

		sample_size = entry->sample_size;

		SLIST_FOREACH(thd, &entry->thdlist_head, next) {
			thd->active = !thd->err && thd->nb >= sample_size;
			if (mask_updated && thd->active)
				signal_thread(thd, thd->nb);

			if (thd->is_writer)
				has_writers |= thd->active;
			else
				has_readers |= thd->active;
		}

		pthread_mutex_unlock(&entry->thdlist_lock);
		pthread_mutex_unlock(&devlist_lock);

		if (!has_readers && !has_writers) {
			struct timespec ts = {
				.tv_sec = 0,
				.tv_nsec = 1000000, /* 1 ms */
			};

			nanosleep(&ts, NULL);
			continue;
		}

		if (has_readers) {
			ssize_t nb_bytes;

			ret = iio_buffer_refill(entry->buf);
			if (ret < 0) {
				ERROR("Reading from device failed: %i\n",
						(int) ret);
				pthread_mutex_lock(&devlist_lock);
				break;
			}

			nb_bytes = ret;
			pthread_mutex_lock(&entry->thdlist_lock);

			/* We don't use SLIST_FOREACH here. As soon as a thread is
			 * signaled, its "thd" structure might be freed;
			 * SLIST_FOREACH would then cause a segmentation fault, as it
			 * reads "thd" to get the address of the next element. */
			for (thd = SLIST_FIRST(&entry->thdlist_head);
					thd; thd = next_thd) {
				next_thd = SLIST_NEXT(thd, next);

				if (!thd->active || thd->is_writer)
					continue;

				ret = send_data(entry, thd, nb_bytes);
				if (ret > 0)
					thd->nb -= ret;

				if (ret < 0 || thd->nb < sample_size)
					signal_thread(thd, (ret < 0) ?
							ret : thd->nb);
			}

			pthread_mutex_unlock(&entry->thdlist_lock);
		}

		if (has_writers) {
			pthread_mutex_lock(&entry->thdlist_lock);

			/* Reset the size of the buffer to its maximum size */
			entry->buf->data_length = entry->buf->length;

			/* Same comment as above */
			for (thd = SLIST_FIRST(&entry->thdlist_head);
					thd; thd = next_thd) {
				next_thd = SLIST_NEXT(thd, next);

				if (!thd->active || !thd->is_writer)
					continue;

				ret = receive_data(entry, thd);
				if (ret > 0)
					thd->nb -= ret;

				if (ret < 0 || thd->nb < sample_size)
					signal_thread(thd, (ret < 0) ?
							ret : thd->nb);
			}

			pthread_mutex_unlock(&entry->thdlist_lock);

			ret = iio_buffer_push(entry->buf);
			if (ret < 0) {
				ERROR("Writing to device failed: %i\n",
						(int) ret);
				pthread_mutex_lock(&devlist_lock);
				break;
			}
		}
	}

	/* Signal all remaining threads */
	SLIST_FOREACH(thd, &entry->thdlist_head, next) {
		SLIST_REMOVE(&entry->thdlist_head, thd,
				ThdEntry, next);
		signal_thread(thd, ret);
	}
	pthread_mutex_unlock(&entry->thdlist_lock);

	DEBUG("Removing device %s from list\n", dev->id);
	SLIST_REMOVE(&devlist_head, entry, DevEntry, next);

	iio_buffer_destroy(entry->buf);
	pthread_mutex_unlock(&devlist_lock);
	pthread_mutex_destroy(&entry->thdlist_lock);
	pthread_attr_destroy(&entry->attr);

	free(entry->mask);
	free(entry);

	DEBUG("Thread terminated\n");
	return NULL;
}

static ssize_t rw_buffer(struct parser_pdata *pdata,
		struct iio_device *dev, unsigned int nb, bool is_write)
{
	struct DevEntry *e, *entry = NULL;
	struct ThdEntry *t, *thd = NULL;
	ssize_t ret;

	pthread_mutex_lock(&devlist_lock);

	SLIST_FOREACH(e, &devlist_head, next) {
		if (e->dev == dev) {
			entry = e;
			break;
		}
	}

	if (!entry) {
		pthread_mutex_unlock(&devlist_lock);
		return -ENXIO;
	}

	if (nb < entry->sample_size) {
		pthread_mutex_unlock(&devlist_lock);
		return 0;
	}

	pthread_mutex_lock(&entry->thdlist_lock);
	SLIST_FOREACH(t, &entry->thdlist_head, next) {
		if (t->pdata == pdata) {
			thd = t;
			break;
		}
	}

	if (!thd) {
		pthread_mutex_unlock(&entry->thdlist_lock);
		pthread_mutex_unlock(&devlist_lock);
		return -ENXIO;
	}

	if (thd->nb) {
		pthread_mutex_unlock(&entry->thdlist_lock);
		pthread_mutex_unlock(&devlist_lock);
		return -EBUSY;
	}

	thd->new_client = true;
	thd->nb = nb;
	thd->err = 0;
	thd->is_writer = is_write;

	pthread_mutex_unlock(&entry->thdlist_lock);
	pthread_mutex_unlock(&devlist_lock);

	DEBUG("Waiting for completion...\n");
	pthread_cond_wait(&thd->cond, &thd->cond_lock);

	fflush(thd->pdata->out);

	ret = thd->err;
	if (ret > 0 && ret < nb)
		print_value(thd->pdata, 0);

	DEBUG("Exiting rw_buffer with code %li\n", (long) ret);
	if (ret < 0)
		return ret;
	else
		return nb - ret;
}

static struct iio_device * get_device(struct iio_context *ctx, const char *id)
{
	unsigned int i;

	for (i = 0; i < ctx->nb_devices; i++) {
		struct iio_device *dev = ctx->devices[i];
		if (!strcmp(id, dev->id)
				|| (dev->name && !strcmp(id, dev->name)))
			return dev;
	}
	return NULL;
}

static struct iio_channel * get_channel(const struct iio_device *dev,
		const char *id)
{
	unsigned int i;
	for (i = 0; i < dev->nb_channels; i++) {
		struct iio_channel *chn = dev->channels[i];
		if (!strcmp(id, chn->id)
				|| (chn->name && !strcmp(id, chn->name)))
			return chn;
	}
	return NULL;
}

static uint32_t *get_mask(const char *mask, size_t *len)
{
	size_t nb = (*len + 7) / 8;
	uint32_t *ptr, *words = calloc(nb, sizeof(*words));
	if (!words)
		return NULL;

	ptr = words + nb;
	while (*mask) {
		char buf[9];
		sprintf(buf, "%.*s", 8, mask);
		sscanf(buf, "%08x", --ptr);
		mask += 8;
		DEBUG("Mask[%lu]: 0x%08x\n",
				(unsigned long) (words - ptr) / 4, *ptr);
	}

	*len = nb;
	return words;
}

static int open_dev_helper(struct parser_pdata *pdata, struct iio_device *dev,
		size_t samples_count, const char *mask)
{
	int ret = -ENOMEM;
	struct DevEntry *e, *entry = NULL;
	struct ThdEntry *thd;
	size_t len = strlen(mask);
	uint32_t *words;
	unsigned int nb_channels = dev->nb_channels;

	if (len != ((nb_channels + 31) / 32) * 8)
		return -EINVAL;

	words = get_mask(mask, &len);
	if (!words)
		return -ENOMEM;

	if (nb_channels % 32)
		words[nb_channels / 32] &= (1 << (nb_channels % 32)) - 1;

	pthread_mutex_lock(&devlist_lock);
	SLIST_FOREACH(e, &devlist_head, next) {
		if (e->dev == dev) {
			entry = e;
			break;
		}
	}

	thd = malloc(sizeof(*thd));
	if (!thd)
		goto err_free_words;

	thd->wait_for_open = true;
	thd->mask = words;
	thd->nb = 0;
	thd->samples_count = samples_count;
	thd->sample_size = iio_device_get_sample_size_mask(dev, words, len);
	thd->pdata = pdata;
	pthread_cond_init(&thd->cond, NULL);
	pthread_mutex_init(&thd->cond_lock, NULL);
	pthread_mutex_lock(&thd->cond_lock);

	if (entry) {
		pthread_mutex_lock(&entry->thdlist_lock);
		SLIST_INSERT_HEAD(&entry->thdlist_head, thd, next);
		entry->update_mask = true;
		pthread_mutex_unlock(&entry->thdlist_lock);
		DEBUG("Added thread to client list\n");

		pthread_mutex_unlock(&devlist_lock);

		/* Wait until the device is opened by the rw thread */
		pthread_cond_wait(&thd->cond, &thd->cond_lock);
		ret = (int) thd->err;
		if (ret < 0)
			goto err_free_thd;
		return 0;
	}

	entry = malloc(sizeof(*entry));
	if (!entry)
		goto err_free_thd;

	entry->mask = malloc(len * sizeof(*words));
	if (!entry->mask)
		goto err_free_entry;

	entry->nb_words = len;
	entry->update_mask = true;
	entry->dev = dev;
	entry->buf = NULL;
	SLIST_INIT(&entry->thdlist_head);
	SLIST_INSERT_HEAD(&entry->thdlist_head, thd, next);
	DEBUG("Added thread to client list\n");

	pthread_mutex_init(&entry->thdlist_lock, NULL);
	pthread_attr_init(&entry->attr);
	pthread_attr_setdetachstate(&entry->attr,
			PTHREAD_CREATE_DETACHED);

	ret = pthread_create(&entry->thd, &entry->attr,
			read_thd, entry);
	if (ret)
		goto err_free_entry_mask;

	DEBUG("Adding new device thread to device list\n");
	SLIST_INSERT_HEAD(&devlist_head, entry, next);
	pthread_mutex_unlock(&devlist_lock);

	/* Wait until the device is opened by the rw thread */
	pthread_cond_wait(&thd->cond, &thd->cond_lock);
	ret = (int) thd->err;
	if (!ret)
		return 0;

err_free_entry_mask:
	free(entry->mask);
err_free_entry:
	free(entry);
err_free_thd:
	free(thd);
err_free_words:
	free(words);
	pthread_mutex_unlock(&devlist_lock);
	return ret;
}

static int close_dev_helper(struct parser_pdata *pdata, struct iio_device *dev)
{
	struct DevEntry *e;

	pthread_mutex_lock(&devlist_lock);
	SLIST_FOREACH(e, &devlist_head, next) {
		if (e->dev == dev) {
			struct ThdEntry *t;
			pthread_mutex_lock(&e->thdlist_lock);
			SLIST_FOREACH(t, &e->thdlist_head, next) {
				if (t->pdata == pdata) {
					e->update_mask = true;
					SLIST_REMOVE(&e->thdlist_head,
							t, ThdEntry, next);
					free(t->mask);
					free(t);
					pthread_mutex_unlock(&e->thdlist_lock);
					pthread_mutex_unlock(&devlist_lock);
					return 0;
				}
			}

			pthread_mutex_unlock(&e->thdlist_lock);
			pthread_mutex_unlock(&devlist_lock);
			return -ENXIO;
		}
	}

	pthread_mutex_unlock(&devlist_lock);
	return -ENODEV;
}

int open_dev(struct parser_pdata *pdata, const char *id,
		size_t samples_count, const char *mask)
{
	int ret = -ENODEV;
	struct iio_device *dev = get_device(pdata->ctx, id);
	if (dev)
		ret = open_dev_helper(pdata, dev, samples_count, mask);
	print_value(pdata, ret);
	return ret;
}

int close_dev(struct parser_pdata *pdata, const char *id)
{
	int ret = -ENODEV;
	struct iio_device *dev = get_device(pdata->ctx, id);
	if (dev)
		ret = close_dev_helper(pdata, dev);
	print_value(pdata, ret);
	return ret;
}

ssize_t rw_dev(struct parser_pdata *pdata, const char *id,
		unsigned int nb, bool is_write)
{
	struct iio_device *dev = get_device(pdata->ctx, id);
	ssize_t ret;

	if (!dev) {
		print_value(pdata, -ENODEV);
		return -ENODEV;
	}

	ret = rw_buffer(pdata, dev, nb, is_write);
	if (ret <= 0 || is_write)
		print_value(pdata, ret);
	return ret;
}

ssize_t read_dev_attr(struct parser_pdata *pdata,
		const char *id, const char *attr, bool is_debug)
{
	FILE *out = pdata->out;
	struct iio_device *dev = get_device(pdata->ctx, id);
	char buf[1024];
	ssize_t ret = -ENODEV;

	if (dev) {
		if (is_debug)
			ret = iio_device_debug_attr_read(dev,
					attr, buf, sizeof(buf));
		else
			ret = iio_device_attr_read(dev, attr, buf, sizeof(buf));
	}
	print_value(pdata, ret);
	if (ret < 0)
		return ret;

	ret = write_all(buf, ret, out);
	output(out, "\n");
	return ret;
}

ssize_t write_dev_attr(struct parser_pdata *pdata, const char *id,
		const char *attr, const char *value, bool is_debug)
{
	struct iio_device *dev = get_device(pdata->ctx, id);
	ssize_t ret = -ENODEV;

	if (dev) {
		if (is_debug)
			ret = iio_device_debug_attr_write(dev, attr, value);
		else
			ret = iio_device_attr_write(dev, attr, value);
	}
	print_value(pdata, ret);
	return ret;
}

ssize_t read_chn_attr(struct parser_pdata *pdata, const char *id,
		const char *channel, const char *attr)
{
	FILE *out = pdata->out;
	char buf[1024];
	ssize_t ret = -ENODEV;
	struct iio_channel *chn = NULL;
	struct iio_device *dev = get_device(pdata->ctx, id);

	if (dev)
		chn = get_channel(dev, channel);
	if (chn)
		ret = iio_channel_attr_read(chn, attr, buf, sizeof(buf));
	print_value(pdata, ret);
	if (ret < 0)
		return ret;

	ret = write_all(buf, ret, out);
	output(out, "\n");
	return ret;
}

ssize_t write_chn_attr(struct parser_pdata *pdata, const char *id,
		const char *channel, const char *attr, const char *value)
{
	ssize_t ret = -ENODEV;
	struct iio_channel *chn = NULL;
	struct iio_device *dev = get_device(pdata->ctx, id);

	if (dev)
		chn = get_channel(dev, channel);
	if (chn)
		ret = iio_channel_attr_write(chn, attr, value);
	print_value(pdata, ret);
	return ret;
}

ssize_t set_trigger(struct parser_pdata *pdata,
		const char *id, const char *trigger)
{
	ssize_t ret = -ENODEV;
	struct iio_device *dev = get_device(pdata->ctx, id);

	if (dev) {
		if (!trigger) {
			ret = iio_device_set_trigger(dev, NULL);
		} else {
			struct iio_device *trig;
			trig = get_device(pdata->ctx, trigger);
			if (trig)
				ret = iio_device_set_trigger(dev, trig);
			else
				ret = -ENXIO;
		}
	}
	print_value(pdata, ret);
	return ret;
}

ssize_t get_trigger(struct parser_pdata *pdata, const char *id)
{
	ssize_t ret = -ENODEV;
	const struct iio_device *trigger, *dev = get_device(pdata->ctx, id);

	if (dev)
		ret = iio_device_get_trigger(dev, &trigger);
	if (!ret && trigger) {
		ret = strlen(trigger->name);
		print_value(pdata, ret);
		ret = write_all(trigger->name, ret, pdata->out);
		output(pdata->out, "\n");
	} else {
		print_value(pdata, ret);
	}
	return ret;
}

void interpreter(struct iio_context *ctx, FILE *in, FILE *out, bool verbose)
{
	yyscan_t scanner;
	struct parser_pdata pdata;
	unsigned int i;

	pdata.ctx = ctx;
	pdata.stop = false;
	pdata.in = in;
	pdata.out = out;
	pdata.verbose = verbose;

	yylex_init_extra(&pdata, &scanner);
	yyset_out(out, scanner);
	yyset_in(in, scanner);

	do {
		if (verbose) {
			output(out, "iio-daemon > ");
			fflush(out);
		}
		yyparse(scanner);
	} while (!pdata.stop && !feof(in));

	yylex_destroy(scanner);

	/* Close all opened devices */
	for (i = 0; i < ctx->nb_devices; i++)
		close_dev_helper(&pdata, ctx->devices[i]);
}
