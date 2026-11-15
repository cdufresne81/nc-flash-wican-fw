/*
 * This file is part of the WiCAN project.
 *
 * Copyright (C) 2022  Meatpi Electronics.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* See ext_client_logic.h. No ESP-IDF includes: tools/hosttest builds this with cc. */

#include "ext_client_logic.h"

void ext_client_touch(ext_client_hold_t *h, uint32_t now_ms)
{
    if (h == NULL) { return; }
    h->last_ms = now_ms;   /* stamp first: a reader that sees `seen` must see a fresh stamp */
    h->seen = true;
}

bool ext_client_active(const ext_client_hold_t *h, uint32_t now_ms)
{
    if (h == NULL || !h->seen) { return false; }
    return (uint32_t)(now_ms - h->last_ms) < EXT_CLIENT_HOLD_MS;
}

uint32_t ext_client_idle_ms(const ext_client_hold_t *h, uint32_t now_ms)
{
    if (h == NULL || !h->seen) { return UINT32_MAX; }
    return (uint32_t)(now_ms - h->last_ms);
}

bool ext_client_expire(ext_client_hold_t *h, uint32_t now_ms)
{
    if (h == NULL || !h->seen) { return false; }
    if ((uint32_t)(now_ms - h->last_ms) < EXT_CLIENT_HOLD_MS) { return false; }
    h->seen = false;
    return true;
}

ext_client_verdict_t ext_client_gate(const ext_client_gate_in_t *in)
{
    if (in == NULL) { return EXT_CLIENT_ADMIT; }
    if (in->flash_active) { return EXT_CLIENT_REFUSE_FLASH; }
    if (in->host_claim || in->host_park) { return EXT_CLIENT_REFUSE_SESSION; }
    if (in->sleep_fence) { return EXT_CLIENT_REFUSE_SLEEP; }
    return EXT_CLIENT_ADMIT;
}

const char *ext_client_verdict_name(ext_client_verdict_t v)
{
    switch (v)
    {
        case EXT_CLIENT_ADMIT:          return "admit";
        case EXT_CLIENT_REFUSE_FLASH:   return "flash";
        case EXT_CLIENT_REFUSE_SESSION: return "session";
        case EXT_CLIENT_REFUSE_SLEEP:   return "sleep";
        default:                        return "unknown";
    }
}

size_t ext_client_count_commands(const uint8_t *buf, size_t len)
{
    size_t n = 0;
    if (buf == NULL) { return 0; }
    for (size_t i = 0; i < len; i++)
    {
        if (buf[i] == '\r') { n++; }
    }
    return n;
}

/* Refused command families, as the chip sees them: upper case, no spaces. Prefix match. */
static const char *const k_forbidden[] = {
    "ATPP",       /* programmable parameters: persist in EEPROM, obd.c owns 0E/0F */
    "ATLP",       /* low power */
    "ATBRD",      /* UART baud rate divisor */
    "ATBRT",      /* UART baud rate timeout */
    "ATMA",       /* monitor all */
    "ATMR",       /* monitor receiver */
    "ATMT",       /* monitor transmitter */
    "ATMP",       /* monitor PGN */
    "STM",        /* ST monitors */
    "STSBR",      /* set UART baud rate */
    "STBR",       /* switch UART baud rate */
    "STWBR",      /* write UART baud rate to NVM */
    "STPBR",      /* set protocol baud rate */
    "STSL",       /* sleep: STSLEEP, STSLCS, STSLU, STSLVL, STSLX... */
    "STRSTNVM",   /* reset NVM to factory defaults */
    "STSAVCAL",   /* save voltage calibration */
    "STSDI",      /* set device ID */
    "STSATI",     /* set ATI string */
};

bool ext_client_cmd_forbidden(const char *cmd, size_t len)
{
    char norm[12];   /* longest entry is 8 characters; anything beyond cannot change a prefix match */
    size_t n = 0;

    if (cmd == NULL) { return false; }
    for (size_t i = 0; i < len && cmd[i] != '\0' && n < sizeof(norm) - 1; i++)
    {
        char c = cmd[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { continue; }
        if (c >= 'a' && c <= 'z') { c = (char)(c - 'a' + 'A'); }
        norm[n++] = c;
    }
    norm[n] = '\0';

    for (size_t k = 0; k < sizeof(k_forbidden) / sizeof(k_forbidden[0]); k++)
    {
        const char *f = k_forbidden[k];
        size_t i = 0;
        while (f[i] != '\0' && norm[i] == f[i]) { i++; }
        if (f[i] == '\0') { return true; }
    }
    return false;
}

bool ext_client_rx_admit(bool flash_active, size_t queue_spaces)
{
    if (flash_active) { return false; }
    return queue_spaces > EXT_CLIENT_RXQ_RESERVE;
}

bool ext_client_sleep_veto(bool client_active, uint32_t veto_ms)
{
    return client_active && veto_ms < EXT_CLIENT_SLEEP_VETO_MAX_MS;
}
