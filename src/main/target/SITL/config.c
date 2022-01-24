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

#include "platform.h"

#ifdef USE_TARGET_CONFIG

#include "common/axis.h"

#include "config/feature.h"
#include "config/config.h"

#include "flight/mixer.h"
#include "flight/pid.h"
#include "pg/rx.h"
#include "pg/motor.h"
#include "rx/rx.h"
#include "io/serial.h"
#include "telemetry/telemetry.h"
#include "sensors/battery.h"
#include "sensors/acceleration.h"


void targetConfiguration(void)
{
    featureConfigSet(FEATURE_GOVERNOR);

    pidConfigMutable()->pid_process_denom = 16;

    motorConfigMutable()->dev.motorPwmProtocol = PWM_TYPE_STANDARD;

    accelerometerConfigMutable()->accZero.values.calibrationCompleted = 1;

    rxConfigMutable()->rcInterpolationChannels = 0; // Until NaN issue fixed

    mixerRulesMutable(0)->oper   = MIXER_OP_SET;
    mixerRulesMutable(0)->input  = MIXER_IN_STABILIZED_ROLL;
    mixerRulesMutable(0)->output = 1; // S1
    mixerRulesMutable(0)->weight = 1000;
    mixerRulesMutable(0)->offset = 0;

    mixerRulesMutable(1)->oper   = MIXER_OP_SET;
    mixerRulesMutable(1)->input  = MIXER_IN_STABILIZED_PITCH;
    mixerRulesMutable(1)->output = 2; // S2
    mixerRulesMutable(1)->weight = 1000;
    mixerRulesMutable(1)->offset = 0;

    mixerRulesMutable(2)->oper   = MIXER_OP_SET;
    mixerRulesMutable(2)->input  = MIXER_IN_STABILIZED_COLLECTIVE;
    mixerRulesMutable(2)->output = 3; // S3
    mixerRulesMutable(2)->weight = 1000;
    mixerRulesMutable(2)->offset = 0;

    mixerRulesMutable(3)->oper   = MIXER_OP_SET;
    mixerRulesMutable(3)->input  = MIXER_IN_STABILIZED_YAW;
    mixerRulesMutable(3)->output = 4; // S4
    mixerRulesMutable(3)->weight = 1000;
    mixerRulesMutable(3)->offset = 0;

    mixerRulesMutable(4)->oper   = MIXER_OP_SET;
    mixerRulesMutable(4)->input  = MIXER_IN_STABILIZED_THROTTLE;
    mixerRulesMutable(4)->output = 9; // M1
    mixerRulesMutable(4)->weight = 1000;
    mixerRulesMutable(4)->offset = 0;

}

#endif
