/*
 * Copyright (C) 2006 Robert Reif
 * Portions copyright (C) 2007 Ralf Beck
 * Portions copyright (C) 2007 Johnny Petrantoni
 * Portions copyright (C) 2007 Stephane Letz
 * Portions copyright (C) 2008 William Steidtmann
 * Portions copyright (C) 2010 Peter L Jones
 * Portions copyright (C) 2010 Torben Hohn
 * Portions copyright (C) 2010 Nedko Arnaudov
 * Portions copyright (C) 2013 Joakim Hernberg
 * Portions copyright (C) 2020-2024 Filipe Coelho
 * Portions copyright (C) 2024-2025 Dawid Kraiński
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>
#include <stdatomic.h>

#include <jack/jack.h>
#include <jack/thread.h>

#include <spa/param/audio/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/pod/builder.h>
#include <spa/utils/dict.h>
#include <pipewire/core.h>
#include <pipewire/context.h>
#include <pipewire/keys.h>
#include <pipewire/buffers.h>
#include <pipewire/filter.h>
#include <pipewire/link.h>

#include "new_gui/gui_stub.inc.c"
#include "pw_helper_c.h"
#include "pw_helper_common.h"
#include "driver_clsid.h"

#ifdef DEBUG
#include <wine/debug.h>
#else
#define TRACE(...) {}
#define WARN(fmt, ...) {} fprintf(stdout, fmt, ##__VA_ARGS__)
#define ERR(fmt, ...) {} fprintf(stderr, fmt, ##__VA_ARGS__)
#endif

#include <objbase.h>
#include <mmsystem.h>
#include <winreg.h>
#ifdef WINE_WITH_UNICODE
#include <wine/unicode.h>
#endif

#define IEEE754_64FLOAT 1
#undef NATIVE_INT64
#include <asio.h>

#ifdef DEBUG
WINE_DEFAULT_DEBUG_CHANNEL(asio);
#endif

#define MAX_ENVIRONMENT_SIZE        6
#define ASIO_MAX_NAME_LENGTH        32
#define ASIO_MINIMUM_BUFFERSIZE     16
#define ASIO_MAXIMUM_BUFFERSIZE     8192
#define ASIO_PREFERRED_BUFFERSIZE   1024

#define ASIO_LONG(typ, x) ({ uint64_t __long_val = (x); (typ) { .lo = (uint32_t)__long_val, .hi = (uint32_t)(__long_val >> 32) }; })

/* ASIO drivers (breaking the COM specification) use the Microsoft variety of
 * thiscall calling convention which gcc is unable to produce.  These macros
 * add an extra layer to fixup the registers. Borrowed from config.h and the
 * wine source code.
 */

/* From config.h */
#define __ASM_DEFINE_FUNC(name,suffix,code) asm(".text\n\t.align 4\n\t.globl " #name suffix "\n\t.type " #name suffix ",@function\n" #name suffix ":\n\t.cfi_startproc\n\t" code "\n\t.cfi_endproc\n\t.previous");
#define __ASM_GLOBAL_FUNC(name,code) __ASM_DEFINE_FUNC(name,"",code)
#define __ASM_NAME(name) name
#define __ASM_STDCALL(args) ""

/* From wine source */
#ifdef __i386__  /* thiscall functions are i386-specific */

#define THISCALL(func) __thiscall_ ## func
#define THISCALL_NAME(func) __ASM_NAME("__thiscall_" #func)
#define __thiscall __stdcall
#define DEFINE_THISCALL_WRAPPER(func,args) \
    extern void THISCALL(func)(void); \
    __ASM_GLOBAL_FUNC(__thiscall_ ## func, \
                      "popl %eax\n\t" \
                      "pushl %ecx\n\t" \
                      "pushl %eax\n\t" \
                      "jmp " __ASM_NAME(#func) __ASM_STDCALL(args) )
#else /* __i386__ */

#define THISCALL(func) func
#define THISCALL_NAME(func) __ASM_NAME(#func)
#define __thiscall __stdcall
#define DEFINE_THISCALL_WRAPPER(func,args) /* nothing */

#endif /* __i386__ */

/* Hide ELF symbols for the COM members - No need to to export them */
#define HIDDEN __attribute__ ((visibility("hidden")))

/*****************************************************************************
 * IWineAsio interface
 */

#define INTERFACE IWineASIO
DECLARE_INTERFACE_(IWineASIO,IUnknown)
{
    STDMETHOD_(HRESULT, QueryInterface)         (THIS_ IID riid, void** ppvObject) PURE;
    STDMETHOD_(ULONG, AddRef)                   (THIS) PURE;
    STDMETHOD_(ULONG, Release)                  (THIS) PURE;
    STDMETHOD_(ASIOBool, Init)                  (THIS_ void *sysRef) PURE;
    STDMETHOD_(void, GetDriverName)             (THIS_ char *name) PURE;
    STDMETHOD_(LONG, GetDriverVersion)          (THIS) PURE;
    STDMETHOD_(void, GetErrorMessage)           (THIS_ char *string) PURE;
    STDMETHOD_(ASIOError, Start)                (THIS) PURE;
    STDMETHOD_(ASIOError, Stop)                 (THIS) PURE;
    STDMETHOD_(ASIOError, GetChannels)          (THIS_ LONG *numInputChannels, LONG *numOutputChannels) PURE;
    STDMETHOD_(ASIOError, GetLatencies)         (THIS_ LONG *inputLatency, LONG *outputLatency) PURE;
    STDMETHOD_(ASIOError, GetBufferSize)        (THIS_ LONG *minSize, LONG *maxSize, LONG *preferredSize, LONG *granularity) PURE;
    STDMETHOD_(ASIOError, CanSampleRate)        (THIS_ ASIOSampleRate sampleRate) PURE;
    STDMETHOD_(ASIOError, GetSampleRate)        (THIS_ ASIOSampleRate *sampleRate) PURE;
    STDMETHOD_(ASIOError, SetSampleRate)        (THIS_ ASIOSampleRate sampleRate) PURE;
    STDMETHOD_(ASIOError, GetClockSources)      (THIS_ ASIOClockSource *clocks, LONG *numSources) PURE;
    STDMETHOD_(ASIOError, SetClockSource)       (THIS_ LONG index) PURE;
    STDMETHOD_(ASIOError, GetSamplePosition)    (THIS_ ASIOSamples *sPos, ASIOTimeStamp *tStamp) PURE;
    STDMETHOD_(ASIOError, GetChannelInfo)       (THIS_ ASIOChannelInfo *info) PURE;
    STDMETHOD_(ASIOError, CreateBuffers)        (THIS_ ASIOBufferInfo *bufferInfo, LONG numChannels, LONG bufferSize, ASIOCallbacks *asioCallbacks) PURE;
    STDMETHOD_(ASIOError, DisposeBuffers)       (THIS) PURE;
    STDMETHOD_(ASIOError, ControlPanel)         (THIS) PURE;
    STDMETHOD_(ASIOError, Future)               (THIS_ LONG selector,void *opt) PURE;
    STDMETHOD_(ASIOError, OutputReady)          (THIS) PURE;
};
#undef INTERFACE

typedef struct IWineASIO *LPWINEASIO;

struct io_port {
    bool              active;
    char              port_name[ASIO_MAX_NAME_LENGTH];
    void             *port;
    struct pw_buffer *buffers[2];
    struct pw_link   *link;
};

#define DEVICE_NAME_SIZE 1024

typedef struct IWineASIOImpl
{
    /* COM stuff */
    const IWineASIOVtbl        *lpVtbl;
    LONG                        ref;

    /* Reference to the DLL class factory (to keep DLL alive while an object is live) */
    IUnknown                   *cls_factory;

    /* The app's main window handle on windows, 0 on OS/X */
    HWND                        sys_ref;

    /* ASIO stuff */
    LONG                        asio_active_inputs;
    LONG                        asio_active_outputs;
    bool                        asio_buffer_index;
    ASIOCallbacks              *asio_callbacks;
    LONG                        asio_current_buffersize;
    INT                         asio_driver_state;
    uint64_t                    asio_sample_position;
    double                      asio_sample_rate;
    ASIOTime                    asio_time;
    uint64_t                    asio_time_stamp;
    LONG                        asio_version;
    bool                        asio_can_time_code;
    bool                        asio_time_info_mode;

    /* WineASIO configuration options */
    bool                        wineasio_fixed_buffersize;
    int                         wineasio_number_inputs;
    int                         wineasio_number_outputs;
    LONG                        wineasio_preferred_buffersize;
    WCHAR                       pwasio_input_device_name[DEVICE_NAME_SIZE];
    WCHAR                       pwasio_output_device_name[DEVICE_NAME_SIZE];

    /* PipeWire stuff */
    struct user_pw_helper      *pw_helper;
    struct pw_loop             *pw_loop;
    struct pw_context          *pw_context;
    struct pw_core             *pw_core;

    struct pw_node             *current_input_node;
    struct pw_node             *current_output_node;

    struct pw_filter           *pw_filter;
    struct spa_hook             pw_filter_listener;

    struct pwasio_gui          *gui;
    struct pwasio_gui_conf      gui_conf;

    char                        client_name[ASIO_MAX_NAME_LENGTH];

    /* jack process callback buffers */
    //jack_default_audio_sample_t *callback_audio_buffer;
    struct io_port             *input_channel;
    struct io_port             *output_channel;

    uint32_t                    asio_buffers_left_to_init;
    pthread_barrier_t           pw_filter_bound;
    pthread_barrier_t           asio_buffers_filled;
} IWineASIOImpl;

enum { Loaded, Initialized, Prepared, Running };

/****************************************************************************
 *  Interface Methods
 */

/*
 *  as seen from the WineASIO source
 */

HIDDEN HRESULT   STDMETHODCALLTYPE      QueryInterface(LPWINEASIO iface, REFIID riid, void **ppvObject);
HIDDEN ULONG     STDMETHODCALLTYPE      AddRef(LPWINEASIO iface);
HIDDEN ULONG     STDMETHODCALLTYPE      Release(LPWINEASIO iface);
HIDDEN ASIOBool  STDMETHODCALLTYPE      Init(LPWINEASIO iface, void *sysRef);
HIDDEN void      STDMETHODCALLTYPE      GetDriverName(LPWINEASIO iface, char *name);
HIDDEN LONG      STDMETHODCALLTYPE      GetDriverVersion(LPWINEASIO iface);
HIDDEN void      STDMETHODCALLTYPE      GetErrorMessage(LPWINEASIO iface, char *string);
HIDDEN ASIOError STDMETHODCALLTYPE      Start(LPWINEASIO iface);
HIDDEN ASIOError STDMETHODCALLTYPE      Stop(LPWINEASIO iface);
HIDDEN ASIOError STDMETHODCALLTYPE      GetChannels (LPWINEASIO iface, LONG *numInputChannels, LONG *numOutputChannels);
HIDDEN ASIOError STDMETHODCALLTYPE      GetLatencies(LPWINEASIO iface, LONG *inputLatency, LONG *outputLatency);
HIDDEN ASIOError STDMETHODCALLTYPE      GetBufferSize(LPWINEASIO iface, LONG *minSize, LONG *maxSize, LONG *preferredSize, LONG *granularity);
HIDDEN ASIOError STDMETHODCALLTYPE      CanSampleRate(LPWINEASIO iface, ASIOSampleRate sampleRate);
HIDDEN ASIOError STDMETHODCALLTYPE      GetSampleRate(LPWINEASIO iface, ASIOSampleRate *sampleRate);
HIDDEN ASIOError STDMETHODCALLTYPE      SetSampleRate(LPWINEASIO iface, ASIOSampleRate sampleRate);
HIDDEN ASIOError STDMETHODCALLTYPE      GetClockSources(LPWINEASIO iface, ASIOClockSource *clocks, LONG *numSources);
HIDDEN ASIOError STDMETHODCALLTYPE      SetClockSource(LPWINEASIO iface, LONG index);
HIDDEN ASIOError STDMETHODCALLTYPE      GetSamplePosition(LPWINEASIO iface, ASIOSamples *sPos, ASIOTimeStamp *tStamp);
HIDDEN ASIOError STDMETHODCALLTYPE      GetChannelInfo(LPWINEASIO iface, ASIOChannelInfo *info);
HIDDEN ASIOError STDMETHODCALLTYPE      CreateBuffers(LPWINEASIO iface, ASIOBufferInfo *bufferInfo, LONG numChannels, LONG bufferSize, ASIOCallbacks *asioCallbacks);
HIDDEN ASIOError STDMETHODCALLTYPE      DisposeBuffers(LPWINEASIO iface);
HIDDEN ASIOError STDMETHODCALLTYPE      ControlPanel(LPWINEASIO iface);
HIDDEN ASIOError STDMETHODCALLTYPE      Future(LPWINEASIO iface, LONG selector, void *opt);
HIDDEN ASIOError STDMETHODCALLTYPE      OutputReady(LPWINEASIO iface);

/*
 * thiscall wrappers for the vtbl (as seen from app side 32bit)
 */

HIDDEN void __thiscall_Init(void);
HIDDEN void __thiscall_GetDriverName(void);
HIDDEN void __thiscall_GetDriverVersion(void);
HIDDEN void __thiscall_GetErrorMessage(void);
HIDDEN void __thiscall_Start(void);
HIDDEN void __thiscall_Stop(void);
HIDDEN void __thiscall_GetChannels(void);
HIDDEN void __thiscall_GetLatencies(void);
HIDDEN void __thiscall_GetBufferSize(void);
HIDDEN void __thiscall_CanSampleRate(void);
HIDDEN void __thiscall_GetSampleRate(void);
HIDDEN void __thiscall_SetSampleRate(void);
HIDDEN void __thiscall_GetClockSources(void);
HIDDEN void __thiscall_SetClockSource(void);
HIDDEN void __thiscall_GetSamplePosition(void);
HIDDEN void __thiscall_GetChannelInfo(void);
HIDDEN void __thiscall_CreateBuffers(void);
HIDDEN void __thiscall_DisposeBuffers(void);
HIDDEN void __thiscall_ControlPanel(void);
HIDDEN void __thiscall_Future(void);
HIDDEN void __thiscall_OutputReady(void);

/*
 *  Jack callbacks
 */

static inline int  jack_buffer_size_callback (jack_nframes_t nframes, void *arg);
static inline void jack_latency_callback(jack_latency_callback_mode_t mode, void *arg);
static inline int  jack_process_callback (jack_nframes_t nframes, void *arg);
static inline int  jack_sample_rate_callback (jack_nframes_t nframes, void *arg);

/*
 *  Support functions
 */

HRESULT WINAPI  WineASIOCreateInstance(REFIID riid, LPVOID *ppobj, IUnknown *cls_factory);
static  void    store_config(IWineASIOImpl *This);
static  VOID    configure_driver(IWineASIOImpl *This);
static  void    get_nodes_by_name(IWineASIOImpl *This);
static  void    connect_io_port(IWineASIOImpl *This, struct io_port *port, uint32_t idx, enum spa_direction dir);
static  void    dispose_io_port(IWineASIOImpl *This, struct io_port *port, enum spa_direction dir);

HIDDEN void GuiClosed(struct pwasio_gui_conf *conf);
HIDDEN void GuiApplyConfig(struct pwasio_gui_conf *conf);
HIDDEN void GuiLoadConfig(struct pwasio_gui_conf *conf);

static DWORD WINAPI jack_thread_creator_helper(LPVOID arg);
static int          jack_thread_creator(pthread_t* thread_id, const pthread_attr_t* attr, void *(*function)(void*), void* arg);

static const IWineASIOVtbl WineASIO_Vtbl =
{
    (void *) QueryInterface,
    (void *) AddRef,
    (void *) Release,

    (void *) THISCALL(Init),
    (void *) THISCALL(GetDriverName),
    (void *) THISCALL(GetDriverVersion),
    (void *) THISCALL(GetErrorMessage),
    (void *) THISCALL(Start),
    (void *) THISCALL(Stop),
    (void *) THISCALL(GetChannels),
    (void *) THISCALL(GetLatencies),
    (void *) THISCALL(GetBufferSize),
    (void *) THISCALL(CanSampleRate),
    (void *) THISCALL(GetSampleRate),
    (void *) THISCALL(SetSampleRate),
    (void *) THISCALL(GetClockSources),
    (void *) THISCALL(SetClockSource),
    (void *) THISCALL(GetSamplePosition),
    (void *) THISCALL(GetChannelInfo),
    (void *) THISCALL(CreateBuffers),
    (void *) THISCALL(DisposeBuffers),
    (void *) THISCALL(ControlPanel),
    (void *) THISCALL(Future),
    (void *) THISCALL(OutputReady)
};

/* structure needed to create the JACK callback thread in the wine process context */
struct {
    void        *(*jack_callback_thread) (void*);
    void        *arg;
    pthread_t   jack_callback_pthread_id;
    HANDLE      jack_callback_thread_created;
} jack_thread_creator_privates;

static void pipewire_state_changed_callback(void *data, enum pw_filter_state from, enum pw_filter_state to, char const *error) {
    IWineASIOImpl *This = (IWineASIOImpl*)data;

    printf("state_chaanged: iface:%p state changed from %s to %s", This, pw_filter_state_as_string(from), pw_filter_state_as_string(to));
    if (error) {
        printf(": ERROR %s\n", error);
    } else {
        putchar('\n');
    }

    if (from == PW_FILTER_STATE_CONNECTING && to == PW_FILTER_STATE_PAUSED) {
        pthread_barrier_wait(&This->pw_filter_bound);
    }
}

static void pipewire_io_changed_callback(void *data, void *port, uint32_t id, void *area, uint32_t size) {
    IWineASIOImpl *This = (IWineASIOImpl*)data;

    printf("io_changed: iface:%p IO changed on port %p: 0x%04x\n", This, port, id);
}

static void pipewire_param_changed_callback(void *data, void *port, uint32_t id, struct spa_pod const *param) {
    IWineASIOImpl *This = (IWineASIOImpl*)data;

    printf("param_changed: iface:%p param 0x%04x changed on port %p\n", This, id, port);
}

static void pipewire_add_buffer_callback(void *data, void *port, struct pw_buffer *buffer) {
    IWineASIOImpl *This = (IWineASIOImpl*)data;

    printf("add_buffer: iface:%p port:%p, buffer:%p\n", This, port, buffer);

    for (int idx = 0; idx < This->wineasio_number_inputs + This->wineasio_number_outputs; ++idx) {
        struct io_port *chan = &This->input_channel[idx];
        if (chan->port == port) {
            if (chan->buffers[1]) {
                if (chan->buffers[0]) {
                    printf("Buffers for channel %s already full!\n", chan->port_name);
                    return;
                } else {
                    printf("Adding second buffer for channel %s\n", chan->port_name);
                    chan->buffers[0] = buffer;

                    buffer = pw_filter_dequeue_buffer(chan->port);
                    buffer->buffer->datas[0].chunk->offset = 0;
                    buffer->buffer->datas[0].chunk->stride = sizeof(float);
                    buffer->buffer->datas[0].chunk->size = 0;
                    printf("Dequeued buffer: %p\n", buffer);
                }
            } else {
                printf("Adding first buffer for channel %s\n", chan->port_name);
                chan->buffers[1] = buffer;
            }

            This->asio_buffers_left_to_init -= 1;
            if (This->asio_buffers_left_to_init == 0) {
                // Signal the creator thread
                pthread_barrier_wait(&This->asio_buffers_filled);
            }
            break;
        }
    }
}

static void pipewire_remove_buffer_callback(void *data, void *port, struct pw_buffer *buffer) {
    IWineASIOImpl *This = (IWineASIOImpl*)data;

    printf("remove_buffer: iface:%p port:%p, buffer:%p\n", This, port, buffer);
}

static void pipewire_process_callback(void *data, struct spa_io_position *position) {
    IWineASIOImpl *This = (IWineASIOImpl*)data;
    int            idx;
    size_t         sample_count = position->clock.duration;

    //printf("process: iface:%p\n", This);

    /* output silence if the ASIO callback isn't running yet */
    if (This->asio_driver_state != Running)
    {
        for (idx = 0; idx < This->asio_active_outputs; ++idx) {
            void *buffer = pw_filter_get_dsp_buffer(This->output_channel[idx].port, sample_count);
            if (buffer)
                bzero(buffer, sizeof (jack_default_audio_sample_t) * sample_count);
        }
        return;
    }

    struct pw_buffer *buffer;
    struct io_port *chan;
    for (idx = 0; idx < This->wineasio_number_inputs; ++idx) {
        chan = &This->input_channel[idx];
        if (!chan->active)
            continue;
        //chan->buffers[This->asio_buffer_index] = pw_filter_dequeue_buffer(chan->port);
        //pw_filter_queue_buffer(chan->port, chan->buffers[This->asio_buffer_index ^ 1]);
        buffer = pw_filter_dequeue_buffer(chan->port);
        pw_filter_queue_buffer(chan->port, buffer);
    }
    for (idx = 0; idx < This->wineasio_number_outputs; ++idx) {
        chan = &This->output_channel[idx];
        if (!chan->active)
            continue;
        //chan->buffers[This->asio_buffer_index] = pw_filter_dequeue_buffer(chan->port);
        //pw_filter_queue_buffer(chan->port, chan->buffers[This->asio_buffer_index ^ 1]);
        //desired_buffer = chan->buffers[This->asio_buffer_index];
        buffer = pw_filter_dequeue_buffer(chan->port);
        buffer->buffer->datas[0].chunk->offset = 0;
        buffer->buffer->datas[0].chunk->stride = sizeof(float);
        buffer->buffer->datas[0].chunk->size = sample_count * sizeof(float);
        if (buffer == chan->buffers[0])
            pw_filter_queue_buffer(chan->port, chan->buffers[1]);
        else
            pw_filter_queue_buffer(chan->port, chan->buffers[0]);
    }

    This->asio_sample_position = position->clock.position - position->offset;
    //This->asio_time_stamp = position->clock.nsec;
    // ASIO required to resort to using Windows API :(
    This->asio_time_stamp = timeGetTime() * 1000000ULL;

    if (This->asio_time_info_mode) /* use the newer bufferSwitchTimeInfo method if supported */
    {
        This->asio_time.timeInfo.samplePosition = ASIO_LONG(ASIOSamples, This->asio_sample_position);
        This->asio_time.timeInfo.systemTime = ASIO_LONG(ASIOTimeStamp, This->asio_time_stamp);
        This->asio_time.timeInfo.sampleRate = This->asio_sample_rate;
        This->asio_time.timeInfo.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid;

        #if 0
        if (This->asio_can_time_code) /* FIXME addionally use time code if supported */
        {
            jack_transport_state = jack_transport_query(This->jack_client, &jack_position);
            This->asio_time.timeCode.flags = kTcValid;
            if (jack_transport_state == JackTransportRolling)
                This->asio_time.timeCode.flags |= kTcRunning;
        }
        #endif
        This->asio_callbacks->bufferSwitchTimeInfo(&This->asio_time, This->asio_buffer_index, ASIOTrue);
    }
    else
    { /* use the old bufferSwitch method */
        This->asio_callbacks->bufferSwitch(This->asio_buffer_index, ASIOTrue);
    }

    /* swith asio buffer */
    This->asio_buffer_index ^= 1;
}

static struct pw_filter_events const pw_filter_events = {
    .version = PW_VERSION_FILTER_EVENTS,
    .state_changed = pipewire_state_changed_callback,
    .io_changed = pipewire_io_changed_callback,
    .param_changed = pipewire_param_changed_callback,
    .add_buffer = pipewire_add_buffer_callback,
    .remove_buffer = pipewire_remove_buffer_callback,
    .process = pipewire_process_callback,
};

/*****************************************************************************
 * Interface method definitions
 */


HIDDEN HRESULT STDMETHODCALLTYPE QueryInterface(LPWINEASIO iface, REFIID riid, void **ppvObject)
{
    IWineASIOImpl   *This = (IWineASIOImpl *)iface;

    TRACE("iface: %p, riid: %s, ppvObject: %p)\n", iface, debugstr_guid(riid), ppvObject);

    if (ppvObject == NULL)
        return E_INVALIDARG;

    if (IsEqualIID(&CLSID_PipeWireASIO, riid))
    {
        AddRef(iface);
        *ppvObject = This;
        return S_OK;
    }

    return E_NOINTERFACE;
}

/*
 * ULONG STDMETHODCALLTYPE AddRef(LPWINEASIO iface);
 * Function: Increment the reference count on the object
 * Returns:  Ref count
 */

HIDDEN ULONG STDMETHODCALLTYPE AddRef(LPWINEASIO iface)
{
    IWineASIOImpl   *This = (IWineASIOImpl *)iface;
    ULONG           ref = InterlockedIncrement(&(This->ref));

    TRACE("iface: %p, ref count is %d\n", iface, ref);
    return ref;
}

/*
 * ULONG Release (LPWINEASIO iface);
 *  Function:   Destroy the interface
 *  Returns:    Ref count
 *  Implies:    ASIOStop() and ASIODisposeBuffers()
 */

HIDDEN ULONG STDMETHODCALLTYPE Release(LPWINEASIO iface)
{
    IWineASIOImpl   *This = (IWineASIOImpl *)iface;
    ULONG            ref = InterlockedDecrement(&This->ref);

    TRACE("iface: %p, ref count is %d\n", iface, ref);

    if (This->asio_driver_state == Running)
        Stop(iface);
    if (This->asio_driver_state == Prepared)
        DisposeBuffers(iface);

    if (This->asio_driver_state == Initialized)
    {
        /* just for good measure we deinitialize IOChannel structures and unregister JACK ports */
        for (int i = 0; i < This->wineasio_number_inputs; i++)
        {
            //jack_port_unregister (This->jack_client, This->input_channel[i].port);
            This->input_channel[i].active = false;
        }
        for (int i = 0; i < This->wineasio_number_outputs; i++)
        {
            //jack_port_unregister (This->jack_client, This->output_channel[i].port);
            This->output_channel[i].active = false;
        }
        This->asio_active_inputs = This->asio_active_outputs = 0;
        TRACE("%i IOChannel structures released\n", This->wineasio_number_inputs + This->wineasio_number_outputs);

        //jack_free (This->jack_output_ports);
        //jack_free (This->jack_input_ports);
        //jack_client_close(This->jack_client);
        if (This->input_channel)
            HeapFree(GetProcessHeap(), 0, This->input_channel);
    }
    if (ref == 0) {
        TRACE("PipeWireASIO terminated\n\n");
        This->cls_factory->lpVtbl->Release(This->cls_factory);
        HeapFree(GetProcessHeap(), 0, This);
    }
    return ref;
}

static void Uninit(IWineASIOImpl *This) {
    // TODOOOO
}

static ASIOError InitPorts(IWineASIOImpl *This) {
    int idx;

    /* Allocate IOChannel structures */
    This->input_channel = HeapAlloc(GetProcessHeap(), 0, (This->wineasio_number_inputs + This->wineasio_number_outputs) * sizeof(struct io_port));
    if (!This->input_channel)
    {
        ERR("Unable to allocate IOChannel structures for %i channels\n", This->wineasio_number_inputs + This->wineasio_number_outputs);
        return ASE_NoMemory;
    }
    This->output_channel = This->input_channel + This->wineasio_number_inputs;
    TRACE("%i IOChannel structures allocated\n", This->wineasio_number_inputs + This->wineasio_number_outputs);

    /* Set up ports */

    char pod_buffer[0x1000];
    struct spa_pod_builder pod_builder = SPA_POD_BUILDER_INIT(pod_buffer, sizeof pod_buffer);

    struct spa_pod const *port_params[] = {
        spa_pod_builder_add_object(&pod_builder,
            SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_buffers, SPA_POD_Int(2),
            SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
            // TODO: Check
            SPA_PARAM_BUFFERS_dataType, SPA_POD_Int(SPA_DATA_MemPtr),
            //SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemPtr),
            SPA_PARAM_BUFFERS_size, SPA_POD_CHOICE_STEP_Int(
                This->wineasio_preferred_buffersize,
                sizeof(float),
                INT_MAX,
                sizeof(float)
            ),
            SPA_PARAM_BUFFERS_stride, SPA_POD_Int(sizeof(float))
        ),
        spa_pod_builder_add_object(&pod_builder,
            SPA_TYPE_OBJECT_ParamIO, SPA_PARAM_IO,
            SPA_PARAM_IO_id, SPA_POD_Id(SPA_IO_Buffers),
            SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_buffers))
        ),
    };

    //char port_name[32];
    #define INPUT_PORT_PREFIX "input_"
    //memcpy(port_name, INPUT_PORT_PREFIX, sizeof(INPUT_PORT_PREFIX));
    for (idx = 0; idx < This->wineasio_number_inputs; ++idx) {
        //snprintf(port_name + sizeof(INPUT_PORT_PREFIX), sizeof(port_name) - sizeof(INPUT_PORT_PREFIX), "%d", idx);
        snprintf(This->input_channel[idx].port_name, ASIO_MAX_NAME_LENGTH, INPUT_PORT_PREFIX "%d", idx);
        This->input_channel[idx].port = pw_filter_add_port(This->pw_filter,
            PW_DIRECTION_INPUT,
            PW_FILTER_PORT_FLAG_MAP_BUFFERS,
            0,
            pw_properties_new(
                PW_KEY_PORT_NAME, This->input_channel[idx].port_name,
                PW_KEY_FORMAT_DSP, JACK_DEFAULT_AUDIO_TYPE,
                NULL),
            port_params, ARRAYSIZE(port_params));
    }
    #define OUTPUT_PORT_PREFIX "output_"
    //memcpy(port_name, OUTPUT_PORT_PREFIX, sizeof(OUTPUT_PORT_PREFIX));
    for (idx = 0; idx < This->wineasio_number_outputs; ++idx) {
        //snprintf(port_name + sizeof(OUTPUT_PORT_PREFIX), sizeof(port_name) - sizeof(OUTPUT_PORT_PREFIX), "%d", idx);
        snprintf(This->output_channel[idx].port_name, ASIO_MAX_NAME_LENGTH, OUTPUT_PORT_PREFIX "%d", idx);
        This->output_channel[idx].port = pw_filter_add_port(This->pw_filter,
            PW_DIRECTION_OUTPUT,
            PW_FILTER_PORT_FLAG_MAP_BUFFERS,
            0,
            pw_properties_new(
                PW_KEY_PORT_NAME, This->output_channel[idx].port_name,
                PW_KEY_FORMAT_DSP, JACK_DEFAULT_AUDIO_TYPE,
                NULL),
            port_params, ARRAYSIZE(port_params));
    }
    TRACE("%i IOChannel structures initialized\n", This->wineasio_number_inputs + This->wineasio_number_outputs);

    return ASE_OK;
}

/*
 * ASIOBool Init (void *sysRef);
 *  Function:   Initialize the driver
 *  Parameters: Pointer to "This"
 *              sysHanle is 0 on OS/X and on windows it contains the applications main window handle
 *  Returns:    ASIOFalse on error, and ASIOTrue on success
 */

DEFINE_THISCALL_WRAPPER(Init,8)
HIDDEN ASIOBool STDMETHODCALLTYPE Init(LPWINEASIO iface, void *sysRef)
{
    IWineASIOImpl   *This = (IWineASIOImpl *)iface;

    struct pw_helper_init_args init_args = {
        .app_name = This->client_name,
        .loop = &This->pw_loop,
        .context = &This->pw_context,
        .core = &This->pw_core,
        .thread_creator = jack_thread_creator,
    };

    This->sys_ref = sysRef;
    configure_driver(This);

    if (!(This->pw_helper = user_pw_create_helper(0, NULL, &init_args)))
    {
        return ASIOFalse;
    }

    This->gui = NULL;
    This->gui_conf.user = This;
    This->gui_conf.closed = GuiClosed;
    This->gui_conf.apply_config = GuiApplyConfig;
    This->gui_conf.load_config = GuiLoadConfig;
    This->gui_conf.pw_helper = This->pw_helper;
    This->gui_conf.cf_buffer_size = This->wineasio_preferred_buffersize;
    This->gui_conf.cf_io_type = PWASIO_IO_SIMPLE;
    This->gui_conf.cf_io_config.simple.input = PWASIO_NODE_DEFAULT;
    This->gui_conf.cf_io_config.simple.output = PWASIO_NODE_DEFAULT;

    get_nodes_by_name(This);

    if (This->current_input_node)
        TRACE("Selected input node: %u\n", pw_proxy_get_bound_id((struct pw_proxy *)This->current_input_node));
    if (This->current_output_node)
        TRACE("Selected output node: %u\n", pw_proxy_get_bound_id((struct pw_proxy *)This->current_output_node));

    //This->asio_sample_rate = jack_get_sample_rate(This->jack_client);
    //This->asio_current_buffersize = jack_get_buffer_size(This->jack_client);

    user_pw_lock_loop(This->pw_helper);

    This->pw_filter = pw_filter_new(This->pw_core, This->client_name, pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_ROLE, "DSP",
        PW_KEY_MEDIA_CLASS, "Stream/Audio",
        PW_KEY_MEDIA_CATEGORY, "Duplex",
        NULL
    ));

    if (!This->pw_filter) {
        ERR("Failed to create filter node\n");
        return ASIOFalse;
    }

    pw_filter_add_listener(This->pw_filter, &This->pw_filter_listener, &pw_filter_events, This);

    InitPorts(This);

    user_pw_unlock_loop(This->pw_helper);

    #if 0
    jack_set_thread_creator(jack_thread_creator);

    if (jack_set_buffer_size_callback(This->jack_client, jack_buffer_size_callback, This))
    {
        Uninit(This);
        HeapFree(GetProcessHeap(), 0, This->input_channel);
        ERR("Unable to register JACK buffer size change callback\n");
        return ASIOFalse;
    }
    
    if (jack_set_latency_callback(This->jack_client, jack_latency_callback, This))
    {
        Uninit(This);
        HeapFree(GetProcessHeap(), 0, This->input_channel);
        ERR("Unable to register JACK latency callback\n");
        return ASIOFalse;
    }


    if (jack_set_process_callback(This->jack_client, jack_process_callback, This))
    {
        jack_client_close(This->jack_client);
        HeapFree(GetProcessHeap(), 0, This->input_channel);
        ERR("Unable to register JACK process callback\n");
        return ASIOFalse;
    }

    if (jack_set_sample_rate_callback (This->jack_client, jack_sample_rate_callback, This))
    {
        jack_client_close(This->jack_client);
        HeapFree(GetProcessHeap(), 0, This->input_channel);
        ERR("Unable to register JACK sample rate change callback\n");
        return ASIOFalse;
    }
    #endif

    This->asio_driver_state = Initialized;
    TRACE("PipeWireASIO 0.%d.%d initialized\n", This->asio_version / 10, This->asio_version % 10);
    return ASIOTrue;
}

/*
 * void GetDriverName(char *name);
 *  Function:    Returns the driver name in name
 */

DEFINE_THISCALL_WRAPPER(GetDriverName,8)
HIDDEN void STDMETHODCALLTYPE GetDriverName(LPWINEASIO iface, char *name)
{
    TRACE("iface: %p, name: %p\n", iface, name);
    strcpy(name, "PipeWireASIO");
    return;
}

/*
 * LONG GetDriverVersion (void);
 *  Function:    Returns the driver version number
 */

DEFINE_THISCALL_WRAPPER(GetDriverVersion,4)
HIDDEN LONG STDMETHODCALLTYPE GetDriverVersion(LPWINEASIO iface)
{
    IWineASIOImpl   *This = (IWineASIOImpl*)iface;

    TRACE("iface: %p\n", iface);
    return This->asio_version;
}

/*
 * void GetErrorMessage(char *string);
 *  Function:    Returns an error message for the last occured error in string
 */

DEFINE_THISCALL_WRAPPER(GetErrorMessage,8)
HIDDEN void STDMETHODCALLTYPE GetErrorMessage(LPWINEASIO iface, char *string)
{
    TRACE("iface: %p, string: %p)\n", iface, string);
    strcpy(string, "PipeWireASIO does not return error messages\n");
    return;
}

/*
 * ASIOError Start(void);
 *  Function:    Start JACK IO processing and reset the sample counter to zero
 *  Returns:     ASE_NotPresent if IO is missing
 *               ASE_HWMalfunction if JACK fails to start
 */

DEFINE_THISCALL_WRAPPER(Start,4)
HIDDEN ASIOError STDMETHODCALLTYPE Start(LPWINEASIO iface)
{
    IWineASIOImpl   *This = (IWineASIOImpl*)iface;

    TRACE("iface: %p\n", iface);

    if (This->asio_driver_state != Prepared)
        return ASE_NotPresent;

    user_pw_lock_loop(This->pw_helper);
    pw_filter_set_active(This->pw_filter, true);
    user_pw_unlock_loop(This->pw_helper);

    /* Zero the audio buffer */
    //for (i = 0; i < (This->wineasio_number_inputs + This->wineasio_number_outputs) * 2 * This->asio_current_buffersize; i++)
    //    This->callback_audio_buffer[i] = 0;

    /* prime the callback by preprocessing one outbound ASIO bufffer */
    This->asio_buffer_index =  0;
    This->asio_sample_position = 0;

    //This->asio_time_stamp = pw_filter_get_nsec(This->pw_filter);
    // ASIO required to resort to using Windows API :(
    This->asio_time_stamp = timeGetTime() * 1000000ULL;

    if (This->asio_time_info_mode) /* use the newer bufferSwitchTimeInfo method if supported */
    {
        This->asio_time.timeInfo.samplePosition = ASIO_LONG(ASIOSamples, 0);
        This->asio_time.timeInfo.systemTime = ASIO_LONG(ASIOTimeStamp, This->asio_time_stamp);
        This->asio_time.timeInfo.sampleRate = This->asio_sample_rate;
        This->asio_time.timeInfo.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid;

        if (This->asio_can_time_code) /* addionally use time code if supported */
        {
            This->asio_time.timeCode.speed = 1; /* FIXME */
            This->asio_time.timeCode.timeCodeSamples = ASIO_LONG(ASIOSamples, This->asio_time_stamp);
            This->asio_time.timeCode.flags = ~(kTcValid | kTcRunning);
        }
        //This->asio_callbacks->bufferSwitchTimeInfo(&This->asio_time, This->asio_buffer_index, ASIOTrue);
    } 
    else
    { /* use the old bufferSwitch method */
        //This->asio_callbacks->bufferSwitch(This->asio_buffer_index, ASIOTrue);
    }

    /* swith asio buffer */
    //This->asio_buffer_index ^= 1;

    This->asio_driver_state = Running;
    TRACE("PipeWireASIO successfully loaded\n");
    return ASE_OK;
}

/*
 * ASIOError Stop(void);
 *  Function:   Stop JACK IO processing
 *  Returns:    ASE_NotPresent on missing IO
 *  Note:       BufferSwitch() must not called after returning
 */

DEFINE_THISCALL_WRAPPER(Stop,4)
HIDDEN ASIOError STDMETHODCALLTYPE Stop(LPWINEASIO iface)
{
    IWineASIOImpl   *This = (IWineASIOImpl*)iface;

    TRACE("iface: %p\n", iface);

    if (This->asio_driver_state != Running)
        return ASE_NotPresent;

    user_pw_lock_loop(This->pw_helper);
    pw_filter_set_active(This->pw_filter, false);
    user_pw_unlock_loop(This->pw_helper);

    This->asio_driver_state = Prepared;

    return ASE_OK;
}

/*
 * ASIOError GetChannels(LONG *numInputChannels, LONG *numOutputChannels);
 *  Function:   Report number of IO channels
 *  Parameters: numInputChannels and numOutputChannels will hold number of channels on returning
 *  Returns:    ASE_NotPresent if no channels are available, otherwise AES_OK
 */

DEFINE_THISCALL_WRAPPER(GetChannels,12)
HIDDEN ASIOError STDMETHODCALLTYPE GetChannels (LPWINEASIO iface, LONG *numInputChannels, LONG *numOutputChannels)
{
    IWineASIOImpl   *This = (IWineASIOImpl*)iface;

    if (!numInputChannels || !numOutputChannels)
        return ASE_InvalidParameter;

    *numInputChannels = This->wineasio_number_inputs;
    *numOutputChannels = This->wineasio_number_outputs;
    TRACE("iface: %p, inputs: %i, outputs: %i\n", iface, This->wineasio_number_inputs, This->wineasio_number_outputs);
    return ASE_OK;
}

/*
 * ASIOError GetLatencies(LONG *inputLatency, LONG *outputLatency);
 *  Function:   Return latency in frames
 *  Returns:    ASE_NotPresent if no IO is available, otherwise AES_OK
 */

DEFINE_THISCALL_WRAPPER(GetLatencies,12)
HIDDEN ASIOError STDMETHODCALLTYPE GetLatencies(LPWINEASIO iface, LONG *inputLatency, LONG *outputLatency)
{
    IWineASIOImpl           *This = (IWineASIOImpl*)iface;

    if (!inputLatency || !outputLatency)
        return ASE_InvalidParameter;

    if (This->asio_driver_state == Loaded)
        return ASE_NotPresent;

    /*jack_port_get_latency_range(This->input_channel[0].port, JackCaptureLatency, &range);
    *inputLatency = range.max;
    jack_port_get_latency_range(This->output_channel[0].port, JackPlaybackLatency, &range);
    *outputLatency = range.max;*/
    TRACE("iface: %p, input latency: %d, output latency: %d\n", iface, *inputLatency, *outputLatency);

    *inputLatency = This->asio_current_buffersize;
    *outputLatency = This->asio_current_buffersize;
    return ASE_OK;
}

/*
 * ASIOError GetBufferSize(LONG *minSize, LONG *maxSize, LONG *preferredSize, LONG *granularity);
 *  Function:    Return minimum, maximum, preferred buffer sizes, and granularity
 *               At the moment return all the same, and granularity 0
 *  Returns:    ASE_NotPresent on missing IO
 */

DEFINE_THISCALL_WRAPPER(GetBufferSize,20)
HIDDEN ASIOError STDMETHODCALLTYPE GetBufferSize(LPWINEASIO iface, LONG *minSize, LONG *maxSize, LONG *preferredSize, LONG *granularity)
{
    IWineASIOImpl   *This = (IWineASIOImpl*)iface;

    TRACE("iface: %p, minSize: %p, maxSize: %p, preferredSize: %p, granularity: %p\n", iface, minSize, maxSize, preferredSize, granularity);

    if (!minSize || !maxSize || !preferredSize || !granularity)
        return ASE_InvalidParameter;

    if (This->wineasio_fixed_buffersize)
    {
        *minSize = *maxSize = *preferredSize = This->asio_current_buffersize;
        *granularity = 0;
        TRACE("Buffersize fixed at %i\n", This->asio_current_buffersize);
        return ASE_OK;
    }

    *minSize = ASIO_MINIMUM_BUFFERSIZE;
    *maxSize = ASIO_MAXIMUM_BUFFERSIZE;
    *preferredSize = This->wineasio_preferred_buffersize;
    *granularity = 1;
    TRACE("The ASIO host can control buffersize\nMinimum: %i, maximum: %i, preferred: %i, granularity: %i, current: %i\n",
          *minSize, *maxSize, *preferredSize, *granularity, This->asio_current_buffersize);
    return ASE_OK;
}

/*
 * ASIOError CanSampleRate(ASIOSampleRate sampleRate);
 *  Function:   Ask if specific SR is available
 *  Returns:    ASE_NoClock if SR isn't available, ASE_NotPresent on missing IO
 */

DEFINE_THISCALL_WRAPPER(CanSampleRate,12)
HIDDEN ASIOError STDMETHODCALLTYPE CanSampleRate(LPWINEASIO iface, ASIOSampleRate sampleRate)
{
    IWineASIOImpl   *This = (IWineASIOImpl*)iface;

    TRACE("iface: %p, Samplerate = %li, requested samplerate = %li\n", iface, (long) This->asio_sample_rate, (long) sampleRate);

    //if (sampleRate != This->asio_sample_rate)
    //    return ASE_NoClock;
    return ASE_OK;
}

/*
 * ASIOError GetSampleRate(ASIOSampleRate *currentRate);
 *  Function:   Return current SR
 *  Parameters: currentRate will hold SR on return, 0 if unknown
 *  Returns:    ASE_NoClock if SR is unknown, ASE_NotPresent on missing IO
 */

DEFINE_THISCALL_WRAPPER(GetSampleRate,8)
HIDDEN ASIOError STDMETHODCALLTYPE GetSampleRate(LPWINEASIO iface, ASIOSampleRate *sampleRate)
{
    IWineASIOImpl   *This = (IWineASIOImpl*)iface;

    TRACE("iface: %p, Sample rate is %i\n", iface, (int) This->asio_sample_rate);

    if (!sampleRate)
        return ASE_InvalidParameter;

    *sampleRate = This->asio_sample_rate;
    return ASE_OK;
}

/*
 * ASIOError SetSampleRate(ASIOSampleRate sampleRate);
 *  Function:   Set requested SR, enable external sync if SR == 0
 *  Returns:    ASE_NoClock if unknown SR
 *              ASE_InvalidMode if current clock is external and SR != 0
 *              ASE_NotPresent on missing IO
 */

DEFINE_THISCALL_WRAPPER(SetSampleRate,12)
HIDDEN ASIOError STDMETHODCALLTYPE SetSampleRate(LPWINEASIO iface, ASIOSampleRate sampleRate)
{
    IWineASIOImpl   *This = (IWineASIOImpl*)iface;

    TRACE("iface: %p, Sample rate %f requested\n", iface, sampleRate);

    This->asio_sample_rate = sampleRate;
    return ASE_OK;
}

/*
 * ASIOError GetClockSources(ASIOClockSource *clocks, LONG *numSources);
 *  Function:   Return available clock sources
 *  Parameters: clocks - a pointer to an array of ASIOClockSource structures.
 *              numSources - when called: number of allocated members
 *                         - on return: number of clock sources, the minimum is 1 - the internal clock
 *  Returns:    ASE_NotPresent on missing IO
 */

DEFINE_THISCALL_WRAPPER(GetClockSources,12)
HIDDEN ASIOError STDMETHODCALLTYPE GetClockSources(LPWINEASIO iface, ASIOClockSource *clocks, LONG *numSources)
{
    TRACE("iface: %p, clocks: %p, numSources: %p\n", iface, clocks, numSources);

    if (!clocks || !numSources)
        return ASE_InvalidParameter;

    clocks->index = 0;
    clocks->associatedChannel = -1;
    clocks->associatedGroup = -1;
    clocks->isCurrentSource = ASIOTrue;
    strcpy(clocks->name, "Internal");
    *numSources = 1;
    return ASE_OK;
}

/*
 * ASIOError SetClockSource(LONG index);
 *  Function:   Set clock source
 *  Parameters: index returned by ASIOGetClockSources() - See asio.h for more details
 *  Returns:    ASE_NotPresent on missing IO
 *              ASE_InvalidMode may be returned if a clock can't be selected
 *              ASE_NoClock should not be returned
 */

DEFINE_THISCALL_WRAPPER(SetClockSource,8)
HIDDEN ASIOError STDMETHODCALLTYPE SetClockSource(LPWINEASIO iface, LONG index)
{
    TRACE("iface: %p, index: %i\n", iface, index);

    if (index != 0)
        return ASE_NotPresent;
    return ASE_OK;
}

/*
 * ASIOError GetSamplePosition (ASIOSamples *sPos, ASIOTimeStamp *tStamp);
 *  Function:   Return sample position and timestamp
 *  Parameters: sPos holds the position on return, reset to 0 on ASIOStart()
 *              tStamp holds the system time of sPos
 *  Return:     ASE_NotPresent on missing IO
 *              ASE_SPNotAdvancing on missing clock
 */

DEFINE_THISCALL_WRAPPER(GetSamplePosition,12)
HIDDEN ASIOError STDMETHODCALLTYPE GetSamplePosition(LPWINEASIO iface, ASIOSamples *sPos, ASIOTimeStamp *tStamp)
{
    IWineASIOImpl   *This = (IWineASIOImpl*)iface;

    TRACE("iface: %p, sPos: %p, tStamp: %p\n", iface, sPos, tStamp);

    if (!sPos || !tStamp)
        return ASE_InvalidParameter;

    *tStamp = ASIO_LONG(ASIOTimeStamp, This->asio_time_stamp);
    *sPos = ASIO_LONG(ASIOSamples, This->asio_sample_position);

    return ASE_OK;
}

/*
 * ASIOError GetChannelInfo (ASIOChannelInfo *info);
 *  Function:   Retrive channel info. - See asio.h for more detail
 *  Returns:    ASE_NotPresent on missing IO
 */

DEFINE_THISCALL_WRAPPER(GetChannelInfo,8)
HIDDEN ASIOError STDMETHODCALLTYPE GetChannelInfo(LPWINEASIO iface, ASIOChannelInfo *info)
{
    IWineASIOImpl   *This = (IWineASIOImpl*)iface;

    TRACE("(iface: %p, info: %p\n", iface, info);

    if (info->channel < 0 || (info->isInput ? info->channel >= This->wineasio_number_inputs : info->channel >= This->wineasio_number_outputs))
        return ASE_InvalidParameter;

    info->channelGroup = 0;
    info->type = ASIOSTFloat32LSB;

    if (info->isInput)
    {
        info->isActive = This->input_channel[info->channel].active;
        memcpy(info->name, This->input_channel[info->channel].port_name, ASIO_MAX_NAME_LENGTH);
    }
    else
    {
        info->isActive = This->output_channel[info->channel].active;
        memcpy(info->name, This->output_channel[info->channel].port_name, ASIO_MAX_NAME_LENGTH);
    }
    return ASE_OK;
}

/*
 * ASIOError CreateBuffers(ASIOBufferInfo *bufferInfo, LONG numChannels, LONG bufferSize, ASIOCallbacks *asioCallbacks);
 *  Function:   Allocate buffers for IO channels
 *  Parameters: bufferInfo      - pointer to an array of ASIOBufferInfo structures
 *              numChannels     - the total number of IO channels to be allocated
 *              bufferSize      - one of the buffer sizes retrieved with ASIOGetBufferSize()
 *              asioCallbacks   - pointer to an ASIOCallbacks structure
 *              See asio.h for more detail
 *  Returns:    ASE_NoMemory if impossible to allocate enough memory
 *              ASE_InvalidMode on unsupported bufferSize or invalid bufferInfo data
 *              ASE_NotPresent on missing IO
 */

DEFINE_THISCALL_WRAPPER(CreateBuffers,20)
HIDDEN ASIOError STDMETHODCALLTYPE CreateBuffers(LPWINEASIO iface, ASIOBufferInfo *bufferInfo, LONG numChannels, LONG bufferSize, ASIOCallbacks *asioCallbacks)
{
    IWineASIOImpl   *This = (IWineASIOImpl*)iface;
    ASIOBufferInfo  *buffer_info = bufferInfo;
    ASIOError        status;
    int             i, j, k;

    TRACE("iface: %p, driver state: %d, bufferInfo: %p, numChannels: %i, bufferSize: %i, asioCallbacks: %p\n", iface, This->asio_driver_state, bufferInfo, (int)numChannels, (int)bufferSize, asioCallbacks);

    if (This->asio_driver_state != Initialized)
        return ASE_NotPresent;

    if (!bufferInfo || !asioCallbacks)
        return ASE_InvalidMode;

    /* set buf_size */
    if (This->wineasio_fixed_buffersize)
    {
        if (This->asio_current_buffersize != bufferSize)
            return ASE_InvalidMode;
        TRACE("Buffersize fixed at %i\n", (int)This->asio_current_buffersize);
    }
    else
    { /* fail if out of range */
        if (!(bufferSize >= ASIO_MINIMUM_BUFFERSIZE
            && bufferSize <= ASIO_MAXIMUM_BUFFERSIZE))
        {
            WARN("Invalid buffersize %i requested\n", (int)bufferSize);
            return ASE_InvalidMode;
        }

        if (This->asio_current_buffersize == bufferSize)
        {
            TRACE("Buffer size already set to %i\n", (int)This->asio_current_buffersize);
        }
        else
        {
            This->asio_current_buffersize = bufferSize;
            TRACE("Buffer size changed to %i\n", (int)This->asio_current_buffersize);
        }
    }

    /* print/discover ASIO host capabilities */
    This->asio_callbacks = asioCallbacks;
    This->asio_time_info_mode = This->asio_can_time_code = FALSE;

    TRACE("The ASIO host supports ASIO v%i: ", This->asio_callbacks->asioMessage(kAsioEngineVersion, 0, 0, 0));
    if (This->asio_callbacks->asioMessage(kAsioSelectorSupported, kAsioBufferSizeChange, 0 , 0))
        TRACE("kAsioBufferSizeChange ");
    if (This->asio_callbacks->asioMessage(kAsioSelectorSupported, kAsioResetRequest, 0 , 0))
        TRACE("kAsioResetRequest ");
    if (This->asio_callbacks->asioMessage(kAsioSelectorSupported, kAsioResyncRequest, 0 , 0))
        TRACE("kAsioResyncRequest ");
    if (This->asio_callbacks->asioMessage(kAsioSelectorSupported, kAsioLatenciesChanged, 0 , 0))
        TRACE("kAsioLatenciesChanged ");

    if (This->asio_callbacks->asioMessage(kAsioSupportsTimeInfo, 0, 0, 0))
    {
        TRACE("bufferSwitchTimeInfo ");
        This->asio_time_info_mode = TRUE;
        if (This->asio_callbacks->asioMessage(kAsioSupportsTimeCode,  0, 0, 0))
        {
            TRACE("TimeCode");
            This->asio_can_time_code = TRUE;
        }
    }
    else
        TRACE("BufferSwitch");
    TRACE("\n");

    /* initialize ASIOBufferInfo structures */
    buffer_info = bufferInfo;
    This->asio_active_inputs = This->asio_active_outputs = 0;

    for (i = 0; i < This->wineasio_number_inputs; i++) {
        This->input_channel[i].active = false;
    }
    for (i = 0; i < This->wineasio_number_outputs; i++) {
        This->output_channel[i].active = false;
    }

    for (i = 0; i < numChannels; i++, buffer_info++)
    {
        struct io_port *chan;
        if (buffer_info->isInput)
        {
            if (buffer_info->channelNum >= This->wineasio_number_inputs) {
                WARN("Non-existant input channel requested: %u/%u\n", buffer_info->channelNum, This->wineasio_number_inputs);
                return ASE_InvalidMode;
            }
            This->asio_active_inputs++;
            chan = &This->input_channel[buffer_info->channelNum];
        }
        else
        {
            if (buffer_info->channelNum >= This->wineasio_number_outputs) {
                WARN("Non-existant output channel requested: %u/%u\n", buffer_info->channelNum, This->wineasio_number_outputs);
                return ASE_InvalidMode;
            }
            This->asio_active_outputs++;
            chan = &This->output_channel[buffer_info->channelNum];
        }

        chan->active = true;
        chan->buffers[0] = NULL;
        chan->buffers[1] = NULL;
    }

    user_pw_lock_loop(This->pw_helper);
    /*status = InitPorts(This);
    if (status != ASE_OK)
        return status;*/
    char pod_buffer[0x1000];
    struct spa_pod_builder pod_builder = SPA_POD_BUILDER_INIT(pod_buffer, sizeof pod_buffer);

    struct spa_audio_info_raw format = SPA_AUDIO_INFO_RAW_INIT(
        .format = SPA_AUDIO_FORMAT_F32,
        .rate = This->asio_sample_rate,
        .channels = This->asio_active_outputs,
    );

    struct spa_pod const *connect_params[] = {
        spa_format_audio_raw_build(&pod_builder, SPA_PARAM_EnumFormat, &format),
    };

    This->asio_buffers_left_to_init = 2 * (This->asio_active_inputs + This->asio_active_outputs);
    pthread_barrier_init(&This->asio_buffers_filled, NULL, 2);
    pthread_barrier_init(&This->pw_filter_bound, NULL, 2);

    if (pw_filter_connect(This->pw_filter, PW_FILTER_FLAG_RT_PROCESS, connect_params, ARRAYSIZE(connect_params)) < 0) {
        ERR("Failed to setup the filter node\n");
        return ASE_HWMalfunction;
    }

    user_pw_unlock_loop(This->pw_helper);
    pthread_barrier_wait(&This->pw_filter_bound);
    //user_pw_lock_loop(This->pw_helper); // locking here interferes with default_node calls

    /* Connect all ports */
    for (i = 0; i < This->wineasio_number_inputs; ++i) {
        if (!This->input_channel[i].active)
            continue;

        connect_io_port(This, &This->input_channel[i], i, SPA_DIRECTION_INPUT);
    }

    for (i = 0; i < This->wineasio_number_outputs; ++i) {
        if (!This->output_channel[i].active)
            continue;

        connect_io_port(This, &This->output_channel[i], i, SPA_DIRECTION_OUTPUT);
    }

    /* Allocate audio buffers */
    #if 0
    buffer_info = bufferInfo;
    for (i = 0; i < numChannels; i++, buffer_info++)
    {
        IOChannel *chan;
        if (buffer_info->isInput)
        {
            chan = &This->input_channel[buffer_info->channelNum];
            /* TRACE("ASIO audio buffer for channel %i as input %li created\n", i, This->asio_active_inputs); */
        }
        else
        {
            chan = &This->output_channel[buffer_info->channelNum];
            /* TRACE("ASIO audio buffer for channel %i as output %li created\n", i, This->asio_active_outputs); */
        }

        chan->buffers[0] = pw_filter_dequeue_buffer(chan->port);
        chan->buffers[1] = pw_filter_dequeue_buffer(chan->port);
        TRACE("Channel idx %d: buffer 0: %p, buffer 1: %p\n", i, chan->buffers[0], chan->buffers[1]);
        buffer_info->buffers[0] = NULL; //chan->buffers[0]->buffer->datas->data;
        buffer_info->buffers[1] = NULL; //chan->buffers[1]->buffer->datas->data;
        chan->active = true;
    }
    TRACE("%i audio channels initialized\n", This->asio_active_inputs + This->asio_active_outputs);
    #endif

    //user_pw_unlock_loop(This->pw_helper); // see above

    pthread_barrier_wait(&This->asio_buffers_filled);

    buffer_info = bufferInfo;
    for (i = 0; i < numChannels; i++, buffer_info++)
    {
        struct io_port *chan;
        if (buffer_info->isInput)
        {
            chan = &This->input_channel[buffer_info->channelNum];
            /* TRACE("ASIO audio buffer for channel %i as input %li created\n", i, This->asio_active_inputs); */
        }
        else
        {
            chan = &This->output_channel[buffer_info->channelNum];
            /* TRACE("ASIO audio buffer for channel %i as output %li created\n", i, This->asio_active_outputs); */
        }

        TRACE("Channel idx %d: buffer 0: %p, buffer 1: %p\n", i, chan->buffers[0], chan->buffers[1]);
        buffer_info->buffers[0] = chan->buffers[0]->buffer->datas->data;
        buffer_info->buffers[1] = chan->buffers[1]->buffer->datas->data;
    }
    TRACE("%i audio channels initialized\n", This->asio_active_inputs + This->asio_active_outputs);

    #if 0
    This->callback_audio_buffer = HeapAlloc(GetProcessHeap(), 0,
        (This->wineasio_number_inputs + This->wineasio_number_outputs) * 2 * This->asio_current_buffersize * sizeof(jack_default_audio_sample_t));
    if (!This->callback_audio_buffer)
    {
        ERR("Unable to allocate %i ASIO audio buffers\n", This->wineasio_number_inputs + This->wineasio_number_outputs);
        return ASE_NoMemory;
    }
    TRACE("%i ASIO audio buffers allocated (%i kB)\n", This->wineasio_number_inputs + This->wineasio_number_outputs,
          (int) ((This->wineasio_number_inputs + This->wineasio_number_outputs) * 2 * This->asio_current_buffersize * sizeof(jack_default_audio_sample_t) / 1024));

    #endif

    /* at this point all the connections are made and the process callback is outputting silence */
    This->asio_driver_state = Prepared;
    return ASE_OK;
}

/*
 * ASIOError DisposeBuffers(void);
 *  Function:   Release allocated buffers
 *  Returns:    ASE_InvalidMode if no buffers were previously allocated
 *              ASE_NotPresent on missing IO
 *  Implies:    ASIOStop()
 */

DEFINE_THISCALL_WRAPPER(DisposeBuffers,4)
HIDDEN ASIOError STDMETHODCALLTYPE DisposeBuffers(LPWINEASIO iface)
{
    IWineASIOImpl   *This = (IWineASIOImpl*)iface;
    int             i;

    TRACE("iface: %p\n", iface);

    if (This->asio_driver_state == Running)
        Stop (iface);
    if (This->asio_driver_state != Prepared)
        return ASE_NotPresent;

    //if (jack_deactivate(This->jack_client))
    //    return ASE_NotPresent;

    This->asio_callbacks = NULL;

    for (i = 0; i < This->wineasio_number_inputs; i++)
    {
        dispose_io_port(This, &This->input_channel[i], SPA_DIRECTION_INPUT);
    }
    for (i = 0; i < This->wineasio_number_outputs; i++)
    {
        dispose_io_port(This, &This->output_channel[i], SPA_DIRECTION_OUTPUT);
    }
    This->asio_active_inputs = This->asio_active_outputs = 0;

    //if (This->callback_audio_buffer)
    //    HeapFree(GetProcessHeap(), 0, This->callback_audio_buffer);

    This->asio_driver_state = Initialized;
    return ASE_OK;
}

/*
 * ASIOError ControlPanel(void);
 *  Function:   Open a control panel for driver settings
 *  Returns:    ASE_NotPresent if no control panel exists.  Actually return code should be ignored
 *  Note:       Call the asioMessage callback if something has changed
 */

DEFINE_THISCALL_WRAPPER(ControlPanel,4)
HIDDEN ASIOError STDMETHODCALLTYPE ControlPanel(LPWINEASIO iface)
{
    IWineASIOImpl   *This = (IWineASIOImpl *)iface;
    puts("OPENING CONTROL PANEL!!!");

    if (This->gui == NULL) {
        This->gui = pwasio_init_gui(&This->gui_conf);
    }
    return ASE_OK;
}

HIDDEN void GuiClosed(struct pwasio_gui_conf *conf)
{
    IWineASIOImpl   *This = (IWineASIOImpl *)conf->user;
    pwasio_destroy_gui(This->gui);
    This->gui = NULL;
}

HIDDEN void GuiApplyConfig(struct pwasio_gui_conf *conf)
{
    IWineASIOImpl   *This = (IWineASIOImpl *)conf->user;
    This->wineasio_preferred_buffersize = conf->cf_buffer_size;
    store_config(This);
}

HIDDEN void GuiLoadConfig(struct pwasio_gui_conf *conf)
{
    IWineASIOImpl   *This = (IWineASIOImpl *)conf->user;
    conf->cf_buffer_size = This->wineasio_preferred_buffersize;
}

/*
 * ASIOError Future(LONG selector, void *opt);
 *  Function:   Various, See asio.h for more detail
 *  Returns:    Depends on the selector but in general ASE_InvalidParameter on invalid selector
 *              ASE_InvalidParameter if function is unsupported to disable further calls
 *              ASE_SUCCESS on success, do not use AES_OK
 */

DEFINE_THISCALL_WRAPPER(Future,12)
HIDDEN ASIOError STDMETHODCALLTYPE Future(LPWINEASIO iface, LONG selector, void *opt)
{
    IWineASIOImpl           *This = (IWineASIOImpl *) iface;

    TRACE("iface: %p, selector: %i, opt: %p\n", iface, selector, opt);

    switch (selector)
    {
        case kAsioEnableTimeCodeRead:
            This->asio_can_time_code = TRUE;
            TRACE("The ASIO host enabled TimeCode\n");
            return ASE_SUCCESS;
        case kAsioDisableTimeCodeRead:
            This->asio_can_time_code = FALSE;
            TRACE("The ASIO host disabled TimeCode\n");
            return ASE_SUCCESS;
        case kAsioSetInputMonitor:
            TRACE("The driver denied request to set input monitor\n");
            return ASE_NotPresent;
        case kAsioTransport:
            TRACE("The driver denied request for ASIO Transport control\n");
            return ASE_InvalidParameter;
        case kAsioSetInputGain:
            TRACE("The driver denied request to set input gain\n");
            return ASE_InvalidParameter;
        case kAsioGetInputMeter:
            TRACE("The driver denied request to get input meter \n");
            return ASE_InvalidParameter;
        case kAsioSetOutputGain:
            TRACE("The driver denied request to set output gain\n");
            return ASE_InvalidParameter;
        case kAsioGetOutputMeter:
            TRACE("The driver denied request to get output meter\n");
            return ASE_InvalidParameter;
        case kAsioCanInputMonitor:
            TRACE("The driver does not support input monitor\n");
            return ASE_InvalidParameter;
        case kAsioCanTimeInfo:
            TRACE("The driver supports TimeInfo\n");
            return ASE_SUCCESS;
        case kAsioCanTimeCode:
            TRACE("The driver supports TimeCode\n");
            return ASE_SUCCESS;
        case kAsioCanTransport:
            TRACE("The driver denied request for ASIO Transport\n");
            return ASE_InvalidParameter;
        case kAsioCanInputGain:
            TRACE("The driver does not support input gain\n");
            return ASE_InvalidParameter;
        case kAsioCanInputMeter:
            TRACE("The driver does not support input meter\n");
            return ASE_InvalidParameter;
        case kAsioCanOutputGain:
            TRACE("The driver does not support output gain\n");
            return ASE_InvalidParameter;
        case kAsioCanOutputMeter:
            TRACE("The driver does not support output meter\n");
            return ASE_InvalidParameter;
        case kAsioSetIoFormat:
            TRACE("The driver denied request to set DSD IO format\n");
            return ASE_NotPresent;
        case kAsioGetIoFormat:
            TRACE("The driver denied request to get DSD IO format\n");
            return ASE_NotPresent;
        case kAsioCanDoIoFormat:
            TRACE("The driver does not support DSD IO format\n");
            return ASE_NotPresent;
        default:
            TRACE("ASIOFuture() called with undocumented selector\n");
            return ASE_InvalidParameter;
    }
}

/*
 * ASIOError OutputReady(void);
 *  Function:   Tells the driver that output bufffers are ready
 *  Returns:    ASE_OK if supported
 *              ASE_NotPresent to disable
 */

DEFINE_THISCALL_WRAPPER(OutputReady,4)
HIDDEN ASIOError STDMETHODCALLTYPE OutputReady(LPWINEASIO iface)
{
    /* disabled to stop stand alone NI programs from spamming the console
    TRACE("iface: %p\n", iface); */
    return ASE_NotPresent;
}

/****************************************************************************
 *  JACK callbacks
 */

static inline int jack_buffer_size_callback(jack_nframes_t nframes, void *arg)
{
    IWineASIOImpl   *This = (IWineASIOImpl*)arg;

    if(This->asio_driver_state != Running)
        return 0;

    if (This->asio_callbacks->asioMessage(kAsioSelectorSupported, kAsioResetRequest, 0 , 0))
        This->asio_callbacks->asioMessage(kAsioResetRequest, 0, 0, 0);
    return 0;
}

static inline void jack_latency_callback(jack_latency_callback_mode_t mode, void *arg)
{
    IWineASIOImpl   *This = (IWineASIOImpl*)arg;

    if(This->asio_driver_state != Running)
        return;

    if (This->asio_callbacks->asioMessage(kAsioSelectorSupported, kAsioLatenciesChanged, 0 , 0))
        This->asio_callbacks->asioMessage(kAsioLatenciesChanged, 0, 0, 0);

    return;
}

#if 0
static inline int jack_process_callback(jack_nframes_t nframes, void *arg)
{
    IWineASIOImpl               *This = (IWineASIOImpl*)arg;

    int                         i;
    jack_transport_state_t      jack_transport_state;
    jack_position_t             jack_position;
    DWORD                       time;

    /* output silence if the ASIO callback isn't running yet */
    if (This->asio_driver_state != Running)
    {
        for (i = 0; i < This->asio_active_outputs; i++)
            bzero(jack_port_get_buffer(This->output_channel[i].port, nframes), sizeof (jack_default_audio_sample_t) * nframes);
        return 0;
    }

    /* copy jack to asio buffers */
    for (i = 0; i < This->wineasio_number_inputs; i++)
        if (This->input_channel[i].active)
            memcpy (&This->input_channel[i].audio_buffer[nframes * This->asio_buffer_index],
                    jack_port_get_buffer(This->input_channel[i].port, nframes),
                    sizeof (jack_default_audio_sample_t) * nframes);

    if (This->asio_sample_position.lo > ULONG_MAX - nframes)
        This->asio_sample_position.hi++;
    This->asio_sample_position.lo += nframes;

    time = timeGetTime();
    This->asio_time_stamp.lo = time * 1000000;
    This->asio_time_stamp.hi = ((unsigned long long) time * 1000000) >> 32;

    if (This->asio_time_info_mode) /* use the newer bufferSwitchTimeInfo method if supported */
    {
        This->asio_time.timeInfo.samplePosition.lo = This->asio_sample_position.lo;
        This->asio_time.timeInfo.samplePosition.hi = This->asio_sample_position.hi;
        This->asio_time.timeInfo.systemTime.lo = This->asio_time_stamp.lo;
        This->asio_time.timeInfo.systemTime.hi = This->asio_time_stamp.hi;
        This->asio_time.timeInfo.sampleRate = This->asio_sample_rate;
        This->asio_time.timeInfo.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid;

        if (This->asio_can_time_code) /* FIXME addionally use time code if supported */
        {
            jack_transport_state = jack_transport_query(This->jack_client, &jack_position);
            This->asio_time.timeCode.flags = kTcValid;
            if (jack_transport_state == JackTransportRolling)
                This->asio_time.timeCode.flags |= kTcRunning;
        }
        This->asio_callbacks->bufferSwitchTimeInfo(&This->asio_time, This->asio_buffer_index, ASIOTrue);
    }
    else
    { /* use the old bufferSwitch method */
        This->asio_callbacks->bufferSwitch(This->asio_buffer_index, ASIOTrue);
    }

    /* copy asio to jack buffers */
    for (i = 0; i < This->wineasio_number_outputs; i++)
        if (This->output_channel[i].active)
            memcpy(jack_port_get_buffer(This->output_channel[i].port, nframes),
                    &This->output_channel[i].audio_buffer[nframes * This->asio_buffer_index],
                    sizeof (jack_default_audio_sample_t) * nframes);

    /* swith asio buffer */
    This->asio_buffer_index = This->asio_buffer_index ? 0 : 1;
    return 0;
}
#endif

static inline int jack_sample_rate_callback(jack_nframes_t nframes, void *arg)
{
    IWineASIOImpl   *This = (IWineASIOImpl*)arg;

    if(This->asio_driver_state != Running)
        return 0;

    This->asio_sample_rate = nframes;
    This->asio_callbacks->sampleRateDidChange(nframes);
    return 0;
}

/*****************************************************************************
 *  Support functions
 */

#ifndef WINE_WITH_UNICODE
/* Funtion required as unicode.h no longer in WINE */
static WCHAR *strrchrW(const WCHAR* str, WCHAR ch)
{
    WCHAR *ret = NULL;
    do { if (*str == ch) ret = (WCHAR *)(ULONG_PTR)str; } while (*str++);
    return ret;
}
#endif

/* Function called by JACK to create a thread in the wine process context,
 *  uses the global structure jack_thread_creator_privates to communicate with jack_thread_creator_helper() */
static int jack_thread_creator(pthread_t* thread_id, const pthread_attr_t* attr, void *(*function)(void*), void* arg)
{
    TRACE("arg: %p, thread_id: %p, attr: %p, function: %p\n", arg, thread_id, attr, function);

    jack_thread_creator_privates.jack_callback_thread = function;
    jack_thread_creator_privates.arg = arg;
    jack_thread_creator_privates.jack_callback_thread_created = CreateEventW(NULL, FALSE, FALSE, NULL);
    CreateThread( NULL, 0, jack_thread_creator_helper, arg, 0,0 );
    WaitForSingleObject(jack_thread_creator_privates.jack_callback_thread_created, INFINITE);
    *thread_id = jack_thread_creator_privates.jack_callback_pthread_id;
    return 0;
}

/* internal helper function for returning the posix thread_id of the newly created callback thread */
static DWORD WINAPI jack_thread_creator_helper(LPVOID arg)
{
    TRACE("arg: %p\n", arg);

    jack_thread_creator_privates.jack_callback_pthread_id = pthread_self();
    SetEvent(jack_thread_creator_privates.jack_callback_thread_created);
    jack_thread_creator_privates.jack_callback_thread(jack_thread_creator_privates.arg);
    return 0;
}

static void get_nodes_by_name(IWineASIOImpl *This) {
    char *namebuf = NULL;
    int namebuf_len = 0;
    int required_len;

    This->current_input_node = NULL;
    This->current_output_node = NULL;

    if (This->pwasio_input_device_name[0]) {
        required_len = WideCharToMultiByte(CP_UTF8, 0, This->pwasio_input_device_name, -1, NULL, 0, NULL, NULL);
        if (required_len == 0) {
            fputs("ERROR: Failed to convert input device name to UTF-8\n", stderr);
        } else {
            if (namebuf_len < required_len) {
                free(namebuf);
                namebuf = malloc(required_len);
                namebuf_len = required_len;
            }
            if (0 == WideCharToMultiByte(CP_UTF8, 0, This->pwasio_input_device_name, -1, namebuf, namebuf_len, NULL, NULL)) {
                // Should never happen.
                abort();
            }
            This->current_input_node = user_pw_find_node_by_name(This->pw_helper, namebuf);
        }
    }

    if (This->pwasio_output_device_name[0]) {
        required_len = WideCharToMultiByte(CP_UTF8, 0, This->pwasio_output_device_name, -1, NULL, 0, NULL, NULL);
        if (required_len == 0) {
            fputs("ERROR: Failed to convert output device name to UTF-8\n", stderr);
        } else {
            if (namebuf_len < required_len) {
                free(namebuf);
                namebuf = malloc(required_len);
                namebuf_len = required_len;
            }
            if (0 == WideCharToMultiByte(CP_UTF8, 0, This->pwasio_output_device_name, -1, namebuf, namebuf_len, NULL, NULL)) {
                // Should never happen.
                abort();
            }
            This->current_output_node = user_pw_find_node_by_name(This->pw_helper, namebuf);
        }
    }

    free(namebuf);

    if (This->current_input_node == NULL) {
        This->current_input_node = user_pw_get_default_node(This->pw_helper, SPA_DIRECTION_INPUT);
    }
    if (This->current_output_node == NULL) {
        This->current_output_node = user_pw_get_default_node(This->pw_helper, SPA_DIRECTION_OUTPUT);
    }
}

static void connect_io_port(IWineASIOImpl *This, struct io_port *port, uint32_t idx, enum spa_direction dir) {
    struct pw_node *node;
    uint16_t dst_port_id;
    switch (This->gui_conf.cf_io_type) {
        case PWASIO_IO_SIMPLE:
            switch (dir) {
                case SPA_DIRECTION_INPUT:
                    node = This->gui_conf.cf_io_config.simple.input;
                    break;
                case SPA_DIRECTION_OUTPUT:
                    node = This->gui_conf.cf_io_config.simple.output;
                    break;
            }
            if (node == PWASIO_NODE_NONE) {
                ERR("Cannot connect inactive node\n");
                port->active = false;
                return;
            }
            if (node == PWASIO_NODE_DEFAULT) {
                node = user_pw_get_default_node(This->pw_helper, dir);
                if (node == NULL) {
                    ERR("Cannot find default node\n");
                    port->active = false;
                    return;
                }
            }
            dst_port_id = idx;
            break;
        case PWASIO_IO_ADVANCED: {
            struct pwasio_ioc_advanced const *io = &This->gui_conf.cf_io_config.advanced;
            pwasio_port_sel const *nodes;
            uint32_t node_idx;
            uint32_t cnt_ports = 0;
            switch (dir) {
                case SPA_DIRECTION_INPUT:
                    nodes = io->inputs;
                    cnt_ports = io->cnt_inputs;
                    break;
                case SPA_DIRECTION_OUTPUT:
                    nodes = io->outputs;
                    cnt_ports = io->cnt_outputs;
                    break;
            }
            if (idx >= cnt_ports) {
                ERR("Port doesn't exist (%u/%u)\n", idx, cnt_ports);
            }
            pwasio_port_get(nodes[idx], &node_idx, &dst_port_id);
            if (node_idx == 0) {
                node = user_pw_get_default_node(This->pw_helper, dir);
                if (node == NULL) {
                    ERR("Cannot find default node\n");
                    port->active = false;
                    return;
                }
            } else {
                if (node_idx > io->cnt_nodes) {
                    ERR("Node index is out of bounds (%u/%u)\n", node_idx - 1, io->cnt_nodes);
                    port->active = false;
                }
                node = io->nodes[node_idx - 1];
            }
            break;
        }
    }

    // Create the link
    uint32_t node_id = pw_proxy_get_bound_id((struct pw_proxy *)node);
    uint32_t link_src_node, link_src_port, link_dst_node, link_dst_port;
    switch (dir) {
        case SPA_DIRECTION_INPUT:
            // ext -> port
            link_src_node = node_id;
            link_src_port = dst_port_id;
            link_dst_node = pw_filter_get_node_id(This->pw_filter);
            link_dst_port = idx;
            break;
        case SPA_DIRECTION_OUTPUT:
            // port -> ext
            link_src_node = pw_filter_get_node_id(This->pw_filter);
            link_src_port = idx;
            link_dst_node = node_id;
            link_dst_port = dst_port_id;
            break;
    }
    TRACE("Creating link: %u:%u -> %u:%u\n", link_src_node, link_src_port, link_dst_node, link_dst_port);
    char props_s[4][16];
    snprintf(props_s[0], sizeof props_s[0], "%u", link_src_node);
    snprintf(props_s[1], sizeof props_s[1], "%u", link_src_port);
    snprintf(props_s[2], sizeof props_s[2], "%u", link_dst_node);
    snprintf(props_s[3], sizeof props_s[3], "%u", link_dst_port);
    struct spa_dict_item properties[] = {
        SPA_DICT_ITEM_INIT(PW_KEY_LINK_OUTPUT_NODE, props_s[0]),
        SPA_DICT_ITEM_INIT(PW_KEY_LINK_OUTPUT_PORT, props_s[1]),
        SPA_DICT_ITEM_INIT(PW_KEY_LINK_INPUT_NODE, props_s[2]),
        SPA_DICT_ITEM_INIT(PW_KEY_LINK_INPUT_PORT, props_s[3]),
    };
    struct spa_dict props = SPA_DICT_INIT_ARRAY(properties);
    user_pw_lock_loop(This->pw_helper);
    port->link = pw_core_create_object(This->pw_core, "link-factory",
        PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props, 0);
    user_pw_unlock_loop(This->pw_helper);
}

static void dispose_io_port(IWineASIOImpl *This, struct io_port *port, enum spa_direction dir) {
    port->active = false;
    port->buffers[0] = NULL;
    port->buffers[1] = NULL;
    if (port->link) {
        pw_core_destroy(This->pw_core, port->link);
        port->link = NULL;
    }
}

static void parse_boolean_env(char const *env, bool *var) {
    if (!env[0])
        return;
    if (!env[1]) {
        switch (env[0]) {
            case 'n': case 'N': case 'f': case 'F':
            case '0': *var = false; break;
            case 'y': case 'Y': case 't': case 'T':
            case '1': *var = true; break;
            default: ;
        }
        return;
    }

    if (!strcasecmp(env, "on") || !strcasecmp(env, "yes") || !strcasecmp(env, "true"))
        *var = true;
    else if (!strcasecmp(env, "off") || !strcasecmp(env, "no") || !strcasecmp(env, "false"))
        *var = false;
}

/* Unicode strings used for the registry */
static const WCHAR key_software_wine_pwasio[] = u"Software\\Wine\\PipeWireASIO";
static const WCHAR value_pwasio_number_inputs[] = u"Number of inputs";
static const WCHAR value_pwasio_number_outputs[] = u"Number of outputs";
static const WCHAR value_pwasio_buffersize_fixed[] = u"Use fixed buffer size";
static const WCHAR value_pwasio_buffersize[] = u"Buffer size";
static const WCHAR value_pwasio_connect_to_hardware[] = u"Connect to hardware";
static const WCHAR value_pwasio_input_device[] = u"Input device";
static const WCHAR value_pwasio_output_device[] = u"Output device";

static void store_config(IWineASIOImpl *This) {
    HKEY  hkey;
    LONG  result;
    DWORD bool_value;

    /* create registry entries with defaults if not present */
    result = RegCreateKeyExW(HKEY_CURRENT_USER, key_software_wine_pwasio, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hkey, NULL);

    result = RegSetValueExW(hkey, value_pwasio_number_inputs, 0, REG_DWORD, (LPBYTE) &This->wineasio_number_inputs, sizeof(This->wineasio_number_inputs));
    result = RegSetValueExW(hkey, value_pwasio_number_outputs, 0, REG_DWORD, (LPBYTE) &This->wineasio_number_outputs, sizeof(This->wineasio_number_outputs));
    result = RegSetValueExW(hkey, value_pwasio_buffersize, 0, REG_DWORD, (LPBYTE) &This->wineasio_preferred_buffersize, sizeof(This->wineasio_preferred_buffersize));
    bool_value = This->wineasio_fixed_buffersize;
    result = RegSetValueExW(hkey, value_pwasio_buffersize_fixed, 0, REG_DWORD, (LPBYTE) &bool_value, sizeof(bool_value));
    result = RegSetValueExW(hkey, value_pwasio_input_device, 0, REG_SZ, (LPBYTE) &This->pwasio_input_device_name, sizeof(This->pwasio_input_device_name));
    result = RegSetValueExW(hkey, value_pwasio_output_device, 0, REG_SZ, (LPBYTE) &This->pwasio_output_device_name, sizeof(This->pwasio_output_device_name));
}

static VOID configure_driver(IWineASIOImpl *This)
{
    HKEY    hkey;
    LONG    result, value;
    LSTATUS status;
    DWORD   type, size;
    WCHAR   application_path [MAX_PATH];
    WCHAR   *application_name;
    char    environment_variable[MAX_ENVIRONMENT_SIZE];

    /* Initialise most member variables,
     * asio_sample_position, asio_time, & asio_time_stamp are initialized in Start()
     * jack_num_input_ports & jack_num_output_ports are initialized in Init() */
    This->asio_active_inputs = 0;
    This->asio_active_outputs = 0;
    This->asio_buffer_index = 0;
    This->asio_callbacks = NULL;
    This->asio_can_time_code = FALSE;
    This->asio_current_buffersize = 0;
    This->asio_driver_state = Loaded;
    This->asio_sample_rate = 0;
    This->asio_time_info_mode = FALSE;
    This->asio_version = 10;

    This->wineasio_number_inputs = 16;
    This->wineasio_number_outputs = 16;
    This->wineasio_fixed_buffersize = FALSE;
    This->wineasio_preferred_buffersize = ASIO_PREFERRED_BUFFERSIZE;

    This->client_name[0] = 0;
    //This->callback_audio_buffer = NULL;
    This->input_channel = NULL;
    This->output_channel = NULL;

    /* create registry entries with defaults if not present */
    result = RegCreateKeyExW(HKEY_CURRENT_USER, key_software_wine_pwasio, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hkey, NULL);

    /* get/set number of asio inputs */
    size = sizeof(DWORD);
    if (RegQueryValueExW(hkey, value_pwasio_number_inputs, NULL, &type, (LPBYTE) &value, &size) == ERROR_SUCCESS)
    {
        if (type == REG_DWORD)
            This->wineasio_number_inputs = value;
    }
    else
    {
        type = REG_DWORD;
        size = sizeof(DWORD);
        value = This->wineasio_number_inputs;
        result = RegSetValueExW(hkey, value_pwasio_number_inputs, 0, REG_DWORD, (LPBYTE) &value, size);
    }

    /* get/set number of asio outputs */
    size = sizeof(DWORD);
    if (RegQueryValueExW(hkey, value_pwasio_number_outputs, NULL, &type, (LPBYTE) &value, &size) == ERROR_SUCCESS)
    {
        if (type == REG_DWORD)
            This->wineasio_number_outputs = value;
    }
    else
    {
        type = REG_DWORD;
        size = sizeof(DWORD);
        value = This->wineasio_number_outputs;
        result = RegSetValueExW(hkey, value_pwasio_number_outputs, 0, REG_DWORD, (LPBYTE) &value, size);
    }

    /* allow changing of asio buffer sizes */
    size = sizeof(DWORD);
    if (RegQueryValueExW(hkey, value_pwasio_buffersize_fixed, NULL, &type, (LPBYTE) &value, &size) == ERROR_SUCCESS)
    {
        if (type == REG_DWORD)
            This->wineasio_fixed_buffersize = value;
    }
    else
    {
        type = REG_DWORD;
        size = sizeof(DWORD);
        value = This->wineasio_fixed_buffersize;
        result = RegSetValueExW(hkey, value_pwasio_buffersize_fixed, 0, REG_DWORD, (LPBYTE) &value, size);
    }

    /* preferred buffer size (if changing buffersize is allowed) */
    size = sizeof(DWORD);
    if (RegQueryValueExW(hkey, value_pwasio_buffersize, NULL, &type, (LPBYTE) &value, &size) == ERROR_SUCCESS)
    {
        if (type == REG_DWORD)
            This->wineasio_preferred_buffersize = value;
    }
    else
    {
        type = REG_DWORD;
        size = sizeof(DWORD);
        value = This->wineasio_preferred_buffersize;
        result = RegSetValueExW(hkey, value_pwasio_buffersize, 0, REG_DWORD, (LPBYTE) &value, size);
    }

    /* input device name */
    This->pwasio_input_device_name[0] = 0;
    size = DEVICE_NAME_SIZE;
    status = RegQueryValueExW(hkey, value_pwasio_input_device, NULL, &type, (LPBYTE) &This->pwasio_input_device_name, &size);
    if (status == ERROR_SUCCESS || status == ERROR_MORE_DATA)
    {
        if (type == REG_SZ) {
            if (size > DEVICE_NAME_SIZE - 1)
                size = DEVICE_NAME_SIZE - 1;

            This->pwasio_input_device_name[size] = 0;
        }
    }
    else
    {
        size = 0;
        result = RegSetValueExW(hkey, value_pwasio_input_device, 0, REG_SZ, (LPBYTE) &This->pwasio_input_device_name, size);
    }

    /* output device name */
    This->pwasio_output_device_name[0] = 0;
    size = DEVICE_NAME_SIZE;
    status = RegQueryValueExW(hkey, value_pwasio_output_device, NULL, &type, (LPBYTE) &This->pwasio_output_device_name, &size);
    if (status == ERROR_SUCCESS || status == ERROR_MORE_DATA)
    {
        if (type == REG_SZ) {
            if (size > DEVICE_NAME_SIZE - 1)
                size = DEVICE_NAME_SIZE - 1;

            This->pwasio_output_device_name[size] = 0;
        }
    }
    else
    {
        size = 0;
        result = RegSetValueExW(hkey, value_pwasio_output_device, 0, REG_SZ, (LPBYTE) &This->pwasio_output_device_name, size);
    }

    /* override the PipeWire client name gotten from the application name */
    size = GetEnvironmentVariableW(u"PWASIO_CLIENT_NAME", application_path, ASIO_MAX_NAME_LENGTH);
    if (size == 0) {
        /* get client name by stripping path and extension */
        GetModuleFileNameW(0, application_path, MAX_PATH);
        application_name = strrchrW(application_path, L'.');
        *application_name = 0;
        application_name = strrchrW(application_path, L'\\');
        application_name++;
    } else {
        application_name = application_path;
    }

    WideCharToMultiByte(CP_UTF8, 0, application_name, -1, This->client_name, ASIO_MAX_NAME_LENGTH, NULL, NULL);

    RegCloseKey(hkey);

    /* Look for environment variables to override registry config values */

    if (GetEnvironmentVariableA("PWASIO_NUMBER_INPUTS", environment_variable, MAX_ENVIRONMENT_SIZE))
    {
        errno = 0;
        result = strtol(environment_variable, 0, 10);
        if (errno != ERANGE)
            This->wineasio_number_inputs = result;
    }

    if (GetEnvironmentVariableA("PWASIO_NUMBER_OUTPUTS", environment_variable, MAX_ENVIRONMENT_SIZE))
    {
        errno = 0;
        result = strtol(environment_variable, 0, 10);
        if (errno != ERANGE)
            This->wineasio_number_outputs = result;
    }

    if (GetEnvironmentVariableA("PWASIO_BUFFERSIZE_IS_FIXED", environment_variable, MAX_ENVIRONMENT_SIZE))
    {
        parse_boolean_env(environment_variable, &This->wineasio_fixed_buffersize);
    }

    if (GetEnvironmentVariableA("PWASIO_PREFERRED_BUFFERSIZE", environment_variable, MAX_ENVIRONMENT_SIZE))
    {
        errno = 0;
        result = strtol(environment_variable, 0, 10);
        if (errno != ERANGE)
            This->wineasio_preferred_buffersize = result;
    }

    /* if wineasio_preferred_buffersize is out of range, then set to ASIO_PREFERRED_BUFFERSIZE */
    if (!(This->wineasio_preferred_buffersize >= ASIO_MINIMUM_BUFFERSIZE
            && This->wineasio_preferred_buffersize <= ASIO_MAXIMUM_BUFFERSIZE))
        This->wineasio_preferred_buffersize = ASIO_PREFERRED_BUFFERSIZE;

    return;
}

/* Allocate the interface pointer and associate it with the vtbl/WineASIO object */
HRESULT WINAPI WineASIOCreateInstance(REFIID riid, LPVOID *ppobj, IUnknown *cls_factory)
{
    IWineASIOImpl   *pobj;

    /* TRACE("riid: %s, ppobj: %p\n", debugstr_guid(riid), ppobj); */

    pobj = HeapAlloc(GetProcessHeap(), 0, sizeof(*pobj));
    if (pobj == NULL)
    {
        WARN("out of memory\n");
        return E_OUTOFMEMORY;
    }

    pobj->lpVtbl = &WineASIO_Vtbl;
    pobj->ref = 1;
    pobj->cls_factory = cls_factory;
    cls_factory->lpVtbl->AddRef(cls_factory);
    TRACE("pobj = %p\n", pobj);
    *ppobj = pobj;
    /* TRACE("return %p\n", *ppobj); */
    return S_OK;
}
