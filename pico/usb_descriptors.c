/*
 * usb_descriptors.c  —  Pi Pico / TinyUSB
 * Skylander Portal of Power  VID=0x1430  PID=0x0150
 *
 * SSA uses the legacy descriptor shape exposed by KAOS and by the PS3
 * portal model: two interrupt endpoints and a 29-byte report descriptor.
 * Later portal types keep the KaosDual Extended input-only descriptor.
 */
#include "usb_descriptors.h"
#include "tusb.h"
#include <string.h>

extern uint8_t portal_get_type(void);

/* ---- Device descriptor ---- */
uint8_t const *tud_descriptor_device_cb(void) {
    static tusb_desc_device_t desc;
    const uint8_t portal_type = portal_get_type();

    desc.bLength            = sizeof(tusb_desc_device_t);
    desc.bDescriptorType    = TUSB_DESC_DEVICE;
    desc.bcdUSB             = (portal_type == 0 || portal_type == 2) ? 0x0200 : 0x0110;
    desc.bDeviceClass       = 0x00;
    desc.bDeviceSubClass    = 0x00;
    desc.bDeviceProtocol    = 0x00;
    desc.bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE;
    desc.idVendor           = PORTAL_USB_VID;
    desc.idProduct          = PORTAL_USB_PID;
    desc.bcdDevice          = (portal_type == 0) ? 0x0100 :
                              (portal_type == 2) ? 0x0200 : 0x0001;
    desc.iManufacturer      = 0x01;
    desc.iProduct           = 0x02;
    desc.iSerialNumber      = (portal_type == 0) ? 0x00 : 0x03;
    desc.bNumConfigurations = 0x01;
    return (uint8_t const*)&desc;
}

/* ---- HID report descriptors ---- */
static const uint8_t desc_hid_report_ssa[] = {
    0x06,0x00,0xFF,
    0x09,0x01,
    0xA1,0x01,
    0x19,0x01,0x29,0x40,0x15,0x00,0x26,0xFF,0x00,
    0x75,0x08,0x95,0x20,0x81,0x00,
    0x19,0x01,0x29,0xFF,0x91,0x00,
    0xC0,
};

static const uint8_t desc_hid_report_modern[] = {
    0x06,0x00,0xFF,
    0x09,0x01,
    0xA1,0x01,
    0x19,0x01,0x29,0x40,0x15,0x00,0x26,0xFF,0x00,
    0x75,0x08,0x95,0x20,0x81,0x00,
    0x19,0x01,0x29,0x40,0x15,0x00,0x26,0xFF,0x00,
    0x75,0x08,0x95,0x20,0x91,0x00,
    0xC0,
};

/* ---- Configuration descriptors ---- */
#define SSA_CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN)
#define MODERN_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

/* Keep descriptor order and values byte-for-byte compatible with KAOS. */
static const uint8_t desc_config_ssa[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, SSA_CONFIG_TOTAL_LEN, 0, 500),

    0x09, TUSB_DESC_INTERFACE,
    0x00, 0x00, 0x02,
    TUSB_CLASS_HID, 0x00, 0x00, 0x00,

    0x09, HID_DESC_TYPE_HID,
    0x11, 0x01, 0x00, 0x01,
    HID_DESC_TYPE_REPORT,
    sizeof(desc_hid_report_ssa), 0x00,

    0x07, TUSB_DESC_ENDPOINT, PORTAL_EP_IN, TUSB_XFER_INTERRUPT,
    PORTAL_SSA_EP_SIZE, 0x00, PORTAL_EP_POLL_MS,

    0x07, TUSB_DESC_ENDPOINT, PORTAL_SSA_EP_OUT, TUSB_XFER_INTERRUPT,
    PORTAL_SSA_EP_SIZE, 0x00, PORTAL_EP_POLL_MS,
};

_Static_assert(sizeof(desc_hid_report_ssa) == 29, "SSA report descriptor must match KAOS");
_Static_assert(sizeof(desc_config_ssa) == 41, "SSA configuration descriptor must match KAOS");

static const uint8_t desc_config_modern[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, MODERN_CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_NONE,
                       sizeof(desc_hid_report_modern),
                       PORTAL_EP_IN,
                       PORTAL_HID_REPORT_LEN,
                       PORTAL_EP_POLL_MS),
};

/* ---- String descriptors ---- */
uint8_t const *tud_hid_descriptor_report_cb(uint8_t i) {
    (void)i;
    return (portal_get_type() == 0) ? desc_hid_report_ssa : desc_hid_report_modern;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t i) {
    (void)i;
    return (portal_get_type() == 0) ? desc_config_ssa : desc_config_modern;
}

uint16_t const *tud_descriptor_string_cb(uint8_t idx, uint16_t langid) {
    (void)langid;
    static uint16_t buf[32];
    uint8_t n;

    const uint8_t portal_type = portal_get_type();
    const char *product = (portal_type == 0) ? "Spyro Porta" :
                          (portal_type == 2) ? "Traptanium Portal" : "Spyro Portals";
    const char *strs[] = {
        (const char[]){0x09,0x04},
        "Activision",
        product,
        "00000001",
    };

    if (idx == 0) { memcpy(&buf[1], strs[0], 2); n=1; }
    else {
        if (idx >= sizeof(strs)/sizeof(strs[0])) return NULL;
        const char *str = strs[idx];
        n = strlen(str); if(n>31) n=31;
        for(uint8_t i=0;i<n;i++) buf[1+i]=str[i];
    }
    buf[0] = (uint16_t)((TUSB_DESC_STRING<<8)|(2*n+2));
    return buf;
}
