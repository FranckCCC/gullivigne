/*
 * Copyright (c) 2016-2019 Espressif Systems (Shanghai) PTE LTD & Cesanta Software Limited
 * All rights reserved
 *
 * This file is part of the esptool.py binary flasher stub.
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
 * Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include "soc_support.h"
#include "stub_write_flash.h"
#include "stub_flasher.h"
#include "rom_functions.h"
#include "miniz.h"

/* local flashing state

   This is wrapped in a structure because gcc 4.8
   generates significantly more code for ESP32
   if they are static variables (literal pool, I think!)
*/
static struct {
  /* set by flash_begin, cleared by flash_end */
  bool in_flash_mode;
  /* offset of next SPI write */
  uint32_t next_write;
  /* sector number for next erase */
  int next_erase_sector;
  /* number of output bytes remaining to write */
  uint32_t remaining;
  /* number of sectors remaining to erase */
  int remaining_erase_sector;
  /* last error generated by a data packet */
  esp_command_error last_error;

  /* inflator state for deflate write */
  tinfl_decompressor inflator;
  /* number of compressed bytes remaining to read */
  uint32_t remaining_compressed;
} fs;

/* SPI status bits */
static const uint32_t STATUS_WIP_BIT = (1 << 0);
#if ESP32_OR_LATER
static const uint32_t STATUS_QIE_BIT = (1 << 9);  /* Quad Enable */
#endif

bool is_in_flash_mode(void)
{
  return fs.in_flash_mode;
}

esp_command_error get_flash_error(void)
{
  return fs.last_error;
}

/* Wait for the SPI state machine to be ready,
   ie no command in progress in the internal host.
*/
inline static void spi_wait_ready(void)
{
  /* Wait for SPI state machine ready */
  while((READ_REG(SPI_EXT2_REG) & SPI_ST))
    { }
#if ESP32_OR_LATER
  while(READ_REG(SPI0_EXT2_REG) & SPI_ST)
  { }
#endif
}

/* Returns true if the spiflash is ready for its next write
   operation.

   Doesn't block, except for the SPI state machine to finish
   any previous SPI host operation.
*/
static bool spiflash_is_ready(void)
{
  spi_wait_ready();
  WRITE_REG(SPI_RD_STATUS_REG, 0);
  /* Issue read status command */
  WRITE_REG(SPI_CMD_REG, SPI_FLASH_RDSR);
  while(READ_REG(SPI_CMD_REG) != 0)
    { }
  uint32_t status_value = READ_REG(SPI_RD_STATUS_REG);
  return (status_value & STATUS_WIP_BIT) == 0;
}

static void spi_write_enable(void)
{
  while(!spiflash_is_ready())
    { }
  WRITE_REG(SPI_CMD_REG, SPI_FLASH_WREN);
  while(READ_REG(SPI_CMD_REG) != 0)
    { }
}

#if ESP32_OR_LATER
static esp_rom_spiflash_chip_t *flashchip = (esp_rom_spiflash_chip_t *)0x3ffae270;

/* Stub version of SPIUnlock() that replaces version in ROM.

   This works around a bug where SPIUnlock sometimes reads the wrong
   high status byte (RDSR2 result) and then copies it back to the
   flash status, causing lock bit CMP or Status Register Protect ` to
   become set.
 */
SpiFlashOpResult SPIUnlock(void)
{
  uint32_t status;

  spi_wait_ready(); /* ROM SPI_read_status_high() doesn't wait for this */
#if ESP32S2_OR_LATER
  if (SPI_read_status_high(flashchip, &status) != SPI_FLASH_RESULT_OK) {
    return SPI_FLASH_RESULT_ERR;
  }
#else
  if (SPI_read_status_high(&status) != SPI_FLASH_RESULT_OK) {
    return SPI_FLASH_RESULT_ERR;
  }
#endif

  /* Clear all bits except QIE, if it is set.
     (This is different from ROM SPIUnlock, which keeps all bits as-is.)
   */
  status &= STATUS_QIE_BIT;

  spi_write_enable();

  REG_SET_MASK(SPI_CTRL_REG, SPI_WRSR_2B);
  if (SPI_write_status(flashchip, status) != SPI_FLASH_RESULT_OK) {
    return SPI_FLASH_RESULT_ERR;
  }

  return SPI_FLASH_RESULT_OK;
}
#endif

esp_command_error handle_flash_begin(uint32_t total_size, uint32_t offset) {
  fs.in_flash_mode = true;
  fs.next_write = offset;
  fs.next_erase_sector = offset / FLASH_SECTOR_SIZE;
  fs.remaining = total_size;
  fs.remaining_erase_sector = ((offset % FLASH_SECTOR_SIZE) + total_size + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE;
  fs.last_error = ESP_OK;

  if (SPIUnlock() != 0) {
    return ESP_FAILED_SPI_UNLOCK;
  }

  return ESP_OK;
}

esp_command_error handle_flash_deflated_begin(uint32_t uncompressed_size, uint32_t compressed_size, uint32_t offset) {
  esp_command_error err = handle_flash_begin(uncompressed_size, offset);
  tinfl_init(&fs.inflator);
  fs.remaining_compressed = compressed_size;
  return err;
}

/* Erase the next sector or block (depending if we're at a block boundary).

   Updates fs.next_erase_sector & fs.remaining_erase_sector on success.

   If nothing left to erase, returns immediately.

   Returns immediately if SPI flash not yet ready for a write operation.

   Does not wait for the erase to complete - the next SPI operation
   should check if a write operation is currently in progress.
 */
static void start_next_erase(void)
{
  if(fs.remaining_erase_sector == 0)
    return; /* nothing left to erase */
  if(!spiflash_is_ready())
    return; /* don't wait for flash to be ready, caller will call again if needed */

  spi_write_enable();

  uint32_t command = SPI_FLASH_SE; /* sector erase, 4KB */
  uint32_t sectors_to_erase = 1;
  if(fs.remaining_erase_sector >= SECTORS_PER_BLOCK
     && fs.next_erase_sector % SECTORS_PER_BLOCK == 0) {
    /* perform a 64KB block erase if we have space for it */
    command = SPI_FLASH_BE;
    sectors_to_erase = SECTORS_PER_BLOCK;
  }

  uint32_t addr = fs.next_erase_sector * FLASH_SECTOR_SIZE;
  spi_wait_ready();
  WRITE_REG(SPI_ADDR_REG, addr & 0xffffff);
  WRITE_REG(SPI_CMD_REG, command);
  while(READ_REG(SPI_CMD_REG) != 0)
    { }
  fs.remaining_erase_sector -= sectors_to_erase;
  fs.next_erase_sector += sectors_to_erase;
}

/* Write data to flash (either direct for non-compressed upload, or
   freshly decompressed.) Erases as it goes.

   Updates fs.remaining_erase_sector, fs.next_write, and fs.remaining
*/
void handle_flash_data(void *data_buf, uint32_t length) {
  int last_sector;

  if (length > fs.remaining) {
      /* Trim the final block, as it may have padding beyond
         the length we are writing */
      length = fs.remaining;
  }

  if (length == 0) {
      return;
  }

  /* what sector is this write going to end in?
     make sure we've erased at least that far.
  */
  last_sector = (fs.next_write + length) / FLASH_SECTOR_SIZE;
  while(fs.remaining_erase_sector > 0 && fs.next_erase_sector <= last_sector) {
    start_next_erase();
  }
  while(!spiflash_is_ready())
    {}

  /* do the actual write */
  if (SPIWrite(fs.next_write, data_buf, length)) {
    fs.last_error = ESP_FAILED_SPI_OP;
  }
  fs.next_write += length;
  fs.remaining -= length;
}

#if !ESP8266
/* Write encrypted data to flash (either direct for non-compressed upload, or
   freshly decompressed.) Erases as it goes.

   Updates fs.remaining_erase_sector, fs.next_write, and fs.remaining
*/
void handle_flash_encrypt_data(void *data_buf, uint32_t length) {
  int last_sector;
  int res;

#if ESP32S2_OR_LATER
  SPI_Write_Encrypt_Enable();
#endif

  if (length > fs.remaining) {
      /* Trim the final block, as it may have padding beyond
         the length we are writing */
      length = fs.remaining;
  }

  if (length == 0) {
      return;
  }

  /* what sector is this write going to end in?
     make sure we've erased at least that far.
  */
  last_sector = (fs.next_write + length) / FLASH_SECTOR_SIZE;
  while(fs.remaining_erase_sector > 0 && fs.next_erase_sector <= last_sector) {
    start_next_erase();
  }
  while(!spiflash_is_ready())
    {}

  /* do the actual write */
#if ESP32
  res = esp_rom_spiflash_write_encrypted(fs.next_write, data_buf, length);
#else
  res = SPI_Encrypt_Write(fs.next_write, data_buf, length);
#endif

  if (res) {
    fs.last_error = ESP_FAILED_SPI_OP;
  }
  fs.next_write += length;
  fs.remaining -= length;

#if ESP32S2_OR_LATER
  SPI_Write_Encrypt_Disable();
#endif
}

#endif // !ESP8266

void handle_flash_deflated_data(void *data_buf, uint32_t length) {
  static uint8_t out_buf[32768];
  static uint8_t *next_out = out_buf;
  int status = TINFL_STATUS_NEEDS_MORE_INPUT;

  while(length > 0 && fs.remaining > 0 && status > TINFL_STATUS_DONE) {
    size_t in_bytes = length; /* input remaining */
    size_t out_bytes = out_buf + sizeof(out_buf) - next_out; /* output space remaining */
    int flags = TINFL_FLAG_PARSE_ZLIB_HEADER;
    if(fs.remaining_compressed > length) {
      flags |= TINFL_FLAG_HAS_MORE_INPUT;
    }

    /* start an opportunistic erase: decompressing takes time, so might as
       well be running a SPI erase in the background. */
    start_next_erase();

    status = tinfl_decompress(&fs.inflator, data_buf, &in_bytes,
                     out_buf, next_out, &out_bytes,
                     flags);

    fs.remaining_compressed -= in_bytes;
    length -= in_bytes;
    data_buf += in_bytes;

    next_out += out_bytes;
    size_t bytes_in_out_buf = next_out - out_buf;
    if (status <= TINFL_STATUS_DONE || bytes_in_out_buf == sizeof(out_buf)) {
      // Output buffer full, or done
      handle_flash_data(out_buf, bytes_in_out_buf);
      next_out = out_buf;
    }
  } // while

  if (status < TINFL_STATUS_DONE) {
    /* error won't get sent back to esptool.py until next block is sent */
    fs.last_error = ESP_INFLATE_ERROR;
  }

  if (status == TINFL_STATUS_DONE && fs.remaining > 0) {
    fs.last_error = ESP_NOT_ENOUGH_DATA;
  }
  if (status != TINFL_STATUS_DONE && fs.remaining == 0) {
    fs.last_error = ESP_TOO_MUCH_DATA;
  }
}

esp_command_error handle_flash_end(void)
{
  if (!fs.in_flash_mode) {
    return ESP_NOT_IN_FLASH_MODE;
  }

  if (fs.remaining > 0) {
    return ESP_NOT_ENOUGH_DATA;
  }

  fs.in_flash_mode = false;
  return fs.last_error;
}
