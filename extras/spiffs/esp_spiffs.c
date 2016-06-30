/**
 * ESP8266 SPIFFS HAL configuration.
 *
 * Part of esp-open-rtos
 * Copyright (c) 2016 sheinz https://github.com/sheinz
 * MIT License
 */
#include "esp_spiffs.h"
#include "spiffs.h"
#include <espressif/spi_flash.h>
#include <stdbool.h>
#include <esp/uart.h>

spiffs fs;

static void *work_buf = 0;
static void *fds_buf = 0;
static void *cache_buf = 0;

/*
 * Flash addresses and size alignment is a rip-off of Arduino implementation.
 */

static s32_t esp_spiffs_read(u32_t addr, u32_t size, u8_t *dst)
{
    uint32_t result = SPIFFS_OK;
    uint32_t alignedBegin = (addr + 3) & (~3);
    uint32_t alignedEnd = (addr + size) & (~3);
    if (alignedEnd < alignedBegin) {
        alignedEnd = alignedBegin;
    }

    if (addr < alignedBegin) {
        uint32_t nb = alignedBegin - addr;
        uint32_t tmp;
        if (sdk_spi_flash_read(alignedEnd - 4, &tmp, 4) != SPI_FLASH_RESULT_OK) {
            printf("spi_flash_read failed\n");
            return SPIFFS_ERR_INTERNAL;
        }
        memcpy(dst, &tmp + 4 - nb, nb);
    }

    if (alignedEnd != alignedBegin) {
        if (sdk_spi_flash_read(alignedBegin,
                    (uint32_t*) (dst + alignedBegin - addr),
                    alignedEnd - alignedBegin) != SPI_FLASH_RESULT_OK) {
            printf("spi_flash_read failed\n");
            return SPIFFS_ERR_INTERNAL;
        }
    }

    if (addr + size > alignedEnd) {
        uint32_t nb = addr + size - alignedEnd;
        uint32_t tmp;
        if (sdk_spi_flash_read(alignedEnd, &tmp, 4) != SPI_FLASH_RESULT_OK) {
            printf("spi_flash_read failed\n");
            return SPIFFS_ERR_INTERNAL;
        }

        memcpy(dst + size - nb, &tmp, nb);
    }

    return result;
}

static const int UNALIGNED_WRITE_BUFFER_SIZE = 512;

static s32_t esp_spiffs_write(u32_t addr, u32_t size, u8_t *src)
{
    uint32_t alignedBegin = (addr + 3) & (~3);
    uint32_t alignedEnd = (addr + size) & (~3);
    if (alignedEnd < alignedBegin) {
        alignedEnd = alignedBegin;
    }

    if (addr < alignedBegin) {
        uint32_t ofs = alignedBegin - addr;
        uint32_t nb = (size < ofs) ? size : ofs;
        uint8_t tmp[4] __attribute__((aligned(4))) = {0xff, 0xff, 0xff, 0xff};
        memcpy(tmp + 4 - ofs, src, nb);
        if (sdk_spi_flash_write(alignedBegin - 4, (uint32_t*) tmp, 4)
                != SPI_FLASH_RESULT_OK) {
            printf("spi_flash_write failed\n");
            return SPIFFS_ERR_INTERNAL;
        }
    }

    if (alignedEnd != alignedBegin) {
        uint32_t* srcLeftover = (uint32_t*) (src + alignedBegin - addr);
        uint32_t srcAlign = ((uint32_t) srcLeftover) & 3;
        if (!srcAlign) {
            if (sdk_spi_flash_write(alignedBegin, (uint32_t*) srcLeftover,
                    alignedEnd - alignedBegin) != SPI_FLASH_RESULT_OK) {
                printf("spi_flash_write failed\n");
                return SPIFFS_ERR_INTERNAL;
            }
        }
        else {
            uint8_t buf[UNALIGNED_WRITE_BUFFER_SIZE];
            for (uint32_t sizeLeft = alignedEnd - alignedBegin; sizeLeft; ) {
                size_t willCopy = sizeLeft < sizeof(buf) ? sizeLeft : sizeof(buf);
                memcpy(buf, srcLeftover, willCopy);

                if (sdk_spi_flash_write(alignedBegin, (uint32_t*) buf, willCopy)
                        != SPI_FLASH_RESULT_OK) {
                    printf("spi_flash_write failed\n");
                    return SPIFFS_ERR_INTERNAL;
                }

                sizeLeft -= willCopy;
                srcLeftover += willCopy;
                alignedBegin += willCopy;
            }
        }
    }

    if (addr + size > alignedEnd) {
        uint32_t nb = addr + size - alignedEnd;
        uint32_t tmp = 0xffffffff;
        memcpy(&tmp, src + size - nb, nb);

        if (sdk_spi_flash_write(alignedEnd, &tmp, 4) != SPI_FLASH_RESULT_OK) {
            printf("spi_flash_write failed\n");
            return SPIFFS_ERR_INTERNAL;
        }
    }

    return SPIFFS_OK;
}

static s32_t esp_spiffs_erase(u32_t addr, u32_t size)
{
    if (addr % SPI_FLASH_SEC_SIZE) {
        printf("Unaligned erase addr=%x\n", addr);
    }
    if (size % SPI_FLASH_SEC_SIZE) {
        printf("Unaligned erase size=%d\n", size);
    }

    const uint32_t sector = addr / SPI_FLASH_SEC_SIZE;
    const uint32_t sectorCount = size / SPI_FLASH_SEC_SIZE;

    for (uint32_t i = 0; i < sectorCount; ++i) {
        sdk_spi_flash_erase_sector(sector + i);
    }
    return SPIFFS_OK;
}

int32_t esp_spiffs_mount()
{
    spiffs_config config = {0};

    config.hal_read_f = esp_spiffs_read;
    config.hal_write_f = esp_spiffs_write;
    config.hal_erase_f = esp_spiffs_erase;

    size_t workBufSize = 2 * SPIFFS_CFG_LOG_PAGE_SZ();
    size_t fdsBufSize = SPIFFS_buffer_bytes_for_filedescs(&fs, 5);
    size_t cacheBufSize = SPIFFS_buffer_bytes_for_cache(&fs, 5);

    work_buf = malloc(workBufSize);
    fds_buf = malloc(fdsBufSize);
    cache_buf = malloc(cacheBufSize);
    printf("spiffs memory, work_buf_size=%d, fds_buf_size=%d, cache_buf_size=%d\n",
            workBufSize, fdsBufSize, cacheBufSize);

    int32_t err = SPIFFS_mount(&fs, &config, work_buf, fds_buf, fdsBufSize,
            cache_buf, cacheBufSize, 0);

    if (err != SPIFFS_OK) {
        printf("Error spiffs mount: %d\n", err);
    }

    return err;
}

void esp_spiffs_unmount()
{
    SPIFFS_unmount(&fs);

    free(work_buf);
    free(fds_buf);
    free(cache_buf);

    work_buf = 0;
    fds_buf = 0;
    cache_buf = 0;
}

/* syscall implementation for stdio write to UART */
long _write_r(struct _reent *r, int fd, const char *ptr, int len )
{
    if(fd != r->_stdout->_file) {
        return SPIFFS_write(&fs, (spiffs_file)fd, (char*)ptr, len);
    }
    for(int i = 0; i < len; i++) {
        /* Auto convert CR to CRLF, ignore other LFs (compatible with Espressif SDK behaviour) */
        if(ptr[i] == '\r')
            continue;
        if(ptr[i] == '\n')
            uart_putc(0, '\r');
        uart_putc(0, ptr[i]);
    }
    return len;
}

/* syscall implementation for stdio read from UART */
long _read_r( struct _reent *r, int fd, char *ptr, int len )
{
    int ch, i;

    if(fd != r->_stdin->_file) {
        return SPIFFS_read(&fs, (spiffs_file)fd, ptr, len);
    }
    uart_rxfifo_wait(0, 1);
    for(i = 0; i < len; i++) {
        ch = uart_getc_nowait(0);
        if (ch < 0) break;
        ptr[i] = ch;
    }
    return i;
}

/* syscall implementation for stdio write to UART */
int _open_r(struct _reent *r, const char *pathname, int flags, int mode)
{
    return SPIFFS_open(&fs, pathname, flags, mode);
}

int _close_r(struct _reent *r, int fd)
{
    return SPIFFS_close(&fs, (spiffs_file)fd);
}

int _unlink_r(struct _reent *r, const char *path)
{
    return SPIFFS_remove(&fs, path);
}
