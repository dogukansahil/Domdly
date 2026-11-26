/*
 * Domdly - a lightweight Markdown reader and editor
 * Copyright (C) 2026 Dogukan Sahil and Domdly contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "olecb.h"

#include <objbase.h>
#include <ole2.h>
#include <richedit.h>
#include <richole.h>

/* {00020D03-0000-0000-C000-000000000046} */
static const IID iid_callback = {0x00020D03, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

typedef struct ole_callback {
    IRichEditOleCallback iface;
    LONG refs;
} ole_callback;

static HRESULT STDMETHODCALLTYPE cb_query_interface(IRichEditOleCallback *self, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &iid_callback)) {
        *out = self;
        self->lpVtbl->AddRef(self);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE cb_add_ref(IRichEditOleCallback *self)
{
    return (ULONG)InterlockedIncrement(&((ole_callback *)self)->refs);
}

static ULONG STDMETHODCALLTYPE cb_release(IRichEditOleCallback *self)
{
    ole_callback *cb = (ole_callback *)self;
    LONG r = InterlockedDecrement(&cb->refs);
    if (r == 0) CoTaskMemFree(cb);
    return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE cb_get_new_storage(IRichEditOleCallback *self, LPSTORAGE *stg)
{
    (void)self;
    ILockBytes *bytes = NULL;
    HRESULT hr = CreateILockBytesOnHGlobal(NULL, TRUE, &bytes);
    if (FAILED(hr)) return hr;
    hr = StgCreateDocfileOnILockBytes(bytes, STGM_SHARE_EXCLUSIVE | STGM_READWRITE | STGM_CREATE, 0, stg);
    bytes->lpVtbl->Release(bytes);
    return hr;
}

static HRESULT STDMETHODCALLTYPE cb_get_in_place_context(IRichEditOleCallback *self, LPOLEINPLACEFRAME *frame,
                                                         LPOLEINPLACEUIWINDOW *doc, LPOLEINPLACEFRAMEINFO info)
{
    (void)self;
    (void)frame;
    (void)doc;
    (void)info;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE cb_show_container_ui(IRichEditOleCallback *self, BOOL show)
{
    (void)self;
    (void)show;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cb_query_insert_object(IRichEditOleCallback *self, LPCLSID clsid, LPSTORAGE stg, LONG cp)
{
    (void)self;
    (void)clsid;
    (void)stg;
    (void)cp;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cb_delete_object(IRichEditOleCallback *self, LPOLEOBJECT obj)
{
    (void)self;
    (void)obj;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cb_query_accept_data(IRichEditOleCallback *self, LPDATAOBJECT data, CLIPFORMAT *fmt,
                                                      DWORD reco, BOOL really, HGLOBAL pict)
{
    (void)self;
    (void)data;
    (void)fmt;
    (void)reco;
    (void)really;
    (void)pict;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cb_context_sensitive_help(IRichEditOleCallback *self, BOOL enter)
{
    (void)self;
    (void)enter;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cb_get_clipboard_data(IRichEditOleCallback *self, CHARRANGE *cr, DWORD reco,
                                                       LPDATAOBJECT *data)
{
    (void)self;
    (void)cr;
    (void)reco;
    (void)data;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE cb_get_drag_drop_effect(IRichEditOleCallback *self, BOOL drag, DWORD keys, LPDWORD effect)
{
    (void)self;
    (void)drag;
    (void)keys;
    if (effect) *effect = DROPEFFECT_NONE;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cb_get_context_menu(IRichEditOleCallback *self, WORD type, LPOLEOBJECT obj,
                                                     CHARRANGE *cr, HMENU *menu)
{
    (void)self;
    (void)type;
    (void)obj;
    (void)cr;
    if (menu) *menu = NULL;
    return E_NOTIMPL;
}

static IRichEditOleCallbackVtbl cb_vtbl = {
    cb_query_interface,      cb_add_ref,          cb_release,
    cb_get_new_storage,      cb_get_in_place_context, cb_show_container_ui,
    cb_query_insert_object,  cb_delete_object,    cb_query_accept_data,
    cb_context_sensitive_help, cb_get_clipboard_data, cb_get_drag_drop_effect,
    cb_get_context_menu,
};

int olecb_attach(HWND richedit)
{
    ole_callback *cb = (ole_callback *)CoTaskMemAlloc(sizeof *cb);
    if (!cb) return 0;
    cb->iface.lpVtbl = &cb_vtbl;
    cb->refs = 1;
    BOOL ok = (BOOL)SendMessageW(richedit, EM_SETOLECALLBACK, 0, (LPARAM)&cb->iface);
    cb->iface.lpVtbl->Release(&cb->iface); /* the control holds its own reference */
    return ok != 0;
}
