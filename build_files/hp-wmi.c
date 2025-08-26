// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HP WMI hotkeys
 *
 * Copyright (C) 2008 Red Hat <mjg@redhat.com>
 * Copyright (C) 2010, 2011 Anssi Hannula <anssi.hannula@iki.fi>
 *
 * Portions based on wistron_btns.c:
 * Copyright (C) 2005 Miloslav Trmac <mitr@volny.cz>
 * Copyright (C) 2005 Bernhard Rosenkraenzer <bero@arklinux.org>
 * Copyright (C) 2005 Dmitry Torokhov <dtor@mail.ru>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/platform_device.h>
#include <linux/platform_profile.h>
#include <linux/hwmon.h>
#include <linux/acpi.h>
#include <linux/mutex.h>
#include <linux/cleanup.h>
#include <linux/power_supply.h>
#include <linux/rfkill.h>
#include <linux/string.h>
#include <linux/dmi.h>
#include <linux/workqueue.h>
#include <linux/delay.h>        // for msleep()
#include <linux/workqueue.h>

MODULE_AUTHOR("Matthew Garrett <mjg59@srcf.ucam.org>");
MODULE_DESCRIPTION("HP laptop WMI driver");
MODULE_LICENSE("GPL");

MODULE_ALIAS("wmi:95F24279-4D7B-4334-9387-ACCDC67EF61C");
MODULE_ALIAS("wmi:5FB7F034-2C63-45E9-BE91-3D44E2C707E4");

#define HPWMI_EVENT_GUID "95F24279-4D7B-4334-9387-ACCDC67EF61C"
#define HPWMI_BIOS_GUID "5FB7F034-2C63-45E9-BE91-3D44E2C707E4"

#define HP_OMEN_EC_THERMAL_PROFILE_FLAGS_OFFSET 0x62
#define HP_OMEN_EC_THERMAL_PROFILE_TIMER_OFFSET 0x63
#define HP_OMEN_EC_THERMAL_PROFILE_OFFSET 0x95

#define HP_FAN_SPEED_AUTOMATIC	 0x00
#define HP_POWER_LIMIT_DEFAULT	 0x00
#define HP_POWER_LIMIT_NO_CHANGE 0xFF

#define ACPI_AC_CLASS "ac_adapter"

#define zero_if_sup(tmp) (zero_insize_support?0:sizeof(tmp)) // use when zero insize is required

/* DMI board names of devices that should use the omen specific path for
 * thermal profiles.
 * This was obtained by taking a look in the windows omen command center
 * app and parsing a json file that they use to figure out what capabilities
 * the device should have.
 * A device is considered an omen if the DisplayName in that list contains
 * "OMEN", and it can use the thermal profile stuff if the "Feature" array
 * contains "PerformanceControl".
 */
static const char * const omen_thermal_profile_boards[] = {
	"84DA", "84DB", "84DC", "8574", "8575", "860A", "87B5", "8572", "8573",
	"8600", "8601", "8602", "8605", "8606", "8607", "8746", "8747", "8749",
	"874A", "8603", "8604", "8748", "886B", "886C", "878A", "878B", "878C",
	"88C8", "88CB", "8786", "8787", "8788", "88D1", "88D2", "88F4", "88FD",
	"88F5", "88F6", "88F7", "88FE", "88FF", "8900", "8901", "8902", "8912",
	"8917", "8918", "8949", "894A", "89EB", "8BAD", "8A42", "8A15", "8A44"
};

/* DMI Board names of Omen laptops that are specifically set to be thermal
 * profile version 0 by the Omen Command Center app, regardless of what
 * the get system design information WMI call returns
 */
static const char * const omen_thermal_profile_force_v0_boards[] = {
	"8607", "8746", "8747", "8749", "874A", "8748"
};

/* DMI board names of Omen laptops that have a thermal profile timer which will
 * cause the embedded controller to set the thermal profile back to
 * "balanced" when reaching zero.
 */
static const char * const omen_timed_thermal_profile_boards[] = {
	"8BAD", "8A42", "8A15", "8A44"
};

/* DMI Board names of Victus 16-d1xxx laptops */
static const char * const victus_thermal_profile_boards[] = {
	"8A25"
};

/* DMI Board names of Victus 16-s1000 laptops */
static const char * const victus_s_thermal_profile_boards[] = {
	"8C9C"
};

static int unified_fan_speed = -1;
static bool unified_manual_mode = false; 
static int last_manual_speed = 50;


enum hp_wmi_radio {
	HPWMI_WIFI	= 0x0,
	HPWMI_BLUETOOTH	= 0x1,
	HPWMI_WWAN	= 0x2,
	HPWMI_GPS	= 0x3,
};

enum hp_wmi_event_ids {
	HPWMI_DOCK_EVENT		= 0x01,
	HPWMI_PARK_HDD			= 0x02,
	HPWMI_SMART_ADAPTER		= 0x03,
	HPWMI_BEZEL_BUTTON		= 0x04,
	HPWMI_WIRELESS			= 0x05,
	HPWMI_CPU_BATTERY_THROTTLE	= 0x06,
	HPWMI_LOCK_SWITCH		= 0x07,
	HPWMI_LID_SWITCH		= 0x08,
	HPWMI_SCREEN_ROTATION		= 0x09,
	HPWMI_COOLSENSE_SYSTEM_MOBILE	= 0x0A,
	HPWMI_COOLSENSE_SYSTEM_HOT	= 0x0B,
	HPWMI_PROXIMITY_SENSOR		= 0x0C,
	HPWMI_BACKLIT_KB_BRIGHTNESS	= 0x0D,
	HPWMI_PEAKSHIFT_PERIOD		= 0x0F,
	HPWMI_BATTERY_CHARGE_PERIOD	= 0x10,
	HPWMI_SANITIZATION_MODE		= 0x17,
	HPWMI_CAMERA_TOGGLE		= 0x1A,
	HPWMI_OMEN_KEY			= 0x1D,
	HPWMI_SMART_EXPERIENCE_APP	= 0x21,
};

/*
 * struct bios_args buffer is dynamically allocated.  New WMI command types
 * were introduced that exceeds 128-byte data size.  Changes to handle
 * the data size allocation scheme were kept in hp_wmi_perform_qurey function.
 */
struct bios_args {
	u32 signature;
	u32 command;
	u32 commandtype;
	u32 datasize;
	u8 data[];
};

enum hp_wmi_commandtype {
	HPWMI_DISPLAY_QUERY		= 0x01,
	HPWMI_HDDTEMP_QUERY		= 0x02,
	HPWMI_ALS_QUERY			= 0x03,
	HPWMI_HARDWARE_QUERY		= 0x04,
	HPWMI_WIRELESS_QUERY		= 0x05,
	HPWMI_BATTERY_QUERY		= 0x07,
	HPWMI_BIOS_QUERY		= 0x09,
	HPWMI_FEATURE_QUERY		= 0x0b,
	HPWMI_HOTKEY_QUERY		= 0x0c,
	HPWMI_FEATURE2_QUERY		= 0x0d,
	HPWMI_WIRELESS2_QUERY		= 0x1b,
	HPWMI_POSTCODEERROR_QUERY	= 0x2a,
	HPWMI_SYSTEM_DEVICE_MODE	= 0x40,
	HPWMI_THERMAL_PROFILE_QUERY	= 0x4c,
};

struct victus_power_limits {
	u8 pl1;
	u8 pl2;
	u8 pl4;
	u8 cpu_gpu_concurrent_limit;
};

struct victus_gpu_power_modes {
	u8 ctgp_enable;
	u8 ppab_enable;
	u8 dstate;
	u8 gpu_slowdown_temp;
};

enum hp_wmi_keyboard_commandtype {
    HPWMI_KEYBOARD_BACKLIGHT_SUPPORT_QUERY = 0x01,
    HPWMI_KEYBOARD_COLOR_GET_QUERY = 0x02,
    HPWMI_KEYBOARD_COLOR_SET_QUERY = 0x03,
    HPWMI_KEYBOARD_BACKLIGHT_GET_QUERY = 0x04,
    HPWMI_KEYBOARD_BACKLIGHT_SET_QUERY = 0x05,
};

enum hp_wmi_gm_commandtype {
	HPWMI_FAN_SPEED_GET_QUERY		= 0x11,
	HPWMI_SET_PERFORMANCE_MODE		= 0x1A,
	HPWMI_FAN_SPEED_MAX_GET_QUERY		= 0x26,
	HPWMI_FAN_SPEED_MAX_SET_QUERY		= 0x27,
	HPWMI_GET_SYSTEM_DESIGN_DATA		= 0x28,
	HPWMI_FAN_COUNT_GET_QUERY		= 0x10,
	HPWMI_GET_GPU_THERMAL_MODES_QUERY	= 0x21,
	HPWMI_SET_GPU_THERMAL_MODES_QUERY	= 0x22,
	HPWMI_SET_POWER_LIMITS_QUERY		= 0x29,
	HPWMI_VICTUS_S_FAN_SPEED_GET_QUERY	= 0x2D,
	HPWMI_FAN_SPEED_SET_QUERY		= 0x2E,
};

enum hp_wmi_command {
	HPWMI_READ	= 0x01,
	HPWMI_WRITE	= 0x02,
	HPWMI_ODM	= 0x03,
	HPWMI_GM	= 0x20008,
};

enum hp_wmi_hardware_mask {
	HPWMI_DOCK_MASK		= 0x01,
	HPWMI_TABLET_MASK	= 0x04,
};

struct bios_return {
	u32 sigpass;
	u32 return_code;
};

enum hp_return_value {
	HPWMI_RET_WRONG_SIGNATURE	= 0x02,
	HPWMI_RET_UNKNOWN_COMMAND	= 0x03,
	HPWMI_RET_UNKNOWN_CMDTYPE	= 0x04,
	HPWMI_RET_INVALID_PARAMETERS	= 0x05,
};

enum hp_wireless2_bits {
	HPWMI_POWER_STATE	= 0x01,
	HPWMI_POWER_SOFT	= 0x02,
	HPWMI_POWER_BIOS	= 0x04,
	HPWMI_POWER_HARD	= 0x08,
	HPWMI_POWER_FW_OR_HW	= HPWMI_POWER_BIOS | HPWMI_POWER_HARD,
};

enum hp_thermal_profile_omen_v0 {
	HP_OMEN_V0_THERMAL_PROFILE_DEFAULT     = 0x00,
	HP_OMEN_V0_THERMAL_PROFILE_PERFORMANCE = 0x01,
	HP_OMEN_V0_THERMAL_PROFILE_COOL        = 0x02,
};

enum hp_thermal_profile_omen_v1 {
	HP_OMEN_V1_THERMAL_PROFILE_DEFAULT	= 0x30,
	HP_OMEN_V1_THERMAL_PROFILE_PERFORMANCE	= 0x31,
	HP_OMEN_V1_THERMAL_PROFILE_COOL		= 0x100,
};

enum hp_thermal_profile_omen_flags {
	HP_OMEN_EC_FLAGS_TURBO		= 0x04,
	HP_OMEN_EC_FLAGS_NOTIMER	= 0x02,
	HP_OMEN_EC_FLAGS_JUSTSET	= 0x01,
};

enum hp_wmi_keyboard_command {
    HPWMI_KEYBOARD_CMD = 0x20009,  // Keyboard control command from OmenMon
};

enum hp_thermal_profile_victus {
	HP_VICTUS_THERMAL_PROFILE_DEFAULT		= 0x00,
	HP_VICTUS_THERMAL_PROFILE_PERFORMANCE		= 0x01,
	HP_VICTUS_THERMAL_PROFILE_QUIET			= 0x03,
};

enum hp_thermal_profile_victus_s {
	HP_VICTUS_S_THERMAL_PROFILE_DEFAULT		= 0x00,
	HP_VICTUS_S_THERMAL_PROFILE_PERFORMANCE		= 0x01,
};

enum hp_thermal_profile {
	HP_THERMAL_PROFILE_PERFORMANCE	= 0x00,
	HP_THERMAL_PROFILE_DEFAULT		= 0x01,
	HP_THERMAL_PROFILE_COOL			= 0x02,
	HP_THERMAL_PROFILE_QUIET		= 0x03,
};

#define IS_HWBLOCKED(x) ((x & HPWMI_POWER_FW_OR_HW) != HPWMI_POWER_FW_OR_HW)
#define IS_SWBLOCKED(x) !(x & HPWMI_POWER_SOFT)



struct bios_rfkill2_device_state {
	u8 radio_type;
	u8 bus_type;
	u16 vendor_id;
	u16 product_id;
	u16 subsys_vendor_id;
	u16 subsys_product_id;
	u8 rfkill_id;
	u8 power;
	u8 unknown[4];
};

/* 7 devices fit into the 128 byte buffer */
#define HPWMI_MAX_RFKILL2_DEVICES	7

struct bios_rfkill2_state {
	u8 unknown[7];
	u8 count;
	u8 pad[8];
	struct bios_rfkill2_device_state device[HPWMI_MAX_RFKILL2_DEVICES];
};

static const struct key_entry hp_wmi_keymap[] = {
	{ KE_KEY, 0x02,    { KEY_BRIGHTNESSUP } },
	{ KE_KEY, 0x03,    { KEY_BRIGHTNESSDOWN } },
	{ KE_KEY, 0x270,   { KEY_MICMUTE } },
	{ KE_KEY, 0x20e6,  { KEY_PROG1 } },
	{ KE_KEY, 0x20e8,  { KEY_MEDIA } },
	{ KE_KEY, 0x2142,  { KEY_MEDIA } },
	{ KE_KEY, 0x213b,  { KEY_INFO } },
	{ KE_KEY, 0x2169,  { KEY_ROTATE_DISPLAY } },
	{ KE_KEY, 0x216a,  { KEY_SETUP } },
	{ KE_IGNORE, 0x21a4,  }, /* Win Lock On */
	{ KE_IGNORE, 0x121a4, }, /* Win Lock Off */
	{ KE_KEY, 0x21a5,  { KEY_PROG2 } }, /* HP Omen Key */
	{ KE_KEY, 0x21a7,  { KEY_FN_ESC } },
	{ KE_KEY, 0x21a8,  { KEY_PROG2 } }, /* HP Envy x360 programmable key */
	{ KE_KEY, 0x21a9,  { KEY_TOUCHPAD_OFF } },
	{ KE_KEY, 0x121a9, { KEY_TOUCHPAD_ON } },
	{ KE_KEY, 0x231b,  { KEY_HELP } },
	{ KE_END, 0 }
};

// RGB Color structure for individual zones
struct hp_omen_rgb_color {
    u8 red;
    u8 green;
    u8 blue;
} __packed;

#define HP_OMEN_KEYBOARD_ZONES 4
#define HP_OMEN_COLOR_TABLE_PADDING 24

struct hp_omen_keyboard_colors {
    u8 zone_count;  // Number of zones (should be 3 for 4 zones, 0-indexed)
    u8 padding[HP_OMEN_COLOR_TABLE_PADDING - 1];  // BIOS required padding
    struct hp_omen_rgb_color zones[HP_OMEN_KEYBOARD_ZONES];  // Zone colors
} __packed;

// Keyboard zones enum (based on OmenMon KbdZone)
enum hp_omen_keyboard_zone {
    HP_OMEN_ZONE_RIGHT = 0,   // Right side (arrows, nav keys)
    HP_OMEN_ZONE_MIDDLE = 1,  // Middle section (F6-F12, right QWERTY)
    HP_OMEN_ZONE_LEFT = 2,    // Left section (F1-F5, left QWERTY)  
    HP_OMEN_ZONE_WASD = 3,    // WASD keys
};

// Global variable to track keyboard RGB support
static bool hp_omen_keyboard_rgb_support = false;

/* Forward declarations */
static int hp_wmi_perform_query(int query, enum hp_wmi_command command,
                                void *buffer, int insize, int outsize);

/* Forward-declare the existing static fan control functions */
static int hp_wmi_fan_speed_max_set(int enabled);
static int hp_wmi_fan_speed_set_unified(int percentage);
static int hp_wmi_fan_get_average_speed(void);

/* Our new helpers to detect and cache max RPM */
static int detected_max_rpm = -1;
static int hp_wmi_detect_max_fan_rpm(void);
static int hp_wmi_get_max_fan_rpm(void);

static int hp_omen_keyboard_set_colors(const struct hp_omen_keyboard_colors *colors);

/* Probe-time detection, caches max RPM */
static int hp_wmi_detect_max_fan_rpm(void)
{
    int prev_mode = unified_manual_mode;
    int prev_speed = unified_fan_speed;
    int max_rpm;

    /* Force max mode, wait, read average, clamp */
    hp_wmi_fan_speed_max_set(1);
    msleep(200);
    max_rpm = hp_wmi_fan_get_average_speed();

    /* Restore previous mode */
    hp_wmi_fan_speed_reset()

    return detected_max_rpm = max_rpm;
}

static int hp_wmi_get_max_fan_rpm(void)
{
    if (detected_max_rpm < 0)
        return hp_wmi_detect_max_fan_rpm();
    return detected_max_rpm;
}


static int hp_omen_keyboard_check_support(void)
{
    u8 support_data[4] = {0};
    int ret;

    ret = hp_wmi_perform_query(HPWMI_KEYBOARD_BACKLIGHT_SUPPORT_QUERY, 
                              HPWMI_KEYBOARD_CMD,
                              support_data, sizeof(support_data), sizeof(support_data));

    if (ret != 0) {
        pr_debug("Keyboard RGB support query failed: %d\n", ret);
        return 0; // Assume not supported on failure
    }

    // Check if bit 0 indicates backlight support (from OmenMon analysis)
    return (support_data[0] & 0x01) ? 1 : 0;
}

/**
 * hp_omen_keyboard_get_colors - Get current keyboard RGB colors
 * @colors: Output buffer for color data
 * Returns: 0 on success, negative on error
 */
static int hp_omen_keyboard_get_colors(struct hp_omen_keyboard_colors *colors)
{
    int ret;

    if (!colors)
        return -EINVAL;

    memset(colors, 0, sizeof(*colors));

    ret = hp_wmi_perform_query(HPWMI_KEYBOARD_COLOR_GET_QUERY,
                              HPWMI_KEYBOARD_CMD,
                              colors, sizeof(*colors), sizeof(*colors));

    if (ret != 0) {
        pr_warn("Failed to get keyboard colors: %d\n", ret);
        return ret;
    }

    return 0;
}
/*
 * Mutex for the active_platform_profile variable,
 * see omen_powersource_event.
 */
static DEFINE_MUTEX(active_platform_profile_lock);

static struct input_dev *hp_wmi_input_dev;
static struct input_dev *camera_shutter_input_dev;
static struct platform_device *hp_wmi_platform_dev;
static struct device *platform_profile_device;
static struct notifier_block platform_power_source_nb;
static enum platform_profile_option active_platform_profile;
static bool platform_profile_support;
static bool zero_insize_support;


static struct rfkill *wifi_rfkill;
static struct rfkill *bluetooth_rfkill;
static struct rfkill *wwan_rfkill;

struct rfkill2_device {
	u8 id;
	int num;
	struct rfkill *rfkill;
};

static int rfkill2_count;
static struct rfkill2_device rfkill2[HPWMI_MAX_RFKILL2_DEVICES];

/*
 * Chassis Types values were obtained from SMBIOS reference
 * specification version 3.00. A complete list of system enclosures
 * and chassis types is available on Table 17.
 */
static const char * const tablet_chassis_types[] = {
	"30", /* Tablet*/
	"31", /* Convertible */
	"32"  /* Detachable */
};

#define DEVICE_MODE_TABLET	0x06

/* map output size to the corresponding WMI method id */
static inline int encode_outsize_for_pvsz(int outsize)
{
	if (outsize > 4096)
		return -EINVAL;
	if (outsize > 1024)
		return 5;
	if (outsize > 128)
		return 4;
	if (outsize > 4)
		return 3;
	if (outsize > 0)
		return 2;
	return 1;
}

/*
 * hp_wmi_perform_query
 *
 * query:	The commandtype (enum hp_wmi_commandtype)
 * write:	The command (enum hp_wmi_command)
 * buffer:	Buffer used as input and/or output
 * insize:	Size of input buffer
 * outsize:	Size of output buffer
 *
 * returns zero on success
 *         an HP WMI query specific error code (which is positive)
 *         -EINVAL if the query was not successful at all
 *         -EINVAL if the output buffer size exceeds buffersize
 *
 * Note: The buffersize must at least be the maximum of the input and output
 *       size. E.g. Battery info query is defined to have 1 byte input
 *       and 128 byte output. The caller would do:
 *       buffer = kzalloc(128, GFP_KERNEL);
 *       ret = hp_wmi_perform_query(HPWMI_BATTERY_QUERY, HPWMI_READ, buffer, 1, 128)
 */

 static bool is_victus_s_thermal_profile(void);
static void stop_fan_mode_watcher(void);

 /* Forward declarations */
static int hp_wmi_perform_query(int query, enum hp_wmi_command command,
				void *buffer, int insize, int outsize)
{
	struct acpi_buffer input, output = { ACPI_ALLOCATE_BUFFER, NULL };
	struct bios_return *bios_return;
	union acpi_object *obj = NULL;
	struct bios_args *args = NULL;
	int mid, actual_insize, actual_outsize;
	size_t bios_args_size;
	int ret;

	mid = encode_outsize_for_pvsz(outsize);
	if (WARN_ON(mid < 0))
		return mid;

	actual_insize = max(insize, 128);
	bios_args_size = struct_size(args, data, actual_insize);
	args = kmalloc(bios_args_size, GFP_KERNEL);
	if (!args)
		return -ENOMEM;

	input.length = bios_args_size;
	input.pointer = args;

	args->signature = 0x55434553;
	args->command = command;
	args->commandtype = query;
	args->datasize = insize;
	memcpy(args->data, buffer, flex_array_size(args, data, insize));

	ret = wmi_evaluate_method(HPWMI_BIOS_GUID, 0, mid, &input, &output);
	if (ret)
		goto out_free;

	obj = output.pointer;
	if (!obj) {
		ret = -EINVAL;
		goto out_free;
	}

	if (obj->type != ACPI_TYPE_BUFFER) {
		pr_warn("query 0x%x returned an invalid object 0x%x\n", query, ret);
		ret = -EINVAL;
		goto out_free;
	}

	bios_return = (struct bios_return *)obj->buffer.pointer;
	ret = bios_return->return_code;

	if (ret) {
		if (ret != HPWMI_RET_UNKNOWN_COMMAND &&
		    ret != HPWMI_RET_UNKNOWN_CMDTYPE)
			pr_warn("query 0x%x returned error 0x%x\n", query, ret);
		goto out_free;
	}

	/* Ignore output data of zero size */
	if (!outsize)
		goto out_free;

	actual_outsize = min(outsize, (int)(obj->buffer.length - sizeof(*bios_return)));
	memcpy(buffer, obj->buffer.pointer + sizeof(*bios_return), actual_outsize);
	memset(buffer + actual_outsize, 0, outsize - actual_outsize);

out_free:
	kfree(obj);
	kfree(args);
	return ret;
}

/*
 * Calling this hp_wmi_get_fan_count_userdefine_trigger function also enables
 * and/or maintains the laptop in user defined thermal and fan states, instead
 * of using a fallback state. After a 120 seconds timeout however, the laptop
 * goes back to its fallback state.
 */
static int hp_wmi_get_fan_count_userdefine_trigger(void)
{
	u8 fan_data[4] = {};
	int ret;

	ret = hp_wmi_perform_query(HPWMI_FAN_COUNT_GET_QUERY, HPWMI_GM,
				   &fan_data, sizeof(u8),
				   sizeof(fan_data));
	if (ret != 0)
		return -EINVAL;

	return fan_data[0]; /* Others bytes aren't providing fan count */
}

static int hp_wmi_get_fan_speed(int fan)
{
	u8 fsh, fsl;
	char fan_data[4] = { fan, 0, 0, 0 };

	int ret = hp_wmi_perform_query(HPWMI_FAN_SPEED_GET_QUERY, HPWMI_GM,
				       &fan_data, sizeof(char),
				       sizeof(fan_data));

	if (ret != 0)
		return -EINVAL;

	fsh = fan_data[2];
	fsl = fan_data[3];

	return (fsh << 8) | fsl;
}

static int hp_wmi_get_fan_speed_victus_s(int fan)
{
	u8 fan_data[128] = {};
	int ret;

	if (fan < 0 || fan >= sizeof(fan_data))
		return -EINVAL;

	ret = hp_wmi_perform_query(HPWMI_VICTUS_S_FAN_SPEED_GET_QUERY,
				   HPWMI_GM, &fan_data, sizeof(u8),
				   sizeof(fan_data));
	if (ret != 0)
		return -EINVAL;

	return fan_data[fan] * 100;
}

static int hp_wmi_read_int(int query)
{
	int val = 0, ret;

	ret = hp_wmi_perform_query(query, HPWMI_READ, &val,
				   zero_if_sup(val), sizeof(val));

	if (ret)
		return ret < 0 ? ret : -EINVAL;

	return val;
}

static int hp_wmi_get_dock_state(void)
{
	int state = hp_wmi_read_int(HPWMI_HARDWARE_QUERY);

	if (state < 0)
		return state;

	return !!(state & HPWMI_DOCK_MASK);
}

static int hp_wmi_get_tablet_mode(void)
{
	char system_device_mode[4] = { 0 };
	const char *chassis_type;
	bool tablet_found;
	int ret;

	chassis_type = dmi_get_system_info(DMI_CHASSIS_TYPE);
	if (!chassis_type)
		return -ENODEV;

	tablet_found = match_string(tablet_chassis_types,
				    ARRAY_SIZE(tablet_chassis_types),
				    chassis_type) >= 0;
	if (!tablet_found)
		return -ENODEV;

	ret = hp_wmi_perform_query(HPWMI_SYSTEM_DEVICE_MODE, HPWMI_READ,
				   system_device_mode, zero_if_sup(system_device_mode),
				   sizeof(system_device_mode));
	if (ret < 0)
		return ret;

	return system_device_mode[0] == DEVICE_MODE_TABLET;
}

static int omen_thermal_profile_set(int mode)
{
	/* The Omen Control Center actively sets the first byte of the buffer to
	 * 255, so let's mimic this behaviour to be as close as possible to
	 * the original software.
	 */
	char buffer[2] = {-1, mode};
	int ret;

	ret = hp_wmi_perform_query(HPWMI_SET_PERFORMANCE_MODE, HPWMI_GM,
				   &buffer, sizeof(buffer), 0);

	if (ret)
		return ret < 0 ? ret : -EINVAL;

	return mode;
}

static bool is_omen_thermal_profile(void)
{
	const char *board_name = dmi_get_system_info(DMI_BOARD_NAME);

	if (!board_name)
		return false;

	return match_string(omen_thermal_profile_boards,
			    ARRAY_SIZE(omen_thermal_profile_boards),
			    board_name) >= 0;
}

static int omen_get_thermal_policy_version(void)
{
	unsigned char buffer[8] = { 0 };
	int ret;

	const char *board_name = dmi_get_system_info(DMI_BOARD_NAME);

	if (board_name) {
		int matches = match_string(omen_thermal_profile_force_v0_boards,
			ARRAY_SIZE(omen_thermal_profile_force_v0_boards),
			board_name);
		if (matches >= 0)
			return 0;
	}

	ret = hp_wmi_perform_query(HPWMI_GET_SYSTEM_DESIGN_DATA, HPWMI_GM,
				   &buffer, sizeof(buffer), sizeof(buffer));

	if (ret)
		return ret < 0 ? ret : -EINVAL;

	return buffer[3];
}

static int omen_thermal_profile_get(void)
{
	u8 data;

	int ret = ec_read(HP_OMEN_EC_THERMAL_PROFILE_OFFSET, &data);

	if (ret)
		return ret;

	return data;
}

static int hp_wmi_fan_speed_max_set(int enabled)
{
	int ret;

	ret = hp_wmi_perform_query(HPWMI_FAN_SPEED_MAX_SET_QUERY, HPWMI_GM,
				   &enabled, sizeof(enabled), 0);

	if (ret)
		return ret < 0 ? ret : -EINVAL;

	return enabled;
}

static int hp_wmi_fan_speed_reset(void)
{
	u8 fan_speed[2] = { HP_FAN_SPEED_AUTOMATIC, HP_FAN_SPEED_AUTOMATIC };
	int ret;

	ret = hp_wmi_perform_query(HPWMI_FAN_SPEED_SET_QUERY, HPWMI_GM,
				   &fan_speed, sizeof(fan_speed), 0);

	return ret;
}

static int hp_wmi_fan_speed_max_reset(void)
{
	int ret;

	ret = hp_wmi_fan_speed_max_set(0);
	if (ret)
		return ret;

	/* Disabling max fan speed on Victus s1xxx laptops needs a 2nd step: */
	ret = hp_wmi_fan_speed_reset();
	return ret;
}

static int hp_wmi_fan_speed_max_get(void)
{
	int val = 0, ret;

	ret = hp_wmi_perform_query(HPWMI_FAN_SPEED_MAX_GET_QUERY, HPWMI_GM,
				   &val, zero_if_sup(val), sizeof(val));

	if (ret)
		return ret < 0 ? ret : -EINVAL;

	return val;
}

/**
 * hp_wmi_fan_speed_set_unified - Set both CPU and GPU fans to same percentage
 * @percentage: Speed percentage (0-100), -1 for automatic
 * Returns: 0 on success, negative on error
 * 
 * This follows OmenMon's approach of controlling both fans together
 */
static int hp_wmi_fan_speed_set_unified(int percentage)
{
    u8 fan_data[4];
    int ret;

    if (percentage < -1 || percentage > 100)
        return -EINVAL;

    if (percentage == -1) {
        fan_data[0] = HP_FAN_SPEED_AUTOMATIC;
        fan_data[1] = HP_FAN_SPEED_AUTOMATIC;
        unified_fan_speed = -1;
        unified_manual_mode = false;
        pr_debug("Set both fans to automatic\n");
    } else {
        u8 speed_value = (u8)percentage;
        if (speed_value == 0)
            speed_value = 1; // Avoid complete stop

        fan_data[0] = speed_value;
        fan_data[1] = speed_value;
        unified_fan_speed = percentage;
        unified_manual_mode = true;
        last_manual_speed = percentage;
        pr_debug("Set both fans to manual %d%%\n", percentage);
    }

    fan_data[2] = 0x00;
    fan_data[3] = 0x00;

    ret = hp_wmi_perform_query(HPWMI_FAN_SPEED_SET_QUERY, HPWMI_GM,
            fan_data, sizeof(fan_data), 0);
    if (ret != 0) {
        pr_warn("Failed to set unified fan speed: %d\n", ret);
        return ret;
    }

    return 0;
}

/**
 * hp_wmi_fan_speed_get_unified - Get current unified fan mode and speed
 * Returns: Current percentage (0-100) if manual, -1 if automatic, negative on error
 */
static int hp_wmi_fan_speed_get_unified(void)
{
    return unified_fan_speed;
}

/**
 * hp_wmi_fan_speed_is_manual - Check if fans are in manual mode
 * Returns: true if manual, false if automatic
 */
static bool hp_wmi_fan_speed_is_manual(void)
{
    return unified_manual_mode;
}

/**
 * hp_wmi_fan_get_average_speed - Get average RPM of both fans
 * Returns: Average fan speed in RPM, negative on error
 */
static int hp_wmi_fan_get_average_speed(void)
{
    int cpu_speed, gpu_speed;
    
    if (is_victus_s_thermal_profile()) {
        cpu_speed = hp_wmi_get_fan_speed_victus_s(0);
        gpu_speed = hp_wmi_get_fan_speed_victus_s(1); 
    } else {
        cpu_speed = hp_wmi_get_fan_speed(0);
        gpu_speed = hp_wmi_get_fan_speed(1);
    }
    
    if (cpu_speed < 0 && gpu_speed < 0)
        return -EINVAL;
    
    // If one fan fails, return the other
    if (cpu_speed < 0) return gpu_speed;
    if (gpu_speed < 0) return cpu_speed;
    
    // Return average of both
    return (cpu_speed + gpu_speed) / 2;
}

/**
 * hp_wmi_fan_get_max_unified - Get maximum RPM for percentage calculations
 * Returns: Max average RPM, negative on error
 */
static int hp_wmi_fan_get_max_unified(void)
{
    return hp_wmi_get_max_fan_rpm();
}


static int __init hp_wmi_bios_2008_later(void)
{
	int state = 0;
	int ret = hp_wmi_perform_query(HPWMI_FEATURE_QUERY, HPWMI_READ, &state,
				       zero_if_sup(state), sizeof(state));
	if (!ret)
		return 1;

	return (ret == HPWMI_RET_UNKNOWN_CMDTYPE) ? 0 : -ENXIO;
}

static int __init hp_wmi_bios_2009_later(void)
{
	u8 state[128];
	int ret = hp_wmi_perform_query(HPWMI_FEATURE2_QUERY, HPWMI_READ, &state,
				       zero_if_sup(state), sizeof(state));
	if (!ret)
		return 1;

	return (ret == HPWMI_RET_UNKNOWN_CMDTYPE) ? 0 : -ENXIO;
}

static int __init hp_wmi_enable_hotkeys(void)
{
	int value = 0x6e;
	int ret = hp_wmi_perform_query(HPWMI_BIOS_QUERY, HPWMI_WRITE, &value,
				       sizeof(value), 0);

	return ret <= 0 ? ret : -EINVAL;
}

static int hp_wmi_set_block(void *data, bool blocked)
{
	enum hp_wmi_radio r = (long)data;
	int query = BIT(r + 8) | ((!blocked) << r);
	int ret;

	ret = hp_wmi_perform_query(HPWMI_WIRELESS_QUERY, HPWMI_WRITE,
				   &query, sizeof(query), 0);

	return ret <= 0 ? ret : -EINVAL;
}

static const struct rfkill_ops hp_wmi_rfkill_ops = {
	.set_block = hp_wmi_set_block,
};

static bool hp_wmi_get_sw_state(enum hp_wmi_radio r)
{
	int mask = 0x200 << (r * 8);

	int wireless = hp_wmi_read_int(HPWMI_WIRELESS_QUERY);

	/* TBD: Pass error */
	WARN_ONCE(wireless < 0, "error executing HPWMI_WIRELESS_QUERY");

	return !(wireless & mask);
}

static bool hp_wmi_get_hw_state(enum hp_wmi_radio r)
{
	int mask = 0x800 << (r * 8);

	int wireless = hp_wmi_read_int(HPWMI_WIRELESS_QUERY);

	/* TBD: Pass error */
	WARN_ONCE(wireless < 0, "error executing HPWMI_WIRELESS_QUERY");

	return !(wireless & mask);
}

static int hp_wmi_rfkill2_set_block(void *data, bool blocked)
{
	int rfkill_id = (int)(long)data;
	char buffer[4] = { 0x01, 0x00, rfkill_id, !blocked };
	int ret;

	ret = hp_wmi_perform_query(HPWMI_WIRELESS2_QUERY, HPWMI_WRITE,
				   buffer, sizeof(buffer), 0);

	return ret <= 0 ? ret : -EINVAL;
}

static const struct rfkill_ops hp_wmi_rfkill2_ops = {
	.set_block = hp_wmi_rfkill2_set_block,
};

static int hp_wmi_rfkill2_refresh(void)
{
	struct bios_rfkill2_state state;
	int err, i;

	err = hp_wmi_perform_query(HPWMI_WIRELESS2_QUERY, HPWMI_READ, &state,
				   zero_if_sup(state), sizeof(state));
	if (err)
		return err;

	for (i = 0; i < rfkill2_count; i++) {
		int num = rfkill2[i].num;
		struct bios_rfkill2_device_state *devstate;

		devstate = &state.device[num];

		if (num >= state.count ||
		    devstate->rfkill_id != rfkill2[i].id) {
			pr_warn("power configuration of the wireless devices unexpectedly changed\n");
			continue;
		}

		rfkill_set_states(rfkill2[i].rfkill,
				  IS_SWBLOCKED(devstate->power),
				  IS_HWBLOCKED(devstate->power));
	}

	return 0;
}

static ssize_t display_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	int value = hp_wmi_read_int(HPWMI_DISPLAY_QUERY);

	if (value < 0)
		return value;
	return sysfs_emit(buf, "%d\n", value);
}

static ssize_t hddtemp_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	int value = hp_wmi_read_int(HPWMI_HDDTEMP_QUERY);

	if (value < 0)
		return value;
	return sysfs_emit(buf, "%d\n", value);
}

static ssize_t als_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	int value = hp_wmi_read_int(HPWMI_ALS_QUERY);

	if (value < 0)
		return value;
	return sysfs_emit(buf, "%d\n", value);
}

static ssize_t dock_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	int value = hp_wmi_get_dock_state();

	if (value < 0)
		return value;
	return sysfs_emit(buf, "%d\n", value);
}

static ssize_t tablet_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	int value = hp_wmi_get_tablet_mode();

	if (value < 0)
		return value;
	return sysfs_emit(buf, "%d\n", value);
}

static ssize_t postcode_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	/* Get the POST error code of previous boot failure. */
	int value = hp_wmi_read_int(HPWMI_POSTCODEERROR_QUERY);

	if (value < 0)
		return value;
	return sysfs_emit(buf, "0x%x\n", value);
}

static ssize_t als_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	u32 tmp;
	int ret;

	ret = kstrtou32(buf, 10, &tmp);
	if (ret)
		return ret;

	ret = hp_wmi_perform_query(HPWMI_ALS_QUERY, HPWMI_WRITE, &tmp,
				       sizeof(tmp), 0);
	if (ret)
		return ret < 0 ? ret : -EINVAL;

	return count;
}

static ssize_t postcode_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	u32 tmp = 1;
	bool clear;
	int ret;

	ret = kstrtobool(buf, &clear);
	if (ret)
		return ret;

	if (clear == false)
		return -EINVAL;

	/* Clear the POST error code. It is kept until cleared. */
	ret = hp_wmi_perform_query(HPWMI_POSTCODEERROR_QUERY, HPWMI_WRITE, &tmp,
				       sizeof(tmp), 0);
	if (ret)
		return ret < 0 ? ret : -EINVAL;

	return count;
}

static int camera_shutter_input_setup(void)
{
	int err;

	camera_shutter_input_dev = input_allocate_device();
	if (!camera_shutter_input_dev)
		return -ENOMEM;

	camera_shutter_input_dev->name = "HP WMI camera shutter";
	camera_shutter_input_dev->phys = "wmi/input1";
	camera_shutter_input_dev->id.bustype = BUS_HOST;

	__set_bit(EV_SW, camera_shutter_input_dev->evbit);
	__set_bit(SW_CAMERA_LENS_COVER, camera_shutter_input_dev->swbit);

	err = input_register_device(camera_shutter_input_dev);
	if (err)
		goto err_free_dev;

	return 0;

 err_free_dev:
	input_free_device(camera_shutter_input_dev);
	camera_shutter_input_dev = NULL;
	return err;
}

static int hp_omen_keyboard_get_backlight_state(void)
{
    u8 state_data[4] = {0};
    int ret;

    ret = hp_wmi_perform_query(HPWMI_KEYBOARD_BACKLIGHT_GET_QUERY,
                              HPWMI_KEYBOARD_CMD,
                              state_data, sizeof(state_data), sizeof(state_data));

    if (ret != 0) {
        pr_warn("Failed to get backlight state: %d\n", ret);
        return ret;
    }

    // Based on OmenMon Backlight enum: 0x64 = off, 0xE4 = on
    return (state_data[0] == 0xE4) ? 1 : 0;
}

/**
 * hp_omen_keyboard_set_backlight_state - Set keyboard backlight on/off state
 * @state: 1 to turn on, 0 to turn off
 * Returns: 0 on success, negative on error
 */
static int hp_omen_keyboard_set_backlight_state(int state)
{
    u8 backlight_data[4];
    int ret;

    // Based on OmenMon Backlight enum values
    backlight_data[0] = state ? 0xE4 : 0x64;  // On : Off
    backlight_data[1] = 0x00;
    backlight_data[2] = 0x00;
    backlight_data[3] = 0x00;

    ret = hp_wmi_perform_query(HPWMI_KEYBOARD_BACKLIGHT_SET_QUERY,
                              HPWMI_KEYBOARD_CMD,
                              backlight_data, sizeof(backlight_data), 0);

    if (ret != 0) {
        pr_warn("Failed to set backlight state: %d\n", ret);
        return ret;
    }

    return 0;
}

static ssize_t keyboard_rgb_colors_show(struct device *dev,
                                       struct device_attribute *attr,
                                       char *buf)
{
    struct hp_omen_keyboard_colors colors;
    int ret;

    if (!hp_omen_keyboard_rgb_support)
        return -ENODEV;

    ret = hp_omen_keyboard_get_colors(&colors);
    if (ret)
        return ret;

    return sprintf(buf, "%02x%02x%02x:%02x%02x%02x:%02x%02x%02x:%02x%02x%02x\n",
                   colors.zones[HP_OMEN_ZONE_RIGHT].red,
                   colors.zones[HP_OMEN_ZONE_RIGHT].green,
                   colors.zones[HP_OMEN_ZONE_RIGHT].blue,
                   colors.zones[HP_OMEN_ZONE_MIDDLE].red,
                   colors.zones[HP_OMEN_ZONE_MIDDLE].green,
                   colors.zones[HP_OMEN_ZONE_MIDDLE].blue,
                   colors.zones[HP_OMEN_ZONE_LEFT].red,
                   colors.zones[HP_OMEN_ZONE_LEFT].green,
                   colors.zones[HP_OMEN_ZONE_LEFT].blue,
                   colors.zones[HP_OMEN_ZONE_WASD].red,
                   colors.zones[HP_OMEN_ZONE_WASD].green,
                   colors.zones[HP_OMEN_ZONE_WASD].blue);
}

/**
 * keyboard_rgb_colors_store - Set RGB colors for all zones
 * Format: "RRGGBB:RRGGBB:RRGGBB:RRGGBB" (Right:Middle:Left:WASD)
 */
static ssize_t keyboard_rgb_colors_store(struct device *dev,
                                        struct device_attribute *attr,
                                        const char *buf, size_t count)
{
    struct hp_omen_keyboard_colors colors;
    int ret;
    unsigned int r[4], g[4], b[4];

    if (!hp_omen_keyboard_rgb_support)
        return -ENODEV;

    // Parse format: "RRGGBB:RRGGBB:RRGGBB:RRGGBB"
    ret = sscanf(buf, "%02x%02x%02x:%02x%02x%02x:%02x%02x%02x:%02x%02x%02x",
                 &r[0], &g[0], &b[0],  // Right zone
                 &r[1], &g[1], &b[1],  // Middle zone
                 &r[2], &g[2], &b[2],  // Left zone
                 &r[3], &g[3], &b[3]); // WASD zone

    if (ret != 12) {
        pr_warn("Invalid RGB format. Use: RRGGBB:RRGGBB:RRGGBB:RRGGBB\n");
        return -EINVAL;
    }

    // Validate color values
    for (int i = 0; i < 4; i++) {
        if (r[i] > 255 || g[i] > 255 || b[i] > 255) {
            return -EINVAL;
        }
    }

    // Set up color structure
    memset(&colors, 0, sizeof(colors));
    colors.zone_count = 3; // 4 zones, 0-indexed (as per OmenMon)

    colors.zones[HP_OMEN_ZONE_RIGHT].red = r[0];
    colors.zones[HP_OMEN_ZONE_RIGHT].green = g[0];
    colors.zones[HP_OMEN_ZONE_RIGHT].blue = b[0];

    colors.zones[HP_OMEN_ZONE_MIDDLE].red = r[1];
    colors.zones[HP_OMEN_ZONE_MIDDLE].green = g[1];
    colors.zones[HP_OMEN_ZONE_MIDDLE].blue = b[1];

    colors.zones[HP_OMEN_ZONE_LEFT].red = r[2];
    colors.zones[HP_OMEN_ZONE_LEFT].green = g[2];
    colors.zones[HP_OMEN_ZONE_LEFT].blue = b[2];

    colors.zones[HP_OMEN_ZONE_WASD].red = r[3];
    colors.zones[HP_OMEN_ZONE_WASD].green = g[3];
    colors.zones[HP_OMEN_ZONE_WASD].blue = b[3];

    ret = hp_omen_keyboard_set_colors(&colors);
    if (ret)
        return ret;

    return count;
}

/**
 * keyboard_backlight_show - Show current backlight state
 */
static ssize_t keyboard_backlight_show(struct device *dev,
                                     struct device_attribute *attr,
                                     char *buf)
{
    int state;

    if (!hp_omen_keyboard_rgb_support)
        return -ENODEV;

    state = hp_omen_keyboard_get_backlight_state();
    if (state < 0)
        return state;

    return sprintf(buf, "%d\n", state);
}

/**
 * keyboard_backlight_store - Set backlight state
 */
static ssize_t keyboard_backlight_store(struct device *dev,
                                      struct device_attribute *attr,
                                      const char *buf, size_t count)
{
    int state, ret;

    if (!hp_omen_keyboard_rgb_support)
        return -ENODEV;

    ret = kstrtoint(buf, 0, &state);
    if (ret)
        return ret;

    if (state < 0 || state > 1)
        return -EINVAL;

    ret = hp_omen_keyboard_set_backlight_state(state);
    if (ret)
        return ret;

    return count;
}

// Individual zone control attributes
static ssize_t keyboard_zone_right_show(struct device *dev,
                                       struct device_attribute *attr,
                                       char *buf)
{
    struct hp_omen_keyboard_colors colors;
    int ret;

    if (!hp_omen_keyboard_rgb_support)
        return -ENODEV;

    ret = hp_omen_keyboard_get_colors(&colors);
    if (ret)
        return ret;

    return sprintf(buf, "%02x%02x%02x\n",
                   colors.zones[HP_OMEN_ZONE_RIGHT].red,
                   colors.zones[HP_OMEN_ZONE_RIGHT].green,
                   colors.zones[HP_OMEN_ZONE_RIGHT].blue);
}

static ssize_t keyboard_zone_right_store(struct device *dev,
                                        struct device_attribute *attr,
                                        const char *buf, size_t count)
{
    struct hp_omen_keyboard_colors colors;
    unsigned int r, g, b;
    int ret;

    if (!hp_omen_keyboard_rgb_support)
        return -ENODEV;

    ret = sscanf(buf, "%02x%02x%02x", &r, &g, &b);
    if (ret != 3 || r > 255 || g > 255 || b > 255)
        return -EINVAL;

    // Get current colors first to preserve other zones
    ret = hp_omen_keyboard_get_colors(&colors);
    if (ret)
        return ret;

    // Update only the right zone
    colors.zones[HP_OMEN_ZONE_RIGHT].red = r;
    colors.zones[HP_OMEN_ZONE_RIGHT].green = g;
    colors.zones[HP_OMEN_ZONE_RIGHT].blue = b;

    ret = hp_omen_keyboard_set_colors(&colors);
    if (ret)
        return ret;

    return count;
}

// Similar functions for other zones would go here...
// (keyboard_zone_middle_*, keyboard_zone_left_*, keyboard_zone_wasd_*)

// Device attributes
static DEVICE_ATTR_RW(keyboard_rgb_colors);
static DEVICE_ATTR_RW(keyboard_backlight);
static DEVICE_ATTR_RW(keyboard_zone_right);
// Add other zone attributes here...

// Add to the existing hp_wmi_attrs array:
static struct attribute *hp_wmi_keyboard_attrs[] = {
    &dev_attr_keyboard_rgb_colors.attr,
    &dev_attr_keyboard_backlight.attr,
    &dev_attr_keyboard_zone_right.attr,
    // Add other zone attributes here...
    NULL,
};

static const struct attribute_group hp_wmi_keyboard_attr_group = {
    .name = "keyboard_rgb",
    .attrs = hp_wmi_keyboard_attrs,
};

// ============================================================================
// Integration with existing hp-wmi.c
// ============================================================================

/**
 * hp_omen_keyboard_rgb_setup - Initialize keyboard RGB support
 * Add this function call to hp_wmi_bios_setup()
 */


 static int hp_omen_keyboard_set_colors(const struct hp_omen_keyboard_colors *colors)
{
    int ret;
    
    if (!colors)
        return -EINVAL;
    
    ret = hp_wmi_perform_query(HPWMI_KEYBOARD_COLOR_SET_QUERY,
                              HPWMI_KEYBOARD_CMD,
                              (void *)colors, sizeof(*colors), 0);
    
    if (ret != 0) {
        pr_warn("Failed to set keyboard colors: %d\n", ret);
        return ret;
    }
    
    return 0;
}

static int hp_omen_keyboard_rgb_setup(struct platform_device *device)
{
    int ret;

    // Check if keyboard RGB is supported
    ret = hp_omen_keyboard_check_support();
    if (ret <= 0) {
        pr_info("Keyboard RGB not supported or detection failed\n");
        hp_omen_keyboard_rgb_support = false;
        return 0; // Don't fail driver init if RGB not supported
    }

    hp_omen_keyboard_rgb_support = true;
    pr_info("Keyboard RGB support detected\n");

    // Create sysfs attribute group
    ret = sysfs_create_group(&device->dev.kobj, &hp_wmi_keyboard_attr_group);
    if (ret) {
        pr_err("Failed to create keyboard RGB sysfs attributes: %d\n", ret);
        hp_omen_keyboard_rgb_support = false;
        return ret;
    }

    pr_info("Keyboard RGB sysfs interface created\n");
    return 0;
}

/**
 * hp_omen_keyboard_rgb_remove - Clean up keyboard RGB support
 * Add this function call to hp_wmi_bios_remove()
 */
static void hp_omen_keyboard_rgb_remove(struct platform_device *device)
{
    if (hp_omen_keyboard_rgb_support) {
        sysfs_remove_group(&device->dev.kobj, &hp_wmi_keyboard_attr_group);
    }
}


static ssize_t fan_unified_show(struct device *dev,
                                struct device_attribute *attr,
                                char *buf)
{
    int cpu_rpm, gpu_rpm, avg_rpm;
    
    if (is_victus_s_thermal_profile()) {
        cpu_rpm = hp_wmi_get_fan_speed_victus_s(0);
        gpu_rpm = hp_wmi_get_fan_speed_victus_s(1);
    } else {
        cpu_rpm = hp_wmi_get_fan_speed(0);
        gpu_rpm = hp_wmi_get_fan_speed(1);
    }
    
    avg_rpm = hp_wmi_fan_get_average_speed();
    
    return sprintf(buf, "Mode: %s\nSpeed: %d%%\nCPU: %d RPM\nGPU: %d RPM\nAverage: %d RPM\n",
                   unified_manual_mode ? "Manual" : "Auto",
                   unified_manual_mode ? unified_fan_speed : -1,
                   cpu_rpm >= 0 ? cpu_rpm : 0,
                   gpu_rpm >= 0 ? gpu_rpm : 0, 
                   avg_rpm >= 0 ? avg_rpm : 0);
}

static ssize_t fan_unified_store(struct device *dev,
                                 struct device_attribute *attr,
                                 const char *buf, size_t count)
{
    int percentage;
    int ret;
    
    if (strncmp(buf, "auto", 4) == 0 || strncmp(buf, "automatic", 9) == 0) {
        ret = hp_wmi_fan_speed_set_unified(-1);  // Auto
    } else {
        ret = kstrtoint(buf, 0, &percentage);
        if (ret)
            return ret;
            
        if (percentage < 0 || percentage > 100)
            return -EINVAL;
            
        ret = hp_wmi_fan_speed_set_unified(percentage);
    }
    
    return ret ? ret : count;
}

static DEVICE_ATTR_RW(fan_unified);

static DEVICE_ATTR_RO(display);
static DEVICE_ATTR_RO(hddtemp);
static DEVICE_ATTR_RW(als);
static DEVICE_ATTR_RO(dock);
static DEVICE_ATTR_RO(tablet);
static DEVICE_ATTR_RW(postcode);

static struct attribute *hp_wmi_attrs[] = {
	&dev_attr_display.attr,
	&dev_attr_hddtemp.attr,
	&dev_attr_als.attr,
	&dev_attr_dock.attr,
	&dev_attr_tablet.attr,
	&dev_attr_postcode.attr,
	&dev_attr_fan_unified.attr,
	NULL,
};
ATTRIBUTE_GROUPS(hp_wmi);

static void hp_wmi_notify(union acpi_object *obj, void *context)
{
	u32 event_id, event_data;
	u32 *location;
	int key_code;

	if (!obj)
		return;
	if (obj->type != ACPI_TYPE_BUFFER) {
		pr_info("Unknown response received %d\n", obj->type);
		return;
	}

	/*
	 * Depending on ACPI version the concatenation of id and event data
	 * inside _WED function will result in a 8 or 16 byte buffer.
	 */
	location = (u32 *)obj->buffer.pointer;
	if (obj->buffer.length == 8) {
		event_id = *location;
		event_data = *(location + 1);
	} else if (obj->buffer.length == 16) {
		event_id = *location;
		event_data = *(location + 2);
	} else {
		pr_info("Unknown buffer length %d\n", obj->buffer.length);
		return;
	}

	switch (event_id) {
	case HPWMI_DOCK_EVENT:
		if (test_bit(SW_DOCK, hp_wmi_input_dev->swbit))
			input_report_switch(hp_wmi_input_dev, SW_DOCK,
					    hp_wmi_get_dock_state());
		if (test_bit(SW_TABLET_MODE, hp_wmi_input_dev->swbit))
			input_report_switch(hp_wmi_input_dev, SW_TABLET_MODE,
					    hp_wmi_get_tablet_mode());
		input_sync(hp_wmi_input_dev);
		break;
	case HPWMI_PARK_HDD:
		break;
	case HPWMI_SMART_ADAPTER:
		break;
	case HPWMI_BEZEL_BUTTON:
		key_code = hp_wmi_read_int(HPWMI_HOTKEY_QUERY);
		if (key_code < 0)
			break;

		if (!sparse_keymap_report_event(hp_wmi_input_dev,
						key_code, 1, true))
			pr_info("Unknown key code - 0x%x\n", key_code);
		break;
	case HPWMI_OMEN_KEY:
		if (event_data) /* Only should be true for HP Omen */
			key_code = event_data;
		else
			key_code = hp_wmi_read_int(HPWMI_HOTKEY_QUERY);

		if (!sparse_keymap_report_event(hp_wmi_input_dev,
						key_code, 1, true))
			pr_info("Unknown key code - 0x%x\n", key_code);
		break;
	case HPWMI_WIRELESS:
		if (rfkill2_count) {
			hp_wmi_rfkill2_refresh();
			break;
		}

		if (wifi_rfkill)
			rfkill_set_states(wifi_rfkill,
					  hp_wmi_get_sw_state(HPWMI_WIFI),
					  hp_wmi_get_hw_state(HPWMI_WIFI));
		if (bluetooth_rfkill)
			rfkill_set_states(bluetooth_rfkill,
					  hp_wmi_get_sw_state(HPWMI_BLUETOOTH),
					  hp_wmi_get_hw_state(HPWMI_BLUETOOTH));
		if (wwan_rfkill)
			rfkill_set_states(wwan_rfkill,
					  hp_wmi_get_sw_state(HPWMI_WWAN),
					  hp_wmi_get_hw_state(HPWMI_WWAN));
		break;
	case HPWMI_CPU_BATTERY_THROTTLE:
		pr_info("Unimplemented CPU throttle because of 3 Cell battery event detected\n");
		break;
	case HPWMI_LOCK_SWITCH:
		break;
	case HPWMI_LID_SWITCH:
		break;
	case HPWMI_SCREEN_ROTATION:
		break;
	case HPWMI_COOLSENSE_SYSTEM_MOBILE:
		break;
	case HPWMI_COOLSENSE_SYSTEM_HOT:
		break;
	case HPWMI_PROXIMITY_SENSOR:
		break;
	case HPWMI_BACKLIT_KB_BRIGHTNESS:
		break;
	case HPWMI_PEAKSHIFT_PERIOD:
		break;
	case HPWMI_BATTERY_CHARGE_PERIOD:
		break;
	case HPWMI_SANITIZATION_MODE:
		break;
	case HPWMI_CAMERA_TOGGLE:
		if (!camera_shutter_input_dev)
			if (camera_shutter_input_setup()) {
				pr_err("Failed to setup camera shutter input device\n");
				break;
			}
		if (event_data == 0xff)
			input_report_switch(camera_shutter_input_dev, SW_CAMERA_LENS_COVER, 1);
		else if (event_data == 0xfe)
			input_report_switch(camera_shutter_input_dev, SW_CAMERA_LENS_COVER, 0);
		else
			pr_warn("Unknown camera shutter state - 0x%x\n", event_data);
		input_sync(camera_shutter_input_dev);
		break;
	case HPWMI_SMART_EXPERIENCE_APP:
		break;
	default:
		pr_info("Unknown event_id - %d - 0x%x\n", event_id, event_data);
		break;
	}
}

static int __init hp_wmi_input_setup(void)
{
	acpi_status status;
	int err, val;

	hp_wmi_input_dev = input_allocate_device();
	if (!hp_wmi_input_dev)
		return -ENOMEM;

	hp_wmi_input_dev->name = "HP WMI hotkeys";
	hp_wmi_input_dev->phys = "wmi/input0";
	hp_wmi_input_dev->id.bustype = BUS_HOST;

	__set_bit(EV_SW, hp_wmi_input_dev->evbit);

	/* Dock */
	val = hp_wmi_get_dock_state();
	if (!(val < 0)) {
		__set_bit(SW_DOCK, hp_wmi_input_dev->swbit);
		input_report_switch(hp_wmi_input_dev, SW_DOCK, val);
	}

	/* Tablet mode */
	val = hp_wmi_get_tablet_mode();
	if (!(val < 0)) {
		__set_bit(SW_TABLET_MODE, hp_wmi_input_dev->swbit);
		input_report_switch(hp_wmi_input_dev, SW_TABLET_MODE, val);
	}

	err = sparse_keymap_setup(hp_wmi_input_dev, hp_wmi_keymap, NULL);
	if (err)
		goto err_free_dev;

	/* Set initial hardware state */
	input_sync(hp_wmi_input_dev);

	if (!hp_wmi_bios_2009_later() && hp_wmi_bios_2008_later())
		hp_wmi_enable_hotkeys();

	status = wmi_install_notify_handler(HPWMI_EVENT_GUID, hp_wmi_notify, NULL);
	if (ACPI_FAILURE(status)) {
		err = -EIO;
		goto err_free_dev;
	}

	err = input_register_device(hp_wmi_input_dev);
	if (err)
		goto err_uninstall_notifier;

	return 0;

 err_uninstall_notifier:
	wmi_remove_notify_handler(HPWMI_EVENT_GUID);
 err_free_dev:
	input_free_device(hp_wmi_input_dev);
	return err;
}

static void hp_wmi_input_destroy(void)
{
	wmi_remove_notify_handler(HPWMI_EVENT_GUID);
	input_unregister_device(hp_wmi_input_dev);
}

static int __init hp_wmi_rfkill_setup(struct platform_device *device)
{
	int err, wireless;

	wireless = hp_wmi_read_int(HPWMI_WIRELESS_QUERY);
	if (wireless < 0)
		return wireless;

	err = hp_wmi_perform_query(HPWMI_WIRELESS_QUERY, HPWMI_WRITE, &wireless,
				   sizeof(wireless), 0);
	if (err)
		return err;

	if (wireless & 0x1) {
		wifi_rfkill = rfkill_alloc("hp-wifi", &device->dev,
					   RFKILL_TYPE_WLAN,
					   &hp_wmi_rfkill_ops,
					   (void *) HPWMI_WIFI);
		if (!wifi_rfkill)
			return -ENOMEM;
		rfkill_init_sw_state(wifi_rfkill,
				     hp_wmi_get_sw_state(HPWMI_WIFI));
		rfkill_set_hw_state(wifi_rfkill,
				    hp_wmi_get_hw_state(HPWMI_WIFI));
		err = rfkill_register(wifi_rfkill);
		if (err)
			goto register_wifi_error;
	}

	if (wireless & 0x2) {
		bluetooth_rfkill = rfkill_alloc("hp-bluetooth", &device->dev,
						RFKILL_TYPE_BLUETOOTH,
						&hp_wmi_rfkill_ops,
						(void *) HPWMI_BLUETOOTH);
		if (!bluetooth_rfkill) {
			err = -ENOMEM;
			goto register_bluetooth_error;
		}
		rfkill_init_sw_state(bluetooth_rfkill,
				     hp_wmi_get_sw_state(HPWMI_BLUETOOTH));
		rfkill_set_hw_state(bluetooth_rfkill,
				    hp_wmi_get_hw_state(HPWMI_BLUETOOTH));
		err = rfkill_register(bluetooth_rfkill);
		if (err)
			goto register_bluetooth_error;
	}

	if (wireless & 0x4) {
		wwan_rfkill = rfkill_alloc("hp-wwan", &device->dev,
					   RFKILL_TYPE_WWAN,
					   &hp_wmi_rfkill_ops,
					   (void *) HPWMI_WWAN);
		if (!wwan_rfkill) {
			err = -ENOMEM;
			goto register_wwan_error;
		}
		rfkill_init_sw_state(wwan_rfkill,
				     hp_wmi_get_sw_state(HPWMI_WWAN));
		rfkill_set_hw_state(wwan_rfkill,
				    hp_wmi_get_hw_state(HPWMI_WWAN));
		err = rfkill_register(wwan_rfkill);
		if (err)
			goto register_wwan_error;
	}

	return 0;

register_wwan_error:
	rfkill_destroy(wwan_rfkill);
	wwan_rfkill = NULL;
	if (bluetooth_rfkill)
		rfkill_unregister(bluetooth_rfkill);
register_bluetooth_error:
	rfkill_destroy(bluetooth_rfkill);
	bluetooth_rfkill = NULL;
	if (wifi_rfkill)
		rfkill_unregister(wifi_rfkill);
register_wifi_error:
	rfkill_destroy(wifi_rfkill);
	wifi_rfkill = NULL;
	return err;
}

static int __init hp_wmi_rfkill2_setup(struct platform_device *device)
{
	struct bios_rfkill2_state state;
	int err, i;

	err = hp_wmi_perform_query(HPWMI_WIRELESS2_QUERY, HPWMI_READ, &state,
				   zero_if_sup(state), sizeof(state));
	if (err)
		return err < 0 ? err : -EINVAL;

	if (state.count > HPWMI_MAX_RFKILL2_DEVICES) {
		pr_warn("unable to parse 0x1b query output\n");
		return -EINVAL;
	}

	for (i = 0; i < state.count; i++) {
		struct rfkill *rfkill;
		enum rfkill_type type;
		char *name;

		switch (state.device[i].radio_type) {
		case HPWMI_WIFI:
			type = RFKILL_TYPE_WLAN;
			name = "hp-wifi";
			break;
		case HPWMI_BLUETOOTH:
			type = RFKILL_TYPE_BLUETOOTH;
			name = "hp-bluetooth";
			break;
		case HPWMI_WWAN:
			type = RFKILL_TYPE_WWAN;
			name = "hp-wwan";
			break;
		case HPWMI_GPS:
			type = RFKILL_TYPE_GPS;
			name = "hp-gps";
			break;
		default:
			pr_warn("unknown device type 0x%x\n",
				state.device[i].radio_type);
			continue;
		}

		if (!state.device[i].vendor_id) {
			pr_warn("zero device %d while %d reported\n",
				i, state.count);
			continue;
		}

		rfkill = rfkill_alloc(name, &device->dev, type,
				      &hp_wmi_rfkill2_ops, (void *)(long)i);
		if (!rfkill) {
			err = -ENOMEM;
			goto fail;
		}

		rfkill2[rfkill2_count].id = state.device[i].rfkill_id;
		rfkill2[rfkill2_count].num = i;
		rfkill2[rfkill2_count].rfkill = rfkill;

		rfkill_init_sw_state(rfkill,
				     IS_SWBLOCKED(state.device[i].power));
		rfkill_set_hw_state(rfkill,
				    IS_HWBLOCKED(state.device[i].power));

		if (!(state.device[i].power & HPWMI_POWER_BIOS))
			pr_info("device %s blocked by BIOS\n", name);

		err = rfkill_register(rfkill);
		if (err) {
			rfkill_destroy(rfkill);
			goto fail;
		}

		rfkill2_count++;
	}

	return 0;
fail:
	for (; rfkill2_count > 0; rfkill2_count--) {
		rfkill_unregister(rfkill2[rfkill2_count - 1].rfkill);
		rfkill_destroy(rfkill2[rfkill2_count - 1].rfkill);
	}
	return err;
}

// OMEN power/fan profile table
struct omen_power_profile {
    u8 cpu_pl1, cpu_pl2, cpu_pl4, cpu_combined;
    u8 gpu_ctgp, gpu_ppab, gpu_dstate, gpu_peak_temp;
};

static const struct omen_power_profile omen_profiles[] = {
    // Cool
    { 28, 30, 35,  40,   0, 0, 0, 75},
    // Balanced
    { 35, 40, 65, 0,   1, 0, 0, 80},
    // Performance (max everything)
    { 65, 65, 100, 0,   1, 1, 1, 87},
};

static int platform_profile_omen_get_ec(enum platform_profile_option *profile)
{
	int tp;

	tp = omen_thermal_profile_get();
	if (tp < 0)
		return tp;

	switch (tp) {
	case HP_OMEN_V0_THERMAL_PROFILE_PERFORMANCE:
	case HP_OMEN_V1_THERMAL_PROFILE_PERFORMANCE:
		*profile = PLATFORM_PROFILE_PERFORMANCE;
		break;
	case HP_OMEN_V0_THERMAL_PROFILE_DEFAULT:
	case HP_OMEN_V1_THERMAL_PROFILE_DEFAULT:
		*profile = PLATFORM_PROFILE_BALANCED;
		break;
	case HP_OMEN_V0_THERMAL_PROFILE_COOL:
	case HP_OMEN_V1_THERMAL_PROFILE_COOL:
		*profile = PLATFORM_PROFILE_COOL;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int platform_profile_omen_get(struct device *dev,
				     enum platform_profile_option *profile)
{
	/*
	 * We directly return the stored platform profile, as the embedded
	 * controller will not accept switching to the performance option when
	 * the conditions are not met (e.g. the laptop is not plugged in).
	 *
	 * If we directly return what the EC reports, the platform profile will
	 * immediately "switch back" to normal mode, which is against the
	 * expected behaviour from a userspace point of view, as described in
	 * the Platform Profile Section page of the kernel documentation.
	 *
	 * See also omen_powersource_event.
	 */
	guard(mutex)(&active_platform_profile_lock);
	*profile = active_platform_profile;

	return 0;
}

static bool has_omen_thermal_profile_ec_timer(void)
{
	const char *board_name = dmi_get_system_info(DMI_BOARD_NAME);

	if (!board_name)
		return false;

	return match_string(omen_timed_thermal_profile_boards,
			    ARRAY_SIZE(omen_timed_thermal_profile_boards),
			    board_name) >= 0;
}

inline int omen_thermal_profile_ec_flags_set(enum hp_thermal_profile_omen_flags flags)
{
	return ec_write(HP_OMEN_EC_THERMAL_PROFILE_FLAGS_OFFSET, flags);
}

inline int omen_thermal_profile_ec_timer_set(u8 value)
{
	return ec_write(HP_OMEN_EC_THERMAL_PROFILE_TIMER_OFFSET, value);
}

static int omen_set_cpu_power(const struct omen_power_profile *p);
static int omen_set_gpu_power(const struct omen_power_profile *p);

static int platform_profile_omen_set_ec(enum platform_profile_option profile)
{
    int err, tp, tp_version;
    enum hp_thermal_profile_omen_flags flags = 0;
    const struct omen_power_profile *opp = NULL;

    tp_version = omen_get_thermal_policy_version();

    if (tp_version < 0 || tp_version > 1)
        return -EOPNOTSUPP;

    switch (profile) {
    case PLATFORM_PROFILE_PERFORMANCE:
        opp = &omen_profiles[2];
        tp = (tp_version == 0) ? HP_OMEN_V0_THERMAL_PROFILE_PERFORMANCE : HP_OMEN_V1_THERMAL_PROFILE_PERFORMANCE;
        break;
    case PLATFORM_PROFILE_BALANCED:
        opp = &omen_profiles[1];
        tp = (tp_version == 0) ? HP_OMEN_V0_THERMAL_PROFILE_DEFAULT : HP_OMEN_V1_THERMAL_PROFILE_DEFAULT;
        break;
    case PLATFORM_PROFILE_COOL:
        opp = &omen_profiles[0];
        tp = (tp_version == 0) ? HP_OMEN_V0_THERMAL_PROFILE_COOL : HP_OMEN_V1_THERMAL_PROFILE_COOL;
        break;
    default:
        return -EOPNOTSUPP;
    }

    // Set thermal profile as before
    err = omen_thermal_profile_set(tp);
    if (err < 0)
        return err;

    // Set power/fan profile
    if (opp) {
        omen_set_gpu_power(opp);
        omen_set_cpu_power(opp);
        // Optionally: implement fan curve logic here if supported by firmware
		// start_fan_mode_watcher();
	} else {
		pr_err("No power profile found for the selected thermal profile\n");
		return -EINVAL;
    }

    if (has_omen_thermal_profile_ec_timer()) {
        err = omen_thermal_profile_ec_timer_set(0);
        if (err < 0)
            return err;

        if (profile == PLATFORM_PROFILE_PERFORMANCE)
            flags = HP_OMEN_EC_FLAGS_NOTIMER |
                HP_OMEN_EC_FLAGS_TURBO;

        err = omen_thermal_profile_ec_flags_set(flags);
        if (err < 0)
            return err;
    }

    return 0;
}

static int platform_profile_omen_set(struct device *dev,
				     enum platform_profile_option profile)
{
	int err;

	guard(mutex)(&active_platform_profile_lock);

	err = platform_profile_omen_set_ec(profile);
	if (err < 0)
		return err;

	active_platform_profile = profile;

	return 0;
}

static int thermal_profile_get(void)
{
	return hp_wmi_read_int(HPWMI_THERMAL_PROFILE_QUERY);
}

static int thermal_profile_set(int thermal_profile)
{
	return hp_wmi_perform_query(HPWMI_THERMAL_PROFILE_QUERY, HPWMI_WRITE, &thermal_profile,
							   sizeof(thermal_profile), 0);
}

static int hp_wmi_platform_profile_get(struct device *dev,
					enum platform_profile_option *profile)
{
	int tp;

	tp = thermal_profile_get();
	if (tp < 0)
		return tp;

	switch (tp) {
	case HP_THERMAL_PROFILE_PERFORMANCE:
		*profile =  PLATFORM_PROFILE_PERFORMANCE;
		break;
	case HP_THERMAL_PROFILE_DEFAULT:
		*profile =  PLATFORM_PROFILE_BALANCED;
		break;
	case HP_THERMAL_PROFILE_COOL:
		*profile =  PLATFORM_PROFILE_COOL;
		break;
	case HP_THERMAL_PROFILE_QUIET:
		*profile = PLATFORM_PROFILE_QUIET;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int hp_wmi_platform_profile_set(struct device *dev,
					enum platform_profile_option profile)
{
	int err, tp;

	switch (profile) {
	case PLATFORM_PROFILE_PERFORMANCE:
		tp =  HP_THERMAL_PROFILE_PERFORMANCE;
		break;
	case PLATFORM_PROFILE_BALANCED:
		tp =  HP_THERMAL_PROFILE_DEFAULT;
		break;
	case PLATFORM_PROFILE_COOL:
		tp =  HP_THERMAL_PROFILE_COOL;
		break;
	case PLATFORM_PROFILE_QUIET:
		tp = HP_THERMAL_PROFILE_QUIET;
		break;
	default:
		return -EOPNOTSUPP;
	}

	err = thermal_profile_set(tp);
	if (err)
		return err;

	return 0;
}

static bool is_victus_thermal_profile(void)
{
	const char *board_name = dmi_get_system_info(DMI_BOARD_NAME);

	if (!board_name)
		return false;

	return match_string(victus_thermal_profile_boards,
			    ARRAY_SIZE(victus_thermal_profile_boards),
			    board_name) >= 0;
}

static int platform_profile_victus_get_ec(enum platform_profile_option *profile)
{
	int tp;

	tp = omen_thermal_profile_get();
	if (tp < 0)
		return tp;

	switch (tp) {
	case HP_VICTUS_THERMAL_PROFILE_PERFORMANCE:
		*profile = PLATFORM_PROFILE_PERFORMANCE;
		break;
	case HP_VICTUS_THERMAL_PROFILE_DEFAULT:
		*profile = PLATFORM_PROFILE_BALANCED;
		break;
	case HP_VICTUS_THERMAL_PROFILE_QUIET:
		*profile = PLATFORM_PROFILE_QUIET;
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int platform_profile_victus_get(struct device *dev,
				       enum platform_profile_option *profile)
{
	/* Same behaviour as platform_profile_omen_get */
	return platform_profile_omen_get(dev, profile);
}

static int platform_profile_victus_set_ec(enum platform_profile_option profile)
{
	int err, tp;

	switch (profile) {
	case PLATFORM_PROFILE_PERFORMANCE:
		tp = HP_VICTUS_THERMAL_PROFILE_PERFORMANCE;
		break;
	case PLATFORM_PROFILE_BALANCED:
		tp = HP_VICTUS_THERMAL_PROFILE_DEFAULT;
		break;
	case PLATFORM_PROFILE_QUIET:
		tp = HP_VICTUS_THERMAL_PROFILE_QUIET;
		break;
	default:
		return -EOPNOTSUPP;
	}

	err = omen_thermal_profile_set(tp);
	if (err < 0)
		return err;

	return 0;
}

static bool is_victus_s_thermal_profile(void)
{
	const char *board_name;

	board_name = dmi_get_system_info(DMI_BOARD_NAME);
	if (!board_name)
		return false;

	return match_string(victus_s_thermal_profile_boards,
			    ARRAY_SIZE(victus_s_thermal_profile_boards),
			    board_name) >= 0;
}

static int victus_s_gpu_thermal_profile_get(bool *ctgp_enable,
					    bool *ppab_enable,
					    u8 *dstate,
					    u8 *gpu_slowdown_temp)
{
	struct victus_gpu_power_modes gpu_power_modes;
	int ret;

	ret = hp_wmi_perform_query(HPWMI_GET_GPU_THERMAL_MODES_QUERY, HPWMI_GM,
				   &gpu_power_modes, sizeof(gpu_power_modes),
				   sizeof(gpu_power_modes));
	if (ret == 0) {
		*ctgp_enable = gpu_power_modes.ctgp_enable ? true : false;
		*ppab_enable = gpu_power_modes.ppab_enable ? true : false;
		*dstate = gpu_power_modes.dstate;
		*gpu_slowdown_temp = gpu_power_modes.gpu_slowdown_temp;
	}

	return ret;
}

static int victus_s_gpu_thermal_profile_set(bool ctgp_enable,
					    bool ppab_enable,
					    u8 dstate)
{
	struct victus_gpu_power_modes gpu_power_modes;
	int ret;

	bool current_ctgp_state, current_ppab_state;
	u8 current_dstate, current_gpu_slowdown_temp;

	/* Retrieving GPU slowdown temperature, in order to keep it unchanged */
	ret = victus_s_gpu_thermal_profile_get(&current_ctgp_state,
					       &current_ppab_state,
					       &current_dstate,
					       &current_gpu_slowdown_temp);
	if (ret < 0) {
		pr_warn("GPU modes not updated, unable to get slowdown temp\n");
		return ret;
	}

	gpu_power_modes.ctgp_enable = ctgp_enable ? 0x01 : 0x00;
	gpu_power_modes.ppab_enable = ppab_enable ? 0x01 : 0x00;
	gpu_power_modes.dstate = dstate;
	gpu_power_modes.gpu_slowdown_temp = current_gpu_slowdown_temp;


	ret = hp_wmi_perform_query(HPWMI_SET_GPU_THERMAL_MODES_QUERY, HPWMI_GM,
				   &gpu_power_modes, sizeof(gpu_power_modes), 0);

	return ret;
}

/* Note: HP_POWER_LIMIT_DEFAULT can be used to restore default PL1 and PL2 */
static int victus_s_set_cpu_pl1_pl2(u8 pl1, u8 pl2)
{
	struct victus_power_limits power_limits;
	int ret;

	/* We need to know both PL1 and PL2 values in order to check them */
	if (pl1 == HP_POWER_LIMIT_NO_CHANGE || pl2 == HP_POWER_LIMIT_NO_CHANGE)
		return -EINVAL;

	/* PL2 is not supposed to be lower than PL1 */
	if (pl2 < pl1)
		return -EINVAL;

	power_limits.pl1 = pl1;
	power_limits.pl2 = pl2;
	power_limits.pl4 = HP_POWER_LIMIT_NO_CHANGE;
	power_limits.cpu_gpu_concurrent_limit = HP_POWER_LIMIT_NO_CHANGE;

	ret = hp_wmi_perform_query(HPWMI_SET_POWER_LIMITS_QUERY, HPWMI_GM,
				   &power_limits, sizeof(power_limits), 0);

	return ret;
}

static int platform_profile_victus_s_set_ec(enum platform_profile_option profile)
{
	bool gpu_ctgp_enable, gpu_ppab_enable;
	u8 gpu_dstate; /* Test shows 1 = 100%, 2 = 50%, 3 = 25%, 4 = 12.5% */
	int err, tp;

	switch (profile) {
	case PLATFORM_PROFILE_PERFORMANCE:
		tp = HP_VICTUS_S_THERMAL_PROFILE_PERFORMANCE;
		gpu_ctgp_enable = true;
		gpu_ppab_enable = true;
		gpu_dstate = 1;
		break;
	case PLATFORM_PROFILE_BALANCED:
		tp = HP_VICTUS_S_THERMAL_PROFILE_DEFAULT;
		gpu_ctgp_enable = false;
		gpu_ppab_enable = true;
		gpu_dstate = 1;
		break;
	case PLATFORM_PROFILE_LOW_POWER:
		tp = HP_VICTUS_S_THERMAL_PROFILE_DEFAULT;
		gpu_ctgp_enable = false;
		gpu_ppab_enable = false;
		gpu_dstate = 1;
		break;
	default:
		return -EOPNOTSUPP;
	}

	hp_wmi_get_fan_count_userdefine_trigger();

	err = omen_thermal_profile_set(tp);
	if (err < 0) {
		pr_err("Failed to set platform profile %d: %d\n", profile, err);
		return err;
	}

	err = victus_s_gpu_thermal_profile_set(gpu_ctgp_enable,
					       gpu_ppab_enable,
					       gpu_dstate);
	if (err < 0) {
		pr_err("Failed to set GPU profile %d: %d\n", profile, err);
		return err;
	}

	return 0;
}

static int platform_profile_victus_s_set(struct device *dev,
					 enum platform_profile_option profile)
{
	int err;

	guard(mutex)(&active_platform_profile_lock);

	err = platform_profile_victus_s_set_ec(profile);
	if (err < 0)
		return err;

	active_platform_profile = profile;

	return 0;
}

static int platform_profile_victus_set(struct device *dev,
				       enum platform_profile_option profile)
{
	int err;

	guard(mutex)(&active_platform_profile_lock);

	err = platform_profile_victus_set_ec(profile);
	if (err < 0)
		return err;

	active_platform_profile = profile;

	return 0;
}

static int hp_wmi_platform_profile_probe(void *drvdata, unsigned long *choices)
{
	if (is_omen_thermal_profile()) {
		set_bit(PLATFORM_PROFILE_COOL, choices);
	} else if (is_victus_thermal_profile()) {
		set_bit(PLATFORM_PROFILE_QUIET, choices);
	} else if (is_victus_s_thermal_profile()) {
		/* Adding an equivalent to HP Omen software ECO mode: */
		set_bit(PLATFORM_PROFILE_LOW_POWER, choices);
	} else {
		set_bit(PLATFORM_PROFILE_QUIET, choices);
		set_bit(PLATFORM_PROFILE_COOL, choices);
	}

	set_bit(PLATFORM_PROFILE_BALANCED, choices);
	set_bit(PLATFORM_PROFILE_PERFORMANCE, choices);

	return 0;
}

static int omen_powersource_event(struct notifier_block *nb,
				  unsigned long value,
				  void *data)
{
	struct acpi_bus_event *event_entry = data;
	enum platform_profile_option actual_profile;
	int err;

	if (strcmp(event_entry->device_class, ACPI_AC_CLASS) != 0)
		return NOTIFY_DONE;

	pr_debug("Received power source device event\n");

	guard(mutex)(&active_platform_profile_lock);

	/*
	 * This handler can only be called on Omen and Victus models, so
	 * there's no need to call is_victus_thermal_profile() here.
	 */
	if (is_omen_thermal_profile())
		err = platform_profile_omen_get_ec(&actual_profile);
	else
		err = platform_profile_victus_get_ec(&actual_profile);

	if (err < 0) {
		/*
		 * Although we failed to get the current platform profile, we
		 * still want the other event consumers to process it.
		 */
		pr_warn("Failed to read current platform profile (%d)\n", err);
		return NOTIFY_DONE;
	}

	/*
	 * If we're back on AC and that the user-chosen power profile is
	 * different from what the EC reports, we restore the user-chosen
	 * one.
	 */
	if (power_supply_is_system_supplied() <= 0 ||
	    active_platform_profile == actual_profile) {
		pr_debug("Platform profile update skipped, conditions unmet\n");
		return NOTIFY_DONE;
	}

	if (is_omen_thermal_profile())
		err = platform_profile_omen_set_ec(active_platform_profile);
	else
		err = platform_profile_victus_set_ec(active_platform_profile);

	if (err < 0) {
		pr_warn("Failed to restore platform profile (%d)\n", err);
		return NOTIFY_DONE;
	}

	return NOTIFY_OK;
}

static int victus_s_powersource_event(struct notifier_block *nb,
				      unsigned long value,
				      void *data)
{
	struct acpi_bus_event *event_entry = data;
	int err;

	if (strcmp(event_entry->device_class, ACPI_AC_CLASS) != 0)
		return NOTIFY_DONE;

	pr_debug("Received power source device event\n");

	/*
	 * Switching to battery power source while Performance mode is active
	 * needs manual triggering of CPU power limits. Same goes when switching
	 * to AC power source while Performance mode is active. Other modes
	 * however are automatically behaving without any manual action.
	 * Seen on HP 16-s1034nf (board 8C9C) with F.11 and F.13 BIOS versions.
	 */

	if (active_platform_profile == PLATFORM_PROFILE_PERFORMANCE) {
		pr_debug("Triggering CPU PL1/PL2 actualization\n");
		err = victus_s_set_cpu_pl1_pl2(HP_POWER_LIMIT_DEFAULT,
					       HP_POWER_LIMIT_DEFAULT);
		if (err)
			pr_warn("Failed to actualize power limits: %d\n", err);

		return NOTIFY_DONE;
	}

	return NOTIFY_OK;
}

static int omen_register_powersource_event_handler(void)
{
	int err;

	platform_power_source_nb.notifier_call = omen_powersource_event;
	err = register_acpi_notifier(&platform_power_source_nb);

	if (err < 0) {
		pr_warn("Failed to install ACPI power source notify handler\n");
		return err;
	}

	return 0;
}

static int victus_s_register_powersource_event_handler(void)
{
	int err;

	platform_power_source_nb.notifier_call = victus_s_powersource_event;
	err = register_acpi_notifier(&platform_power_source_nb);
	if (err < 0) {
		pr_warn("Failed to install ACPI power source notify handler\n");
		return err;
	}

	return 0;
}

static inline void omen_unregister_powersource_event_handler(void)
{
	unregister_acpi_notifier(&platform_power_source_nb);
}

static inline void victus_s_unregister_powersource_event_handler(void)
{
	unregister_acpi_notifier(&platform_power_source_nb);
}

static const struct platform_profile_ops platform_profile_omen_ops = {
	.probe = hp_wmi_platform_profile_probe,
	.profile_get = platform_profile_omen_get,
	.profile_set = platform_profile_omen_set,
};

static const struct platform_profile_ops platform_profile_victus_ops = {
	.probe = hp_wmi_platform_profile_probe,
	.profile_get = platform_profile_victus_get,
	.profile_set = platform_profile_victus_set,
};

static const struct platform_profile_ops platform_profile_victus_s_ops = {
	.probe = hp_wmi_platform_profile_probe,
	.profile_get = platform_profile_omen_get,
	.profile_set = platform_profile_victus_s_set,
};

static const struct platform_profile_ops hp_wmi_platform_profile_ops = {
	.probe = hp_wmi_platform_profile_probe,
	.profile_get = hp_wmi_platform_profile_get,
	.profile_set = hp_wmi_platform_profile_set,
};

static int thermal_profile_setup(struct platform_device *device)
{
	const struct platform_profile_ops *ops;
	int err, tp;

	if (is_omen_thermal_profile()) {
		err = platform_profile_omen_get_ec(&active_platform_profile);
		if (err < 0)
			return err;

		/*
		 * call thermal profile write command to ensure that the
		 * firmware correctly sets the OEM variables
		 */
		err = platform_profile_omen_set_ec(active_platform_profile);
		if (err < 0)
			return err;

		ops = &platform_profile_omen_ops;
	} else if (is_victus_thermal_profile()) {
		err = platform_profile_victus_get_ec(&active_platform_profile);
		if (err < 0)
			return err;

		/*
		 * call thermal profile write command to ensure that the
		 * firmware correctly sets the OEM variables
		 */
		err = platform_profile_victus_set_ec(active_platform_profile);
		if (err < 0)
			return err;

		ops = &platform_profile_victus_ops;
	} else if (is_victus_s_thermal_profile()) {
		/*
		 * Being unable to retrieve laptop's current thermal profile,
		 * during this setup, we set it to Balanced by default.
		 */
		active_platform_profile = PLATFORM_PROFILE_BALANCED;

		err = platform_profile_victus_s_set_ec(active_platform_profile);
		if (err < 0)
			return err;

		ops = &platform_profile_victus_s_ops;
	} else {
		tp = thermal_profile_get();

		if (tp < 0)
			return tp;

		/*
		 * call thermal profile write command to ensure that the
		 * firmware correctly sets the OEM variables for the DPTF
		 */
		err = thermal_profile_set(tp);
		if (err)
			return err;

		ops = &hp_wmi_platform_profile_ops;
	}

	platform_profile_device = devm_platform_profile_register(&device->dev, "hp-wmi",
								 NULL, ops);
	if (IS_ERR(platform_profile_device))
		return PTR_ERR(platform_profile_device);

	pr_info("Registered as platform profile handler\n");
	platform_profile_support = true;

	return 0;
}

static int hp_wmi_hwmon_init(void);

static int __init hp_wmi_bios_setup(struct platform_device *device)
{
	int err;
	/* clear detected rfkill devices */
	wifi_rfkill = NULL;
	bluetooth_rfkill = NULL;
	wwan_rfkill = NULL;
	rfkill2_count = 0;

	/*
	 * In pre-2009 BIOS, command 1Bh return 0x4 to indicate that
	 * BIOS no longer controls the power for the wireless
	 * devices. All features supported by this command will no
	 * longer be supported.
	 */
	if (!hp_wmi_bios_2009_later()) {
		if (hp_wmi_rfkill_setup(device))
			hp_wmi_rfkill2_setup(device);
	}

	err = hp_wmi_hwmon_init();

	if (err < 0)
		return err;

	thermal_profile_setup(device);

	 err = hp_omen_keyboard_rgb_setup(device);
    if (err) {
        pr_warn("Keyboard RGB setup failed: %d\n", err);
        // Don't fail driver init, just warn
    }

	return 0;
}

static void __exit hp_wmi_bios_remove(struct platform_device *device)
{
	int i;
	 hp_omen_keyboard_rgb_remove(device);
	for (i = 0; i < rfkill2_count; i++) {
		rfkill_unregister(rfkill2[i].rfkill);
		rfkill_destroy(rfkill2[i].rfkill);
	}

	if (wifi_rfkill) {
		rfkill_unregister(wifi_rfkill);
		rfkill_destroy(wifi_rfkill);
	}
	if (bluetooth_rfkill) {
		rfkill_unregister(bluetooth_rfkill);
		rfkill_destroy(bluetooth_rfkill);
	}
	if (wwan_rfkill) {
		rfkill_unregister(wwan_rfkill);
		rfkill_destroy(wwan_rfkill);
	}
}

static int hp_wmi_resume_handler(struct device *device)
{
	/*
	 * Hardware state may have changed while suspended, so trigger
	 * input events for the current state. As this is a switch,
	 * the input layer will only actually pass it on if the state
	 * changed.
	 */
	if (hp_wmi_input_dev) {
		if (test_bit(SW_DOCK, hp_wmi_input_dev->swbit))
			input_report_switch(hp_wmi_input_dev, SW_DOCK,
					    hp_wmi_get_dock_state());
		if (test_bit(SW_TABLET_MODE, hp_wmi_input_dev->swbit))
			input_report_switch(hp_wmi_input_dev, SW_TABLET_MODE,
					    hp_wmi_get_tablet_mode());
		input_sync(hp_wmi_input_dev);
	}

	if (rfkill2_count)
		hp_wmi_rfkill2_refresh();

	if (wifi_rfkill)
		rfkill_set_states(wifi_rfkill,
				  hp_wmi_get_sw_state(HPWMI_WIFI),
				  hp_wmi_get_hw_state(HPWMI_WIFI));
	if (bluetooth_rfkill)
		rfkill_set_states(bluetooth_rfkill,
				  hp_wmi_get_sw_state(HPWMI_BLUETOOTH),
				  hp_wmi_get_hw_state(HPWMI_BLUETOOTH));
	if (wwan_rfkill)
		rfkill_set_states(wwan_rfkill,
				  hp_wmi_get_sw_state(HPWMI_WWAN),
				  hp_wmi_get_hw_state(HPWMI_WWAN));

	return 0;
}

static const struct dev_pm_ops hp_wmi_pm_ops = {
	.resume  = hp_wmi_resume_handler,
	.restore  = hp_wmi_resume_handler,
};

/*
 * hp_wmi_bios_remove() lives in .exit.text. For drivers registered via
 * module_platform_driver_probe() this is ok because they cannot get unbound at
 * runtime. So mark the driver struct with __refdata to prevent modpost
 * triggering a section mismatch warning.
 */
// Use official 2-step reset sequence on enabling auto mode
static int hp_wmi_enable_auto_fan_mode(void)
{
    int ret;

    // Disable max mode first
    ret = hp_wmi_fan_speed_max_reset(); 
    if (ret)
        return ret;
}


// Convert user percentage 0-100 to PWM in safe model range
static int hp_wmi_percentage_to_pwm(int percentage)
{
    int pwm_min = 0;  // model-specific min PWM
    int pwm_max = 186;  // model-specific max PWM

    if (percentage == 0)
        return 0;  // auto mode disables manual PWM

    if (percentage < 0)
        percentage = 0;
    if (percentage > 100)
        percentage = 100;

    return pwm_min + (percentage * (pwm_max - pwm_min)) / 100;
}

struct fan_curve_point {
    u8 temp_c;
    u8 pwm;
};

static const struct fan_curve_point omen_perf_curve[] = {
    { 40, 120 },
    { 50, 150 },
    { 60, 180 },
    { 70, 220 },
    { 80, 255 },
};

static int apply_fan_curve(int cpu_temp)
{
    int i;
    int pwm = 0;

    for (i = 0; i < ARRAY_SIZE(omen_perf_curve) - 1; i++) {
        if (cpu_temp >= omen_perf_curve[i].temp_c && cpu_temp < omen_perf_curve[i+1].temp_c) {
            // linear interpolation for PWM
            int range_temp = omen_perf_curve[i+1].temp_c - omen_perf_curve[i].temp_c;
            int range_pwm = omen_perf_curve[i+1].pwm - omen_perf_curve[i].pwm;
            int offset_temp = cpu_temp - omen_perf_curve[i].temp_c;
            pwm = omen_perf_curve[i].pwm + (range_pwm * offset_temp) / range_temp;
            break;
        }
    }
    if (cpu_temp <= omen_perf_curve[0].temp_c)
        pwm = omen_perf_curve[0].pwm;
    if (cpu_temp >= omen_perf_curve[ARRAY_SIZE(omen_perf_curve) - 1].temp_c)
        pwm = omen_perf_curve[ARRAY_SIZE(omen_perf_curve) - 1].pwm;

    return pwm;
}

static struct platform_driver hp_wmi_driver __refdata = {
	.driver = {
		.name = "hp-wmi",
		.pm = &hp_wmi_pm_ops,
		.dev_groups = hp_wmi_groups,
	},
	.remove = __exit_p(hp_wmi_bios_remove),
};

static umode_t hp_wmi_hwmon_is_visible(const void *data,
                                      enum hwmon_sensor_types type,
                                      u32 attr, int channel)
{
    switch (type) {
    case hwmon_pwm:
        // Only expose channel 0 (unified control)
        if (channel > 0) return 0;
        
        if (attr == hwmon_pwm_enable)
            return 0644;  // Fan mode control: 0=max, 1=manual, 2=auto
        if (attr == hwmon_pwm_input)
            return 0644;  // Unified PWM control: 0-255
        break;
        
    case hwmon_fan:
        // Expose both individual fan readings + unified average
        if (channel > 2) return 0;  // fan1=CPU, fan2=GPU, fan3=average
        
        if (channel < 2) {
            // Individual fan readings
            if (is_victus_s_thermal_profile()) {
                if (hp_wmi_get_fan_speed_victus_s(channel) >= 0)
                    return 0444;
            } else {
                if (hp_wmi_get_fan_speed(channel) >= 0)
                    return 0444;
            }
        } else {
            // Channel 2 = unified average
            return 0444;
        }
        break;
        
    default:
        return 0;
    }

    return 0;
}

static int hp_wmi_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
                            u32 attr, int channel, long *val)
{
    int ret;

    switch (type) {
    case hwmon_fan:
        if (channel < 2) {
            // Individual fan readings (fan1=CPU, fan2=GPU)
            if (is_victus_s_thermal_profile())
                ret = hp_wmi_get_fan_speed_victus_s(channel);
            else
                ret = hp_wmi_get_fan_speed(channel);
        } else if (channel == 2) {
            // Unified average reading (fan3)
            ret = hp_wmi_fan_get_average_speed();
        } else {
            return -EINVAL;
        }
        
        if (ret < 0)
            return ret;
        *val = ret;
        return 0;
        
    case hwmon_pwm:
        if (channel > 0) return -EINVAL;  // Only unified control
        
        if (attr == hwmon_pwm_enable) {
            // Return current fan mode
            if (unified_manual_mode) {
                *val = 1;  // Manual mode
            } else {
                // Check if in max or auto mode
                switch (hp_wmi_fan_speed_max_get()) {
                case 0:
                    *val = 2;  // Automatic
                    return 0;
                case 1:
                    *val = 0;  // Max speed
                    return 0;
                default:
                    return -ENODATA;
                }
            }
            return 0;
            
        } else if (attr == hwmon_pwm_input) {
            // Return unified PWM value (0-255)
            if (unified_manual_mode && unified_fan_speed >= 0) {
                // In manual mode, return set percentage converted to 0-255
                *val = (unified_fan_speed * 255) / 100;
            } else {
                // Not in manual mode, calculate from current average speed
                int current_rpm = hp_wmi_fan_get_average_speed();
                int max_rpm;
                
                if (current_rpm < 0)
                    return current_rpm;
                
                max_rpm = hp_wmi_fan_get_max_unified();
                if (max_rpm <= 0) {
                    *val = 255;  // Default to full if can't determine max
                } else {
                    *val = (current_rpm * 255) / max_rpm;
                    if (*val > 255) *val = 255;
                }
            }
            return 0;
        }
        break;
        
    default:
        return -EINVAL;
    }
    
    return -EINVAL;
}

static int hp_wmi_hwmon_write(struct device *dev,
                              enum hwmon_sensor_types type,
                              u32 attr, int channel, long val)
{
    int pwm;

    if (type == hwmon_pwm && channel == 0 && attr == hwmon_pwm_input) {
        // Auto mode
        if (val == 0) {
            unified_manual_mode = false;
            unified_fan_speed = -1;
            hp_wmi_fan_speed_reset();
            return 0;
        }

        // Manual mode, enforce min/max PWM safe ranges
        pwm = hp_wmi_percentage_to_pwm(val);
        unified_manual_mode = true;
        unified_fan_speed = val;
        last_manual_speed = val;

        return hp_wmi_fan_speed_set_unified(val);
    }
    return -EOPNOTSUPP;
}


// Update hwmon channel info for unified control
static const struct hwmon_channel_info * const info[] = {
    // fan1=CPU, fan2=GPU, fan3=unified_average
    HWMON_CHANNEL_INFO(fan, 
                      HWMON_F_INPUT,      // CPU fan RPM
                      HWMON_F_INPUT,      // GPU fan RPM  
                      HWMON_F_INPUT),     // Unified average RPM
    // Single unified PWM control                  
    HWMON_CHANNEL_INFO(pwm,
                      HWMON_PWM_ENABLE | HWMON_PWM_INPUT),  // Unified control
    NULL
};

static const struct hwmon_ops ops = {
	.is_visible = hp_wmi_hwmon_is_visible,
	.read = hp_wmi_hwmon_read,
	.write = hp_wmi_hwmon_write,
};

static const struct hwmon_chip_info chip_info = {
	.ops = &ops,
	.info = info,
};

static int hp_wmi_hwmon_init(void)
{
	struct device *dev = &hp_wmi_platform_dev->dev;
	struct device *hwmon;

	hwmon = devm_hwmon_device_register_with_info(dev, "hp", &hp_wmi_driver,
						     &chip_info, NULL);

	if (IS_ERR(hwmon)) {
		dev_err(dev, "Could not register hp hwmon device\n");
		return PTR_ERR(hwmon);
	}

	return 0;
}

static int __init hp_wmi_init(void)
{
	int event_capable = wmi_has_guid(HPWMI_EVENT_GUID);
	int bios_capable = wmi_has_guid(HPWMI_BIOS_GUID);
	int err, tmp = 0;
	detected_max_rpm = -1;
    hp_wmi_detect_max_fan_rpm();

	if (!bios_capable && !event_capable)
		return -ENODEV;

	if (hp_wmi_perform_query(HPWMI_HARDWARE_QUERY, HPWMI_READ, &tmp,
				 sizeof(tmp), sizeof(tmp)) == HPWMI_RET_INVALID_PARAMETERS)
		zero_insize_support = true;

	if (event_capable) {
		err = hp_wmi_input_setup();
		if (err)
			return err;
	}

	if (bios_capable) {
		hp_wmi_platform_dev =
			platform_device_register_simple("hp-wmi", PLATFORM_DEVID_NONE, NULL, 0);
		if (IS_ERR(hp_wmi_platform_dev)) {
			err = PTR_ERR(hp_wmi_platform_dev);
			goto err_destroy_input;
		}

		err = platform_driver_probe(&hp_wmi_driver, hp_wmi_bios_setup);
		if (err)
			goto err_unregister_device;
	}

	if (is_omen_thermal_profile() || is_victus_thermal_profile()) {
		err = omen_register_powersource_event_handler();
		if (err)
			goto err_unregister_device;
	} else if (is_victus_s_thermal_profile()) {
		err = victus_s_register_powersource_event_handler();
		if (err)
			goto err_unregister_device;
	}

	return 0;

err_unregister_device:
	platform_device_unregister(hp_wmi_platform_dev);
err_destroy_input:
	if (event_capable)
		hp_wmi_input_destroy();

	return err;
}
module_init(hp_wmi_init);

static void __exit hp_wmi_exit(void)
{
	if (is_omen_thermal_profile() || is_victus_thermal_profile())
		omen_unregister_powersource_event_handler();
		 

	if (is_victus_s_thermal_profile())
		victus_s_unregister_powersource_event_handler();

	if (wmi_has_guid(HPWMI_EVENT_GUID))
		hp_wmi_input_destroy();

	if (camera_shutter_input_dev)
		input_unregister_device(camera_shutter_input_dev);

	if (hp_wmi_platform_dev) {
		platform_device_unregister(hp_wmi_platform_dev);
		platform_driver_unregister(&hp_wmi_driver);
	}
}
module_exit(hp_wmi_exit);

static int omen_set_cpu_power(const struct omen_power_profile *p)
{
    struct victus_power_limits pl = {
        .pl1 = p->cpu_pl1,
        .pl2 = p->cpu_pl2,
        .pl4 = p->cpu_pl4,
        .cpu_gpu_concurrent_limit = p->cpu_combined,
    };
    return hp_wmi_perform_query(HPWMI_SET_POWER_LIMITS_QUERY, HPWMI_GM,
                   &pl, sizeof(pl), 0);
}

static int omen_set_gpu_power(const struct omen_power_profile *p)
{
    struct victus_gpu_power_modes gp = {
        .ctgp_enable = p->gpu_ctgp,
        .ppab_enable = p->gpu_ppab,
        .dstate = p->gpu_dstate,
        .gpu_slowdown_temp = p->gpu_peak_temp,
    };
    return hp_wmi_perform_query(HPWMI_SET_GPU_THERMAL_MODES_QUERY, HPWMI_GM,
                   &gp, sizeof(gp), 0);
}

static struct delayed_work fan_mode_watcher_work;
static int user_manual_fan = 0; // 0 = automatic, 1 = manual/max