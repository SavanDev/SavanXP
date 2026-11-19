#pragma once
#include "sxn/sxn.h"

/* Pointer speed (docs/kernel/INPUT.md#pointer-speed): SAVANXP_POINTER_SPEED_MIN
 * to _MAX, _DEFAULT moves one to one. The kernel applies it to relative
 * pointers for every reader. Through /dev/pointer, which has the speed and none
 * of the events, so any user program may read and set it. POSIX has
 * input_get_pointer_speed / input_set_pointer_speed. */

/* The saved preference: the speed in decimal ("10"). init applies it at boot,
 * before windowd starts. */
#define SXN_POINTER_SETTINGS_PATH "/users/user/Settings/mouse.cfg"

/* The current speed, or -errno. */
static inline long sxn_pointer_speed(void) {
    long device = sxn_open_device("pointer", SXN_DEVICE_READ);
    long result;
    if (device < 0) return device;
    result = sxn_device_control(device, INPUT_IOC_GET_POINTER_SPEED, 0);
    (void)sxn_close(device);
    return result;
}

/* 0, or -EINVAL out of range, or -errno. Takes effect with the next move. */
static inline long sxn_set_pointer_speed(int speed) {
    long device, result;
    if (speed < SAVANXP_POINTER_SPEED_MIN || speed > SAVANXP_POINTER_SPEED_MAX) return -SAVANXP_EINVAL;
    device = sxn_open_device("pointer", SXN_DEVICE_READ | SXN_DEVICE_WRITE);
    if (device < 0) return device;
    result = sxn_device_control(device, INPUT_IOC_SET_POINTER_SPEED, (unsigned long)speed);
    (void)sxn_close(device);
    return result;
}
