// Module for interacting with rBoot

#include "module.h"
#include "lauxlib.h"
#include "platform.h"

#include "rboot-api.h"
#include "espconn.h"
#include "mem.h"
#include "stdlib.h"
#include "lwip/err.h"
#include <rboot.h>

#pragma mark - rBoot tools

static uint8_t curr_rom() {
    uint8_t rom;
#if defined(BOOT_RTC_ENABLED)
    if (rboot_get_last_boot_rom(&rom)) return rom;
#endif
    return rboot_get_config().current_rom;
}

#pragma mark - LUA API

#if defined(DEVELOP_VERSION)
#define RBOOT_DEBUG(format, ...) dbg_printf(format"\n", ##__VA_ARGS__)
#else
#define RBOOT_DEBUG(...)
#endif

#if defined(NODE_ERROR)
#define RBOOT_ERR(format, ...) NODE_ERR(format"\n", ##__VA_ARGS__)
#else
#define RBOOT_ERR(...)
#endif

typedef struct request_args_t {
    const char *hostname;
    u16 port;
    const char *path;
    char *buffer;
    u32 buffer_size;
    int32 remaining_content_length;
    uint16 current_sector;
    os_timer_t timeout_timer;
} request_args_t;

static void http_free_req(request_args_t *req) {
    if (req->buffer) {
        os_free(req->buffer);
    }
    os_free(req);
}

static void ICACHE_FLASH_ATTR http_disconnect_callback(void *arg) {
    RBOOT_DEBUG("Disconnected");
    struct espconn *conn = (struct espconn *) arg;

    if (conn == NULL) {
        return;
    }
}

static void ICACHE_FLASH_ATTR http_disconnect(void *arg) {
    RBOOT_ERR("Connection timeout");
    struct espconn *conn = (struct espconn *) arg;
    if (conn == NULL) {
        RBOOT_ERR("Connection is NULL");
        return;
    }
    if (conn->reverse == NULL) {
        RBOOT_ERR("Connection request data (reverse) is NULL");
        return;
    }
    request_args_t *req = (request_args_t *) conn->reverse;
    RBOOT_DEBUG("Calling disconnect");
    /* Call disconnect */
    sint8 result;
    result = espconn_disconnect(conn);

    if (result == ESPCONN_OK || result == ESPCONN_INPROGRESS)
        return;

    /* not connected; execute the callback ourselves. */
    RBOOT_DEBUG("manually Calling disconnect callback due to error %d", result);
    http_disconnect_callback(arg);

}

static char *get_header(const char *buff, const char *header) {
    char *contentLenOff = os_strstr(buff, header);
    contentLenOff += strlen(header);

    while (*contentLenOff == ' ') {
        contentLenOff++;
    }
    char *contentLenOffEnd = os_strstr(contentLenOff, "\r\n");
    *contentLenOffEnd = '\0';
    return contentLenOff;
}

static int rboot_swap(lua_State *L);

static void ICACHE_FLASH_ATTR http_receive_callback(void *arg, char *buf, unsigned short len) {
    struct espconn *conn = (struct espconn *) arg;
    request_args_t *req = (request_args_t *) conn->reverse;

    if (req->buffer == NULL) {
        return;
    }

    uint32 new_size = req->buffer_size + len;
    char *new_buffer;
    new_buffer = os_malloc(new_size);   //create new buffer to append received data

    if (req->remaining_content_length == -1) {
        os_memcpy(new_buffer, req->buffer, req->buffer_size);       //append data to buffer and terminate as string
        os_memcpy(new_buffer + req->buffer_size - 1, buf, len);
        new_buffer[new_size - 1] = '\0';

        os_free(req->buffer);
        req->buffer = new_buffer;
        req->buffer_size = new_size;

        char *headersEnd = os_strstr(req->buffer, "\r\n\r\n");      //find if buffer has end of HTTP headers

        if (headersEnd != NULL) {
            *headersEnd = '\0';
            RBOOT_DEBUG("Response headers: %s", req->buffer);
            headersEnd += 4;        //skip \r\n\r\n

            char *content_len = get_header(req->buffer, "Content-Length:");
            req->remaining_content_length = atoi(content_len);

            RBOOT_DEBUG("Content-Length: %d", req->remaining_content_length);

            os_timer_disarm(&(req->timeout_timer));     //increase timeout to 120s as flash operations are slow
            os_timer_setfn(&(req->timeout_timer), (os_timer_func_t *) http_disconnect, conn);
            os_timer_arm(&(req->timeout_timer), 120000, false);

            int sectors = req->remaining_content_length / SPI_FLASH_SEC_SIZE + 1;
            RBOOT_DEBUG("Erasing %d sectors", sectors);
            for (uint16 i = 0; i < sectors; i++) {
                flash_erase(req->current_sector + i);
            }

            new_size = (uint32) (req->buffer_size - (headersEnd - req->buffer) - 1);
            if (new_size > 0) { //copy remaining GET data bytes received after HTTP headers
                new_buffer = os_malloc(new_size);
                os_memcpy(new_buffer, headersEnd, new_size);

                os_free(req->buffer);
                req->buffer = new_buffer;
                req->buffer_size = new_size;
            } else {
                req->buffer = 0;
                req->buffer_size = 0;
            }

            return;
        }
    } else {
        if (req->buffer) {
            os_memcpy(new_buffer, req->buffer, req->buffer_size);
        }
        os_memcpy(new_buffer + req->buffer_size, buf, len);
        if (req->buffer) {
            os_free(req->buffer);
        }
        req->buffer = new_buffer;
        req->buffer_size = new_size;

        if (new_size == req->remaining_content_length && new_size != 0 && new_size < SPI_FLASH_SEC_SIZE) {  //last packet resize to flash sector size for writing
            new_size = SPI_FLASH_SEC_SIZE;
            new_buffer = os_malloc(SPI_FLASH_SEC_SIZE);
            os_memset(new_buffer, 0xFF, SPI_FLASH_SEC_SIZE);
            os_memcpy(new_buffer, req->buffer + (req->buffer_size - req->remaining_content_length),
                      (uint32) req->remaining_content_length);
            os_free(req->buffer);
            req->buffer = new_buffer;
            req->buffer_size = SPI_FLASH_SEC_SIZE;
        }

        if (new_size >= SPI_FLASH_SEC_SIZE) {
            while (new_size >= SPI_FLASH_SEC_SIZE) {    //consume full sectors from buffer and write to flash
                //flash
                flash_write((uint32) req->current_sector * SPI_FLASH_SEC_SIZE,
                            (uint32 *) req->buffer + req->buffer_size - new_size, SPI_FLASH_SEC_SIZE);

                req->current_sector++;

                new_size -= SPI_FLASH_SEC_SIZE;
                req->remaining_content_length -= SPI_FLASH_SEC_SIZE;
            }
            if (new_size > 0) {         //remove consumed sectors from buffer
                new_buffer = os_malloc(new_size);
                os_memcpy(new_buffer, req->buffer + (req->buffer_size - new_size), new_size);
                os_free(req->buffer);
                req->buffer = new_buffer;
                req->buffer_size = new_size;
            } else {
                os_free(req->buffer);
                req->buffer = 0;
                req->buffer_size = 0;
            }
        }
        if (req->remaining_content_length <= 0) {
            c_printf("OTA completed restarting");
            http_disconnect(arg);
            os_delay_us(10);
            rboot_swap(NULL);
        }
    }
}


static void ICACHE_FLASH_ATTR http_connect_callback(void *arg) {
    RBOOT_DEBUG("OTA http connected");
    struct espconn *conn = (struct espconn *) arg;
    request_args_t *req = (request_args_t *) conn->reverse;
    espconn_regist_recvcb(conn, http_receive_callback);

    req->buffer_size = 1;
    req->buffer = (char *) os_malloc(1);
    req->buffer[0] = '\0';
    req->remaining_content_length = -1;

    char host_header[100] = "";
    int host_len = 0;
    if (req->port == 80) {
        os_sprintf(host_header, "Host: %s\r\n", req->hostname);
    } else {
        os_sprintf(host_header, "Host: %s:%d\r\n", req->hostname, req->port);
    }
    host_len = strlen(host_header);

    char buf[36 + strlen(req->path) + host_len];
    int len = os_sprintf(buf,
                         "GET %s HTTP/1.1\r\n"
                                 "%s"
                                 "Connection: close\r\n"
                                 "\r\n",
                         req->path, host_header);

    espconn_sent(conn, (uint8_t *) buf, (uint16) len);
    RBOOT_DEBUG("Sending request header %s", buf);
}

static void ICACHE_FLASH_ATTR http_error_callback(void *arg, sint8 errType) {
    RBOOT_ERR("Disconnected with error: %d", errType);
    http_disconnect(arg);
}

static void ICACHE_FLASH_ATTR http_dns_callback(const char *hostname, ip_addr_t *addr, void *arg) {
    request_args_t *req = (request_args_t *) arg;

    if (addr == NULL) {
        RBOOT_DEBUG("DNS failed for %s", hostname);
        http_free_req(req);
    } else {
        RBOOT_DEBUG("connecting to %s "IPSTR, hostname, IP2STR(addr));

        struct espconn *conn = (struct espconn *) os_zalloc(sizeof(struct espconn));
        conn->type = ESPCONN_TCP;
        conn->state = ESPCONN_NONE;
        conn->proto.tcp = (esp_tcp *) os_zalloc(sizeof(esp_tcp));
        conn->proto.tcp->local_port = espconn_port();
        conn->proto.tcp->remote_port = req->port;
        conn->reverse = req;

        os_memcpy(conn->proto.tcp->remote_ip, addr, 4);

        espconn_regist_connectcb(conn, http_connect_callback);
        espconn_regist_disconcb(conn, http_disconnect_callback);
        espconn_regist_reconcb(conn, http_error_callback);

        os_timer_disarm(&(req->timeout_timer));
        os_timer_setfn(&(req->timeout_timer), (os_timer_func_t *) http_disconnect, conn);
        os_timer_arm(&(req->timeout_timer), 10000, false);

        espconn_connect(conn);
    }
}

// Lua: rom()
static int rboot_rom(lua_State *L) {
    uint8_t rom = curr_rom();
    lua_pushinteger(L, rom);
    return 1;
}

// Lua: swap()
static int rboot_swap(lua_State *L) {
    uint8_t rom = curr_rom();
    if (rom != 0) rom = 0;
    else rom = 1;
    rboot_set_current_rom(rom);
    system_restart();
    return 0;
}

// Lua: ota()
static int rboot_ota(lua_State *L) {
    const char *host = luaL_checkstring(L, 1);
    uint16 port = (uint16) luaL_checkinteger(L, 2);
    const char *path = luaL_checkstring(L, 3);
    c_printf("OTA Start");

    int rom = curr_rom() != 0 ? 0 : 1;
    rboot_config config = rboot_get_config();


    uint32 ip32 = ipaddr_addr(host);
    request_args_t *req = (request_args_t *) os_zalloc(sizeof(request_args_t));
    req->hostname = host;
    req->port = port;
    req->path = path;
    req->current_sector = (uint16) (config.roms[rom] / SPI_FLASH_SEC_SIZE);

    if (ip32 == IPADDR_NONE) {
        ip_addr_t addr;
        RBOOT_DEBUG("DNS query");
        err_t error = espconn_gethostbyname((struct espconn *) req, host, &addr, http_dns_callback);
    } else {
        RBOOT_DEBUG("IP address, skip DNS");
        http_dns_callback(host, (ip_addr_t *) &ip32, req);
    }

    return 0;
}

// Lua: ota()
static int rboot_info(lua_State *L) {
    rboot_config config =  rboot_get_config();

    lua_newtable(L);
    lua_pushinteger(L, config.mode);
    lua_setfield(L, -2, "mode");
    lua_pushinteger(L, config.version);
    lua_setfield(L, -2, "version");
    lua_pushinteger(L, config.current_rom);
    lua_setfield(L, -2, "current_rom");
    lua_pushinteger(L, config.gpio_rom);
    lua_setfield(L, -2, "gpio_rom");
    lua_pushinteger(L, config.count);
    lua_setfield(L, -2, "count");
    lua_newtable(L);
    for (int i = 0; i < config.count; ++i) {
        lua_pushinteger(L, i + 1);
        lua_pushinteger(L, config.roms[i]);
        lua_settable(L, -3);
    }
    lua_setfield(L, -2, "roms");
    return 1;
}

#if defined(BOOT_RTC_ENABLED)

// Lua: swap_temp()
static int rboot_swap_temp(lua_State *L) {
    uint8_t rom = curr_rom();
    if (rom != 0) rom = 0;
    else rom = 1;
    rboot_set_temp_rom(rom);
    system_restart();
    return 0;
}

// Lua: default_rom()
static int rboot_default_rom(lua_State *L) {
    uint8_t rom = rboot_get_config().current_rom;
    lua_pushinteger(L, rom);
    return 1;
}

// Lua: save_default()
static int rboot_save_default(lua_State *L) {
    uint8_t rom;
    if (rboot_get_last_boot_rom(&rom)) {
        rboot_set_current_rom(rom);
    }
    return 0;
}

#endif

#pragma mark -  Module function map


const LUA_REG_TYPE rboot_map[] = {
        {LSTRKEY("rom"), LFUNCVAL(rboot_rom)},
        {LSTRKEY("swap"), LFUNCVAL(rboot_swap)},
        {LSTRKEY("ota"), LFUNCVAL(rboot_ota)},
        {LSTRKEY("info"), LFUNCVAL(rboot_info)},
#if defined(BOOT_RTC_ENABLED)
        {LSTRKEY("swap_temp"), LFUNCVAL(rboot_swap_temp)},
        {LSTRKEY("default_rom"), LFUNCVAL(rboot_default_rom)},
        {LSTRKEY("save_default"), LFUNCVAL(rboot_save_default)},
#endif
        {LNILKEY, LNILVAL}
};

NODEMCU_MODULE(RBOOT, "rboot", rboot_map, NULL);

