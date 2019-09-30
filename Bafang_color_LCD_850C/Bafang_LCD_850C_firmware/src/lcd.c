/*
 * Bafang LCD 850C firmware
 *
 * Copyright (C) Casainho, 2018.
 *
 * Released under the GPL License, Version 3
 */

#include <math.h>
#include "stm32f10x.h"
#include "stm32f10x_gpio.h"
#include "stdio.h"
#include "main.h"
#include "config.h"
#include "utils.h"
#include "pins.h"
#include "lcd_configurations.h"
#include "lcd.h"
#include "buttons.h"
#include "eeprom.h"
#include "usart1.h"
#include "ugui_driver/ugui_bafang_850c.h"
#include "ugui/ugui.h"
#include "rtc.h"
#include "graphs.h"

// Battery SOC symbol:
// 10 bars, each bar: with = 7, height = 24
// symbol has contour lines of 1 pixel
#define BATTERY_SOC_START_X 8
#define BATTERY_SOC_START_Y 4
#define BATTERY_SOC_BAR_WITH 7
#define BATTERY_SOC_BAR_HEIGHT 24
#define BATTERY_SOC_CONTOUR 1

volatile lcd_vars_t m_lcd_vars =
{
  .ui32_main_screen_draw_static_info = 1,
  .lcd_screen_state = LCD_SCREEN_MAIN,
  .ui8_lcd_menu_counter_1000ms_state = 0,
  .main_screen_state = MAIN_SCREEN_STATE_MAIN,
};

static volatile l2_vars_t l2_vars;
static volatile l3_vars_t l3_vars;

static lcd_configurations_menu_t *p_lcd_configurations_vars;

static uint8_t ui8_lcd_menu_counter_100ms_state = 0;

static uint8_t ui8_lcd_menu_config_submenu_state = 0;
static uint8_t ui8_lcd_menu_flash_counter = 0;
static uint16_t ui16_lcd_menu_flash_counter_temperature = 0;
static uint8_t ui8_lcd_menu_flash_state;
static uint8_t ui8_lcd_menu_flash_state_temperature;
static uint8_t ui8_lcd_menu_config_submenu_number = 0;
static uint8_t ui8_lcd_menu_config_submenu_active = 0;

volatile uint32_t ui32_g_layer_2_can_execute = 0;

static uint16_t ui16_m_battery_soc_watts_hour = 0;
static uint16_t ui16_m_battery_soc_watts_hour_fixed = 0;

static uint8_t ui8_m_usart1_received_first_package = 0;

volatile uint8_t ui8_g_usart1_tx_buffer[UART_NUMBER_DATA_BYTES_TO_SEND + 3];

static volatile graphs_t *m_p_graphs;
static volatile uint32_t ui32_m_draw_graphs_1 = 0;
static volatile uint32_t ui32_m_draw_graphs_2 = 0;

volatile uint32_t ui32_g_first_time = 1;

void lcd_main_screen(void);
uint8_t first_time_management(void);
void assist_level_state(void);
void power_off_management(void);
void l2_low_pass_filter_battery_voltage_current_power(void);
void update_menu_flashing_state(void);
void calc_battery_soc_watts_hour(void);
void l2_calc_odometer(void);
static void automatic_power_off_management(void);
void brake(void);
void walk_assist_state(void);
void wheel_speed(void);
void power(void);
void pedal_human_power(void);
void power_off_management(void);
void temperature(void);
void time(void);
void battery_soc(void);
void l2_calc_battery_voltage_soc(void);
void l2_calc_wh(void);
void l2_low_pass_filter_pedal_torque_and_power(void);
static void l2_low_pass_filter_pedal_cadence(void);
void lights_state(void);
void lcd_set_backlight_intensity(uint8_t ui8_intensity);
void battery_soc_bar_set(uint32_t ui32_bar_number, uint16_t ui16_color);
void battery_soc_bar_clear(uint32_t ui32_bar_number);
void draw_configurations_screen_mask(void);
void copy_layer_2_layer_3_vars(void);
void graphs_measurements_update(void);
void trip_distance(void);
void trip_time(void);
void change_graph(void);

/* Place your initialization/startup code here (e.g. MyInst_Start()) */
void lcd_init(void)
{
  bafang_500C_lcd_init();
  lcd_set_backlight_intensity(20); // need otherwise no image will be shownbefore this
  UG_FillScreen(C_BLACK);

  lcd_configurations_screen_init();
  p_lcd_configurations_vars = get_lcd_configurations_menu();

  m_p_graphs = get_graphs();
}

void lcd_clock(void)
{
  static uint8_t ui8_counter_100ms = 0;

  // every 100ms
  if(ui8_counter_100ms++ >= 4)
  {
    ui8_counter_100ms = 0;

    // receive data from layer 2 to layer 3
    // send data from layer 3 to layer 2
    ui32_g_layer_2_can_execute = 0;
    copy_layer_2_layer_3_vars();
    ui32_g_layer_2_can_execute = 1;
  }

  if(first_time_management())
  {
    return;
  }

  update_menu_flashing_state();

  calc_battery_soc_watts_hour();

  // enter menu configurations: UP + DOWN click event
  if(buttons_get_up_down_click_event() &&
      m_lcd_vars.lcd_screen_state == LCD_SCREEN_MAIN)
  {
    buttons_clear_all_events();

    // reset needed variables of configurations screen
    p_lcd_configurations_vars->ui8_refresh_full_menu_1 = 1;

    // need to track start configuration
    p_lcd_configurations_vars->ui8_battery_soc_power_used_state = 1;

    m_lcd_vars.lcd_screen_state = LCD_SCREEN_CONFIGURATIONS;
  }

  // enter in menu set power: ONOFF + UP click event
  if(m_lcd_vars.lcd_screen_state == LCD_SCREEN_MAIN &&
      buttons_get_onoff_click_event() &&
      buttons_get_up_click_event())
  {
    buttons_clear_all_events();
    m_lcd_vars.main_screen_state = MAIN_SCREEN_STATE_POWER;
  }

  // ui32_m_draw_graphs_1 == 1 every 3.5 seconds, set on timer interrupt
  // note: this piece of code must run before lcd_main_screen() -> graphs_draw()
  if(ui32_m_draw_graphs_1 &&
      ui32_g_first_time == 0) // start update graphs only after a startup delay to avoid wrong values of the variables
  {
    ui32_m_draw_graphs_2 = 1;
    graphs_clock_1();
  }

  switch(m_lcd_vars.lcd_screen_state)
  {
    case LCD_SCREEN_MAIN:
      lcd_main_screen();
    break;

    case LCD_SCREEN_CONFIGURATIONS:
      lcd_configurations_screen();
    break;
  }

  // ui32_m_draw_graphs_2 == 1 every 3.5 seconds, set on timer interrupt
  // note: this piece of code must run after lcd_main_screen() -> graphs_draw()
  if(ui32_m_draw_graphs_1 &&
      ui32_m_draw_graphs_2 &&
      ui32_g_first_time == 0) // start update graphs only after a startup delay to avoid wrong values of the variables
  {
    graphs_clock_2();
  }

  automatic_power_off_management();
  power_off_management();

  // must be reset after a full cycle of lcd_clock()
  ui32_m_draw_graphs_1 = 0;
  ui32_m_draw_graphs_2 = 0;
}

void lcd_draw_main_menu_mask(void)
{
  UG_DrawLine(0, 39, 319, 39, MAIN_SCREEN_FIELD_LABELS_COLOR);
  UG_DrawLine(0, 159, 319, 159, MAIN_SCREEN_FIELD_LABELS_COLOR);
  UG_DrawLine(0, 239, 319, 239, MAIN_SCREEN_FIELD_LABELS_COLOR);
  UG_DrawLine(0, 319, 319, 319, MAIN_SCREEN_FIELD_LABELS_COLOR);

  // vertical line
  UG_DrawLine(159, 159, 159, 319, MAIN_SCREEN_FIELD_LABELS_COLOR);
}

void lcd_main_screen(void)
{
  // run once only, to draw static info
  if(m_lcd_vars.ui32_main_screen_draw_static_info)
  {
    UG_FillScreen(C_BLACK);
    lcd_draw_main_menu_mask();
  }

  lights_state();
  time();
  assist_level_state();
  wheel_speed();
  walk_assist_state();
//  offroad_mode();
  power();
  pedal_human_power();
  battery_soc();
  brake();
  trip_time();
  trip_distance();

  change_graph();

  // ui32_m_draw_graphs_2 == 1 every 3.5 seconds, set on timer interrupt
  if(ui32_m_draw_graphs_2 ||
      m_lcd_vars.ui32_main_screen_draw_static_info)
  {
    graphs_draw(&m_lcd_vars);
  }

  // this event is not used so we must clear it
  buttons_get_onoff_click_long_click_event();

  m_lcd_vars.ui32_main_screen_draw_static_info = 0;
}

void layer_2(void)
{
  uint8_t *p_rx_buffer = 0;
  static uint32_t ui32_wheel_speed_sensor_tick_temp;
  static uint8_t ui8_i;
  uint8_t ui8_temp;
  uint16_t ui16_temp;
  uint16_t ui16_crc_tx;
  static uint8_t ui8_message_id = 0;
  static uint8_t ui8_state_machine = 0;

  /************************************************************************************************/
  // process rx package
  if(usart1_received_package())
  {
    p_rx_buffer = usart1_get_rx_buffer();

    // now process rx data
    // only if first byte is equal to package start byte
    if(*p_rx_buffer == 67)
    {
      p_rx_buffer++;

      l2_vars.ui16_adc_battery_voltage = *p_rx_buffer;
      p_rx_buffer++;

      l2_vars.ui16_adc_battery_voltage |= ((uint16_t) (*p_rx_buffer & 0x30)) << 4;
      p_rx_buffer++;

      l2_vars.ui8_battery_current_x5 = *p_rx_buffer;
      p_rx_buffer++;

      l2_vars.ui16_wheel_speed_x10 = (uint16_t) *p_rx_buffer;
      p_rx_buffer++;
      l2_vars.ui16_wheel_speed_x10 += ((uint16_t) *p_rx_buffer << 8);
      p_rx_buffer++;

      ui8_temp = *p_rx_buffer;
      l2_vars.ui8_braking = ui8_temp & 1;
      p_rx_buffer++;

      l2_vars.ui8_adc_throttle = *p_rx_buffer;
      p_rx_buffer++;

      if(l2_vars.ui8_temperature_limit_feature_enabled)
      {
        l2_vars.ui8_motor_temperature = *p_rx_buffer;
      }
      else
      {
        l2_vars.ui8_throttle = *p_rx_buffer;
      }
      p_rx_buffer++;

      l2_vars.ui8_adc_pedal_torque_sensor = *p_rx_buffer;
      p_rx_buffer++;

      l2_vars.ui8_pedal_torque_sensor = *p_rx_buffer;
      p_rx_buffer++;

      l2_vars.ui8_pedal_cadence = *p_rx_buffer;
      p_rx_buffer++;

      l2_vars.ui8_pedal_human_power = *p_rx_buffer;
      p_rx_buffer++;

      l2_vars.ui8_duty_cycle = *p_rx_buffer;
      p_rx_buffer++;

      l2_vars.ui16_motor_speed_erps = (uint16_t) *p_rx_buffer;
      p_rx_buffer++;
      l2_vars.ui16_motor_speed_erps += ((uint16_t) *p_rx_buffer << 8);
      p_rx_buffer++;

      l2_vars.ui8_foc_angle = *p_rx_buffer;
      p_rx_buffer++;

      // error states
      l2_vars.ui8_error_states = *p_rx_buffer;
      p_rx_buffer++;

      // temperature actual limiting value
      l2_vars.ui8_temperature_current_limiting_value = *p_rx_buffer;
      p_rx_buffer++;

      // wheel_speed_sensor_tick_counter
      ui32_wheel_speed_sensor_tick_temp = ((uint32_t) *p_rx_buffer);
      p_rx_buffer++;
      ui32_wheel_speed_sensor_tick_temp |= (((uint32_t) *p_rx_buffer) << 8);
      p_rx_buffer++;
      ui32_wheel_speed_sensor_tick_temp |= (((uint32_t) *p_rx_buffer) << 16);
      l2_vars.ui32_wheel_speed_sensor_tick_counter = ui32_wheel_speed_sensor_tick_temp;
      p_rx_buffer++;

      // ui16_pedal_torque_x10
      l2_vars.ui16_pedal_torque_x10 = (uint16_t) *p_rx_buffer;
      p_rx_buffer++;
      l2_vars.ui16_pedal_torque_x10 += ((uint16_t) *p_rx_buffer << 8);
      p_rx_buffer++;

      // ui16_pedal_power_x10
      l2_vars.ui16_pedal_power_x10 = (uint16_t) *p_rx_buffer;
      p_rx_buffer++;
      l2_vars.ui16_pedal_power_x10 += ((uint16_t) *p_rx_buffer << 8);

      usart1_reset_received_package();
    }
  }
  /************************************************************************************************/

  /************************************************************************************************/
  // send tx package

  // start up byte
  ui8_g_usart1_tx_buffer[0] = 0x59;
  ui8_g_usart1_tx_buffer[1] = ui8_message_id;

  if(l2_vars.ui8_walk_assist)
  {
    ui8_g_usart1_tx_buffer[2] = l2_vars.ui8_walk_assist_level_factor[((l2_vars.ui8_assist_level) - 1)];
  }
  else if(l2_vars.ui8_assist_level)
  {
    ui8_g_usart1_tx_buffer[2] = l2_vars.ui8_assist_level_factor[((l2_vars.ui8_assist_level) - 1)];
  }
  else
  {
    ui8_g_usart1_tx_buffer[2] = 0;
  }


  // set lights state
  // walk assist level state
  // set offroad state
//  ui8_g_usart1_tx_buffer[3] = (l2_vars.ui8_lights & 1) |
//      ((l2_vars.ui8_walk_assist & 1) << 1) |
//      ((l2_vars.ui8_offroad_mode & 1) << 2);

  ui8_g_usart1_tx_buffer[3] = (l2_vars.ui8_lights & 1) |
      ((l2_vars.ui8_walk_assist & 1) << 1);

  // battery power
  ui8_g_usart1_tx_buffer[4] = l2_vars.ui8_target_max_battery_power;

  switch(ui8_message_id)
  {
    case 0:
      // battery low voltage cut-off
      ui8_g_usart1_tx_buffer[5] = (uint8_t) (l2_vars.ui16_battery_low_voltage_cut_off_x10 & 0xff);
      ui8_g_usart1_tx_buffer[6] = (uint8_t) (l2_vars.ui16_battery_low_voltage_cut_off_x10 >> 8);
    break;

    case 1:
      // wheel perimeter
      if(l2_vars.ui8_units_type == 0)
      {
        ui8_g_usart1_tx_buffer[5] = (uint8_t) (l2_vars.ui16_wheel_perimeter & 0xff);
        ui8_g_usart1_tx_buffer[6] = (uint8_t) (l2_vars.ui16_wheel_perimeter >> 8);
      }
      else
      {
        // convert to imperial
        ui16_temp = (l2_vars.ui16_wheel_perimeter_imperial_x10 * 254) / 10;
        ui8_g_usart1_tx_buffer[5] = (uint8_t) (ui16_temp & 0xff);
        ui8_g_usart1_tx_buffer[6] = (uint8_t) (ui16_temp >> 8);
      }
    break;

    case 2:
      // wheel max speed
      if(l2_vars.ui8_units_type == 0)
      {
        ui8_g_usart1_tx_buffer[5] = l2_vars.ui8_wheel_max_speed;
      }
      else
      {
        // convert to imperial
        ui8_g_usart1_tx_buffer[5] = (uint8_t) (((uint16_t) l2_vars.ui8_wheel_max_speed_imperial * 16) / 10);
      }

      // battery max current
      ui8_g_usart1_tx_buffer[6] = l2_vars.ui8_battery_max_current;
    break;

    case 3:
      ui8_g_usart1_tx_buffer[5] = l2_vars.ui8_motor_type;

      ui8_g_usart1_tx_buffer[6] = (l2_vars.ui8_startup_motor_power_boost_state & 1) |
                          ((l2_vars.ui8_startup_motor_power_boost_state & 1) << 1);
    break;

    case 4:
      // startup motor power boost
      ui8_g_usart1_tx_buffer[5] = l2_vars.ui8_startup_motor_power_boost_factor[((l2_vars.ui8_assist_level) - 1)];
      // startup motor power boost time
      ui8_g_usart1_tx_buffer[6] = l2_vars.ui8_startup_motor_power_boost_time;
    break;

    case 5:
      // startup motor power boost fade time
      ui8_g_usart1_tx_buffer[5] = l2_vars.ui8_startup_motor_power_boost_fade_time;
      // boost feature enabled
      ui8_g_usart1_tx_buffer[6] = (l2_vars.ui8_startup_motor_power_boost_feature_enabled & 1) ? 1 : 0;
    break;

    case 6:
      // motor over temperature min and max values to limit
      if(l2_vars.ui8_units_type == 0)
      {
        ui8_g_usart1_tx_buffer[5] = l2_vars.ui8_motor_temperature_min_value_to_limit;
        ui8_g_usart1_tx_buffer[6] = l2_vars.ui8_motor_temperature_max_value_to_limit;
      }
      else
      {
        // convert to SI
        ui8_g_usart1_tx_buffer[5] = (uint8_t) ((((uint16_t) l2_vars.ui8_motor_temperature_min_value_to_limit_imperial) * 18) + 320) / 10;
        ui8_g_usart1_tx_buffer[6] = (uint8_t) ((((uint16_t) l2_vars.ui8_motor_temperature_max_value_to_limit_imperial) * 18) + 320) / 10;
      }
    break;

    case 7:
      ui8_g_usart1_tx_buffer[5] = l2_vars.ui8_ramp_up_amps_per_second_x10;

      // TODO
      // target speed for cruise
      ui8_g_usart1_tx_buffer[6] = 0;
    break;

    case 8:
      // motor temperature limit function or throttle
      ui8_g_usart1_tx_buffer[5] = l2_vars.ui8_temperature_limit_feature_enabled & 3;

      // motor assistance without pedal rotation enable/disable when startup
      ui8_g_usart1_tx_buffer[6] = l2_vars.ui8_motor_assistance_startup_without_pedal_rotation;
    break;

    default:
      ui8_message_id = 0;
    break;
  }

  // prepare crc of the package
  ui16_crc_tx = 0xffff;
  for (ui8_i = 0; ui8_i <= UART_NUMBER_DATA_BYTES_TO_SEND; ui8_i++)
  {
    crc16 (ui8_g_usart1_tx_buffer[ui8_i], &ui16_crc_tx);
  }
  ui8_g_usart1_tx_buffer[UART_NUMBER_DATA_BYTES_TO_SEND + 1] = (uint8_t) (ui16_crc_tx & 0xff);
  ui8_g_usart1_tx_buffer[UART_NUMBER_DATA_BYTES_TO_SEND + 2] = (uint8_t) (ui16_crc_tx >> 8) & 0xff;

  // send the full package to UART
  // start DMA UART transfer
  usart1_start_dma_transfer();

  // increment message_id for next package
  if(++ui8_message_id > UART_MAX_NUMBER_MESSAGE_ID)
  {
    ui8_message_id = 0;
  }

  // let's wait for 10 packages, seems that first ADC battery voltages have incorrect values
  ui8_m_usart1_received_first_package++;
  if(ui8_m_usart1_received_first_package > 10)
    ui8_m_usart1_received_first_package = 10;

  /************************************************************************************************/
  // now do all the calculations that must be done every 100ms

  l2_low_pass_filter_battery_voltage_current_power();
  l2_low_pass_filter_pedal_torque_and_power();
  l2_low_pass_filter_pedal_cadence();
  l2_calc_battery_voltage_soc();
//  l2_calc_odometer();
  l2_calc_wh();
//  automatic_power_off_management();

  graphs_measurements_update();
  /************************************************************************************************/
}

uint8_t first_time_management(void)
{
  static uint8_t ui8_motor_controller_init = 1;
  static uint32_t ui32_counter = 0;
  uint8_t ui8_status = 0;

  // count 10 seconds
  if(++ui32_counter > 500 &&
      ui32_g_first_time == 1)
  {
    ui32_g_first_time = 0;
  }

  // don't update LCD up to we get first communication package from the motor controller
  if(ui8_motor_controller_init &&
      (ui8_m_usart1_received_first_package < 10))
  {
    ui8_status = 1;
  }
  // this will be executed only 1 time at startup
  else if(ui8_motor_controller_init &&
      ui32_g_first_time == 0)
  {
    ui8_motor_controller_init = 0;

    // reset Wh value if battery voltage is over ui16_battery_voltage_reset_wh_counter_x10 (value configured by user)
    if (((uint32_t) l3_vars.ui16_adc_battery_voltage *
        ADC_BATTERY_VOLTAGE_PER_ADC_STEP_X10000) > ((uint32_t) l3_vars.ui16_battery_voltage_reset_wh_counter_x10 * 1000))
    {
      l3_vars.ui32_wh_x10_offset = 0;
    }

    if (l3_vars.ui8_offroad_feature_enabled &&
      l3_vars.ui8_offroad_enabled_on_startup)
    {
      l3_vars.ui8_offroad_mode = 1;
    }
  }

  return ui8_status;
}

void assist_level_state(void)
{
  static print_number_t assist_level =
  {
    .font = &FONT_45X72,
    .fore_color = C_WHITE,
    .back_color = C_BLACK,
    .ui8_previous_digits_array = {255, 255, 255, 255, 255},
    .ui8_field_number_of_digits = 1,
    .ui8_left_zero_paddig = 0,
  };

  static uint8_t ui8_assist_level_previous = 0xff;

  if (m_lcd_vars.ui32_main_screen_draw_static_info)
  {
    UG_SetBackcolor(C_BLACK);
    UG_SetForecolor(MAIN_SCREEN_FIELD_LABELS_COLOR);
    UG_FontSelect(&FONT_10X16);
    UG_PutString(12, 50, "ASSIST");
  }

  if (buttons_get_up_click_event() &&
      m_lcd_vars.main_screen_state == MAIN_SCREEN_STATE_MAIN)
  {
      buttons_clear_all_events();

    l3_vars.ui8_assist_level++;

    if (l3_vars.ui8_assist_level > l3_vars.ui8_number_of_assist_levels)
      { l3_vars.ui8_assist_level = l3_vars.ui8_number_of_assist_levels; }
  }

  if (buttons_get_down_click_event() &&
      m_lcd_vars.main_screen_state == MAIN_SCREEN_STATE_MAIN)
  {
      buttons_clear_all_events();

    if (l3_vars.ui8_assist_level > 0)
      l3_vars.ui8_assist_level--;
  }

  if ((l3_vars.ui8_assist_level != ui8_assist_level_previous) ||
      m_lcd_vars.ui32_main_screen_draw_static_info)
  {
    ui8_assist_level_previous = l3_vars.ui8_assist_level;

    assist_level.ui32_x_position = 20;
    assist_level.ui32_y_position = 81;
    assist_level.ui32_number = (uint32_t) l3_vars.ui8_assist_level;
    assist_level.ui8_refresh_all_digits = m_lcd_vars.ui32_main_screen_draw_static_info;
    lcd_print_number(&assist_level);
  }
}

volatile l3_vars_t* get_l3_vars(void)
{
  return &l3_vars;
}

void trip_time(void)
{
  uint32_t ui32_x_position;
  uint32_t ui32_y_position;
  struct_rtc_time_t *p_time;
  struct_rtc_time_t trip_time;
  static struct_rtc_time_t trip_time_previous;

  p_time = rtc_get_time_since_startup();
  trip_time.ui8_hours = p_time->ui8_hours;
  trip_time.ui8_minutes = p_time->ui8_minutes;

  static print_number_t hours =
  {
    .font = &FONT_24X40,
    .fore_color = C_WHITE,
    .back_color = C_BLACK,
    .ui8_previous_digits_array = {255, 255, 255, 255, 255},
    .ui8_field_number_of_digits = 2,
    .ui8_left_zero_paddig = 0,
  };

  static print_number_t minutes =
  {
    .font = &FONT_24X40,
    .fore_color = C_WHITE,
    .back_color = C_BLACK,
    .ui8_previous_digits_array = {255, 255, 255, 255, 255},
    .ui8_field_number_of_digits = 2,
    .ui8_left_zero_paddig = 1,
  };

  if(m_lcd_vars.ui32_main_screen_draw_static_info)
  {
    UG_SetBackcolor(C_BLACK);
    UG_SetForecolor(MAIN_SCREEN_FIELD_LABELS_COLOR);
    UG_FontSelect(&FONT_10X16);
    UG_PutString(28, 244, "trip time");
  }

  if ((trip_time.ui8_minutes != trip_time_previous.ui8_minutes) ||
      m_lcd_vars.ui32_main_screen_draw_static_info)
  {
    trip_time_previous.ui8_hours = trip_time.ui8_hours;
    trip_time_previous.ui8_minutes = trip_time.ui8_minutes;

    // print hours number
    ui32_x_position = 21;
    ui32_y_position = 268;
    hours.ui32_x_position = ui32_x_position;
    hours.ui32_y_position = ui32_y_position;
    hours.ui32_number = trip_time.ui8_hours;
    hours.ui8_refresh_all_digits = m_lcd_vars.ui32_main_screen_draw_static_info;
    lcd_print_number(&hours);

    // print ":"
    ui32_x_position = hours.ui32_x_final_position;
    ui32_y_position = hours.ui32_y_final_position;
    UG_PutChar(58, ui32_x_position, ui32_y_position, C_WHITE, C_BLACK);
    ui32_x_position += minutes.font->char_width; // x width from ":"

    // print minutes number
    minutes.ui32_x_position = ui32_x_position;
    minutes.ui32_y_position = ui32_y_position;
    minutes.ui32_number = trip_time.ui8_minutes;
    minutes.ui8_refresh_all_digits = m_lcd_vars.ui32_main_screen_draw_static_info;
    lcd_print_number(&minutes);
  }
}

void trip_distance(void)
{
  uint32_t ui32_temp;
  uint16_t ui16_temp;
  static uint32_t ui32_trip_distance_previous = 0xffffffff;
  static uint32_t ui32_trip_distance;

  static print_number_t trip_distance =
  {
    .font = &FONT_24X40,
    .fore_color = C_WHITE,
    .back_color = C_BLACK,
    .ui32_x_position = 32,
    .ui32_y_position = 191,
    .ui8_previous_digits_array = {255, 255, 255, 255, 255},
    .ui8_field_number_of_digits = 4,
    .ui8_left_zero_paddig = 0,
    .ui32_number = 0,
    .ui8_refresh_all_digits = 1,
    .ui8_decimal_digits = 1
  };

  if(m_lcd_vars.ui32_main_screen_draw_static_info)
  {
    UG_SetBackcolor(C_BLACK);
    UG_SetForecolor(MAIN_SCREEN_FIELD_LABELS_COLOR);
    UG_FontSelect(&FONT_10X16);
    UG_PutString(8, 164, "trip distance");
  }

  // calculate how many revolutions since last reset and convert to distance traveled
  if(l2_vars.ui8_units_type == 0)
  {
    ui16_temp = l3_vars.ui16_wheel_perimeter;
  }
  else
  {
    ui16_temp = (l3_vars.ui16_wheel_perimeter_imperial_x10 * 254) / 10;
  }

  ui32_temp = (l3_vars.ui32_wheel_speed_sensor_tick_counter - l3_vars.ui32_wheel_speed_sensor_tick_counter_offset) * ((uint32_t) ui16_temp);

  // if traveled distance is more than 100 meters update all distance variables and reset
  if (ui32_temp >= 100000) // 100000 -> 100000 mm -> 0.1 km
  {
    // update all distance variables
    l3_vars.ui16_distance_since_power_on_x10 += 1;
    l3_vars.ui32_odometer_x10 += 1;
    l3_vars.ui32_trip_x10 += 1;

    // reset the always incrementing value (up to motor controller power reset) by setting the offset to current value
    l3_vars.ui32_wheel_speed_sensor_tick_counter_offset = l3_vars.ui32_wheel_speed_sensor_tick_counter;
  }

  ui32_trip_distance = l3_vars.ui16_distance_since_power_on_x10;

  // convert to imperial
  if(l3_vars.ui8_units_type)
  {
    ui32_trip_distance = (ui32_trip_distance * 10) / 16;
  }

  if((ui32_trip_distance != ui32_trip_distance_previous) ||
      m_lcd_vars.ui32_main_screen_draw_static_info)
  {
    ui32_trip_distance_previous = ui32_trip_distance;

    // print the number
    trip_distance.ui32_number = ui32_trip_distance;
//    trip_distance.ui8_refresh_all_digits = m_lcd_vars.ui32_main_screen_draw_static_info;
    trip_distance.ui8_refresh_all_digits = 1; // seems that decimal number needs always refresh other way there is an issue with print the "."
    lcd_print_number(&trip_distance);
    trip_distance.ui8_refresh_all_digits = 0;
  }
}

void power_off_management(void)
{
  if(buttons_get_onoff_long_click_event() &&
    m_lcd_vars.lcd_screen_state == LCD_SCREEN_MAIN &&
    buttons_get_up_state() == 0 &&
    buttons_get_down_state() == 0)
  {
    lcd_power_off();
  }
}

void lcd_power_off(void)
{
//  if (updateDistanceOdo)
//  {
    l3_vars.ui32_wh_x10_offset = l3_vars.ui32_wh_x10;
//    l3_vars.ui32_odometer_x10 += ((uint32_t) l3_vars.ui16_odometer_distance_x10);
//  }

  // save the variables on EEPROM
  eeprom_write_variables ();

  // put screen all black and disable backlight
  UG_FillScreen(0);
  lcd_set_backlight_intensity(0);

  // now disable the power to all the system
  system_power(0);

  // block here
  while(1) ;
}

void l2_low_pass_filter_battery_voltage_current_power(void)
{
  static uint32_t ui32_battery_voltage_accumulated_x10000 = 0;
  static uint16_t ui16_battery_current_accumulated_x5 = 0;

  // low pass filter battery voltage
  ui32_battery_voltage_accumulated_x10000 -= ui32_battery_voltage_accumulated_x10000 >> BATTERY_VOLTAGE_FILTER_COEFFICIENT;
  ui32_battery_voltage_accumulated_x10000 += (uint32_t) l2_vars.ui16_adc_battery_voltage * ADC_BATTERY_VOLTAGE_PER_ADC_STEP_X10000;
  l2_vars.ui16_battery_voltage_filtered_x10 = ((uint32_t) (ui32_battery_voltage_accumulated_x10000 >> BATTERY_VOLTAGE_FILTER_COEFFICIENT)) / 1000;

  // low pass filter batery current
  ui16_battery_current_accumulated_x5 -= ui16_battery_current_accumulated_x5 >> BATTERY_CURRENT_FILTER_COEFFICIENT;
  ui16_battery_current_accumulated_x5 += (uint16_t) l2_vars.ui8_battery_current_x5;
  l2_vars.ui16_battery_current_filtered_x5 = ui16_battery_current_accumulated_x5 >> BATTERY_CURRENT_FILTER_COEFFICIENT;

  // battery power
  l2_vars.ui16_battery_power_filtered_x50 = l2_vars.ui16_battery_current_filtered_x5 * l2_vars.ui16_battery_voltage_filtered_x10;
  l2_vars.ui16_battery_power_filtered = l2_vars.ui16_battery_power_filtered_x50 / 50;

  // loose resolution under 200W
  if(l2_vars.ui16_battery_power_filtered < 200)
  {
    l2_vars.ui16_battery_power_filtered /= 10;
    l2_vars.ui16_battery_power_filtered *= 10;
  }
  // loose resolution under 400W
  else if(l2_vars.ui16_battery_power_filtered < 400)
  {
    l2_vars.ui16_battery_power_filtered /= 20;
    l2_vars.ui16_battery_power_filtered *= 20;
  }
  // loose resolution all other values
  else
  {
    l2_vars.ui16_battery_power_filtered /= 25;
    l2_vars.ui16_battery_power_filtered *= 25;
  }
}

void l2_low_pass_filter_pedal_torque_and_power(void)
{
  static uint32_t ui32_pedal_torque_accumulated = 0;
  static uint32_t ui32_pedal_power_accumulated = 0;

  // low pass filter
  ui32_pedal_torque_accumulated -= ui32_pedal_torque_accumulated >> PEDAL_TORQUE_FILTER_COEFFICIENT;
  ui32_pedal_torque_accumulated += (uint32_t) l2_vars.ui16_pedal_torque_x10 / 10;
  l2_vars.ui16_pedal_torque_filtered = ((uint32_t) (ui32_pedal_torque_accumulated >> PEDAL_TORQUE_FILTER_COEFFICIENT));

  // low pass filter
  ui32_pedal_power_accumulated -= ui32_pedal_power_accumulated >> PEDAL_POWER_FILTER_COEFFICIENT;
  ui32_pedal_power_accumulated += (uint32_t) l2_vars.ui16_pedal_power_x10 / 10;
  l2_vars.ui16_pedal_power_filtered = ((uint32_t) (ui32_pedal_power_accumulated >> PEDAL_POWER_FILTER_COEFFICIENT));

  if(l2_vars.ui16_pedal_torque_filtered > 200)
  {
    l2_vars.ui16_pedal_torque_filtered /= 20;
    l2_vars.ui16_pedal_torque_filtered *= 20;
  }
  else if(l2_vars.ui16_pedal_torque_filtered > 100)
  {
    l2_vars.ui16_pedal_torque_filtered /= 10;
    l2_vars.ui16_pedal_torque_filtered *= 10;
  }
  else
  {
    // do nothing to original values
  }

  if(l2_vars.ui16_pedal_power_filtered > 500)
  {
    l2_vars.ui16_pedal_power_filtered /= 25;
    l2_vars.ui16_pedal_power_filtered *= 25;
  }
  else if(l2_vars.ui16_pedal_power_filtered > 200)
  {
    l2_vars.ui16_pedal_power_filtered /= 20;
    l2_vars.ui16_pedal_power_filtered *= 20;
  }
  else if(l2_vars.ui16_pedal_power_filtered > 10)
  {
    l2_vars.ui16_pedal_power_filtered /= 10;
    l2_vars.ui16_pedal_power_filtered *= 10;
  }
}

static void l2_low_pass_filter_pedal_cadence(void)
{
  static uint16_t ui16_pedal_cadence_accumulated = 0;

  // low pass filter
  ui16_pedal_cadence_accumulated -= (ui16_pedal_cadence_accumulated >> PEDAL_CADENCE_FILTER_COEFFICIENT);
  ui16_pedal_cadence_accumulated += (uint16_t) l2_vars.ui8_pedal_cadence;

  // consider the filtered value only for medium and high values of the unfiltered value
  if(l2_vars.ui8_pedal_cadence > 20)
  {
    l2_vars.ui8_pedal_cadence_filtered = (uint8_t) (ui16_pedal_cadence_accumulated >> PEDAL_CADENCE_FILTER_COEFFICIENT);
  }
  else
  {
    l2_vars.ui8_pedal_cadence_filtered = l2_vars.ui8_pedal_cadence;
  }
}

void l2_calc_wh(void)
{
  static uint8_t ui8_1s_timmer_counter = 0;
  uint32_t ui32_temp = 0;

  if(l2_vars.ui16_battery_power_filtered_x50 > 0)
  {
    l2_vars.ui32_wh_sum_x5 += l2_vars.ui16_battery_power_filtered_x50 / 10;
    l2_vars.ui32_wh_sum_counter++;
  }

  // calc at 1s rate
  if(ui8_1s_timmer_counter < 10)
  {
    ui8_1s_timmer_counter = 0;

    // avoid zero divisison
    if(l2_vars.ui32_wh_sum_counter != 0)
    {
      ui32_temp = l2_vars.ui32_wh_sum_counter / 36;
      ui32_temp = (ui32_temp * (l2_vars.ui32_wh_sum_x5 / l2_vars.ui32_wh_sum_counter)) / 500;
    }

    l2_vars.ui32_wh_x10 = l2_vars.ui32_wh_x10_offset + ui32_temp;
  }
  ui8_1s_timmer_counter++;
}

void l2_calc_odometer(void)
{
//  uint32_t uint32_temp;
//  static uint8_t ui8_1s_timmer_counter;
//
//  // calc at 1s rate
//  if (ui8_1s_timmer_counter++ >= 10)
//  {
//    ui8_1s_timmer_counter = 0;
//
//    uint32_temp = (uart_rx_vars.ui32_wheel_speed_sensor_tick_counter - l3_vars.ui32_wheel_speed_sensor_tick_counter_offset)
//        * ((uint32_t) configuration_variables.ui16_wheel_perimeter);
//    // avoid division by 0
//    if (uint32_temp > 100000) { uint32_temp /= 100000;}  // milimmeters to 0.1kms
//    else { uint32_temp = 0; }
//
//    // now store the value on the global variable
//    configuration_variables.ui16_odometer_distance_x10 = (uint16_t) uint32_temp;
//  }
}

static void automatic_power_off_management(void)
{
  static uint8_t ui8_lcd_power_off_time_counter_minutes = 0;
  static uint16_t ui16_lcd_power_off_time_counter = 0;

  if(l3_vars.ui8_lcd_power_off_time_minutes != 0)
  {
    // see if we should reset the automatic power off minutes counter
    if ((l3_vars.ui16_wheel_speed_x10 > 0) ||   // wheel speed > 0
        (l3_vars.ui8_battery_current_x5 > 0) || // battery current > 0
        (l3_vars.ui8_braking) ||                // braking
        buttons_get_events())                   // any button active
    {
      ui16_lcd_power_off_time_counter = 0;
      ui8_lcd_power_off_time_counter_minutes = 0;
    }

    // increment the automatic power off minutes counter
    ui16_lcd_power_off_time_counter++;

    // check if we should power off the LCD
    if(ui16_lcd_power_off_time_counter >= (50 * 60)) // 1 minute passed
    {
      ui16_lcd_power_off_time_counter = 0;

      ui8_lcd_power_off_time_counter_minutes++;
      if(ui8_lcd_power_off_time_counter_minutes >= l3_vars.ui8_lcd_power_off_time_minutes)
      {
        lcd_power_off();
      }
    }
  }
  // keep automatic_power_off_management disabled
  else
  {
    ui16_lcd_power_off_time_counter = 0;
    ui8_lcd_power_off_time_counter_minutes = 0;
  }
}

void update_menu_flashing_state(void)
{
  static uint8_t ui8_lcd_menu_counter_100ms = 0;
  static uint8_t ui8_lcd_menu_counter_1000ms = 0;

  // ***************************************************************************************************
  // For flashing on menus, 0.15 seconds flash
  if (ui8_lcd_menu_flash_counter++ > 15)
  {
    ui8_lcd_menu_flash_counter = 0;

    if(ui8_lcd_menu_flash_state)
      ui8_lcd_menu_flash_state = 0;
    else
      ui8_lcd_menu_flash_state = 1;
  }
  // ***************************************************************************************************

  // ***************************************************************************************************
  ui8_lcd_menu_counter_100ms_state = 0;
  if (ui8_lcd_menu_counter_100ms++ > 5)
  {
    ui8_lcd_menu_counter_100ms = 0;
    ui8_lcd_menu_counter_100ms_state = 1;
  }

  // disable trigger signal
  if (m_lcd_vars.ui8_lcd_menu_counter_1000ms_trigger) { m_lcd_vars.ui8_lcd_menu_counter_1000ms_trigger = 0; }

  if(m_lcd_vars.ui8_lcd_menu_counter_1000ms_state)
  {
    if(ui8_lcd_menu_counter_1000ms++ > 40)
    {
      ui8_lcd_menu_counter_1000ms = 0;
      m_lcd_vars.ui8_lcd_menu_counter_1000ms_state = 0;
      m_lcd_vars.ui8_lcd_menu_counter_1000ms_trigger = 1;
    }
  }
  else
  {
    if(ui8_lcd_menu_counter_1000ms++ > 10)
    {
      ui8_lcd_menu_counter_1000ms = 0;
      m_lcd_vars.ui8_lcd_menu_counter_1000ms_state = 1;
      m_lcd_vars.ui8_lcd_menu_counter_1000ms_trigger = 2;
    }
  }
  // ***************************************************************************************************

//  // ***************************************************************************************************
//  // For flashing the temperature field when the current is being limited due to motor over temperature
//  // flash only if current is being limited: ui8_temperature_current_limiting_value != 255
//  if (l3_vars.ui8_temperature_current_limiting_value != 255)
//  {
//    if (ui8_lcd_menu_flash_state_temperature == 0) // state 0: disabled
//    {
//      if (ui16_lcd_menu_flash_counter_temperature > 0)
//      {
//        ui16_lcd_menu_flash_counter_temperature--;
//      }
//
//      if (ui16_lcd_menu_flash_counter_temperature == 0)
//      {
//        // if l3_vars.ui8_temperature_current_limiting_value == 0, flash quicker meaning motor is shutoff
//        if (l3_vars.ui8_temperature_current_limiting_value > 0)
//        {
//          ui16_lcd_menu_flash_counter_temperature = 50 + ((uint16_t) l3_vars.ui8_temperature_current_limiting_value);
//        }
//        else
//        {
//          ui16_lcd_menu_flash_counter_temperature = 25;
//        }
//
//        ui8_lcd_menu_flash_state_temperature = 1;
//      }
//    }
//
//    if (ui8_lcd_menu_flash_state_temperature == 1) // state 1: enabled
//    {
//      if (ui16_lcd_menu_flash_counter_temperature > 0)
//      {
//        ui16_lcd_menu_flash_counter_temperature--;
//      }
//
//      if (ui16_lcd_menu_flash_counter_temperature == 0)
//      {
//        ui16_lcd_menu_flash_counter_temperature = 25; // 0.25 second
//        ui8_lcd_menu_flash_state_temperature = 0;
//      }
//    }
//  }
//  else
//  {
//    ui8_lcd_menu_flash_state_temperature = 1;
//  }
//  // ***************************************************************************************************
}

void brake(void)
{
  static uint8_t ui8_braking_previous;
  uint32_t ui32_x1;
  uint32_t ui32_y1;
  uint32_t ui32_x2;
  uint32_t ui32_y2;

  // if previous state was disable, draw
  if((l3_vars.ui8_braking != ui8_braking_previous) ||
      (m_lcd_vars.ui32_main_screen_draw_static_info))
  {
    ui8_braking_previous = l3_vars.ui8_braking;

    if(l3_vars.ui8_braking)
    {
      UG_SetBackcolor(C_BLACK);
      UG_SetForecolor(C_WHITE);
      UG_FontSelect(&SMALL_TEXT_FONT);
      ui32_x1 = 190;
      ui32_y1 = 12;
      UG_PutString(ui32_x1, ui32_y1, "B");
    }
    else
    {
      // clear area
      // 1 leters
      ui32_x1 = 190;
      ui32_y1 = 12;
      ui32_x2 = ui32_x1 + ((1 * 10) + (1 * 1) + 1);
      ui32_y2 = ui32_y1 + 16;
      UG_FillFrame(ui32_x1, ui32_y1, ui32_x2, ui32_y2, C_BLACK);
    }
  }
}

void lcd_set_backlight_intensity(uint8_t ui8_intensity)
{
  // force to be min of 20% and max of 100%
  if(ui8_intensity < 4)
  {
    ui8_intensity = 4;
  }
  else if(ui8_intensity > 20)
  {
    ui8_intensity = 20;
  }

  TIM_SetCompare2(TIM3, ((uint16_t) ui8_intensity) * 2000);
  TIM_CtrlPWMOutputs(TIM3, ENABLE);
}

void lights_state(void)
{
  static uint8_t ui8_lights_previous;
  uint32_t ui32_x1;
  uint32_t ui32_y1;
  uint32_t ui32_x2;
  uint32_t ui32_y2;

  if(buttons_get_up_long_click_event())
  {
    buttons_clear_up_long_click_event();

    if(l3_vars.ui8_lights == 0)
    {
      l3_vars.ui8_lights = 1;
    }
    else
    {
      l3_vars.ui8_lights = 0;
    }
  }

  if(l3_vars.ui8_lights == 0)
  {
    lcd_set_backlight_intensity(l3_vars.ui8_lcd_backlight_off_brightness);
  }
  else
  {
    lcd_set_backlight_intensity(l3_vars.ui8_lcd_backlight_on_brightness);
  }

  // if previous state was disable, draw
  if((l3_vars.ui8_lights != ui8_lights_previous) ||
      (m_lcd_vars.ui32_main_screen_draw_static_info))
  {
    ui8_lights_previous = l3_vars.ui8_lights;

    if(l3_vars.ui8_lights)
    {
      UG_SetBackcolor(C_BLACK);
      UG_SetForecolor(C_WHITE);
      UG_FontSelect(&SMALL_TEXT_FONT);
      ui32_x1 = 205;
      ui32_y1 = 12;
      UG_PutString(ui32_x1, ui32_y1, "L");
    }
    else
    {
      // clear area
      // 1 leters
      ui32_x1 = 205;
      ui32_y1 = 12;
      ui32_x2 = ui32_x1 + ((1 * 10) + (1 * 1) + 1);
      ui32_y2 = ui32_y1 + 16;
      UG_FillFrame(ui32_x1, ui32_y1, ui32_x2, ui32_y2, C_BLACK);
    }
  }
}

void l2_calc_battery_voltage_soc(void)
{
  uint16_t ui16_fluctuate_battery_voltage_x10;

  // calculate flutuate voltage, that depends on the current and battery pack resistance
  ui16_fluctuate_battery_voltage_x10 = (uint16_t) ((((uint32_t) l2_vars.ui16_battery_pack_resistance_x1000) * ((uint32_t) l2_vars.ui16_battery_current_filtered_x5)) / ((uint32_t) 500));
  // now add fluctuate voltage value
  l2_vars.ui16_battery_voltage_soc_x10 = l2_vars.ui16_battery_voltage_filtered_x10 + ui16_fluctuate_battery_voltage_x10;
}

void battery_soc_bar_clear(uint32_t ui32_bar_number)
{
  uint32_t ui32_x1, ui32_x2;
  uint32_t ui32_y1, ui32_y2;

  // the first nine bars share the same code
  if (ui32_bar_number < 10)
  {
    // draw the bar itself
    ui32_x1 = BATTERY_SOC_START_X + BATTERY_SOC_CONTOUR + ((BATTERY_SOC_BAR_WITH + BATTERY_SOC_CONTOUR + 1) * (ui32_bar_number - 1));
    ui32_y1 = BATTERY_SOC_START_Y + BATTERY_SOC_CONTOUR;
    ui32_x2 = ui32_x1 + BATTERY_SOC_BAR_WITH;
    ui32_y2 = ui32_y1 + BATTERY_SOC_BAR_HEIGHT;
    UG_FillFrame(ui32_x1, ui32_y1, ui32_x2, ui32_y2, C_BLACK);

    // draw bar contour
    if(ui32_bar_number < 9)
    {
      ui32_x1 = ui32_x2 + BATTERY_SOC_CONTOUR;
      UG_DrawLine(ui32_x1, ui32_y1, ui32_x1, ui32_y2, C_BLACK);
    }
    else
    {
      ui32_x1 = ui32_x2 + 1;
      ui32_y1 = BATTERY_SOC_START_Y + BATTERY_SOC_CONTOUR + (BATTERY_SOC_BAR_HEIGHT / 4);
      ui32_y2 = ui32_y1 + (BATTERY_SOC_BAR_HEIGHT / 2);
      UG_DrawLine(ui32_x1, ui32_y1, ui32_x1, ui32_y2, C_BLACK);
    }
  }
  else
  {
    ui32_x1 = BATTERY_SOC_START_X + BATTERY_SOC_CONTOUR + ((BATTERY_SOC_BAR_WITH + BATTERY_SOC_CONTOUR + 1) * 9);
    ui32_y1 = BATTERY_SOC_START_Y + BATTERY_SOC_CONTOUR + (BATTERY_SOC_BAR_HEIGHT / 4);
    ui32_x2 = ui32_x1 + BATTERY_SOC_BAR_WITH;
    ui32_y2 = ui32_y1 + (BATTERY_SOC_BAR_HEIGHT / 2);
    UG_FillFrame(ui32_x1, ui32_y1, ui32_x2, ui32_y2, C_BLACK);
  }
}

void battery_soc_bar_set(uint32_t ui32_bar_number, uint16_t ui16_color)
{
  uint32_t ui32_x1, ui32_x2;
  uint32_t ui32_y1, ui32_y2;

  // the first nine bars share the same code
  if(ui32_bar_number < 10)
  {
    ui32_x1 = BATTERY_SOC_START_X + BATTERY_SOC_CONTOUR + ((BATTERY_SOC_BAR_WITH + BATTERY_SOC_CONTOUR + 1) * (ui32_bar_number - 1));
    ui32_y1 = BATTERY_SOC_START_Y + BATTERY_SOC_CONTOUR;
    ui32_x2 = ui32_x1 + BATTERY_SOC_BAR_WITH;
    ui32_y2 = ui32_y1 + BATTERY_SOC_BAR_HEIGHT;
    UG_FillFrame(ui32_x1, ui32_y1, ui32_x2, ui32_y2, ui16_color);

    if(ui32_bar_number < 9)
    {
      ui32_x1 = ui32_x2 + 1;
      UG_DrawLine(ui32_x1, ui32_y1, ui32_x1, ui32_y2, C_DIM_GRAY);
    }
    else
    {
      ui32_x1 = ui32_x2 + 1;
      ui32_y1 = BATTERY_SOC_START_Y + BATTERY_SOC_CONTOUR + (BATTERY_SOC_BAR_HEIGHT / 4);
      ui32_y2 = ui32_y1 + (BATTERY_SOC_BAR_HEIGHT / 2);
      UG_DrawLine(ui32_x1, ui32_y1, ui32_x1, ui32_y2, C_DIM_GRAY);
    }
  }
  else
  {
    ui32_x1 = BATTERY_SOC_START_X + BATTERY_SOC_CONTOUR + ((BATTERY_SOC_BAR_WITH + BATTERY_SOC_CONTOUR + 1) * 9);
    ui32_y1 = BATTERY_SOC_START_Y + BATTERY_SOC_CONTOUR + (BATTERY_SOC_BAR_HEIGHT / 4);
    ui32_x2 = ui32_x1 + BATTERY_SOC_BAR_WITH;
    ui32_y2 = ui32_y1 + (BATTERY_SOC_BAR_HEIGHT / 2);
    UG_FillFrame(ui32_x1, ui32_y1, ui32_x2, ui32_y2, ui16_color);
  }
}

void battery_soc(void)
{
  uint32_t ui32_x1, ui32_x2;
  uint32_t ui32_y1, ui32_y2;
  static uint8_t ui8_timmer_counter;
  uint32_t ui32_battery_bar_number;
  static uint32_t ui32_battery_bar_number_previous = 0;
  uint32_t ui32_battery_bar_number_offset;
  uint32_t ui32_battery_cells_number_x10;
  uint16_t ui16_color;
  static uint16_t ui16_color_previous;
  uint32_t ui32_temp;
  uint32_t ui32_i;
  static uint16_t ui16_battery_voltage_filtered_x10_previous = 0xffff;
  uint32_t ui32_value_temp;
  uint32_t ui32_value_integer;
  uint32_t ui32_value_decimal;
  uint32_t ui32_value_integer_number_digits;
  uint8_t ui8_counter;
  static uint16_t ui16_battery_soc_watts_hour_previous = 0xffff;

  static print_number_t soc =
  {
    .font = &REGULAR_TEXT_FONT,
    .fore_color = C_WHITE,
    .back_color = C_BLACK,
    .ui8_previous_digits_array = {255, 255, 255, 255, 255},
    .ui8_field_number_of_digits = 3,
    .ui8_left_zero_paddig = 0,
    .ui8_left_paddig = 1
  };

  if(m_lcd_vars.ui32_main_screen_draw_static_info)
  {
    // first, clear the full symbol area
    // first 9 bars
    ui32_x1 = BATTERY_SOC_START_X;
    ui32_y1 = BATTERY_SOC_START_Y;
    ui32_x2 = ui32_x1 + ((BATTERY_SOC_BAR_WITH + BATTERY_SOC_CONTOUR) * 9) + (BATTERY_SOC_CONTOUR * 2);
    ui32_y2 = ui32_y1 + BATTERY_SOC_BAR_HEIGHT + (BATTERY_SOC_CONTOUR * 2);
    UG_FillFrame(ui32_x1, ui32_y1, ui32_x2, ui32_y2, C_BLACK);

    // last small bar
    ui32_x1 = ui32_x2;
    ui32_y1 = BATTERY_SOC_START_Y + (BATTERY_SOC_BAR_HEIGHT / 4);
    ui32_x2 = ui32_x1 + BATTERY_SOC_BAR_WITH + (BATTERY_SOC_CONTOUR * 2);
    ui32_y2 = ui32_y1 + (BATTERY_SOC_BAR_HEIGHT / 2) + (BATTERY_SOC_CONTOUR * 2);
    UG_FillFrame(ui32_x1, ui32_y1, ui32_x2, ui32_y2, C_BLACK);

    // now draw the empty battery symbol
    // first 9 bars
    ui32_x1 = BATTERY_SOC_START_X;
    ui32_y1 = BATTERY_SOC_START_Y;
    ui32_x2 = ui32_x1 + ((BATTERY_SOC_BAR_WITH + BATTERY_SOC_CONTOUR + 1) * 9) + (BATTERY_SOC_CONTOUR * 2) - 2;
    ui32_y2 = ui32_y1;
    UG_DrawLine(ui32_x1, ui32_y1, ui32_x2, ui32_y2, C_WHITE);

    // last bar
    ui32_x1 = ui32_x2;
    ui32_y1 = ui32_y2;
    ui32_x2 = ui32_x1;
    ui32_y2 = ui32_y1 + (BATTERY_SOC_BAR_HEIGHT / 4);
    UG_DrawLine(ui32_x1, ui32_y1, ui32_x2, ui32_y2, C_WHITE);

    ui32_x1 = ui32_x2;
    ui32_y1 = ui32_y2;
    ui32_x2 = ui32_x1 + BATTERY_SOC_BAR_WITH + BATTERY_SOC_CONTOUR + 1;
    ui32_y2 = ui32_y1;
    UG_DrawLine(ui32_x1, ui32_y1, ui32_x2, ui32_y2, C_WHITE);

    ui32_x1 = ui32_x2;
    ui32_y1 = ui32_y2;
    ui32_x2 = ui32_x1;
    ui32_y2 = ui32_y1 + (BATTERY_SOC_BAR_HEIGHT / 2) + (BATTERY_SOC_CONTOUR * 2);
    UG_DrawLine(ui32_x1, ui32_y1, ui32_x2, ui32_y2, C_WHITE);

    ui32_x1 = ui32_x2;
    ui32_y1 = ui32_y2;
    ui32_x2 = ui32_x1 - (BATTERY_SOC_BAR_WITH + BATTERY_SOC_CONTOUR + 1);
    ui32_y2 = ui32_y1;
    UG_DrawLine(ui32_x1, ui32_y1, ui32_x2, ui32_y2, C_WHITE);

    ui32_x1 = ui32_x2;
    ui32_y1 = ui32_y2;
    ui32_x2 = ui32_x1;
    ui32_y2 = ui32_y1 + (BATTERY_SOC_BAR_HEIGHT / 4);
    UG_DrawLine(ui32_x1, ui32_y1, ui32_x2, ui32_y2, C_WHITE);

    ui32_x1 = ui32_x2;
    ui32_y1 = ui32_y2;
    ui32_x2 = ui32_x1 - (((BATTERY_SOC_BAR_WITH + BATTERY_SOC_CONTOUR + 1) * 9) + (BATTERY_SOC_CONTOUR * 2) - 2);
    ui32_y2 = ui32_y1;
    UG_DrawLine(ui32_x1, ui32_y1, ui32_x2, ui32_y2, C_WHITE);

    ui32_x1 = ui32_x2;
    ui32_y1 = ui32_y2;
    ui32_x2 = ui32_x1;
    ui32_y2 = ui32_y1 - (BATTERY_SOC_BAR_HEIGHT + BATTERY_SOC_CONTOUR);
    UG_DrawLine(ui32_x1, ui32_y1, ui32_x2, ui32_y2, C_WHITE);
  }

  // update battery level value only at every 1 second and this helps to visual filter the fast changing values
  if((ui8_timmer_counter++ >= 50) ||
      (m_lcd_vars.ui32_main_screen_draw_static_info))
  {
    ui8_timmer_counter = 0;

    // to keep same scale as voltage of x10
    ui32_battery_cells_number_x10 = (uint32_t) (l3_vars.ui8_battery_cells_number * 10);

    if(l3_vars.ui16_battery_voltage_soc_x10 > ((uint16_t) ((float) ui32_battery_cells_number_x10 * LI_ION_CELL_VOLTS_90))) { ui32_battery_bar_number = 10; }
    else if(l3_vars.ui16_battery_voltage_soc_x10 > ((uint16_t) ((float) ui32_battery_cells_number_x10 * LI_ION_CELL_VOLTS_80))) { ui32_battery_bar_number = 9; }
    else if(l3_vars.ui16_battery_voltage_soc_x10 > ((uint16_t) ((float) ui32_battery_cells_number_x10 * LI_ION_CELL_VOLTS_70))) { ui32_battery_bar_number = 8; }
    else if(l3_vars.ui16_battery_voltage_soc_x10 > ((uint16_t) ((float) ui32_battery_cells_number_x10 * LI_ION_CELL_VOLTS_60))) { ui32_battery_bar_number = 7; }
    else if(l3_vars.ui16_battery_voltage_soc_x10 > ((uint16_t) ((float) ui32_battery_cells_number_x10 * LI_ION_CELL_VOLTS_50))) { ui32_battery_bar_number = 6; }
    else if(l3_vars.ui16_battery_voltage_soc_x10 > ((uint16_t) ((float) ui32_battery_cells_number_x10 * LI_ION_CELL_VOLTS_40))) { ui32_battery_bar_number = 5; }
    else if(l3_vars.ui16_battery_voltage_soc_x10 > ((uint16_t) ((float) ui32_battery_cells_number_x10 * LI_ION_CELL_VOLTS_30))) { ui32_battery_bar_number = 4; }
    else if(l3_vars.ui16_battery_voltage_soc_x10 > ((uint16_t) ((float) ui32_battery_cells_number_x10 * LI_ION_CELL_VOLTS_20))) { ui32_battery_bar_number = 3; }
    else if(l3_vars.ui16_battery_voltage_soc_x10 > ((uint16_t) ((float) ui32_battery_cells_number_x10 * LI_ION_CELL_VOLTS_10))) { ui32_battery_bar_number = 2; }
    else if(l3_vars.ui16_battery_voltage_soc_x10 > ((uint16_t) ((float) ui32_battery_cells_number_x10 * LI_ION_CELL_VOLTS_0))) { ui32_battery_bar_number = 1; }
    else { ui32_battery_bar_number = 0; }

    // find the color to draw the bars
    if(ui32_battery_bar_number > 3) { ui16_color = C_GREEN; }
    else if(ui32_battery_bar_number == 3) { ui16_color = C_YELLOW; }
    else if(ui32_battery_bar_number == 2) { ui16_color = C_ORANGE; }
    else if(ui32_battery_bar_number == 1) { ui16_color = C_RED; }

    // force draw of the bars if needed
    if(m_lcd_vars.ui32_main_screen_draw_static_info)
    {
      ui32_battery_bar_number_previous = 0;
    }

    // number of vars are equal as before, nothing new to draw so return
    if(ui32_battery_bar_number == ui32_battery_bar_number_previous)
    {
      // do nothing
    }
    // draw new bars
    else if(ui32_battery_bar_number > ui32_battery_bar_number_previous)
    {
      // we need to redraw the total number of bars
      if(ui16_color != ui16_color_previous)
      {
        for(ui32_i = 1; ui32_i <= ui32_battery_bar_number; ui32_i++)
        {
          battery_soc_bar_set(ui32_i, ui16_color);
        }
      }
      else
      {
        ui32_temp = (ui32_battery_bar_number - ui32_battery_bar_number_previous) + 1;
        for(ui32_i = 1; ui32_i < ui32_temp; ui32_i++)
        {
          battery_soc_bar_set(ui32_battery_bar_number_previous + ui32_i, ui16_color);
        }
      }
    }
    // delete bars
    else if(ui32_battery_bar_number < ui32_battery_bar_number_previous)
    {
      // we need to redraw the total number of bars
      if(ui16_color != ui16_color_previous)
      {
        // first deleted the needed number of vars
        ui32_temp = ui32_battery_bar_number_previous - ui32_battery_bar_number;
        for(ui32_i = 0; ui32_i <= (ui32_temp - 1); ui32_i++)
        {
          battery_soc_bar_clear(ui32_battery_bar_number_previous - ui32_i);
        }

        // now draw the new ones with the new color
        for(ui32_i = 1; ui32_i <= ui32_battery_bar_number; ui32_i++)
        {
          battery_soc_bar_set(ui32_i, ui16_color);
        }
      }
      else
      {
        ui32_temp = ui32_battery_bar_number_previous - ui32_battery_bar_number;
        for(ui32_i = 0; ui32_i <= (ui32_temp - 1); ui32_i++)
        {
          battery_soc_bar_clear(ui32_battery_bar_number_previous - ui32_i);
        }
      }
    }

    ui32_battery_bar_number_previous = ui32_battery_bar_number;
    ui16_color_previous = ui16_color;

    // draw SOC in percentage
    if((ui16_m_battery_soc_watts_hour != ui16_battery_soc_watts_hour_previous) ||
        (m_lcd_vars.ui32_main_screen_draw_static_info))
    {
      soc.ui32_x_position = BATTERY_SOC_START_X + ((BATTERY_SOC_BAR_WITH + BATTERY_SOC_CONTOUR + 1) * 10) + (BATTERY_SOC_CONTOUR * 2) + 10;
      soc.ui32_y_position = 6;

      // clean full area because it lcd_print_number() with left padding can't deal with that
      UG_SetBackcolor(C_BLACK);
      UG_SetForecolor(C_WHITE);
      UG_FontSelect(&REGULAR_TEXT_FONT);
      UG_PutString(soc.ui32_x_position, soc.ui32_y_position, "    ");

      ui16_battery_soc_watts_hour_previous = ui16_m_battery_soc_watts_hour;
      soc.ui32_x_position = BATTERY_SOC_START_X + ((BATTERY_SOC_BAR_WITH + BATTERY_SOC_CONTOUR + 1) * 10) + (BATTERY_SOC_CONTOUR * 2) + 10;
      soc.ui32_y_position = 6;
      soc.ui32_number = ui16_m_battery_soc_watts_hour;
      soc.ui8_refresh_all_digits = 1;
      lcd_print_number(&soc);

      ui32_x1 = soc.ui32_x_final_position + 2;
      ui32_y1 = soc.ui32_y_final_position + 7;
      UG_SetBackcolor(C_BLACK);
      UG_SetForecolor(C_WHITE);
      UG_FontSelect(&SMALL_TEXT_FONT);
      UG_PutString(ui32_x1, ui32_y1, "%");
    }
  }
}

void temperature(void)
{
  static uint8_t ui8_motor_temperature_previous;
  uint32_t ui32_x1;
  uint32_t ui32_y1;
  uint32_t ui32_x2;
  uint32_t ui32_y2;
  uint8_t ui8_ascii_degree = 176;

  if(l3_vars.ui8_temperature_limit_feature_enabled)
  {
    if((l3_vars.ui8_motor_temperature != ui8_motor_temperature_previous) ||
        (m_lcd_vars.ui32_main_screen_draw_static_info))
    {
      ui8_motor_temperature_previous = l3_vars.ui8_motor_temperature;

      // first clear the area
      // 5 digits + some space
      ui32_x1 = DISPLAY_WIDTH - 1 - 18 - (7 * 10) + (7 * 1) + 10;
      ui32_y1 = 32;
      ui32_x2 = ui32_x1 + (7 * 10) + (7 * 1) + 10;
      ui32_y2 = ui32_y1 + 18;
      UG_FillFrame(ui32_x1, ui32_y1, ui32_x2, ui32_y2, C_BLACK);

      // draw
      UG_SetBackcolor(C_BLACK);
      UG_SetForecolor(C_WHITE);
      UG_FontSelect(&SMALL_TEXT_FONT);
      ui32_x1 = DISPLAY_WIDTH - 1 - 18 - (7 * 10) + (7 * 1) + 10;
      ui32_y1 = 32;

      if(l3_vars.ui8_motor_temperature < 10)
      {
        ui32_x1 += 22;
        UG_PutString(ui32_x1, ui32_y1, itoa(l3_vars.ui8_motor_temperature));

        ui32_x1 += ((1 * 10) + (1 * 1) + 1);
        UG_PutString(ui32_x1, ui32_y1, &ui8_ascii_degree);
        ui32_x1 += 11;
        UG_PutString(ui32_x1, ui32_y1, "c");
      }
      else if(l3_vars.ui8_motor_temperature < 100)
      {
        ui32_x1 += 11;
        UG_PutString(ui32_x1, ui32_y1, itoa(l3_vars.ui8_motor_temperature));

        ui32_x1 += ((2 * 10) + (2 * 1) + 1);
        UG_PutString(ui32_x1, ui32_y1, &ui8_ascii_degree);
        ui32_x1 += 11;
        UG_PutString(ui32_x1, ui32_y1, "c");
      }
      else
      {
        UG_PutString(ui32_x1, ui32_y1, itoa(l3_vars.ui8_motor_temperature));

        ui32_x1 += ((3 * 10) + (3 * 1) + 1);
        UG_PutString(ui32_x1, ui32_y1, &ui8_ascii_degree);
        ui32_x1 += 11;
        UG_PutString(ui32_x1, ui32_y1, "c");
      }
    }
  }
}

void time(void)
{
  uint32_t ui32_x_position;
  uint32_t ui32_y_position;
  static struct_rtc_time_t rtc_time_previous;
  static struct_rtc_time_t *p_rtc_time_previous;
  struct_rtc_time_t *p_rtc_time;

  static print_number_t hours =
  {
    .font = &REGULAR_TEXT_FONT,
    .fore_color = C_WHITE,
    .back_color = C_BLACK,
    .ui8_previous_digits_array = {255, 255, 255, 255, 255},
    .ui8_field_number_of_digits = 2,
    .ui8_left_zero_paddig = 0,
  };

  static print_number_t minutes =
  {
    .font = &REGULAR_TEXT_FONT,
    .fore_color = C_WHITE,
    .back_color = C_BLACK,
    .ui8_previous_digits_array = {255, 255, 255, 255, 255},
    .ui8_field_number_of_digits = 2,
    .ui8_left_zero_paddig = 1,
  };

  p_rtc_time_previous = &rtc_time_previous;
  p_rtc_time = rtc_get_time();

  // force to be [0 - 12]
  if(l3_vars.ui8_units_type)
  {
    if(p_rtc_time->ui8_hours > 12)
    {
      p_rtc_time->ui8_hours -= 12;
    }
  }

  if ((p_rtc_time->ui8_hours != p_rtc_time_previous->ui8_hours) ||
      (p_rtc_time->ui8_minutes != p_rtc_time_previous->ui8_minutes) ||
      m_lcd_vars.ui32_main_screen_draw_static_info)
  {
    p_rtc_time_previous->ui8_hours = p_rtc_time->ui8_hours;
    p_rtc_time_previous->ui8_minutes = p_rtc_time->ui8_minutes;

    // print hours number
    ui32_x_position = DISPLAY_WIDTH - 1 - hours.font->char_width - (5 * hours.font->char_width) + (5 * 1);
    ui32_y_position = 6;
    hours.ui32_x_position = ui32_x_position;
    hours.ui32_y_position = ui32_y_position;
    hours.ui32_number = p_rtc_time->ui8_hours;
    hours.ui8_refresh_all_digits = m_lcd_vars.ui32_main_screen_draw_static_info;
    lcd_print_number(&hours);

    // print ":"
    ui32_x_position = hours.ui32_x_final_position;
    ui32_y_position = hours.ui32_y_final_position;
    UG_PutChar(58, ui32_x_position, ui32_y_position, C_WHITE, C_BLACK);
    ui32_x_position += minutes.font->char_width; // x width from ":"

    // print minutes number
    minutes.ui32_x_position = ui32_x_position;
    minutes.ui32_y_position = ui32_y_position;
    minutes.ui32_number = p_rtc_time->ui8_minutes;
    minutes.ui8_refresh_all_digits = m_lcd_vars.ui32_main_screen_draw_static_info;
    lcd_print_number(&minutes);
  }
}

void power(void)
{
  static uint16_t ui16_battery_power_filtered_previous;
  uint32_t ui32_x1;
  uint32_t ui32_y1;
  uint32_t ui32_x2;
  uint32_t ui32_y2;
  static uint8_t ui8_target_max_battery_power_state = 0;
  uint16_t _ui16_battery_power_filtered;
  uint16_t ui16_target_max_power;

  static print_number_t power_number =
  {
    .font = &FONT_24X40,
    .fore_color = C_WHITE,
    .back_color = C_BLACK,
    .ui32_x_position = 191,
    .ui32_y_position = 191,
    .ui8_previous_digits_array = {255, 255, 255, 255, 255},
    .ui8_field_number_of_digits = 4,
    .ui8_left_zero_paddig = 0,
    .ui32_number = 0,
    .ui8_refresh_all_digits = 1
  };

  if(m_lcd_vars.ui32_main_screen_draw_static_info)
  {
    UG_SetBackcolor(C_BLACK);
    UG_SetForecolor(MAIN_SCREEN_FIELD_LABELS_COLOR);
    UG_FontSelect(&FONT_10X16);
    UG_PutString(183, 164, "motor power");
  }

  if(m_lcd_vars.main_screen_state == MAIN_SCREEN_STATE_MAIN)
  {
    _ui16_battery_power_filtered = l3_vars.ui16_battery_power_filtered;

    if((_ui16_battery_power_filtered != ui16_battery_power_filtered_previous) ||
        m_lcd_vars.ui32_main_screen_draw_static_info ||
        ui8_target_max_battery_power_state == 0)
    {
      ui16_battery_power_filtered_previous = _ui16_battery_power_filtered;
      ui8_target_max_battery_power_state = 1;

      if (_ui16_battery_power_filtered > 9999) { _ui16_battery_power_filtered = 9999; }

      power_number.ui32_number = _ui16_battery_power_filtered;
      power_number.ui8_refresh_all_digits = m_lcd_vars.ui32_main_screen_draw_static_info;
      lcd_print_number(&power_number);
      power_number.ui8_refresh_all_digits = 0;
    }
    else
    {

    }
  }
  else if(m_lcd_vars.main_screen_state == MAIN_SCREEN_STATE_POWER)
  {
    // leave this menu with a button_onoff_long_click
    if(buttons_get_onoff_long_click_event())
    {
      buttons_clear_all_events();
      m_lcd_vars.main_screen_state = MAIN_SCREEN_STATE_MAIN;
      ui8_target_max_battery_power_state = 0;
      power_number.ui8_refresh_all_digits = 1;

      // save the updated variables on EEPROM
      eeprom_write_variables();
      return;
    }

    if(buttons_get_up_click_event())
    {
      buttons_clear_all_events();

      if(l3_vars.ui8_target_max_battery_power < 10)
      {
        l3_vars.ui8_target_max_battery_power++;
      }
      else
      {
        l3_vars.ui8_target_max_battery_power += 2;
      }

      // limit to 100 * 25 = 2500 Watts
      if(l3_vars.ui8_target_max_battery_power > 100) { l3_vars.ui8_target_max_battery_power = 100; }
    }

    if(buttons_get_down_click_event())
    {
      buttons_clear_all_events();

      if(l3_vars.ui8_target_max_battery_power == 0)
      {

      }
      else if(l3_vars.ui8_target_max_battery_power <= 10)
      {
        l3_vars.ui8_target_max_battery_power--;
      }
      else
      {
        l3_vars.ui8_target_max_battery_power -= 2;
      }
    }

    if(ui8_lcd_menu_flash_state)
    {
      if(ui8_target_max_battery_power_state == 1)
      {
        ui8_target_max_battery_power_state = 0;

        // clear area
        power_number.ui8_clean_area_all_digits = 1;
        lcd_print_number(&power_number);
        power_number.ui8_clean_area_all_digits = 0;
      }
    }
    else
    {
      if(ui8_target_max_battery_power_state == 0)
      {
        ui8_target_max_battery_power_state = 1;

        ui16_target_max_power = l3_vars.ui8_target_max_battery_power * 25;

        power_number.ui8_refresh_all_digits = 1;
        power_number.ui32_number = ui16_target_max_power;
        lcd_print_number(&power_number);

        l3_vars.ui8_target_max_battery_power = ui16_target_max_power / 25;
      }
    }
  }
}

void pedal_human_power(void)
{
  static uint16_t ui16_pedal_power_previous = 0;
  uint32_t ui32_x1;
  uint32_t ui32_y1;
  uint32_t ui32_x2;
  uint32_t ui32_y2;
  uint16_t ui16_pedal_power;

  static print_number_t power_number =
  {
    .font = &FONT_24X40,
    .fore_color = C_WHITE,
    .back_color = C_BLACK,
    .ui32_x_position = 191,
    .ui32_y_position = 268,
    .ui8_previous_digits_array = {255, 255, 255, 255, 255},
    .ui8_field_number_of_digits = 4,
    .ui8_left_zero_paddig = 0,
    .ui32_number = 0,
    .ui8_refresh_all_digits = 1
  };

  if(m_lcd_vars.ui32_main_screen_draw_static_info)
  {
    UG_SetBackcolor(C_BLACK);
    UG_SetForecolor(MAIN_SCREEN_FIELD_LABELS_COLOR);
    UG_FontSelect(&FONT_10X16);
    UG_PutString(178, 244, "human power");
  }

  ui16_pedal_power = l3_vars.ui16_pedal_power_filtered;

  if((ui16_pedal_power != ui16_pedal_power_previous) ||
      m_lcd_vars.ui32_main_screen_draw_static_info)
  {
    ui16_pedal_power_previous = ui16_pedal_power;

    power_number.ui32_number = ui16_pedal_power;
    power_number.ui8_refresh_all_digits = m_lcd_vars.ui32_main_screen_draw_static_info;
    lcd_print_number(&power_number);
    power_number.ui8_refresh_all_digits = 0;
  }
  else
  {

  }
}

void wheel_speed(void)
{
  static uint16_t ui16_wheel_x10_speed_previous = 0xffff;

  static print_number_t wheel_speed_integer =
  {
    .font = &FONT_61X99,
    .fore_color = C_WHITE,
    .back_color = C_BLACK,
    .ui8_previous_digits_array = {255, 255, 255, 255, 255},
    .ui8_field_number_of_digits = 2,
    .ui8_left_zero_paddig = 0,
  };

  static print_number_t wheel_speed_decimal =
  {
    .font = &FONT_45X72,
    .fore_color = C_WHITE,
    .back_color = C_BLACK,
    .ui8_previous_digits_array = {255, 255, 255, 255, 255},
    .ui8_field_number_of_digits = 1,
    .ui8_left_zero_paddig = 0,
  };

  const uint32_t ui32_x_position_integer = 110;
  const uint32_t ui32_x_position_dot = 238;
  const uint32_t ui32_x_position_decimal = 246;
  const uint32_t ui32_y_position_integer = 60;
  const uint32_t ui32_y_position_dot = 134;
  const uint32_t ui32_y_position_decimal = 81;

  uint16_t ui16_wheel_speed_x10 = l3_vars.ui16_wheel_speed_x10;

  if (m_lcd_vars.ui32_main_screen_draw_static_info)
  {
    UG_SetBackcolor(C_BLACK);
    UG_SetForecolor(MAIN_SCREEN_FIELD_LABELS_COLOR);
    UG_FontSelect(&FONT_10X16);

    if(l3_vars.ui8_units_type == 0)
    {
      UG_PutString(257, 50 , "KM/H");
    }
    else
    {
      UG_PutString(262, 50 , "MPH");
    }

    // print dot
    UG_FillCircle(ui32_x_position_dot, ui32_y_position_dot, 3, C_WHITE);
  }

  // convert to imperial
  if(l3_vars.ui8_units_type)
  {
    ui16_wheel_speed_x10 = (ui16_wheel_speed_x10 * 10) / 16;
  }

  if((ui16_wheel_speed_x10 != ui16_wheel_x10_speed_previous) ||
      m_lcd_vars.ui32_main_screen_draw_static_info)
  {
    ui16_wheel_x10_speed_previous = ui16_wheel_speed_x10;

    wheel_speed_integer.ui32_x_position = ui32_x_position_integer;
    wheel_speed_integer.ui32_y_position = ui32_y_position_integer;
    wheel_speed_integer.ui32_number = (uint32_t) (ui16_wheel_speed_x10 / 10);
    wheel_speed_integer.ui8_refresh_all_digits = m_lcd_vars.ui32_main_screen_draw_static_info;
    lcd_print_number(&wheel_speed_integer);

    wheel_speed_decimal.ui32_x_position = ui32_x_position_decimal;
    wheel_speed_decimal.ui32_y_position = ui32_y_position_decimal;
    wheel_speed_decimal.ui32_number = (uint32_t) (ui16_wheel_speed_x10 % 10);
    wheel_speed_decimal.ui8_refresh_all_digits = m_lcd_vars.ui32_main_screen_draw_static_info;
    lcd_print_number(&wheel_speed_decimal);
  }
}

void calc_battery_soc_watts_hour(void)
{
  uint32_t ui32_temp;

  ui32_temp = l3_vars.ui32_wh_x10 * 100;

  if (l3_vars.ui32_wh_x10_100_percent > 0)
  {
    ui32_temp /= l3_vars.ui32_wh_x10_100_percent;
  }
  else
  {
    ui32_temp = 0;
  }

  // 100% - current SOC or just current SOC
  if (l3_vars.ui8_battery_soc_increment_decrement)
  {
    if (ui32_temp > 100)
      ui32_temp = 100;

    ui16_m_battery_soc_watts_hour = 100 - ui32_temp;
    ui16_m_battery_soc_watts_hour_fixed = 100 - ui32_temp;
  }
  else
  {
    ui16_m_battery_soc_watts_hour = ui32_temp;
  }

  // fixed range
  if (ui32_temp > 100)
  {
    ui32_temp = 100;
  }
  ui16_m_battery_soc_watts_hour_fixed = 100 - ui32_temp;
}

void lcd_print_number(print_number_t* number)
{
  uint32_t ui32_number_temp;
  uint8_t ui8_digit_inverse_counter_1;
  uint8_t ui8_digit_inverse_counter_2;
  uint8_t ui8_digits_array[MAX_NUMBER_DIGITS];
  static uint32_t ui32_power_array[MAX_NUMBER_DIGITS] = {1, 10, 100, 1000, 10000};
  uint32_t ui32_number = number->ui32_number;
  uint8_t ui8_i;
  uint32_t ui32_x_position_1 = number->ui32_x_position;
  uint32_t ui32_x_position_2 = ui32_x_position_1;
  uint32_t ui32_x_position_3;
  uint32_t ui32_y_position = number->ui32_y_position;
  uint8_t ui8_decimal_digits = number->ui8_decimal_digits;
  uint8_t ui8_decimal_digits_inverse_1;
  uint8_t ui8_decimal_digits_inverse_2;
  uint8_t ui8_decimal_digits_printed_1;
  uint8_t ui8_decimal_digits_printed_2;
  uint8_t ui8_digit_number_start = 0;
  uint8_t ui8_left_padding_digit = 0;
  uint8_t ui8_left_padding_first_digit = 0;
  uint8_t ui8_left_padding_digit_total = 0;

  // can't process over MAX_NUMBER_DIGITS
  if(number->ui8_field_number_of_digits > MAX_NUMBER_DIGITS)
  {
    return;
  }

  // set the font that will be used
  UG_FontSelect(number->font);

  ui8_decimal_digits_printed_1 = number->ui8_decimal_digits ? 0: 1;
  ui8_decimal_digits_printed_2 = ui8_decimal_digits_printed_1;

  // get all digits from the number
  ui32_number_temp = ui32_number;
  for(ui8_i = 0; ui8_i < number->ui8_field_number_of_digits; ui8_i++)
  {
    ui8_digits_array[ui8_i] = ui32_number_temp % 10;
    ui32_number_temp /= 10;

    // find the digit number start
    if(ui8_digits_array[ui8_i] != 0)
    {
      ui8_digit_number_start = ui8_i;
    }
  }

  ui8_digit_inverse_counter_1 = number->ui8_field_number_of_digits - 1;
  ui8_digit_inverse_counter_2 = ui8_digit_inverse_counter_1;

  // print first the "."
  // invert the decimal digits
  if(ui8_decimal_digits)
  {
    ui8_decimal_digits_inverse_1 = ui8_digit_inverse_counter_2 - ui8_decimal_digits;
    ui8_decimal_digits_inverse_2 = ui8_decimal_digits_inverse_1;

    // print first the "."
    // loop over all digits
    for(ui8_i = 0; ui8_i < number->ui8_field_number_of_digits; ui8_i++)
    {
      // increase X position for next char
      ui32_x_position_2 += number->font->char_width + 1;

      ui8_digit_inverse_counter_2--;

      // print only 1 time the "."
      if(ui8_decimal_digits_printed_2 == 0 &&
          ui8_decimal_digits_inverse_2 == 0)
      {
        ui8_decimal_digits_printed_2 = 1;

        // print a "."
        UG_PutChar(46, ui32_x_position_2 - (number->font->char_width / 4), ui32_y_position, number->fore_color, number->back_color);

        break;
      }

      // decrement only if positive
      if(ui8_decimal_digits_inverse_2)
      {
        ui8_decimal_digits_inverse_2--;
      }
    }
  }

  // loop over all digits
  for(ui8_i = 0; ui8_i < number->ui8_field_number_of_digits; ui8_i++)
  {
    // only digits that changed
    if(((ui8_digits_array[ui8_digit_inverse_counter_1] != number->ui8_previous_digits_array[ui8_digit_inverse_counter_1]) ||
        (number->ui8_refresh_all_digits)) &&
        (!number->ui8_clean_area_all_digits))
    {
      if((ui8_digits_array[ui8_digit_inverse_counter_1] == 0) && // if is a 0
          (number->ui8_left_paddig) && // left padding
          (ui8_digit_inverse_counter_1 > 0) && // digits that not be printed
          (ui8_left_padding_first_digit == 0)) // if first digit was printed, do not skip next zeros
      {
        // print nothing
        ui8_left_padding_digit = 1;
      }
      else if((ui8_digits_array[ui8_digit_inverse_counter_1] == 0) && // if is a 0
          (ui8_decimal_digits_printed_1 == 0) && // decimal digit not printed yet
          (ui8_decimal_digits_inverse_1 == 0))
      {
        // print a "0"
        UG_PutChar(48, ui32_x_position_1, ui32_y_position, number->fore_color, number->back_color);
      }
      else if((ui8_digits_array[ui8_digit_inverse_counter_1] == 0) && // if is a 0
          (ui8_digit_inverse_counter_1 > ui8_digit_number_start) && // if is a digit at left from the first digit
          (number->ui8_left_zero_paddig)) // if we want to print a 0 at left
      {
        // print a "0"
        UG_PutChar(48, ui32_x_position_1, ui32_y_position, number->fore_color, number->back_color);
      }
      else if((ui8_digits_array[ui8_digit_inverse_counter_1] == 0) &&  // if is a 0
          (ui8_digit_inverse_counter_1 > ui8_digit_number_start) && // if is a digit at left from the first digit
          (!number->ui8_left_zero_paddig)) // if we NOT want to print a 0 at left
      {
        // print a " "
        UG_PutChar(32, ui32_x_position_1, ui32_y_position, number->fore_color, number->back_color);
      }
      else
      {
        // print the digit
        UG_PutChar((ui8_digits_array[ui8_digit_inverse_counter_1] + 48), ui32_x_position_1, ui32_y_position, number->fore_color, number->back_color);

        if(number->ui8_left_paddig)
        {
          ui8_left_padding_first_digit = 1;
        }
      }
    }
    else if((ui8_digits_array[ui8_digit_inverse_counter_1] == 0) && // if is a 0
        (number->ui8_left_paddig) && // left padding
        (ui8_digit_inverse_counter_1 > 0) && // digits that not be printed
        (ui8_left_padding_first_digit == 0)) // if first digit was printed, do not skip next zeros
    {
      // print nothing
    }
    else if((ui8_digits_array[ui8_digit_inverse_counter_1] == 0) && // if is a 0
        (ui8_decimal_digits_printed_1 == 0) && // decimal digit not printed yet
        (ui8_decimal_digits_inverse_1 == 0))
    {
      // print a "0"
      UG_PutChar(48, ui32_x_position_1, ui32_y_position, number->fore_color, number->back_color);
    }
    // the case where there was a 0 but we want to remove it
    else if(((ui8_digits_array[ui8_digit_inverse_counter_1] == 0) &&  // if is a 0
        (ui8_digit_inverse_counter_1 > ui8_digit_number_start) && // if is a digit at left from the first digit
        (!number->ui8_left_zero_paddig)) || // if we NOT want to print a 0 at left
        (number->ui8_clean_area_all_digits)) // we want to clean, so print a " "
    {
      // print a " "
      UG_PutChar(32, ui32_x_position_1, ui32_y_position, number->fore_color, number->back_color);
    }
    // the case where there was a " " but we need to write a 0
    else if((ui8_digits_array[ui8_digit_inverse_counter_1] == 0) &&  // if is a 0
        (ui8_digit_number_start > number->ui8_digit_number_start_previous)) // if is a digit at left from the first digit
    {
      // print a "0"
      UG_PutChar(48, ui32_x_position_1, ui32_y_position, number->fore_color, number->back_color);
    }
    // the case where there was a " " (decimal number) but we need to write a 0
    else if((ui8_digits_array[ui8_digit_inverse_counter_1] == 0) &&  // if is a 0
        (ui8_decimal_digits_printed_1 == 0) && // decimal digits were not printed yet
        (ui8_decimal_digits_inverse_1 == 1)) // first unit digit
    {
      // print a "0"
      UG_PutChar(48, ui32_x_position_1, ui32_y_position, number->fore_color, number->back_color);
    }
    else
    {
      // do not change the field, keep with previous value
    }

    if(ui8_left_padding_digit == 0)
    {
      // increase X position for next char
      ui32_x_position_1 += number->font->char_width + 1;
    }
    // do not increment for next char position
    else
    {
      ui8_left_padding_digit = 0;
      ui8_left_padding_digit_total++;
    }

    ui8_digit_inverse_counter_1--;

    // print only 1 time the "."
    if(ui8_decimal_digits_printed_1 == 0 &&
        ui8_decimal_digits_inverse_1 == 0)
    {
      ui8_decimal_digits_printed_1 = 1;

      // increase X position for next char
      ui32_x_position_1 += ((number->font->char_width / 2) + 1);
    }

    // decrement only if positive
    if(ui8_decimal_digits_inverse_1)
    {
      ui8_decimal_digits_inverse_1--;
    }
  }

  // clean empty
  ui32_x_position_3 = ui32_x_position_1;
  while(ui8_left_padding_digit_total > 0)
  {
    ui8_left_padding_digit_total--;

    // print a " "
    UG_PutChar(32, ui32_x_position_3, ui32_y_position, number->fore_color, number->back_color);

    // increase X position for next char
    ui32_x_position_3 += number->font->char_width + 1;
  }

  // save the digits
  for(ui8_i = 0; ui8_i < number->ui8_field_number_of_digits; ui8_i++)
  {
    number->ui8_previous_digits_array[ui8_i] = ui8_digits_array[ui8_i];
  }

  number->ui8_digit_number_start_previous = ui8_digit_number_start;

  // store final position
  number->ui32_x_final_position = ui32_x_position_1;
  number->ui32_y_final_position = ui32_y_position;
}

void copy_layer_2_layer_3_vars(void)
{
  l3_vars.ui16_adc_battery_voltage = l2_vars.ui16_adc_battery_voltage;
  l3_vars.ui8_battery_current_x5 = l2_vars.ui8_battery_current_x5;
  l3_vars.ui8_throttle = l2_vars.ui8_throttle;
  l3_vars.ui8_adc_pedal_torque_sensor = l2_vars.ui8_adc_pedal_torque_sensor;
  l3_vars.ui8_pedal_torque_sensor = l2_vars.ui8_pedal_torque_sensor;
  l3_vars.ui8_pedal_human_power = l2_vars.ui8_pedal_human_power;
  l3_vars.ui8_duty_cycle = l2_vars.ui8_duty_cycle;
  l3_vars.ui8_error_states = l2_vars.ui8_error_states;
  l3_vars.ui16_wheel_speed_x10 = l2_vars.ui16_wheel_speed_x10;
  l3_vars.ui8_pedal_cadence = l2_vars.ui8_pedal_cadence;
  l3_vars.ui16_motor_speed_erps = l2_vars.ui16_motor_speed_erps;
  l3_vars.ui8_temperature_current_limiting_value = l2_vars.ui8_temperature_current_limiting_value;
  l3_vars.ui8_motor_temperature = l2_vars.ui8_motor_temperature;
  l3_vars.ui32_wheel_speed_sensor_tick_counter = l2_vars.ui32_wheel_speed_sensor_tick_counter;
  l3_vars.ui16_pedal_power_x10 = l2_vars.ui16_pedal_power_x10;
  l3_vars.ui16_battery_voltage_filtered_x10 = l2_vars.ui16_battery_voltage_filtered_x10;
  l3_vars.ui16_battery_current_filtered_x5 = l2_vars.ui16_battery_current_filtered_x5;
  l3_vars.ui16_battery_power_filtered_x50 = l2_vars.ui16_battery_power_filtered_x50;
  l3_vars.ui16_battery_power_filtered = l2_vars.ui16_battery_power_filtered;
  l3_vars.ui16_pedal_torque_filtered = l2_vars.ui16_pedal_torque_filtered;
  l3_vars.ui16_pedal_power_filtered = l2_vars.ui16_pedal_power_filtered;
  l3_vars.ui8_pedal_cadence_filtered = l2_vars.ui8_pedal_cadence_filtered;
  l3_vars.ui16_battery_voltage_soc_x10 = l2_vars.ui16_battery_voltage_soc_x10;
  l3_vars.ui32_wh_sum_x5 = l2_vars.ui32_wh_sum_x5;
  l3_vars.ui32_wh_sum_counter = l2_vars.ui32_wh_sum_counter;
  l3_vars.ui32_wh_x10 = l2_vars.ui32_wh_x10;
  l3_vars.ui8_braking = l2_vars.ui8_braking;
  l3_vars.ui8_foc_angle = l2_vars.ui8_foc_angle;

  l2_vars.ui32_wh_x10_offset = l3_vars.ui32_wh_x10_offset;
  l2_vars.ui16_battery_pack_resistance_x1000 = l3_vars.ui16_battery_pack_resistance_x1000;
  l2_vars.ui8_assist_level = l3_vars.ui8_assist_level;
  l2_vars.ui8_assist_level_factor[0] = l3_vars.ui8_assist_level_factor[0];
  l2_vars.ui8_assist_level_factor[1] = l3_vars.ui8_assist_level_factor[1];
  l2_vars.ui8_assist_level_factor[2] = l3_vars.ui8_assist_level_factor[2];
  l2_vars.ui8_assist_level_factor[3] = l3_vars.ui8_assist_level_factor[3];
  l2_vars.ui8_assist_level_factor[4] = l3_vars.ui8_assist_level_factor[4];
  l2_vars.ui8_assist_level_factor[5] = l3_vars.ui8_assist_level_factor[5];
  l2_vars.ui8_assist_level_factor[6] = l3_vars.ui8_assist_level_factor[6];
  l2_vars.ui8_assist_level_factor[7] = l3_vars.ui8_assist_level_factor[7];
  l2_vars.ui8_assist_level_factor[8] = l3_vars.ui8_assist_level_factor[8];
  l2_vars.ui8_walk_assist_feature_enabled = l3_vars.ui8_walk_assist_feature_enabled;
  l2_vars.ui8_walk_assist_level_factor[0] = l3_vars.ui8_walk_assist_level_factor[0];
  l2_vars.ui8_walk_assist_level_factor[1] = l3_vars.ui8_walk_assist_level_factor[1];
  l2_vars.ui8_walk_assist_level_factor[2] = l3_vars.ui8_walk_assist_level_factor[2];
  l2_vars.ui8_walk_assist_level_factor[3] = l3_vars.ui8_walk_assist_level_factor[3];
  l2_vars.ui8_walk_assist_level_factor[4] = l3_vars.ui8_walk_assist_level_factor[4];
  l2_vars.ui8_walk_assist_level_factor[5] = l3_vars.ui8_walk_assist_level_factor[5];
  l2_vars.ui8_walk_assist_level_factor[6] = l3_vars.ui8_walk_assist_level_factor[6];
  l2_vars.ui8_walk_assist_level_factor[7] = l3_vars.ui8_walk_assist_level_factor[7];
  l2_vars.ui8_walk_assist_level_factor[8] = l3_vars.ui8_walk_assist_level_factor[8];
  l2_vars.ui8_lights = l3_vars.ui8_lights;
  l2_vars.ui8_walk_assist = l3_vars.ui8_walk_assist;
  l2_vars.ui8_offroad_mode = l3_vars.ui8_offroad_mode;
  l2_vars.ui8_battery_max_current = l3_vars.ui8_battery_max_current;
  l2_vars.ui8_ramp_up_amps_per_second_x10 = l3_vars.ui8_ramp_up_amps_per_second_x10;
  l2_vars.ui8_target_max_battery_power = l3_vars.ui8_target_max_battery_power;
  l2_vars.ui16_battery_low_voltage_cut_off_x10 = l3_vars.ui16_battery_low_voltage_cut_off_x10;
  l2_vars.ui16_wheel_perimeter = l3_vars.ui16_wheel_perimeter;
  l2_vars.ui16_wheel_perimeter_imperial_x10 = l3_vars.ui16_wheel_perimeter_imperial_x10;
  l2_vars.ui8_wheel_max_speed = l3_vars.ui8_wheel_max_speed;
  l2_vars.ui8_wheel_max_speed_imperial = l3_vars.ui8_wheel_max_speed_imperial;
  l2_vars.ui8_motor_type = l3_vars.ui8_motor_type;
  l2_vars.ui8_motor_assistance_startup_without_pedal_rotation = l3_vars.ui8_motor_assistance_startup_without_pedal_rotation;
  l2_vars.ui8_temperature_limit_feature_enabled = l3_vars.ui8_temperature_limit_feature_enabled;
  l2_vars.ui8_startup_motor_power_boost_state = l3_vars.ui8_startup_motor_power_boost_state;
  l2_vars.ui8_startup_motor_power_boost_time = l3_vars.ui8_startup_motor_power_boost_time;
  l2_vars.ui8_startup_motor_power_boost_factor[1] = l3_vars.ui8_startup_motor_power_boost_factor[0];
  l2_vars.ui8_startup_motor_power_boost_factor[2] = l3_vars.ui8_startup_motor_power_boost_factor[1];
  l2_vars.ui8_startup_motor_power_boost_factor[3] = l3_vars.ui8_startup_motor_power_boost_factor[2];
  l2_vars.ui8_startup_motor_power_boost_factor[4] = l3_vars.ui8_startup_motor_power_boost_factor[3];
  l2_vars.ui8_startup_motor_power_boost_factor[5] = l3_vars.ui8_startup_motor_power_boost_factor[4];
  l2_vars.ui8_startup_motor_power_boost_factor[6] = l3_vars.ui8_startup_motor_power_boost_factor[5];
  l2_vars.ui8_startup_motor_power_boost_factor[7] = l3_vars.ui8_startup_motor_power_boost_factor[7];
  l2_vars.ui8_startup_motor_power_boost_factor[8] = l3_vars.ui8_startup_motor_power_boost_factor[8];
  l2_vars.ui8_startup_motor_power_boost_factor[9] = l3_vars.ui8_startup_motor_power_boost_factor[9];
  l2_vars.ui8_startup_motor_power_boost_fade_time = l3_vars.ui8_startup_motor_power_boost_fade_time;
  l2_vars.ui8_startup_motor_power_boost_feature_enabled = l3_vars.ui8_startup_motor_power_boost_feature_enabled;
  l2_vars.ui8_motor_temperature_min_value_to_limit = l3_vars.ui8_motor_temperature_min_value_to_limit;
  l2_vars.ui8_motor_temperature_min_value_to_limit_imperial = l3_vars.ui8_motor_temperature_min_value_to_limit_imperial;
  l2_vars.ui8_motor_temperature_max_value_to_limit = l3_vars.ui8_motor_temperature_max_value_to_limit;
  l2_vars.ui8_motor_temperature_max_value_to_limit_imperial = l3_vars.ui8_motor_temperature_max_value_to_limit_imperial;
  l2_vars.ui8_offroad_feature_enabled = l3_vars.ui8_offroad_feature_enabled;
  l2_vars.ui8_offroad_enabled_on_startup = l3_vars.ui8_offroad_enabled_on_startup;
  l2_vars.ui8_offroad_speed_limit = l3_vars.ui8_offroad_speed_limit;
  l2_vars.ui8_offroad_power_limit_enabled = l3_vars.ui8_offroad_power_limit_enabled;
  l2_vars.ui8_offroad_power_limit_div25 = l3_vars.ui8_offroad_power_limit_div25;
}

volatile lcd_vars_t* get_lcd_vars(void)
{
  return &m_lcd_vars;
}

void graphs_measurements_update(void)
{
  static uint32_t counter = 0;
  static uint32_t ui32_pedal_power_accumulated = 0;
  graphs_id_t graph_id = 0;
  uint32_t ui32_temp;

  // start update graphs only after a startup delay to avoid wrong values of the variables
  if(ui32_g_first_time == 0)
  {
    for(graph_id = 0; graph_id < NUMBER_OF_GRAPHS_ID; graph_id++)
    {
      switch(graph_id)
      {
        case GRAPH_WHEEL_SPEED:
          m_p_graphs[graph_id].measurement.ui32_sum_value +=
              l3_vars.ui16_wheel_speed_x10;
        break;

        case GRAPH_PEDAL_HUMAN_POWER:
          // apply the same low pass filter as for the value show to user
          ui32_pedal_power_accumulated -= ui32_pedal_power_accumulated >> PEDAL_POWER_FILTER_COEFFICIENT;
          ui32_pedal_power_accumulated += (uint32_t) l2_vars.ui16_pedal_power_x10 / 10;

          // sum the value
          m_p_graphs[graph_id].measurement.ui32_sum_value += ((uint32_t) (ui32_pedal_power_accumulated >> PEDAL_POWER_FILTER_COEFFICIENT));
        break;

        case GRAPH_PEDAL_CADENCE:
          m_p_graphs[graph_id].measurement.ui32_sum_value +=
              l3_vars.ui8_pedal_cadence_filtered;
        break;

        case GRAPH_BATTERY_VOLTAGE:
          m_p_graphs[graph_id].measurement.ui32_sum_value +=
              l3_vars.ui16_battery_voltage_filtered_x10;
        break;

        case GRAPH_BATTERY_CURRENT:
          m_p_graphs[graph_id].measurement.ui32_sum_value +=
              l3_vars.ui16_battery_current_filtered_x5 * 2; // x10
        break;

        case GRAPH_BATTERY_SOC:
          m_p_graphs[graph_id].measurement.ui32_sum_value +=
              ui16_m_battery_soc_watts_hour_fixed;
        break;

        case GRAPH_MOTOR_POWER:
          m_p_graphs[graph_id].measurement.ui32_sum_value +=
              l3_vars.ui16_battery_power_filtered;
        break;

        case GRAPH_MOTOR_TEMPERATURE:
          if(l2_vars.ui8_units_type == 0)
          {
            m_p_graphs[graph_id].measurement.ui32_sum_value +=
                l3_vars.ui8_motor_temperature;
          }
          else
          {
            m_p_graphs[graph_id].measurement.ui32_sum_value +=
                (uint8_t) ((((uint16_t) l3_vars.ui8_motor_temperature) * 10) - 320) / 18;
          }
        break;

        case GRAPH_MOTOR_PWM_DUTY_CYCLE:
          m_p_graphs[graph_id].measurement.ui32_sum_value +=
              l3_vars.ui8_duty_cycle;
        break;

        case GRAPH_MOTOR_ERPS:
          m_p_graphs[graph_id].measurement.ui32_sum_value +=
              l3_vars.ui16_motor_speed_erps;
        break;

        case GRAPH_MOTOR_FOC_ANGLE:
          ui32_temp = l3_vars.ui8_foc_angle * 140; // each 1 unit equals to 1.4 degrees
          m_p_graphs[graph_id].measurement.ui32_sum_value +=
              ui32_temp;
        break;

        default:
        break;
      }
    }
  }

  // every 3.5 seconds, update the graph array values
  if(++counter >= 35)
  {
    for(graph_id = 0; graph_id < NUMBER_OF_GRAPHS_ID; graph_id++)
    {
      if(m_p_graphs[graph_id].measurement.ui32_sum_value)
      {
        /*store the average value on the 3.5 seconds*/
        m_p_graphs[graph_id].ui32_data_y_last_value = m_p_graphs[graph_id].measurement.ui32_sum_value / counter;
        m_p_graphs[graph_id].measurement.ui32_sum_value = 0;
      }
      else
      {
        /*store the average value on the 3.5 seconds*/
        m_p_graphs[graph_id].ui32_data_y_last_value = 0;
        m_p_graphs[graph_id].measurement.ui32_sum_value = 0;
      }

      m_p_graphs[graph_id].ui32_data_y_last_value_previous = m_p_graphs[graph_id].ui32_data_y_last_value;
    }

    counter = 0;

    // signal to draw graphs on main loop
    ui32_m_draw_graphs_1 = 1;
  }
}

void walk_assist_state(void)
{
  static uint8_t ui8_walk_assist_state = 0;
  static uint8_t ui8_walk_assist_previous;
  uint32_t ui32_x1;
  uint32_t ui32_y1;
  uint32_t ui32_x2;
  uint32_t ui32_y2;

  if(m_lcd_vars.lcd_screen_state == LCD_SCREEN_MAIN &&
      l3_vars.ui8_walk_assist_feature_enabled)
  {
    if(buttons_get_down_long_click_event())
    {
      // clear button long down click event
      buttons_clear_down_long_click_event();
      ui8_walk_assist_state = 1;
    }

    // if down button is still pressed
    if(ui8_walk_assist_state &&
        buttons_get_down_state())
    {
      l3_vars.ui8_walk_assist = 1;
    }
    else if(buttons_get_down_state() == 0)
    {
      ui8_walk_assist_state = 0;
      l3_vars.ui8_walk_assist = 0;
    }
  }
  else
  {
    ui8_walk_assist_state = 0;
    l3_vars.ui8_walk_assist = 0;
  }

  // if previous state was disable, draw
  if((l3_vars.ui8_walk_assist != ui8_walk_assist_previous) ||
      (m_lcd_vars.ui32_main_screen_draw_static_info))
  {
    ui8_walk_assist_previous = l3_vars.ui8_walk_assist;

    if(l3_vars.ui8_walk_assist)
    {
      UG_SetBackcolor(C_BLACK);
      UG_SetForecolor(C_WHITE);
      UG_FontSelect(&SMALL_TEXT_FONT);
      ui32_x1 = 190;
      ui32_y1 = 10;
      UG_PutString(ui32_x1, ui32_y1, "W");
    }
    else
    {
      // clear area
      // 1 leters
      ui32_x1 = 190;
      ui32_y1 = 10;
      ui32_x2 = ui32_x1 + ((1 * 10) + (1 * 1) + 1);
      ui32_y2 = ui32_y1 + 16;
      UG_FillFrame(ui32_x1, ui32_y1, ui32_x2, ui32_y2, C_BLACK);
    }
  }
}

void change_graph(void)
{
  // see if we should enter the MAIN_SCREEN_STATE_CHANGE_GRAPH
  if(buttons_get_onoff_click_long_click_event() &&
      m_lcd_vars.main_screen_state == MAIN_SCREEN_STATE_MAIN)
  {
    buttons_clear_all_events();
    m_lcd_vars.main_screen_state = MAIN_SCREEN_STATE_CHANGE_GRAPH;
  }

  // enter the MAIN_SCREEN_STATE_CHANGE_GRAPH
  if(m_lcd_vars.main_screen_state == MAIN_SCREEN_STATE_CHANGE_GRAPH)
  {
    // change the graph_id with UP and DOWN buttons
    if(buttons_get_up_click_event())
    {
      buttons_clear_all_events();

      l3_vars.graph_id++;
      if(l3_vars.graph_id >= NUMBER_OF_GRAPHS_ID)
      {
        l3_vars.graph_id = 0;
      }

      // force draw the title
      graphs_draw_title(&m_lcd_vars, 1);
      graphs_draw_title(&m_lcd_vars, 2);

      // force draw graph
      ui32_m_draw_graphs_2 = 1;
    }

    if(buttons_get_down_click_event())
    {
      buttons_clear_all_events();

      if(l3_vars.graph_id > 0)
      {
        l3_vars.graph_id--;
      }
      else
      {
        l3_vars.graph_id = NUMBER_OF_GRAPHS_ID - 1;
      }

      // force draw the title
      graphs_draw_title(&m_lcd_vars, 1);
      graphs_draw_title(&m_lcd_vars, 2);

      // force draw graph
      ui32_m_draw_graphs_2 = 1;
    }

    // draw or clean the title
    if(ui8_lcd_menu_flash_state)
    {
      graphs_draw_title(&m_lcd_vars, 2);
    }
    else
    {
      graphs_draw_title(&m_lcd_vars, 1);
    }

    // leave this menu with a button_onoff_long_click
    if(buttons_get_onoff_long_click_event())
    {
      buttons_clear_all_events();
      m_lcd_vars.main_screen_state = MAIN_SCREEN_STATE_MAIN;

      // force draw graph
      ui32_m_draw_graphs_2 = 1;

      // force draw the title
      graphs_draw_title(&m_lcd_vars, 1);
      graphs_draw_title(&m_lcd_vars, 2);
    }

    // clear the events that should not happen but can block the one we want to catch
    buttons_get_onoff_click_event();
    buttons_get_onoff_click_long_click_event();
  }
  // keep drawing tittle in default mode
  else
  {
    graphs_draw_title(&m_lcd_vars, 0);
  }
}
