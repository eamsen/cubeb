/*
 * Copyright �� 2011 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */
#undef NDEBUG
#include <assert.h>
#include <windows.h>
#include <mmreg.h>
#include <mmsystem.h>
#include <process.h>
#include <stdlib.h>
#include "cubeb/cubeb.h"

#include <stdio.h>

#define NBUFS 4

struct cubeb_stream_item {
  SLIST_ENTRY head;
  cubeb_stream * stream;
};

struct cubeb {
  HANDLE event;
  HANDLE thread;
  int shutdown;
  PSLIST_HEADER work;
};

struct cubeb_stream {
  cubeb * context;
  cubeb_stream_params params;
  cubeb_data_callback data_callback;
  cubeb_state_callback state_callback;
  void * user_ptr;
  WAVEHDR buffers[NBUFS];
  int next_buffer;
  int free_buffers;
  int shutdown;
  int draining;
  HANDLE event;
  HWAVEOUT waveout;
  CRITICAL_SECTION lock;
};

static size_t
bytes_per_frame(cubeb_stream_params params)
{
  size_t bytes;

  switch (params.format) {
  case CUBEB_SAMPLE_S16LE:
    bytes = sizeof(signed int);
    break;
  case CUBEB_SAMPLE_FLOAT32LE:
    bytes = sizeof(float);
    break;
  default:
    assert(0);
  }

  return bytes * params.channels;
}

static WAVEHDR *
cubeb_get_next_buffer(cubeb_stream * stm)
{
  WAVEHDR * hdr = NULL;

  assert(stm->free_buffers > 0 && stm->free_buffers <= NBUFS);
  hdr = &stm->buffers[stm->next_buffer];
  assert(hdr->dwFlags == 0 ||
         (hdr->dwFlags & WHDR_DONE && !(hdr->dwFlags & WHDR_INQUEUE)));
  stm->next_buffer = (stm->next_buffer + 1) % NBUFS;
  stm->free_buffers -= 1;

  return hdr;
}

static void
cubeb_submit_buffer(cubeb_stream * stm, WAVEHDR * hdr)
{
  long got;
  MMRESULT r;

  got = stm->data_callback(stm, stm->user_ptr, hdr->lpData, hdr->dwBufferLength / bytes_per_frame(stm->params));
  if (got < 0) {
    // XXX handle this case
    assert(0);
    return;
  } else if ((DWORD) got < hdr->dwBufferLength / bytes_per_frame(stm->params)) {
    r = waveOutUnprepareHeader(stm->waveout, hdr, sizeof(*hdr));
    assert(r == MMSYSERR_NOERROR);

    hdr->dwBufferLength = got * bytes_per_frame(stm->params);

    r = waveOutPrepareHeader(stm->waveout, hdr, sizeof(*hdr));
    assert(r == MMSYSERR_NOERROR);

    stm->draining = 1;
  }

  assert(hdr->dwFlags & WHDR_PREPARED);

  r = waveOutWrite(stm->waveout, hdr, sizeof(*hdr));
  assert(r == MMSYSERR_NOERROR);
}

static unsigned __stdcall
cubeb_buffer_thread(void * user_ptr)
{
  cubeb * ctx = (cubeb *) user_ptr;
  assert(ctx);

  for (;;) {
    DWORD rv;
    struct cubeb_stream_item * item;

    rv = WaitForSingleObject(ctx->event, INFINITE);
    assert(rv == WAIT_OBJECT_0);

    item = (struct cubeb_stream_item *) InterlockedPopEntrySList(ctx->work);
    while (item) {
      cubeb_stream * stm = item->stream;

      EnterCriticalSection(&stm->lock);
      stm->free_buffers += 1;
      assert(stm->free_buffers > 0 && stm->free_buffers <= NBUFS);

      if (stm->draining || stm->shutdown) {
        if (stm->free_buffers == NBUFS) {
          if (stm->draining) {
            stm->state_callback(stm, stm->user_ptr, CUBEB_STATE_DRAINED);
          }
          SetEvent(stm->event);
        }
      } else {
        cubeb_submit_buffer(stm, cubeb_get_next_buffer(stm));
      }
      LeaveCriticalSection(&stm->lock);

      _aligned_free(item);

      item = (struct cubeb_stream_item *) InterlockedPopEntrySList(ctx->work);
    }

    assert(!InterlockedPopEntrySList(ctx->work));

    if (ctx->shutdown) {
      break;
    }
  }

  return 0;
}

static void CALLBACK
cubeb_buffer_callback(HWAVEOUT waveout, UINT msg, DWORD_PTR user_ptr, DWORD_PTR param1, DWORD_PTR param2)
{
  cubeb_stream * stm = (cubeb_stream *) user_ptr;
  struct cubeb_stream_item * item;

  if (msg != WOM_DONE) {
    return;
  }

  item = _aligned_malloc(sizeof(struct cubeb_stream_item), MEMORY_ALLOCATION_ALIGNMENT);
  assert(item);
  item->stream = stm;
  InterlockedPushEntrySList(stm->context->work, &item->head);

  SetEvent(stm->context->event);
}

int
cubeb_init(cubeb ** context, char const * context_name)
{
  cubeb * ctx;

  ctx = calloc(1, sizeof(*ctx));
  assert(ctx);

  ctx->work = _aligned_malloc(sizeof(*ctx->work), MEMORY_ALLOCATION_ALIGNMENT);
  assert(ctx->work);
  InitializeSListHead(ctx->work);

  ctx->event = CreateEvent(NULL, FALSE, FALSE, NULL);
  if (!ctx->event) {
    cubeb_destroy(ctx);
    return CUBEB_ERROR;
  }

  ctx->thread = (HANDLE) _beginthreadex(NULL, 64 * 1024, cubeb_buffer_thread, ctx, 0, NULL);
  if (!ctx->thread) {
    cubeb_destroy(ctx);
    return CUBEB_ERROR;
  }

  SetThreadPriority(ctx->thread, THREAD_PRIORITY_TIME_CRITICAL);

  *context = ctx;

  return CUBEB_OK;
}

void
cubeb_destroy(cubeb * ctx)
{
  DWORD rv;

  assert(!InterlockedPopEntrySList(ctx->work));

  if (ctx->thread) {
    ctx->shutdown = 1;
    SetEvent(ctx->event);
    rv = WaitForSingleObject(ctx->thread, INFINITE);
    assert(rv == WAIT_OBJECT_0);
    CloseHandle(ctx->thread);
  }

  if (ctx->event) {
    CloseHandle(ctx->event);
  }

  _aligned_free(ctx->work);

  free(ctx);
}

int
cubeb_stream_init(cubeb * context, cubeb_stream ** stream, char const * stream_name,
                  cubeb_stream_params stream_params, unsigned int latency,
                  cubeb_data_callback data_callback,
                  cubeb_state_callback state_callback,
                  void * user_ptr)
{
  MMRESULT r;
  WAVEFORMATEXTENSIBLE wfx;
  cubeb_stream * stm;
  int i;
  size_t bufsz;

  memset(&wfx, 0, sizeof(wfx));
  if (stream_params.channels > 2) {
    wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.cbSize = sizeof(wfx) - sizeof(wfx.Format);
  } else {
    wfx.Format.wFormatTag = WAVE_FORMAT_PCM;
    if (stream_params.format == CUBEB_SAMPLE_FLOAT32LE) {
      wfx.Format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    }
    wfx.Format.cbSize = 0;
  }
  wfx.Format.nChannels = stream_params.channels;
  wfx.Format.nSamplesPerSec = stream_params.rate;

  /* XXX fix channel mappings */
  wfx.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;

  switch (stream_params.format) {
  case CUBEB_SAMPLE_S16LE:
    wfx.Format.wBitsPerSample = 16;
    wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    break;
  case CUBEB_SAMPLE_FLOAT32LE:
    wfx.Format.wBitsPerSample = 32;
    wfx.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    break;
  default:
    return CUBEB_ERROR_INVALID_FORMAT;
  }

  wfx.Format.nBlockAlign = (wfx.Format.wBitsPerSample * wfx.Format.nChannels) / 8;
  wfx.Format.nAvgBytesPerSec = wfx.Format.nSamplesPerSec * wfx.Format.nBlockAlign;
  wfx.Samples.wValidBitsPerSample = 0;
  wfx.Samples.wSamplesPerBlock = 0;
  wfx.Samples.wReserved = 0;

  stm = calloc(1, sizeof(*stm));
  assert(stm);

  stm->context = context;

  stm->params = stream_params;

  stm->data_callback = data_callback;
  stm->state_callback = state_callback;
  stm->user_ptr = user_ptr;

  bufsz = stm->params.rate / 1000.0 * latency * bytes_per_frame(stm->params) / NBUFS;
  if (bufsz % bytes_per_frame(stm->params) != 0) {
    bufsz += bytes_per_frame(stm->params) - (bufsz % bytes_per_frame(stm->params));
  }
  assert(bufsz % bytes_per_frame(stm->params) == 0);

  for (i = 0; i < NBUFS; ++i) {
    stm->buffers[i].lpData = calloc(1, bufsz);
    assert(stm->buffers[i].lpData);
    stm->buffers[i].dwBufferLength = bufsz;
    stm->buffers[i].dwFlags = 0;
  }

  InitializeCriticalSection(&stm->lock);

  stm->event = CreateEvent(NULL, FALSE, FALSE, NULL);
  if (!stm->event) {
    cubeb_stream_destroy(stm);
    return CUBEB_ERROR;
  }

  stm->free_buffers = NBUFS;

  /* cubeb_buffer_callback will be called during waveOutOpen, so all
     other initialization must be complete before calling it. */
  r = waveOutOpen(&stm->waveout, WAVE_MAPPER, &wfx.Format,
                  (DWORD_PTR) cubeb_buffer_callback, (DWORD_PTR) stm,
                  CALLBACK_FUNCTION);
  if (r != MMSYSERR_NOERROR) {
    cubeb_stream_destroy(stm);
    return CUBEB_ERROR;
  }
  assert(r == MMSYSERR_NOERROR);

  r = waveOutPause(stm->waveout);
  assert(r == MMSYSERR_NOERROR);

  for (i = 0; i < NBUFS; ++i) {
    WAVEHDR * hdr = cubeb_get_next_buffer(stm);

    r = waveOutPrepareHeader(stm->waveout, hdr, sizeof(*hdr));
    assert(r == MMSYSERR_NOERROR);

    cubeb_submit_buffer(stm, hdr);
  }

  *stream = stm;

  return CUBEB_OK;
}

void
cubeb_stream_destroy(cubeb_stream * stm)
{
  MMRESULT r;
  DWORD rv;
  int i;
  int enqueued;

  if (stm->waveout) {
    EnterCriticalSection(&stm->lock);
    stm->shutdown = 1;

    r = waveOutReset(stm->waveout);
    assert(r == MMSYSERR_NOERROR);

    enqueued = NBUFS - stm->free_buffers;
    LeaveCriticalSection(&stm->lock);

    /* wait for all blocks to complete */
    if (enqueued > 0) {
      rv = WaitForSingleObject(stm->event, INFINITE);
      assert(rv == WAIT_OBJECT_0);
    }

    for (i = 0; i < NBUFS; ++i) {
      r = waveOutUnprepareHeader(stm->waveout, &stm->buffers[i], sizeof(stm->buffers[i]));
      assert(r == MMSYSERR_NOERROR);
    }

    r = waveOutClose(stm->waveout);
    assert(r == MMSYSERR_NOERROR);
  }

  if (stm->event) {
    CloseHandle(stm->event);
  }

  DeleteCriticalSection(&stm->lock);

  for (i = 0; i < NBUFS; ++i) {
    free(stm->buffers[i].lpData);
  }

  free(stm);
}

int
cubeb_stream_start(cubeb_stream * stm)
{
  MMRESULT r;

  r = waveOutRestart(stm->waveout);
  assert(r == MMSYSERR_NOERROR);

  stm->state_callback(stm, stm->user_ptr, CUBEB_STATE_STARTED);

  return CUBEB_OK;
}

int
cubeb_stream_stop(cubeb_stream * stm)
{
  MMRESULT r;

  r = waveOutPause(stm->waveout);
  assert(r == MMSYSERR_NOERROR);

  stm->state_callback(stm, stm->user_ptr, CUBEB_STATE_STOPPED);

  return CUBEB_OK;
}

int
cubeb_stream_get_position(cubeb_stream * stm, uint64_t * position)
{
  MMRESULT r;
  MMTIME time;

  time.wType = TIME_SAMPLES;
  r = waveOutGetPosition(stm->waveout, &time, sizeof(time));
  assert(r == MMSYSERR_NOERROR);
  assert(time.wType == TIME_SAMPLES);

  *position = time.u.sample;

  return CUBEB_OK;
}
