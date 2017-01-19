// Module for interacting with rBoot

#include "module.h"
#include "lua.h"
#include "lauxlib.h"

#include "user_config.h"
#include "rboot-api.h"
#include "user_interface.h"

#pragma mark - rBoot tools

static uint8_t curr_rom() {
  uint8_t rom;
#if defined(BOOT_RTC_ENABLED)
  if (rboot_get_last_boot_rom(&rom)) return rom;
#endif
  return rboot_get_config().current_rom;
}

#pragma mark - LUA API

// Lua: rom()
static int rboot_rom( lua_State* L ) {
	uint8_t rom = curr_rom();
	lua_pushinteger(L, rom);
	return 1;
}

// Lua: swap()
static int rboot_swap( lua_State* L ) {
  uint8_t rom = curr_rom();
	if (rom != 0) rom = 0;
	else rom = 1;
	rboot_set_current_rom(rom);
	system_restart();
	return 0;
}

#if defined(BOOT_RTC_ENABLED)
// Lua: swap_temp()
static int rboot_swap_temp( lua_State* L ) {
  uint8_t rom = curr_rom();
  if (rom != 0) rom = 0;
  else rom = 1;
  rboot_set_temp_rom(rom);
  system_restart();
  return 0;
}

// Lua: default_rom()
static int rboot_default_rom( lua_State* L ) {
  uint8_t rom = rboot_get_config().current_rom;
  lua_pushinteger(L, rom);
  return 1;
}

// Lua: save_default()
static int rboot_save_default( lua_State* L ) {
  uint8_t rom;
  if (rboot_get_last_boot_rom(&rom)) {
    rboot_set_current_rom(rom);
  }
  return 0;
}
#endif

#pragma mark -  Module function map

#include "lrodefs.h"

const LUA_REG_TYPE rboot_map[] = {
	{ LSTRKEY( "rom" ), LFUNCVAL( rboot_rom ) },
  { LSTRKEY( "swap" ), LFUNCVAL( rboot_swap ) },
#if defined(BOOT_RTC_ENABLED)
  { LSTRKEY( "swap_temp" ), LFUNCVAL( rboot_swap_temp ) },
  { LSTRKEY( "default_rom" ), LFUNCVAL( rboot_default_rom ) },
  { LSTRKEY( "save_default" ), LFUNCVAL( rboot_save_default ) },
#endif
	{ LNILKEY, LNILVAL }
};

NODEMCU_MODULE(RBOOT, "rboot", rboot_map, NULL);

