/*
 * Driver for HTU21D/SHT21 humidity/temperature sensor.
 *
 */
#include "module.h"
#include "lauxlib.h"
#include "platform.h"

#define HTU21_ADDRESS           0x40
/* HTU21 Commands */
#define HTU21_T_MEASUREMENT_HM	0xE3
#define HTU21_RH_MEASUREMENT_HM	0xE5

//Give this function the 2 byte message (measurement) and the check_value byte from the HTU21D
//If it returns 0, then the transmission was good
//If it returns something other than 0, then the communication was corrupted
//From: http://www.nongnu.org/avr-libc/user-manual/group__util__crc.html
//POLYNOMIAL = 0x0131 = x^8 + x^5 + x^4 + 1 : http://en.wikipedia.org/wiki/Computation_of_cyclic_redundancy_checks
#define SHIFTED_DIVISOR 0x988000 //This is the 0x0131 polynomial shifted to farthest left of three bytes

static inline int32_t htu21_temp_ticks_to_millicelsius(uint32_t ticks)
{
    ticks &= ~0x0003; /* clear status bits */
    /*
     * Formula T = -46.85 + 175.72 * ST / 2^16 from datasheet p14,
     * optimized for integer fixed point (3 digits) arithmetic
     */
    return ((21965 * ticks) >> 13) - 46850;
}

static inline int32_t htu21_rh_ticks_to_per_cent_mille(uint32_t ticks)
{
    ticks &= ~0x0003; /* clear status bits */
    /*
     * Formula RH = -6 + 125 * SRH / 2^16 from datasheet p14,
     * optimized for integer fixed point (3 digits) arithmetic
     */
    return ((15625 * ticks) >> 13) - 6000;
}

static uint8_t checkCRC(uint16_t ravValue, uint8_t checksum)
{
    uint32_t remainder = (uint32_t) ravValue << 8; //Pad with 8 bits because we have to add in the check value
    remainder |= checksum; //Add on the check value

    uint32_t divsor = (uint32_t) SHIFTED_DIVISOR;

    for (int i = 0 ; i < 16 ; i++) //Operate on only 16 positions of max 24. The remaining 8 are our remainder and should be zero when we're done.
    {
        if ( remainder & (uint32_t)1 << (23 - i) ) //Check if there is a one in the left position
            remainder ^= divsor;

        divsor >>= 1; //Rotate the divsor max 16 times so that we have 8 bits left of a remainder
    }

    return (uint8_t) remainder;
}

static uint16_t ICACHE_FLASH_ATTR r16u(lua_State* L, uint8_t reg) {
    uint16_t rawValue;
    uint8_t checksum;

    platform_i2c_send_start(0);
    platform_i2c_send_address(0, HTU21_ADDRESS, PLATFORM_I2C_DIRECTION_TRANSMITTER);
    platform_i2c_send_byte(0, reg);
    platform_i2c_send_start(0);
    platform_i2c_send_address(0, HTU21_ADDRESS, PLATFORM_I2C_DIRECTION_RECEIVER);
    rawValue = (uint16_t) platform_i2c_recv_byte(0, 1) << 8;
    rawValue |= platform_i2c_recv_byte(0, 1);
    checksum = (uint8_t) platform_i2c_recv_byte(0, 0);
    platform_i2c_send_stop(0);

    if (checkCRC(rawValue, checksum) != 0) luaL_error(L, "invalid CRC");

    return rawValue;
}

static int ICACHE_FLASH_ATTR htu21_init(lua_State* L) {
    uint32_t sda;
    uint32_t scl;

    sda = luaL_checkinteger(L, 1);
    scl = luaL_checkinteger(L, 2);

    luaL_argcheck(L, sda > 0 && scl > 0, 1, "no i2c for D0");

    platform_i2c_setup(0, sda, scl, PLATFORM_I2C_SPEED_SLOW);

    return 0;
}

static int ICACHE_FLASH_ATTR htu21_read(lua_State* L) {
    uint16_t rawT = r16u(L, HTU21_T_MEASUREMENT_HM);
    uint16_t rawRH = r16u(L, HTU21_RH_MEASUREMENT_HM);

    lua_pushinteger(L, htu21_temp_ticks_to_millicelsius(rawT));
    lua_pushinteger(L, htu21_rh_ticks_to_per_cent_mille(rawRH));

    return 2;
}

static const LUA_REG_TYPE htu21_map[] = {
    { LSTRKEY( "read" ),         LFUNCVAL( htu21_read )},
    { LSTRKEY( "init" ),         LFUNCVAL( htu21_init )},
    { LNILKEY, LNILVAL}
};

NODEMCU_MODULE(HTU21, "htu21", htu21_map, NULL);
