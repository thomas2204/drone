/* Application-owned extension to the CubeMX linker script.
 * Kept separate so regeneration cannot discard the DMA memory placement.
 * RAM is D1 AXI SRAM; RAM_D2 is SRAM for DMA1/DMA2 buffers.
 */
SECTIONS
{
  .sd_dma_buffer (NOLOAD) : ALIGN(32)
  {
    *(.sd_dma_buffer)
    *(.sd_dma_buffer*)
    . = ALIGN(32);
  } >RAM

  .dma_buffer (NOLOAD) : ALIGN(32)
  {
    *(.dma_buffer)
    *(.dma_buffer*)
    . = ALIGN(32);
  } >RAM_D2
}
ASSERT(SDFatFS >= ORIGIN(RAM) && SDFatFS < ORIGIN(RAM) + LENGTH(RAM),
       "SDFatFS must be in AXI SRAM for SDMMC1 IDMA")
ASSERT(SDFile >= ORIGIN(RAM) && SDFile < ORIGIN(RAM) + LENGTH(RAM),
       "SDFile must be in AXI SRAM for SDMMC1 IDMA")
