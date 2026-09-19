/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ulanzi TC001 system test.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/input/input.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/sys/timeutil.h>

#define W 32
#define H 8

static int n_pass, n_fail;

#define CHECK(name, cond, fmt, ...)                                                    \
	do {                                                                           \
		if (cond) {                                                            \
			n_pass++;                                                      \
			printk("[PASS] %s: " fmt "\n", name, ##__VA_ARGS__);           \
		} else {                                                               \
			n_fail++;                                                      \
			printk("[FAIL] %s: " fmt "\n", name, ##__VA_ARGS__);           \
		}                                                                      \
	} while (0)

#define INFO(fmt, ...) printk("[INFO] " fmt "\n", ##__VA_ARGS__)

static const struct device *const disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static const struct device *const i2c = DEVICE_DT_GET(DT_ALIAS(i2c_0));
static const struct device *const rtc = DEVICE_DT_GET(DT_ALIAS(rtc));
static const struct device *const sht = DEVICE_DT_GET(DT_NODELABEL(sht3xd));
static const struct device *const trng = DEVICE_DT_GET(DT_NODELABEL(trng0));
static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static const struct pwm_dt_spec buzzer = PWM_DT_SPEC_GET(DT_NODELABEL(buzzer0));
static const struct adc_dt_spec adc_ch[] = {
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0),
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 1),
};

/* ---- display helpers ---- */

static uint8_t fb[W * H * 3];

static void px(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
	uint8_t *p = &fb[(y * W + x) * 3];

	p[0] = r;
	p[1] = g;
	p[2] = b;
}

static void fill(uint8_t r, uint8_t g, uint8_t b)
{
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			px(x, y, r, g, b);
		}
	}
}

static int flush(void)
{
	struct display_buffer_descriptor d = {
		.buf_size = sizeof(fb), .width = W, .height = H, .pitch = W,
	};

	return display_write(disp, 0, 0, &d, fb);
}

/* ---- input ---- */

static atomic_t key_press, key_release;

static void input_cb(struct input_event *evt, void *user_data)
{
	int bit;

	ARG_UNUSED(user_data);

	if (evt->type != INPUT_EV_KEY) {
		return;
	}

	switch (evt->code) {
	case INPUT_KEY_LEFT:
		bit = 0;
		break;
	case INPUT_KEY_ENTER:
		bit = 1;
		break;
	case INPUT_KEY_RIGHT:
		bit = 2;
		break;
	default:
		INFO("unexpected key code %u", evt->code);
		return;
	}

	atomic_or(evt->value ? &key_press : &key_release, BIT(bit));
}
INPUT_CALLBACK_DEFINE(NULL, input_cb, NULL);

/* ---- ADC ---- */

static int read_adc(int idx, int32_t *raw_out, int32_t *mv_out)
{
	int16_t raw;
	struct adc_sequence seq = {.buffer = &raw, .buffer_size = sizeof(raw)};
	int32_t mv;
	int ret;

	adc_sequence_init_dt(&adc_ch[idx], &seq);
	ret = adc_read_dt(&adc_ch[idx], &seq);
	if (ret) {
		return ret;
	}

	mv = raw;
	ret = adc_raw_to_millivolts_dt(&adc_ch[idx], &mv);
	*raw_out = raw;
	*mv_out = ret ? -1 : mv;
	return 0;
}

/* ---- automatic tests ---- */

static void test_entropy(void)
{
	uint8_t a[16] = {0}, b[16] = {0};
	int r1, r2;

	r1 = device_is_ready(trng) ? entropy_get_entropy(trng, a, sizeof(a)) : -ENODEV;
	r2 = device_is_ready(trng) ? entropy_get_entropy(trng, b, sizeof(b)) : -ENODEV;
	CHECK("trng", r1 == 0 && r2 == 0 && memcmp(a, b, sizeof(a)) != 0,
	      "ret=%d/%d, %02x%02x%02x%02x.. vs %02x%02x%02x%02x..", r1, r2,
	      a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
}

static void test_i2c(void)
{
	bool found44 = false, found68 = false;
	char list[128] = "";

	if (!device_is_ready(i2c)) {
		CHECK("i2c", false, "bus not ready");
		return;
	}

	for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
		struct i2c_msg msg = {.buf = NULL, .len = 0, .flags = I2C_MSG_WRITE | I2C_MSG_STOP};

		if (i2c_transfer(i2c, &msg, 1, addr) == 0) {
			char tmp[8];

			snprintk(tmp, sizeof(tmp), "0x%02x ", addr);
			strncat(list, tmp, sizeof(list) - strlen(list) - 1);
			found44 |= addr == 0x44;
			found68 |= addr == 0x68;
		}
	}
	CHECK("i2c scan", found44 && found68, "responders: %s(expect 0x44 0x68)", list);
}

static time_t rtc_secs(const struct rtc_time *t)
{
	return timeutil_timegm((const struct tm *)t);
}

static void test_rtc(void)
{
	struct rtc_time t0 = {0}, t1 = {0};
	int ret;

	if (!device_is_ready(rtc)) {
		CHECK("rtc", false, "device not ready");
		return;
	}

	ret = rtc_get_time(rtc, &t0);
	INFO("rtc initial get_time ret=%d", ret);
	if (ret) {
		t0 = (struct rtc_time){.tm_year = 126, .tm_mon = 8, .tm_mday = 19,
				       .tm_hour = 14, .tm_wday = 6};
		INFO("rtc not set, initializing to 2026-09-19 14:00:00");
	}

	ret = rtc_set_time(rtc, &t0);
	CHECK("rtc set_time", ret == 0, "ret=%d", ret);

	k_sleep(K_MSEC(2200));

	ret = rtc_get_time(rtc, &t1);
	if (ret == 0) {
		long dt = (long)(rtc_secs(&t1) - rtc_secs(&t0));

		CHECK("rtc ticking", dt >= 2 && dt <= 3,
		      "%04d-%02d-%02d %02d:%02d:%02d, advanced %lds in 2.2s",
		      t1.tm_year + 1900, t1.tm_mon + 1, t1.tm_mday, t1.tm_hour, t1.tm_min,
		      t1.tm_sec, dt);
	} else {
		CHECK("rtc ticking", false, "get_time ret=%d", ret);
	}
}

static void test_sht(void)
{
	struct sensor_value t, h;
	int ret;

	if (!device_is_ready(sht)) {
		CHECK("sht3xd", false, "device not ready");
		return;
	}

	ret = sensor_sample_fetch(sht);
	if (ret == 0) {
		ret = sensor_channel_get(sht, SENSOR_CHAN_AMBIENT_TEMP, &t);
	}
	if (ret == 0) {
		ret = sensor_channel_get(sht, SENSOR_CHAN_HUMIDITY, &h);
	}
	if (ret) {
		CHECK("sht3xd", false, "ret=%d", ret);
		return;
	}
	CHECK("sht3xd", sensor_value_to_double(&t) > -10 && sensor_value_to_double(&t) < 60 &&
			sensor_value_to_double(&h) > 0 && sensor_value_to_double(&h) <= 100,
	      "%d.%02d C, %d.%02d %%RH", t.val1, t.val2 / 10000, h.val1, h.val2 / 10000);
}

static void test_adc(void)
{
	static const char *const names[] = {"battery (GPIO34)", "ldr (GPIO35)"};

	for (int i = 0; i < ARRAY_SIZE(adc_ch); i++) {
		int32_t raw = 0, mv = 0;
		int ret;

		if (!adc_is_ready_dt(&adc_ch[i])) {
			CHECK("adc", false, "%s not ready", names[i]);
			continue;
		}
		ret = adc_channel_setup_dt(&adc_ch[i]);
		if (ret == 0) {
			ret = read_adc(i, &raw, &mv);
		}
		CHECK(names[i], ret == 0 && raw > 0, "ret=%d raw=%d mv=%d", ret, raw, mv);
	}
}

static K_SEM_DEFINE(scan_done, 0, 1);
static int scan_count;

static void wifi_evt(struct net_mgmt_event_callback *cb, uint64_t evt, struct net_if *iface)
{
	if (evt == NET_EVENT_WIFI_SCAN_RESULT) {
		const struct wifi_scan_result *r = cb->info;

		if (scan_count < 8) {
			INFO("  wifi ap: ch%-2u %4d dBm  %.*s", r->channel, r->rssi, r->ssid_length,
			     r->ssid);
		}
		scan_count++;
	} else if (evt == NET_EVENT_WIFI_SCAN_DONE) {
		k_sem_give(&scan_done);
	}
}

static void test_wifi(void)
{
	static struct net_mgmt_event_callback cb;
	struct net_if *iface = net_if_get_first_wifi();
	struct wifi_scan_params params = {0};
	int ret;

	if (!iface) {
		CHECK("wifi scan", false, "no wifi interface");
		return;
	}

	net_mgmt_init_event_callback(&cb, wifi_evt,
				     NET_EVENT_WIFI_SCAN_RESULT | NET_EVENT_WIFI_SCAN_DONE);
	net_mgmt_add_event_callback(&cb);
	net_if_up(iface);

	ret = net_mgmt(NET_REQUEST_WIFI_SCAN, iface, &params, sizeof(params));
	if (ret == 0) {
		ret = k_sem_take(&scan_done, K_SECONDS(15));
	}
	CHECK("wifi scan", ret == 0 && scan_count > 0, "ret=%d, %d access points", ret, scan_count);
}

static int bt_count;

static void bt_scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
		       struct net_buf_simple *buf)
{
	bt_count++;
}

static void test_bt(void)
{
	int ret = bt_enable(NULL);

	if (ret) {
		CHECK("bluetooth", false, "bt_enable ret=%d", ret);
		return;
	}
	ret = bt_le_scan_start(BT_LE_SCAN_PASSIVE, bt_scan_cb);
	if (ret) {
		CHECK("bluetooth", false, "scan_start ret=%d", ret);
		return;
	}
	k_sleep(K_SECONDS(6));
	bt_le_scan_stop();
	CHECK("bluetooth", bt_count > 0, "%d advertising reports in 6s", bt_count);
}

static void test_wdt_expire(void)
{
	struct wdt_timeout_cfg cfg = {
		.window = {.min = 0, .max = 2000},
		.flags = WDT_FLAG_RESET_SOC,
	};
	int ch;

	if (!device_is_ready(wdt)) {
		CHECK("watchdog", false, "device not ready");
		return;
	}
	ch = wdt_install_timeout(wdt, &cfg);
	if (ch < 0 || wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG) < 0) {
		CHECK("watchdog", false, "install/setup failed (%d)", ch);
		return;
	}
	for (int i = 0; i < 6; i++) {
		wdt_feed(wdt, ch);
		k_sleep(K_MSEC(500));
	}
	CHECK("watchdog feed", true, "survived 3s with 2s timeout while fed");
	INFO("watchdog: not feeding, expecting reset within ~2s");
	k_sleep(K_SECONDS(6));
	CHECK("watchdog reset", false, "no reset after 6s without feeding");
}

/* ---- interactive tests ---- */

static void test_buzzer(void)
{
	static const uint32_t freqs[] = {1000, 2000, 2700, 4000};
	int ret = 0;

	if (!pwm_is_ready_dt(&buzzer)) {
		CHECK("buzzer", false, "pwm not ready");
		return;
	}

	INFO("BUZZER: silence for 2s, then 4 beeps (1k, 2k, 2.7k, 4k Hz), then silence");
	pwm_set_pulse_dt(&buzzer, 0);
	k_sleep(K_SECONDS(2));
	for (int i = 0; i < ARRAY_SIZE(freqs); i++) {
		uint32_t period = 1000000000U / freqs[i];

		ret |= pwm_set_dt(&buzzer, period, period / 2);
		k_sleep(K_MSEC(500));
		ret |= pwm_set_pulse_dt(&buzzer, 0);
		k_sleep(K_MSEC(400));
	}
	CHECK("buzzer pwm", ret == 0, "ret=%d (audible result needs a human)", ret);
}

static void test_matrix(void)
{
	struct display_capabilities caps;
	int ret = 0;

	if (!device_is_ready(disp)) {
		CHECK("matrix", false, "display not ready");
		return;
	}
	display_get_capabilities(disp, &caps);
	CHECK("matrix caps", caps.x_resolution == W && caps.y_resolution == H &&
			     caps.current_pixel_format == PIXEL_FORMAT_RGB_888,
	      "%ux%u fmt=0x%x", caps.x_resolution, caps.y_resolution, caps.current_pixel_format);

	INFO("MATRIX 1/3: corner markers: top-left RED, top-right GREEN, bottom-left BLUE, "
	     "bottom-right WHITE");
	fill(0, 0, 0);
	px(0, 0, 60, 0, 0);
	px(W - 1, 0, 0, 60, 0);
	px(0, H - 1, 0, 0, 60);
	px(W - 1, H - 1, 40, 40, 40);
	ret |= flush();
	k_sleep(K_SECONDS(6));

	INFO("MATRIX 2/3: one white pixel sweeping in raster order (row by row, left to right)");
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			fill(0, 0, 0);
			px(x, y, 40, 40, 40);
			ret |= flush();
			k_sleep(K_MSEC(25));
		}
	}

	INFO("MATRIX 3/3: full panel RED, GREEN, BLUE, 2s each");
	fill(16, 0, 0);
	ret |= flush();
	k_sleep(K_SECONDS(2));
	fill(0, 16, 0);
	ret |= flush();
	k_sleep(K_SECONDS(2));
	fill(0, 0, 16);
	ret |= flush();
	k_sleep(K_SECONDS(2));

	CHECK("matrix write", ret == 0, "ret=%d (visual result needs a human)", ret);
}

static void draw_buttons(void)
{
	fill(0, 0, 0);
	for (int b = 0; b < 3; b++) {
		int x0 = b * 11;
		bool ok = (atomic_get(&key_press) & BIT(b)) != 0;

		for (int y = 2; y < 6; y++) {
			for (int x = x0; x < x0 + 10; x++) {
				px(x, y, ok ? 0 : 30, ok ? 30 : 0, 0);
			}
		}
	}
	flush();
}

static void test_buttons_ldr(void)
{
	int32_t ldr_min = INT32_MAX, ldr_max = -1;
	int64_t end = k_uptime_get() + 30000;
	int64_t next_ldr = 0;

	INFO("BUTTONS: press LEFT, MIDDLE, RIGHT (30s). Also cover/uncover the light sensor.");
	INFO("         Blocks on the matrix are red until pressed, then green.");
	atomic_set(&key_press, 0);
	atomic_set(&key_release, 0);
	draw_buttons();

	while (k_uptime_get() < end && atomic_get(&key_press) != 0x7) {
		int32_t raw, mv;

		draw_buttons();
		if (k_uptime_get() >= next_ldr && read_adc(1, &raw, &mv) == 0) {
			ldr_min = MIN(ldr_min, mv);
			ldr_max = MAX(ldr_max, mv);
			next_ldr = k_uptime_get() + 500;
		}
		k_sleep(K_MSEC(50));
	}
	draw_buttons();
	/* keep sampling the LDR for the remainder of a 10s minimum window */
	end = k_uptime_get() + 8000;
	INFO("LDR: keep covering/uncovering the light sensor for 8 more seconds");
	while (k_uptime_get() < end) {
		int32_t raw, mv;

		if (read_adc(1, &raw, &mv) == 0) {
			ldr_min = MIN(ldr_min, mv);
			ldr_max = MAX(ldr_max, mv);
		}
		k_sleep(K_MSEC(100));
	}

	CHECK("button left", atomic_get(&key_press) & BIT(0), "press=%d release=%d",
	      !!(atomic_get(&key_press) & BIT(0)), !!(atomic_get(&key_release) & BIT(0)));
	CHECK("button middle", atomic_get(&key_press) & BIT(1), "press=%d release=%d",
	      !!(atomic_get(&key_press) & BIT(1)), !!(atomic_get(&key_release) & BIT(1)));
	CHECK("button right", atomic_get(&key_press) & BIT(2), "press=%d release=%d",
	      !!(atomic_get(&key_press) & BIT(2)), !!(atomic_get(&key_release) & BIT(2)));
	INFO("ldr range while covering: %d..%d mV (delta %d mV)", ldr_min, ldr_max,
	     ldr_max - ldr_min);
	CHECK("ldr varies", (ldr_max - ldr_min) > 100, "delta %d mV", ldr_max - ldr_min);
}

/* ---- radio discoverability tests (end with the middle button) ---- */

#define RADIO_NAME "tc001-zephyr"

static void first_column(uint8_t r, uint8_t g, uint8_t b)
{
	fill(0, 0, 0);
	for (int y = 0; y < H; y++) {
		px(0, y, r, g, b);
	}
	flush();
}

static void wait_middle(void)
{
	atomic_clear_bit(&key_press, 1);
	while (!(atomic_get(&key_press) & BIT(1))) {
		k_sleep(K_MSEC(50));
	}
}

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, RADIO_NAME, sizeof(RADIO_NAME) - 1),
};

static void test_bt_discoverable(void)
{
	int ret = bt_enable(NULL);

	if (ret == -EALREADY) {
		ret = 0;
	}
	if (ret == 0) {
		ret = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), NULL, 0);
	}
	CHECK("bt discoverable", ret == 0, "advertising as '%s' ret=%d", RADIO_NAME, ret);
	if (ret) {
		return;
	}

	first_column(0, 0, 60);
	INFO("BT: first column BLUE, clock is discoverable as '%s'. Press MIDDLE when done.",
	     RADIO_NAME);
	wait_middle();
	bt_le_adv_stop();
	INFO("BT test finished");
}

static void test_wifi_ap(void)
{
	struct net_if *iface = net_if_get_wifi_sap();
	struct wifi_connect_req_params params = {
		.ssid = RADIO_NAME,
		.ssid_length = sizeof(RADIO_NAME) - 1,
		.security = WIFI_SECURITY_TYPE_NONE,
		.band = WIFI_FREQ_BAND_2_4_GHZ,
		.channel = 6,
	};
	struct net_in_addr addr, mask, pool;
	int ret;

	if (iface == NULL) {
		iface = net_if_get_first_wifi();
	}
	if (iface == NULL) {
		CHECK("wifi ap", false, "no wifi interface");
		return;
	}

	net_addr_pton(NET_AF_INET, "192.168.4.1", &addr);
	net_addr_pton(NET_AF_INET, "255.255.255.0", &mask);
	net_addr_pton(NET_AF_INET, "192.168.4.10", &pool);
	net_if_up(iface);
	net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0);
	net_if_ipv4_set_netmask_by_addr(iface, &addr, &mask);

	ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, iface, &params, sizeof(params));
	CHECK("wifi ap enable", ret == 0, "ssid '%s' open, ch6, ret=%d", RADIO_NAME, ret);
	if (ret) {
		return;
	}
	ret = net_dhcpv4_server_start(iface, &pool);
	INFO("dhcp server on 192.168.4.1, pool from 192.168.4.10: ret=%d", ret);

	first_column(40, 40, 40);
	INFO("WIFI: first column WHITE, open AP '%s' is up. Press MIDDLE when done.", RADIO_NAME);
	wait_middle();
	net_mgmt(NET_REQUEST_WIFI_AP_DISABLE, iface, NULL, 0);
	INFO("WiFi test finished");
}

int main(void)
{
	uint32_t cause = 0;
	int ret = hwinfo_get_reset_cause(&cause);

	hwinfo_clear_reset_cause();
	INFO("reset cause: ret=%d flags=0x%x%s%s%s%s", ret, cause,
	     (cause & RESET_POR) ? " POR" : "", (cause & RESET_PIN) ? " PIN" : "",
	     (cause & RESET_SOFTWARE) ? " SOFTWARE" : "", (cause & RESET_WATCHDOG) ? " WATCHDOG" : "");

	if (cause & RESET_WATCHDOG) {
		CHECK("watchdog reset", true, "reset cause reports WATCHDOG after expiry");
	} else {
		INFO("== automatic tests ==");
		test_entropy();
		test_i2c();
		test_rtc();
		test_sht();
		test_adc();
		test_wifi();
		test_bt();
		test_wdt_expire();
	}

	INFO("== interactive tests ==");
	test_buzzer();
	test_matrix();
	test_buttons_ldr();
	test_bt_discoverable();
	test_wifi_ap();

	fill(0, 0, 0);
	flush();
	INFO("SUMMARY (this boot): pass=%d fail=%d", n_pass, n_fail);
	INFO("ALL DONE");
	return 0;
}
