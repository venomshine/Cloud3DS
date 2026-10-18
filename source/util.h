#pragma once

#include "app.h"
#include <stddef.h>

/* Tiempo */
u64  util_ms(void);                       /* milisegundos desde el arranque */
void util_fmt_clock(char *out, size_t n); /* "sáb 18 oct · 14:30" según idioma */
void util_fmt_hm(char *out, size_t n, u64 unix_s);
void util_fmt_when(char *out, size_t n, u64 unix_s); /* "Hoy 14:30", "Ayer", "15 oct" */
u64  util_unix_now(void);

/* Tamaños y velocidades */
void util_fmt_size(char *out, size_t n, u64 bytes);
void util_fmt_speed(char *out, size_t n, u64 bytes_per_s);
void util_fmt_eta(char *out, size_t n, u64 secs);

/* Batería y Wi-Fi */
int  util_battery(bool *charging); /* porcentaje 0-100, cacheado */
u32  util_ip(void);                /* 0 si no hay Wi-Fi */

/* Cadenas */
void util_rand_bytes(void *buf, size_t n);
void util_rand_hex(char *out, int nchars);
size_t util_strlcpy(char *dst, const char *src, size_t n);
int  util_url_decode(char *s);    /* en el sitio; -1 si mal formado */
void util_utf16_to_utf8(char *out, size_t n, const u16 *in, size_t max_in);
bool util_utf8_valid(const char *s);

/* Registro de diagnóstico en la SD */
void util_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
