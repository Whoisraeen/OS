#ifndef USB_H
#define USB_H

#include <stdint.h>

// Standard Device Requests
#define USB_REQ_GET_STATUS        0x00
#define USB_REQ_CLEAR_FEATURE     0x01
#define USB_REQ_SET_FEATURE       0x03
#define USB_REQ_SET_ADDRESS       0x05
#define USB_REQ_GET_DESCRIPTOR    0x06
#define USB_REQ_SET_DESCRIPTOR    0x07
#define USB_REQ_GET_CONFIGURATION 0x08
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_GET_INTERFACE     0x0A
#define USB_REQ_SET_INTERFACE     0x0B
#define USB_REQ_SYNCH_FRAME       0x0C

// Descriptor Types
#define USB_DESC_DEVICE           0x01
#define USB_DESC_CONFIGURATION    0x02
#define USB_DESC_STRING           0x03
#define USB_DESC_INTERFACE        0x04
#define USB_DESC_ENDPOINT         0x05
#define USB_DESC_DEVICE_QUALIFIER 0x06
#define USB_DESC_OTHER_SPEED_CONF 0x07
#define USB_DESC_INTERFACE_POWER  0x08
#define USB_DESC_HID              0x21
#define USB_DESC_HID_REPORT       0x22

// Device Class Codes
#define USB_CLASS_PER_INTERFACE   0x00
#define USB_CLASS_AUDIO           0x01
#define USB_CLASS_COMM            0x02
#define USB_CLASS_HID             0x03
#define USB_CLASS_PHYSICAL        0x05
#define USB_CLASS_IMAGE           0x06
#define USB_CLASS_PRINTER         0x07
#define USB_CLASS_MASS_STORAGE    0x08
#define USB_CLASS_HUB             0x09
#define USB_CLASS_CDC_DATA        0x0A
#define USB_CLASS_SMART_CARD      0x0B
#define USB_CLASS_SECURITY        0x0D
#define USB_CLASS_VIDEO           0x0E
#define USB_CLASS_HEALTHCARE      0x0F
#define USB_CLASS_DIAGNOSTIC      0xDC
#define USB_CLASS_WIRELESS        0xE0
#define USB_CLASS_MISC            0xEF
#define USB_CLASS_APP_SPEC        0xFE
#define USB_CLASS_VENDOR          0xFF

// Structures (Packed)
typedef struct {
    uint8_t length;
    uint8_t type;
    uint16_t bcd_usb;
    uint8_t device_class;
    uint8_t device_subclass;
    uint8_t device_protocol;
    uint8_t max_packet_size0;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t bcd_device;
    uint8_t manufacturer_idx;
    uint8_t product_idx;
    uint8_t serial_idx;
    uint8_t num_configurations;
} __attribute__((packed)) usb_device_descriptor_t;

typedef struct {
    uint8_t length;
    uint8_t type;
    uint16_t total_length;
    uint8_t num_interfaces;
    uint8_t configuration_value;
    uint8_t configuration_idx;
    uint8_t attributes;
    uint8_t max_power;
} __attribute__((packed)) usb_config_descriptor_t;

typedef struct {
    uint8_t length;
    uint8_t type;
    uint8_t interface_number;
    uint8_t alternate_setting;
    uint8_t num_endpoints;
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
    uint8_t interface_idx;
} __attribute__((packed)) usb_interface_descriptor_t;

typedef struct {
    uint8_t length;
    uint8_t type;
    uint8_t endpoint_address;
    uint8_t attributes;
    uint16_t max_packet_size;
    uint8_t interval;
} __attribute__((packed)) usb_endpoint_descriptor_t;

#endif // USB_H
