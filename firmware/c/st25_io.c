#include "st25_io.h"
#include "config.h"
#define LOG_MODULE LOG_MOD_ST25
#include "debug.h"
#include "i2c_bus.h"
#include "st25dv.h"

#include "hardware/i2c.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"

#include <stdio.h>
#include <string.h>

/* ---------- pin / timing configuration ----------------------------------- */

#ifndef ST25_VCC_EN_PIN
#define ST25_VCC_EN_PIN 18
#endif

#ifndef ST25_POWER_ON_DELAY_MS
#define ST25_POWER_ON_DELAY_MS 10
#endif

#ifndef ST25_I2C_OP_TIMEOUT_US
#define ST25_I2C_OP_TIMEOUT_US 20000
#endif

/* ---------- module state -------------------------------------------------- */

static ST25DV_Object_t st25_obj;
static bool st25_initialized = false;
static uint16_t st25_request_addr = 0; /* computed from memory size */
static uint16_t st25_request_len = 16;

/* ---------- low-level I2C bus adapter (matches ST25DV_IO_t) --------------- */

static uint8_t addr7(uint16_t st_addr8) { return (uint8_t)(st_addr8 >> 1); }

static int32_t bus_init(void) {
    /* I2C already initialized by init_clock(); nothing to do */
    return 0;
}

static int32_t bus_deinit(void) { return 0; }

static uint32_t bus_get_tick(void) { return (uint32_t)to_ms_since_boot(get_absolute_time()); }

static int32_t bus_is_ready(uint16_t dev_addr, const uint32_t trials) {
    const uint8_t a7 = addr7(dev_addr);
    const uint8_t probe[2] = {0x00, 0x00};
    for (uint32_t i = 0; i < trials; i++) {
        const int res = i2c_write_timeout_us(i2c_default, a7, probe, sizeof(probe), false,
                                             ST25_I2C_OP_TIMEOUT_US);
        if (res == (int)sizeof(probe)) {
            return NFCTAG_OK;
        }
        sleep_ms(2);
    }
    return NFCTAG_NACK;
}

static int32_t bus_write(uint16_t dev_addr, uint16_t reg, const uint8_t *data, uint16_t len) {
    const uint8_t a7 = addr7(dev_addr);
    uint8_t tmp[2 + ST25DV_MAX_WRITE_BYTE];
    if (len > ST25DV_MAX_WRITE_BYTE) {
        return NFCTAG_ERROR;
    }
    tmp[0] = (uint8_t)((reg >> 8) & 0xFF);
    tmp[1] = (uint8_t)(reg & 0xFF);
    if (len > 0) {
        memcpy(&tmp[2], data, len);
    }
    const int res = i2c_write_timeout_us(i2c_default, a7, tmp, (size_t)(len + 2), false,
                                         ST25_I2C_OP_TIMEOUT_US);
    return (res < 0) ? NFCTAG_NACK : NFCTAG_OK;
}

static int32_t bus_read(uint16_t dev_addr, uint16_t reg, uint8_t *data, uint16_t len) {
    const uint8_t a7 = addr7(dev_addr);
    const uint8_t addr_buf[2] = {
        (uint8_t)((reg >> 8) & 0xFF),
        (uint8_t)(reg & 0xFF),
    };
    int res = i2c_write_timeout_us(i2c_default, a7, addr_buf, sizeof(addr_buf), true,
                                   ST25_I2C_OP_TIMEOUT_US);
    if (res < 0) {
        return NFCTAG_NACK;
    }
    res = i2c_read_timeout_us(i2c_default, a7, data, len, false, ST25_I2C_OP_TIMEOUT_US);
    return (res < 0) ? NFCTAG_NACK : NFCTAG_OK;
}

/* ---------- power control ------------------------------------------------- */

static void st25_power_on(void) {
#if (ST25_VCC_EN_PIN >= 0)
    gpio_init(ST25_VCC_EN_PIN);
    gpio_set_dir(ST25_VCC_EN_PIN, GPIO_OUT);
    gpio_put(ST25_VCC_EN_PIN, 1);
#endif
}

void st25_power_off(void) {
#if (ST25_VCC_EN_PIN >= 0)
    gpio_put(ST25_VCC_EN_PIN, 0);
#endif
}

/* ---------- retry helpers for EEPROM writes ------------------------------- */

static uint32_t get_tick(void) { return (uint32_t)to_ms_since_boot(get_absolute_time()); }

static int32_t write_itpulse_retry(ST25DV_Object_t *st, ST25DV_PULSE_DURATION pulse,
                                   uint32_t timeout_ms) {
    const uint32_t t0 = get_tick();
    int32_t ret;
    do {
        ret = ST25DV_WriteITPulse(st, pulse);
        if (ret == NFCTAG_OK)
            return ret;
        sleep_ms(2);
    } while ((get_tick() - t0) < timeout_ms);
    return ret;
}

static int32_t config_it_retry(ST25DV_Object_t *st, uint16_t conf, uint32_t timeout_ms) {
    const uint32_t t0 = get_tick();
    int32_t ret;
    do {
        ret = St25Dv_Drv.ConfigIT(st, conf);
        if (ret == NFCTAG_OK)
            return ret;
        sleep_ms(2);
    } while ((get_tick() - t0) < timeout_ms);
    return ret;
}

/* ---------- GPO wake configuration ---------------------------------------- */

static bool configure_wake_gpo(ST25DV_Object_t *st) {
    const uint32_t timeout = (uint32_t)ST25DV_WRITE_TIMEOUT + 50u;
    bool ok = true;

    if (write_itpulse_retry(st, ST25DV_302_US, timeout) == NFCTAG_OK) {
        dlog("[ST25] GPO pulse: 302 us\n");
    } else {
        dlog("[ST25] GPO pulse config failed\n");
        ok = false;
    }

    const uint16_t wake_conf = (uint16_t)(ST25DV_GPO_ENABLE_MASK | ST25DV_GPO_RFWRITE_MASK);
    if (config_it_retry(st, wake_conf, timeout) == NFCTAG_OK) {
        dlog("[ST25] GPO sources: ENABLE + RF_WRITE\n");
    } else {
        dlog("[ST25] GPO source config failed\n");
        ok = false;
    }

    return ok;
}

/* ---------- request decoding ---------------------------------------------- */

static bool is_inki_magic(const uint8_t buf[16]) {
    return buf[0] == 'I' && buf[1] == 'N' && buf[2] == 'K' && buf[3] == 'I';
}

static void decode_request(const uint8_t buf[16], st25_inki_request_t *req) {
    memcpy(req->magic, buf, 4);
    req->version = buf[4];
    req->opcode = buf[5];
    req->duration_min = (uint16_t)buf[6] | ((uint16_t)buf[7] << 8);
    req->unix_seconds = (uint32_t)buf[8] | ((uint32_t)buf[9] << 8) | ((uint32_t)buf[10] << 16) |
                        ((uint32_t)buf[11] << 24);
    req->nonce = (uint32_t)buf[12] | ((uint32_t)buf[13] << 8) | ((uint32_t)buf[14] << 16) |
                 ((uint32_t)buf[15] << 24);
}

static bool validate_request(const st25_inki_request_t *req) {
    if (req->version != 1u)
        return false;
    if (req->opcode == 0u)
        return false;
    /* bytes 6/7 are width/height for DRAW_IMAGE, not duration — skip range check */
    if (req->opcode != ST25_OPCODE_DRAW_IMAGE) {
        if (req->duration_min == 0u || req->duration_min > 1440u)
            return false;
    }
    if (req->unix_seconds == 0u)
        return false;
    if (req->nonce == 0u)
        return false;
    return true;
}

/* ---------- public API ---------------------------------------------------- */

st25_boot_result_t st25_boot_check(void) {
    st25_boot_result_t result = {0};

    /* Power on ST25 */
    st25_power_on();
    sleep_ms(ST25_POWER_ON_DELAY_MS);

    /* Register bus I/O */
    ST25DV_IO_t io = {
        .Init = bus_init,
        .DeInit = bus_deinit,
        .IsReady = bus_is_ready,
        .Write = bus_write,
        .Read = bus_read,
        .GetTick = bus_get_tick,
    };
    if (ST25DV_RegisterBusIO(&st25_obj, &io) != NFCTAG_OK) {
        dlog("[ST25] RegisterBusIO failed\n");
        st25_power_off();
        return result;
    }

    /* Init driver */
    if (St25Dv_Drv.Init(&st25_obj) != NFCTAG_OK) {
        dlog("[ST25] driver init failed\n");
        st25_power_off();
        return result;
    }

    /* Read ICREF */
    uint8_t icref = 0;
    if (St25Dv_Drv.ReadID(&st25_obj, &icref) == NFCTAG_OK) {
        dlog("[ST25] ICREF=0x%02X\n", icref);
    } else {
        dlog("[ST25] ReadID failed\n");
        st25_power_off();
        return result;
    }

    result.present = true;
    st25_initialized = true;

    /* Read memory size to compute request slot address */
    ST25DV_MEM_SIZE mem = {0};
    if (ST25DV_ReadMemSize(&st25_obj, &mem) == NFCTAG_OK) {
        const uint32_t blocks = (uint32_t)mem.Mem_Size + 1u;
        const uint32_t bpb = (uint32_t)mem.BlockSize + 1u;
        const uint32_t total = blocks * bpb;
        dlog("[ST25] memory: %lu bytes\n", (unsigned long)total);

        /* Request lives in last 16 bytes, aligned to block boundary */
        if (total >= 16u) {
            uint16_t raw_start = (uint16_t)(total - 16u);
            st25_request_addr = raw_start - (raw_start % (uint16_t)bpb);
            st25_request_len = (uint16_t)(total - st25_request_addr);
            if (st25_request_len > 64u) {
                st25_request_len = 64u;
            }
            dlog("[ST25] request slot: addr=0x%04X len=%u\n", st25_request_addr, st25_request_len);
        }
    }

    /* Read IT_STS_Dyn — latched interrupt flags showing recent RF activity */
    uint8_t it_sts = 0;
    if (ST25DV_ReadITSTStatus_Dyn(&st25_obj, &it_sts) == NFCTAG_OK) {
        result.it_sts = it_sts;
        dlog("[ST25] IT_STS_Dyn: 0x%02X\n", it_sts);
    }

    /* Check RF field state */
    ST25DV_EH_CTRL eh = {0};
    if (ST25DV_ReadEHCtrl_Dyn(&st25_obj, &eh) == NFCTAG_OK) {
        result.rf_field_on = (eh.Field_on == ST25DV_ENABLE);
        dlog("[ST25] RF field: %s\n", result.rf_field_on ? "ON" : "OFF");
    }

    /* Read last-processed nonce for stale request deduplication */
    uint32_t prev_nonce = 0;
    {
        uint8_t nb[4] = {0};
        if (St25Dv_Drv.ReadData(&st25_obj, nb, ST25_NONCE_ADDR, 4) == NFCTAG_OK) {
            prev_nonce = (uint32_t)nb[0] | ((uint32_t)nb[1] << 8) | ((uint32_t)nb[2] << 16) |
                         ((uint32_t)nb[3] << 24);
        }
        dlog("[ST25] prev_nonce: %lu\n", (unsigned long)prev_nonce);
    }

    /* If no RF field and no RF activity flags, there's no phone — skip the
     * wait and do a single read to check for a pre-written request. */
    const bool phone_present = result.rf_field_on || (it_sts != 0);
    if (phone_present) {
        /* Wait for the phone to finish writing before we start I2C reads.
         * The GPO fires on the FIRST block write, but the full 16-byte payload
         * needs 4 block writes (~200-300 ms total). */
        sleep_ms(300);
    }

    /* Read request from EEPROM with retries if phone is present. */
    const int max_attempts = phone_present ? 50 : 1; /* 300ms + 49×200ms ≈ 10s window */
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        if (attempt > 0)
            sleep_ms(200);
        watchdog_update();

        uint8_t req_buf[64] = {0};
        uint16_t read_len = st25_request_len < 64u ? st25_request_len : 64u;
        if (St25Dv_Drv.ReadData(&st25_obj, req_buf, st25_request_addr, read_len) != NFCTAG_OK) {
            dlog("[ST25] request read failed (attempt %d)\n", attempt + 1);
            continue; /* transient NACK during active RF write — keep retrying */
        }

        const uint8_t *req16 = &req_buf[read_len >= 16u ? read_len - 16u : 0u];
        dlog("[ST25] raw @0x%04X: %02X%02X%02X%02X %02X%02X%02X%02X "
             "%02X%02X%02X%02X %02X%02X%02X%02X (attempt %d)\n",
             st25_request_addr, req16[0], req16[1], req16[2], req16[3], req16[4], req16[5],
             req16[6], req16[7], req16[8], req16[9], req16[10], req16[11], req16[12], req16[13],
             req16[14], req16[15], attempt + 1);
        if (is_inki_magic(req16)) {
            decode_request(req16, &result.req);
            result.request_valid = validate_request(&result.req);
            dlog("[ST25] request: opcode=0x%02X dur=%u unix=%lu nonce=%lu valid=%d "
                 "(attempt %d)\n",
                 result.req.opcode, result.req.duration_min, (unsigned long)result.req.unix_seconds,
                 (unsigned long)result.req.nonce, result.request_valid, attempt + 1);

            if (result.request_valid) {
                if (result.req.nonce == prev_nonce) {
                    dlog("[ST25] stale request (nonce matches previous), retrying...\n");
                    result.request_valid = false;
                    continue;
                }
                break;
            }
        } else {
            dlog("[ST25] no INKI request in EEPROM (attempt %d)\n", attempt + 1);
        }

        /* Phone left mid-write — no point spinning the full 10 s */
        ST25DV_EH_CTRL eh_check = {0};
        if (ST25DV_ReadEHCtrl_Dyn(&st25_obj, &eh_check) == NFCTAG_OK &&
            eh_check.Field_on != ST25DV_ENABLE) {
            dlog("[ST25] RF field gone after attempt %d, exiting retry loop\n", attempt + 1);
            break;
        }
    }

    /* Configure GPO for wake on RF write — done AFTER the request read loop
     * to avoid I2C EEPROM writes that block RF access while the phone is
     * still writing. GPO config is non-volatile, so this is just idempotent
     * insurance that the setting persists across power cycles. */
    configure_wake_gpo(&st25_obj);

    return result;
}

int st25_read_text(char *buf, size_t buf_size) {
    if (!st25_initialized || buf_size == 0) {
        if (buf_size > 0)
            buf[0] = '\0';
        return 0;
    }

    uint16_t read_len = ST25_TEXT_MAX_LEN;
    if (read_len >= (uint16_t)buf_size)
        read_len = (uint16_t)(buf_size - 1);

    if (St25Dv_Drv.ReadData(&st25_obj, (uint8_t *)buf, ST25_TEXT_ADDR, read_len) != NFCTAG_OK) {
        dlog("[ST25] text read failed\n");
        buf[0] = '\0';
        return 0;
    }

    /* Find null terminator or end of read */
    int len = 0;
    for (uint16_t i = 0; i < read_len; i++) {
        if (buf[i] == '\0')
            break;
        len++;
    }
    buf[len] = '\0';

    dlog("[ST25] text read: %d bytes\n", len);
    return len;
}

int st25_read_image(uint8_t *buf, size_t buf_size) {
    if (!st25_initialized || buf_size == 0)
        return 0;

    uint16_t read_len = ST25_IMAGE_BYTES;
    if (read_len > (uint16_t)buf_size)
        read_len = (uint16_t)buf_size;

    if (St25Dv_Drv.ReadData(&st25_obj, buf, ST25_IMAGE_ADDR, read_len) != NFCTAG_OK) {
        dlog("[ST25] image read failed\n");
        return 0;
    }

    dlog("[ST25] image read: %u bytes\n", (unsigned)read_len);
    return (int)read_len;
}

bool st25_save_processed_nonce(uint32_t nonce) {
    if (!st25_initialized) {
        return false;
    }

    uint8_t nb[4] = {
        (uint8_t)(nonce & 0xFF),
        (uint8_t)((nonce >> 8) & 0xFF),
        (uint8_t)((nonce >> 16) & 0xFF),
        (uint8_t)((nonce >> 24) & 0xFF),
    };

    const uint32_t timeout = (uint32_t)ST25DV_WRITE_TIMEOUT + 50u;
    const uint32_t t0 = get_tick();
    int32_t ret;
    do {
        ret = St25Dv_Drv.WriteData(&st25_obj, nb, ST25_NONCE_ADDR, 4);
        if (ret == NFCTAG_OK)
            break;
        sleep_ms(2);
    } while ((get_tick() - t0) < timeout);

    if (ret != NFCTAG_OK) {
        dlog("[ST25] save nonce failed\n");
        return false;
    }

    sleep_ms(6);
    dlog("[ST25] nonce saved: %lu\n", (unsigned long)nonce);
    return true;
}

bool st25_clear_request(void) {
    if (!st25_initialized) {
        return false;
    }

    uint8_t zeros[16] = {0};
    /* Write zeros to the last 16 bytes (request slot) */
    uint16_t clear_addr = st25_request_addr;
    if (st25_request_len > 16u) {
        clear_addr = (uint16_t)(st25_request_addr + st25_request_len - 16u);
    }

    /* ST25DV EEPROM writes are limited to ST25DV_MAX_WRITE_BYTE per call.
     * Write 4 bytes at a time (one block) for reliability. */
    for (uint16_t i = 0; i < 16u; i += 4u) {
        const uint32_t timeout = (uint32_t)ST25DV_WRITE_TIMEOUT + 50u;
        const uint32_t t0 = get_tick();
        int32_t ret;
        do {
            ret = St25Dv_Drv.WriteData(&st25_obj, &zeros[i], (uint16_t)(clear_addr + i), 4);
            if (ret == NFCTAG_OK)
                break;
            sleep_ms(2);
        } while ((get_tick() - t0) < timeout);

        if (ret != NFCTAG_OK) {
            dlog("[ST25] clear request failed at offset %u\n", i);
            return false;
        }
    }

    /* Wait for last EEPROM write cycle to complete before caller cuts VCC */
    sleep_ms(6);

    dlog("[ST25] request cleared\n");
    return true;
}

bool st25_read_bytes(uint16_t addr, uint8_t *buf, uint16_t len) {
    if (!st25_initialized)
        return false;
    return St25Dv_Drv.ReadData(&st25_obj, buf, addr, len) == NFCTAG_OK;
}

bool st25_write_bytes(uint16_t addr, const uint8_t *buf, uint16_t len) {
    if (!st25_initialized)
        return false;

    const uint32_t timeout = (uint32_t)ST25DV_WRITE_TIMEOUT + 50u;
    const uint32_t t0 = get_tick();
    int32_t ret;
    do {
        ret = St25Dv_Drv.WriteData(&st25_obj, buf, addr, len);
        if (ret == NFCTAG_OK)
            break;
        sleep_ms(2);
    } while ((get_tick() - t0) < timeout);

    if (ret != NFCTAG_OK) {
        dlog("[ST25] write of %u bytes at 0x%04X failed\n", (unsigned)len, addr);
        return false;
    }
    sleep_ms(6);
    return true;
}
