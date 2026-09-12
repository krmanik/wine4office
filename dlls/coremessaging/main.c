/* WinRT CoreMessaging Implementation
 *
 * Copyright (C) 2024 Mohamad Al-Jaf
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

#include "initguid.h"
#include "private.h"
#include "dispatcherqueue.h"

#include "winuser.h"
#include "objbase.h"

#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(messaging);

/* A dispatcher queue belongs to one thread and runs its work items from that
 * thread's message loop, through a message-only window owned by the thread. */
#define WM_WINE_DISPATCHER_QUEUE_WORK     (WM_USER + 0x100)
#define WM_WINE_DISPATCHER_QUEUE_SHUTDOWN (WM_USER + 0x101)

static const WCHAR dispatcher_queue_class_name[] = L"__wine_dispatcher_queue";

struct handler_entry
{
    struct list entry;
    EventRegistrationToken token;
    IUnknown *handler;
};

struct dispatcher_queue
{
    IDispatcherQueue IDispatcherQueue_iface;
    IDispatcherQueue2 IDispatcherQueue2_iface;
    LONG ref;

    struct list entry;
    DWORD thread_id;
    BOOL dedicated;
    HWND window;

    CRITICAL_SECTION cs;
    BOOL shutdown;
    struct list shutdown_starting;
    struct list shutdown_completed;
    INT64 next_token;
    HANDLE shutdown_done;
    IAsyncInfo *shutdown_info;
};

static struct list dispatcher_queues = LIST_INIT( dispatcher_queues );
static CRITICAL_SECTION dispatcher_queues_cs;
static CRITICAL_SECTION_DEBUG dispatcher_queues_cs_debug =
{
    0, 0, &dispatcher_queues_cs,
    { &dispatcher_queues_cs_debug.ProcessLocksList, &dispatcher_queues_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": dispatcher_queues_cs") }
};
static CRITICAL_SECTION dispatcher_queues_cs = { &dispatcher_queues_cs_debug, -1, 0, 0, 0, 0 };

static INIT_ONCE dispatcher_queue_class_once = INIT_ONCE_STATIC_INIT;

static inline struct dispatcher_queue *impl_from_IDispatcherQueue( IDispatcherQueue *iface )
{
    return CONTAINING_RECORD( iface, struct dispatcher_queue, IDispatcherQueue_iface );
}

static void release_handlers( struct list *handlers )
{
    struct handler_entry *entry, *next;

    LIST_FOR_EACH_ENTRY_SAFE( entry, next, handlers, struct handler_entry, entry )
    {
        list_remove( &entry->entry );
        IUnknown_Release( entry->handler );
        free( entry );
    }
}

static HRESULT add_handler( struct dispatcher_queue *queue, struct list *handlers, IUnknown *handler,
                            EventRegistrationToken *token )
{
    struct handler_entry *entry;

    if (!handler || !token) return E_POINTER;
    if (!(entry = malloc( sizeof(*entry) ))) return E_OUTOFMEMORY;
    entry->handler = handler;
    IUnknown_AddRef( handler );

    EnterCriticalSection( &queue->cs );
    entry->token.value = ++queue->next_token;
    list_add_tail( handlers, &entry->entry );
    LeaveCriticalSection( &queue->cs );

    *token = entry->token;
    return S_OK;
}

static HRESULT remove_handler( struct dispatcher_queue *queue, struct list *handlers, EventRegistrationToken token )
{
    struct handler_entry *entry;
    IUnknown *handler = NULL;

    EnterCriticalSection( &queue->cs );
    LIST_FOR_EACH_ENTRY( entry, handlers, struct handler_entry, entry )
    {
        if (entry->token.value != token.value) continue;
        list_remove( &entry->entry );
        handler = entry->handler;
        free( entry );
        break;
    }
    LeaveCriticalSection( &queue->cs );

    if (handler) IUnknown_Release( handler );
    return S_OK;
}

static void dispatcher_queue_raise_shutdown_completed( struct dispatcher_queue *queue )
{
    struct handler_entry *entry;
    IUnknown **handlers;
    UINT count, i = 0;

    EnterCriticalSection( &queue->cs );
    count = list_count( &queue->shutdown_completed );
    if ((handlers = calloc( max( count, 1 ), sizeof(*handlers) )))
    {
        LIST_FOR_EACH_ENTRY( entry, &queue->shutdown_completed, struct handler_entry, entry )
        {
            handlers[i] = entry->handler;
            IUnknown_AddRef( handlers[i] );
            i++;
        }
    }
    LeaveCriticalSection( &queue->cs );
    if (!handlers) return;

    for (i = 0; i < count; i++)
    {
        ITypedEventHandler_DispatcherQueue_IInspectable *handler = (ITypedEventHandler_DispatcherQueue_IInspectable *)handlers[i];
        ITypedEventHandler_DispatcherQueue_IInspectable_Invoke( handler, &queue->IDispatcherQueue_iface, NULL );
        IUnknown_Release( handlers[i] );
    }
    free( handlers );
}

/* Runs on the queue thread once every work item queued before the shutdown request has run. */
static void dispatcher_queue_finish_shutdown( struct dispatcher_queue *queue )
{
    AsyncStatus status = Started;
    IAsyncInfo *info;
    HWND window;
    UINT i;

    EnterCriticalSection( &dispatcher_queues_cs );
    list_remove( &queue->entry );
    list_init( &queue->entry );
    LeaveCriticalSection( &dispatcher_queues_cs );

    /* Let the shutdown operation complete, and raise ShutdownCompleted only
     * once it reports completion. */
    SetEvent( queue->shutdown_done );

    EnterCriticalSection( &queue->cs );
    info = queue->shutdown_info;
    queue->shutdown_info = NULL;
    LeaveCriticalSection( &queue->cs );

    if (info)
    {
        for (i = 0; i < 5000; i++)
        {
            if (FAILED(IAsyncInfo_get_Status( info, &status )) || status != Started) break;
            Sleep( 1 );
        }
        IAsyncInfo_Release( info );
    }

    dispatcher_queue_raise_shutdown_completed( queue );

    EnterCriticalSection( &queue->cs );
    window = queue->window;
    queue->window = NULL;
    LeaveCriticalSection( &queue->cs );

    if (window)
    {
        SetWindowLongPtrW( window, GWLP_USERDATA, 0 );
        DestroyWindow( window );
    }
    if (queue->dedicated) PostQuitMessage( 0 );
}

static LRESULT CALLBACK dispatcher_queue_wndproc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct dispatcher_queue *queue;

    switch (msg)
    {
    case WM_WINE_DISPATCHER_QUEUE_WORK:
    {
        IDispatcherQueueHandler *handler = (IDispatcherQueueHandler *)lparam;
        HRESULT hr;

        if (FAILED(hr = IDispatcherQueueHandler_Invoke( handler )))
            WARN( "handler %p returned %#lx.\n", handler, hr );
        IDispatcherQueueHandler_Release( handler );
        return 0;
    }

    case WM_WINE_DISPATCHER_QUEUE_SHUTDOWN:
        if ((queue = (struct dispatcher_queue *)GetWindowLongPtrW( hwnd, GWLP_USERDATA )))
        {
            IDispatcherQueue_AddRef( &queue->IDispatcherQueue_iface );
            dispatcher_queue_finish_shutdown( queue );
            IDispatcherQueue_Release( &queue->IDispatcherQueue_iface );
        }
        return 0;
    }

    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

static HINSTANCE coremessaging_instance(void)
{
    HMODULE module = NULL;

    GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        (const WCHAR *)dispatcher_queue_wndproc, &module );
    return module;
}

static BOOL WINAPI register_dispatcher_queue_class( INIT_ONCE *once, void *param, void **context )
{
    WNDCLASSW class = {0};

    class.lpfnWndProc = dispatcher_queue_wndproc;
    class.hInstance = coremessaging_instance();
    class.lpszClassName = dispatcher_queue_class_name;
    return RegisterClassW( &class ) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

static HRESULT WINAPI dispatcher_queue_QueryInterface( IDispatcherQueue *iface, REFIID iid, void **out )
{
    struct dispatcher_queue *queue = impl_from_IDispatcherQueue( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IDispatcherQueue ))
    {
        *out = &queue->IDispatcherQueue_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IDispatcherQueue2 ))
    {
        *out = &queue->IDispatcherQueue2_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI dispatcher_queue_AddRef( IDispatcherQueue *iface )
{
    struct dispatcher_queue *queue = impl_from_IDispatcherQueue( iface );
    ULONG ref = InterlockedIncrement( &queue->ref );
    TRACE( "iface %p increasing ref to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI dispatcher_queue_Release( IDispatcherQueue *iface )
{
    struct dispatcher_queue *queue = impl_from_IDispatcherQueue( iface );
    ULONG ref = InterlockedDecrement( &queue->ref );

    TRACE( "iface %p decreasing ref to %lu.\n", iface, ref );

    if (!ref)
    {
        EnterCriticalSection( &dispatcher_queues_cs );
        list_remove( &queue->entry );
        LeaveCriticalSection( &dispatcher_queues_cs );

        if (queue->window)
        {
            SetWindowLongPtrW( queue->window, GWLP_USERDATA, 0 );
            if (queue->thread_id == GetCurrentThreadId()) DestroyWindow( queue->window );
            else PostMessageW( queue->window, WM_CLOSE, 0, 0 );
        }
        release_handlers( &queue->shutdown_starting );
        release_handlers( &queue->shutdown_completed );
        if (queue->shutdown_info) IAsyncInfo_Release( queue->shutdown_info );
        CloseHandle( queue->shutdown_done );
        DeleteCriticalSection( &queue->cs );
        free( queue );
    }
    return ref;
}

static HRESULT WINAPI dispatcher_queue_GetIids( IDispatcherQueue *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI dispatcher_queue_GetRuntimeClassName( IDispatcherQueue *iface, HSTRING *class_name )
{
    TRACE( "iface %p, class_name %p.\n", iface, class_name );
    return WindowsCreateString( RuntimeClass_Windows_System_DispatcherQueue,
                                wcslen( RuntimeClass_Windows_System_DispatcherQueue ), class_name );
}

static HRESULT WINAPI dispatcher_queue_GetTrustLevel( IDispatcherQueue *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI dispatcher_queue_CreateTimer( IDispatcherQueue *iface, IDispatcherQueueTimer **result )
{
    FIXME( "iface %p, result %p stub!\n", iface, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI dispatcher_queue_TryEnqueue( IDispatcherQueue *iface, IDispatcherQueueHandler *callback, boolean *result )
{
    struct dispatcher_queue *queue = impl_from_IDispatcherQueue( iface );

    TRACE( "iface %p, callback %p, result %p.\n", iface, callback, result );

    if (!callback || !result) return E_POINTER;

    EnterCriticalSection( &queue->cs );
    *result = FALSE;
    if (!queue->shutdown && queue->window)
    {
        IDispatcherQueueHandler_AddRef( callback );
        if (PostMessageW( queue->window, WM_WINE_DISPATCHER_QUEUE_WORK, 0, (LPARAM)callback )) *result = TRUE;
        else IDispatcherQueueHandler_Release( callback );
    }
    LeaveCriticalSection( &queue->cs );

    return S_OK;
}

static HRESULT WINAPI dispatcher_queue_TryEnqueueWithPriority( IDispatcherQueue *iface, DispatcherQueuePriority priority,
                                                               IDispatcherQueueHandler *callback, boolean *result )
{
    if (priority != DispatcherQueuePriority_Normal)
        FIXME( "iface %p, priority %d ignored.\n", iface, priority );
    return dispatcher_queue_TryEnqueue( iface, callback, result );
}

static HRESULT WINAPI dispatcher_queue_add_ShutdownStarting( IDispatcherQueue *iface,
        ITypedEventHandler_DispatcherQueue_DispatcherQueueShutdownStartingEventArgs *handler, EventRegistrationToken *token )
{
    struct dispatcher_queue *queue = impl_from_IDispatcherQueue( iface );

    FIXME( "iface %p, handler %p, token %p, event is never raised.\n", iface, handler, token );
    return add_handler( queue, &queue->shutdown_starting, (IUnknown *)handler, token );
}

static HRESULT WINAPI dispatcher_queue_remove_ShutdownStarting( IDispatcherQueue *iface, EventRegistrationToken token )
{
    struct dispatcher_queue *queue = impl_from_IDispatcherQueue( iface );

    TRACE( "iface %p, token %#I64x.\n", iface, token.value );
    return remove_handler( queue, &queue->shutdown_starting, token );
}

static HRESULT WINAPI dispatcher_queue_add_ShutdownCompleted( IDispatcherQueue *iface,
        ITypedEventHandler_DispatcherQueue_IInspectable *handler, EventRegistrationToken *token )
{
    struct dispatcher_queue *queue = impl_from_IDispatcherQueue( iface );

    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );
    return add_handler( queue, &queue->shutdown_completed, (IUnknown *)handler, token );
}

static HRESULT WINAPI dispatcher_queue_remove_ShutdownCompleted( IDispatcherQueue *iface, EventRegistrationToken token )
{
    struct dispatcher_queue *queue = impl_from_IDispatcherQueue( iface );

    TRACE( "iface %p, token %#I64x.\n", iface, token.value );
    return remove_handler( queue, &queue->shutdown_completed, token );
}

static const struct IDispatcherQueueVtbl dispatcher_queue_vtbl =
{
    dispatcher_queue_QueryInterface,
    dispatcher_queue_AddRef,
    dispatcher_queue_Release,
    /* IInspectable methods */
    dispatcher_queue_GetIids,
    dispatcher_queue_GetRuntimeClassName,
    dispatcher_queue_GetTrustLevel,
    /* IDispatcherQueue methods */
    dispatcher_queue_CreateTimer,
    dispatcher_queue_TryEnqueue,
    dispatcher_queue_TryEnqueueWithPriority,
    dispatcher_queue_add_ShutdownStarting,
    dispatcher_queue_remove_ShutdownStarting,
    dispatcher_queue_add_ShutdownCompleted,
    dispatcher_queue_remove_ShutdownCompleted,
};

DEFINE_IINSPECTABLE( dispatcher_queue2, IDispatcherQueue2, struct dispatcher_queue, IDispatcherQueue_iface )

static HRESULT WINAPI dispatcher_queue2_get_HasThreadAccess( IDispatcherQueue2 *iface, boolean *value )
{
    struct dispatcher_queue *queue = impl_from_IDispatcherQueue2( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = queue->thread_id == GetCurrentThreadId();
    return S_OK;
}

static const struct IDispatcherQueue2Vtbl dispatcher_queue2_vtbl =
{
    dispatcher_queue2_QueryInterface,
    dispatcher_queue2_AddRef,
    dispatcher_queue2_Release,
    /* IInspectable methods */
    dispatcher_queue2_GetIids,
    dispatcher_queue2_GetRuntimeClassName,
    dispatcher_queue2_GetTrustLevel,
    /* IDispatcherQueue2 methods */
    dispatcher_queue2_get_HasThreadAccess,
};

/* Must be called on the thread that will own the queue. */
static HRESULT dispatcher_queue_create( BOOL dedicated, struct dispatcher_queue **out )
{
    struct dispatcher_queue *queue;
    HRESULT hr;

    if (!InitOnceExecuteOnce( &dispatcher_queue_class_once, register_dispatcher_queue_class, NULL, NULL ))
        return HRESULT_FROM_WIN32( GetLastError() );
    if (!(queue = calloc( 1, sizeof(*queue) ))) return E_OUTOFMEMORY;

    queue->IDispatcherQueue_iface.lpVtbl = &dispatcher_queue_vtbl;
    queue->IDispatcherQueue2_iface.lpVtbl = &dispatcher_queue2_vtbl;
    queue->ref = 1;
    queue->thread_id = GetCurrentThreadId();
    queue->dedicated = dedicated;
    list_init( &queue->entry );
    list_init( &queue->shutdown_starting );
    list_init( &queue->shutdown_completed );
    InitializeCriticalSection( &queue->cs );

    if (!(queue->shutdown_done = CreateEventW( NULL, TRUE, FALSE, NULL ))
            || !(queue->window = CreateWindowExW( 0, dispatcher_queue_class_name, NULL, 0, 0, 0, 0, 0,
                                                  HWND_MESSAGE, NULL, coremessaging_instance(), NULL )))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        if (queue->shutdown_done) CloseHandle( queue->shutdown_done );
        DeleteCriticalSection( &queue->cs );
        free( queue );
        return hr;
    }
    SetWindowLongPtrW( queue->window, GWLP_USERDATA, (LONG_PTR)queue );

    EnterCriticalSection( &dispatcher_queues_cs );
    list_add_tail( &dispatcher_queues, &queue->entry );
    LeaveCriticalSection( &dispatcher_queues_cs );

    TRACE( "created queue %p on thread %04lx.\n", queue, queue->thread_id );
    *out = queue;
    return S_OK;
}

struct dedicated_thread_params
{
    DISPATCHERQUEUE_THREAD_APARTMENTTYPE apartment_type;
    HANDLE ready;
    struct dispatcher_queue *queue;
    HRESULT hr;
};

static DWORD WINAPI dispatcher_queue_thread_proc( void *arg )
{
    struct dedicated_thread_params *params = arg;
    struct dispatcher_queue *queue;
    BOOL com_initialized;
    MSG msg;

    SetThreadDescription( GetCurrentThread(), L"wine_coremessaging_dispatcher_queue" );

    if (params->apartment_type == DQTAT_COM_ASTA || params->apartment_type == DQTAT_COM_STA)
        com_initialized = SUCCEEDED(CoInitializeEx( NULL, COINIT_APARTMENTTHREADED ));
    else
        com_initialized = FALSE;

    if (FAILED(params->hr = dispatcher_queue_create( TRUE, &queue )))
    {
        SetEvent( params->ready );
        if (com_initialized) CoUninitialize();
        return 0;
    }

    /* The thread keeps its own reference until its message loop ends. */
    params->queue = queue;
    IDispatcherQueue_AddRef( &queue->IDispatcherQueue_iface );
    SetEvent( params->ready );

    while (GetMessageW( &msg, NULL, 0, 0 ) > 0)
    {
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }

    IDispatcherQueue_Release( &queue->IDispatcherQueue_iface );
    if (com_initialized) CoUninitialize();
    return 0;
}

static HRESULT dispatcher_queue_create_dedicated( DISPATCHERQUEUE_THREAD_APARTMENTTYPE apartment_type,
                                                  struct dispatcher_queue **out )
{
    struct dedicated_thread_params params = { apartment_type };
    HANDLE thread;

    if (!(params.ready = CreateEventW( NULL, TRUE, FALSE, NULL ))) return HRESULT_FROM_WIN32( GetLastError() );
    if (!(thread = CreateThread( NULL, 0, dispatcher_queue_thread_proc, &params, 0, NULL )))
    {
        CloseHandle( params.ready );
        return HRESULT_FROM_WIN32( GetLastError() );
    }
    WaitForSingleObject( params.ready, INFINITE );
    CloseHandle( params.ready );
    CloseHandle( thread );

    if (FAILED(params.hr)) return params.hr;
    *out = params.queue;
    return S_OK;
}

struct dispatcher_queue_controller_statics
{
    IActivationFactory IActivationFactory_iface;
    IDispatcherQueueControllerStatics IDispatcherQueueControllerStatics_iface;
    LONG ref;
};

static inline struct dispatcher_queue_controller_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct dispatcher_queue_controller_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct dispatcher_queue_controller_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = &impl->IActivationFactory_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IDispatcherQueueControllerStatics ))
    {
        *out = &impl->IDispatcherQueueControllerStatics_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct dispatcher_queue_controller_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing ref to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct dispatcher_queue_controller_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing ref to %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    FIXME( "iface %p, instance %p stub!\n", iface, instance );
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    /* IInspectable methods */
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    /* IActivationFactory methods */
    factory_ActivateInstance,
};

struct dispatcher_queue_controller
{
    IDispatcherQueueController IDispatcherQueueController_iface;
    LONG ref;

    struct dispatcher_queue *queue;
    IAsyncAction *shutdown_operation;
};

static inline struct dispatcher_queue_controller *impl_from_IDispatcherQueueController( IDispatcherQueueController *iface )
{
    return CONTAINING_RECORD( iface, struct dispatcher_queue_controller, IDispatcherQueueController_iface );
}

static HRESULT WINAPI dispatcher_queue_controller_QueryInterface( IDispatcherQueueController *iface, REFIID iid, void **out )
{
    struct dispatcher_queue_controller *impl = impl_from_IDispatcherQueueController( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IDispatcherQueueController ))
    {
        *out = &impl->IDispatcherQueueController_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI dispatcher_queue_controller_AddRef( IDispatcherQueueController *iface )
{
    struct dispatcher_queue_controller *impl = impl_from_IDispatcherQueueController( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing ref to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI dispatcher_queue_controller_Release( IDispatcherQueueController *iface )
{
    struct dispatcher_queue_controller *impl = impl_from_IDispatcherQueueController( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p decreasing ref to %lu.\n", iface, ref );

    if (!ref)
    {
        if (impl->shutdown_operation) IAsyncAction_Release( impl->shutdown_operation );
        IDispatcherQueue_Release( &impl->queue->IDispatcherQueue_iface );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI dispatcher_queue_controller_GetIids( IDispatcherQueueController *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI dispatcher_queue_controller_GetRuntimeClassName( IDispatcherQueueController *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI dispatcher_queue_controller_GetTrustLevel( IDispatcherQueueController *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI dispatcher_queue_controller_get_DispatcherQueue( IDispatcherQueueController *iface, IDispatcherQueue **value )
{
    struct dispatcher_queue_controller *impl = impl_from_IDispatcherQueueController( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = &impl->queue->IDispatcherQueue_iface;
    IDispatcherQueue_AddRef( *value );
    return S_OK;
}

static HRESULT shutdown_queue_async( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct dispatcher_queue *queue = impl_from_IDispatcherQueue( (IDispatcherQueue *)invoker );

    if (!called_async) return STATUS_PENDING;
    WaitForSingleObject( queue->shutdown_done, INFINITE );
    return S_OK;
}

static HRESULT WINAPI dispatcher_queue_controller_ShutdownQueueAsync( IDispatcherQueueController *iface, IAsyncAction **operation )
{
    struct dispatcher_queue_controller *impl = impl_from_IDispatcherQueueController( iface );
    struct dispatcher_queue *queue = impl->queue;
    IAsyncInfo *info = NULL;
    HRESULT hr;

    TRACE( "iface %p, operation %p.\n", iface, operation );

    if (!operation) return E_POINTER;
    *operation = NULL;

    if (impl->shutdown_operation)
    {
        *operation = impl->shutdown_operation;
        IAsyncAction_AddRef( *operation );
        return S_OK;
    }

    if (FAILED(hr = async_action_create( (IUnknown *)&queue->IDispatcherQueue_iface, shutdown_queue_async, operation )))
        return hr;
    impl->shutdown_operation = *operation;
    IAsyncAction_AddRef( impl->shutdown_operation );
    IAsyncAction_QueryInterface( *operation, &IID_IAsyncInfo, (void **)&info );

    /* Work queued before this point runs first; the shutdown request is
     * processed by the queue thread after it. */
    EnterCriticalSection( &queue->cs );
    queue->shutdown_info = info;
    if (!queue->shutdown)
    {
        queue->shutdown = TRUE;
        if (!queue->window || !PostMessageW( queue->window, WM_WINE_DISPATCHER_QUEUE_SHUTDOWN, 0, 0 ))
            SetEvent( queue->shutdown_done );
    }
    LeaveCriticalSection( &queue->cs );

    return S_OK;
}

static const struct IDispatcherQueueControllerVtbl dispatcher_queue_controller_vtbl =
{
    dispatcher_queue_controller_QueryInterface,
    dispatcher_queue_controller_AddRef,
    dispatcher_queue_controller_Release,
    /* IInspectable methods */
    dispatcher_queue_controller_GetIids,
    dispatcher_queue_controller_GetRuntimeClassName,
    dispatcher_queue_controller_GetTrustLevel,
    /* IDispatcherQueueController methods */
    dispatcher_queue_controller_get_DispatcherQueue,
    dispatcher_queue_controller_ShutdownQueueAsync,
};

static HRESULT dispatcher_queue_controller_create( struct dispatcher_queue *queue, IDispatcherQueueController **out )
{
    struct dispatcher_queue_controller *impl;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;

    impl->IDispatcherQueueController_iface.lpVtbl = &dispatcher_queue_controller_vtbl;
    impl->ref = 1;
    impl->queue = queue;

    *out = &impl->IDispatcherQueueController_iface;
    TRACE( "created IDispatcherQueueController %p.\n", *out );
    return S_OK;
}

DEFINE_IINSPECTABLE( dispatcher_queue_controller_statics, IDispatcherQueueControllerStatics, struct dispatcher_queue_controller_statics, IActivationFactory_iface )

static HRESULT WINAPI dispatcher_queue_controller_statics_CreateOnDedicatedThread( IDispatcherQueueControllerStatics *iface, IDispatcherQueueController **result )
{
    DispatcherQueueOptions options = { sizeof(options), DQTYPE_THREAD_DEDICATED, DQTAT_COM_STA };

    TRACE( "iface %p, result %p.\n", iface, result );

    if (!result) return E_POINTER;
    return CreateDispatcherQueueController( options, result );
}

static const struct IDispatcherQueueControllerStaticsVtbl dispatcher_queue_controller_statics_vtbl =
{
    dispatcher_queue_controller_statics_QueryInterface,
    dispatcher_queue_controller_statics_AddRef,
    dispatcher_queue_controller_statics_Release,
    /* IInspectable methods */
    dispatcher_queue_controller_statics_GetIids,
    dispatcher_queue_controller_statics_GetRuntimeClassName,
    dispatcher_queue_controller_statics_GetTrustLevel,
    /* IDispatcherQueueControllerStatics methods */
    dispatcher_queue_controller_statics_CreateOnDedicatedThread,
};

static struct dispatcher_queue_controller_statics dispatcher_queue_controller_statics =
{
    {&factory_vtbl},
    {&dispatcher_queue_controller_statics_vtbl},
    1,
};

static IActivationFactory *dispatcher_queue_controller_factory = &dispatcher_queue_controller_statics.IActivationFactory_iface;

struct dispatcher_queue_statics
{
    IActivationFactory IActivationFactory_iface;
    IDispatcherQueueStatics IDispatcherQueueStatics_iface;
    LONG ref;
};

static inline struct dispatcher_queue_statics *dispatcher_queue_statics_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct dispatcher_queue_statics, IActivationFactory_iface );
}

static HRESULT WINAPI queue_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct dispatcher_queue_statics *impl = dispatcher_queue_statics_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = &impl->IActivationFactory_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IDispatcherQueueStatics ))
    {
        *out = &impl->IDispatcherQueueStatics_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI queue_factory_AddRef( IActivationFactory *iface )
{
    struct dispatcher_queue_statics *impl = dispatcher_queue_statics_from_IActivationFactory( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing ref to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI queue_factory_Release( IActivationFactory *iface )
{
    struct dispatcher_queue_statics *impl = dispatcher_queue_statics_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing ref to %lu.\n", iface, ref );
    return ref;
}

static const struct IActivationFactoryVtbl queue_factory_vtbl =
{
    queue_factory_QueryInterface,
    queue_factory_AddRef,
    queue_factory_Release,
    /* IInspectable methods */
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    /* IActivationFactory methods */
    factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( dispatcher_queue_statics, IDispatcherQueueStatics, struct dispatcher_queue_statics, IActivationFactory_iface )

static HRESULT WINAPI dispatcher_queue_statics_GetForCurrentThread( IDispatcherQueueStatics *iface, IDispatcherQueue **result )
{
    struct dispatcher_queue *queue;
    DWORD thread_id = GetCurrentThreadId();

    TRACE( "iface %p, result %p.\n", iface, result );

    if (!result) return E_POINTER;
    *result = NULL;

    EnterCriticalSection( &dispatcher_queues_cs );
    LIST_FOR_EACH_ENTRY( queue, &dispatcher_queues, struct dispatcher_queue, entry )
    {
        if (queue->thread_id != thread_id) continue;
        *result = &queue->IDispatcherQueue_iface;
        IDispatcherQueue_AddRef( *result );
        break;
    }
    LeaveCriticalSection( &dispatcher_queues_cs );

    return S_OK;
}

static const struct IDispatcherQueueStaticsVtbl dispatcher_queue_statics_vtbl =
{
    dispatcher_queue_statics_QueryInterface,
    dispatcher_queue_statics_AddRef,
    dispatcher_queue_statics_Release,
    /* IInspectable methods */
    dispatcher_queue_statics_GetIids,
    dispatcher_queue_statics_GetRuntimeClassName,
    dispatcher_queue_statics_GetTrustLevel,
    /* IDispatcherQueueStatics methods */
    dispatcher_queue_statics_GetForCurrentThread,
};

static struct dispatcher_queue_statics dispatcher_queue_statics =
{
    {&queue_factory_vtbl},
    {&dispatcher_queue_statics_vtbl},
    1,
};

static IActivationFactory *dispatcher_queue_factory = &dispatcher_queue_statics.IActivationFactory_iface;

HRESULT WINAPI DllGetActivationFactory( HSTRING classid, IActivationFactory **factory )
{
    const WCHAR *name = WindowsGetStringRawBuffer( classid, NULL );

    TRACE( "classid %s, factory %p.\n", debugstr_hstring( classid ), factory );

    *factory = NULL;

    if (!wcscmp( name, RuntimeClass_Windows_System_DispatcherQueueController ))
        IActivationFactory_QueryInterface( dispatcher_queue_controller_factory, &IID_IActivationFactory, (void **)factory );
    if (!wcscmp( name, RuntimeClass_Windows_System_DispatcherQueue ))
        IActivationFactory_QueryInterface( dispatcher_queue_factory, &IID_IActivationFactory, (void **)factory );

    if (*factory) return S_OK;
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI CreateDispatcherQueueController( DispatcherQueueOptions options, PDISPATCHERQUEUECONTROLLER *queue_controller )
{
    struct dispatcher_queue *queue;
    HRESULT hr;

    TRACE( "options.dwSize = %lu, options.threadType = %d, options.apartmentType = %d, queue_controller %p.\n",
           options.dwSize, options.threadType, options.apartmentType, queue_controller );

    if (!queue_controller) return E_POINTER;
    if (options.dwSize != sizeof( DispatcherQueueOptions )) return E_INVALIDARG;
    if (options.threadType != DQTYPE_THREAD_DEDICATED && options.threadType != DQTYPE_THREAD_CURRENT) return E_INVALIDARG;

    if (options.threadType == DQTYPE_THREAD_CURRENT) hr = dispatcher_queue_create( FALSE, &queue );
    else hr = dispatcher_queue_create_dedicated( options.apartmentType, &queue );
    if (FAILED(hr)) return hr;

    if (FAILED(hr = dispatcher_queue_controller_create( queue, queue_controller )))
        IDispatcherQueue_Release( &queue->IDispatcherQueue_iface );
    return hr;
}
