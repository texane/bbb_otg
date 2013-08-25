#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>


#define CONFIG_LIBUSB_VERSION 1

#if CONFIG_LIBUSB_VERSION
# include <usb.h>
#else
# include <libusb-1.0/libusb.h>
#endif


#define CONFIG_DEBUG 1
#if CONFIG_DEBUG

# include <stdio.h>

# define DEBUG_ENTER() printf("[>] %s\n", __FUNCTION__)
# define DEBUG_LEAVE() printf("[<] %s\n", __FUNCTION__)
# define DEBUG_PRINTF(s, ...) printf("[?] %s_%u: " s, __FUNCTION__, __LINE__, ## __VA_ARGS__)
# define DEBUG_ERROR(s, ...) printf("[!] %s_%u: " s, __FUNCTION__, __LINE__, ## __VA_ARGS__)

#else /* ! CONFIG_DEBUG */

# define DEBUG_ENTER()
# define DEBUG_LEAVE()
# define DEBUG_PRINTF(s, ...)
# define DEBUG_ERROR(s, ...)

#endif /* CONFIG_DEBUG */


#define CONFIG_BUF_SIZE 32


/* usb vid:pid */
#define CONFIG_VENDOR_ID 0x0525
#define CONFIG_PRODUCT_ID 0xa4a4

/* endpoints */
#define CONFIG_EP_REP 1
#define CONFIG_EP_REQ 1


/* dev handle */

typedef struct dev_handle
{
#if CONFIG_LIBUSB_VERSION
  usb_dev_handle* usb_handle;
#else
  libusb_device_handle* usb_handle;
  struct libusb_transfer* req_trans;
  struct libusb_transfer* rep_trans;
#endif

  unsigned int ep_req;
  unsigned int ep_rep;

} dev_handle_t;


/* internal globals */

#if CONFIG_LIBUSB_VERSION
/* none */
#else
static libusb_context* libusb_ctx =  NULL;
#endif


/* allocation wrappers */

static dev_handle_t* alloc_dev_handle(void)
{
  dev_handle_t* handle = malloc(sizeof(dev_handle_t));
  if (handle == NULL) return NULL;
  handle->usb_handle = NULL;

#if CONFIG_LIBUSB_VERSION

  /* nop */

#else

  handle->req_trans = NULL;
  handle->rep_trans = NULL;

#endif

  return handle;
}


static void free_dev_handle(dev_handle_t* handle)
{
  free(handle);
}


/* is the usb device a dev card reader */

#if CONFIG_LIBUSB_VERSION

static unsigned int is_device(const struct usb_device* dev)
{
  DEBUG_PRINTF("device: 0x%04x, 0x%04x\n",
	       dev->descriptor.idVendor,
	       dev->descriptor.idProduct);

  if (dev->descriptor.idVendor != CONFIG_VENDOR_ID)
    return 0;

  if (dev->descriptor.idProduct != CONFIG_PRODUCT_ID)
    return 0;

  return 1;
}

#else

static unsigned int is_device(libusb_device* dev)
{
  struct libusb_device_descriptor desc;

  DEBUG_PRINTF("device: 0x%04x, 0x%04x\n", desc.idVendor, desc.idProduct);

  if (libusb_get_device_descriptor(dev, &desc))
    return 0;

  if (desc.idVendor != CONFIG_VENDOR_ID)
    return 0;

  if (desc.idProduct != CONFIG_PRODUCT_ID)
    return 0;

  return 1;
}

#endif


#if CONFIG_LIBUSB_VERSION

static int send_recv_buf(dev_handle_t* handle, uint8_t* buf)
{
  /* timeouts in ms */
#define CONFIG_DEFAULT_TIMEOUT (2000)

  int usberr;

  usberr = usb_bulk_write
  (
   handle->usb_handle,
   handle->ep_req,
   (void*)buf,
   CONFIG_BUF_SIZE,
   CONFIG_DEFAULT_TIMEOUT
  );

  if (usberr < 0)
  {
    DEBUG_ERROR("usb_bulk_write() == %d(%s)\n", usberr, usb_strerror());
    return -1;
  }

  usberr = usb_bulk_read
  (
   handle->usb_handle,
   handle->ep_rep,
   (void*)buf,
   CONFIG_BUF_SIZE,
   CONFIG_DEFAULT_TIMEOUT
  );

  if (usberr < 0)
  {
    DEBUG_ERROR("usb_bulk_read() == %d(%s)\n", usberr, usb_strerror());
    return -1;
  }

  return 0;
}

#else /* ! CONFIG_LIBUSB_VERSION */

/* usb async io wrapper */

struct trans_ctx
{
#define TRANS_FLAGS_IS_DONE (1 << 0)
#define TRANS_FLAGS_HAS_ERROR (1 << 1)
  volatile unsigned long flags;
};


static void on_trans_done(struct libusb_transfer* trans)
{
  struct trans_ctx* const ctx = trans->user_data;

  if (trans->status != LIBUSB_TRANSFER_COMPLETED)
    ctx->flags |= TRANS_FLAGS_HAS_ERROR;

  ctx->flags = TRANS_FLAGS_IS_DONE;
}


static int submit_wait(struct libusb_transfer* trans)
{
  struct trans_ctx trans_ctx;
  enum libusb_error error;

  trans_ctx.flags = 0;

  /* brief intrusion inside the libusb interface */
  trans->callback = on_trans_done;
  trans->user_data = &trans_ctx;

  if ((error = libusb_submit_transfer(trans)))
  {
    DEBUG_ERROR("libusb_submit_transfer(%d)\n", error);
    return -1;
  }

  while (!(trans_ctx.flags & TRANS_FLAGS_IS_DONE))
  {
    if (libusb_handle_events(NULL))
    {
      DEBUG_ERROR("libusb_handle_events()\n");
      return -1;
    }
  }

  return 0;
}

static int send_recv_buf(dev_handle_t* handle, uint8_t* buf)
{
  /* endpoint 0: control, not used here
     endpoint 1: bulk transfer used since
     time delivery does not matter but data
     integrity and transfer completion
  */

  dev_error_t error;

  /* send the command */

#if 0
  libusb_fill_interrupt_transfer
  (
   handle->req_trans,
   handle->usb_handle,
   handle->ep_req,
   (void*)&cmd->req,
   sizeof(cmd->req),
   NULL, NULL,
   0
  );
#else
  libusb_fill_bulk_transfer
  (
   handle->req_trans,
   handle->usb_handle,
   handle->ep_req,
   buf,
   CONFIG_BUF_SIZE,
   NULL, NULL,
   0
  );
#endif

  DEBUG_PRINTF("-- req transfer\n");

  error = submit_wait(handle->req_trans);
  if (error) return -1;

  /* read the response */

  libusb_fill_bulk_transfer
  (
   handle->rep_trans,
   handle->usb_handle,
   handle->ep_rep,
   buf, CONFIG_BUF_SIZE,
   NULL, NULL,
   0
  );

  DEBUG_PRINTF("-- rep transfer\n");

  error = submit_wait(handle->rep_trans);
  if (error) return -1;

  /* success */

  error = DEV_ERROR_SUCCESS;

 on_error:
  return error;
}

#endif /* CONFIG_LIBUSB_VERSION */


/* forward decls */

#if CONFIG_LIBUSB_VERSION
static int open_dev_usb_handle(usb_dev_handle**, int);
static void close_dev_usb_handle(usb_dev_handle*);
#else
static int open_dev_usb_handle(libusb_dev_handle**, int);
static void close_dev_usb_handle(libusb_dev_handle**, int);
#endif

/* find dev device wrapper */

static int find_device(void)
{
  /* return 0 if dev device found */

  usb_dev_handle* dummy_handle = NULL;
  return open_dev_usb_handle(&dummy_handle, 1);
}

static int send_recv_buf_or_reopen(dev_handle_t* handle, uint8_t* buf)
{
  /* send_recv_buf or reopen handle on libusb failure */

  int error = send_recv_buf(handle, buf);
  if (error == -1)
  {
    if (find_device() == -1) return -1;

    /* reopen the usb handle */

    close_dev_usb_handle(handle->usb_handle);

    error = open_dev_usb_handle(&handle->usb_handle, 0);
    if (error == -1) return -1;

    /* resend the buf */

    error = send_recv_buf(handle, buf);
  }

  return 0;
}


/* endianness */

static int is_mach_le = 0;

static inline int get_is_mach_le(void)
{
  const uint16_t magic = 0x0102;
  if ((*(const uint8_t*)&magic) == 0x02) return 1;
  return 0;
}

static inline uint16_t le16_to_mach(uint16_t n)
{
  if (!is_mach_le) n = ((n >> 8) & 0xff) | ((n & 0xff) << 8);
  return n;
}


/* constructors */

static void finalize(void)
{
#if CONFIG_LIBUSB_VERSION

  /* nop */

#else

  if (libusb_ctx != NULL)
  {
    libusb_exit(libusb_ctx);
    libusb_ctx = NULL;
  }

#endif
}


static int initialize(void)
{
  is_mach_le = get_is_mach_le();

#if CONFIG_LIBUSB_VERSION

  usb_init();

#else

  if (libusb_ctx != NULL) return -1;
  if (libusb_init(&libusb_ctx)) return -1;

#endif

  return 0;
}


#if CONFIG_LIBUSB_VERSION

static void close_dev_usb_handle(usb_dev_handle* usb_handle)
{
  usb_close(usb_handle);
}

static int open_dev_usb_handle(usb_dev_handle** usb_handle, int enum_only)
{
  /* enum_only is used when we only wants to find the dev device. */

  int error;
  struct usb_bus* bus;

  *usb_handle = NULL;

  usb_find_busses();
  usb_find_devices();

  for (bus = usb_busses; bus != NULL; bus = bus->next)
  {
    struct usb_device* dev;
    for (dev = bus->devices; dev != NULL; dev = dev->next)
    {
      if (is_device(dev))
      {
	if (enum_only)
	{
	  error = 0;
	  goto on_error;
	}

	*usb_handle = usb_open(dev);
	if (*usb_handle == NULL)
	{
	  error = -1;
	  goto on_error;
	}

#if 0
	if (usb_set_configuration(*usb_handle, 0))
	{
	  DEBUG_ERROR("libusb_set_configuration(): %s\n", usb_strerror());
	  error = -1;
	  goto on_error;
	}
#endif

	if (usb_claim_interface(*usb_handle, 0))
	{
	  DEBUG_ERROR("libusb_claim_interface(): %s\n", usb_strerror());
	  error = -1;
	  goto on_error;
	}

	return 0;
      }
    }
  }

  /* not found */

  error = -1;

 on_error:

  if (*usb_handle != NULL)
  {
    usb_close(*usb_handle);
    *usb_handle = NULL;
  }

  return error;
}

#else

static void close_dev_usb_handle(libusb_dev_handle* usb_handle)
{
  libusb_close(usb_handle);
}

static int open_dev_usb_handle(libusb_dev_handle** usb_handle, int enum_only)
{
  /* @see the above comment for enum_only meaning */

  ssize_t i;
  ssize_t count;
  libusb_device** devs = NULL;
  libusb_device* dev;
  int error = 0;

  *usb_handle = NULL;

  if (libusb_ctx == NULL)
  {
    error = -1;
    goto on_error;
  }

  count = libusb_get_device_list(libusb_ctx, &devs);
  if (count < 0)
  {
    error = -1;
    goto on_error;
  }

  for (i = 0; i < count; ++i)
  {
    dev = devs[i];

    if (is_target_device(dev))
      break;
  }

  if (i == count)
  {
    error = -1;
    goto on_error;
  }

  /* open for enumeration only */
  if (enum_only)
  {
    error = 0;
    goto on_error;
  }

  if (libusb_open(dev, usb_handle))
  {
    error = -1;
    goto on_error;
  }

  if (libusb_set_configuration(*usb_handle, 0))
  {
    DEBUG_ERROR("libusb_set_configuration()\n");
    error = -1;
    goto on_error;
  }

  if (libusb_claim_interface(*usb_handle, 0))
  {
    DEBUG_ERROR("libusb_claim_interface()\n");
    error = -1;
    goto on_error;
  }

 on_error:

  if (devs != NULL)
    libusb_free_device_list(devs, 1);

  if (error == 0) return 0;

  if (*usb_handle != NULL)
  {
    libusb_close(*usb_handle);
    *usb_handle = NULL;
  }

  return -1;
}

#endif /* CONFIG_LIBUSB_VERSION */


static int dev_open(dev_handle_t** dev_handle)
{
  usb_dev_handle* usb_handle = NULL;
  
  *dev_handle = NULL;

  /* open usb device */

  if (open_dev_usb_handle(&usb_handle, 0) == -1) goto on_error;

  /* open and init the device */

  *dev_handle = alloc_dev_handle();
  if (*dev_handle == NULL) goto on_error;

  (*dev_handle)->usb_handle = usb_handle;

#if CONFIG_LIBUSB_VERSION

  (*dev_handle)->ep_req = CONFIG_EP_REQ | USB_ENDPOINT_OUT;
  (*dev_handle)->ep_rep = CONFIG_EP_REP | USB_ENDPOINT_IN;

#else

  (*dev_handle)->req_trans = libusb_alloc_transfer(0);
  if ((*dev_handle)->req_trans == NULL) goto on_error;

  (*dev_handle)->rep_trans = libusb_alloc_transfer(0);
  if ((*dev_handle)->rep_trans == NULL) goto on_error;

  (*dev_handle)->ep_req = CONFIG_EP_REQ | LIBUSB_ENDPOINT_OUT;
  (*dev_handle)->ep_rep = CONFIG_EP_REP | LIBUSB_ENDPOINT_IN;

#endif

  return 0;
  
 on_error:
  return -1;
}


void dev_close(dev_handle_t* handle)
{
#if CONFIG_LIBUSB_VERSION

  /* nothing to do */

#else

  if (handle->req_trans != NULL)
    libusb_free_transfer(handle->req_trans);

  if (handle->rep_trans != NULL)
    libusb_free_transfer(handle->rep_trans);

#endif

  if (handle->usb_handle != NULL)
    close_dev_usb_handle(handle->usb_handle);

  free_dev_handle(handle);
}


static int dev_echo(dev_handle_t* handle, uint8_t* buf)
{
  return send_recv_buf_or_reopen(handle, buf);
}

static void dump_buf(uint8_t* buf, size_t n)
{
  size_t i;
  for (i = 0; i < n; ++i) printf(" %02x", buf[i]);
  printf("\n");
}

int main(int ac, char** av)
{
  dev_handle_t* handle;

  uint8_t buf[CONFIG_BUF_SIZE];

  if (initialize()) goto on_error_0;
  if (dev_open(&handle)) goto on_error_1;

  printf("sending\n"); getchar();

  memset(buf, 0x2a, CONFIG_BUF_SIZE);
  if (dev_echo(handle, buf)) goto on_error_2;

  dump_buf(buf, CONFIG_BUF_SIZE);

 on_error_2:
  dev_close(handle);
 on_error_1:
  finalize();
 on_error_0:

  return 0;
}
