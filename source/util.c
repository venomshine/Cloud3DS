#include "util.h"
#include "i18n.h"

#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

u64 util_ms(void)
{
	return svcGetSystemTick() / CPU_TICKS_PER_MSEC;
}

u64 util_unix_now(void)
{
	return (u64)time(NULL);
}

static const char *const WD_ES[] = {"dom", "lun", "mar", "mié", "jue", "vie", "sáb"};
static const char *const WD_EN[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const WD_JA[] = {"日", "月", "火", "水", "木", "金", "土"};
static const char *const MO_ES[] = {"ene", "feb", "mar", "abr", "may", "jun",
                                    "jul", "ago", "sep", "oct", "nov", "dic"};
static const char *const MO_EN[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static void fmt_date(char *out, size_t n, const struct tm *t)
{
	switch (i18n_lang()) {
	case LANG_EN:
		snprintf(out, n, "%s, %s %d", WD_EN[t->tm_wday], MO_EN[t->tm_mon], t->tm_mday);
		break;
	case LANG_JA:
		snprintf(out, n, "%d/%d(%s)", t->tm_mon + 1, t->tm_mday, WD_JA[t->tm_wday]);
		break;
	default:
		snprintf(out, n, "%s %d %s", WD_ES[t->tm_wday], t->tm_mday, MO_ES[t->tm_mon]);
		break;
	}
}

void util_fmt_clock(char *out, size_t n)
{
	time_t now = time(NULL);
	struct tm t;
	gmtime_r(&now, &t); /* el reloj de la 3DS ya está en hora local */
	char d[32];
	fmt_date(d, sizeof d, &t);
	snprintf(out, n, "%s · %02d:%02d", d, t.tm_hour, t.tm_min);
}

void util_fmt_hm(char *out, size_t n, u64 unix_s)
{
	time_t ts = (time_t)unix_s;
	struct tm t;
	gmtime_r(&ts, &t);
	snprintf(out, n, "%02d:%02d", t.tm_hour, t.tm_min);
}

void util_fmt_when(char *out, size_t n, u64 unix_s)
{
	time_t ts = (time_t)unix_s, now = time(NULL);
	struct tm t, tn;
	gmtime_r(&ts, &t);
	gmtime_r(&now, &tn);
	long day = (long)(ts / 86400), today = (long)(now / 86400);
	if (day == today)
		snprintf(out, n, "%s %02d:%02d", _("Hoy"), t.tm_hour, t.tm_min);
	else if (day == today - 1)
		snprintf(out, n, "%s %02d:%02d", _("Ayer"), t.tm_hour, t.tm_min);
	else if (i18n_lang() == LANG_EN)
		snprintf(out, n, "%s %d", MO_EN[t.tm_mon], t.tm_mday);
	else if (i18n_lang() == LANG_JA)
		snprintf(out, n, "%d/%d", t.tm_mon + 1, t.tm_mday);
	else
		snprintf(out, n, "%d %s", t.tm_mday, MO_ES[t.tm_mon]);
}

static char dec_sep(void)
{
	return i18n_lang() == LANG_ES ? ',' : '.';
}

void util_fmt_size(char *out, size_t n, u64 b)
{
	static const char *const U[] = {"B", "KB", "MB", "GB", "TB"};
	if (b < 1024) {
		snprintf(out, n, "%u B", (unsigned)b);
		return;
	}
	double v = (double)b;
	int u = 0;
	while (v >= 1024.0 && u < 4) {
		v /= 1024.0;
		u++;
	}
	if (v >= 100.0)
		snprintf(out, n, "%.0f %s", v, U[u]);
	else {
		snprintf(out, n, "%.1f %s", v, U[u]);
		char *p = strchr(out, '.');
		if (p)
			*p = dec_sep();
	}
}

void util_fmt_speed(char *out, size_t n, u64 bps)
{
	char s[24];
	util_fmt_size(s, sizeof s, bps);
	snprintf(out, n, "%s/s", s);
}

void util_fmt_eta(char *out, size_t n, u64 secs)
{
	if (secs < 60)
		snprintf(out, n, _("%d s"), (int)secs);
	else if (secs < 3600)
		snprintf(out, n, _("%d min"), (int)((secs + 30) / 60));
	else
		snprintf(out, n, _("%d h %d min"), (int)(secs / 3600), (int)((secs % 3600) / 60));
}

int util_battery(bool *charging)
{
	static u64 last;
	static int pct = -1;
	static bool chg;
	u64 now = util_ms();
	if (pct < 0 || now - last > 5000) {
		last = now;
		u8 lvl = 0, c = 0;
		bool got = false;
		if (R_SUCCEEDED(mcuHwcInit())) {
			if (R_SUCCEEDED(MCUHWC_GetBatteryLevel(&lvl))) {
				pct = lvl > 100 ? 100 : lvl;
				got = true;
			}
			mcuHwcExit();
		}
		/* sin acceso a mcu::HWC: nivel aproximado (0-5) */
		if (!got && R_SUCCEEDED(PTMU_GetBatteryLevel(&lvl)))
			pct = lvl * 20;
		if (R_SUCCEEDED(PTMU_GetBatteryChargeState(&c)))
			chg = c != 0;
		if (pct < 0)
			pct = 100;
	}
	if (charging)
		*charging = chg;
	return pct;
}

u32 util_ip(void)
{
	return (u32)gethostid();
}

void util_rand_bytes(void *buf, size_t n)
{
	/* PS_GenerateRandomBytes usa el generador del sistema */
	static bool ok;
	if (!ok)
		ok = R_SUCCEEDED(psInit());
	if (!ok || R_FAILED(PS_GenerateRandomBytes(buf, n))) {
		u8 *p = buf;
		u64 x = svcGetSystemTick() ^ 0x9E3779B97F4A7C15ull;
		for (size_t i = 0; i < n; i++) {
			x ^= x << 13;
			x ^= x >> 7;
			x ^= x << 17;
			p[i] = (u8)x;
		}
	}
}

void util_rand_hex(char *out, int nchars)
{
	static const char H[] = "0123456789abcdef";
	u8 b[64];
	int nb = (nchars + 1) / 2;
	if (nb > (int)sizeof b)
		nb = sizeof b;
	util_rand_bytes(b, nb);
	for (int i = 0; i < nchars; i++)
		out[i] = H[(b[i / 2] >> ((i & 1) ? 0 : 4)) & 15];
	out[nchars] = 0;
}

size_t util_strlcpy(char *dst, const char *src, size_t n)
{
	size_t l = strlen(src);
	if (n) {
		size_t c = l < n - 1 ? l : n - 1;
		memcpy(dst, src, c);
		dst[c] = 0;
	}
	return l;
}

static int hexv(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

int util_url_decode(char *s)
{
	char *o = s;
	for (; *s; s++) {
		if (*s == '%') {
			int a = hexv(s[1]), b = a < 0 ? -1 : hexv(s[2]);
			if (b < 0)
				return -1;
			*o++ = (char)(a * 16 + b);
			s += 2;
		} else if (*s == '+') {
			*o++ = ' ';
		} else {
			*o++ = *s;
		}
	}
	*o = 0;
	return 0;
}

void util_utf16_to_utf8(char *out, size_t n, const u16 *in, size_t max_in)
{
	size_t o = 0;
	for (size_t i = 0; i < max_in && in[i]; i++) {
		u32 c = in[i];
		if (c >= 0xD800 && c < 0xDC00 && i + 1 < max_in && in[i + 1] >= 0xDC00 && in[i + 1] < 0xE000) {
			c = 0x10000 + ((c - 0xD800) << 10) + (in[i + 1] - 0xDC00);
			i++;
		}
		u8 b[4];
		int k;
		if (c < 0x80) {
			b[0] = c;
			k = 1;
		} else if (c < 0x800) {
			b[0] = 0xC0 | (c >> 6);
			b[1] = 0x80 | (c & 63);
			k = 2;
		} else if (c < 0x10000) {
			b[0] = 0xE0 | (c >> 12);
			b[1] = 0x80 | ((c >> 6) & 63);
			b[2] = 0x80 | (c & 63);
			k = 3;
		} else {
			b[0] = 0xF0 | (c >> 18);
			b[1] = 0x80 | ((c >> 12) & 63);
			b[2] = 0x80 | ((c >> 6) & 63);
			b[3] = 0x80 | (c & 63);
			k = 4;
		}
		if (o + k >= n)
			break;
		memcpy(out + o, b, k);
		o += k;
	}
	if (n)
		out[o < n ? o : n - 1] = 0;
}

bool util_utf8_valid(const char *s)
{
	const u8 *p = (const u8 *)s;
	while (*p) {
		int k;
		if (*p < 0x80)
			k = 0;
		else if ((*p & 0xE0) == 0xC0)
			k = 1;
		else if ((*p & 0xF0) == 0xE0)
			k = 2;
		else if ((*p & 0xF8) == 0xF0)
			k = 3;
		else
			return false;
		p++;
		while (k--) {
			if ((*p & 0xC0) != 0x80)
				return false;
			p++;
		}
	}
	return true;
}

static LightLock log_lock;
static bool log_init;

void util_log(const char *fmt, ...)
{
	if (!log_init) {
		LightLock_Init(&log_lock);
		log_init = true;
	}
	LightLock_Lock(&log_lock);
	FILE *f = fopen(LOG_PATH, "a");
	if (f) {
		/* el registro no crece sin límite */
		fseek(f, 0, SEEK_END);
		if (ftell(f) > 256 * 1024) {
			fclose(f);
			f = fopen(LOG_PATH, "w");
		}
	}
	if (f) {
		char t[16];
		util_fmt_hm(t, sizeof t, util_unix_now());
		fprintf(f, "[%s %llu] ", t, (unsigned long long)util_ms());
		va_list ap;
		va_start(ap, fmt);
		vfprintf(f, fmt, ap);
		va_end(ap);
		fputc('\n', f);
		fclose(f);
	}
	LightLock_Unlock(&log_lock);
}
