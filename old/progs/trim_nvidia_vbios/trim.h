#define HEADER_LENGTH 16
#define INPUT_SIZE_THRESHOLD 1000

uint8_t
vbios_header[HEADER_LENGTH] = {
	0x55, 0xAA,				// U
	0x00,					// skipped
	0xEB, 0x4B, 0x37, 0x34, 0x30,		// signature
	0x30, 0xE9, 0x4C, 0x19, 0x77,		// signature
	0xCC, 0x56, 0x49			// IVI(DEO)
};

uint8_t
vbios_mask[HEADER_LENGTH] = {
	1, 1,
	0,
	1, 1, 1, 1, 1,
	1, 1, 1, 1, 1,
	1, 1, 1
};
