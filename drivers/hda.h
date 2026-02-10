#ifndef HDA_H
#define HDA_H

#include <stdint.h>
#include "pci.h"

// Intel HDA Registers
#define HDA_GCAP      0x00
#define HDA_VMIN      0x02
#define HDA_VMAJ      0x03
#define HDA_OUTPAY    0x04
#define HDA_INPAY     0x06
#define HDA_GCTL      0x08
#define HDA_WAKEEN    0x0C
#define HDA_STATESTS  0x0E
#define HDA_GSTS      0x10
#define HDA_INTCTL    0x20
#define HDA_INTSTS    0x24
#define HDA_WALCLK    0x30
#define HDA_SSYNC     0x38

#define HDA_CORB_LBASE 0x40
#define HDA_CORB_UBASE 0x44
#define HDA_CORB_WP    0x48
#define HDA_CORB_RP    0x4A
#define HDA_CORB_CTL   0x4C
#define HDA_CORB_STS   0x4D
#define HDA_CORB_SIZE  0x4E

#define HDA_RIRB_LBASE 0x50
#define HDA_RIRB_UBASE 0x54
#define HDA_RIRB_WP    0x58
#define HDA_RIRB_INTCNT 0x5A
#define HDA_RIRB_CTL   0x5C
#define HDA_RIRB_STS   0x5D
#define HDA_RIRB_SIZE  0x5E

#define HDA_ICOI       0x60
#define HDA_ICII       0x64
#define HDA_ICIS       0x68

#define HDA_DPLBASE    0x70
#define HDA_DPUBASE    0x74

// Stream Descriptor Offsets (Input and Output)
#define HDA_SD_CTL     0x00
#define HDA_SD_STS     0x03
#define HDA_SD_LPIB    0x04
#define HDA_SD_CBL     0x08
#define HDA_SD_LVI     0x0C
#define HDA_SD_FIFOW   0x0E // Word
#define HDA_SD_FIFOS   0x10 // Word
#define HDA_SD_FMT     0x12 // Word
#define HDA_SD_BDLPL   0x18
#define HDA_SD_BDLPU   0x1C

// HDA Codec Verbs
#define HDA_VERB_GET_PARAM      0xF0000
#define HDA_VERB_GET_CONN_LIST  0xF0200
#define HDA_VERB_SET_CONN_SEL   0x70100
#define HDA_VERB_GET_CONN_SEL   0xF0100
#define HDA_VERB_GET_PIN_CTL    0xF0700
#define HDA_VERB_SET_PIN_CTL    0x70700
#define HDA_VERB_SET_AMP_GAIN   0x30000
#define HDA_VERB_GET_AMP_GAIN   0xB0000
#define HDA_VERB_SET_POWER      0x70500
#define HDA_VERB_GET_POWER      0xF0500
#define HDA_VERB_SET_FMT        0x20000
#define HDA_VERB_GET_FMT        0xA0000
#define HDA_VERB_SET_STREAM_CH  0x70600
#define HDA_VERB_GET_STREAM_CH  0xF0600

// Parameter IDs
#define HDA_PARAM_VENDOR_ID     0x00
#define HDA_PARAM_REV_ID        0x02
#define HDA_PARAM_NODE_COUNT    0x04
#define HDA_PARAM_FUNC_GROUP    0x05
#define HDA_PARAM_AUDIO_FG_CAP  0x08
#define HDA_PARAM_PIN_CAP       0x0C
#define HDA_PARAM_IN_AMP_CAP    0x0D
#define HDA_PARAM_OUT_AMP_CAP   0x12
#define HDA_PARAM_CONN_LIST_LEN 0x0E
#define HDA_PARAM_POWER_STATE   0x0F
#define HDA_PARAM_PROC_CAP      0x10
#define HDA_PARAM_GPIO_COUNT    0x11
#define HDA_PARAM_VOL_KNOB_CAP  0x13

void hda_init(void);

#endif
