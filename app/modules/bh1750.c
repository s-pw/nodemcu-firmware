/*
 * Driver for BH1750 ambient light sensor.
 *
 */
#include "module.h"
#include "lauxlib.h"
#include "platform.h"

#define BH1750_ADDRESS           0x23
/* BH1750 Commands */
#define BH1750_CONTINUOUS_HIGH_RES_MODE	0x10
#define RES_DIV                 (78642)

static int ICACHE_FLASH_ATTR bh1750_init(lua_State* L) {
    uint32_t sda;
    uint32_t scl;

    sda = luaL_checkinteger(L, 1);
    scl = luaL_checkinteger(L, 2);

    luaL_argcheck(L, sda > 0 && scl > 0, 1, "no i2c for D0");

    platform_i2c_setup(0, sda, scl, PLATFORM_I2C_SPEED_SLOW);

    platform_i2c_send_start(0);
    platform_i2c_send_address(0, BH1750_ADDRESS, PLATFORM_I2C_DIRECTION_TRANSMITTER);
    platform_i2c_send_byte(0, BH1750_CONTINUOUS_HIGH_RES_MODE);
    platform_i2c_send_stop(0);

    return 0;
}

static int ICACHE_FLASH_ATTR bh1750_read(lua_State* L) {
    uint32_t rawValue;
    platform_i2c_send_start(0);
    platform_i2c_send_address(0, BH1750_ADDRESS, PLATFORM_I2C_DIRECTION_RECEIVER);
    rawValue = (uint16_t) platform_i2c_recv_byte(0, 1) << 8;
    rawValue |= platform_i2c_recv_byte(0, 0);
    platform_i2c_send_stop(0);

    lua_pushinteger(L, rawValue * 1000 / 12);

    return 1;
}

static const LUA_REG_TYPE bh1750_map[] = {
    { LSTRKEY( "read" ),         LFUNCVAL( bh1750_read )},
    { LSTRKEY( "init" ),         LFUNCVAL( bh1750_init )},
    { LNILKEY, LNILVAL}
};

NODEMCU_MODULE(BH1750, "bh1750", bh1750_map, NULL);
