
#include <u8x8_nodemcu_hal.h>
#include "module.h"
#include "lauxlib.h"
#include "platform.h"
#include "c_stdlib.h"
#include "vfs.h"

#include "u8g2.h"
#include "u8x8_nodemcu_hal.h"

#include "z80emu.h"
#include "z80user.h"

u8g2_nodemcu_t ud;
CA80 ca80;

void loadFile(unsigned char *buf, char *filename, uint16_t offset) {
  int file_fd = vfs_open(filename, "rb");
  vfs_lseek(file_fd, offset, VFS_SEEK_SET);
  vfs_read(file_fd, buf, PAGE_SIZE);
  vfs_close(file_fd);
}

void load_rom(unsigned char *p, uint16_t addr) {
  if (addr < 0x4000) {
    loadFile(p, "CA80_new.rom", (uint16_t) (addr - (addr % PAGE_SIZE)));
  } else if (addr < 0x8000) {
    loadFile(p, "C800.rom", (uint16_t) (addr - 0x4000 - (addr % PAGE_SIZE)));
  } else if (addr < 0xC000) {
    loadFile(p, "C930.rom", (uint16_t) (addr - 0x8000 - (addr % PAGE_SIZE)));
  }
}

uint8_t ICACHE_RAM_ATTR ca80_read_byte(CA80 *ca80, uint16_t addr) {
  int page = addr / PAGE_SIZE;
  unsigned char *p = ca80->memory_pages[page];
  if (p == 0) {
    p = c_malloc(PAGE_SIZE);
    ca80->memory_pages[page] = p;
    if (addr < 0xC000) {
      load_rom(p, addr);
    }
  }
  return p[addr % PAGE_SIZE];
}

void ICACHE_RAM_ATTR ca80_write_byte(CA80 *ca80, uint16_t addr, uint8_t val) {
  if (addr < 0xC000 || addr >= 0x10000 ) return;
  int page = addr / PAGE_SIZE;
  unsigned char *p = ca80->memory_pages[page];
  if (p == 0) {
    p = c_malloc(PAGE_SIZE);
    ca80->memory_pages[page] = p;
  }
  p[addr % PAGE_SIZE] = val;
}

uint8_t ICACHE_RAM_ATTR ca80_in(CA80 *ca80, uint16_t port) {
  int cs = port & 0xFFFC;
  int addr = port & 0x3;
  if (cs == 0xF0) {   //PSYS 8255
    if (addr == 0) {   //PA
      uint8_t PA = 0;
      PA |= ca80->PC & 0x1 ? 0 : ca80->keypad[0];
      PA |= ca80->PC & 0x2 ? 0 : ca80->keypad[1];
      PA |= ca80->PC & 0x4 ? 0 : ca80->keypad[2];
      PA |= ca80->PC & 0x8 ? 0 : ca80->keypad[3];
      uint8_t ret = ~PA & (uint8_t) 0x7E;
      return ret;
    } else if (addr == 1) {   //PB
      return ca80->PB;
    } else if (addr == 2) {   //PC
      return ca80->PC;
    } else {
//      printf("PSYS 8255 IN\n");
//      printf("in port=%X, portaddr=%X, \n", cs, addr);
//      exit(1);
    }
  } else {
//    printf("UNKNOWN PORT IN\n");
//    printf("in port=%X, portaddr=%X, \n", cs, addr);
//    exit(1);
  }
}

void ICACHE_RAM_ATTR ca80_out(CA80 *ca80, uint16_t port, uint8_t val) {
  int cs = port & 0xFFFC;
  int addr = port & 0x3;
  if (cs == 0xF8) {
    //NO Z80A_CTC
  } else if (cs == 0xF0) {   //PSYS 8255
    if (addr == 0) {   //PA
      //NOP inputs only
    } else if (addr == 1) {   //PB
      ca80->PB = val;
    } else if (addr == 2) {   //PC
      ca80->PC = val;
    } else if (addr == 3) {   //PC BSR
      int bit = val >> 1;
      ca80->PC = val & 1 ?  ca80->PC | ((uint8_t) 1 << bit) : ca80->PC & ~((uint8_t) 1 << bit);
    }
  } else if (cs == 0xEC) {   //buzzer
//    putchar('\a');
    //c_printf("buzz\n");
  } else if (cs == 0xE0) {   //ext 8255
  } else {
//    printf("out port=%X, portaddr=%X, val=%X\n", cs, addr, val);
//    printf("UNKNOWN PORT OUT\n");
//    exit(1);
  }

}


static int ICACHE_RAM_ATTR draw_digit(int x, int y, const uint8_t data)
{
  u8g2_nodemcu_t *ext_u8g2 = &ud;
  u8g2_t *u8g2 = (u8g2_t *) ext_u8g2;
  if (data & 0x1)
  {
    u8g2_DrawHLine(u8g2, (u8g2_uint_t) (2 + x), (u8g2_uint_t) (0 + y), 6);
    u8g2_DrawHLine(u8g2, (u8g2_uint_t) (3 + x), (u8g2_uint_t) (1 + y), 4);
  }
  if (data & 0x2)
  {
    u8g2_DrawVLine(u8g2, (u8g2_uint_t) (9 + x), (u8g2_uint_t) (2 + y), 6);
    u8g2_DrawVLine(u8g2, (u8g2_uint_t) (8 + x), (u8g2_uint_t) (3 + y), 4);
  }
  if (data & 0x4)
  {
    u8g2_DrawVLine(u8g2, (u8g2_uint_t) (9 + x), (u8g2_uint_t) (10 + y), 6);
    u8g2_DrawVLine(u8g2, (u8g2_uint_t) (8 + x), (u8g2_uint_t) (11 + y), 4);
  }
  if (data & 0x8)
  {
    u8g2_DrawHLine(u8g2, (u8g2_uint_t) (3 + x), (u8g2_uint_t) (16 + y), 4);
    u8g2_DrawHLine(u8g2, (u8g2_uint_t) (2 + x), (u8g2_uint_t) (17 + y), 6);
  }
  if (data & 0x10)
  {
    u8g2_DrawVLine(u8g2, (u8g2_uint_t) (0 + x), (u8g2_uint_t) (10 + y), 6);
    u8g2_DrawVLine(u8g2, (u8g2_uint_t) (1 + x), (u8g2_uint_t) (11 + y), 4);
  }
  if (data & 0x20)
  {
    u8g2_DrawVLine(u8g2, (u8g2_uint_t) (0 + x), (u8g2_uint_t) (2 + y), 6);
    u8g2_DrawVLine(u8g2, (u8g2_uint_t) (1 + x), (u8g2_uint_t) (3 + y), 4);
  }
  if (data & 0x40)
  {
    u8g2_DrawBox(u8g2, (u8g2_uint_t) (3 + x), (u8g2_uint_t) (8 + y), 4, 2);
  }
  if (data & 0x80)
  {
    u8g2_DrawBox(u8g2, (u8g2_uint_t) (11 + x), (u8g2_uint_t) (16 + y), 2, 2);
  }
}

static int ICACHE_RAM_ATTR draw_digits(const uint8_t *data)
{
  u8g2_nodemcu_t *ext_u8g2 = &ud;
  u8g2_t *u8g2 = (u8g2_t *) ext_u8g2;
  for (int i = 0; i < 8; i++)
  {
    draw_digit(6 + i * 15, 23, data[i]);
  }
}

uint8_t u8x8_d_overlay(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr);

static os_timer_t lcd_timer;
static os_timer_t cpu_timer;

void lcd_draw(){
  u8g2_nodemcu_t *ext_u8g2 = &ud;
  u8g2_t *u8g2 = (u8g2_t *) ext_u8g2;
  u8g2_ClearBuffer(u8g2);
  draw_digits(ca80.display);
  u8g2_SendBuffer(u8g2);
}

void cpu(){

  //int start = 0x7FFFFFFF & system_get_time();
  Z80Emulate(&ca80.state, 1200, &ca80);
  ca80.display[7-(ca80.PC >> 5)] = ~ca80.PB;
  Z80NonMaskableInterrupt(&ca80.state, &ca80);
  //int end = 0x7FFFFFFF & system_get_time();
  //c_printf("h=%d t=%d\n", system_get_free_heap_size(), end-start);
}

static int ca80_start(lua_State *L)
{
  u8g2_nodemcu_t *ext_u8g2 = &ud;

  ext_u8g2->hal = (void *) 0;

  u8g2_t *u8g2 = (u8g2_t *) ext_u8g2;
  u8x8_t *u8x8 = (u8x8_t *) u8g2;

  u8g2_Setup_ssd1306_i2c_128x64_noname_f(u8g2, U8G2_R0, u8x8_byte_nodemcu_i2c,
                                         u8x8_gpio_and_delay_nodemcu);  // init u8g2 structure
  u8x8_SetI2CAddress(u8x8, 0x3c);
  ext_u8g2->overlay.hardware_display_cb = ext_u8g2->overlay.template_display_cb;
  u8g2_InitDisplay((u8g2_t *) u8g2); // send init sequence to the display, display is in sleep mode after this,
  u8g2_ClearDisplay(u8g2);
  u8g2_SetPowerSave((u8g2_t *) u8g2, 0); // wake up display
  u8g2_SetDrawColor(u8g2, 1);
  u8g2_SetFont(u8g2, u8g2_font_6x12_tf);

  os_timer_disarm(&lcd_timer);
  os_timer_setfn(&lcd_timer, (os_timer_func_t *)lcd_draw, NULL);
  os_timer_arm(&lcd_timer, 50, 1);

  os_timer_disarm(&cpu_timer);
  os_timer_setfn(&cpu_timer, (os_timer_func_t *)cpu, NULL);
  os_timer_arm(&cpu_timer, 1, 1);

  Z80Reset(&ca80.state);
  return 0;
}

static int ca80_key(lua_State *L)
{
  const char *c = luaL_checkstring(L, 1);
  switch(c[0]) {
    case ' ':
      ca80.keypad[0] = 0;
      ca80.keypad[1] = 0;
      ca80.keypad[2] = 0;
      ca80.keypad[3] = 0;
      break;
    case '!': ca80.keypad[0] = 0x10; break;
    case '@': ca80.keypad[1] = 0x10; break;
    case '#': ca80.keypad[2] = 0x10; break;
    case '$': ca80.keypad[3] = 0x10; break;
    case 'c': ca80.keypad[0] = 0x08; break;
    case '8': ca80.keypad[1] = 0x08; break;
    case '4': ca80.keypad[2] = 0x08; break;
    case '0': ca80.keypad[3] = 0x08; break;
    case 'd': ca80.keypad[0] = 0x20; break;
    case '9': ca80.keypad[1] = 0x20; break;
    case '5': ca80.keypad[2] = 0x20; break;
    case '1': ca80.keypad[3] = 0x20; break;
    case 'e': ca80.keypad[0] = 0x04; break;
    case 'a': ca80.keypad[1] = 0x04; break;
    case '6': ca80.keypad[2] = 0x04; break;
    case '2': ca80.keypad[3] = 0x04; break;
    case 'f': ca80.keypad[0] = 0x40; break;
    case 'b': ca80.keypad[1] = 0x40; break;
    case '7': ca80.keypad[2] = 0x40; break;
    case '3': ca80.keypad[3] = 0x40; break;
    case 'm': ca80.keypad[0] = 0x02; break;
    case 'g': ca80.keypad[1] = 0x02; break;
    case '.': ca80.keypad[2] = 0x02; break;
    case '=': ca80.keypad[3] = 0x02; break;
  }
}

static const LUA_REG_TYPE ca80_map[] = {
    {LSTRKEY("start"), LFUNCVAL(ca80_start)},
    {LSTRKEY("key"), LFUNCVAL(ca80_key)},
    {LNILKEY,          LNILVAL}
};

NODEMCU_MODULE(CA80, "ca80", ca80_map, NULL);
