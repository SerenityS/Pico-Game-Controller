/*
 * Pico Game Controller
 * @author SpeedyPotato
 */
#define PICO_GAME_CONTROLLER_C

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/board.h"
#include "controller_config.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "tusb.h"
#include "usb_descriptors.h"
// clang-format off
#include "debounce/debounce_include.h"
#include "rgb/rgb_include.h"
// clang-format on

PIO pio, pio_1;

bool prev_sw_val[SW_GPIO_SIZE];
uint64_t sw_timestamp[SW_GPIO_SIZE];

uint64_t reactive_timeout_timestamp;

void (*ws2812b_mode)();
void (*loop_mode)();
uint16_t (*debounce_mode)();
bool joy_mode_check = true;

union {
  struct {
    uint8_t buttons[LED_GPIO_SIZE];
    RGB_t rgb[WS2812B_LED_ZONES];
  } lights;
  uint8_t raw[LED_GPIO_SIZE + WS2812B_LED_ZONES * 3];
} lights_report;

/**
 * WS2812B Lighting
 * @param counter Current number of WS2812B cycles
 **/
void ws2812b_update(uint32_t counter) {
  if (time_us_64() - reactive_timeout_timestamp >= REACTIVE_TIMEOUT_MAX) {
    ws2812b_mode(counter);
  } else {
    for (int i = 0; i < WS2812B_LED_ZONES; i++) {
      for (int j = 0; j < WS2812B_LEDS_PER_ZONE; j++) {
        put_pixel(urgb_u32(lights_report.lights.rgb[i].r,
                           lights_report.lights.rgb[i].g,
                           lights_report.lights.rgb[i].b));
      }
    }
  }
}

/**
 * HID/Reactive Lights
 **/
void update_lights() {
  for (int i = 0; i < LED_GPIO_SIZE; i++) {
    if (time_us_64() - reactive_timeout_timestamp >= REACTIVE_TIMEOUT_MAX) {
      if (!gpio_get(SW_GPIO[i])) {
        gpio_put(LED_GPIO[i], 1);
      } else {
        gpio_put(LED_GPIO[i], 0);
      }
    } else {
      if (lights_report.lights.buttons[i] == 0) {
        gpio_put(LED_GPIO[i], 0);
      } else {
        gpio_put(LED_GPIO[i], 1);
      }
    }
  }
}

struct report {
  uint16_t buttons;
} report;

/**
 * Gamepad Mode
 **/
void joy_mode() {
  if (tud_hid_ready()) {
    tud_hid_n_report(0x00, REPORT_ID_JOYSTICK, &report, sizeof(report));
  }
}

/**
 * Keyboard Mode
 **/
void key_mode() {
  if (tud_hid_ready()) {  // Wait for ready
    /*------------- Keyboard -------------*/
    uint8_t nkro_report[32] = {0};
    for (int i = 0; i < SW_GPIO_SIZE; i++) {
      if ((report.buttons >> i) % 2 == 1) {
        uint8_t bit = SW_KEYCODE[i] % 8;
        uint8_t byte = (SW_KEYCODE[i] / 8) + 1;
        if (SW_KEYCODE[i] >= 240 && SW_KEYCODE[i] <= 247) {
          nkro_report[0] |= (1 << bit);
        } else if (byte > 0 && byte <= 31) {
          nkro_report[byte] |= (1 << bit);
        }
      }
    }
    tud_hid_n_report(0x00, REPORT_ID_KEYBOARD, &nkro_report,
                      sizeof(nkro_report));
  }
}

/**
 * Update Input States
 * Note: Switches are pull up, negate value
 **/
void update_inputs() {
  for (int i = 0; i < SW_GPIO_SIZE; i++) {
    // If switch gets pressed, record timestamp
    if (prev_sw_val[i] == false && !gpio_get(SW_GPIO[i]) == true) {
      sw_timestamp[i] = time_us_64();
    }
    prev_sw_val[i] = !gpio_get(SW_GPIO[i]);
  }
}

/**
 * Second Core Runnable
 **/
void core1_entry() {
  uint32_t counter = 0;
  while (1) {
    ws2812b_update(++counter);
    sleep_ms(5);
  }
}

/**
 * Initialize Board Pins
 **/
void init() {
  // LED Pin on when connected
  gpio_init(25);
  gpio_set_dir(25, GPIO_OUT);
  gpio_put(25, 1);

  // Set up WS2812B
  pio_1 = pio1;
  uint offset2 = pio_add_program(pio_1, &ws2812_program);
  ws2812_program_init(pio_1, 0, offset2, WS2812B_GPIO, 800000,
                      false);

  // Setup Button GPIO
  for (int i = 0; i < SW_GPIO_SIZE; i++) {
    prev_sw_val[i] = false;
    sw_timestamp[i] = 0;
    gpio_init(SW_GPIO[i]);
    gpio_set_function(SW_GPIO[i], GPIO_FUNC_SIO);
    gpio_set_dir(SW_GPIO[i], GPIO_IN);
    gpio_pull_up(SW_GPIO[i]);
  }

  // Setup LED GPIO
  for (int i = 0; i < LED_GPIO_SIZE; i++) {
    gpio_init(LED_GPIO[i]);
    gpio_set_dir(LED_GPIO[i], GPIO_OUT);
  }

  // Joy/KB Mode Switching
  if (gpio_get(SW_GPIO[10])) {
    loop_mode = &key_mode;
    joy_mode_check = false;
  } else {
    loop_mode = &joy_mode;
    joy_mode_check = true;
  }
  
  // RGB Mode
  ws2812b_mode = &ws2812b_color_cycle;

  // Debouncing Mode
  debounce_mode = &debounce_deferred;

  // Disable RGB
  if (gpio_get(SW_GPIO[11])) {
    multicore_launch_core1(core1_entry);
  }
}

/**
 * Main Loop Function
 **/
int main(void) {
  board_init();
  init();
  tusb_init();

  while (1) {
    tud_task();  // tinyusb device task
    update_inputs();
    report.buttons = debounce_mode();
    loop_mode();
    update_lights();
  }

  return 0;
}

// Invoked when received GET_REPORT control request
// Application must fill buffer report's content and return its length.
// Return zero will cause the stack to STALL request
uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t* buffer,
                               uint16_t reqlen) {
  // TODO not Implemented
  (void)itf;
  (void)report_id;
  (void)report_type;
  (void)buffer;
  (void)reqlen;

  return 0;
}

// Invoked when received SET_REPORT control request or
// received data on OUT endpoint ( Report ID = 0, Type = 0 )
void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const* buffer,
                           uint16_t bufsize) {
  (void)itf;
  if (report_id == 2 && report_type == HID_REPORT_TYPE_OUTPUT &&
      bufsize >= sizeof(lights_report))  // light data
  {
    size_t i = 0;
    for (i; i < sizeof(lights_report); i++) {
      lights_report.raw[i] = buffer[i];
    }
    reactive_timeout_timestamp = time_us_64();
  }
}
