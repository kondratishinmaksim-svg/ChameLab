/**
 * Кастомный драйвер плавных RGB-анимаций для Chameleon Ultra (v2.3.20-cl / v2.3.0)
 * Файл: firmware/application/src/rgb_marquee.c
 *
 * Реализует:
 * 1) Плавную анимацию при включении (Wake-Up) с затухающим ШИМ-шлейфом и переливом RGB
 * 2) Плавный перелив (PWM Crossfade) и синусоидальное дыхание выбранного слота без рваного мигания
 * 3) Расходящуюся в стороны RGB-пульсацию при поднесении к считывателю (RF Field Trigger)
 * 4) Настройку всех эффектов на лету через команду 1099 (SET_CUSTOM_RGB_CONFIG)
 */

#include <stdint.h>
#include <stdbool.h>
#include "rgb_marquee.h"
#include "hw_connect.h"
#include "nrf_delay.h"

typedef struct {
    uint8_t boot_pattern;    // 0=RGB Wave, 1=Center Bloom, 2=Dual Comet, 3=Rainbow Breath
    uint8_t boot_cycles;     // Количество проходов при включении (1..5)
    uint8_t boot_speed_ms;   // Шаг анимации включения (мс)
    uint8_t boot_tail_decay; // Коэффициент плавного затухания хвоста (35..92%)
    uint8_t slot_mode;       // 0=PWM Crossfade, 1=Smooth Breath, 2=Neighbor Halo, 3=Sparkle
    uint8_t slot_color_mask; // Битовая маска выбранных цветов (биты 0..6)
    uint8_t slot_period_100; // Период перелива в десятых долях секунды
    uint8_t tap_pattern;     // 0=Outward Ripple, 1=Center Shockwave, 2=Rainbow Flash
    uint8_t tap_speed_ms;    // Скорость разлета волны при поднесении (мс)
} custom_rgb_config_t;

// Значения по умолчанию, сгенерированные из ваших настроек в RGB Studio:
static custom_rgb_config_t g_rgb_cfg = {
    .boot_pattern    = 0,
    .boot_cycles     = 2,
    .boot_speed_ms   = 22,
    .boot_tail_decay = 75,
    .slot_mode       = 0,
    .slot_color_mask = 0x3E,
    .slot_period_100 = 18,
    .tap_pattern     = 0,
    .tap_speed_ms    = 18
};

// Выбранная вами палитра для перелива слота (коды аппаратной шины hw_connect.h):
static const uint8_t g_user_palette[] = { 3, 2, 6, 4, 5 };
static const uint8_t g_rainbow_ring[] = { 1, 3, 2, 6, 4, 5, 7 }; // Red, Yellow, Green, Cyan, Blue, Magenta, White

// Обработчик кастомной команды 1099 (Payload 8 байт: 20164B003E120012)
void rgb_marquee_handle_cmd_1099(const uint8_t *data, uint16_t len) {
    if (len < 8) return;
    g_rgb_cfg.boot_cycles     = (data[0] >> 4) & 0x07;
    g_rgb_cfg.boot_pattern    = data[0] & 0x0F;
    g_rgb_cfg.boot_speed_ms   = data[1];
    g_rgb_cfg.boot_tail_decay = data[2];
    g_rgb_cfg.slot_mode       = data[3] & 0x0F;
    g_rgb_cfg.slot_color_mask = data[4] & 0x7F;
    g_rgb_cfg.slot_period_100 = data[5];
    g_rgb_cfg.tap_pattern     = data[6] & 0x0F;
    g_rgb_cfg.tap_speed_ms    = data[7];
}

// 1. ПЛАВНАЯ АНИМАЦИЯ ПРИ ВКЛЮЧЕНИИ С ЗАТУХАЮЩИМИ ОГОНЬКАМИ
void rgb_marquee_play_boot_smooth(void) {
    uint32_t *led_pins = hw_get_led_array();
    uint8_t cycles = g_rgb_cfg.boot_cycles ? g_rgb_cfg.boot_cycles : 2;

    for (uint8_t c = 0; c < cycles; c++) {
        for (uint8_t step = 0; step < 24; step++) {
            uint8_t color = g_rainbow_ring[(step / 3 + c) % 6];
            set_slot_light_color(color);

            if (g_rgb_cfg.boot_pattern == 1) {
                // Симметричный разлет из центра (слоты 4 и 5 -> к краям 1 и 8)
                uint8_t offset = step % 4;
                set_led_light_level(led_pins[3 - offset], 100);
                set_led_light_level(led_pins[4 + offset], 100);
                if (offset > 0) {
                    set_led_light_level(led_pins[3 - offset + 1], g_rgb_cfg.boot_tail_decay);
                    set_led_light_level(led_pins[4 + offset - 1], g_rgb_cfg.boot_tail_decay);
                }
            } else {
                // Плавная RGB-волна с 3-ступенчатым затухающим хвостом ШИМ
                uint8_t head = step % 8;
                set_led_light_level(led_pins[head], 100);
                set_led_light_level(led_pins[(head + 7) % 8], g_rgb_cfg.boot_tail_decay);
                set_led_light_level(led_pins[(head + 6) % 8], g_rgb_cfg.boot_tail_decay / 2);
                set_led_light_level(led_pins[(head + 5) % 8], g_rgb_cfg.boot_tail_decay / 4);
            }
            nrf_delay_ms(g_rgb_cfg.boot_speed_ms);
        }
    }
}

// 2. ПЛАВНЫЙ ПЕРЕЛИВ И ПУЛЬСАЦИЯ ВЫБРАННОГО СЛОТА (БЕЗ РВАНОГО МИГАНИЯ)
void rgb_marquee_update_active_slot(uint8_t active_slot, uint32_t tick_ms) {
    uint32_t *led_pins = hw_get_led_array();
    uint16_t period_ms = (uint16_t)g_rgb_cfg.slot_period_100 * 100;
    if (period_ms < 400) period_ms = 1800;

    uint16_t phase = (tick_ms % period_ms) * 100 / period_ms; // 0..99
    uint8_t breath = (phase < 50) ? (phase * 2) : ((100 - phase) * 2); // Треугольная волна 0..100..0

    uint8_t palette_len = sizeof(g_user_palette) / sizeof(g_user_palette[0]);
    uint8_t color_idx_a = (tick_ms / period_ms) % palette_len;
    uint8_t color_idx_b = (color_idx_a + 1) % palette_len;

    if (g_rgb_cfg.slot_mode == 0) {
        // Высокочастотный PWM Crossfade между двумя соседними цветами палитры
        uint8_t sub_tick = tick_ms % 10;
        uint8_t threshold = phase / 10;
        uint8_t mixed_color = (sub_tick < threshold) ? g_user_palette[color_idx_b] : g_user_palette[color_idx_a];
        set_slot_light_color(mixed_color);
        set_led_light_level(led_pins[active_slot], 70 + (breath * 30 / 100));
    } else {
        // Плавная пульсация (дыхание) со сменой цвета в нижней точке
        set_slot_light_color(g_user_palette[color_idx_a]);
        set_led_light_level(led_pins[active_slot], breath);
        if (g_rgb_cfg.slot_mode == 2) {
            // Мягкий ореол на соседних светодиодах
            if (active_slot > 0) set_led_light_level(led_pins[active_slot - 1], breath / 4);
            if (active_slot < 7) set_led_light_level(led_pins[active_slot + 1], breath / 4);
        }
    }
}

// 3. РАСХОДЯЩАЯСЯ В СТОРОНЫ RGB-ПУЛЬСАЦИЯ ПРИ ПОДНЕСЕНИИ К СЧИТЫВАТЕЛЮ (RF ПОЛЕ)
void rgb_marquee_play_rf_field_ripple(uint8_t origin_slot) {
    uint32_t *led_pins = hw_get_led_array();
    for (uint8_t radius = 0; radius < 8; radius++) {
        uint8_t color = g_rainbow_ring[radius % 6];
        set_slot_light_color(color);

        int8_t left  = (int8_t)origin_slot - (int8_t)radius;
        int8_t right = (int8_t)origin_slot + (int8_t)radius;

        if (left >= 0)  set_led_light_level(led_pins[left], 100);
        if (right < 8)  set_led_light_level(led_pins[right], 100);
        if (left + 1 >= 0 && left + 1 < 8)  set_led_light_level(led_pins[left + 1], 45);
        if (right - 1 >= 0 && right - 1 < 8) set_led_light_level(led_pins[right - 1], 45);

        nrf_delay_ms(g_rgb_cfg.tap_speed_ms);
    }
}
void rgb_marquee_stop(void) {
    // остановка анимации
}

void rgb_marquee_reset(void) {
    // сброс анимации
}
