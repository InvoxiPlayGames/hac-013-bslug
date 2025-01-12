/* main.c
 *   by Alex Chadwick
 *
 * Copyright (C) 2017, Alex Chadwick
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/* This module adds support for the USB GCN Adapter (WUP-028). It achieves this
 * by simply replacing the Wii's PADRead and PADControlMotor calls. When PADRead
 * is called for the first time, the module initialises. Thereafter, the reading
 * of the actual USB proceeds asynchronously and continuously and the PADRead
 * method just returns the result of the most recent message from the USB. The
 * outbound rumble control messages get inserted between inbound controller data
 * messages, as it seems like having both inbound and outbound messages
 * travelling independently can cause lock ups. */

/* Interfacing with the USB is done by the IOS on behalf of the game. The
 * interface that the IOS exposes differs by IOS version. This module uses the
 * /dev/usb/hid interface. I've not tested exhaustively but IOS36 doesn't
 * support /dev/usb/hid at all, so this module won't work there. IOS37
 * introduces /dev/usb/hid at version 4. Later, IOS58 changes /dev/usb/hid
 * completely to version 5. This module supports both version 4 and version 5 of
 * /dev/usb/hid which it detects dynamically. I don't know if any later IOSs
 * change /dev/usb/hid in a non-suported way, or if earlier IOSs may be
 * compatible. */

#include <bslug.h>
#include <rvl/cache.h>
#include <rvl/ipc.h>
#include <rvl/Pad.h>
#include <stdbool.h>

void printf(const char * format, ...);
static int sendRumble(void);
void VIResetDimmingCount(void);

BSLUG_MODULE_GAME("????");
BSLUG_MODULE_NAME("Switch Pro Controller Support");
BSLUG_MODULE_VERSION("v1.0");
BSLUG_MODULE_AUTHOR("InvoxiPlayGames");
BSLUG_MODULE_LICENSE("BSD");

/*============================================================================*/
/* Macros */
/*============================================================================*/

/* If you don't want to support both interfaces, remove one of these two
 * defines. */
#define SUPPORT_DEV_USB_HID4
#define SUPPORT_DEV_USB_HID5

#define IOS_ALIGN __attribute__((aligned(32)))

/* You can't really change this! */
#define GCN_CONTROLLER_COUNT 4
/* L and R slider's "pressed" state. */
#define GCN_TRIGGER_THRESHOLD 170
/* How long (in time base units) to go without inputs before reporting a disconnect. */
#define ms * (243000/4)
#define GCN_TIMEOUT (1500 ms)
/* Commands that the adapter controller. */
#define HAC_013_CMD_PREFIX 0x80
#define HAC_013_CMD_RUMBLE 0x11
#define HAC_013_CMD_MAC 0x01
#define HAC_013_CMD_HANDSHAKE 0x02
#define HAC_013_CMD_HID 0x04
/* VendorID and ProductID of the controller. */
#define HAC_013_ID 0x057e2009
/* Size of the data returned by the controller. */
#define HAC_013_POLL_SIZE 0x40
/* Size of the rumble message circular buffer. */
#define RUMBLE_BUFFER 16
/* Number of polls to wait before giving up on outstanding rumble command. */
#define RUMBLE_DELAY 3
/* Path to the USB device interface. */
#define DEV_USB_HID_PATH "/dev/usb/hid"

/*============================================================================*/
/* Globals */
/*============================================================================*/

static ios_fd_t dev_usb_hid_fd = -1;
static int8_t started = 0;
static PADData_t gcn_data[GCN_CONTROLLER_COUNT];
static uint32_t gcn_adapter_id = -1;
#if defined(SUPPORT_DEV_USB_HID5) && defined(SUPPORT_DEV_USB_HID4)
#define HAVE_VERSION
static int8_t version;
#endif
int8_t error;
int8_t errorMethod;
int8_t lastCommand;
/* Messages for device. */
static uint8_t handshake_msg_buffer[2] IOS_ALIGN = { HAC_013_CMD_PREFIX, HAC_013_CMD_HANDSHAKE };
static uint8_t hid_msg_buffer[2] IOS_ALIGN = { HAC_013_CMD_PREFIX, HAC_013_CMD_HID };
static uint8_t rumble_msg_buffer[10] IOS_ALIGN = { 0x10, 0x0F, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00 };
static uint8_t led_msg_buffer[12] IOS_ALIGN = { 0x01, 0x0F, 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40, 0x30, 0x02 };
/* Pad buffer size to cache line size of IOS core. Otherwise IOS will write ver
 * later data, or we could read a stale value. */
static uint8_t poll_msg_buffer[-(-HAC_013_POLL_SIZE & ~0x1f)] IOS_ALIGN;

/*============================================================================*/
/* Top level interface to game */
/*============================================================================*/

//static uint32_t mftb(void);
static uint32_t cpu_isr_disable(void);
static void cpu_isr_restore(uint32_t isr);
static void onDevOpen(ios_fd_t fd, usr_t unused);

static void myPADRead(PADData_t result[GCN_CONTROLLER_COUNT]) {
  uint32_t isr = cpu_isr_disable();
  if (!started) {
    /* On first call only, initialise USB and globals. */
    started = 1;
    for (int i = 0; i < GCN_CONTROLLER_COUNT; i++)
      gcn_data[i].error = PADData_ERROR_NO_CONNECTION;
    //gcn_data_written = mftb();
    IOS_OpenAsync(DEV_USB_HID_PATH, 0, onDevOpen, NULL);
  }
  if (errorMethod > 0) {
    /* On a USB error, disconnect all controllers. */
    errorMethod = -errorMethod;
    for (int i = 0; i < GCN_CONTROLLER_COUNT; i++)
      gcn_data[i].error = PADData_ERROR_NO_CONNECTION;
  }
  for (int i = 0; i < GCN_CONTROLLER_COUNT; i++) {
    /* Copy last seen controller inputs. */
    result[i] = gcn_data[i];
    if (gcn_data[i].error == 0)
      gcn_data[i].error = PADData_ERROR_2;
  }
  cpu_isr_restore(isr);
}

static void myPADControlMotor(int pad, int control) {
  if (pad > 0) return;
  printf("Control motor: %i\n", control);
  if (control > 1) control = 0;
  lastCommand = control;
  DCFlushRange(rumble_msg_buffer, sizeof(rumble_msg_buffer));
  rumble_msg_buffer[3] = (control == 1 ? 0x20 : 0x00);
  rumble_msg_buffer[4] = (control == 1 ? 0x62 : 0x00);
  rumble_msg_buffer[5] = (control == 1 ? 0x10 : 0x00);
  rumble_msg_buffer[6] = (control == 1 ? 0x98 : 0x80);
  rumble_msg_buffer[7] = (control == 1 ? 0x20 : 0x00);
  rumble_msg_buffer[8] = (control == 1 ? 0x62 : 0x00);
  rumble_msg_buffer[9] = (control == 1 ? 0x10 : 0x00);
  sendRumble();
}

BSLUG_MUST_REPLACE(PADRead, myPADRead);
BSLUG_REPLACE(PADControlMotor, myPADControlMotor);

/*============================================================================*/
/* USB support */
/*============================================================================*/

static uint32_t cpu_isr_disable(void) {
  uint32_t isr, tmp;
  asm volatile("mfmsr %0; rlwinm %1, %0, 0, 0xFFFF7FFF; mtmsr %1" : "=r"(isr), "=r"(tmp));
  return isr;
}
static void cpu_isr_restore(uint32_t isr) {
  uint32_t tmp;
  asm volatile ("mfmsr %0; rlwimi %0, %1, 0, 0x8000; mtmsr %0" : "=&r"(tmp) : "r" (isr));
}

static void callbackIgnore(ios_ret_t ret, usr_t unused);
static void onDevUsbInit(ios_ret_t ret, usr_t unused);
static void onDevUsbPoll(ios_ret_t ret, usr_t unused);

#ifdef SUPPORT_DEV_USB_HID4
  /* The basic flow for version 4:
   *  1) ioctl GET_VERSION
   *       Check the return value is 0x00040001.
   *  2) ioctl GET_DEVICE_CHANGE
   *       Returns immediately + every time a device is added or removed. Output
   *       describes what is connected.
   *  3) Find an interesting device.
   *  4) ioctl INTERRUPT_OUT
   *       USB interrupt packet to send initialise command to WUP-028.
   *  5) ioctl INTERRUPT_IN
   *       USB interrupt packet to poll device for inputs.
   */

  /* Size of the adapter's description. */
# define HAC_013_DESCRIPTOR_SIZE 0x44
  /* Endpoint numbering for the device. */
# define HAC_013_ENDPOINT_OUT 0x1
# define HAC_013_ENDPOINT_IN 0x81
  /* Size of the DeviceChange ioctl's return (in words). */
# define DEV_USB_HID4_DEVICE_CHANGE_SIZE 0x180
  /* IOCTL numbering for the device. */
# define DEV_USB_HID4_IOCTL_GET_DEVICE_CHANGE 0
# define DEV_USB_HID4_IOCTL_INTERRUPT_IN 3
# define DEV_USB_HID4_IOCTL_INTERRUPT_OUT 4
# define DEV_USB_HID4_IOCTL_GET_VERSION 6
  /* Version id. */
# define DEV_USB_HID4_VERSION 0x00040001

static uint32_t dev_usb_hid4_devices[DEV_USB_HID4_DEVICE_CHANGE_SIZE] IOS_ALIGN;
struct interrupt_msg4 {
  uint8_t padding[16];
  uint32_t device;
  uint32_t endpoint;
  uint32_t length;
  void *ptr;
};

static struct interrupt_msg4 handshake_msg4 IOS_ALIGN = {
  .device = -1,
  .endpoint = HAC_013_ENDPOINT_OUT,
  .length = sizeof(handshake_msg_buffer),
  .ptr = handshake_msg_buffer
};

static struct interrupt_msg4 hid_msg4 IOS_ALIGN = {
  .device = -1,
  .endpoint = HAC_013_ENDPOINT_OUT,
  .length = sizeof(hid_msg_buffer),
  .ptr = hid_msg_buffer
};

static struct interrupt_msg4 rumble_msg4 IOS_ALIGN = {
  .device = -1,
  .endpoint = HAC_013_ENDPOINT_OUT,
  .length = sizeof(rumble_msg_buffer),
  .ptr = rumble_msg_buffer
};

static struct interrupt_msg4 led_msg4 IOS_ALIGN = {
  .device = -1,
  .endpoint = HAC_013_ENDPOINT_OUT,
  .length = sizeof(led_msg_buffer),
  .ptr = led_msg_buffer
};

static struct interrupt_msg4 poll_msg4 IOS_ALIGN = {
  .device = -1,
  .endpoint = HAC_013_ENDPOINT_IN,
  .length = HAC_013_POLL_SIZE,
  .ptr = poll_msg_buffer
};

static void onDevGetVersion4(ios_ret_t ret, usr_t unused);
static void onDevUsbChange4(ios_ret_t ret, usr_t unused);

static int checkVersion4(ios_cb_t cb, usr_t data) {
  return IOS_IoctlAsync(
    dev_usb_hid_fd, DEV_USB_HID4_IOCTL_GET_VERSION,
    NULL, 0,
    NULL, 0,
    cb, data
  );
}
static int getDeviceChange4(ios_cb_t cb, usr_t data) {
  return  IOS_IoctlAsync(
    dev_usb_hid_fd, DEV_USB_HID4_IOCTL_GET_DEVICE_CHANGE,
    NULL, 0,
    dev_usb_hid4_devices, sizeof(dev_usb_hid4_devices),
    cb, data
  );
}
static int sendHandshake4(ios_cb_t cb, usr_t data) {
  handshake_msg4.device = gcn_adapter_id;
  return IOS_IoctlAsync(
    dev_usb_hid_fd, DEV_USB_HID4_IOCTL_INTERRUPT_OUT,
    &handshake_msg4, sizeof(handshake_msg4),
    NULL, 0,
    cb, data
  );
}
static int sendHid4(ios_cb_t cb, usr_t data) {
  hid_msg4.device = gcn_adapter_id;
  return IOS_IoctlAsync(
    dev_usb_hid_fd, DEV_USB_HID4_IOCTL_INTERRUPT_OUT,
    &hid_msg4, sizeof(hid_msg4),
    NULL, 0,
    cb, data
  );
}
static int sendRumble4(ios_cb_t cb, usr_t data) {
  rumble_msg4.device = gcn_adapter_id;
  return IOS_IoctlAsync(
    dev_usb_hid_fd, DEV_USB_HID4_IOCTL_INTERRUPT_OUT,
    &rumble_msg4, sizeof(rumble_msg4),
    NULL, 0,
    cb, data
  );
}
static int sendLed4(ios_cb_t cb, usr_t data) {
  led_msg4.device = gcn_adapter_id;
  return IOS_IoctlAsync(
    dev_usb_hid_fd, DEV_USB_HID4_IOCTL_INTERRUPT_OUT,
    &led_msg4, sizeof(led_msg4),
    NULL, 0,
    cb, data
  );
}

static int sendPoll4(ios_cb_t cb, usr_t data) {
  poll_msg4.device = gcn_adapter_id;
  DCFlushRange(poll_msg_buffer, sizeof(poll_msg_buffer));
  return IOS_IoctlAsync(
    dev_usb_hid_fd, DEV_USB_HID4_IOCTL_INTERRUPT_IN,
    &poll_msg4, sizeof(poll_msg4),
    NULL, 0,
    cb, data
  );
}

#endif

#ifdef SUPPORT_DEV_USB_HID5
  /* The basic flow for version 5:
   *  1) ioctl GET_VERSION
   *       Check the return value is 0x00050001.
   *  2) ioctl GET_DEVICE_CHANGE
   *       Returns immediately + every time a device is added or removed. Output
   *       describes what is connected.
   *  3) ioctl ATTACH_FINISH
   *       Don't know why, but you have to do this!
   *  4) Find an interesting device.
   *  5) ioctl SET_RESUME
   *       Turn the device on.
   *  5) ioctl GET_DEVICE_PARAMETERS
   *       You have to do this, even if you don't care about the result.
   *  6) ioctl INTERRUPT
   *       USB interrupt packet to send initialise command to WUP-028.
   *  7) ioctl INTERRUPT
   *       USB interrupt packet to  poll device for inputs.
   */

  /* Size of the DeviceChange ioctl's return (in strctures). */
# define DEV_USB_HID5_DEVICE_CHANGE_SIZE 0x20
  /* Total size of all IOS buffers (in words). */
# define DEV_USB_HID5_TMP_BUFFER_SIZE 0x20
  /* IOCTL numbering for the device. */
# define DEV_USB_HID5_IOCTL_GET_VERSION 0
# define DEV_USB_HID5_IOCTL_GET_DEVICE_CHANGE 1
# define DEV_USB_HID5_IOCTL_GET_DEVICE_PARAMETERS 3
# define DEV_USB_HID5_IOCTL_ATTACH_FINISH 6
# define DEV_USB_HID5_IOCTL_SET_RESUME 16
# define DEV_USB_HID5_IOCTL_INTERRUPT 19
  /* Version id. */
# define DEV_USB_HID5_VERSION 0x00050001

# define OS_IPC_HEAP_HIGH ((void **)0x80003134)

static struct {
  uint32_t id;
  uint32_t vid_pid;
  uint32_t _unknown8;
} *dev_usb_hid5_devices;
/* This buffer gets managed pretty carefully. During init it's split 0x20 bytes
 * to 0x60 bytes to store the descriptor. The rest of the time it's split
 * evenly, one half for rumble messages and one half for polls. Be careful! */
static uint32_t *dev_usb_hid5_buffer;
static ioctlv dev_usb_hid5_argv[2] IOS_ALIGN;
/* Polls may be sent at the same time as rumbles, so need two ioctlv arrays. */
static ioctlv dev_usb_hid5_poll_argv[2] IOS_ALIGN;

/* Annoyingly some of the buffers for v5 MUST be in MEM2, so we wrap _start to
 * allocate these before the application boots. */
void _start(void);

static void my_start(void) {
  /* Allocate some area in MEM2. */
  dev_usb_hid5_devices = *OS_IPC_HEAP_HIGH;
  dev_usb_hid5_devices -= DEV_USB_HID5_DEVICE_CHANGE_SIZE;
  *OS_IPC_HEAP_HIGH = dev_usb_hid5_devices;

  dev_usb_hid5_buffer = *OS_IPC_HEAP_HIGH;
  dev_usb_hid5_buffer -= DEV_USB_HID5_TMP_BUFFER_SIZE;
  *OS_IPC_HEAP_HIGH = dev_usb_hid5_buffer;

  _start();
}

BSLUG_MUST_REPLACE(_start, my_start);

static void onDevGetVersion5(ios_ret_t ret, usr_t unused);
static void onDevUsbAttach5(ios_ret_t ret, usr_t vcount);
static void onDevUsbChange5(ios_ret_t ret, usr_t unused);
static void onDevUsbResume5(ios_ret_t ret, usr_t unused);
static void onDevUsbParams5(ios_ret_t ret, usr_t unused);

static int checkVersion5(ios_cb_t cb, usr_t data) {
  return IOS_IoctlAsync(
    dev_usb_hid_fd, DEV_USB_HID5_IOCTL_GET_VERSION,
    NULL, 0,
    dev_usb_hid5_buffer, 0x20,
    cb, data
  );
}
static int getDeviceChange5(ios_cb_t cb, usr_t data) {
  return IOS_IoctlAsync(
    dev_usb_hid_fd, DEV_USB_HID5_IOCTL_GET_DEVICE_CHANGE,
    NULL, 0,
    dev_usb_hid5_devices, sizeof(dev_usb_hid5_devices[0]) * DEV_USB_HID5_DEVICE_CHANGE_SIZE,
    cb, data
  );
}

static int sendAttach5(ios_cb_t cb, usr_t data) {
  return IOS_IoctlAsync(
    dev_usb_hid_fd, DEV_USB_HID5_IOCTL_ATTACH_FINISH,
    NULL, 0,
    NULL, 0,
    cb, data
  );
}

static int sendResume5(ios_cb_t cb, usr_t data) {
  dev_usb_hid5_buffer[0] = gcn_adapter_id;
  dev_usb_hid5_buffer[1] = 0;
  dev_usb_hid5_buffer[2] = 1;
  dev_usb_hid5_buffer[3] = 0;
  dev_usb_hid5_buffer[4] = 0;
  dev_usb_hid5_buffer[5] = 0;
  dev_usb_hid5_buffer[6] = 0;
  dev_usb_hid5_buffer[7] = 0;
  return IOS_IoctlAsync(
    dev_usb_hid_fd, DEV_USB_HID5_IOCTL_SET_RESUME,
    dev_usb_hid5_buffer, 0x20,
    NULL, 0,
    cb, data
  );
}

static int sendParams5(ios_cb_t cb, usr_t data) {
  /* Assumes buffer still in state from sendResume5 */
  return IOS_IoctlAsync(
    dev_usb_hid_fd, DEV_USB_HID5_IOCTL_GET_DEVICE_PARAMETERS,
    dev_usb_hid5_buffer, 0x20,
    dev_usb_hid5_buffer + 8, 0x60,
    cb, data
  );
}

static int sendHandshake5(ios_cb_t cb, usr_t data) {
  /* Assumes buffer already set up */
  dev_usb_hid5_argv[0] = (ioctlv){dev_usb_hid5_buffer, 0x40};
  dev_usb_hid5_argv[1] = (ioctlv){handshake_msg_buffer, sizeof(handshake_msg_buffer)};
  return IOS_IoctlvAsync(
    dev_usb_hid_fd, DEV_USB_HID5_IOCTL_INTERRUPT,
    2, 0, dev_usb_hid5_argv,
    cb, data
  );
}

static int sendHid5(ios_cb_t cb, usr_t data) {
  /* Assumes buffer already set up */
  dev_usb_hid5_argv[0] = (ioctlv){dev_usb_hid5_buffer, 0x40};
  dev_usb_hid5_argv[1] = (ioctlv){hid_msg_buffer, sizeof(hid_msg_buffer)};
  return IOS_IoctlvAsync(
    dev_usb_hid_fd, DEV_USB_HID5_IOCTL_INTERRUPT,
    2, 0, dev_usb_hid5_argv,
    cb, data
  );
}

static int sendRumble5(ios_cb_t cb, usr_t data) {
  /* Assumes buffer already set up */
  dev_usb_hid5_argv[0] = (ioctlv){dev_usb_hid5_buffer, 0x40};
  dev_usb_hid5_argv[1] = (ioctlv){rumble_msg_buffer, sizeof(rumble_msg_buffer)};
  return IOS_IoctlvAsync(
    dev_usb_hid_fd, DEV_USB_HID5_IOCTL_INTERRUPT,
    2, 0, dev_usb_hid5_argv,
    cb, data
  );
}

static int sendLed5(ios_cb_t cb, usr_t data) {
  /* Assumes buffer already set up */
  dev_usb_hid5_argv[0] = (ioctlv){dev_usb_hid5_buffer, 0x40};
  dev_usb_hid5_argv[1] = (ioctlv){led_msg_buffer, sizeof(led_msg_buffer)};
  return IOS_IoctlvAsync(
    dev_usb_hid_fd, DEV_USB_HID5_IOCTL_INTERRUPT,
    2, 0, dev_usb_hid5_argv,
    cb, data
  );
}

static int sendPoll5(ios_cb_t cb, usr_t data) {
  /* Assumes buffer already set up */
  dev_usb_hid5_poll_argv[0] = (ioctlv){dev_usb_hid5_buffer+0x10, 0x40};
  dev_usb_hid5_poll_argv[1] = (ioctlv){poll_msg_buffer, HAC_013_POLL_SIZE};
  return IOS_IoctlvAsync(
    dev_usb_hid_fd, DEV_USB_HID5_IOCTL_INTERRUPT,
    1, 1, dev_usb_hid5_poll_argv,
    cb, data
  );
}

#endif

static void onError(void) {
  dev_usb_hid_fd = -1;
  IOS_CloseAsync(dev_usb_hid_fd, callbackIgnore, NULL);
}

/*============================================================================*/
/* Start of USB callback chain. Each method calls another as a callback after an
 * IOS command. */
/*============================================================================*/

static void onDevOpen(ios_fd_t fd, usr_t unused) {
  printf("Opening IOS...\n");
  uint32_t ret;
  (void)unused;
  dev_usb_hid_fd = fd;
  if (fd >= 0)
    ret =
#if defined(SUPPORT_DEV_USB_HID4)
      checkVersion4(onDevGetVersion4, NULL);
#elif defined(SUPPORT_DEV_USB_HID5)
      checkVersion5(onDevGetVersion5, NULL);
#else
      -1;
#endif
  else
    ret = fd;
  if (ret) {
    printf("Error opening IOS %i\n", ret);
    error = ret;
    errorMethod = 1;
  }
}

static void callbackIgnore(ios_ret_t ret, usr_t unused) {
  (void)unused;
  (void)ret;
}

#ifdef SUPPORT_DEV_USB_HID4
static void onDevGetVersion4(ios_ret_t ret, usr_t unused) {
  (void)unused;
  if (ret == DEV_USB_HID4_VERSION) {
#ifdef HAVE_VERSION
    version = 4;
#endif
    ret = getDeviceChange4(onDevUsbChange4, NULL);
  } else
    ret =
#ifdef SUPPORT_DEV_USB_HID5
      checkVersion5(onDevGetVersion5, NULL);
#else
      ret;
#endif
  if (ret) {
    error = ret;
    errorMethod = 2;
    onError();
  }
}
#endif

#ifdef SUPPORT_DEV_USB_HID5
static void onDevGetVersion5(ios_ret_t ret, usr_t unused) {
  (void)unused;
  if (ret == 0 && dev_usb_hid5_buffer[0] == DEV_USB_HID5_VERSION) {
#ifdef HAVE_VERSION
    version = 5;
#endif
    ret = getDeviceChange5(onDevUsbChange5, NULL);
  } else if (ret == 0)
    ret = dev_usb_hid5_buffer[0];

  if (ret) {
    error = ret;
    errorMethod = 3;
    onError();
  }
}
#endif

#ifdef SUPPORT_DEV_USB_HID4
static void onDevUsbChange4(ios_ret_t ret, usr_t unused) {
  if (ret >= 0) {
    int found = 0;
    for (int i = 0; i < DEV_USB_HID4_DEVICE_CHANGE_SIZE && dev_usb_hid4_devices[i] < sizeof(dev_usb_hid4_devices); i += dev_usb_hid4_devices[i] / 4) {
      uint32_t device_id = dev_usb_hid4_devices[i + 1];
      if (
        //dev_usb_hid4_devices[i] == HAC_013_DESCRIPTOR_SIZE &&
        dev_usb_hid4_devices[i + 4] == HAC_013_ID
        //true
      ) {
        found = 1;
        if (gcn_adapter_id != device_id) {
          gcn_adapter_id = device_id;
          onDevUsbInit(0, NULL);
        }
        break;
      } else {
        printf("Device not found. %x\n", dev_usb_hid4_devices[i + 4]);
      }
    }
    if (!found) gcn_adapter_id = (uint32_t)-1;
    ret = getDeviceChange4(onDevUsbChange4, NULL);
  }
  if (ret) {
    error = ret;
    errorMethod = 5;
    onError();
  }
}
#endif

#ifdef SUPPORT_DEV_USB_HID5
static void onDevUsbChange5(ios_ret_t ret, usr_t unused) {
  if (ret >= 0) {
    ret = sendAttach5(onDevUsbAttach5, (usr_t)ret);
  }
  if (ret) {
    error = ret;
    errorMethod = 4;
    onError();
  }
}
#endif

#ifdef SUPPORT_DEV_USB_HID5
static void onDevUsbAttach5(ios_ret_t ret, usr_t vcount) {
  if (ret == 0) {
    int found = 0;
    int count = (int)vcount;
    for (int i = 0; i < DEV_USB_HID5_DEVICE_CHANGE_SIZE && i < count; i++) {
      uint32_t device_id = dev_usb_hid5_devices[i].id;
      if (dev_usb_hid5_devices[i].vid_pid == HAC_013_ID) {
        found = 1;
        if (gcn_adapter_id != device_id) {
          gcn_adapter_id = device_id;

          if (sendResume5(onDevUsbResume5, NULL))
            found = 0;
        }
        break;
      }
    }
    if (!found) gcn_adapter_id = (uint32_t)-1;
    ret = getDeviceChange5(onDevUsbChange5, NULL);
  }
  if (ret) {
    error = ret;
    errorMethod = 5;
    onError();
  }
}
#endif

#ifdef SUPPORT_DEV_USB_HID5
static void onDevUsbResume5(ios_ret_t ret, usr_t unused) {
  if (ret == 0) {
    ret = sendParams5(onDevUsbParams5, NULL);
  }
  if (ret) {
    error = ret;
    errorMethod = 6;
    gcn_adapter_id = -1;
  }
}
#endif

#ifdef SUPPORT_DEV_USB_HID5
static void onDevUsbParams5(ios_ret_t ret, usr_t unused) {
  if (ret == 0) {
    /* 0-7 are already correct :) */
    dev_usb_hid5_buffer[8] = 0;
    dev_usb_hid5_buffer[9] = 0;
    dev_usb_hid5_buffer[10] = 0;
    dev_usb_hid5_buffer[11] = 0;
    dev_usb_hid5_buffer[12] = 0;
    dev_usb_hid5_buffer[13] = 0;
    dev_usb_hid5_buffer[14] = 0;
    dev_usb_hid5_buffer[15] = 0;
    dev_usb_hid5_buffer[16] = gcn_adapter_id;
    dev_usb_hid5_buffer[17] = 0;
    dev_usb_hid5_buffer[18] = 0;
    dev_usb_hid5_buffer[19] = 0;
    dev_usb_hid5_buffer[20] = 0;
    dev_usb_hid5_buffer[21] = 0;
    dev_usb_hid5_buffer[22] = 0;
    dev_usb_hid5_buffer[23] = 0;
    dev_usb_hid5_buffer[24] = 0;
    dev_usb_hid5_buffer[25] = 0;
    dev_usb_hid5_buffer[26] = 0;
    dev_usb_hid5_buffer[27] = 0;
    dev_usb_hid5_buffer[28] = 0;
    dev_usb_hid5_buffer[29] = 0;
    dev_usb_hid5_buffer[30] = 0;
    dev_usb_hid5_buffer[31] = 0;
    onDevUsbInit(0, NULL);
  }
  if (ret) {
    error = ret;
    errorMethod = 7;
    gcn_adapter_id = -1;
  }
}
#endif

static int sendPoll(void) {
#ifdef SUPPORT_DEV_USB_HID5
#ifdef HAVE_VERSION
  if (version == 5)
#endif
  return sendPoll5(onDevUsbPoll, NULL);
#endif
#ifdef SUPPORT_DEV_USB_HID4
#ifdef HAVE_VERSION
  if (version == 4)
#endif
  return sendPoll4(onDevUsbPoll, NULL);
#endif
  return -1;
}

static int sendHandshake() {
#ifdef SUPPORT_DEV_USB_HID5
#ifdef HAVE_VERSION
  if (version == 5)
#endif
  return sendHandshake5(callbackIgnore, NULL);
#endif
#ifdef SUPPORT_DEV_USB_HID4
#ifdef HAVE_VERSION
  if (version == 4)
#endif
  return sendHandshake4(callbackIgnore, NULL);
#endif
  return -1;
}

static int sendHid() {
#ifdef SUPPORT_DEV_USB_HID5
#ifdef HAVE_VERSION
  if (version == 5)
#endif
  return sendHid5(callbackIgnore, NULL);
#endif
#ifdef SUPPORT_DEV_USB_HID4
#ifdef HAVE_VERSION
  if (version == 4)
#endif
  return sendHid4(callbackIgnore, NULL);
#endif
  return -1;
}

static int sendRumble(void) {
#ifdef SUPPORT_DEV_USB_HID5
#ifdef HAVE_VERSION
  if (version == 5)
#endif
  return sendRumble5(callbackIgnore, NULL);
#endif
#ifdef SUPPORT_DEV_USB_HID4
#ifdef HAVE_VERSION
  if (version == 4)
#endif
  return sendRumble4(callbackIgnore, NULL);
#endif
  return -1;
}

static int sendLed(void) {
#ifdef SUPPORT_DEV_USB_HID5
#ifdef HAVE_VERSION
  if (version == 5)
#endif
  return sendLed5(callbackIgnore, NULL);
#endif
#ifdef SUPPORT_DEV_USB_HID4
#ifdef HAVE_VERSION
  if (version == 4)
#endif
  return sendLed4(callbackIgnore, NULL);
#endif
  return -1;
}

static void onDevUsbInit(ios_ret_t ret, usr_t unused) {
  if (ret >= 0) {
    printf("USB init\n");
    ret = sendPoll();
  }
  if (ret) {
    printf("Error on USB Init %i\n", ret);
    error = ret;
    errorMethod = 8;
    gcn_adapter_id = -1;
  }
}

static void onDevUsbPoll(ios_ret_t ret, usr_t unused) {
  if (ret >= 0) {
    //printf("MSG: %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
    //poll_msg_buffer[0], poll_msg_buffer[1], poll_msg_buffer[2], poll_msg_buffer[3], poll_msg_buffer[4],
    //poll_msg_buffer[5], poll_msg_buffer[6], poll_msg_buffer[7], poll_msg_buffer[8], poll_msg_buffer[9],
    //poll_msg_buffer[10], poll_msg_buffer[11], poll_msg_buffer[12], poll_msg_buffer[13], poll_msg_buffer[14]);
    if (poll_msg_buffer[0] != 0x30) {
      if (poll_msg_buffer[0] == 0x81) {
        switch (poll_msg_buffer[1]) {
          case 0x01:
            printf("Starting handshake...\n");
            sendHandshake();
            break;
          case 0x02:
            printf("Establishing HID...\n");
            sendHid();
            break;
          case 0x04:
            printf("HID established...\n");
            sendLed();
            break;
        }
      }
      ret = 0;
    } else {
      uint32_t isr = cpu_isr_disable();
      for (int i = 0; i < GCN_CONTROLLER_COUNT; i++) {
        if (i >= 1) {
          gcn_data[i].error = PADData_ERROR_NO_CONNECTION;
          continue;
        }
        uint8_t *data = poll_msg_buffer + 2;
        uint8_t *analog = poll_msg_buffer + 6;
        int8_t lx = (((analog[1] & 0x0F) << 4) | ((analog[0] & 0xF0) >> 4)) + 127;
        int8_t ly = analog[2] + 127;
        int8_t rx = (((analog[4] & 0x0F) << 4) | ((analog[3] & 0xF0) >> 4)) + 127;
        int8_t ry = analog[5] + 127;
        uint32_t buttondata = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
        gcn_data[i].buttons =
          (buttondata & 0x00000800 ? PADData_BUTTON_A : 0) |
          (buttondata & 0x00000400 ? PADData_BUTTON_B : 0) |
          (buttondata & 0x00000200 ? PADData_BUTTON_X : 0) |
          (buttondata & 0x00000100 ? PADData_BUTTON_Y : 0) |
          (buttondata & 0x08000000 ? PADData_BUTTON_DL : 0) |
          (buttondata & 0x04000000 ? PADData_BUTTON_DR : 0) |
          (buttondata & 0x01000000 ? PADData_BUTTON_DD : 0) |
          (buttondata & 0x02000000 ? PADData_BUTTON_DU : 0) |
          (buttondata & 0x00020000 ? PADData_BUTTON_S : 0) |
          (buttondata & 0x00008000 ? PADData_BUTTON_Z : 0) |
          (buttondata & 0x40000000 ? PADData_BUTTON_L : 0) |
          (buttondata & 0x00004000 ? PADData_BUTTON_R : 0);
        gcn_data[i].aStickX = lx;
        gcn_data[i].aStickY = ly;
        gcn_data[i].cStickX = rx;
        gcn_data[i].cStickY = ry;
        gcn_data[i].sliderL = (buttondata & 0x40000000 ? 0xFF : 0);
        gcn_data[i].sliderR = (buttondata & 0x00004000 ? 0xFF : 0);
        gcn_data[i]._unknown8 = 0;
        gcn_data[i]._unknown9 = 0;
        gcn_data[i].error = 0;
        //printf("Buttons: %08x - LX: %02x, LY: %02x - RX: %02x, RY: %02x", buttondata, lx, ly, rx, ry);
      }
      //gcn_data_written = mftb();
      VIResetDimmingCount();
      cpu_isr_restore(isr);
    }
    ret = sendPoll();
  }
  if (ret) {
    printf("Error reading: %i", ret);
    error = ret;
    errorMethod = 9;
    gcn_adapter_id = -1;
  }
}

