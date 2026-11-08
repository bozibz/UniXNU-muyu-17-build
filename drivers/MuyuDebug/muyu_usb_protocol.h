/*
 * muyu: CDC ACM descriptors and the DMA/request predicates for the debug port (u128).
 * Copied from ios-vermeer-lab/xnu/vermeer/vermeer_usb_protocol.h.  The two SoC-specific
 * predicates (SMMU stream match, identity route) are gone: muyu runs this port with the PHY,
 * clocks and SMMU exactly as ABL left them, so the first experiment does not try to verify them.
 */
#ifndef MUYU_USB_PROTOCOL_H
#define MUYU_USB_PROTOCOL_H

#include <sys/types.h>

/* USB 2.0, class 2/2/1 (CDC ACM), 1209:0001 (pid.codes lab range). */
static const uint8_t vusb_device[18] = {
	18, 1, 0x00, 0x02, 2, 2, 1, 64, 0x09, 0x12, 1, 0, 0, 4, 1, 2, 3, 1
};

static const uint8_t vusb_config[67] = {
	9, 2, 67, 0, 2, 1, 0, 0x80, 250,
	9, 4, 0, 0, 1, 2, 2, 1, 0,
	5, 0x24, 0, 0x10, 1,
	5, 0x24, 1, 0, 1,
	4, 0x24, 2, 2,
	5, 0x24, 6, 0, 1,
	7, 5, 0x82, 3, 16, 0, 9,
	9, 4, 1, 0, 2, 0x0a, 0, 0, 0,
	7, 5, 0x01, 2, 0, 2, 0,
	7, 5, 0x81, 2, 0, 2, 0
};

static const uint8_t vusb_qualifier[10] = { 10, 6, 0, 2, 2, 2, 1, 64, 1, 0 };

/*
 * u131: 4 KiB is the real requirement.  GEVNTADR/DALEP/TRB all only need page alignment, and
 * this allocator ignores an alignment mask larger than a page (asking for 16 KiB still returned
 * 0x8caeeb000).  Demanding 16 KiB bought nothing and refused every buffer.
 */
static inline int
vusb_dma_allowed(uint64_t pa, unsigned size, unsigned bits)
{
	if (size != 0x4000 || (pa & 0xfff)) {
		return 0;
	}
	if (bits < 32 || bits > 64) {
		return 0;
	}
	if (bits < 64 && (size > (1ULL << bits) || pa + size - 1 >= (1ULL << bits))) {
		return 0;
	}
	return 1;
}

/* Return only recognized request kinds; malformed class OUT lengths stall. */
enum {
	VUSB_BAD, VUSB_DESC, VUSB_ADDRESS, VUSB_CONFIG, VUSB_GET_CONFIG,
	VUSB_STATUS, VUSB_INTERFACE, VUSB_GET_INTERFACE, VUSB_SET_LINE,
	VUSB_GET_LINE, VUSB_CONTROL, VUSB_BREAK, VUSB_CLEAR_HALT
};

static inline int
vusb_request(uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t len)
{
	if (type == 0x80 && req == 6 && ((value >> 8) >= 1 && (value >> 8) <= 7)) {
		return VUSB_DESC;
	}
	if (type == 0 && req == 5 && !index && !len && value < 128) {
		return VUSB_ADDRESS;
	}
	if (type == 0 && req == 9 && !index && !len && value <= 1) {
		return VUSB_CONFIG;
	}
	if (type == 0x80 && req == 8 && !value && !index && len == 1) {
		return VUSB_GET_CONFIG;
	}
	if ((type == 0x80 || type == 0x81 || type == 0x82) && req == 0 && !value && len == 2) {
		return VUSB_STATUS;
	}
	if (type == 1 && req == 11 && index <= 1 && !value && !len) {
		return VUSB_INTERFACE;
	}
	if (type == 0x81 && req == 10 && index <= 1 && !value && len == 1) {
		return VUSB_GET_INTERFACE;
	}
	if (type == 0x21 && req == 0x20 && !index && !value && len == 7) {
		return VUSB_SET_LINE;
	}
	if (type == 0xa1 && req == 0x21 && !index && !value && len == 7) {
		return VUSB_GET_LINE;
	}
	if (type == 0x21 && req == 0x22 && !index && !len && !(value & ~3)) {
		return VUSB_CONTROL;
	}
	if (type == 0x21 && req == 0x23 && !index && !len) {
		return VUSB_BREAK;
	}
	if (type == 2 && req == 1 && !value && !len &&
	    (index == 1 || index == 0x81 || index == 0x82)) {
		return VUSB_CLEAR_HALT;
	}
	return VUSB_BAD;
}

#endif /* MUYU_USB_PROTOCOL_H */
