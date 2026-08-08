#pragma once
#ifndef USB_DESCRIPTORS_H
#define USB_DESCRIPTORS_H

#define PORTAL_USB_VID        0x1430
#define PORTAL_USB_PID        0x0150
#define PORTAL_HID_REPORT_LEN 32
#define PORTAL_EP_IN          0x81   /* interrupt IN — portal → host */
#define PORTAL_SSA_EP_OUT     0x02   /* legacy KAOS/PS3 descriptor only */
#define PORTAL_SSA_EP_SIZE    64
#define PORTAL_EP_POLL_MS     1      /* 1ms polling interval */

/* Portal commands normally arrive through HID SET_REPORT on EP0.  The SSA
 * configuration also exposes the legacy interrupt OUT endpoint because the
 * original PS3 title is sensitive to the complete interface descriptor. */

#endif
