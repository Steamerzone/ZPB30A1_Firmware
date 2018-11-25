#include "load.h"
#include "config.h"
#include "adc.h"
#include "settings.h"
#include "inc/stm8s_tim1.h"

uint32_t mAmpere_seconds = 0;
uint32_t mWatt_seconds = 0;
bool load_active = 0;
error_t error = ERROR_NONE;
calibration_t calibration_step;
uint16_t calibration_value;
uint16_t actual_current_setpoint;

void load_init()
{
    #define PWM_RELOAD (F_CPU / F_PWM)
    // I-SET
    // Low pass is <8 Hz, so we can use full 16 bit resolution (~244Hz).
	TIM1->ARRH = 0xff;
	TIM1->ARRL = 0xff;
	TIM1->PSCRH = 0;
	TIM1->PSCRL = 0;

	TIM1->CCMR1 = TIM1_OCMODE_PWM1 | TIM1_CCMR_OCxPE;
	TIM1->CCER1 = TIM1_CCER1_CC1E;
	TIM1->CCR1H = 0;
	TIM1->CCR1L = 0;
	TIM1->CR1 = TIM1_CR1_CEN;
	TIM1->BKR = TIM1_BKR_MOE;
}

void load_disable()
{
    load_active = 0;
    GPIOE->ODR |= PINE_ENABLE;
}

void load_enable()
{
    load_active = 1;
    //actual activation happens in load_update()
}

static inline void load_update()
{
    uint16_t setpoint = settings.setpoints[settings.mode];
    uint16_t current = 0;

    if (calibration_step == CAL_CURRENT) {
        TIM1->CCR1H = calibration_value >> 8;
        TIM1->CCR1L = calibration_value & 0xff;
        return;
    }
    switch (settings.mode) {
        case MODE_CC:
            current = setpoint;
            break;
        case MODE_CV:
            current = 0;
            //TODO: Unsupported
            break;
        case MODE_CR:
            //U[mV]/R[10mOhm]=I[mA]
            //U*1000 * c / R*100 = I * 1000
            // => c = 100
            current = (uint32_t)adc_get_voltage() * 100 / setpoint;
            break;
        case MODE_CW:
            //P[mW]/U[mV] = I[mA]
            //P*1000 * c / U*1000 = I * 1000
            // => c = 1000
            current = (uint32_t)setpoint * 1000 / adc_get_voltage();
            break;
    }
    /* NOTE: Here v_load is used directly instead of adc_get_voltage, because
       for the power dissipation only the voltage that reaches the load's
       terminals is relevant. */
    uint16_t current_power_limited = (uint32_t)(POW_ABS_MAX) * 1000 / v_load;
    if (current < CUR_MIN) current = CUR_MIN;
    if (current > CUR_MAX) current = CUR_MAX;
    if (settings.mode != MODE_CC && current > settings.current_limit) current = settings.current_limit;
    if (load_active && (current > current_power_limited)) {
        if (settings.max_power_action == MAX_P_LIM) {
            current = current_power_limited;
        } else {
            error = ERROR_OVERLOAD;
        }
    }
    actual_current_setpoint = current;

    uint32_t tmp = current;
    tmp = tmp * LOAD_CAL_M - LOAD_CAL_T;
    TIM1->CCR1H = tmp >> 24;
    TIM1->CCR1L = tmp >> 16;

    // Don't turn on load if an error condition is present
    if (load_active && (error == ERROR_NONE)) GPIOE->ODR &= ~PINE_ENABLE;
}

static inline void load_calc_power()
{
    static uint16_t timer = 0;
    static uint8_t power_remainder = 0;
    static uint8_t current_remainder = 0;
    #if F_SYSTICK % F_POWER_CALC != 0
        #error "F_POWER_CALC must be an integer divider of F_SYSTICK"
    #endif
    timer++;
    if (timer == F_SYSTICK/F_POWER_CALC) {
        timer = 0;
        uint32_t power = actual_current_setpoint;
        power *= v_sense;
        power /= 1000;
        power += power_remainder; //Keep track of rounding errors
        power_remainder = power % F_POWER_CALC;
        mWatt_seconds += power / F_POWER_CALC;

        uint16_t current = actual_current_setpoint + current_remainder;
        current_remainder = current % F_POWER_CALC;
        mAmpere_seconds += current / F_POWER_CALC;
    }
}

void load_timer()
{
    if (load_active) {
        load_calc_power();
    }
    load_update();
}
