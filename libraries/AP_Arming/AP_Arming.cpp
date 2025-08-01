/*
   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "AP_Arming_config.h"

#if AP_ARMING_ENABLED

#include "AP_Arming.h"
#include <AP_HAL/AP_HAL.h>
#include <AP_BoardConfig/AP_BoardConfig.h>
#include <AP_BattMonitor/AP_BattMonitor.h>
#include <AP_Compass/AP_Compass.h>
#include <AP_Notify/AP_Notify.h>
#include <GCS_MAVLink/GCS.h>
#include <GCS_MAVLink/GCS_MAVLink.h>
#include <AP_Mission/AP_Mission.h>
#include <AP_Proximity/AP_Proximity.h>
#include <AP_Rally/AP_Rally.h>
#include <SRV_Channel/SRV_Channel.h>
#include <AC_Fence/AC_Fence.h>
#include <AP_InertialSensor/AP_InertialSensor.h>
#include <AP_InternalError/AP_InternalError.h>
#include <AP_GPS/AP_GPS.h>
#include <AP_Declination/AP_Declination.h>
#include <AP_Airspeed/AP_Airspeed.h>
#include <AP_AHRS/AP_AHRS.h>
#include <AP_Baro/AP_Baro.h>
#include <AP_RangeFinder/AP_RangeFinder.h>
#include <AP_Generator/AP_Generator.h>
#include <AP_Terrain/AP_Terrain.h>
#include <AP_ADSB/AP_ADSB.h>
#include <AP_Scripting/AP_Scripting.h>
#include <AP_Camera/AP_RunCam.h>
#include <AP_GyroFFT/AP_GyroFFT.h>
#include <AP_VisualOdom/AP_VisualOdom.h>
#include <AP_Parachute/AP_Parachute.h>
#include <AP_OSD/AP_OSD.h>
#include <AP_Relay/AP_Relay.h>
#include <RC_Channel/RC_Channel.h>
#include <AP_Button/AP_Button.h>
#include <AP_FETtecOneWire/AP_FETtecOneWire.h>
#include <AP_RPM/AP_RPM.h>
#include <AP_Mount/AP_Mount.h>
#include <AP_OpenDroneID/AP_OpenDroneID.h>
#include <AP_SerialManager/AP_SerialManager.h>
#include <AP_Vehicle/AP_Vehicle_Type.h>
#include <AP_Scheduler/AP_Scheduler.h>
#include <AP_KDECAN/AP_KDECAN.h>
#include <AP_Vehicle/AP_Vehicle.h>

#if HAL_MAX_CAN_PROTOCOL_DRIVERS
  #include <AP_CANManager/AP_CANManager.h>
  #include <AP_Common/AP_Common.h>
  #include <AP_Vehicle/AP_Vehicle_Type.h>

  #include <AP_PiccoloCAN/AP_PiccoloCAN.h>
  #include <AP_DroneCAN/AP_DroneCAN.h>
#endif

#include <AP_Logger/AP_Logger.h>

#define AP_ARMING_COMPASS_MAGFIELD_EXPECTED 530
#define AP_ARMING_COMPASS_MAGFIELD_MIN  185     // 0.35 * 530 milligauss
#define AP_ARMING_COMPASS_MAGFIELD_MAX  875     // 1.65 * 530 milligauss
#define AP_ARMING_BOARD_VOLTAGE_MAX     5.8f
#define AP_ARMING_ACCEL_ERROR_THRESHOLD 0.75f
#define AP_ARMING_MAGFIELD_ERROR_THRESHOLD 100
#define AP_ARMING_AHRS_GPS_ERROR_MAX    10      // accept up to 10m difference between AHRS and GPS

#if APM_BUILD_TYPE(APM_BUILD_ArduPlane)
  #define ARMING_RUDDER_DEFAULT         (uint8_t)RudderArming::ARMONLY
#else
  #define ARMING_RUDDER_DEFAULT         (uint8_t)RudderArming::ARMDISARM
#endif

// find a default value for ARMING_NEED_POS parameter, and determine
// whether the parameter should be shown:
#ifndef AP_ARMING_NEED_LOC_PARAMETER_ENABLED
// determine whether ARMING_NEED_POS is shown:
#if APM_BUILD_COPTER_OR_HELI || APM_BUILD_TYPE(APM_BUILD_Rover)
#define AP_ARMING_NEED_LOC_PARAMETER_ENABLED 1
#else
#define AP_ARMING_NEED_LOC_PARAMETER_ENABLED 0
#endif  // build types
#endif  // AP_ARMING_NEED_LOC_PARAMETER_ENABLED

// if ARMING_NEED_POS is shown, determine what its default should be:
#if AP_ARMING_NEED_LOC_PARAMETER_ENABLED
#if APM_BUILD_COPTER_OR_HELI || APM_BUILD_TYPE(APM_BUILD_Rover)
#define AP_ARMING_NEED_LOC_DEFAULT 0
#else
#error "Unable to find value for AP_ARMING_NEED_LOC_DEFAULT"
#endif  // APM_BUILD_TYPE
#endif  // AP_ARMING_NEED_LOC_PARAMETER_ENABLED

#ifndef PREARM_DISPLAY_PERIOD
# define PREARM_DISPLAY_PERIOD 30
#endif

extern const AP_HAL::HAL& hal;

const AP_Param::GroupInfo AP_Arming::var_info[] = {

    // @Param{Plane, Rover}: REQUIRE
    // @DisplayName: Require Arming Motors 
    // @Description{Plane}: Arming disabled until some requirements are met. If 0, there are no requirements (arm immediately).  If 1, sends the minimum throttle PWM value to the throttle channel when disarmed. If 2, send 0 PWM (no signal) to throttle channel when disarmed. On planes with ICE enabled and the throttle while disarmed option set in ICE_OPTIONS, the motor will always get THR_MIN when disarmed. Arming will be blocked until all mandatory and ARMING_CHECK items are satisfied; arming can then be accomplished via (eg.) rudder gesture or GCS command.
    // @Description{Rover}: Arming disabled until some requirements are met. If 0, there are no requirements (arm immediately).  If 1, all checks specified by ARMING_CHECKS must pass before the vehicle can be armed (for example, via rudder stick or GCS command).  If 3, Arm immediately once pre-arm/arm checks are satisfied, but only one time per boot up.  Note that a reboot is NOT required when setting to 0 but IS require when setting to 3.
    // @Values{Plane}: 0:Disabled,1:Yes(minimum PWM when disarmed),2:Yes(0 PWM when disarmed)
    // @Values{Rover}: 0:No,1:Yes(minimum PWM when disarmed),3:No(AutoArmOnce after checks are passed)
    // @User: Advanced
    AP_GROUPINFO_FLAGS_FRAME("REQUIRE",     0,      AP_Arming,  require, float(Required::YES_MIN_PWM),
                             AP_PARAM_FLAG_NO_SHIFT,
                             AP_PARAM_FRAME_PLANE | AP_PARAM_FRAME_ROVER),

    // 2 was the CHECK paramter stored in a AP_Int16

    // @Param: ACCTHRESH
    // @DisplayName: Accelerometer error threshold
    // @Description: Accelerometer error threshold used to determine inconsistent accelerometers. Compares this error range to other accelerometers to detect a hardware or calibration error. Lower value means tighter check and harder to pass arming check. Not all accelerometers are created equal.
    // @Units: m/s/s
    // @Range: 0.25 3.0
    // @User: Advanced
    AP_GROUPINFO("ACCTHRESH",    3,     AP_Arming,  accel_error_threshold,  AP_ARMING_ACCEL_ERROR_THRESHOLD),

    // index 4 was VOLT_MIN, moved to AP_BattMonitor
    // index 5 was VOLT2_MIN, moved to AP_BattMonitor

    // @Param{Plane,Rover,Copter,Blimp}: RUDDER
    // @DisplayName: Arming with Rudder enable/disable
    // @Description: Allow arm/disarm by rudder input. When enabled arming can be done with right rudder, disarming with left rudder. Rudder arming only works with throttle at zero +- deadzone (RCx_DZ). Depending on vehicle type, arming in certain modes is prevented. See the wiki for each vehicle. Caution is recommended when arming if it is allowed in an auto-throttle mode!
    // @Values: 0:Disabled,1:ArmingOnly,2:ArmOrDisarm
    // @User: Advanced
    AP_GROUPINFO_FRAME("RUDDER",  6,     AP_Arming, _rudder_arming, ARMING_RUDDER_DEFAULT, AP_PARAM_FRAME_PLANE |
                                                                                           AP_PARAM_FRAME_ROVER |
                                                                                           AP_PARAM_FRAME_COPTER |
                                                                                           AP_PARAM_FRAME_TRICOPTER |
                                                                                           AP_PARAM_FRAME_HELI |
                                                                                           AP_PARAM_FRAME_BLIMP),

    // @Param: MIS_ITEMS
    // @DisplayName: Required mission items
    // @Description: Bitmask of mission items that are required to be planned in order to arm the aircraft
    // @Bitmask: 0:Land,1:VTOL Land,2:DO_LAND_START,3:Takeoff,4:VTOL Takeoff,5:Rallypoint,6:RTL
    // @User: Advanced
    AP_GROUPINFO("MIS_ITEMS",    7,     AP_Arming, _required_mission_items, 0),

    // @Param: CHECK
    // @DisplayName: Arm Checks to Perform (bitmask)
    // @Description: Checks prior to arming motor. This is a bitmask of checks that will be performed before allowing arming. For most users it is recommended to leave this at the default of 1 (all checks enabled). You can select whatever checks you prefer by adding together the values of each check type to set this parameter. For example, to only allow arming when you have GPS lock and no RC failsafe you would set ARMING_CHECK to 72.
    // @Bitmask: 0:All,1:Barometer,2:Compass,3:GPS lock,4:INS,5:Parameters,6:RC Channels,7:Board voltage,8:Battery Level,10:Logging Available,11:Hardware safety switch,12:GPS Configuration,13:System,14:Mission,15:Rangefinder,16:Camera,17:AuxAuth,18:VisualOdometry,19:FFT
    // @Bitmask{Plane}: 0:All,1:Barometer,2:Compass,3:GPS lock,4:INS,5:Parameters,6:RC Channels,7:Board voltage,8:Battery Level,9:Airspeed,10:Logging Available,11:Hardware safety switch,12:GPS Configuration,13:System,14:Mission,15:Rangefinder,16:Camera,17:AuxAuth,19:FFT
    // @User: Standard
    AP_GROUPINFO("CHECK",        8,     AP_Arming,  checks_to_perform,       float(Check::ALL)),

    // @Param: OPTIONS
    // @DisplayName: Arming options
    // @Description: Options that can be applied to change arming behaviour
    // @Bitmask: 0:Disable prearm display,1:Do not send status text on state change
    // @User: Advanced
    AP_GROUPINFO("OPTIONS", 9,   AP_Arming, _arming_options, 0),

    // @Param: MAGTHRESH
    // @DisplayName: Compass magnetic field strength error threshold vs earth magnetic model
    // @Description: Compass magnetic field strength error threshold vs earth magnetic model.  X and y axis are compared using this threhold, Z axis uses 2x this threshold.  0 to disable check
    // @Units: mGauss
    // @Range: 0 500
    // @User: Advanced
    AP_GROUPINFO("MAGTHRESH", 10, AP_Arming, magfield_error_threshold,  AP_ARMING_MAGFIELD_ERROR_THRESHOLD),

#if AP_ARMING_CRASHDUMP_ACK_ENABLED
    // @Param: CRSDP_IGN
    // @DisplayName: Disable CrashDump Arming check
    // @Description: Must have value "1" if crashdump data is present on the system, or a prearm failure will be raised.  Do not set this parameter unless the risks of doing so are fully understood.  The presence of a crash dump means that the firmware currently installed has suffered a critical software failure which resulted in the autopilot immediately rebooting.  The crashdump file gives diagnostic information which can help in finding the issue, please contact the ArduPIlot support team.  If this crashdump data is present, the vehicle is likely unsafe to fly.  Check the ArduPilot documentation for more details.
    // @Values: 0:Crash Dump arming check active, 1:Crash Dump arming check deactivated
    // @User: Advanced
    AP_GROUPINFO("CRSDP_IGN", 11, AP_Arming, crashdump_ack.acked, 0),
#endif  // AP_ARMING_CRASHDUMP_ACK_ENABLED

#if AP_ARMING_NEED_LOC_PARAMETER_ENABLED
    // @Param: NEED_LOC
    // @DisplayName: Require vehicle location
    // @Description: Require that the vehicle have an absolute position before it arms.  This can help ensure that the vehicle can Return To Launch.
    // @User: Advanced
    // @Values{Copter,Rover}: 0:Do not require location,1:Require Location
    AP_GROUPINFO("NEED_LOC", 12, AP_Arming, require_location, float(AP_ARMING_NEED_LOC_DEFAULT)),
#endif  // AP_ARMING_NEED_LOC_PARAMETER_ENABLED

    AP_GROUPEND
};

#if HAL_WITH_IO_MCU
#include <AP_IOMCU/AP_IOMCU.h>
extern AP_IOMCU iomcu;
#endif

#pragma GCC diagnostic push
#if defined(__clang_major__) && __clang_major__ >= 14
#pragma GCC diagnostic ignored "-Wbitwise-instead-of-logical"
#endif

AP_Arming::AP_Arming()
{
    if (_singleton) {
        AP_HAL::panic("Too many AP_Arming instances");
    }
    _singleton = this;

    AP_Param::setup_object_defaults(this, var_info);
}

// performs pre-arm checks. expects to be called at 1hz.
void AP_Arming::update(void)
{
#if AP_ARMING_CRASHDUMP_ACK_ENABLED
    // if we boot with no crashdump data present, reset the "ignore"
    // parameter so the user will need to acknowledge future crashes
    // too:
    crashdump_ack.check_reset();
#endif

    const uint32_t now_ms = AP_HAL::millis();
    // perform pre-arm checks & display failures every 30 seconds
    bool display_fail = false;
    if ((report_immediately && (now_ms - last_prearm_display_ms > 4000)) ||
        (now_ms - last_prearm_display_ms > PREARM_DISPLAY_PERIOD*1000)) {
        report_immediately = false;
        display_fail = true;
        last_prearm_display_ms = now_ms;
    }
    // OTOH, the user may never want to display them:
    if (option_enabled(Option::DISABLE_PREARM_DISPLAY)) {
        display_fail = false;
    }

    pre_arm_checks(display_fail);
}

#if AP_ARMING_CRASHDUMP_ACK_ENABLED
void AP_Arming::CrashDump::check_reset()
{
    // if there is no crash dump data then clear the crash dump ack.
    // This means on subsequent crash-dumps appearing the user must
    // re-acknowledge.
    if (hal.util->last_crash_dump_size() == 0) {
        // no crash dump data
        acked.set_and_save_ifchanged(0);
    }
}
#endif  // AP_ARMING_CRASHDUMP_ACK_ENABLED

uint16_t AP_Arming::compass_magfield_expected() const
{
    return AP_ARMING_COMPASS_MAGFIELD_EXPECTED;
}

bool AP_Arming::is_armed() const
{
    return armed || arming_required() == Required::NO;
}

/*
  true if armed and safety is off
 */
bool AP_Arming::is_armed_and_safety_off() const
{
    return is_armed() && hal.util->safety_switch_state() != AP_HAL::Util::SAFETY_DISARMED;
}

uint32_t AP_Arming::get_enabled_checks() const
{
    return checks_to_perform;
}

bool AP_Arming::check_enabled(const AP_Arming::Check check) const
{
    if (checks_to_perform & uint32_t(Check::ALL)) {
        return true;
    }
    return (checks_to_perform & uint32_t(check));
}

void AP_Arming::check_failed(const AP_Arming::Check check, bool report, const char *fmt, ...) const
{
    if (!report) {
        return;
    }
    char taggedfmt[MAVLINK_MSG_STATUSTEXT_FIELD_TEXT_LEN+1];

    // metafmt is wrapped around the passed-in format string to
    // prepend "PreArm" or "Arm", depending on what sorts of checks
    // we're currently doing.
    const char *metafmt = "PreArm: %s";  // it's formats all the way down
    if (running_arming_checks) {
        metafmt = "Arm: %s";
    }
    hal.util->snprintf(taggedfmt, sizeof(taggedfmt), metafmt, fmt);

#if HAL_GCS_ENABLED
    MAV_SEVERITY severity = MAV_SEVERITY_CRITICAL;
    if (!check_enabled(check)) {
        // technically should be NOTICE, but will annoy users at that level:
        severity = MAV_SEVERITY_DEBUG;
    }
    va_list arg_list;
    va_start(arg_list, fmt);
    gcs().send_textv(severity, taggedfmt, arg_list);
    va_end(arg_list);
#endif  // HAL_GCS_ENABLED
}

void AP_Arming::check_failed(bool report, const char *fmt, ...) const
{
#if HAL_GCS_ENABLED
    if (!report) {
        return;
    }
    char taggedfmt[MAVLINK_MSG_STATUSTEXT_FIELD_TEXT_LEN+1];

    // metafmt is wrapped around the passed-in format string to
    // prepend "PreArm" or "Arm", depending on what sorts of checks
    // we're currently doing.
    const char *metafmt = "PreArm: %s";  // it's formats all the way down
    if (running_arming_checks) {
        metafmt = "Arm: %s";
    }
    hal.util->snprintf(taggedfmt, sizeof(taggedfmt), metafmt, fmt);

    va_list arg_list;
    va_start(arg_list, fmt);
    gcs().send_textv(MAV_SEVERITY_CRITICAL, taggedfmt, arg_list);
    va_end(arg_list);
#endif  // HAL_GCS_ENABLED
}

bool AP_Arming::barometer_checks(bool report)
{
#ifdef HAL_BARO_ALLOW_INIT_NO_BARO
    return true;
#endif
#if CONFIG_HAL_BOARD == HAL_BOARD_SITL
    if (AP::sitl()->baro_count == 0) {
        // simulate no baro boards
        return true;
    }
#endif
    if (check_enabled(Check::BARO)) {
        char buffer[MAVLINK_MSG_STATUSTEXT_FIELD_TEXT_LEN+1] {};
        if (!AP::baro().arming_checks(sizeof(buffer), buffer)) {
            check_failed(Check::BARO, report, "Baro: %s", buffer);
            return false;
        }
    }

    return true;
}

#if AP_AIRSPEED_ENABLED
bool AP_Arming::airspeed_checks(bool report)
{
    if (check_enabled(Check::AIRSPEED)) {
        const AP_Airspeed *airspeed = AP_Airspeed::get_singleton();
        if (airspeed == nullptr) {
            // not an airspeed capable vehicle
            return true;
        }
        char buffer[MAVLINK_MSG_STATUSTEXT_FIELD_TEXT_LEN+1] {};
        if (!airspeed->arming_checks(sizeof(buffer), buffer)) {
            check_failed(Check::AIRSPEED, report, "Airspeed: %s", buffer);
            return false;
        }
    }

    return true;
}
#endif  // AP_AIRSPEED_ENABLED

#if HAL_LOGGING_ENABLED
bool AP_Arming::logging_checks(bool report)
{
    if (check_enabled(Check::LOGGING)) {
        if (!AP::logger().logging_present()) {
            // Logging is disabled, so nothing to check.
            return true;
        }
        if (AP::logger().logging_failed()) {
            check_failed(Check::LOGGING, report, "Logging failed");
            return false;
        }
        if (!AP::logger().CardInserted()) {
            check_failed(Check::LOGGING, report, "No SD card");
            return false;
        }
        if (AP::logger().in_log_download()) {
            check_failed(Check::LOGGING, report, "Downloading logs");
            return false;
        }
    }
    return true;
}
#endif  // HAL_LOGGING_ENABLED

#if AP_INERTIALSENSOR_ENABLED
bool AP_Arming::ins_accels_consistent(const AP_InertialSensor &ins)
{
    const uint32_t now = AP_HAL::millis();
    if (!ins.accels_consistent(accel_error_threshold)) {
        // accels are inconsistent:
        last_accel_pass_ms = 0;
        return false;
    }

    if (last_accel_pass_ms == 0) {
        // we didn't return false above, so sensors are
        // consistent right now:
        last_accel_pass_ms = now;
    }

    // if accels can in theory be inconsistent,
    // must pass for at least 10 seconds before we're considered consistent:
    if (ins.get_accel_count() > 1 && now - last_accel_pass_ms < 10000) {
        return false;
    }

    return true;
}

bool AP_Arming::ins_gyros_consistent(const AP_InertialSensor &ins)
{
    const uint32_t now = AP_HAL::millis();
    // allow for up to 5 degrees/s difference
    if (!ins.gyros_consistent(5)) {
        // gyros are inconsistent:
        last_gyro_pass_ms = 0;
        return false;
    }

    // we didn't return false above, so sensors are
    // consistent right now:
    if (last_gyro_pass_ms == 0) {
        last_gyro_pass_ms = now;
    }

    // if gyros can in theory be inconsistent,
    // must pass for at least 10 seconds before we're considered consistent:
    if (ins.get_gyro_count() > 1 && now - last_gyro_pass_ms < 10000) {
        return false;
    }

    return true;
}

bool AP_Arming::ins_checks(bool report)
{
    if (check_enabled(Check::INS)) {
        const AP_InertialSensor &ins = AP::ins();
        if (!ins.get_gyro_health_all()) {
            check_failed(Check::INS, report, "Gyros not healthy");
            return false;
        }
        if (!ins.gyro_calibrated_ok_all()) {
            check_failed(Check::INS, report, "Gyros not calibrated");
            return false;
        }
        if (!ins.get_accel_health_all()) {
            check_failed(Check::INS, report, "Accels not healthy");
            return false;
        }
        if (!ins.accel_calibrated_ok_all()) {
            check_failed(Check::INS, report, "3D Accel calibration needed");
            return false;
        }
        
        //check if accelerometers have calibrated and require reboot
        if (ins.accel_cal_requires_reboot()) {
            check_failed(Check::INS, report, "Accels calibrated requires reboot");
            return false;
        }

        // check all accelerometers point in roughly same direction
        if (!ins_accels_consistent(ins)) {
            check_failed(Check::INS, report, "Accels inconsistent");
            return false;
        }

        // check all gyros are giving consistent readings
        if (!ins_gyros_consistent(ins)) {
            check_failed(Check::INS, report, "Gyros inconsistent");
            return false;
        }

        // no arming while doing temp cal
        if (ins.temperature_cal_running()) {
            check_failed(Check::INS, report, "temperature cal running");
            return false;
        }

#if AP_INERTIALSENSOR_BATCHSAMPLER_ENABLED
        // If Batch sampling enabled it must be initialized
        if (ins.batchsampler.enabled() && !ins.batchsampler.is_initialised()) {
            check_failed(Check::INS, report, "Batch sampling requires reboot");
            return false;
        }
#endif

        // check if IMU gyro updates are greater than or equal to Ardupilot Loop rate
        char fail_msg[MAVLINK_MSG_STATUSTEXT_FIELD_TEXT_LEN+1];
        if (!ins.pre_arm_check_gyro_backend_rate_hz(fail_msg, ARRAY_SIZE(fail_msg))) {
            check_failed(Check::INS, report, "%s", fail_msg);
            return false;
        }
    }

#if HAL_GYROFFT_ENABLED
    // gyros are healthy so check the FFT
    if (check_enabled(Check::FFT)) {
        // Check that the noise analyser works
        AP_GyroFFT *fft = AP::fft();

        char fail_msg[MAVLINK_MSG_STATUSTEXT_FIELD_TEXT_LEN+1];
        if (fft != nullptr && !fft->pre_arm_check(fail_msg, ARRAY_SIZE(fail_msg))) {
            check_failed(Check::INS, report, "%s", fail_msg);
            return false;
        }
    }
#endif

    return true;
}
#endif // AP_INERTIALSENSOR_ENABLED

bool AP_Arming::compass_checks(bool report)
{
    Compass &_compass = AP::compass();

#if COMPASS_CAL_ENABLED
    // check if compass is calibrating
    if (_compass.is_calibrating()) {
        check_failed(report, "Compass calibration running");
        return false;
    }

    // check if compass has calibrated and requires reboot
    if (_compass.compass_cal_requires_reboot()) {
        check_failed(report, "Compass calibrated requires reboot");
        return false;
    }
#endif

    if (check_enabled(Check::COMPASS)) {

        // avoid Compass::use_for_yaw(void) as it implicitly calls healthy() which can
        // incorrectly skip the remaining checks, pass the primary instance directly
        if (!_compass.use_for_yaw(0)) {
            // compass use is disabled
            return true;
        }

        if (!_compass.healthy()) {
            check_failed(Check::COMPASS, report, "Compass not healthy");
            return false;
        }
        // check compass learning is on or offsets have been set
#if !APM_BUILD_COPTER_OR_HELI && !APM_BUILD_TYPE(APM_BUILD_Blimp)
        // check compass offsets have been set if learning is off
        // copter and blimp always require configured compasses
        if (!_compass.learn_offsets_enabled())
#endif
        {
            char failure_msg[100] = {};
            if (!_compass.configured(failure_msg, ARRAY_SIZE(failure_msg))) {
                check_failed(Check::COMPASS, report, "%s", failure_msg);
                return false;
            }
        }

        // check for unreasonable compass offsets
        const Vector3f offsets = _compass.get_offsets();
        if (offsets.length() > _compass.get_offsets_max()) {
            check_failed(Check::COMPASS, report, "Compass offsets too high");
            return false;
        }

        // check for unreasonable mag field length
        const float mag_field = _compass.get_field().length();
        if (mag_field > AP_ARMING_COMPASS_MAGFIELD_MAX || mag_field < AP_ARMING_COMPASS_MAGFIELD_MIN) {
            check_failed(Check::COMPASS, report, "Check mag field: %4.0f, max %d, min %d", (double)mag_field, AP_ARMING_COMPASS_MAGFIELD_MAX, AP_ARMING_COMPASS_MAGFIELD_MIN);
            return false;
        }

        // check all compasses point in roughly same direction
        if (!_compass.consistent()) {
            check_failed(Check::COMPASS, report, "Compasses inconsistent");
            return false;
        }

#if AP_AHRS_ENABLED
        // if ahrs is using compass and we have location, check mag field versus expected earth magnetic model
        Location ahrs_loc;
        AP_AHRS &ahrs = AP::ahrs();
        if ((magfield_error_threshold > 0) && ahrs.use_compass() && ahrs.get_location(ahrs_loc)) {
            const Vector3f veh_mag_field_ef = ahrs.get_rotation_body_to_ned() * _compass.get_field();
            const Vector3f earth_field_mgauss = AP_Declination::get_earth_field_ga(ahrs_loc) * 1000.0;
            const Vector3f diff_mgauss = veh_mag_field_ef - earth_field_mgauss;
            if (MAX(fabsf(diff_mgauss.x), fabsf(diff_mgauss.y)) > magfield_error_threshold) {
                check_failed(Check::COMPASS, report, "Check mag field (xy diff:%.0f>%d)",
                             (double)MAX(fabsf(diff_mgauss.x), (double)fabsf(diff_mgauss.y)), (int)magfield_error_threshold);
                return false;
            }
            if (fabsf(diff_mgauss.z) > magfield_error_threshold*2.0) {
                check_failed(Check::COMPASS, report, "Check mag field (z diff:%.0f>%d)",
                             (double)fabsf(diff_mgauss.z), (int)magfield_error_threshold*2);
                return false;
            }
        }
#endif  // AP_AHRS_ENABLED
    }

    return true;
}

#if AP_GPS_ENABLED
bool AP_Arming::gps_checks(bool report)
{
    const AP_GPS &gps = AP::gps();
    if (check_enabled(Check::GPS)) {

        // Any failure messages from GPS backends
        char failure_msg[100] = {};
        if (!AP::gps().pre_arm_checks(failure_msg, ARRAY_SIZE(failure_msg))) {
            if (failure_msg[0] != '\0') {
                check_failed(Check::GPS, report, "%s", failure_msg);
            }
            return false;
        }

        for (uint8_t i = 0; i < gps.num_sensors(); i++) {
#if AP_GPS_BLENDED_ENABLED
            if ((i != GPS_BLENDED_INSTANCE) &&
#else
            if (
#endif
                    (gps.get_type(i) == AP_GPS::GPS_Type::GPS_TYPE_NONE)) {
                if (gps.primary_sensor() == i) {
                    check_failed(Check::GPS, report, "GPS %i: primary but TYPE 0", i+1);
                    return false;
                }
                continue;
            }

            //GPS OK?
            if (gps.status(i) < AP_GPS::GPS_OK_FIX_3D) {
                check_failed(Check::GPS, report, "GPS %i: Bad fix", i+1);
                return false;
            }

            //GPS update rate acceptable
            if (!gps.is_healthy(i)) {
                check_failed(Check::GPS, report, "GPS %i: not healthy", i+1);
                return false;
            }
        }

        if (!AP::ahrs().home_is_set()) {
            check_failed(Check::GPS, report, "AHRS: waiting for home");
            return false;
        }

        // check GPSs are within 50m of each other and that blending is healthy
        float distance_m;
        if (!gps.all_consistent(distance_m)) {
            check_failed(Check::GPS, report, "GPS positions differ by %4.1fm",
                         (double)distance_m);
            return false;
        }

        // check AHRS and GPS are within 10m of each other
        if (gps.num_sensors() > 0) {
            const Location gps_loc = gps.location();
            Location ahrs_loc;
            if (AP::ahrs().get_location(ahrs_loc)) {
                const float distance = gps_loc.get_distance(ahrs_loc);
                if (distance > AP_ARMING_AHRS_GPS_ERROR_MAX) {
                    check_failed(Check::GPS, report, "GPS and AHRS differ by %4.1fm", (double)distance);
                    return false;
                }
            }
        }
    }

    if (check_enabled(Check::GPS_CONFIG)) {
        uint8_t first_unconfigured;
        if (gps.first_unconfigured_gps(first_unconfigured)) {
            check_failed(Check::GPS_CONFIG,
                         report,
                         "GPS %d still configuring this GPS",
                         first_unconfigured + 1);
            if (report) {
                gps.broadcast_first_configuration_failure_reason();
            }
            return false;
        }
    }

    return true;
}
#endif  // AP_GPS_ENABLED

#if AP_BATTERY_ENABLED
bool AP_Arming::battery_checks(bool report)
{
    if (check_enabled(Check::BATTERY)) {

        char buffer[MAVLINK_MSG_STATUSTEXT_FIELD_TEXT_LEN+1] {};
        if (!AP::battery().arming_checks(sizeof(buffer), buffer)) {
            check_failed(Check::BATTERY, report, "%s", buffer);
            return false;
        }
     }
    return true;
}
#endif  // AP_BATTERY_ENABLED

bool AP_Arming::hardware_safety_check(bool report) 
{
    if (check_enabled(Check::SWITCH)) {

      // check if safety switch has been pushed
      if (hal.util->safety_switch_state() == AP_HAL::Util::SAFETY_DISARMED) {
          check_failed(Check::SWITCH, report, "Hardware safety switch");
          return false;
      }
    }

    return true;
}

#if AP_RC_CHANNEL_ENABLED
bool AP_Arming::rc_arm_checks(AP_Arming::Method method)
{
    // don't check the trims if we are in a failsafe
    if (!rc().has_valid_input()) {
        return true;
    }

    // only check if we've received some form of input within the last second
    // this is a protection against a vehicle having never enabled an input
    uint32_t last_input_ms = rc().last_input_ms();
    if ((last_input_ms == 0) || ((AP_HAL::millis() - last_input_ms) > 1000)) {
        return true;
    }

    bool check_passed = true;
    // ensure all rc channels have different functions
    if (rc().duplicate_options_exist()) {
        check_failed(Check::PARAMETERS, true, "Duplicate Aux Switch Options");
        check_passed = false;
    }
    if (rc().flight_mode_channel_conflicts_with_rc_option()) {
        check_failed(Check::PARAMETERS, true, "Mode channel and RC%d_OPTION conflict", rc().flight_mode_channel_number());
        check_passed = false;
    }
    {
        if (!rc().option_is_enabled(RC_Channels::Option::ARMING_SKIP_CHECK_RPY)) {
            const struct {
                const char *name;
                const RC_Channel *channel;
            } channels_to_check[] {
                { "Roll", &rc().get_roll_channel(), },
                { "Pitch", &rc().get_pitch_channel(), },
                { "Yaw", &rc().get_yaw_channel(), },
            };
            for (const auto &channel_to_check : channels_to_check) {
                const auto *c = channel_to_check.channel;
                if (c->get_control_in() != 0) {
                    if ((method != Method::RUDDER) || (c != rc().get_arming_channel())) { // ignore the yaw input channel if rudder arming
                        check_failed(Check::RC, true, "%s (RC%d) is not neutral", channel_to_check.name, c->ch());
                        check_passed = false;
                    }
                }
            }
        }

        // if throttle check is enabled, require zero input
        if (rc().arming_check_throttle()) {
            const RC_Channel *c = &rc().get_throttle_channel();
                if (c->get_control_in() != 0) {
                    check_failed(Check::RC, true, "%s (RC%d) is not neutral", "Throttle", c->ch());
                    check_passed = false;
                }
            c = rc().find_channel_for_option(RC_Channel::AUX_FUNC::FWD_THR);
            if (c != nullptr) {
                uint8_t fwd_thr = c->percent_input();
                // require channel input within 2% of minimum
                if (fwd_thr > 2) {
                    check_failed(Check::RC, true, "VTOL Fwd Throttle is not zero");
                    check_passed = false;
                }
            }
        }
    }
    return check_passed;
}

bool AP_Arming::rc_calibration_checks(bool report)
{
    bool check_passed = true;
    const uint8_t num_channels = RC_Channels::get_valid_channel_count();
    for (uint8_t i = 0; i < NUM_RC_CHANNELS; i++) {
        const RC_Channel *c = rc().channel(i);
        if (c == nullptr) {
            continue;
        }
        if (i >= num_channels && !(c->has_override())) {
            continue;
        }
        const uint16_t trim = c->get_radio_trim();
        if (c->get_radio_min() > trim) {
            check_failed(Check::RC, report, "RC%d_MIN is greater than RC%d_TRIM", i + 1, i + 1);
            check_passed = false;
        }
        if (c->get_radio_max() < trim) {
            check_failed(Check::RC, report, "RC%d_MAX is less than RC%d_TRIM", i + 1, i + 1);
            check_passed = false;
        }
    }

    return check_passed;
}

bool AP_Arming::rc_in_calibration_check(bool report)
{
    if (rc().calibrating()) {
        check_failed(Check::RC, report, "RC calibrating");
        return false;
    }
    return true;
}

bool AP_Arming::manual_transmitter_checks(bool report)
{
    if (check_enabled(Check::RC)) {

        if (AP_Notify::flags.failsafe_radio) {
            check_failed(Check::RC, report, "Radio failsafe on");
            return false;
        }

        if (!rc_calibration_checks(report)) {
            return false;
        }
    }

    return rc_in_calibration_check(report);
}
#endif  // AP_RC_CHANNEL_ENABLED

#if AP_MISSION_ENABLED
bool AP_Arming::mission_checks(bool report)
{
    AP_Mission *mission = AP::mission();
    if (check_enabled(Check::MISSION) && _required_mission_items) {
        if (mission == nullptr) {
            check_failed(Check::MISSION, report, "No mission library present");
            return false;
        }

        const struct MisItemTable {
          MIS_ITEM_CHECK check;
          MAV_CMD mis_item_type;
          const char *type;
        } misChecks[] = {
          {MIS_ITEM_CHECK_LAND,          MAV_CMD_NAV_LAND,           "land"},
          {MIS_ITEM_CHECK_VTOL_LAND,     MAV_CMD_NAV_VTOL_LAND,      "vtol land"},
          {MIS_ITEM_CHECK_DO_LAND_START, MAV_CMD_DO_LAND_START,      "do land start"},
          {MIS_ITEM_CHECK_TAKEOFF,       MAV_CMD_NAV_TAKEOFF,        "takeoff"},
          {MIS_ITEM_CHECK_VTOL_TAKEOFF,  MAV_CMD_NAV_VTOL_TAKEOFF,   "vtol takeoff"},
          {MIS_ITEM_CHECK_RETURN_TO_LAUNCH,  MAV_CMD_NAV_RETURN_TO_LAUNCH,   "RTL"},
        };
        for (uint8_t i = 0; i < ARRAY_SIZE(misChecks); i++) {
            if (_required_mission_items & misChecks[i].check) {
                if (!mission->contains_item(misChecks[i].mis_item_type)) {
                    check_failed(Check::MISSION, report, "Missing mission item: %s", misChecks[i].type);
                    return false;
                }
            }
        }
        if (_required_mission_items & MIS_ITEM_CHECK_RALLY) {
#if HAL_RALLY_ENABLED
            AP_Rally *rally = AP::rally();
            if (rally == nullptr) {
                check_failed(Check::MISSION, report, "No rally library present");
                return false;
            }
            Location ahrs_loc;
            if (!AP::ahrs().get_location(ahrs_loc)) {
                check_failed(Check::MISSION, report, "Can't check rally without position");
                return false;
            }
            RallyLocation rally_loc = {};
            if (!rally->find_nearest_rally_point(ahrs_loc, rally_loc)) {
                check_failed(Check::MISSION, report, "No sufficiently close rally point located");
                return false;
            }
#else
            check_failed(Check::MISSION, report, "No rally library present");
            return false;
#endif
        }
    }

#if AP_SDCARD_STORAGE_ENABLED
    if (check_enabled(Check::MISSION) &&
        mission != nullptr &&
        (mission->failed_sdcard_storage() || StorageManager::storage_failed())) {
        check_failed(Check::MISSION, report, "Failed to open %s", AP_MISSION_SDCARD_FILENAME);
        return false;
    }
#endif

#if AP_VEHICLE_ENABLED
    // do not allow arming if there are no mission items and we are in
    // (e.g.) AUTO mode
    if (AP::vehicle()->current_mode_requires_mission() &&
        (mission == nullptr || !mission->present())) {
        check_failed(Check::MISSION, report, "Mode requires mission");
        return false;
    }
#endif

    return true;
}
#endif  // AP_MISSION_ENABLED

bool AP_Arming::rangefinder_checks(bool report)
{
#if AP_RANGEFINDER_ENABLED
    if (check_enabled(Check::RANGEFINDER)) {
        RangeFinder *range = RangeFinder::get_singleton();
        if (range == nullptr) {
            return true;
        }

        char buffer[MAVLINK_MSG_STATUSTEXT_FIELD_TEXT_LEN+1];
        if (!range->prearm_healthy(buffer, ARRAY_SIZE(buffer))) {
            check_failed(Check::RANGEFINDER, report, "%s", buffer);
            return false;
        }
    }
#endif

    return true;
}

bool AP_Arming::servo_checks(bool report) const
{
#if NUM_SERVO_CHANNELS
    bool check_passed = true;
    for (uint8_t i = 0; i < NUM_SERVO_CHANNELS; i++) {
        const SRV_Channel *c = SRV_Channels::srv_channel(i);
        if (c == nullptr || c->get_function() <= SRV_Channel::k_none) {
            continue;
        }

        const uint16_t trim = c->get_trim();
        if (c->get_output_min() > trim) {
            check_failed(report, "SERVO%d_MIN is greater than SERVO%d_TRIM", i + 1, i + 1);
            check_passed = false;
        }
        if (c->get_output_max() < trim) {
            check_failed(report, "SERVO%d_MAX is less than SERVO%d_TRIM", i + 1, i + 1);
            check_passed = false;
        }

        // check functions using PWM are enabled
        if (SRV_Channels::get_disabled_channel_mask() & 1U<<i) {
            const SRV_Channel::Function ch_function = c->get_function();

            // motors, e-stoppable functions, neopixels and ProfiLEDs may be digital outputs and thus can be disabled
            // scripting can use its functions as labels for LED setup
            const bool disabled_ok = SRV_Channel::is_motor(ch_function) ||
                                     SRV_Channel::should_e_stop(ch_function) ||
                                     (ch_function >= SRV_Channel::k_LED_neopixel1 && ch_function <= SRV_Channel::k_LED_neopixel4) ||
                                     (ch_function >= SRV_Channel::k_ProfiLED_1 && ch_function <= SRV_Channel::k_ProfiLED_Clock) ||
                                     (ch_function >= SRV_Channel::k_scripting1 && ch_function <= SRV_Channel::k_scripting16);

            // for all other functions raise a pre-arm failure
            if (!disabled_ok) {
                check_failed(report, "SERVO%u_FUNCTION=%u on disabled channel", i + 1, (unsigned)ch_function);
                check_passed = false;
            }
        }
    }

#if HAL_WITH_IO_MCU
    if (!iomcu.healthy() && AP_BoardConfig::io_enabled()) {
        check_failed(report, "IOMCU is unhealthy");
        check_passed = false;
    }
#endif

    return check_passed;
#else
    return false;
#endif
}

bool AP_Arming::board_voltage_checks(bool report)
{
    // check board voltage
    if (check_enabled(Check::VOLTAGE)) {
#if HAL_HAVE_BOARD_VOLTAGE
        const float bus_voltage =  hal.analogin->board_voltage();
        const float vbus_min = AP_BoardConfig::get_minimum_board_voltage();
        if(((bus_voltage < vbus_min) || (bus_voltage > AP_ARMING_BOARD_VOLTAGE_MAX))) {
            check_failed(Check::VOLTAGE, report, "Board (%1.1fv) out of range %1.1f-%1.1fv", (double)bus_voltage, (double)vbus_min, (double)AP_ARMING_BOARD_VOLTAGE_MAX);
            return false;
        }
#endif // HAL_HAVE_BOARD_VOLTAGE

#if HAL_HAVE_SERVO_VOLTAGE
       const float vservo_min = AP_BoardConfig::get_minimum_servo_voltage();
        if (is_positive(vservo_min)) {
            const float servo_voltage =  hal.analogin->servorail_voltage();
            if (servo_voltage < vservo_min) {
                check_failed(Check::VOLTAGE, report, "Servo voltage to low (%1.2fv < %1.2fv)", (double)servo_voltage, (double)vservo_min);
                return false;
            }
        }
#endif // HAL_HAVE_SERVO_VOLTAGE
    }

    return true;
}

#if HAL_HAVE_IMU_HEATER
bool AP_Arming::heater_min_temperature_checks(bool report)
{
    if (checks_to_perform & uint32_t(Check::ALL)) {
        AP_BoardConfig *board = AP::boardConfig();
        if (board) {
            float temperature;
            int8_t min_temperature;
            if (board->get_board_heater_temperature(temperature) &&
                board->get_board_heater_arming_temperature(min_temperature) &&
                (temperature < min_temperature)) {
                check_failed(Check::SYSTEM, report, "heater temp low (%0.1f < %i)", temperature, min_temperature);
                return false;
            }
        }
    }
    return true;
}
#endif // HAL_HAVE_IMU_HEATER

/*
  check base system operations
 */
bool AP_Arming::system_checks(bool report)
{
    char buffer[MAVLINK_MSG_STATUSTEXT_FIELD_TEXT_LEN+1] {};

    if (check_enabled(Check::SYSTEM)) {
        if (!hal.storage->healthy()) {
            check_failed(Check::SYSTEM, report, "Param storage failed");
            return false;
        }

        if (AP_Param::get_eeprom_full()) {
            check_failed(Check::PARAMETERS, report, "parameter storage full");
            return false;
        }
        
        // check main loop rate is at least 90% of expected value
        const float actual_loop_rate = AP::scheduler().get_filtered_loop_rate_hz();
        const uint16_t expected_loop_rate = AP::scheduler().get_loop_rate_hz();
        const float loop_rate_pct =  actual_loop_rate / expected_loop_rate;
        if (loop_rate_pct < 0.90) {
            check_failed(Check::SYSTEM, report, "Main loop slow (%uHz < %uHz)", (unsigned)actual_loop_rate, (unsigned)expected_loop_rate);
            return false;
        }

#if AP_TERRAIN_AVAILABLE
        const AP_Terrain *terrain = AP_Terrain::get_singleton();
        if ((terrain != nullptr) && terrain->init_failed()) {
            check_failed(Check::SYSTEM, report, "Terrain out of memory");
            return false;
        }
#endif
#if AP_SCRIPTING_ENABLED
        const AP_Scripting *scripting = AP_Scripting::get_singleton();
        if ((scripting != nullptr) && !scripting->arming_checks(sizeof(buffer), buffer)) {
            check_failed(Check::SYSTEM, report, "%s", buffer);
            return false;
        }
#endif
#if HAL_ADSB_ENABLED
        AP_ADSB *adsb = AP::ADSB();
        if ((adsb != nullptr) && adsb->enabled() && adsb->init_failed()) {
            check_failed(Check::SYSTEM, report, "ADSB out of memory");
            return false;
        }
#endif
    }
    if (AP::internalerror().errors() != 0) {
        AP::internalerror().errors_as_string((uint8_t*)buffer, ARRAY_SIZE(buffer));
        check_failed(report, "Internal errors 0x%x l:%u %s", (unsigned int)AP::internalerror().errors(), AP::internalerror().last_error_line(), buffer);
        return false;
    }

    if (!hal.gpio->arming_checks(sizeof(buffer), buffer)) {
        check_failed(report, "%s", buffer);
        return false;
    }

    if (check_enabled(Check::PARAMETERS)) {
#if !AP_GPS_BLENDED_ENABLED
        if (!blending_auto_switch_checks(report)) {
            return false;
        }
#endif
#if AP_RPM_ENABLED
        auto *rpm = AP::rpm();
        if (rpm && !rpm->arming_checks(sizeof(buffer), buffer)) {
            check_failed(Check::PARAMETERS, report, "%s", buffer);
            return false;
        }
#endif
#if AP_RELAY_ENABLED
        auto *relay = AP::relay();
        if (relay && !relay->arming_checks(sizeof(buffer), buffer)) {
            check_failed(Check::PARAMETERS, report, "%s", buffer);
            return false;
        }
#endif
#if HAL_PARACHUTE_ENABLED
        auto *chute = AP::parachute();
        if (chute && !chute->arming_checks(sizeof(buffer), buffer)) {
            check_failed(Check::PARAMETERS, report, "%s", buffer);
            return false;
        }
#endif
#if HAL_BUTTON_ENABLED
        const auto &button = AP::button();
        if (!button.arming_checks(sizeof(buffer), buffer)) {
            check_failed(Check::PARAMETERS, report, "%s", buffer);
            return false;
        }
#endif
    }

    return true;
}

bool AP_Arming::terrain_database_required() const
{
#if AP_MISSION_ENABLED
    AP_Mission *mission = AP::mission();
    if (mission == nullptr) {
        // no mission support?
        return false;
    }
    if (mission->contains_terrain_alt_items()) {
        return true;
    }
#endif
    return false;
}

// check terrain database is fit-for-purpose
bool AP_Arming::terrain_checks(bool report) const
{
    if (!check_enabled(Check::PARAMETERS)) {
        return true;
    }

    if (!terrain_database_required()) {
        return true;
    }

#if AP_TERRAIN_AVAILABLE

    const AP_Terrain *terrain = AP_Terrain::get_singleton();
    if (terrain == nullptr) {
        // this is also a system error, and it is already complaining
        // about it.
        return false;
    }

    if (!terrain->enabled()) {
        check_failed(Check::PARAMETERS, report, "terrain disabled");
        return false;
    }

    char fail_msg[MAVLINK_MSG_STATUSTEXT_FIELD_TEXT_LEN+1];
    if (!terrain->pre_arm_checks(fail_msg, sizeof(fail_msg))) {
        check_failed(Check::PARAMETERS, report, "%s", fail_msg);
        return false;
    }

    return true;

#else
    check_failed(Check::PARAMETERS, report, "terrain required but disabled");
    return false;
#endif
}


#if HAL_PROXIMITY_ENABLED
// check nothing is too close to vehicle
bool AP_Arming::proximity_checks(bool report) const
{
    const AP_Proximity *proximity = AP::proximity();
    // return true immediately if no sensor present
    if (proximity == nullptr) {
        return true;
    }
    char buffer[MAVLINK_MSG_STATUSTEXT_FIELD_TEXT_LEN+1];
    if (!proximity->prearm_healthy(buffer, ARRAY_SIZE(buffer))) {
        check_failed(report, "%s", buffer);
        return false;
    }
    return true;
}
#endif  // HAL_PROXIMITY_ENABLED

#if HAL_MAX_CAN_PROTOCOL_DRIVERS && HAL_CANMANAGER_ENABLED
bool AP_Arming::can_checks(bool report)
{
    if (check_enabled(Check::SYSTEM)) {
        char fail_msg[100] = {};
        (void)fail_msg; // might be left unused
        uint8_t num_drivers = AP::can().get_num_drivers();

        for (uint8_t i = 0; i < num_drivers; i++) {
            switch (AP::can().get_driver_type(i)) {
                case AP_CAN::Protocol::PiccoloCAN: {
#if HAL_PICCOLO_CAN_ENABLE
                    AP_PiccoloCAN *ap_pcan = AP_PiccoloCAN::get_pcan(i);

                    if (ap_pcan != nullptr && !ap_pcan->pre_arm_check(fail_msg, ARRAY_SIZE(fail_msg))) {
                        check_failed(Check::SYSTEM, report, "PiccoloCAN: %s", fail_msg);
                        return false;
                    }

#else
                    check_failed(Check::SYSTEM, report, "PiccoloCAN not enabled");
                    return false;
#endif
                    break;
                }
                case AP_CAN::Protocol::DroneCAN:
                {
#if HAL_ENABLE_DRONECAN_DRIVERS
                    AP_DroneCAN *ap_dronecan = AP_DroneCAN::get_dronecan(i);
                    if (ap_dronecan != nullptr && !ap_dronecan->prearm_check(fail_msg, ARRAY_SIZE(fail_msg))) {
                        check_failed(Check::SYSTEM, report, "DroneCAN: %s", fail_msg);
                        return false;
                    }
#endif
                    break;
                }
                case AP_CAN::Protocol::USD1:
                case AP_CAN::Protocol::TOFSenseP:
                case AP_CAN::Protocol::RadarCAN:
                case AP_CAN::Protocol::Benewake:
                {
                    for (uint8_t j = i; j; j--) {
                        if (AP::can().get_driver_type(i) == AP::can().get_driver_type(j-1)) {
                            check_failed(Check::SYSTEM, report, "Same rfnd on different CAN ports");
                            return false;
                        }
                    }
                    break;
                }
                case AP_CAN::Protocol::EFI_NWPMU:
                case AP_CAN::Protocol::None:
                case AP_CAN::Protocol::Scripting:
                case AP_CAN::Protocol::Scripting2:
                case AP_CAN::Protocol::KDECAN:

                    break;
            }
        }
    }
    return true;
}
#endif  // HAL_MAX_CAN_PROTOCOL_DRIVERS && HAL_CANMANAGER_ENABLED


#if AP_FENCE_ENABLED
bool AP_Arming::fence_checks(bool display_failure)
{
    const AC_Fence *fence = AP::fence();
    if (fence == nullptr) {
        return true;
    }

    // check fence is ready
    char fail_msg[MAVLINK_MSG_STATUSTEXT_FIELD_TEXT_LEN+1];
    if (fence->pre_arm_check(fail_msg, ARRAY_SIZE(fail_msg))) {
        return true;
    }

    check_failed(display_failure, "%s", fail_msg);

#if AP_SDCARD_STORAGE_ENABLED
    if (fence->failed_sdcard_storage() || StorageManager::storage_failed()) {
        check_failed(display_failure, "Failed to open fence storage");
        return false;
    }
#endif
    
    return false;
}
#endif  // AP_FENCE_ENABLED

#if AP_CAMERA_RUNCAM_ENABLED
bool AP_Arming::camera_checks(bool display_failure)
{
    if (check_enabled(Check::CAMERA)) {
        AP_RunCam *runcam = AP::runcam();
        if (runcam == nullptr) {
            return true;
        }

        // check camera is ready
        char fail_msg[MAVLINK_MSG_STATUSTEXT_FIELD_TEXT_LEN+1];
        if (!runcam->pre_arm_check(fail_msg, ARRAY_SIZE(fail_msg))) {
            check_failed(Check::CAMERA, display_failure, "%s", fail_msg);
            return false;
        }
    }
    return true;
}
#endif  // AP_CAMERA_RUNCAM_ENABLED

#if OSD_ENABLED
bool AP_Arming::osd_checks(bool display_failure) const
{
    if (check_enabled(Check::OSD)) {
        // if no OSD then pass
        const AP_OSD *osd = AP::osd();
        if (osd == nullptr) {
            return true;
        }
        // do osd checks for configuration
        char fail_msg[MAVLINK_MSG_STATUSTEXT_FIELD_TEXT_LEN+1];
        if (!osd->pre_arm_check(fail_msg, ARRAY_SIZE(fail_msg))) {
            check_failed(Check::OSD, display_failure, "%s", fail_msg);
            return false;
        }
   }
    return true;
}
#endif  // OSD_ENABLED

#if HAL_MOUNT_ENABLED
bool AP_Arming::mount_checks(bool display_failure) const
{
    if (check_enabled(Check::CAMERA)) {
        AP_Mount *mount = AP::mount();
        if (mount == nullptr) {
            return true;
        }
        char fail_msg[MAVLINK_MSG_STATUSTEXT_FIELD_TEXT_LEN+1] = {};
        if (!mount->pre_arm_checks(fail_msg, sizeof(fail_msg))) {
            check_failed(Check::CAMERA, display_failure, "Mount: %s", fail_msg);
            return false;
        }
    }
    return true;
}
#endif  // HAL_MOUNT_ENABLED

#if AP_FETTEC_ONEWIRE_ENABLED
bool AP_Arming::fettec_checks(bool display_failure) const
{
    const AP_FETtecOneWire *f = AP_FETtecOneWire::get_singleton();
    if (f == nullptr) {
        return true;
    }

    // check ESCs are ready
    char fail_msg[MAVLINK_MSG_STATUSTEXT_FIELD_TEXT_LEN+1];
    if (!f->pre_arm_check(fail_msg, ARRAY_SIZE(fail_msg))) {
        check_failed(Check::ALL, display_failure, "FETtec: %s", fail_msg);
        return false;
    }
    return true;
}
#endif  // AP_FETTEC_ONEWIRE_ENABLED

#if AP_ARMING_AUX_AUTH_ENABLED
// request an auxiliary authorisation id.  This id should be used in subsequent calls to set_aux_auth_passed/failed
// returns true on success
bool AP_Arming::get_aux_auth_id(uint8_t& auth_id)
{
    WITH_SEMAPHORE(aux_auth_sem);

    // check we have enough room to allocate another id
    if (aux_auth_count >= aux_auth_count_max) {
        aux_auth_error = true;
        return false;
    }

    // allocate buffer for failure message
    if (aux_auth_fail_msg == nullptr) {
        aux_auth_fail_msg = (char *)calloc(aux_auth_str_len, sizeof(char));
        if (aux_auth_fail_msg == nullptr) {
            aux_auth_error = true;
            return false;
        }
    }
    auth_id = aux_auth_count;
    aux_auth_count++;
    return true;
}

// set auxiliary authorisation passed
void AP_Arming::set_aux_auth_passed(uint8_t auth_id)
{
    WITH_SEMAPHORE(aux_auth_sem);

    // sanity check auth_id
    if (auth_id >= aux_auth_count) {
        return;
    }

    aux_auth_state[auth_id] = AuxAuthStates::AUTH_PASSED;
}

// set auxiliary authorisation failed and provide failure message
void AP_Arming::set_aux_auth_failed(uint8_t auth_id, const char* fail_msg)
{
    WITH_SEMAPHORE(aux_auth_sem);

    // sanity check auth_id
    if (auth_id >= aux_auth_count) {
        return;
    }

    // update state
    aux_auth_state[auth_id] = AuxAuthStates::AUTH_FAILED;

    // store failure message if this authoriser has the lowest auth_id
    for (uint8_t i = 0; i < auth_id; i++) {
        if (aux_auth_state[i] == AuxAuthStates::AUTH_FAILED) {
            return;
        }
    }
    if (aux_auth_fail_msg != nullptr) {
        if (fail_msg == nullptr) {
            strncpy(aux_auth_fail_msg, "Auxiliary authorisation refused", aux_auth_str_len);
        } else {
            strncpy(aux_auth_fail_msg, fail_msg, aux_auth_str_len);
        }
        aux_auth_fail_msg_source = auth_id;
    }
}

void AP_Arming::reset_all_aux_auths()
{
    WITH_SEMAPHORE(aux_auth_sem);

    // clear all auxiliary authorisation ids
    aux_auth_count = 0;
    // clear any previous allocation errors
    aux_auth_error = false;

    // reset states for all auxiliary authorisation ids
    for (uint8_t i = 0; i < aux_auth_count_max; i++) {
        aux_auth_state[i] = AuxAuthStates::NO_RESPONSE;
    }

    // free up the failure message buffer
    if (aux_auth_fail_msg != nullptr) {
        free(aux_auth_fail_msg);
        aux_auth_fail_msg = nullptr;
    }
}

bool AP_Arming::aux_auth_checks(bool display_failure)
{
    // handle error cases
    if (aux_auth_error) {
        if (aux_auth_fail_msg == nullptr) {
            check_failed(Check::AUX_AUTH, display_failure, "memory low for auxiliary authorisation");
        } else {
            check_failed(Check::AUX_AUTH, display_failure, "Too many auxiliary authorisers");
        }
        return false;
    }

    WITH_SEMAPHORE(aux_auth_sem);

    // check results for each auxiliary authorisation id
    bool some_failures = false;
    bool failure_msg_sent = false;
    bool waiting_for_responses = false;
    for (uint8_t i = 0; i < aux_auth_count; i++) {
        switch (aux_auth_state[i]) {
        case AuxAuthStates::NO_RESPONSE:
            waiting_for_responses = true;
            break;
        case AuxAuthStates::AUTH_FAILED:
            some_failures = true;
            if (i == aux_auth_fail_msg_source) {
                check_failed(Check::AUX_AUTH, display_failure, "%s", aux_auth_fail_msg);
                failure_msg_sent = true;
            }
            break;
        case AuxAuthStates::AUTH_PASSED:
            break;
        }
    }

    // send failure or waiting message
    if (some_failures) {
        if (!failure_msg_sent) {
            check_failed(Check::AUX_AUTH, display_failure, "Auxiliary authorisation refused");
        }
        return false;
    } else if (waiting_for_responses) {
        check_failed(Check::AUX_AUTH, display_failure, "Waiting for auxiliary authorisation");
        return false;
    }

    // if we got this far all auxiliary checks must have passed
    return true;
}
#endif  // AP_ARMING_AUX_AUTH_ENABLED

#if HAL_GENERATOR_ENABLED
bool AP_Arming::generator_checks(bool display_failure) const
{
    const AP_Generator *generator = AP::generator();
    if (generator == nullptr) {
        return true;
    }
    char failure_msg[100] = {};
    if (!generator->pre_arm_check(failure_msg, sizeof(failure_msg))) {
        check_failed(display_failure, "Generator: %s", failure_msg);
        return false;
    }
    return true;
}
#endif  // HAL_GENERATOR_ENABLED

#if AP_OPENDRONEID_ENABLED
// OpenDroneID Checks
bool AP_Arming::opendroneid_checks(bool display_failure)
{
    auto &opendroneid = AP::opendroneid();

    char failure_msg[100] {};
    if (!opendroneid.pre_arm_check(failure_msg, sizeof(failure_msg))) {
        check_failed(display_failure, "OpenDroneID: %s", failure_msg);
        return false;
    }
    return true;
}
#endif  // AP_OPENDRONEID_ENABLED

//Check for multiple RC in serial protocols
bool AP_Arming::serial_protocol_checks(bool display_failure)
{
    if (AP::serialmanager().have_serial(AP_SerialManager::SerialProtocol_RCIN, 1)) {
       check_failed(display_failure, "Multiple SERIAL ports configured for RC input");
       return false;
    }
    char failure_msg[100] = {};
    if (!AP::serialmanager().pre_arm_checks(failure_msg, ARRAY_SIZE(failure_msg))) {
        check_failed(display_failure, "%s", failure_msg);
        return false;
    }
    return true;
}

//Check for estop
bool AP_Arming::estop_checks(bool display_failure)
{
    if (!SRV_Channels::get_emergency_stop()) {
       // not emergency-stopped, so no prearm failure:
       return true;
    }
#if AP_RC_CHANNEL_ENABLED
    // vehicle is emergency-stopped; if this *appears* to have been done via switch then we do not fail prearms:
    const RC_Channel *chan = rc().find_channel_for_option(RC_Channel::AUX_FUNC::ARM_EMERGENCY_STOP);
    if (chan != nullptr) {
        // an RC channel is configured for arm_emergency_stop option, so estop maybe activated via this switch
        if (chan->get_aux_switch_pos() == RC_Channel::AuxSwitchPos::LOW) {
            // switch is configured and is in estop position, so likely the reason we are estopped, so no prearm failure
            return true;  // no prearm failure
        }
    }
#endif  // AP_RC_CHANNEL_ENABLED
    check_failed(display_failure,"Motors Emergency Stopped");
    return false;
}

bool AP_Arming::pre_arm_checks(bool report)
{
#if !APM_BUILD_COPTER_OR_HELI
    if (armed || arming_required() == Required::NO) {
        // if we are already armed or don't need any arming checks
        // then skip the checks
        return true;
    }
#endif

    bool checks_result = hardware_safety_check(report)
#if HAL_HAVE_IMU_HEATER
        &  heater_min_temperature_checks(report)
#endif
#if AP_BARO_ENABLED
        &  barometer_checks(report)
#endif
#if AP_INERTIALSENSOR_ENABLED
        &  ins_checks(report)
#endif
#if AP_COMPASS_ENABLED
        &  compass_checks(report)
#endif
#if AP_GPS_ENABLED
        &  gps_checks(report)
#endif
#if AP_BATTERY_ENABLED
        &  battery_checks(report)
#endif
#if HAL_LOGGING_ENABLED
        &  logging_checks(report)
#endif
#if AP_RC_CHANNEL_ENABLED
        &  manual_transmitter_checks(report)
#endif
#if AP_MISSION_ENABLED
        &  mission_checks(report)
#endif
#if AP_RANGEFINDER_ENABLED
        &  rangefinder_checks(report)
#endif
        &  servo_checks(report)
        &  board_voltage_checks(report)
        &  system_checks(report)
        &  terrain_checks(report)
#if HAL_MAX_CAN_PROTOCOL_DRIVERS && HAL_CANMANAGER_ENABLED
        &  can_checks(report)
#endif
#if HAL_GENERATOR_ENABLED
        &  generator_checks(report)
#endif
#if HAL_PROXIMITY_ENABLED
        &  proximity_checks(report)
#endif
#if AP_CAMERA_RUNCAM_ENABLED
        &  camera_checks(report)
#endif
#if OSD_ENABLED
        &  osd_checks(report)
#endif
#if HAL_MOUNT_ENABLED
        &  mount_checks(report)
#endif
#if AP_FETTEC_ONEWIRE_ENABLED
        &  fettec_checks(report)
#endif
#if HAL_VISUALODOM_ENABLED
        &  visodom_checks(report)
#endif
#if AP_ARMING_AUX_AUTH_ENABLED
        &  aux_auth_checks(report)
#endif
#if AP_RC_CHANNEL_ENABLED
        &  disarm_switch_checks(report)
#endif
#if AP_FENCE_ENABLED
        &  fence_checks(report)
#endif
#if AP_OPENDRONEID_ENABLED
        &  opendroneid_checks(report)
#endif
#if AP_ARMING_CRASHDUMP_ACK_ENABLED
        & crashdump_checks(report)
#endif
        &  serial_protocol_checks(report)
        &  estop_checks(report);

    if (!checks_result && last_prearm_checks_result) { // check went from true to false
        report_immediately = true;
    }
    last_prearm_checks_result = checks_result;

    return checks_result;
}

bool AP_Arming::arm_checks(AP_Arming::Method method)
{
#if AP_RC_CHANNEL_ENABLED
    if (check_enabled(Check::RC)) {
        if (!rc_arm_checks(method)) {
            return false;
        }
    }
#endif

    // ensure the GPS drivers are ready on any final changes
    if (check_enabled(Check::GPS_CONFIG)) {
        if (!AP::gps().prepare_for_arming()) {
            return false;
        }
    }

    // note that this will prepare AP_Logger to start logging
    // so should be the last check to be done before arming

    // Note also that we need to PrepForArming() regardless of whether
    // the arming check flag is set - disabling the arming check
    // should not stop logging from working.

#if HAL_LOGGING_ENABLED
    AP_Logger *logger = AP_Logger::get_singleton();
    if (logger->logging_present()) {
        // If we're configured to log, prep it
        logger->PrepForArming();
        if (!logger->logging_started() &&
            check_enabled(Check::LOGGING)) {
            check_failed(Check::LOGGING, true, "Logging not started");
            return false;
        }
    }
#endif  // HAL_LOGGING_ENABLED

    return true;
}

#if !AP_GPS_BLENDED_ENABLED
bool AP_Arming::blending_auto_switch_checks(bool report)
{
    if (AP::gps().get_auto_switch_type() == 2) {
        if (report) {
            check_failed(Check::GPS, true, "GPS_AUTO_SWITCH==2 but no blending");
        }
        return false;
    }
    return true;
}
#endif

#if AP_ARMING_CRASHDUMP_ACK_ENABLED
bool AP_Arming::crashdump_checks(bool report)
{
    if (hal.util->last_crash_dump_size() == 0) {
        // no crash dump data
        return true;
    }

    // see if the user has acknowledged the failure and wants to fly anyway:
    if (crashdump_ack.acked) {
        // they may have acked the problem, that doesn't mean we don't
        // continue to warn them they're on thin ice:
        if (report) {
            GCS_SEND_TEXT(MAV_SEVERITY_WARNING, "CrashDump data detected");
        }
        return true;
    }

    check_failed(Check::PARAMETERS, true, "CrashDump data detected");

    return false;
}
#endif  // AP_ARMING_CRASHDUMP_ACK_ENABLED

bool AP_Arming::mandatory_checks(bool report)
{
    bool ret = true;
#if AP_OPENDRONEID_ENABLED
    ret &= opendroneid_checks(report);
#endif
    ret &= rc_in_calibration_check(report);
    ret &= serial_protocol_checks(report);
    return ret;
}

//returns true if arming occurred successfully
bool AP_Arming::arm(AP_Arming::Method method, const bool do_arming_checks)
{
    if (armed) { //already armed
        return false;
    }

    if (method == Method::RUDDER) {
        switch (get_rudder_arming_type()) {
        case AP_Arming::RudderArming::IS_DISABLED:
            //parameter disallows rudder arming/disabling
            return false;
        case AP_Arming::RudderArming::ARMONLY:
        case AP_Arming::RudderArming::ARMDISARM:
            break;
        }
    }

    running_arming_checks = true;  // so we show Arm: rather than Disarm: in messages

    if ((!do_arming_checks && mandatory_checks(true)) || (pre_arm_checks(true) && arm_checks(method))) {
        armed = true;
        last_arm_time_us = AP_HAL::micros64();

        _last_arm_method = method;

#if HAL_LOGGING_ENABLED
        Log_Write_Arm(!do_arming_checks, method); // note Log_Write_Armed takes forced not do_arming_checks
#endif

    } else {
#if HAL_LOGGING_ENABLED
        AP::logger().arming_failure();
#endif
        armed = false;
    }

    running_arming_checks = false;

    if (armed && do_arming_checks && checks_to_perform == 0) {
        GCS_SEND_TEXT(MAV_SEVERITY_WARNING, "Warning: Arming Checks Disabled");
    }
    
#if HAL_GYROFFT_ENABLED
    // make sure the FFT subsystem is enabled if arming checks have been disabled
    AP_GyroFFT *fft = AP::fft();
    if (fft != nullptr) {
        fft->prepare_for_arming();
    }
#endif

#if AP_TERRAIN_AVAILABLE
    if (armed) {
        // tell terrain we have just armed, so it can setup
        // a reference location for terrain adjustment
        auto *terrain = AP::terrain();
        if (terrain != nullptr) {
            terrain->set_reference_location();
        }
    }
#endif

#if AP_FENCE_ENABLED
    if (armed) {
        auto *fence = AP::fence();
        if (fence != nullptr) {
            fence->auto_enable_fence_on_arming();
        }
    }
#endif
#if defined(HAL_ARM_GPIO_PIN)
    update_arm_gpio();
#endif
    return armed;
}

//returns true if disarming occurred successfully
bool AP_Arming::disarm(const AP_Arming::Method method, bool do_disarm_checks)
{
    if (!armed) { // already disarmed
        return false;
    }
    if (method == AP_Arming::Method::RUDDER) {
        // if throttle is not down, then pilot cannot rudder arm/disarm
        if (rc().get_throttle_channel().get_control_in() > 0) {
            return false;
        }
        // option must be enabled:
        if (get_rudder_arming_type() != AP_Arming::RudderArming::ARMDISARM) {
            gcs().send_text(MAV_SEVERITY_INFO, "Disarm: rudder disarm disabled");
            return false;
        }
    }
    armed = false;
    _last_disarm_method = method;

#if HAL_LOGGING_ENABLED
    Log_Write_Disarm(!do_disarm_checks, method);  // Log_Write_Disarm takes "force"

    check_forced_logging(method);
#endif

#if HAL_HAVE_SAFETY_SWITCH
    AP_BoardConfig *board_cfg = AP_BoardConfig::get_singleton();
    if ((board_cfg != nullptr) &&
        (board_cfg->get_safety_button_options() & AP_BoardConfig::BOARD_SAFETY_OPTION_SAFETY_ON_DISARM)) {
        hal.rcout->force_safety_on();
    }
#endif // HAL_HAVE_SAFETY_SWITCH

#if HAL_GYROFFT_ENABLED
    AP_GyroFFT *fft = AP::fft();
    if (fft != nullptr) {
        fft->save_params_on_disarm();
    }
#endif

#if AP_FENCE_ENABLED
    AC_Fence *fence = AP::fence();
    if (fence != nullptr) {
        fence->auto_disable_fence_on_disarming();
    }
#endif
#if defined(HAL_ARM_GPIO_PIN)
    update_arm_gpio();
#endif
    return true;
}

#if defined(HAL_ARM_GPIO_PIN)
void AP_Arming::update_arm_gpio()
{
    if (!AP_BoardConfig::arming_gpio_disabled()) {
        hal.gpio->write(HAL_ARM_GPIO_PIN, HAL_ARM_GPIO_POL_INVERT ? !armed : armed);
    }
}
#endif

void AP_Arming::send_arm_disarm_statustext(const char *str) const
{
    if (option_enabled(AP_Arming::Option::DISABLE_STATUSTEXT_ON_STATE_CHANGE)) {
        return;
    }
    GCS_SEND_TEXT(MAV_SEVERITY_INFO, "%s", str);
}

AP_Arming::Required AP_Arming::arming_required() const
{
#if AP_OPENDRONEID_ENABLED
    // cannot be disabled if OpenDroneID is present
    if (AP_OpenDroneID::get_singleton() != nullptr && AP::opendroneid().enabled()) {
        if (require != Required::YES_MIN_PWM && require != Required::YES_ZERO_PWM) {
            return Required::YES_MIN_PWM;
        }
    }
#endif
    return require;
}

#if AP_RC_CHANNEL_ENABLED
// Copter and sub share the same RC input limits
// Copter checks that min and max have been configured by default, Sub does not
bool AP_Arming::rc_checks_copter_sub(const bool display_failure, const RC_Channel *channels[4]) const
{
    // set rc-checks to success if RC checks are disabled
    if (!check_enabled(Check::RC)) {
        return true;
    }

    bool ret = true;

    const char *channel_names[] = { "Roll", "Pitch", "Throttle", "Yaw" };

    for (uint8_t i=0; i<ARRAY_SIZE(channel_names);i++) {
        const RC_Channel *channel = channels[i];
        const char *channel_name = channel_names[i];
        // check if radio has been calibrated
        if (channel->get_radio_min() > RC_Channel::RC_CALIB_MIN_LIMIT_PWM) {
            check_failed(Check::RC, display_failure, "%s radio min too high", channel_name);
            ret = false;
        }
        if (channel->get_radio_max() < RC_Channel::RC_CALIB_MAX_LIMIT_PWM) {
            check_failed(Check::RC, display_failure, "%s radio max too low", channel_name);
            ret = false;
        }
    }
    return ret;
}
#endif  // AP_RC_CHANNEL_ENABLED

#if HAL_VISUALODOM_ENABLED
// check visual odometry is working
bool AP_Arming::visodom_checks(bool display_failure) const
{
    if (!check_enabled(Check::VISION)) {
        return true;
    }

    AP_VisualOdom *visual_odom = AP::visualodom();
    if (visual_odom != nullptr) {
        char fail_msg[MAVLINK_MSG_STATUSTEXT_FIELD_TEXT_LEN+1];
        if (!visual_odom->pre_arm_check(fail_msg, ARRAY_SIZE(fail_msg))) {
            check_failed(Check::VISION, display_failure, "VisOdom: %s", fail_msg);
            return false;
        }
    }

    return true;
}
#endif

#if AP_RC_CHANNEL_ENABLED
// check disarm switch is asserted
bool AP_Arming::disarm_switch_checks(bool display_failure) const
{
    const RC_Channel *chan = rc().find_channel_for_option(RC_Channel::AUX_FUNC::DISARM);
    if (chan != nullptr &&
        chan->get_aux_switch_pos() == RC_Channel::AuxSwitchPos::HIGH) {
        check_failed(display_failure, "Disarm Switch on");
        return false;
    }

    return true;
}
#endif  // AP_RC_CHANNEL_ENABLED

#if HAL_LOGGING_ENABLED
void AP_Arming::Log_Write_Arm(const bool forced, const AP_Arming::Method method)
{
    const struct log_Arm_Disarm pkt {
        LOG_PACKET_HEADER_INIT(LOG_ARM_DISARM_MSG),
        time_us                 : AP_HAL::micros64(),
        arm_state               : is_armed(),
        arm_checks              : get_enabled_checks(),
        forced                  : forced,
        method                  : (uint8_t)method,
    };
    AP::logger().WriteCriticalBlock(&pkt, sizeof(pkt));
    AP::logger().Write_Event(LogEvent::ARMED);
}

void AP_Arming::Log_Write_Disarm(const bool forced, const AP_Arming::Method method)
{
    const struct log_Arm_Disarm pkt {
        LOG_PACKET_HEADER_INIT(LOG_ARM_DISARM_MSG),
        time_us                 : AP_HAL::micros64(),
        arm_state               : is_armed(),
        arm_checks              : 0,
        forced                  : forced,
        method                  : (uint8_t)method
    };
    AP::logger().WriteCriticalBlock(&pkt, sizeof(pkt));
    AP::logger().Write_Event(LogEvent::DISARMED);
}

// check if we should keep logging after disarming
void AP_Arming::check_forced_logging(const AP_Arming::Method method)
{
    // keep logging if disarmed for a bad reason
    switch(method) {
        case Method::TERMINATION:
        case Method::CPUFAILSAFE:
        case Method::BATTERYFAILSAFE:
        case Method::AFS:
        case Method::ADSBCOLLISIONACTION:
        case Method::PARACHUTE_RELEASE:
        case Method::CRASH:
        case Method::FENCEBREACH:
        case Method::RADIOFAILSAFE:
        case Method::GCSFAILSAFE:
        case Method::TERRRAINFAILSAFE:
        case Method::FAILSAFE_ACTION_TERMINATE:
        case Method::TERRAINFAILSAFE:
        case Method::BADFLOWOFCONTROL:
        case Method::EKFFAILSAFE:
        case Method::GCS_FAILSAFE_SURFACEFAILED:
        case Method::GCS_FAILSAFE_HOLDFAILED:
        case Method::PILOT_INPUT_FAILSAFE:
        case Method::DEADRECKON_FAILSAFE:
        case Method::BLACKBOX:
            // keep logging for longer if disarmed for a bad reason
            AP::logger().set_long_log_persist(true);
            return;

        case Method::RUDDER:
        case Method::TOYMODE:
        case Method::MAVLINK:
        case Method::AUXSWITCH:
        case Method::MOTORTEST:
        case Method::SCRIPTING:
        case Method::SOLOPAUSEWHENLANDED:
        case Method::LANDED:
        case Method::MISSIONEXIT:
        case Method::DISARMDELAY:
        case Method::MOTORDETECTDONE:
        case Method::TAKEOFFTIMEOUT:
        case Method::AUTOLANDED:
        case Method::TOYMODELANDTHROTTLE:
        case Method::TOYMODELANDFORCE:
        case Method::LANDING:
        case Method::DDS:
        case Method::AUTO_ARM_ONCE:
        case Method::TURTLE_MODE:
        case Method::UNKNOWN:
            AP::logger().set_long_log_persist(false);
            return;
    }
}
#endif  // HAL_LOGGING_ENABLED

AP_Arming *AP_Arming::_singleton = nullptr;

/*
 * Get the AP_Arming singleton
 */
AP_Arming *AP_Arming::get_singleton()
{
    return AP_Arming::_singleton;
}

namespace AP {

AP_Arming &arming()
{
    return *AP_Arming::get_singleton();
}

};

#pragma GCC diagnostic pop

#endif  // AP_ARMING_ENABLED
