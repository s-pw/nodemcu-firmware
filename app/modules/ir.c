#include "module.h"
#include "lauxlib.h"
#include "platform.h"
#include "c_stdlib.h"

// Constants
#define HEADER         2U  // Usual nr. of header entries.
#define FOOTER         2U  // Usual nr. of footer (stop bits) entries.
#define OFFSET_START   1U  // Usual rawbuf entry to start processing from.
#define MS_TO_USEC(x)  (x * 1000U)  // Convert milli-Seconds to micro-Seconds.
// Marks tend to be 100us too long, and spaces 100us too short
// when received due to sensor lag.
#define MARK_EXCESS   50U
#define REPEAT (18446744073709551615ULL)
// receiver states
#define STATE_IDLE     2U
#define STATE_MARK     3U
#define STATE_STOP     5U
#define TOLERANCE     25U  // default percent tolerance in measurements
#define RAWTICK        2U  // Capture tick to uSec factor.

#define NEC_BITS                    32U
#define NEC_TICK                     560U
#define NEC_HDR_MARK_TICKS            16U
#define NEC_HDR_MARK                 (NEC_HDR_MARK_TICKS * NEC_TICK)
#define NEC_HDR_SPACE_TICKS            8U
#define NEC_HDR_SPACE                (NEC_HDR_SPACE_TICKS * NEC_TICK)
#define NEC_BIT_MARK_TICKS             1U
#define NEC_ONE_SPACE_TICKS            3U
#define NEC_ZERO_SPACE_TICKS           1U
#define NEC_RPT_SPACE_TICKS            4U
#define NEC_RPT_SPACE                (NEC_RPT_SPACE_TICKS * NEC_TICK)
#define NEC_RPT_LENGTH                 4U
#define NEC_MIN_COMMAND_LENGTH_TICKS 193U
#define NEC_MIN_GAP_TICKS (NEC_MIN_COMMAND_LENGTH_TICKS - \
    (NEC_HDR_MARK_TICKS + NEC_HDR_SPACE_TICKS + \
     NEC_BITS * (NEC_BIT_MARK_TICKS + NEC_ONE_SPACE_TICKS) + \
     NEC_BIT_MARK_TICKS))

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

typedef struct
{
    uint8_t rcvstate;             // state machine
    uint16_t bufsize;             // max. nr. of entries in the capture buffer.
    uint16_t *rawbuf;             // raw data
    uint16_t rawlen;              // counter of entries in rawbuf.
    uint8_t overflow;             // Buffer overflow indicator.
} irparams_t;

enum decode_type_t
{
    UNKNOWN = -1,
    UNUSED = 0,
    NEC,
    RAW,
};

typedef struct
{
    enum decode_type_t decode_type;
    union
    {
        struct
        {
            uint64_t value;  // Decoded value
            uint32_t address;  // Decoded device address.
            uint32_t command;  // Decoded command.
        };
    };
    uint16_t bits;  // Number of bits in decoded value
    volatile uint16_t *rawbuf;  // Raw intervals in .5 us ticks
    uint16_t rawlen;  // Number of records in rawbuf.
    bool overflow;
    bool repeat;  // Is the result a repeat code?
} decode_results;

typedef struct
{
    bool success;  // Was the match successful?
    uint64_t data;  // The data found.
    uint16_t used;  // How many buffer positions were used.
} match_result_t;

uint32_t rev_gpio_bits;
uint8_t timeout;              // Nr. of milliSeconds before we give up.
static ETSTimer timer;
volatile irparams_t irparams;

static void ICACHE_RAM_ATTR read_timeout(void *arg __attribute__((unused)))
{
  ets_intr_lock();
  if (irparams.rawlen)
    irparams.rcvstate = STATE_STOP;
  ets_intr_unlock();
}

static uint32_t ICACHE_RAM_ATTR gpio_intr(uint32_t ret_gpio_status, uint32_t now)
{
  static uint32_t start = 0;
  ret_gpio_status &= rev_gpio_bits;

  uint32 gpio_status = GPIO_REG_READ(GPIO_STATUS_ADDRESS);
  GPIO_REG_WRITE(GPIO_STATUS_W1TC_ADDRESS, gpio_status);
  os_timer_disarm(&timer);

  uint16_t rawlen = irparams.rawlen;

  if (rawlen >= irparams.bufsize)
  {
    irparams.overflow = true;
    irparams.rcvstate = STATE_STOP;
  }

  if (irparams.rcvstate == STATE_STOP)
    return ret_gpio_status;

  if (irparams.rcvstate == STATE_IDLE)
  {
    irparams.rcvstate = STATE_MARK;
    irparams.rawbuf[rawlen] = 1;
  } else
  {
    if (now < start)
      irparams.rawbuf[rawlen] = (uint16_t) ((0xFFFFFFFF - start + now) / RAWTICK);
    else
      irparams.rawbuf[rawlen] = (uint16_t) ((now - start) / RAWTICK);
  }
  irparams.rawlen++;

  start = now;
  os_timer_arm(&timer, timeout, 0);

  return ret_gpio_status;
}

void resume()
{
  irparams.rcvstate = STATE_IDLE;
  irparams.rawlen = 0;
  irparams.overflow = false;
}

static int ir_setup(lua_State *L)
{
  uint8_t recvpin = (uint8_t) luaL_optinteger(L, 1, 0);
  uint16_t bufsize = (uint16_t) luaL_optinteger(L, 2, 100);
  uint8_t timeout_in = (uint8_t) luaL_optinteger(L, 3, 15);

  irparams.bufsize = bufsize;
  // Ensure we are going to be able to store all possible values in the
  // capture buffer.
  timeout = timeout_in;
  irparams.rawbuf = c_malloc(sizeof(uint16_t) * bufsize);
  if (irparams.rawbuf == NULL)
  {
    luaL_error(L, "out of memory");
  }

  resume();

  // Initialize timer
  os_timer_disarm(&timer);
  os_timer_setfn(&timer, (os_timer_func_t *) read_timeout, NULL);

  // Attach Interrupt
  platform_gpio_mode(recvpin, PLATFORM_GPIO_INT, PLATFORM_GPIO_PULLUP);
  gpio_pin_intr_state_set(GPIO_ID_PIN(pin_num[recvpin]), GPIO_PIN_INTR_ANYEDGE);

  uint32_t bits = (uint32_t) 1 << pin_num[recvpin];
  rev_gpio_bits = ~bits;
  platform_gpio_register_intr_hook(bits, gpio_intr);

  return 0;
}

int16_t compare(uint16_t oldval, uint16_t newval)
{
  if (newval < oldval * 0.8)
    return 0;
  else if (oldval < newval * 0.8)
    return 2;
  else
    return 1;
}


uint32_t ticksLow(uint32_t usecs, uint8_t tolerance, uint16_t delta)
{
  // max() used to ensure the result can't drop below 0 before the cast.
  return ((uint32_t) MAX((int32_t) (usecs * (1.0 - tolerance / 100.0) - delta), 0));
}

uint32_t ticksHigh(uint32_t usecs, uint8_t tolerance, uint16_t delta)
{
  return ((uint32_t) (usecs * (1.0 + tolerance / 100.0)) + 1 + delta);
}

bool match(uint32_t measured, uint32_t desired, uint8_t tolerance, uint16_t delta)
{
  measured *= RAWTICK;  // Convert to uSecs.
  return (bool) (measured >= ticksLow(desired, tolerance, delta) &&
                 measured <= ticksHigh(desired, tolerance, delta));
}

bool matchSpace(uint32_t measured, uint32_t desired,
                uint8_t tolerance, int16_t excess)
{
  return match(measured, desired - excess, tolerance, 0);
}

bool matchMark(uint32_t measured, uint32_t desired,
               uint8_t tolerance, int16_t excess)
{
  return match(measured, desired + excess, tolerance, 0);
}

match_result_t matchData(const volatile uint16_t *data_ptr,
                         const uint16_t nbits, const uint16_t onemark,
                         const uint32_t onespace,
                         const uint16_t zeromark,
                         const uint32_t zerospace,
                         const uint8_t tolerance)
{
  match_result_t result;
  result.success = false;  // Fail by default.
  result.data = 0;
  for (result.used = 0;
       result.used < nbits * 2;
       result.used += 2, data_ptr += 2)
  {
    // Is the bit a '1'?
    if (matchMark(*data_ptr, onemark, tolerance, MARK_EXCESS) &&
        matchSpace(*(data_ptr + 1), onespace, tolerance, MARK_EXCESS))
      result.data = (result.data << 1) | 1;
      // or is the bit a '0'?
    else if (matchMark(*data_ptr, zeromark, tolerance, MARK_EXCESS) &&
             matchSpace(*(data_ptr + 1), zerospace, tolerance, MARK_EXCESS))
      result.data <<= 1;
    else
      return result;  // It's neither, so fail.
  }
  result.success = true;
  return result;
}

bool matchAtLeast(uint32_t measured, uint32_t desired,
                  uint8_t tolerance, uint16_t delta)
{
  measured *= RAWTICK;  // Convert to uSecs.
  if (measured == 0) return true;
  return (bool) (measured >= ticksLow(MIN(desired, MS_TO_USEC(timeout)), tolerance, delta));
}

uint64_t reverseBits(uint64_t input, uint16_t nbits)
{
  if (nbits <= 1)
    return input;  // Reversing <= 1 bits makes no change at all.
  // Cap the nr. of bits to rotate to the max nr. of bits in the input.
  nbits = MIN(nbits, (uint16_t) (sizeof(input) * 8));
  uint64_t output = 0;
  for (uint16_t i = 0; i < nbits; i++)
  {
    output <<= 1;
    output |= (input & 1);
    input >>= 1;
  }
  // Merge any remaining unreversed bits back to the top of the reversed bits.
  return (input << nbits) | output;
}

bool decodeNEC(decode_results *results)
{
  if (results->rawlen < 2 * NEC_BITS + HEADER + FOOTER - 1 &&
      results->rawlen != NEC_RPT_LENGTH)
    return false;  // Can't possibly be a valid NEC message.

  uint64_t data = 0;
  uint16_t offset = OFFSET_START;

  // Header
  if (!matchMark(results->rawbuf[offset], NEC_HDR_MARK, TOLERANCE, MARK_EXCESS)) return false;
  // Calculate how long the lowest tick time is based on the header mark.
  uint32_t mark_tick = results->rawbuf[offset++] * RAWTICK /
                       NEC_HDR_MARK_TICKS;
  // Check if it is a repeat code.
  if (results->rawlen == NEC_RPT_LENGTH &&
      matchSpace(results->rawbuf[offset], NEC_RPT_SPACE, TOLERANCE, MARK_EXCESS) &&
      matchMark(results->rawbuf[offset + 1], NEC_BIT_MARK_TICKS * mark_tick, TOLERANCE, MARK_EXCESS))
  {
    results->value = REPEAT;
    results->decode_type = NEC;
    results->bits = 0;
    results->address = 0;
    results->command = 0;
    results->repeat = true;
    return true;
  }

  // Header (cont.)
  if (!matchSpace(results->rawbuf[offset], NEC_HDR_SPACE, TOLERANCE, MARK_EXCESS)) return false;
  // Calculate how long the common tick time is based on the header space.
  uint32_t space_tick = results->rawbuf[offset++] * RAWTICK /
                        NEC_HDR_SPACE_TICKS;
  // Data
  match_result_t data_result = matchData(&(results->rawbuf[offset]), NEC_BITS,
                                         (uint16_t) (NEC_BIT_MARK_TICKS * mark_tick),
                                         NEC_ONE_SPACE_TICKS * space_tick,
                                         (uint16_t) (NEC_BIT_MARK_TICKS * mark_tick),
                                         NEC_ZERO_SPACE_TICKS * space_tick, TOLERANCE);
  if (data_result.success == false)
    return false;

  data = data_result.data;
  offset += data_result.used;

  // Footer
  if (!matchMark(results->rawbuf[offset++], NEC_BIT_MARK_TICKS * mark_tick, TOLERANCE, MARK_EXCESS))
    return false;

  if (offset < results->rawlen &&
      !matchAtLeast(results->rawbuf[offset], NEC_MIN_GAP_TICKS * space_tick, TOLERANCE, 0))
    return false;

  // Compliance
  // Calculate command and optionally enforce integrity checking.
  uint8_t command = (uint8_t) (data & 0xFF00) >> 8;
  // Command is sent twice, once as plain and then inverted.
  if ((command ^ 0xFF) != (data & 0xFF))
    command = 0;

  // Success
  results->bits = NEC_BITS;
  results->value = data;
  results->decode_type = NEC;
  // NEC command and address are technically in LSB first order so the
  // final versions have to be reversed.
  results->command = (uint32_t) reverseBits(command, 8);
  // Normal NEC protocol has an 8 bit address sent, followed by it inverted.
  uint8_t address = (uint8_t) (data & 0xFF000000) >> 24;
  uint8_t address_inverted = (uint8_t) (data & 0x00FF0000) >> 16;
  if (address == (address_inverted ^ 0xFF))
    // Inverted, so it is normal NEC protocol.
    results->address = (uint32_t) reverseBits(address, 8);
  else  // Not inverted, so must be Extended NEC protocol, thus 16 bit address.
    results->address = (uint32_t) reverseBits((data >> 16) & 0xFFFF, 16);
  return true;
}

bool decode(decode_results *results)
{
  // Proceed only if an IR message been received.
  if (irparams.rcvstate != STATE_STOP)
    return false;

  irparams.rawbuf[irparams.rawlen] = 0;

  results->rawbuf = irparams.rawbuf;
  results->rawlen = irparams.rawlen;
  results->overflow = irparams.overflow;

  // Reset any previously partially processed results.
  results->decode_type = UNKNOWN;
  results->bits = 0;
  results->value = 0;
  results->address = 0;
  results->command = 0;
  results->repeat = false;

  if (decodeNEC(results))
    return true;

  resume();
  return false;
}

static int ir_decode(lua_State *L)
{
  decode_results results;
  if (decode(&results)) {
    lua_pushinteger(L, (uint32_t) results.value);
    lua_pushboolean(L, results.repeat);
    resume();  // Receive the next value
    return 2;
  }
  return 0;
}

// Module function map
static const LUA_REG_TYPE ir_map[] = {
    {LSTRKEY("setup"),  LFUNCVAL(ir_setup)},
    {LSTRKEY("decode"), LFUNCVAL(ir_decode)},
    {LNILKEY,           LNILVAL}
};

NODEMCU_MODULE(IR, "ir", ir_map, NULL);

/*
 *
ir.setup(6)
tmr.create():alarm(100, tmr.ALARM_AUTO, function()
  val, rep = ir.decode()
  if val then
    print(string.format("%x", val / 256 % 256))
  end
end)
 */