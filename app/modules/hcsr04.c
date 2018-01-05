/*
 * Driver for HCSR04D ultrasonic sensor.
 *
 */
#include "module.h"
#include "lauxlib.h"
#include "platform.h"

static uint32_t trig;
static uint32_t echo;

static int ICACHE_FLASH_ATTR hcsr04_init(lua_State* L) {
    trig = luaL_checkinteger(L, 1);
    echo = luaL_checkinteger(L, 2);

    platform_gpio_mode(trig, PLATFORM_GPIO_OUTPUT, PLATFORM_GPIO_FLOAT);
    platform_gpio_mode(echo, PLATFORM_GPIO_INPUT, PLATFORM_GPIO_FLOAT);
    platform_gpio_write(trig, 0);

    return 0;
}

static uint32_t read_sensor() {
    platform_gpio_write(trig, 0);
    os_delay_us(2);
    platform_gpio_write(trig, 1);
    os_delay_us(10);
    platform_gpio_write(trig, 0);

    uint32_t now = system_get_time();
    while(platform_gpio_read(echo) == 1 && system_get_time() - now < 25000);
    while(platform_gpio_read(echo) == 0 && system_get_time() - now < 25000);
    now = system_get_time();
    while(platform_gpio_read(echo) == 1 && system_get_time() - now < 25000);
    uint32_t raw_time = system_get_time() - now;
    return raw_time;
}

static int ICACHE_FLASH_ATTR hcsr04_read(lua_State* L) {
    lua_pushinteger(L, read_sensor() * 1000 / 582);

    return 1;
}

static int ICACHE_FLASH_ATTR hcsr04_read_raw(lua_State* L) {
    lua_pushinteger(L, read_sensor());

    return 1;
}

static const LUA_REG_TYPE hcsr04_map[] = {
    { LSTRKEY( "read" ),         LFUNCVAL( hcsr04_read )},
    { LSTRKEY( "readRaw" ),      LFUNCVAL( hcsr04_read_raw )},
    { LSTRKEY( "init" ),         LFUNCVAL( hcsr04_init )},
    { LNILKEY, LNILVAL}
};

NODEMCU_MODULE(HCSR04, "hcsr04", hcsr04_map, NULL);
