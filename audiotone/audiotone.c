// audiotone.c
#include "py/obj.h"
#include "py/runtime.h"

#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"

static int tone_pin_l = 26;
static int tone_pin_r = 27;
static int tone_slice = -1;
static bool tone_inited = false;
static bool tone_running = false;

static void tone_configure_pwm_pins(int pin_l, int pin_r) {
    // Configure pins as PWM outputs
    gpio_set_function(pin_l, GPIO_FUNC_PWM);
    gpio_set_function(pin_r, GPIO_FUNC_PWM);

    // Determine the PWM slice from one of the pins (GP26/27 share a slice)
    int slice_l = pwm_gpio_to_slice_num(pin_l);
    // int slice_r = pwm_gpio_to_slice_num(pin_r);

    // For GP26/27 this should be the same; keep it simple and use left's slice
    tone_slice = slice_l;

    // Set initial wrap and clkdiv to something sane (will be updated in start)
    pwm_set_wrap(tone_slice, 255);     // 8-bit period
    pwm_set_clkdiv(tone_slice, 1.0f);  // will be updated in start()

    // Start with outputs disabled
    pwm_set_enabled(tone_slice, false);
}

static void tone_set_frequency(uint32_t freq_hz) {
    // We choose an 8-bit wrap for a square tone; easy, and enough for beeps.
    // f = clk_sys / (clkdiv * (wrap + 1))
    // -> clkdiv = clk_sys / (f * (wrap + 1))
    uint32_t clk_sys_hz = clock_get_hz(clk_sys);
    uint32_t wrap = 255;

    float clkdiv = (float)clk_sys_hz / ( (float)freq_hz * (float)(wrap + 1) );
    if (clkdiv < 1.0f) clkdiv = 1.0f;
    if (clkdiv > 255.0f) clkdiv = 255.0f;

    pwm_set_wrap(tone_slice, wrap);
    pwm_set_clkdiv(tone_slice, clkdiv);
}

static void tone_set_duty_for_pin(int pin, float duty) {
    // duty in [0.0 .. 1.0]
    if (duty < 0.0f) duty = 0.0f;
    if (duty > 1.0f) duty = 1.0f;

    uint slice = pwm_gpio_to_slice_num(pin);
    uint chan  = pwm_gpio_to_channel(pin);
    uint16_t wrap = pwm_hw->slice[slice].top; // current wrap

    uint16_t level = (uint16_t)((float)wrap * duty);
    pwm_set_chan_level(slice, chan, level);
}

/* Micropython: audiotone.init(pin_l=26, pin_r=27) */
static mp_obj_t audiotone_init(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    enum { ARG_pin_l, ARG_pin_r };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_pin_l, MP_ARG_INT, {.u_int = 26} },
        { MP_QSTR_pin_r, MP_ARG_INT, {.u_int = 27} },
    };

    mp_arg_val_t v[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, v);

    tone_pin_l = v[ARG_pin_l].u_int;
    tone_pin_r = v[ARG_pin_r].u_int;

    tone_configure_pwm_pins(tone_pin_l, tone_pin_r);
    tone_inited = true;
    tone_running = false;

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(audiotone_init_obj, 0, audiotone_init);

/* Micropython: audiotone.start(freq_hz, duty=0.3) */
static mp_obj_t audiotone_start(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    if (!tone_inited) {
        mp_raise_ValueError(MP_ERROR_TEXT("audiotone not initialized; call init() first"));
    }

    enum { ARG_freq_hz, ARG_duty };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_freq_hz, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 440} },
        { MP_QSTR_duty,    MP_ARG_KW_ONLY | MP_ARG_OBJ,  {.u_obj = MP_OBJ_NEW_SMALL_INT(0)} }, // default set below
    };
    mp_arg_val_t v[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, v);

    uint32_t freq_hz = (uint32_t)v[ARG_freq_hz].u_int;
    float duty = 0.3f;
    if (v[ARG_duty].u_obj != MP_OBJ_NEW_SMALL_INT(0)) {
        duty = mp_obj_get_float(v[ARG_duty].u_obj);
    }

    tone_set_frequency(freq_hz);

    // Set duty for both channels
    tone_set_duty_for_pin(tone_pin_l, duty);
    tone_set_duty_for_pin(tone_pin_r, duty);

    // Enable the slice (both channels start)
    pwm_set_enabled(tone_slice, true);
    tone_running = true;

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(audiotone_start_obj, 1, audiotone_start);

/* Micropython: audiotone.stop() */
static mp_obj_t audiotone_stop(void) {
    if (tone_inited && tone_slice >= 0) {
        pwm_set_enabled(tone_slice, false);
        tone_set_duty_for_pin(tone_pin_l, 0.0f);
        tone_set_duty_for_pin(tone_pin_r, 0.0f);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(audiotone_stop_obj, audiotone_stop);

/* Module table */
static const mp_rom_map_elem_t audiotone_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),    MP_ROM_QSTR(MP_QSTR_audiotone) },
    { MP_ROM_QSTR(MP_QSTR_init),        MP_ROM_PTR(&audiotone_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_start),       MP_ROM_PTR(&audiotone_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop),        MP_ROM_PTR(&audiotone_stop_obj) },
};
static MP_DEFINE_CONST_DICT(audiotone_module_globals, audiotone_module_globals_table);

const mp_obj_module_t audiotone_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t*)&audiotone_module_globals,
};

// Register as a built-in module
MP_REGISTER_MODULE(MP_QSTR_audiotone, audiotone_user_cmodule);
