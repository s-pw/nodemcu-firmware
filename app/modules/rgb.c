/*
 * Driver for RGB controller.
 *
 */
#include "module.h"
#include "lauxlib.h"
#include "platform.h"
#include "c_stdlib.h"
#include "vfs.h"

typedef struct {
    uint16 r;
    uint16 g;
    uint16 b;
    uint32 t;
} seq_data_t;

typedef enum {
    RGB_OFF,
    RGB_SEQ
} mode_t;

uint32 r_id, g_id, b_id;
uint32 seq_t;
uint32 seq_len;
uint32 seq_curr;
sint16 speed = 100;
sint16 brightness = 100;
static mode_t mode = RGB_OFF;
static seq_data_t *data = NULL;
static os_timer_t rgb_timer;

void rgb_timer_cb() {
  if (mode == RGB_OFF) {
    platform_pwm_set_duty(r_id, 0);
    platform_pwm_set_duty(g_id, 0);
    platform_pwm_set_duty(b_id, 0);
  } else if (mode == RGB_SEQ && data != NULL) {
    seq_data_t from = data[seq_curr];

    uint32 t = from.t;
    uint32 st = seq_t / 100;
    if (st >= t) {
      st = 0;
      seq_t = 0;
      seq_curr = (seq_curr + 1) % seq_len;
      from = data[seq_curr];
      t = from.t;
    }
    seq_data_t to = data[(seq_curr + 1) % seq_len];
    uint16 r = (uint16) (((((((uint32) to.r) * st + ((uint32) from.r) * (t - st)) / t) * brightness) / 100));
    uint16 g = (uint16) (((((((uint32) to.g) * st + ((uint32) from.g) * (t - st)) / t) * brightness) / 100));
    uint16 b = (uint16) (((((((uint32) to.b) * st + ((uint32) from.b) * (t - st)) / t) * brightness) / 100));

    platform_pwm_set_duty(r_id, r);
    platform_pwm_set_duty(g_id, g);
    platform_pwm_set_duty(b_id, b);

    seq_t += speed;
  }
}

static int rgb_init(lua_State *L) {
  r_id = (uint32) luaL_checkinteger(L, 1);
  g_id = (uint32) luaL_checkinteger(L, 2);
  b_id = (uint32) luaL_checkinteger(L, 3);

  platform_pwm_setup(r_id, 500, 0);
  platform_pwm_start(r_id);
  platform_pwm_setup(g_id, 500, 0);
  platform_pwm_start(g_id);
  platform_pwm_setup(b_id, 500, 0);
  platform_pwm_start(b_id);

  os_timer_disarm(&rgb_timer);
  os_timer_setfn(&rgb_timer, (os_timer_func_t *) rgb_timer_cb, NULL);
  os_timer_arm(&rgb_timer, 10, 1);

  return 0;
}

static int rgb_file(lua_State *L) {
  mode = RGB_OFF;
  char buff[1024];
  const char *fname = luaL_checkstring(L, 1);
  const char *basename = vfs_basename(fname);
  int file_fd = vfs_open(fname, "r");
  if (file_fd) {
    if (data != NULL) {
      c_free(data);
    }
    int n = 0;
    int s = 0;
    sint32_t read = vfs_read(file_fd, buff, 1024);
    while (read > 0) {
      int num = 0;
      for (int i = 0; i < read; i++) {
        char c = buff[i];
        if (c >= '0' && c <= '9') {
          num *= 10;
          num += c - '0';
        } else if (c == '\n' || c == ',') {
          if (n == 0) {
            data = c_malloc(sizeof(seq_data_t) * num);
            seq_len = (uint32) num;
          } else {
            if (n % 4 == 1) {
              data[s].r = (uint16) num;
            } else if (n % 4 == 2) {
              data[s].g = (uint16) num;
            } else if (n % 4 == 3) {
              data[s].b = (uint16) num;
            } else if (n % 4 == 0) {
              data[s].t = (uint16) num;
              s++;
            }
          }
          num = 0;
          n++;
        }
      }
      read = vfs_read(file_fd, buff, 1024);
    }
    seq_t = 0;
    seq_curr = 0;
    mode = RGB_SEQ;
    vfs_close(file_fd);
  }
}

static int rgb_static(lua_State *L) {
  mode = RGB_OFF;
  uint16 r = (uint16) luaL_checkinteger(L, 1);
  uint16 g = (uint16) luaL_checkinteger(L, 2);
  uint16 b = (uint16) luaL_checkinteger(L, 3);
  if (data != NULL) {
    c_free(data);
  }
  data = c_malloc(sizeof(seq_data_t));
  data[0].t = 100;
  data[0].r = r;
  data[0].g = g;
  data[0].b = b;

  seq_t = 0;
  seq_len = 1;
  seq_curr = 0;
  mode = RGB_SEQ;
}

static int rgb_off(lua_State *L) {
  mode = RGB_OFF;

  return 0;
}

static int rgb_get(lua_State *L) {
  lua_pushinteger(L, platform_pwm_get_duty(r_id));
  lua_pushinteger(L, platform_pwm_get_duty(g_id));
  lua_pushinteger(L, platform_pwm_get_duty(b_id));
  lua_pushinteger(L, brightness);
  lua_pushinteger(L, speed);

  return 5;
}

static int rgb_speed(lua_State *L) {
  sint16 new_speed = (sint16) luaL_checkinteger(L, 1);
  if (new_speed < 1) {
    new_speed = 1;
  }
  if (new_speed > 10000) {
    new_speed = 10000;
  }
  speed = new_speed;
  return 0;
}

static int rgb_speed_add(lua_State *L) {
  sint16 new_speed = speed + (sint16) luaL_checkinteger(L, 1);
  if (new_speed < 1) {
    new_speed = 1;
  }
  if (new_speed > 10000) {
    new_speed = 10000;
  }
  speed = new_speed;
  return 0;
}

static int rgb_brightness(lua_State *L) {
  sint16 new_brightness = (sint16) luaL_checkinteger(L, 1);
  if (new_brightness < 0) {
    new_brightness = 0;
  }
  if (new_brightness > 100) {
    new_brightness = 100;
  }
  brightness = new_brightness;
  return 0;
}

static int rgb_brightness_add(lua_State *L) {
  sint16 new_brightness = brightness + (sint16) luaL_checkinteger(L, 1);
  if (new_brightness < 0) {
    new_brightness = 0;
  }
  if (new_brightness > 100) {
    new_brightness = 100;
  }
  brightness = new_brightness;
  return 0;
}

static const LUA_REG_TYPE rgb_map[] = {
    {LSTRKEY("init"),          LFUNCVAL(rgb_init)},
    {LSTRKEY("static"),        LFUNCVAL(rgb_static)},
    {LSTRKEY("file"),          LFUNCVAL(rgb_file)},
    {LSTRKEY("get"),           LFUNCVAL(rgb_get)},
    {LSTRKEY("off"),           LFUNCVAL(rgb_off)},
    {LSTRKEY("speed"),         LFUNCVAL(rgb_speed)},
    {LSTRKEY("speedAdd"),      LFUNCVAL(rgb_speed_add)},
    {LSTRKEY("brightness"),    LFUNCVAL(rgb_brightness)},
    {LSTRKEY("brightnessAdd"), LFUNCVAL(rgb_brightness_add)},
    {LNILKEY,                  LNILVAL}
};

NODEMCU_MODULE(RGB, "rgb", rgb_map, NULL);
