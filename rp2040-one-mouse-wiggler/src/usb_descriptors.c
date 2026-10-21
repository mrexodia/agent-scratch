// USB descriptors cloned from a Logitech M-U0026 "USB Optical Mouse"
// (046d:c077). See README.md for how to compare against your own mouse.

#include "tusb.h"

static tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x046D,
    .idProduct          = 0xC077,
    .bcdDevice          = 0x7200,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x00,
    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

// 46 bytes: 8 buttons, X, Y, wheel (4-byte reports, no report ID).
static uint8_t const desc_hid_report[] = {
    0x05, 0x01,  // Usage Page (Generic Desktop)
    0x09, 0x02,  // Usage (Mouse)
    0xA1, 0x01,  // Collection (Application)
    0x09, 0x01,  //   Usage (Pointer)
    0xA1, 0x00,  //   Collection (Physical)
    0x05, 0x09,  //     Usage Page (Button)
    0x19, 0x01,  //     Usage Minimum (1)
    0x29, 0x08,  //     Usage Maximum (8)
    0x15, 0x00,  //     Logical Minimum (0)
    0x25, 0x01,  //     Logical Maximum (1)
    0x95, 0x08,  //     Report Count (8)
    0x75, 0x01,  //     Report Size (1)
    0x81, 0x02,  //     Input (Data, Var, Abs)
    0x05, 0x01,  //     Usage Page (Generic Desktop)
    0x09, 0x30,  //     Usage (X)
    0x09, 0x31,  //     Usage (Y)
    0x09, 0x38,  //     Usage (Wheel)
    0x15, 0x81,  //     Logical Minimum (-127)
    0x25, 0x7F,  //     Logical Maximum (127)
    0x75, 0x08,  //     Report Size (8)
    0x95, 0x03,  //     Report Count (3)
    0x81, 0x06,  //     Input (Data, Var, Rel)
    0xC0,        //   End Collection
    0xC0,        // End Collection
};
TU_VERIFY_STATIC(sizeof(desc_hid_report) == 46, "report descriptor must be 46 bytes");

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return desc_hid_report;
}

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

// Bus powered + remote wakeup (0xA0), 100 mA, one boot-mouse interface with a
// 4-byte interrupt IN endpoint polled every 10 ms.
static uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_MOUSE, sizeof(desc_hid_report), 0x81, 4, 10),
};
TU_VERIFY_STATIC(sizeof(desc_configuration) == 0x22, "wTotalLength must be 0x22");

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

static char const *const string_desc_arr[] = {
    NULL,                 // 0: language ID, handled below
    "Logitech",           // 1: manufacturer
    "USB Optical Mouse",  // 2: product
};

static uint16_t desc_str[32 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    size_t chr_count;

    if (index == 0) {
        desc_str[1] = 0x0409;  // English (US)
        chr_count = 1;
    } else {
        if (index >= TU_ARRAY_SIZE(string_desc_arr)) return NULL;
        char const *str = string_desc_arr[index];
        chr_count = strlen(str);
        if (chr_count > 32) chr_count = 32;
        for (size_t i = 0; i < chr_count; i++) desc_str[1 + i] = str[i];
    }

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return desc_str;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
}
