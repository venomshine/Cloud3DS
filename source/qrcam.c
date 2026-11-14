/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 *
 * La secuencia de la cámara sigue la de FBI (Steveice10, licencia MIT).
 */
#include "qrcam.h"
#include "quirc.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_BYTES (QRCAM_W * QRCAM_H * 2)

static struct {
	Thread thr;
	Handle cancel;
	LightLock lock;
	bool init;
	volatile int st;
	Result err;
	bool inner;
	u16 *cap;          /* donde escribe la cámara */
	u16 *front;        /* la última imagen completa, para enseñarla */
	u32 frame;         /* cuántas van */
	char text[1024];   /* lo leído */
	bool have;
	struct quirc *q;
	s32 prio_cap, prio_scan; /* recibir imágenes, por delante de la interfaz; buscar el QR, por detrás */
} C;

/* busca un QR en la imagen (gris a partir de RGB565) */
static void scan(const u16 *px)
{
	int w, h;
	u8 *g = quirc_begin(C.q, &w, &h);
	for (int i = 0; i < w * h; i++) {
		u16 p = px[i];
		g[i] = (u8)((((p >> 11) & 31) * 8 * 77 + ((p >> 5) & 63) * 4 * 150 + (p & 31) * 8 * 29) >> 8);
	}
	quirc_end(C.q);
	int n = quirc_count(C.q);
	for (int i = 0; i < n; i++) {
		struct quirc_code code;
		struct quirc_data data;
		quirc_extract(C.q, i, &code);
		if (quirc_decode(&code, &data) != QUIRC_SUCCESS) { // también puede estar del revés (espejo)
			quirc_flip(&code);
			if (quirc_decode(&code, &data) != QUIRC_SUCCESS)
				continue;
		}
		LightLock_Lock(&C.lock);
		if (!C.have) {
			snprintf(C.text, sizeof C.text, "%.*s", data.payload_len, (const char *)data.payload);
			C.have = true;
		}
		LightLock_Unlock(&C.lock);
		return;
	}
}

static void cam_thread(void *arg)
{
	(void)arg;
	Result r = camInit();
	u32 sel = C.inner ? SELECT_IN1 : SELECT_OUT1;
	Handle ev[3] = {C.cancel, 0, 0};
	u32 unit = 0;
	bool active = false, capturing = false;
	if (R_SUCCEEDED(r) && R_SUCCEEDED(r = CAMU_SetSize(sel, SIZE_CTR_TOP_LCD, CONTEXT_A)) &&
	    R_SUCCEEDED(r = CAMU_SetOutputFormat(sel, OUTPUT_RGB_565, CONTEXT_A)) && R_SUCCEEDED(r = CAMU_SetFrameRate(sel, FRAME_RATE_30)) &&
	    R_SUCCEEDED(r = CAMU_SetNoiseFilter(sel, true)) && R_SUCCEEDED(r = CAMU_SetAutoExposure(sel, true)) &&
	    R_SUCCEEDED(r = CAMU_SetAutoWhiteBalance(sel, true)) && R_SUCCEEDED(r = CAMU_Activate(sel))) {
		active = true;
		if (R_SUCCEEDED(r = CAMU_GetBufferErrorInterruptEvent(&ev[2], PORT_CAM1)) && R_SUCCEEDED(r = CAMU_SetTrimming(PORT_CAM1, true)) &&
		    R_SUCCEEDED(r = CAMU_SetTrimmingParamsCenter(PORT_CAM1, QRCAM_W, QRCAM_H, 400, 240)) &&
		    R_SUCCEEDED(r = CAMU_GetMaxBytes(&unit, QRCAM_W, QRCAM_H)) && R_SUCCEEDED(r = CAMU_SetTransferBytes(PORT_CAM1, unit, QRCAM_W, QRCAM_H)) &&
		    R_SUCCEEDED(r = CAMU_ClearBuffer(PORT_CAM1)) &&
		    R_SUCCEEDED(r = CAMU_SetReceiving(&ev[1], C.cap, PORT_CAM1, FRAME_BYTES, (s16)unit)) && R_SUCCEEDED(r = CAMU_StartCapture(PORT_CAM1))) {
			capturing = true;
			C.st = QRC_RUN;
			for (;;) {
				s32 idx = -1;
				r = svcWaitSynchronizationN(&idx, ev, 3, false, U64_MAX);
				if (R_FAILED(r) || idx == 0)
					break;
				if (idx == 1) { // imagen nueva: se guarda, se vuelve a pedir y se busca el QR
					svcCloseHandle(ev[1]);
					ev[1] = 0;
					LightLock_Lock(&C.lock);
					memcpy(C.front, C.cap, FRAME_BYTES);
					C.frame++;
					bool look = !C.have;
					LightLock_Unlock(&C.lock);
					if (R_FAILED(r = CAMU_SetReceiving(&ev[1], C.cap, PORT_CAM1, FRAME_BYTES, (s16)unit)))
						break;
					if (look) { // buscar el QR cuesta CPU: por detrás de la interfaz, para que no se trabe mientras se escanea
						svcSetThreadPriority(CUR_THREAD_HANDLE, C.prio_scan);
						scan(C.front);
						svcSetThreadPriority(CUR_THREAD_HANDLE, C.prio_cap);
					}
				} else if (idx == 2) { // se perdió una imagen: se empieza otra vez
					if (ev[1]) {
						svcCloseHandle(ev[1]);
						ev[1] = 0;
					}
					if (R_FAILED(r = CAMU_ClearBuffer(PORT_CAM1)) ||
					    R_FAILED(r = CAMU_SetReceiving(&ev[1], C.cap, PORT_CAM1, FRAME_BYTES, (s16)unit)) ||
					    R_FAILED(r = CAMU_StartCapture(PORT_CAM1)))
						break;
				}
			}
		}
	}
	if (capturing) {
		CAMU_StopCapture(PORT_CAM1);
		bool busy = true;
		for (int i = 0; i < 100 && R_SUCCEEDED(CAMU_IsBusy(&busy, PORT_CAM1)) && busy; i++)
			svcSleepThread(1000000);
		CAMU_ClearBuffer(PORT_CAM1);
	}
	for (int i = 1; i < 3; i++)
		if (ev[i])
			svcCloseHandle(ev[i]);
	if (active)
		CAMU_Activate(SELECT_NONE);
	camExit();
	C.err = r;
	C.st = R_FAILED(r) ? QRC_FAIL : QRC_OFF;
	if (R_FAILED(r))
		util_log("cámara: error %08lX", (unsigned long)r);
}

bool qrcam_start(bool inner)
{
	qrcam_stop();
	if (!C.init) {
		LightLock_Init(&C.lock);
		C.init = true;
	}
	if (!C.cap)
		C.cap = linearAlloc(FRAME_BYTES); // la cámara escribe por DMA: memoria lineal
	if (!C.front)
		C.front = malloc(FRAME_BYTES);
	if (!C.q && (C.q = quirc_new()) && quirc_resize(C.q, QRCAM_W, QRCAM_H) < 0) {
		quirc_destroy(C.q);
		C.q = NULL;
	}
	if (!C.cap || !C.front || !C.q || R_FAILED(svcCreateEvent(&C.cancel, RESET_STICKY))) {
		C.st = QRC_FAIL;
		C.err = -1;
		return false;
	}
	memset(C.front, 0, FRAME_BYTES);
	C.inner = inner;
	C.have = false;
	C.frame = 0;
	C.err = 0;
	C.st = QRC_STARTING;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	C.prio_cap = prio - 1, C.prio_scan = prio + 1;
	C.thr = threadCreate(cam_thread, NULL, 32 * 1024, prio - 1, -2, false);
	if (!C.thr) {
		svcCloseHandle(C.cancel);
		C.cancel = 0;
		C.st = QRC_FAIL;
		return false;
	}
	return true;
}

void qrcam_stop(void)
{
	if (C.thr) {
		svcSignalEvent(C.cancel);
		threadJoin(C.thr, U64_MAX);
		threadFree(C.thr);
		C.thr = NULL;
	}
	if (C.cancel) {
		svcCloseHandle(C.cancel);
		C.cancel = 0;
	}
	if (C.st != QRC_FAIL)
		C.st = QRC_OFF;
	if (C.cap) {
		linearFree(C.cap);
		C.cap = NULL;
	}
	free(C.front);
	C.front = NULL;
	if (C.q) {
		quirc_destroy(C.q);
		C.q = NULL;
	}
}

int qrcam_state(Result *err)
{
	if (err)
		*err = C.err;
	return C.st;
}

bool qrcam_frame(u16 *out, u32 *seen)
{
	if (!C.front || !C.init)
		return false;
	LightLock_Lock(&C.lock);
	bool fresh = C.frame != *seen;
	if (fresh) {
		memcpy(out, C.front, FRAME_BYTES);
		*seen = C.frame;
	}
	LightLock_Unlock(&C.lock);
	return fresh;
}

bool qrcam_take(char *out, size_t n)
{
	if (!C.init)
		return false;
	LightLock_Lock(&C.lock);
	bool h = C.have;
	if (h) {
		snprintf(out, n, "%s", C.text);
		C.have = false;
	}
	LightLock_Unlock(&C.lock);
	return h;
}
