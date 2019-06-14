/*
 * Copyright 2013, Fredrik Hultin.
 * Copyright 2013, Jakob Bornecrantz.
 * Copyright 2016 Philipp Zabel
 * Copyright 2019 Jan Schmidt
 * SPDX-License-Identifier: BSL-1.0
 *
 * OpenHMD - Free and Open Source API and drivers for immersive technology.
 */

/* Oculus Rift Driver - HID/USB Driver Implementation */


#include <stdlib.h>
#include <hidapi.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <assert.h>

#include "rift.h"
#include "../hid.h"

#define TICK_LEN (1.0f / 1000.0f) // 1000 Hz ticks
#define KEEP_ALIVE_VALUE (10 * 1000)
#define SETFLAG(_s, _flag, _val) (_s) = ((_s) & ~(_flag)) | ((_val) ? (_flag) : 0)

typedef struct {
	ohmd_device base;

	hid_device* handle;
	pkt_sensor_range sensor_range;
	pkt_sensor_display_info display_info;
	rift_coordinate_frame coordinate_frame, hw_coordinate_frame;
	pkt_sensor_config sensor_config;
	pkt_tracker_sensor sensor;
	uint32_t last_imu_timestamp;
	double last_keep_alive;
	fusion sensor_fusion;
	vec3f raw_mag, raw_accel, raw_gyro;

	struct {
		vec3f pos;
	} imu;

	uint8_t radio_address[5];
	rift_led *leds;
	uint8_t num_leds;
 } rift_priv;

typedef enum {
	REV_DK1,
	REV_DK2,
	REV_CV1,

	REV_GEARVR_GEN1
} rift_revision;

typedef struct {
	const char* name;
	int company;
	int id;
	int iface;
	rift_revision rev;
} rift_devices;

static rift_priv* rift_priv_get(ohmd_device* device)
{
	return (rift_priv*)device;
}

static int get_feature_report(rift_priv* priv, rift_sensor_feature_cmd cmd, unsigned char* buf)
{
	memset(buf, 0, FEATURE_BUFFER_SIZE);
	buf[0] = (unsigned char)cmd;
	return hid_get_feature_report(priv->handle, buf, FEATURE_BUFFER_SIZE);
}

static int send_feature_report(rift_priv* priv, const unsigned char *data, size_t length)
{
	return hid_send_feature_report(priv->handle, data, length);
}

static void set_coordinate_frame(rift_priv* priv, rift_coordinate_frame coordframe)
{
	priv->coordinate_frame = coordframe;

	// set the RIFT_SCF_SENSOR_COORDINATES in the sensor config to match whether coordframe is hmd or sensor
	SETFLAG(priv->sensor_config.flags, RIFT_SCF_SENSOR_COORDINATES, coordframe == RIFT_CF_SENSOR);

	// encode send the new config to the Rift
	unsigned char buf[FEATURE_BUFFER_SIZE];
	int size = encode_sensor_config(buf, &priv->sensor_config);
	if(send_feature_report(priv, buf, size) == -1){
		ohmd_set_error(priv->base.ctx, "send_feature_report failed in set_coordinate frame");
		return;
	}

	// read the state again, set the hw_coordinate_frame to match what
	// the hardware actually is set to just in case it doesn't stick.
	size = get_feature_report(priv, RIFT_CMD_SENSOR_CONFIG, buf);
	if(size <= 0){
		LOGW("could not set coordinate frame");
		priv->hw_coordinate_frame = RIFT_CF_HMD;
		return;
	}

	decode_sensor_config(&priv->sensor_config, buf, size);
	priv->hw_coordinate_frame = (priv->sensor_config.flags & RIFT_SCF_SENSOR_COORDINATES) ? RIFT_CF_SENSOR : RIFT_CF_HMD;

	if(priv->hw_coordinate_frame != coordframe) {
		LOGW("coordinate frame didn't stick");
	}
}

static void handle_tracker_sensor_msg(rift_priv* priv, unsigned char* buffer, int size)
{
	if (buffer[0] == RIFT_IRQ_SENSORS_DK1
	  && !decode_tracker_sensor_msg_dk1(&priv->sensor, buffer, size)){
		LOGE("couldn't decode tracker sensor message");
	}
	else if (buffer[0] == RIFT_IRQ_SENSORS_DK2 /* DK2 and CV1 variant */
	  && !decode_tracker_sensor_msg_dk2(&priv->sensor, buffer, size)){
		LOGE("couldn't decode tracker sensor message");
	}

	pkt_tracker_sensor* s = &priv->sensor;

	dump_packet_tracker_sensor(s);

	int32_t mag32[] = { s->mag[0], s->mag[1], s->mag[2] };
	vec3f_from_rift_vec(mag32, &priv->raw_mag);

	// TODO: handle overflows in a nicer way
	float dt = TICK_LEN; // TODO: query the Rift for the sample rate
	if (s->timestamp > priv->last_imu_timestamp)
	{
		dt = (s->timestamp - priv->last_imu_timestamp) / 1000000.0f;
		dt -= (s->num_samples - 1) * TICK_LEN; // TODO: query the Rift for the sample rate
	}

	for(int i = 0; i < s->num_samples; i++){
		vec3f_from_rift_vec(s->samples[i].accel, &priv->raw_accel);
		vec3f_from_rift_vec(s->samples[i].gyro, &priv->raw_gyro);

		ofusion_update(&priv->sensor_fusion, dt, &priv->raw_gyro, &priv->raw_accel, &priv->raw_mag);
		dt = TICK_LEN; // TODO: query the Rift for the sample rate
	}

	priv->last_imu_timestamp = s->timestamp;
}

static void update_device(ohmd_device* device)
{
	rift_priv* priv = rift_priv_get(device);
	unsigned char buffer[FEATURE_BUFFER_SIZE];

	// Handle keep alive messages
	double t = ohmd_get_tick();
	if(t - priv->last_keep_alive >= (double)priv->sensor_config.keep_alive_interval / 1000.0 - .2){
		// send keep alive message
		pkt_keep_alive keep_alive = { 0, priv->sensor_config.keep_alive_interval };
		int ka_size = encode_dk1_keep_alive(buffer, &keep_alive);
		if (send_feature_report(priv, buffer, ka_size) == -1)
			LOGE("error sending keepalive");

		// Update the time of the last keep alive we have sent.
		priv->last_keep_alive = t;
	}

	// Read all the messages from the device.
	while(true){
		int size = hid_read(priv->handle, buffer, FEATURE_BUFFER_SIZE);
		if(size < 0){
			LOGE("error reading from device");
			return;
		} else if(size == 0) {
			return; // No more messages, return.
		}

		// currently the only message type the hardware supports (I think)
		if(buffer[0] == RIFT_IRQ_SENSORS_DK1 || buffer[0] == RIFT_IRQ_SENSORS_DK2) {
			handle_tracker_sensor_msg(priv, buffer, size);
		}else{
			LOGE("unknown message type: %u", buffer[0]);
		}
	}
}

static bool rift_radio_send_cmd(rift_priv *priv, uint8_t a, uint8_t b, uint8_t c)
{
	unsigned char buffer[FEATURE_BUFFER_SIZE];
	int cmd_size = encode_radio_control_cmd(buffer, a, b, c);
	int ret_size;

	if (send_feature_report(priv, buffer, cmd_size) == -1) {
		LOGE("error sending HMD radio command 0x%02x/%02x/%02x", a, b, c);
		return false;
	}

	do {
		ret_size = get_feature_report(priv, RIFT_CMD_RADIO_CONTROL, buffer);
		if (ret_size < 1) {
			LOGE("HMD radio command 0x%02x/%02x/%02x failed - response too small", a, b, c);
			return false;
		}
	} while (buffer[3] & 0x80);

	if (buffer[3] & 0x08) {
		LOGE("HMD radio command 0x%02x/%02x/%02x failed", a, b, c);
		return false;
	}

	return true;
}

static bool rift_radio_get_address(rift_priv* priv, uint8_t address[5])
{
	unsigned char buf[FEATURE_BUFFER_SIZE];
	int ret_size;

	if (!rift_radio_send_cmd (priv, 0x05, 0x03, 0x05))
		return false;

	ret_size = get_feature_report(priv, RIFT_CMD_RADIO_DATA, buf);
	if (ret_size < 0)
		return false;

	if (!decode_radio_address (priv->radio_address, buf, ret_size)) {
		LOGE("Failed to decode received radio address");
		return false;
	}

	return true;
}

static int getf(ohmd_device* device, ohmd_float_value type, float* out)
{
	rift_priv* priv = rift_priv_get(device);

	switch(type){
	case OHMD_DISTORTION_K: {
			for (int i = 0; i < 6; i++) {
				out[i] = priv->display_info.distortion_k[i];
			}
			break;
		}

	case OHMD_ROTATION_QUAT: {
			*(quatf*)out = priv->sensor_fusion.orient;
			break;
		}

	case OHMD_POSITION_VECTOR:
		out[0] = out[1] = out[2] = 0;
		break;

	default:
		ohmd_set_error(priv->base.ctx, "invalid type given to getf (%ud)", type);
		return -1;
		break;
	}

	return 0;
}

static void close_device(ohmd_device* device)
{
	LOGD("closing device");
	rift_priv* priv = rift_priv_get(device);

	if (priv->leds)
		free (priv->leds);

	hid_close(priv->handle);
	free(priv);
}

#define UDEV_WIKI_URL "https://github.com/OpenHMD/OpenHMD/wiki/Udev-rules-list"

/*
 * Obtains the positions and blinking patterns of the IR LEDs from the Rift.
 */
static int rift_get_led_info(rift_priv *priv)
{
	int first_index = -1;
	unsigned char buf[FEATURE_BUFFER_SIZE];
	int size;
	int num_leds = 0;

	//Get LED positions
	while (true) {
		pkt_position_info pos;

		size = get_feature_report(priv, RIFT_CMD_POSITION_INFO, buf);
		if (size <= 0 || !decode_position_info(&pos, buf, size) ||
		    first_index == pos.index) {
			break;
		}

		if (first_index < 0) {
			first_index = pos.index;
			priv->leds = calloc(pos.num, sizeof(rift_led));
		}

		if (pos.flags == 1) { //reports 0's
			priv->imu.pos.x = (float)pos.pos_x;
			priv->imu.pos.y = (float)pos.pos_y;
			priv->imu.pos.z = (float)pos.pos_z;
			LOGV ("IMU index %d pos x/y/x %d/%d/%d\n", pos.index, pos.pos_x, pos.pos_y, pos.pos_z);
		} else if (pos.flags == 2) {
			rift_led *led = &priv->leds[pos.index];
			led->pos.x = (float)pos.pos_x;
			led->pos.y = (float)pos.pos_y;
			led->pos.z = (float)pos.pos_z;
			led->dir.x = (float)pos.dir_x;
			led->dir.y = (float)pos.dir_y;
			led->dir.z = (float)pos.dir_z;
			ovec3f_normalize_me(&led->dir);
			if (pos.index >= num_leds)
				num_leds = pos.index + 1;
			LOGV ("LED index %d pos x/y/x %d/%d/%d\n", pos.index, pos.pos_x, pos.pos_y, pos.pos_z);
		}
	}
	priv->num_leds = num_leds;

	// Get LED patterns
	first_index = -1;
	while (true) {
		pkt_led_pattern_report pkt;
		int8_t pattern_length;
		int32_t pattern;

		size = get_feature_report(priv, RIFT_CMD_PATTERN_INFO, buf);
		if (size <= 0 || !decode_led_pattern_info(&pkt, buf, size) ||
		    first_index == pkt.index) {
			break;
		}

		if (first_index < 0) {
			first_index = pkt.index;
			if (priv->num_leds != pkt.num) {
				LOGE("LED positions count doesn't match pattern count - got %d patterns for %d LEDs", pkt.num, priv->num_leds);
				return -1;
			}
		}
		if (pkt.index >= priv->num_leds) {
			LOGE("Invalid LED pattern index %d (%d LEDs)", pkt.index, priv->num_leds);
			return -1;
		}

		pattern_length = pkt.pattern_length;
		pattern = pkt.pattern;

		/* pattern_length should be 10 */
		if (pattern_length != 10) {
			LOGE("Rift: Unexpected LED pattern length: %d\n",
				pattern_length);
			return -1;
		}

		LOGV ("LED index %d pattern 0x%08x\n", pkt.index, pkt.pattern);
		/*
		 * pattern should consist of 10 2-bit values that are either
		 * 1 (dark) or 3 (bright).
		 */
		if ((pattern & ~0xaaaaa) != 0x55555) {
			LOGE("Rift: Unexpected pattern: 0x%x", pattern);
			return -1;
		}

		/* Convert into 10 single-bit values 1 -> 0, 3 -> 1 */
		pattern &= 0xaaaaa;
		pattern |= pattern >> 1;
		pattern &= 0x66666;
		pattern |= pattern >> 2;
		pattern &= 0xe1e1e;
		pattern |= pattern >> 4;
		pattern &= 0xe01fe;
		pattern |= pattern >> 8;
		pattern = (pattern >> 1) & 0x3ff;

		priv->leds[pkt.index].pattern = pattern;
	}

	return 0;
}

/*
 * Sends a tracking report to enable the IR tracking LEDs.
 */
static int rift_send_tracking_config(rift_priv *rift, bool blink,
    uint16_t exposure_us, uint16_t period_us)
{
	pkt_tracking_config tracking_config = { 0, };
	unsigned char buf[FEATURE_BUFFER_SIZE];
	int size;

	tracking_config.vsync_offset = RIFT_TRACKING_VSYNC_OFFSET;
	tracking_config.duty_cycle = RIFT_TRACKING_DUTY_CYCLE;
	tracking_config.exposure_us = exposure_us;
	tracking_config.period_us = period_us;

	if (blink) {
		tracking_config.pattern = 0;
		tracking_config.flags = RIFT_TRACKING_ENABLE |
		                        RIFT_TRACKING_USE_CARRIER |
		                        RIFT_TRACKING_AUTO_INCREMENT;
	} else {
		tracking_config.pattern = 0xff;
		tracking_config.flags = RIFT_TRACKING_ENABLE |
		                        RIFT_TRACKING_USE_CARRIER;
	}

	size = encode_tracking_config(buf, &tracking_config);
	if (send_feature_report(rift, buf, size) == -1) {
		LOGE("Error sending LED tracking config");
	  return -1;
	}

	return 0;
}

static ohmd_device* open_device(ohmd_driver* driver, ohmd_device_desc* desc)
{
	rift_priv* priv = ohmd_alloc(driver->ctx, sizeof(rift_priv));
	if(!priv)
		goto cleanup;

	priv->last_imu_timestamp = -1;

	priv->base.ctx = driver->ctx;

	// Open the HID device
	priv->handle = hid_open_path(desc->path);

	if(!priv->handle) {
		char* path = _hid_to_unix_path(desc->path);
		ohmd_set_error(driver->ctx, "Could not open %s.\n"
		                            "Check your permissions: "
		                            UDEV_WIKI_URL, path);
		free(path);
		goto cleanup;
	}

	if(hid_set_nonblocking(priv->handle, 1) == -1){
		ohmd_set_error(driver->ctx, "failed to set non-blocking on device");
		goto cleanup;
	}

	unsigned char buf[FEATURE_BUFFER_SIZE];

	int size;

	// Read and decode the sensor range
	size = get_feature_report(priv, RIFT_CMD_RANGE, buf);
	decode_sensor_range(&priv->sensor_range, buf, size);
	dump_packet_sensor_range(&priv->sensor_range);

	// Read and decode display information
	size = get_feature_report(priv, RIFT_CMD_DISPLAY_INFO, buf);
	decode_sensor_display_info(&priv->display_info, buf, size);
	dump_packet_sensor_display_info(&priv->display_info);

	// Read and decode the sensor config
	size = get_feature_report(priv, RIFT_CMD_SENSOR_CONFIG, buf);
	decode_sensor_config(&priv->sensor_config, buf, size);
	dump_packet_sensor_config(&priv->sensor_config);

	// if the sensor has display info data, use HMD coordinate frame
	priv->coordinate_frame = priv->display_info.distortion_type != RIFT_DT_NONE ? RIFT_CF_HMD : RIFT_CF_SENSOR;

	// enable calibration
	SETFLAG(priv->sensor_config.flags, RIFT_SCF_USE_CALIBRATION, 1);
	SETFLAG(priv->sensor_config.flags, RIFT_SCF_AUTO_CALIBRATION, 1);

	// apply sensor config
	set_coordinate_frame(priv, priv->coordinate_frame);

	// Turn the screens on
	if (desc->revision == REV_CV1)
	{
		size = encode_enable_components(buf, true, true, true);
		if (send_feature_report(priv, buf, size) == -1)
			LOGE("error turning the screens on");

		rift_send_tracking_config (priv, false, RIFT_TRACKING_EXPOSURE_US_CV1,
				RIFT_TRACKING_PERIOD_US_CV1);

		/* Read the radio ID for CV1 to enable camera sensor sync */
		rift_radio_get_address(priv, priv->radio_address);
	}
	else if (desc->revision == REV_DK2)
	{
		rift_send_tracking_config (priv, false, RIFT_TRACKING_EXPOSURE_US_DK2,
				RIFT_TRACKING_PERIOD_US_DK2);
	}

	/* We only need the LED info if we have a sensor to observe them with,
	   so we could skip this */
	if (rift_get_led_info (priv) < 0) {
		ohmd_set_error(driver->ctx, "failed to read LED info from device");
		goto cleanup;
	}

	// set keep alive interval to n seconds
	pkt_keep_alive keep_alive = { 0, KEEP_ALIVE_VALUE };
	size = encode_dk1_keep_alive(buf, &keep_alive);
	if (send_feature_report(priv, buf, size) == -1)
		LOGE("error setting up keepalive");

	// Update the time of the last keep alive we have sent.
	priv->last_keep_alive = ohmd_get_tick();

	// update sensor settings with new keep alive value
	// (which will have been ignored in favor of the default 1000 ms one)
	size = get_feature_report(priv, RIFT_CMD_SENSOR_CONFIG, buf);
	decode_sensor_config(&priv->sensor_config, buf, size);
	dump_packet_sensor_config(&priv->sensor_config);

	// Set default device properties
	ohmd_set_default_device_properties(&priv->base.properties);

	// Set device properties
	priv->base.properties.hsize = priv->display_info.h_screen_size;
	priv->base.properties.vsize = priv->display_info.v_screen_size;
	priv->base.properties.hres = priv->display_info.h_resolution;
	priv->base.properties.vres = priv->display_info.v_resolution;
	priv->base.properties.lens_sep = priv->display_info.lens_separation;
	priv->base.properties.lens_vpos = priv->display_info.v_center;
	priv->base.properties.ratio = ((float)priv->display_info.h_resolution / (float)priv->display_info.v_resolution) / 2.0f;

	//setup generic distortion coeffs, from hand-calibration
	switch (desc->revision) {
		case REV_DK2:
			ohmd_set_universal_distortion_k(&(priv->base.properties), 0.247, -0.145, 0.103, 0.795);
			ohmd_set_universal_aberration_k(&(priv->base.properties), 0.985, 1.000, 1.015);
			break;
		case REV_DK1:
			ohmd_set_universal_distortion_k(&(priv->base.properties), 1.003, -1.005, 0.403, 0.599);
			ohmd_set_universal_aberration_k(&(priv->base.properties), 0.985, 1.000, 1.015);
			break;
		case REV_CV1:
			ohmd_set_universal_distortion_k(&(priv->base.properties), 0.098, .324, -0.241, 0.819);
			ohmd_set_universal_aberration_k(&(priv->base.properties), 0.9952420, 1.0, 1.0008074);
			/* CV1 reports IPD, but not lens center, at least not anywhere I could find, so use the manually measured value of 0.054 */
			priv->display_info.lens_separation = 0.054;
			priv->base.properties.lens_sep = priv->display_info.lens_separation;
		default:
			break;
	}

	// calculate projection eye projection matrices from the device properties
	//ohmd_calc_default_proj_matrices(&priv->base.properties);
	float l,r,t,b,n,f;
	// left eye screen bounds
	l = -1.0f * (priv->display_info.h_screen_size/2 - priv->display_info.lens_separation/2);
	r = priv->display_info.lens_separation/2;
	t = priv->display_info.v_screen_size - priv->display_info.v_center;
	b = -1.0f * priv->display_info.v_center;
	n = priv->display_info.eye_to_screen_distance[0];
	f = n*10e6;
	//LOGD("l: %0.3f, r: %0.3f, b: %0.3f, t: %0.3f, n: %0.3f, f: %0.3f", l,r,b,t,n,f);
	/* eye separation is handled by IPD in the Modelview matrix */
	omat4x4f_init_frustum(&priv->base.properties.proj_left, l, r, b, t, n, f);
	//right eye screen bounds
	l = -1.0f * priv->display_info.lens_separation/2;
	r = priv->display_info.h_screen_size/2 - priv->display_info.lens_separation/2;
	n = priv->display_info.eye_to_screen_distance[1];
	f = n*10e6;
	//LOGD("l: %0.3f, r: %0.3f, b: %0.3f, t: %0.3f, n: %0.3f, f: %0.3f", l,r,b,t,n,f);
	/* eye separation is handled by IPD in the Modelview matrix */
	omat4x4f_init_frustum(&priv->base.properties.proj_right, l, r, b, t, n, f);

	priv->base.properties.fov = 2 * atan2f(
			priv->display_info.h_screen_size/2 - priv->display_info.lens_separation/2,
			priv->display_info.eye_to_screen_distance[0]);

	// set up device callbacks
	priv->base.update = update_device;
	priv->base.close = close_device;
	priv->base.getf = getf;

	// initialize sensor fusion
	ofusion_init(&priv->sensor_fusion);

	return &priv->base;

cleanup:
	if(priv) {
		if (priv->leds)
			free (priv->leds);
		free(priv);
	}

	return NULL;
}

#define OCULUS_VR_INC_ID 0x2833
#define SAMSUNG_ELECTRONICS_CO_ID 0x04e8
#define RIFT_ID_COUNT 5

static void get_device_list(ohmd_driver* driver, ohmd_device_list* list)
{
	// enumerate HID devices and add any Rifts found to the device list

	rift_devices rd[RIFT_ID_COUNT] = {
		{ "Rift (DK1)", OCULUS_VR_INC_ID, 0x0001,	-1, REV_DK1 },
		{ "Rift (DK2)", OCULUS_VR_INC_ID, 0x0021,	-1, REV_DK2 },
		{ "Rift (DK2)", OCULUS_VR_INC_ID, 0x2021,	-1, REV_DK2 },
		{ "Rift (CV1)", OCULUS_VR_INC_ID, 0x0031,	 0, REV_CV1 },

		{ "GearVR (Gen1)", SAMSUNG_ELECTRONICS_CO_ID, 0xa500,	 0, REV_GEARVR_GEN1 },
	};

	for(int i = 0; i < RIFT_ID_COUNT; i++){
		struct hid_device_info* devs = hid_enumerate(rd[i].company, rd[i].id);
		struct hid_device_info* cur_dev = devs;

		if(devs == NULL)
			continue;

		while (cur_dev) {
			if(rd[i].iface == -1 || cur_dev->interface_number == rd[i].iface){
				ohmd_device_desc* desc = &list->devices[list->num_devices++];

				strcpy(desc->driver, "OpenHMD Rift Driver");
				strcpy(desc->vendor, "Oculus VR, Inc.");
				strcpy(desc->product, rd[i].name);

				desc->revision = rd[i].rev;
		
				desc->device_class = OHMD_DEVICE_CLASS_HMD;
				desc->device_flags = OHMD_DEVICE_FLAGS_ROTATIONAL_TRACKING;

				strcpy(desc->path, cur_dev->path);

				desc->driver_ptr = driver;
			}

			cur_dev = cur_dev->next;
		}

		hid_free_enumeration(devs);
	}
}

static void destroy_driver(ohmd_driver* drv)
{
	LOGD("shutting down driver");
	hid_exit();
	free(drv);

	ohmd_toggle_ovr_service(1); //re-enable OVRService if previously running
}

ohmd_driver* ohmd_create_oculus_rift_drv(ohmd_context* ctx)
{
	ohmd_driver* drv = ohmd_alloc(ctx, sizeof(ohmd_driver));
	if(drv == NULL)
		return NULL;

	ohmd_toggle_ovr_service(0); //disable OVRService if running

	drv->get_device_list = get_device_list;
	drv->open_device = open_device;
	drv->destroy = destroy_driver;
	drv->ctx = ctx;

	return drv;
}
