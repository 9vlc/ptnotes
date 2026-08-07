#pragma once

/* Intel dGPU MMIO registers */
#define IOREG_INTEL_GET_REGION	0x102090 /* Get ROM regions */
#define IOREG_INTEL_SEL_REGION	0x102084 /* Select ROM region */
#define IOREG_INTEL_GET_ROM_OFF	0x1020C0 /* Get OpROM offset in region */
#define IOREG_INTEL_SEL_ADDR	0x102080 /* Offset to read */
#define IOREG_INTEL_READ_ADDR	0x102040 /* Returned read DWORD */

/* Intel masks */
#define MASK_INTEL_ROM_REGION	0xFF
#define MASK_INTEL_ROM_OFFSET	0x1F0000

/* AMD GPU MMIO registers for SOC15+ */
#define IOREG_AMD_ROM_INDEX	0x5A0A0 /* Select ROM index */
#define IOREG_AMD_ROM_DATA	0x5A0A4 /* Read ROM */

/* Nvidia GPU MMIO ROM offset */
#define IOREG_NVIDIA_ROM_OFFSET	0x300000 /* ROM located directly over here */
