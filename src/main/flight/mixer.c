/*
 * This file is part of Rotorflight.
 *
 * Rotorflight is free software. You can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Rotorflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software. If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "platform.h"

#include "build/build_config.h"

#include "common/axis.h"
#include "common/filter.h"
#include "common/maths.h"

#include "config/config.h"
#include "config/config_reset.h"

#include "fc/runtime_config.h"
#include "fc/rc_controls.h"
#include "fc/rc_modes.h"

#include "flight/pid.h"
#include "flight/imu.h"
#include "flight/mixer.h"
#include "flight/governor.h"
#include "flight/leveling.h"

#include "rx/rx.h"

#include "pg/pg.h"
#include "pg/pg_ids.h"

#include "sensors/gyro.h"


PG_REGISTER_WITH_RESET_TEMPLATE(mixerConfig_t, mixerConfig, PG_GENERIC_MIXER_CONFIG, 0);

PG_RESET_TEMPLATE(mixerConfig_t, mixerConfig,
    .main_rotor_dir = DIR_CW,
    .tail_rotor_mode = TAIL_MODE_VARIABLE,
    .tail_motor_idle = 0,
    .swash_ring = 0,
    .swash_phase = 0,
);

PG_REGISTER_ARRAY(mixerRule_t, MIXER_RULE_COUNT, mixerRules, PG_GENERIC_MIXER_RULES, 0);

PG_REGISTER_ARRAY_WITH_RESET_FN(mixerInput_t, MIXER_INPUT_COUNT, mixerInputs, PG_GENERIC_MIXER_INPUTS, 0);

void pgResetFn_mixerInputs(mixerInput_t *input)
{
    for (int i = MIXER_IN_STABILIZED_ROLL; i <= MIXER_IN_STABILIZED_THROTTLE; i++) {
        input[i].rate =  1000;
        input[i].min  = -1000;
        input[i].max  =  1000;
    }
}


static FAST_RAM_ZERO_INIT mixerRule_t rules[MIXER_RULE_COUNT];

static FAST_RAM_ZERO_INIT float     mixInput[MIXER_INPUT_COUNT];
static FAST_RAM_ZERO_INIT float     mixOutput[MIXER_OUTPUT_COUNT];
static FAST_RAM_ZERO_INIT int16_t   mixOverride[MIXER_INPUT_COUNT];
static FAST_RAM_ZERO_INIT uint32_t  mixOutputMap[MIXER_OUTPUT_COUNT];
static FAST_RAM_ZERO_INIT uint16_t  mixSaturated[MIXER_INPUT_COUNT];

static FAST_RAM_ZERO_INIT float     cyclicTotal;
static FAST_RAM_ZERO_INIT float     cyclicLimit;

static FAST_RAM_ZERO_INIT float     tailMotorIdle;
static FAST_RAM_ZERO_INIT int8_t    tailMotorDirection;

static FAST_RAM_ZERO_INIT float     phaseSin, phaseCos;


static inline void mixerSetInput(int index, float value)
{
    const mixerInput_t *in = mixerInputs(index);

    // Check override only if not armed
    if (!ARMING_FLAG(ARMED)) {
        if (mixOverride[index] >= MIXER_OVERRIDE_MIN && mixOverride[index] <= MIXER_OVERRIDE_MAX)
            value = mixOverride[index] / 1000.0f;
    }

    // Input limits
    const float imin = in->min / 1000.0f;
    const float imax = in->max / 1000.0f;

    // Constrain and saturate
    if (value > imax) {
        mixInput[index] = imax;
        mixerSaturateInput(index);
    }
    else if (value < imin) {
        mixInput[index] = imin;
        mixerSaturateInput(index);
    }
    else {
        mixInput[index] = value;
    }
}

static void mixerCyclicLimit(void)
{
    // Swash phasing
    if (phaseSin != 0)
    {
        float SR = mixInput[MIXER_IN_STABILIZED_ROLL];
        float SP = mixInput[MIXER_IN_STABILIZED_PITCH];

        mixInput[MIXER_IN_STABILIZED_PITCH] = SP * phaseCos - SR * phaseSin;
        mixInput[MIXER_IN_STABILIZED_ROLL]  = SP * phaseSin + SR * phaseCos;
    }

    // Swashring enabled
    if (cyclicLimit > 0)
    {
        float SR = mixInput[MIXER_IN_STABILIZED_ROLL];
        float SP = mixInput[MIXER_IN_STABILIZED_PITCH];

        const mixerInput_t *mixR = mixerInputs(MIXER_IN_STABILIZED_ROLL);
        const mixerInput_t *mixP = mixerInputs(MIXER_IN_STABILIZED_PITCH);

        // Assume min<0 and max>0 for cyclic & pitch
        const float maxR = ABS((SR < 0) ? mixR->min : mixR->max) / 1000.0f;
        const float maxP = ABS((SP < 0) ? mixP->min : mixP->max) / 1000.0f;

        // Stretch the limits to the unit circle
        SR /= fmaxf(maxR, 0.001f) * cyclicLimit;
        SP /= fmaxf(maxP, 0.001f) * cyclicLimit;

        // Stretched cyclic deflection
        const float cyclic = sqrtf(sq(SR) + sq(SP));

        // Cyclic limits reached - scale back
        if (cyclic > 1.0f)
        {
            mixerSaturateInput(MIXER_IN_STABILIZED_ROLL);
            mixerSaturateInput(MIXER_IN_STABILIZED_PITCH);

            mixInput[MIXER_IN_STABILIZED_ROLL]  /= cyclic;
            mixInput[MIXER_IN_STABILIZED_PITCH] /= cyclic;
        }
    }

    // Total cyclic deflection
    cyclicTotal = sqrtf(sq(mixInput[MIXER_IN_STABILIZED_ROLL]) +
                        sq(mixInput[MIXER_IN_STABILIZED_PITCH]));
}

static void mixerUpdateMotorizedTail(void)
{
    // Motorized tail control
    if (mixerIsTailMode(TAIL_MODE_MOTORIZED)) {
        // Yaw input value
        const float yaw = mixInput[MIXER_IN_STABILIZED_YAW];

        // Thrust linearization
        float throttle = sqrtf(fmaxf(yaw,0));

        // Apply minimum throttle
        throttle = fmaxf(throttle, tailMotorIdle);

        // Slow spoolup
        if (!isSpooledUp()) {
            if (mixInput[MIXER_IN_STABILIZED_THROTTLE] < 0.01f)
                throttle = 0;
            else if (mixInput[MIXER_IN_STABILIZED_THROTTLE] < 0.20f)
                throttle *= mixInput[MIXER_IN_STABILIZED_THROTTLE] / 0.20f;
        }

        // Yaw is now tail motor throttle
        mixInput[MIXER_IN_STABILIZED_YAW] = throttle;
    }
    else if (mixerIsTailMode(TAIL_MODE_BIDIRECTIONAL)) {
        // Yaw input value
        const float yaw = mixInput[MIXER_IN_STABILIZED_YAW];

        // Thrust linearization
        float throttle = copysignf(sqrtf(fabsf(yaw)),yaw);

        // Apply minimum throttle
        if (throttle > -tailMotorIdle && throttle < tailMotorIdle)
            throttle = tailMotorDirection * tailMotorIdle;

        // Slow spoolup
        if (!isSpooledUp()) {
            if (mixInput[MIXER_IN_STABILIZED_THROTTLE] < 0.01f)
                throttle = 0;
            else if (mixInput[MIXER_IN_STABILIZED_THROTTLE] < 0.20f)
                throttle *= mixInput[MIXER_IN_STABILIZED_THROTTLE] / 0.20f;
        }

        // Direction sign
        tailMotorDirection = (throttle < 0) ? -1 : 1;

        // Yaw is now tail motor throttle
        mixInput[MIXER_IN_STABILIZED_YAW] = throttle;
    }
}

static void mixerUpdateInputs(void)
{
    // Update saturation
    for (int i = 0; i < MIXER_INPUT_COUNT; i++) {
        if (mixSaturated[i])
            mixSaturated[i]--;
    }

    // Flight Dynamics
    mixerSetInput(MIXER_IN_RC_COMMAND_ROLL,rcCommand[ROLL] * MIXER_RC_SCALING);
    mixerSetInput(MIXER_IN_RC_COMMAND_PITCH, rcCommand[PITCH] * MIXER_RC_SCALING);
    mixerSetInput(MIXER_IN_RC_COMMAND_YAW, rcCommand[YAW] * MIXER_RC_SCALING);
    mixerSetInput(MIXER_IN_RC_COMMAND_COLLECTIVE, rcCommand[COLLECTIVE] * MIXER_RC_SCALING);

    // Throttle input
    mixerSetInput(MIXER_IN_RC_COMMAND_THROTTLE, (rcCommand[THROTTLE] - MIXER_THR_OFFSET) * MIXER_THR_SCALING);

    // RC channels
    for (int i = 0; i < MAX_SUPPORTED_RC_CHANNEL_COUNT; i++)
        mixerSetInput(MIXER_IN_RC_CHANNEL_ROLL + i, (rcData[i] - rxConfig()->midrc) * MIXER_RC_SCALING);

    // Collective
    mixerSetInput(MIXER_IN_STABILIZED_COLLECTIVE, pidGetStabilizedCollective());

    // PASSTHROUGH mode disables cyclic stabilization (flybar mode)
    if (!FLIGHT_MODE(PASSTHRU_MODE)) {
        mixerSetInput(MIXER_IN_STABILIZED_ROLL, getPidSum(FD_ROLL));
        mixerSetInput(MIXER_IN_STABILIZED_PITCH, getPidSum(FD_PITCH));
    } else {
        mixerSetInput(MIXER_IN_STABILIZED_ROLL, rcCommand[ROLL] * MIXER_RC_SCALING);
        mixerSetInput(MIXER_IN_STABILIZED_PITCH, rcCommand[PITCH] * MIXER_RC_SCALING);
    }

    // Apply swash ring
    mixerCyclicLimit();

    // Tail/Yaw is always stabilised - positive is against main rotor torque
    mixerSetInput(MIXER_IN_STABILIZED_YAW, mixerRotationSign() * getPidSum(FD_YAW));

    // Update governor sub-mixer
    governorUpdate();

    // Update throttle from governor
    mixerSetInput(MIXER_IN_STABILIZED_THROTTLE, getGovernorOutput());

    // Update motorized tail
    if (mixerMotorizedTail())
        mixerUpdateMotorizedTail();
}

void mixerUpdate(void)
{
    // Fetch input values
    mixerUpdateInputs();

    // Reset mixer outputs
    for (int i = 0; i < MIXER_OUTPUT_COUNT; i++) {
        mixOutput[i] = 0;
        mixOutputMap[i] = 0;
    }

    // Current flight mode bitmap
    uint32_t flightModeMask = ((uint32_t)(~flightModeFlags)) << 16 | flightModeFlags;

    // Calculate mixer outputs
    for (int i = 0; i < MIXER_RULE_COUNT; i++) {
        if (rules[i].oper && ((rules[i].mode == 0) || (rules[i].mode & flightModeMask))) {
            uint8_t src = rules[i].input;
            uint8_t dst = rules[i].output;
            float   val = mixInput[src] * mixerInputs(src)->rate / 1000.0f;
            float   out = (rules[i].offset + rules[i].weight * val) / 1000.0f;

            switch (rules[i].oper)
            {
                case MIXER_OP_SET:
                    mixOutput[dst] = out;
                    mixOutputMap[dst] = BIT(src);
                    break;
                case MIXER_OP_ADD:
                    mixOutput[dst] += out;
                    mixOutputMap[dst] |= BIT(src);
                    break;
                case MIXER_OP_MUL:
                    mixOutput[dst] *= out;
                    mixOutputMap[dst] |= BIT(src);
                    break;
            }
        }
    }
}

void mixerInitSwash(void)
{
    if (mixerConfig()->swash_phase) {
        const float angle = DECIDEGREES_TO_RADIANS(mixerConfig()->swash_phase);
        phaseSin = sin_approx(angle);
        phaseCos = cos_approx(angle);
    }
    else {
        phaseSin = 0;
        phaseCos = 1;
    }

    if (mixerConfig()->swash_ring)
        cyclicLimit = 1.41f - mixerConfig()->swash_ring * 0.0041f;
    else
        cyclicLimit = 0;
}

void mixerInit(void)
{
    for (int i = 0; i < MIXER_RULE_COUNT; i++)
    {
        const mixerRule_t *rule = mixerRules(i);

        if (rule->oper) {
            rules[i].mode    = rule->mode;
            rules[i].oper    = constrain(rule->oper, 0, MIXER_OP_COUNT - 1);
            rules[i].input   = constrain(rule->input, 0, MIXER_INPUT_COUNT - 1);
            rules[i].output  = constrain(rule->output, 0, MIXER_OUTPUT_COUNT - 1);
            rules[i].offset  = constrain(rule->offset, MIXER_INPUT_MIN, MIXER_INPUT_MAX);
            rules[i].weight  = constrain(rule->weight, MIXER_WEIGHT_MIN, MIXER_WEIGHT_MAX);
        }
    }

    for (int i = 1; i < MIXER_INPUT_COUNT; i++) {
        mixOverride[i] = MIXER_OVERRIDE_OFF;
    }

    tailMotorIdle = mixerConfig()->tail_motor_idle / 1000.0f;

    mixerInitSwash();
}


bool mixerSaturated(uint8_t index)
{
    return (mixSaturated[index] > 0);
}

void mixerSaturateInput(uint8_t index)
{
    mixSaturated[index] = MIXER_SATURATION_TIME;
}

void mixerSaturateOutput(uint8_t index)
{
    for (int i = 1; i < MIXER_INPUT_COUNT; i++) {
        if (mixOutputMap[index] & BIT(i)) {
            mixerSaturateInput(i);
        }
    }
}

float mixerGetInput(uint8_t i)
{
    return mixInput[i];
}

float mixerGetOutput(uint8_t i)
{
    return mixOutput[i];
}

float mixerGetServoOutput(uint8_t i)
{
    return mixOutput[MIXER_SERVO_OFFSET + i];
}

float mixerGetMotorOutput(uint8_t i)
{
    return mixOutput[MIXER_MOTOR_OFFSET + i];
}

int16_t mixerGetOverride(uint8_t i)
{
    return mixOverride[i];
}

int16_t mixerSetOverride(uint8_t i, int16_t value)
{
    return mixOverride[i] = value;
}

float getCyclicDeflection(void)
{
    return cyclicTotal;
}

float getCollectiveDeflection(void)
{
    return mixInput[MIXER_IN_STABILIZED_COLLECTIVE];
}
