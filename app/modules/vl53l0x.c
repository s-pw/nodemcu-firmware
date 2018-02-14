/*
 * Driver for VL53L0XD/SHT21 humidity/temperature sensor.
 *
 */
#include "module.h"
#include "lauxlib.h"
#include "platform.h"

#define ADDRESS_DEFAULT 0b0101001
#define decodeVcselPeriod(reg_val)      (uint8_t) (((reg_val) + 1) << 1)
#define calcMacroPeriod(vcsel_period_pclks) ((((uint32_t)2304 * (vcsel_period_pclks) * 1655) + 500) / 1000)

enum regAddr
{
    SYSRANGE_START                              = 0x00,

    SYSTEM_THRESH_HIGH                          = 0x0C,
    SYSTEM_THRESH_LOW                           = 0x0E,

    SYSTEM_SEQUENCE_CONFIG                      = 0x01,
    SYSTEM_RANGE_CONFIG                         = 0x09,
    SYSTEM_INTERMEASUREMENT_PERIOD              = 0x04,

    SYSTEM_INTERRUPT_CONFIG_GPIO                = 0x0A,

    GPIO_HV_MUX_ACTIVE_HIGH                     = 0x84,

    SYSTEM_INTERRUPT_CLEAR                      = 0x0B,

    RESULT_INTERRUPT_STATUS                     = 0x13,
    RESULT_RANGE_STATUS                         = 0x14,

    RESULT_CORE_AMBIENT_WINDOW_EVENTS_RTN       = 0xBC,
    RESULT_CORE_RANGING_TOTAL_EVENTS_RTN        = 0xC0,
    RESULT_CORE_AMBIENT_WINDOW_EVENTS_REF       = 0xD0,
    RESULT_CORE_RANGING_TOTAL_EVENTS_REF        = 0xD4,
    RESULT_PEAK_SIGNAL_RATE_REF                 = 0xB6,

    ALGO_PART_TO_PART_RANGE_OFFSET_MM           = 0x28,

    I2C_SLAVE_DEVICE_ADDRESS                    = 0x8A,

    MSRC_CONFIG_CONTROL                         = 0x60,

    PRE_RANGE_CONFIG_MIN_SNR                    = 0x27,
    PRE_RANGE_CONFIG_VALID_PHASE_LOW            = 0x56,
    PRE_RANGE_CONFIG_VALID_PHASE_HIGH           = 0x57,
    PRE_RANGE_MIN_COUNT_RATE_RTN_LIMIT          = 0x64,

    FINAL_RANGE_CONFIG_MIN_SNR                  = 0x67,
    FINAL_RANGE_CONFIG_VALID_PHASE_LOW          = 0x47,
    FINAL_RANGE_CONFIG_VALID_PHASE_HIGH         = 0x48,
    FINAL_RANGE_CONFIG_MIN_COUNT_RATE_RTN_LIMIT = 0x44,

    PRE_RANGE_CONFIG_SIGMA_THRESH_HI            = 0x61,
    PRE_RANGE_CONFIG_SIGMA_THRESH_LO            = 0x62,

    PRE_RANGE_CONFIG_VCSEL_PERIOD               = 0x50,
    PRE_RANGE_CONFIG_TIMEOUT_MACROP_HI          = 0x51,
    PRE_RANGE_CONFIG_TIMEOUT_MACROP_LO          = 0x52,

    SYSTEM_HISTOGRAM_BIN                        = 0x81,
    HISTOGRAM_CONFIG_INITIAL_PHASE_SELECT       = 0x33,
    HISTOGRAM_CONFIG_READOUT_CTRL               = 0x55,

    FINAL_RANGE_CONFIG_VCSEL_PERIOD             = 0x70,
    FINAL_RANGE_CONFIG_TIMEOUT_MACROP_HI        = 0x71,
    FINAL_RANGE_CONFIG_TIMEOUT_MACROP_LO        = 0x72,
    CROSSTALK_COMPENSATION_PEAK_RATE_MCPS       = 0x20,

    MSRC_CONFIG_TIMEOUT_MACROP                  = 0x46,

    SOFT_RESET_GO2_SOFT_RESET_N                 = 0xBF,
    IDENTIFICATION_MODEL_ID                     = 0xC0,
    IDENTIFICATION_REVISION_ID                  = 0xC2,

    OSC_CALIBRATE_VAL                           = 0xF8,

    GLOBAL_CONFIG_VCSEL_WIDTH                   = 0x32,
    GLOBAL_CONFIG_SPAD_ENABLES_REF_0            = 0xB0,
    GLOBAL_CONFIG_SPAD_ENABLES_REF_1            = 0xB1,
    GLOBAL_CONFIG_SPAD_ENABLES_REF_2            = 0xB2,
    GLOBAL_CONFIG_SPAD_ENABLES_REF_3            = 0xB3,
    GLOBAL_CONFIG_SPAD_ENABLES_REF_4            = 0xB4,
    GLOBAL_CONFIG_SPAD_ENABLES_REF_5            = 0xB5,

    GLOBAL_CONFIG_REF_EN_START_SELECT           = 0xB6,
    DYNAMIC_SPAD_NUM_REQUESTED_REF_SPAD         = 0x4E,
    DYNAMIC_SPAD_REF_EN_START_OFFSET            = 0x4F,
    POWER_MANAGEMENT_GO1_POWER_FORCE            = 0x80,

    VHV_CONFIG_PAD_SCL_SDA__EXTSUP_HV           = 0x89,

    ALGO_PHASECAL_LIM                           = 0x30,
    ALGO_PHASECAL_CONFIG_TIMEOUT                = 0x30,
};
enum vcselPeriodType { VcselPeriodPreRange, VcselPeriodFinalRange };

struct SequenceStepEnables
{
    bool tcc, msrc, dss, pre_range, final_range;
};

struct SequenceStepTimeouts
{
    uint16_t pre_range_vcsel_period_pclks, final_range_vcsel_period_pclks;

    uint16_t msrc_dss_tcc_mclks, pre_range_mclks, final_range_mclks;
    uint32_t msrc_dss_tcc_us,    pre_range_us,    final_range_us;
};

static void w8u(uint8_t reg, uint8_t val) {
  platform_i2c_send_start(0);
  platform_i2c_send_address(0, ADDRESS_DEFAULT, PLATFORM_I2C_DIRECTION_TRANSMITTER);
  platform_i2c_send_byte(0, reg);
  platform_i2c_send_byte(0, val);
  platform_i2c_send_stop(0);
}

static uint8_t r8u(uint8_t reg) {
  uint8_t value;
  platform_i2c_send_start(0);
  platform_i2c_send_address(0, ADDRESS_DEFAULT, PLATFORM_I2C_DIRECTION_TRANSMITTER);
  platform_i2c_send_byte(0, reg);
  platform_i2c_send_start(0);
  platform_i2c_send_address(0, ADDRESS_DEFAULT, PLATFORM_I2C_DIRECTION_RECEIVER);
  value = (uint8_t) platform_i2c_recv_byte(0, 0);
  platform_i2c_send_stop(0);

  return value;
}

static uint16_t r16u(uint8_t reg) {
  uint16_t value;
  platform_i2c_send_start(0);
  platform_i2c_send_address(0, ADDRESS_DEFAULT, PLATFORM_I2C_DIRECTION_TRANSMITTER);
  platform_i2c_send_byte(0, reg);
  platform_i2c_send_start(0);
  platform_i2c_send_address(0, ADDRESS_DEFAULT, PLATFORM_I2C_DIRECTION_RECEIVER);
  value = (uint16_t) platform_i2c_recv_byte(0, 1) << 8;
  value |= (uint8_t) platform_i2c_recv_byte(0, 0);
  platform_i2c_send_stop(0);

  return value;
}

static void r8u_multi(uint8_t reg, uint8_t * dst, uint8_t count) {
  platform_i2c_send_start(0);
  platform_i2c_send_address(0, ADDRESS_DEFAULT, PLATFORM_I2C_DIRECTION_TRANSMITTER);
  platform_i2c_send_byte(0, reg);
  platform_i2c_send_start(0);
  platform_i2c_send_address(0, ADDRESS_DEFAULT, PLATFORM_I2C_DIRECTION_RECEIVER);
  for (int i = 0; i < count; ++i)
  {
    dst[i] = (uint8_t) platform_i2c_recv_byte(0, i == count - 1 ? 0 : 1);
  }
  platform_i2c_send_stop(0);
}

static void w8u_multi(uint8_t reg, uint8_t * src, uint8_t count) {
  platform_i2c_send_start(0);
  platform_i2c_send_address(0, ADDRESS_DEFAULT, PLATFORM_I2C_DIRECTION_TRANSMITTER);
  platform_i2c_send_byte(0, reg);
  
  while (count-- > 0)
  {
    platform_i2c_send_byte(0, *(src++));
  }
  platform_i2c_send_stop(0);
}

static void w16u(uint8_t reg, uint16_t val) {
  platform_i2c_send_start(0);
  platform_i2c_send_address(0, ADDRESS_DEFAULT, PLATFORM_I2C_DIRECTION_TRANSMITTER);
  platform_i2c_send_byte(0, reg);
  platform_i2c_send_byte(0, (uint8_t) (val >> 8));
  platform_i2c_send_byte(0, (uint8_t) (val & 0xFF));
  platform_i2c_send_stop(0);
}

static void performSingleRefCalibration(uint8_t vhv_init_byte)
{
  w8u(SYSRANGE_START, (uint8_t) 0x01 | vhv_init_byte); // VL53L0X_REG_SYSRANGE_MODE_START_STOP

  uint32_t now = system_get_time();
  while((r8u(RESULT_INTERRUPT_STATUS) & 0x07) == 0 && system_get_time() - now < 500000);

  w8u(SYSTEM_INTERRUPT_CLEAR, 0x01);

  w8u(SYSRANGE_START, 0x00);
}

static void getSequenceStepEnables(struct SequenceStepEnables * enables)
{
  uint8_t sequence_config = r8u(SYSTEM_SEQUENCE_CONFIG);

  enables->tcc          = (sequence_config >> 4) & (uint8_t) 0x1;
  enables->dss          = (sequence_config >> 3) & (uint8_t) 0x1;
  enables->msrc         = (sequence_config >> 2) & (uint8_t) 0x1;
  enables->pre_range    = (sequence_config >> 6) & (uint8_t) 0x1;
  enables->final_range  = (sequence_config >> 7) & (uint8_t) 0x1;
}

static uint8_t getVcselPulsePeriod(enum vcselPeriodType type)
{
  if (type == VcselPeriodPreRange)
  {
    return decodeVcselPeriod(r8u(PRE_RANGE_CONFIG_VCSEL_PERIOD));
  }
  else if (type == VcselPeriodFinalRange)
  {
    return decodeVcselPeriod(r8u(FINAL_RANGE_CONFIG_VCSEL_PERIOD));
  }
  else { return 255; }
}

static uint32_t timeoutMclksToMicroseconds(uint16_t timeout_period_mclks, uint16_t vcsel_period_pclks)
{
  uint32_t macro_period_ns = calcMacroPeriod(vcsel_period_pclks);

  return ((timeout_period_mclks * macro_period_ns) + (macro_period_ns / 2)) / 1000;
}

static uint16_t decodeTimeout(uint16_t reg_val)
{
  // format: "(LSByte * 2^MSByte) + 1"
  return (uint16_t) ((uint16_t)((reg_val & 0x00FF) << (uint16_t)((reg_val & 0xFF00) >> 8)) + 1);
}

static void getSequenceStepTimeouts(struct SequenceStepEnables const * enables, struct SequenceStepTimeouts * timeouts)
{
  timeouts->pre_range_vcsel_period_pclks = getVcselPulsePeriod(VcselPeriodPreRange);

  timeouts->msrc_dss_tcc_mclks = r8u(MSRC_CONFIG_TIMEOUT_MACROP) + (uint8_t) 1;
  timeouts->msrc_dss_tcc_us =
      timeoutMclksToMicroseconds(timeouts->msrc_dss_tcc_mclks,
                                 timeouts->pre_range_vcsel_period_pclks);

  timeouts->pre_range_mclks =
      decodeTimeout(r16u(PRE_RANGE_CONFIG_TIMEOUT_MACROP_HI));
  timeouts->pre_range_us =
      timeoutMclksToMicroseconds(timeouts->pre_range_mclks,
                                 timeouts->pre_range_vcsel_period_pclks);

  timeouts->final_range_vcsel_period_pclks = getVcselPulsePeriod(VcselPeriodFinalRange);

  timeouts->final_range_mclks =
      decodeTimeout(r16u(FINAL_RANGE_CONFIG_TIMEOUT_MACROP_HI));

  if (enables->pre_range)
  {
    timeouts->final_range_mclks -= timeouts->pre_range_mclks;
  }

  timeouts->final_range_us =
      timeoutMclksToMicroseconds(timeouts->final_range_mclks,
                                 timeouts->final_range_vcsel_period_pclks);
}

static uint32_t getMeasurementTimingBudget(void)
{
  struct SequenceStepEnables enables;
  struct SequenceStepTimeouts timeouts;

  uint16_t const StartOverhead     = 1910; // note that this is different than the value in set_
  uint16_t const EndOverhead        = 960;
  uint16_t const MsrcOverhead       = 660;
  uint16_t const TccOverhead        = 590;
  uint16_t const DssOverhead        = 690;
  uint16_t const PreRangeOverhead   = 660;
  uint16_t const FinalRangeOverhead = 550;

  // "Start and end overhead times always present"
  uint32_t budget_us = StartOverhead + EndOverhead;

  getSequenceStepEnables(&enables);
  getSequenceStepTimeouts(&enables, &timeouts);

  if (enables.tcc)
  {
    budget_us += (timeouts.msrc_dss_tcc_us + TccOverhead);
  }

  if (enables.dss)
  {
    budget_us += 2 * (timeouts.msrc_dss_tcc_us + DssOverhead);
  }
  else if (enables.msrc)
  {
    budget_us += (timeouts.msrc_dss_tcc_us + MsrcOverhead);
  }

  if (enables.pre_range)
  {
    budget_us += (timeouts.pre_range_us + PreRangeOverhead);
  }

  if (enables.final_range)
  {
    budget_us += (timeouts.final_range_us + FinalRangeOverhead);
  }

  return budget_us;
}

static uint32_t timeoutMicrosecondsToMclks(uint32_t timeout_period_us, uint16_t vcsel_period_pclks)
{
  uint32_t macro_period_ns = calcMacroPeriod(vcsel_period_pclks);

  return (((timeout_period_us * 1000) + (macro_period_ns / 2)) / macro_period_ns);
}

static uint16_t encodeTimeout(uint16_t timeout_mclks)
{
  // format: "(LSByte * 2^MSByte) + 1"

  uint32_t ls_byte = 0;
  uint16_t ms_byte = 0;

  if (timeout_mclks > 0)
  {
    ls_byte = (uint32_t) (timeout_mclks - 1);

    while ((ls_byte & 0xFFFFFF00) > 0)
    {
      ls_byte >>= 1;
      ms_byte++;
    }

    return (uint16_t) ((ms_byte << 8) | (ls_byte & 0xFF));
  }
  else { return 0; }
}

static bool setMeasurementTimingBudget(uint32_t budget_us)
{
  struct SequenceStepEnables enables;
  struct SequenceStepTimeouts timeouts;

  uint16_t const StartOverhead      = 1320; // note that this is different than the value in get_
  uint16_t const EndOverhead        = 960;
  uint16_t const MsrcOverhead       = 660;
  uint16_t const TccOverhead        = 590;
  uint16_t const DssOverhead        = 690;
  uint16_t const PreRangeOverhead   = 660;
  uint16_t const FinalRangeOverhead = 550;

  uint32_t const MinTimingBudget = 20000;

  if (budget_us < MinTimingBudget) { return false; }

  uint32_t used_budget_us = StartOverhead + EndOverhead;

  getSequenceStepEnables(&enables);
  getSequenceStepTimeouts(&enables, &timeouts);

  if (enables.tcc)
  {
    used_budget_us += (timeouts.msrc_dss_tcc_us + TccOverhead);
  }

  if (enables.dss)
  {
    used_budget_us += 2 * (timeouts.msrc_dss_tcc_us + DssOverhead);
  }
  else if (enables.msrc)
  {
    used_budget_us += (timeouts.msrc_dss_tcc_us + MsrcOverhead);
  }

  if (enables.pre_range)
  {
    used_budget_us += (timeouts.pre_range_us + PreRangeOverhead);
  }

  if (enables.final_range)
  {
    used_budget_us += FinalRangeOverhead;

    // "Note that the final range timeout is determined by the timing
    // budget and the sum of all other timeouts within the sequence.
    // If there is no room for the final range timeout, then an error
    // will be set. Otherwise the remaining time will be applied to
    // the final range."

    if (used_budget_us > budget_us)
    {
      // "Requested timeout too big."
      return false;
    }

    uint32_t final_range_timeout_us = budget_us - used_budget_us;

    // set_sequence_step_timeout() begin
    // (SequenceStepId == VL53L0X_SEQUENCESTEP_FINAL_RANGE)

    // "For the final range timeout, the pre-range timeout
    //  must be added. To do this both final and pre-range
    //  timeouts must be expressed in macro periods MClks
    //  because they have different vcsel periods."

    uint16_t final_range_timeout_mclks =
        (uint16_t) timeoutMicrosecondsToMclks(final_range_timeout_us, timeouts.final_range_vcsel_period_pclks);

    if (enables.pre_range)
    {
      final_range_timeout_mclks += timeouts.pre_range_mclks;
    }

    w16u(FINAL_RANGE_CONFIG_TIMEOUT_MACROP_HI, encodeTimeout(final_range_timeout_mclks));

    // set_sequence_step_timeout() end
  }
  return true;
}

static int init(lua_State* L) {
  //2.8V IO
  w8u(VHV_CONFIG_PAD_SCL_SDA__EXTSUP_HV, r8u(VHV_CONFIG_PAD_SCL_SDA__EXTSUP_HV) | (uint8_t) 0x01); // set bit 0
  // "Set I2C standard mode"
  w8u(0x88, 0x00);

  w8u(0x80, 0x01);
  w8u(0xFF, 0x01);
  w8u(0x00, 0x00);
  uint8_t stop_variable = r8u(0x91);
  w8u(0x00, 0x01);
  w8u(0xFF, 0x00);
  w8u(0x80, 0x00);

  // disable SIGNAL_RATE_MSRC (bit 1) and SIGNAL_RATE_PRE_RANGE (bit 4) limit checks
  w8u(MSRC_CONFIG_CONTROL, r8u(MSRC_CONFIG_CONTROL) | (uint8_t) 0x12);

  // set final range signal rate limit to 0.25 MCPS (million counts per second)
  w16u(FINAL_RANGE_CONFIG_MIN_COUNT_RATE_RTN_LIMIT, 0x0020);

  w8u(SYSTEM_SEQUENCE_CONFIG, 0xFF);

  // VL53L0X_DataInit() end

  // VL53L0X_StaticInit() begin
  
// Get reference SPAD (single photon avalanche diode) count and type
// based on VL53L0X_get_info_from_device(),
// but only gets reference SPAD count and type
  uint8_t spad_count;
  bool spad_type_is_aperture;
  
  w8u(0x80, 0x01);
  w8u(0xFF, 0x01);
  w8u(0x00, 0x00);

  w8u(0xFF, 0x06);
  w8u(0x83, r8u(0x83) | (uint8_t) 0x04);
  w8u(0xFF, 0x07);
  w8u(0x81, 0x01);

  w8u(0x80, 0x01);

  w8u(0x94, 0x6b);
  w8u(0x83, 0x00);
  uint32_t now = system_get_time();
  while(r8u(0x83) == 0x00 && system_get_time() - now < 500000);
  w8u(0x83, 0x01);
  uint8_t tmp = r8u(0x92);

  spad_count = tmp & (uint8_t) 0x7f;
  spad_type_is_aperture = (tmp >> 7) & (uint8_t) 0x01;

  w8u(0x81, 0x00);
  w8u(0xFF, 0x06);
  w8u(0x83, r8u(0x83)  & (uint8_t) ~0x04);
  w8u(0xFF, 0x01);
  w8u(0x00, 0x01);

  w8u(0xFF, 0x00);
  w8u(0x80, 0x00);

  // The SPAD map (RefGoodSpadMap) is read by VL53L0X_get_info_from_device() in
  // the API, but the same data seems to be more easily readable from
  // GLOBAL_CONFIG_SPAD_ENABLES_REF_0 through _6, so read it from there
  uint8_t ref_spad_map[6];
  r8u_multi(GLOBAL_CONFIG_SPAD_ENABLES_REF_0, ref_spad_map, 6);

  // -- VL53L0X_set_reference_spads() begin (assume NVM values are valid)

  w8u(0xFF, 0x01);
  w8u(DYNAMIC_SPAD_REF_EN_START_OFFSET, 0x00);
  w8u(DYNAMIC_SPAD_NUM_REQUESTED_REF_SPAD, 0x2C);
  w8u(0xFF, 0x00);
  w8u(GLOBAL_CONFIG_REF_EN_START_SELECT, 0xB4);

  uint8_t first_spad_to_enable = spad_type_is_aperture ? (uint8_t) 12 : (uint8_t) 0; // 12 is the first aperture spad
  uint8_t spads_enabled = 0;

  for (uint8_t i = 0; i < 48; i++)
  {
    if (i < first_spad_to_enable || spads_enabled == spad_count)
    {
      // This bit is lower than the first one that should be enabled, or
      // (reference_spad_count) bits have already been enabled, so zero this bit
      ref_spad_map[i / 8] &= ~(1 << (i % 8));
    }
    else if ((ref_spad_map[i / 8] >> (i % 8)) & 0x1)
    {
      spads_enabled++;
    }
  }

  w8u_multi(GLOBAL_CONFIG_SPAD_ENABLES_REF_0, ref_spad_map, 6);

  // -- VL53L0X_set_reference_spads() end

  // -- VL53L0X_load_tuning_settings() begin
  // DefaultTuningSettings from vl53l0x_tuning.h

  w8u(0xFF, 0x01);
  w8u(0x00, 0x00);

  w8u(0xFF, 0x00);
  w8u(0x09, 0x00);
  w8u(0x10, 0x00);
  w8u(0x11, 0x00);

  w8u(0x24, 0x01);
  w8u(0x25, 0xFF);
  w8u(0x75, 0x00);

  w8u(0xFF, 0x01);
  w8u(0x4E, 0x2C);
  w8u(0x48, 0x00);
  w8u(0x30, 0x20);

  w8u(0xFF, 0x00);
  w8u(0x30, 0x09);
  w8u(0x54, 0x00);
  w8u(0x31, 0x04);
  w8u(0x32, 0x03);
  w8u(0x40, 0x83);
  w8u(0x46, 0x25);
  w8u(0x60, 0x00);
  w8u(0x27, 0x00);
  w8u(0x50, 0x06);
  w8u(0x51, 0x00);
  w8u(0x52, 0x96);
  w8u(0x56, 0x08);
  w8u(0x57, 0x30);
  w8u(0x61, 0x00);
  w8u(0x62, 0x00);
  w8u(0x64, 0x00);
  w8u(0x65, 0x00);
  w8u(0x66, 0xA0);

  w8u(0xFF, 0x01);
  w8u(0x22, 0x32);
  w8u(0x47, 0x14);
  w8u(0x49, 0xFF);
  w8u(0x4A, 0x00);

  w8u(0xFF, 0x00);
  w8u(0x7A, 0x0A);
  w8u(0x7B, 0x00);
  w8u(0x78, 0x21);

  w8u(0xFF, 0x01);
  w8u(0x23, 0x34);
  w8u(0x42, 0x00);
  w8u(0x44, 0xFF);
  w8u(0x45, 0x26);
  w8u(0x46, 0x05);
  w8u(0x40, 0x40);
  w8u(0x0E, 0x06);
  w8u(0x20, 0x1A);
  w8u(0x43, 0x40);

  w8u(0xFF, 0x00);
  w8u(0x34, 0x03);
  w8u(0x35, 0x44);

  w8u(0xFF, 0x01);
  w8u(0x31, 0x04);
  w8u(0x4B, 0x09);
  w8u(0x4C, 0x05);
  w8u(0x4D, 0x04);

  w8u(0xFF, 0x00);
  w8u(0x44, 0x00);
  w8u(0x45, 0x20);
  w8u(0x47, 0x08);
  w8u(0x48, 0x28);
  w8u(0x67, 0x00);
  w8u(0x70, 0x04);
  w8u(0x71, 0x01);
  w8u(0x72, 0xFE);
  w8u(0x76, 0x00);
  w8u(0x77, 0x00);

  w8u(0xFF, 0x01);
  w8u(0x0D, 0x01);

  w8u(0xFF, 0x00);
  w8u(0x80, 0x01);
  w8u(0x01, 0xF8);

  w8u(0xFF, 0x01);
  w8u(0x8E, 0x01);
  w8u(0x00, 0x01);
  w8u(0xFF, 0x00);
  w8u(0x80, 0x00);

  // -- VL53L0X_load_tuning_settings() end

  // "Set interrupt config to new sample ready"
  // -- VL53L0X_SetGpioConfig() begin

  w8u(SYSTEM_INTERRUPT_CONFIG_GPIO, 0x04);
  w8u(GPIO_HV_MUX_ACTIVE_HIGH, r8u(GPIO_HV_MUX_ACTIVE_HIGH) & (uint8_t) ~0x10); // active low
  w8u(SYSTEM_INTERRUPT_CLEAR, 0x01);

  // -- VL53L0X_SetGpioConfig() end

  uint32_t measurement_timing_budget_us = getMeasurementTimingBudget();

  // "Disable MSRC and TCC by default"
  // MSRC = Minimum Signal Rate Check
  // TCC = Target CentreCheck
  // -- VL53L0X_SetSequenceStepEnable() begin

  w8u(SYSTEM_SEQUENCE_CONFIG, 0xE8);

  // -- VL53L0X_SetSequenceStepEnable() end

  // "Recalculate timing budget"
  setMeasurementTimingBudget(measurement_timing_budget_us);

  // VL53L0X_StaticInit() end

  // VL53L0X_PerformRefCalibration() begin (VL53L0X_perform_ref_calibration())

  // -- VL53L0X_perform_vhv_calibration() begin

  w8u(SYSTEM_SEQUENCE_CONFIG, 0x01);
  performSingleRefCalibration(0x40);

  // -- VL53L0X_perform_vhv_calibration() end

  // -- VL53L0X_perform_phase_calibration() begin

  w8u(SYSTEM_SEQUENCE_CONFIG, 0x02);
  performSingleRefCalibration(0x00);

  // -- VL53L0X_perform_phase_calibration() end

  // "restore the previous Sequence Config"
  w8u(SYSTEM_SEQUENCE_CONFIG, 0xE8);
  
  //startContinuous
  w8u(0x80, 0x01);
  w8u(0xFF, 0x01);
  w8u(0x00, 0x00);
  w8u(0x91, stop_variable);
  w8u(0x00, 0x01);
  w8u(0xFF, 0x00);
  w8u(0x80, 0x00);

  // continuous back-to-back mode
  w8u(SYSRANGE_START, 0x02); // VL53L0X_REG_SYSRANGE_MODE_BACKTOBACK
  
  return 0;
}

static int readRangeContinuousMillimeters(lua_State* L)
{

  uint32_t now = system_get_time();
  while((r8u(RESULT_INTERRUPT_STATUS) & 0x07) == 0 && system_get_time() - now < 500000);
  
  // assumptions: Linearity Corrective Gain is 1000 (default);
  // fractional ranging is not enabled
  uint16_t range = r16u(RESULT_RANGE_STATUS + 10);

  w8u(SYSTEM_INTERRUPT_CLEAR, 0x01);

  lua_pushinteger(L, range);
  
  return 1;
}

static const LUA_REG_TYPE vl53l0x_map[] = {
    { LSTRKEY( "read" ),         LFUNCVAL( readRangeContinuousMillimeters )},
    { LSTRKEY( "setup" ),        LFUNCVAL( init )},
    { LNILKEY, LNILVAL}
};

NODEMCU_MODULE(VL53L0X, "vl53l0x", vl53l0x_map, NULL);
