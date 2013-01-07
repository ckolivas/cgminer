#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>

#include "picominer.h"

#define LINUX 1
#define POSIX 1
#include <picodrv.h>
#include <pico_errors.h>


static const char *		bitfile_x501_virtex6_240t	= "hackjealousy-v0.5.3.bit";
static const unsigned int	model_m501			= 0x501;

typedef struct {
	const char *		bitstream_filename;
	const unsigned int	model;
} supported_device_t;

static const supported_device_t s_supported_devices[] = {
	{ bitfile_x501_virtex6_240t, model_m501 },
	{ 0, 0 }
};


static int picominer_init_lists(picominer_device *d) {

	pthread_mutex_init(&d->nonce_lock, 0);
	if(!(d->nonce_list = (data_list_t *)malloc(sizeof(data_list_t)))) {
		perror("malloc");
		return -1;
	}
	memset(d->nonce_list, 0, sizeof(data_list_t));
	
	return 0;
}


static void picominer_delete_lists(picominer_device *d) {

	data_list_t *l, *t;

	pthread_mutex_lock(&d->nonce_lock);
	for(l = d->nonce_list; l;) {
		t = l;
		l = l->next;
		free(t);
	}
	d->nonce_list = 0;
	pthread_mutex_unlock(&d->nonce_lock);
}


static int picominer_put_list(data_list_t *list, pthread_mutex_t *m, uint32_t d) {

	data_list_t *l;

	pthread_mutex_lock(m);
	if(!list) {
		pthread_mutex_unlock(m);
		return -1;
	}
	for(l = list; l->next; l = l->next);
	if(!(l->next = (data_list_t *)malloc(sizeof(data_list_t)))) {
		perror("malloc");
		pthread_mutex_unlock(m);
		return -1;
	}
	memset(l->next, 0, sizeof(data_list_t));
	l->next->data = d;
	pthread_mutex_unlock(m);
	return 0;
}


static int picominer_put_nonce(picominer_device *dev, uint32_t n) {

	return picominer_put_list(dev->nonce_list, &dev->nonce_lock, n);
}


static int picominer_get_list(data_list_t *list, pthread_mutex_t *m, uint32_t *d) {

	data_list_t *l;

	pthread_mutex_lock(m);
	if(!list) {
		pthread_mutex_unlock(m);
		return -1;
	}
	l = list->next;
	if(!l) {
		pthread_mutex_unlock(m);
		return 0;
	}
	*d = l->data;
	list->next = l->next;
	pthread_mutex_unlock(m);
	free(l);
	return 1;
}


int picominer_get_nonce(picominer_device *dev, uint32_t *n) {

	return picominer_get_list(dev->nonce_list, &dev->nonce_lock, n);
}


int picominer_has_nonce(picominer_device *dev) {

	pthread_mutex_lock(&dev->nonce_lock);
	if((!dev->nonce_list) || (!dev->nonce_list->next)) {
		pthread_mutex_unlock(&dev->nonce_lock);
		return 0;
	}
	pthread_mutex_unlock(&dev->nonce_lock);
	return 1;
}


static picominer_dev_list *new_dev_list() {

	picominer_dev_list *l = (picominer_dev_list *)malloc(sizeof(*l));
	if(!l)
		return 0;
	memset(l, 0, sizeof(*l));

	return l;
}


int picominer_loads() {

	return 1;
}


void picominer_destroy_device(picominer_device *device) {

	PicoDrv *pd;

	if(!device)
		return;

	if(device->read_threadid) {
		pthread_cancel(*(pthread_t *)(device->read_threadid));
		free(device->read_threadid);
	}

	picominer_delete_lists(device);

	pd = (PicoDrv *)device->pd;
	if(device->streamd >= 0) {
		pd->CloseStream(device->streamd);
		device->streamd = -1;
	}
	if(pd)
		delete pd;
	free(device);
}


void picominer_destroy_device_list(picominer_dev_list *dl) {

	picominer_dev_list *c, *n;

	for(c = dl; c;) {
		n = c->next;
		c->next = 0;
		if(c->dev)
			picominer_destroy_device(c->dev);
		c->dev = 0;
		free(c);
		c = n;
	}
}


void picominer_destroy_list(picominer_dev_list *dl) {

	picominer_dev_list *c, *n;

	for(c = dl; c;) {
		n = c->next;
		c->next = 0;
		c->dev = 0;
		free(c);
		c = n;
	}
}


static int device_read128(picominer_device *device, void *v) {

	int r;
	char errbuf[BUFSIZ];
	PicoDrv *pd;

	pd = (PicoDrv *)device->pd;
	if((r = pd->ReadStream(device->streamd, v, 16)) < 0) {
		fprintf(stderr, "error: ReadStream: %s\n", PicoErrors_FullError(r, errbuf, sizeof(errbuf)));
		return -1;
	}
	if(r != 16) {
		fprintf(stderr, "error: ReadStream: short read: %d\n", r);
		return -2;
	}

	return 0;
}


static int device_write128(picominer_device *device, void *v) {

	int r;
	char errbuf[BUFSIZ];
	PicoDrv *pd;

	pd = (PicoDrv *)device->pd;
	if((r = pd->WriteStream(device->streamd, v, 16)) < 0) {
		fprintf(stderr, "error: WriteStream: %s\n", PicoErrors_FullError(r, errbuf, sizeof(errbuf)));
		return -1;
	}
	if(r != 16) {
		fprintf(stderr, "error: WriteStream: short write: %d\n", r);
		return -2;
	}

	return 0;
}


static void *picominer_read_thread(void *vdev) {

	uint32_t r[4];
	picominer_device *d = (picominer_device *)vdev;

	while(1) {
		if(!d->nonce_list) {
			fprintf(stderr, "error: list missing\n");
			return 0;
		}

		if(device_read128(d, r)) {
			fprintf(stderr, "error: picominer_read_thread: device_read128 failed\n");
			continue;
		}
		if(picominer_put_nonce(d, r[0])) {
			fprintf(stderr, "notice: nonce list deleted, exiting\n");
			return 0;
		}
	}
}


static int start_read_thread(picominer_device *dev) {

	// start read thread
	if(picominer_init_lists(dev) < 0) {
		fprintf(stderr, "error: picominer_init_lists\n");
		return -1;
	}
	if(!(dev->read_threadid = malloc(sizeof(pthread_t)))) {
		perror("malloc");
		picominer_delete_lists(dev);
		return -1;
	}
	if(pthread_create((pthread_t *)dev->read_threadid, 0, picominer_read_thread, dev)) {
		perror("pthread_create");
		free(dev->read_threadid);
		picominer_delete_lists(dev);
		return -1;
	}
	return 0;
}


void stop_read_thread(picominer_device *dev) {

	pthread_cancel(*(pthread_t *)(dev->read_threadid));
	free(dev->read_threadid);
	dev->read_threadid = 0;
	picominer_delete_lists(dev);
}


/*
 * create_and_flush:
 */
static int create_and_flush(picominer_device *dev) {

	int avail, r;
	char errbuf[BUFSIZ];
	unsigned char *buf;
	PicoDrv *pd;

	pd = (PicoDrv *)dev->pd;

	// create stream
	if((dev->streamd = pd->CreateStream(1)) < 0) {
		fprintf(stderr, "error: cannot create stream\n");
		return -1;
	}

	// flush stream
flush:
	if((avail = pd->GetBytesAvailable(1, true)) > 0) {
		avail = (avail + 15) & ~15;
		if(!(buf = (unsigned char *)malloc(avail))) {
			pd->CloseStream(dev->streamd);
			perror("malloc");
			return -1;
		}
		if((r = pd->ReadStream(dev->streamd, buf, avail)) < 0) {
			fprintf(stderr, "error: ReadStream: %s\n", PicoErrors_FullError(r, errbuf, sizeof(errbuf)));
			pd->CloseStream(dev->streamd);
			return -1;
		}
		free(buf);
		if(r != avail) {
			fprintf(stderr, "warning: flush returned short read\n");
		}
		goto flush;
	}

	return 0;
}


int picominer_prepare_device(picominer_device *dev) {

	PicoDrv *pd;

	pd = (PicoDrv *)dev->pd;
	if(pd->LoadFPGA(dev->bitstream_filename) < 0) {
		return -1;
	}
	if(create_and_flush(dev) < 0) {
		return -1;
	}
	if(start_read_thread(dev)) {
		pd->CloseStream(dev->streamd);
		return -1;
	}
	pthread_mutex_lock(&dev->ready_lock);
	dev->device_ready = 1;
	pthread_mutex_unlock(&dev->ready_lock);

	return 0;
}


static picominer_device *picominer_create_device(PicoDrv *pd, const char *bitstream_filename) {

	picominer_device *dev;

	// get memory for device structure
	if(!(dev = (picominer_device *)malloc(sizeof(*dev))))
		return 0;
	memset(dev, 0, sizeof(*dev));
	dev->pd = pd;
	snprintf(dev->bitstream_filename, sizeof(dev->bitstream_filename), "/usr/local/bin/bitstreams/%s", bitstream_filename);
	dev->clock_freq = 200;
	pthread_mutex_init(&dev->ready_lock, 0);

	return dev;
}


int picominer_get_all_available(picominer_dev_list **dev_list) {

	int i, num = 0;
	const supported_device_t *sd;
	picominer_dev_list *l = 0, *cur = 0;
	PicoDrv *pd;

	if(!(l = new_dev_list())) {
		return -1;
	}
	cur = l;

	for(i = 0;; i++) {
		sd = &s_supported_devices[i];
		if(!sd->model)
			break;
		pd = 0;
		while(!FindPico(sd->model, &pd)) {
			if(cur->dev) {
				if(!(cur->next = new_dev_list())) {
					picominer_destroy_device_list(l);
					return -1;
				}
				cur = cur->next;
			}
			if(!(cur->dev = picominer_create_device(pd, sd->bitstream_filename))) {
				continue;
			}
			cur->dev->device_model = sd->model;
			num += 1;
			pd = 0;
		}
	}
	*dev_list = l;

	return num;
}


int picominer_send_hash_data(picominer_device *device, const unsigned char *data_in) {

	int i;
	unsigned char data[48];

	memset(data, 0, sizeof(data));
	memcpy(data, data_in, 44);
	for(i = 0; i < 3; i++) {
		if(device_write128(device, &data[16 * i])) {
			fprintf(stderr, "error: device_write128\n");
			return -1;
		}
	}

	return 0;
}


int picominer_reset(picominer_device *) {

	// XXX
	return 0;
}


int picominer_get_stats(picominer_device *dev, float *t, float *v, float *i) {

	PicoDrv *pd;

	if(!dev)
		return -1;

	pd = (PicoDrv *)dev->pd;
	if(pd->GetSysMon(t, v, i)) {
		fprintf(stderr, "error: GetSysMon\n");
		return -1;
	} else {
		if(t)
			*t = 0;
		if(v)
			*v = 0;
		if(i)
			*i = 0;
	}

	return 0;
}


void picominer_build_time(char *buf, unsigned int buf_len) {

	snprintf(buf, buf_len, "%s %s", __DATE__, __TIME__);
}

