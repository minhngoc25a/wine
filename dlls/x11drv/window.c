/*
 * Window related functions
 *
 * Copyright 1993, 1994, 1995, 1996, 2001 Alexandre Julliard
 * Copyright 1993 David Metcalfe
 * Copyright 1995, 1996 Alex Korobka
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"

#include <stdlib.h>

#include "ts_xlib.h"
#include "ts_xutil.h"

#include "winbase.h"
#include "wingdi.h"
#include "winreg.h"
#include "winuser.h"
#include "wine/unicode.h"

#include "wine/debug.h"
#include "x11drv.h"
#include "win.h"
#include "winpos.h"
#include "dce.h"
#include "hook.h"
#include "mwm.h"

WINE_DEFAULT_DEBUG_CHANNEL(x11drv);

extern Pixmap X11DRV_BITMAP_Pixmap( HBITMAP );

#define HAS_DLGFRAME(style,exStyle) \
    (((exStyle) & WS_EX_DLGMODALFRAME) || \
     (((style) & WS_DLGFRAME) && !((style) & WS_THICKFRAME)))

/* X context to associate a hwnd to an X window */
XContext winContext = 0;

Atom wmProtocols = None;
Atom wmDeleteWindow = None;
Atom wmTakeFocus = None;
Atom dndProtocol = None;
Atom dndSelection = None;
Atom wmChangeState = None;
Atom mwmHints = None;
Atom kwmDockWindow = None;
Atom _kde_net_wm_system_tray_window_for = None; /* KDE 2 Final */

static LPCSTR whole_window_atom;
static LPCSTR client_window_atom;
static LPCSTR icon_window_atom;

/***********************************************************************
 *		is_window_managed
 *
 * Check if a given window should be managed
 */
inline static BOOL is_window_managed( WND *win )
{
    if (!managed_mode) return FALSE;
    /* tray window is always managed */
    if (win->dwExStyle & WS_EX_TRAYWINDOW) return TRUE;
    /* child windows are not managed */
    if (win->dwStyle & WS_CHILD) return FALSE;
    /* tool windows are not managed */
    if (win->dwExStyle & WS_EX_TOOLWINDOW) return FALSE;
    /* windows with caption or thick frame are managed */
    if ((win->dwStyle & WS_CAPTION) == WS_CAPTION) return TRUE;
    if (win->dwStyle & WS_THICKFRAME) return TRUE;
    /* default: not managed */
    return FALSE;
}


/***********************************************************************
 *		is_window_top_level
 *
 * Check if a given window is a top level X11 window
 */
inline static BOOL is_window_top_level( WND *win )
{
    return (root_window == DefaultRootWindow(gdi_display) && win->parent == GetDesktopWindow());
}


/***********************************************************************
 *		is_client_window_mapped
 *
 * Check if the X client window should be mapped
 */
inline static BOOL is_client_window_mapped( WND *win )
{
    struct x11drv_win_data *data = win->pDriverData;
    return !(win->dwStyle & WS_MINIMIZE) && !IsRectEmpty( &data->client_rect );
}


/***********************************************************************
 *              get_window_attributes
 *
 * Fill the window attributes structure for an X window.
 * Returned cursor must be freed by caller.
 */
static int get_window_attributes( Display *display, WND *win, XSetWindowAttributes *attr )
{
    BOOL is_top_level = is_window_top_level( win );
    BOOL managed = is_top_level && is_window_managed( win );

    if (managed) WIN_SetExStyle( win->hwndSelf, win->dwExStyle | WS_EX_MANAGED );
    else WIN_SetExStyle( win->hwndSelf, win->dwExStyle & ~WS_EX_MANAGED );

    attr->override_redirect = !managed;
    attr->colormap          = X11DRV_PALETTE_PaletteXColormap;
    attr->save_under        = ((win->clsStyle & CS_SAVEBITS) != 0);
    attr->cursor            = None;
    attr->event_mask        = (ExposureMask | KeyPressMask | KeyReleaseMask | PointerMotionMask |
                               ButtonPressMask | ButtonReleaseMask);
    if (is_window_top_level( win ))
    {
        attr->event_mask |= StructureNotifyMask | FocusChangeMask | KeymapStateMask;
        attr->cursor = X11DRV_GetCursor( display, GlobalLock16(GetCursor()) );
    }
    return (CWOverrideRedirect | CWSaveUnder | CWEventMask | CWColormap | CWCursor);
}


/***********************************************************************
 *              sync_window_style
 *
 * Change the X window attributes when the window style has changed.
 */
static void sync_window_style( Display *display, WND *win )
{
    XSetWindowAttributes attr;
    int mask;

    wine_tsx11_lock();
    mask = get_window_attributes( display, win, &attr );
    XChangeWindowAttributes( display, get_whole_window(win), mask, &attr );
    if (attr.cursor) XFreeCursor( display, attr.cursor );
    wine_tsx11_unlock();
}


/***********************************************************************
 *              get_window_changes
 *
 * fill the window changes structure
 */
static int get_window_changes( XWindowChanges *changes, const RECT *old, const RECT *new )
{
    int mask = 0;

    if (old->right - old->left != new->right - new->left )
    {
        if (!(changes->width = new->right - new->left)) changes->width = 1;
        mask |= CWWidth;
    }
    if (old->bottom - old->top != new->bottom - new->top)
    {
        if (!(changes->height = new->bottom - new->top)) changes->height = 1;
        mask |= CWHeight;
    }
    if (old->left != new->left)
    {
        changes->x = new->left;
        mask |= CWX;
    }
    if (old->top != new->top)
    {
        changes->y = new->top;
        mask |= CWY;
    }
    return mask;
}


/***********************************************************************
 *              create_icon_window
 */
static Window create_icon_window( Display *display, WND *win )
{
    struct x11drv_win_data *data = win->pDriverData;
    XSetWindowAttributes attr;

    attr.event_mask = (ExposureMask | KeyPressMask | KeyReleaseMask | PointerMotionMask |
                       ButtonPressMask | ButtonReleaseMask);
    attr.bit_gravity = NorthWestGravity;
    attr.backing_store = NotUseful/*WhenMapped*/;
    attr.colormap      = X11DRV_PALETTE_PaletteXColormap; /* Needed due to our visual */

    wine_tsx11_lock();
    data->icon_window = XCreateWindow( display, root_window, 0, 0,
                                       GetSystemMetrics( SM_CXICON ),
                                       GetSystemMetrics( SM_CYICON ),
                                       0, screen_depth,
                                       InputOutput, visual,
                                       CWEventMask | CWBitGravity | CWBackingStore | CWColormap, &attr );
    XSaveContext( display, data->icon_window, winContext, (char *)win->hwndSelf );
    wine_tsx11_unlock();

    TRACE( "created %lx\n", data->icon_window );
    SetPropA( win->hwndSelf, icon_window_atom, (HANDLE)data->icon_window );
    return data->icon_window;
}



/***********************************************************************
 *              destroy_icon_window
 */
inline static void destroy_icon_window( Display *display, WND *win )
{
    struct x11drv_win_data *data = win->pDriverData;

    if (!data->icon_window) return;
    wine_tsx11_lock();
    XDeleteContext( display, data->icon_window, winContext );
    XDestroyWindow( display, data->icon_window );
    data->icon_window = 0;
    wine_tsx11_unlock();
    RemovePropA( win->hwndSelf, icon_window_atom );
}


/***********************************************************************
 *              set_icon_hints
 *
 * Set the icon wm hints
 */
static void set_icon_hints( Display *display, WND *wndPtr, XWMHints *hints )
{
    X11DRV_WND_DATA *data = wndPtr->pDriverData;
    HICON hIcon = GetClassLongA( wndPtr->hwndSelf, GCL_HICON );

    if (data->hWMIconBitmap) DeleteObject( data->hWMIconBitmap );
    if (data->hWMIconMask) DeleteObject( data->hWMIconMask);
    data->hWMIconBitmap = 0;
    data->hWMIconMask = 0;

    if (!(wndPtr->dwExStyle & WS_EX_MANAGED))
    {
        destroy_icon_window( display, wndPtr );
        hints->flags &= ~(IconPixmapHint | IconMaskHint | IconWindowHint);
    }
    else if (!hIcon)
    {
        if (!data->icon_window) create_icon_window( display, wndPtr );
        hints->icon_window = data->icon_window;
        hints->flags = (hints->flags & ~(IconPixmapHint | IconMaskHint)) | IconWindowHint;
    }
    else
    {
        HBITMAP hbmOrig;
        RECT rcMask;
        BITMAP bmMask;
        ICONINFO ii;
        HDC hDC;

        GetIconInfo(hIcon, &ii);

        X11DRV_CreateBitmap(ii.hbmMask);
        X11DRV_CreateBitmap(ii.hbmColor);

        GetObjectA(ii.hbmMask, sizeof(bmMask), &bmMask);
        rcMask.top    = 0;
        rcMask.left   = 0;
        rcMask.right  = bmMask.bmWidth;
        rcMask.bottom = bmMask.bmHeight;

        hDC = CreateCompatibleDC(0);
        hbmOrig = SelectObject(hDC, ii.hbmMask);
        InvertRect(hDC, &rcMask);
        SelectObject(hDC, hbmOrig);
        DeleteDC(hDC);

        data->hWMIconBitmap = ii.hbmColor;
        data->hWMIconMask = ii.hbmMask;

        hints->icon_pixmap = X11DRV_BITMAP_Pixmap(data->hWMIconBitmap);
        hints->icon_mask = X11DRV_BITMAP_Pixmap(data->hWMIconMask);
        destroy_icon_window( display, wndPtr );
        hints->flags = (hints->flags & ~IconWindowHint) | IconPixmapHint | IconMaskHint;
    }
}


/***********************************************************************
 *              set_size_hints
 *
 * set the window size hints
 */
static void set_size_hints( Display *display, WND *win )
{
    XSizeHints* size_hints;
    struct x11drv_win_data *data = win->pDriverData;

    if ((size_hints = XAllocSizeHints()))
    {
        size_hints->win_gravity = StaticGravity;
        size_hints->x = data->whole_rect.left;
        size_hints->y = data->whole_rect.top;
        size_hints->flags = PWinGravity | PPosition;

        if (HAS_DLGFRAME( win->dwStyle, win->dwExStyle ))
        {
            size_hints->max_width = data->whole_rect.right - data->whole_rect.left;
            size_hints->max_height = data->whole_rect.bottom - data->whole_rect.top;
            size_hints->min_width = size_hints->max_width;
            size_hints->min_height = size_hints->max_height;
            size_hints->flags |= PMinSize | PMaxSize;
        }
        XSetWMNormalHints( display, data->whole_window, size_hints );
        XFree( size_hints );
    }
}


/***********************************************************************
 *              set_wm_hints
 *
 * Set the window manager hints for a newly-created window
 */
static void set_wm_hints( Display *display, WND *win )
{
    struct x11drv_win_data *data = win->pDriverData;
    Window group_leader;
    XClassHint *class_hints;
    XWMHints* wm_hints;
    Atom protocols[2];
    int i;

    wine_tsx11_lock();

    /* wm protocols */
    i = 0;
    protocols[i++] = wmDeleteWindow;
    if (wmTakeFocus) protocols[i++] = wmTakeFocus;
    XSetWMProtocols( display, data->whole_window, protocols, i );

    /* class hints */
    if ((class_hints = XAllocClassHint()))
    {
        class_hints->res_name = "wine";
        class_hints->res_class = "Wine";
        XSetClassHint( display, data->whole_window, class_hints );
        XFree( class_hints );
    }

    /* transient for hint */
    if (win->owner)
    {
        Window owner_win = X11DRV_get_whole_window( win->owner );
        XSetTransientForHint( display, data->whole_window, owner_win );
        group_leader = owner_win;
    }
    else group_leader = data->whole_window;

    /* size hints */
    set_size_hints( display, win );

    /* systray properties (KDE only for now) */
    if (win->dwExStyle & WS_EX_TRAYWINDOW)
    {
        int val = 1;
        if (kwmDockWindow != None)
            XChangeProperty( display, data->whole_window, kwmDockWindow, kwmDockWindow,
                             32, PropModeReplace, (char*)&val, 1 );
        if (_kde_net_wm_system_tray_window_for != None)
            XChangeProperty( display, data->whole_window, _kde_net_wm_system_tray_window_for,
                             XA_WINDOW, 32, PropModeReplace, (char*)&data->whole_window, 1 );
    }

    if (mwmHints != None)
    {
        MwmHints mwm_hints;
        mwm_hints.flags = MWM_HINTS_FUNCTIONS | MWM_HINTS_DECORATIONS;
        mwm_hints.functions = 0;
        if ((win->dwStyle & WS_CAPTION) == WS_CAPTION) mwm_hints.functions |= MWM_FUNC_MOVE;
        if (win->dwStyle & WS_THICKFRAME) mwm_hints.functions |= MWM_FUNC_MOVE | MWM_FUNC_RESIZE;
        if (win->dwStyle & WS_MINIMIZE)   mwm_hints.functions |= MWM_FUNC_MINIMIZE;
        if (win->dwStyle & WS_MAXIMIZE)   mwm_hints.functions |= MWM_FUNC_MAXIMIZE;
        if (win->dwStyle & WS_SYSMENU)    mwm_hints.functions |= MWM_FUNC_CLOSE;
        mwm_hints.decorations = 0;
        if ((win->dwStyle & WS_CAPTION) == WS_CAPTION) mwm_hints.decorations |= MWM_DECOR_TITLE;
        if (win->dwExStyle & WS_EX_DLGMODALFRAME) mwm_hints.decorations |= MWM_DECOR_BORDER;
        else if (win->dwStyle & WS_THICKFRAME) mwm_hints.decorations |= MWM_DECOR_BORDER | MWM_DECOR_RESIZEH;
        else if ((win->dwStyle & (WS_DLGFRAME|WS_BORDER)) == WS_DLGFRAME) mwm_hints.decorations |= MWM_DECOR_BORDER;
        else if (win->dwStyle & WS_BORDER) mwm_hints.decorations |= MWM_DECOR_BORDER;
        else if (!(win->dwStyle & (WS_CHILD|WS_POPUP))) mwm_hints.decorations |= MWM_DECOR_BORDER;
        if (win->dwStyle & WS_SYSMENU)  mwm_hints.decorations |= MWM_DECOR_MENU;
        if (win->dwStyle & WS_MINIMIZE) mwm_hints.decorations |= MWM_DECOR_MINIMIZE;
        if (win->dwStyle & WS_MAXIMIZE) mwm_hints.decorations |= MWM_DECOR_MAXIMIZE;

        XChangeProperty( display, data->whole_window, mwmHints, mwmHints, 32,
                         PropModeReplace, (char*)&mwm_hints, sizeof(mwm_hints)/sizeof(long) );
    }

    wine_tsx11_unlock();

    /* wm hints */
    if ((wm_hints = TSXAllocWMHints()))
    {
        wm_hints->flags = InputHint | StateHint | WindowGroupHint;
        /* use globally active model if take focus is supported,
         * passive model otherwise (cf. ICCCM) */
        wm_hints->input = !wmTakeFocus;

        set_icon_hints( display, win, wm_hints );

        wm_hints->initial_state = (win->dwStyle & WS_MINIMIZE) ? IconicState : NormalState;
        wm_hints->window_group = group_leader;

        wine_tsx11_lock();
        XSetWMHints( display, data->whole_window, wm_hints );
        XFree(wm_hints);
        wine_tsx11_unlock();
    }
}


/***********************************************************************
 *              X11DRV_set_iconic_state
 *
 * Set the X11 iconic state according to the window style.
 */
void X11DRV_set_iconic_state( WND *win )
{
    Display *display = thread_display();
    struct x11drv_win_data *data = win->pDriverData;
    XWMHints* wm_hints;
    BOOL iconic = IsIconic( win->hwndSelf );

    wine_tsx11_lock();

    if (iconic) XUnmapWindow( display, data->client_window );
    else if (is_client_window_mapped( win )) XMapWindow( display, data->client_window );

    if (!(wm_hints = XGetWMHints( display, data->whole_window ))) wm_hints = XAllocWMHints();
    wm_hints->flags |= StateHint | IconPositionHint;
    wm_hints->initial_state = iconic ? IconicState : NormalState;
    wm_hints->icon_x = win->rectWindow.left;
    wm_hints->icon_y = win->rectWindow.top;
    XSetWMHints( display, data->whole_window, wm_hints );

    if (win->dwStyle & WS_VISIBLE)
    {
        if (iconic)
            XIconifyWindow( display, data->whole_window, DefaultScreen(display) );
        else
            if (!IsRectEmpty( &win->rectWindow )) XMapWindow( display, data->whole_window );
    }

    XFree(wm_hints);
    wine_tsx11_unlock();
}


/***********************************************************************
 *		X11DRV_window_to_X_rect
 *
 * Convert a rect from client to X window coordinates
 */
void X11DRV_window_to_X_rect( WND *win, RECT *rect )
{
    RECT rc;

    if (!(win->dwExStyle & WS_EX_MANAGED)) return;
    if (IsRectEmpty( rect )) return;

    rc.top = rc.bottom = rc.left = rc.right = 0;

    AdjustWindowRectEx( &rc, win->dwStyle & ~(WS_HSCROLL|WS_VSCROLL), FALSE, win->dwExStyle );

    rect->left   -= rc.left;
    rect->right  -= rc.right;
    rect->top    -= rc.top;
    rect->bottom -= rc.bottom;
    if (rect->top >= rect->bottom) rect->bottom = rect->top + 1;
    if (rect->left >= rect->right) rect->right = rect->left + 1;
}


/***********************************************************************
 *		X11DRV_X_to_window_rect
 *
 * Opposite of X11DRV_window_to_X_rect
 */
void X11DRV_X_to_window_rect( WND *win, RECT *rect )
{
    if (!(win->dwExStyle & WS_EX_MANAGED)) return;
    if (IsRectEmpty( rect )) return;

    AdjustWindowRectEx( rect, win->dwStyle & ~(WS_HSCROLL|WS_VSCROLL), FALSE, win->dwExStyle );

    if (rect->top >= rect->bottom) rect->bottom = rect->top + 1;
    if (rect->left >= rect->right) rect->right = rect->left + 1;
}


/***********************************************************************
 *		X11DRV_sync_whole_window_position
 *
 * Synchronize the X whole window position with the Windows one
 */
int X11DRV_sync_whole_window_position( Display *display, WND *win, int zorder )
{
    XWindowChanges changes;
    int mask;
    struct x11drv_win_data *data = win->pDriverData;
    RECT whole_rect = win->rectWindow;

    X11DRV_window_to_X_rect( win, &whole_rect );
    mask = get_window_changes( &changes, &data->whole_rect, &whole_rect );

    if (zorder)
    {
        /* find window that this one must be after */
        HWND prev = GetWindow( win->hwndSelf, GW_HWNDPREV );
        while (prev && !(GetWindowLongW( prev, GWL_STYLE ) & WS_VISIBLE))
            prev = GetWindow( prev, GW_HWNDPREV );
        if (!prev)  /* top child */
        {
            changes.stack_mode = Above;
            mask |= CWStackMode;
        }
        else
        {
            changes.stack_mode = Below;
            changes.sibling = X11DRV_get_whole_window(prev);
            mask |= CWStackMode | CWSibling;
        }
    }

    data->whole_rect = whole_rect;

    if (mask)
    {
        TRACE( "setting win %lx pos %d,%d,%dx%d after %lx changes=%x\n",
               data->whole_window, whole_rect.left, whole_rect.top,
               whole_rect.right - whole_rect.left, whole_rect.bottom - whole_rect.top,
               changes.sibling, mask );
        wine_tsx11_lock();
        XSync( gdi_display, False );  /* flush graphics operations before moving the window */
        if (is_window_top_level( win ))
        {
            if (mask & (CWWidth|CWHeight)) set_size_hints( display, win );
            XReconfigureWMWindow( display, data->whole_window,
                                  DefaultScreen(display), mask, &changes );
        }
        else XConfigureWindow( display, data->whole_window, mask, &changes );
        wine_tsx11_unlock();
    }
    return mask;
}


/***********************************************************************
 *		X11DRV_sync_client_window_position
 *
 * Synchronize the X client window position with the Windows one
 */
int X11DRV_sync_client_window_position( Display *display, WND *win )
{
    XWindowChanges changes;
    int mask;
    struct x11drv_win_data *data = win->pDriverData;
    RECT client_rect = win->rectClient;

    OffsetRect( &client_rect, -data->whole_rect.left, -data->whole_rect.top );

    if ((mask = get_window_changes( &changes, &data->client_rect, &client_rect )))
    {
        BOOL was_mapped = is_client_window_mapped( win );

        TRACE( "setting win %lx pos %d,%d,%dx%d (was %d,%d,%dx%d) after %lx changes=%x\n",
               data->client_window, client_rect.left, client_rect.top,
               client_rect.right - client_rect.left, client_rect.bottom - client_rect.top,
               data->client_rect.left, data->client_rect.top,
               data->client_rect.right - data->client_rect.left,
               data->client_rect.bottom - data->client_rect.top,
               changes.sibling, mask );
        data->client_rect = client_rect;
        wine_tsx11_lock();
        XSync( gdi_display, False );  /* flush graphics operations before moving the window */
        if (was_mapped && !is_client_window_mapped( win ))
            XUnmapWindow( display, data->client_window );
        XConfigureWindow( display, data->client_window, mask, &changes );
        if (!was_mapped && is_client_window_mapped( win ))
            XMapWindow( display, data->client_window );
        wine_tsx11_unlock();
    }
    return mask;
}


/***********************************************************************
 *		X11DRV_register_window
 *
 * Associate an X window to a HWND.
 */
void X11DRV_register_window( Display *display, HWND hwnd, struct x11drv_win_data *data )
{
    wine_tsx11_lock();
    XSaveContext( display, data->whole_window, winContext, (char *)hwnd );
    XSaveContext( display, data->client_window, winContext, (char *)hwnd );
    wine_tsx11_unlock();
}


/**********************************************************************
 *		create_desktop
 */
static void create_desktop( Display *display, WND *wndPtr, CREATESTRUCTA *cs )
{
    X11DRV_WND_DATA *data = wndPtr->pDriverData;

    wine_tsx11_lock();
    winContext     = XUniqueContext();
    wmProtocols    = XInternAtom( display, "WM_PROTOCOLS", False );
    wmDeleteWindow = XInternAtom( display, "WM_DELETE_WINDOW", False );
/*    wmTakeFocus    = XInternAtom( display, "WM_TAKE_FOCUS", False );*/
    wmTakeFocus = 0;  /* not yet */
    dndProtocol = XInternAtom( display, "DndProtocol" , False );
    dndSelection = XInternAtom( display, "DndSelection" , False );
    wmChangeState = XInternAtom( display, "WM_CHANGE_STATE", False );
    mwmHints = XInternAtom( display, _XA_MWM_HINTS, False );
    kwmDockWindow = XInternAtom( display, "KWM_DOCKWINDOW", False );
    _kde_net_wm_system_tray_window_for = XInternAtom( display, "_KDE_NET_WM_SYSTEM_TRAY_WINDOW_FOR", False );
    wine_tsx11_unlock();

    whole_window_atom  = MAKEINTATOMA( GlobalAddAtomA( "__wine_x11_whole_window" ));
    client_window_atom = MAKEINTATOMA( GlobalAddAtomA( "__wine_x11_client_window" ));
    icon_window_atom   = MAKEINTATOMA( GlobalAddAtomA( "__wine_x11_icon_window" ));

    data->whole_window = data->client_window = root_window;
    data->whole_rect = data->client_rect = wndPtr->rectWindow;

    SetPropA( wndPtr->hwndSelf, whole_window_atom, (HANDLE)root_window );
    SetPropA( wndPtr->hwndSelf, client_window_atom, (HANDLE)root_window );
    SetPropA( wndPtr->hwndSelf, "__wine_x11_visual_id", (HANDLE)XVisualIDFromVisual(visual) );

    SendMessageW( wndPtr->hwndSelf, WM_NCCREATE, 0, (LPARAM)cs );
    if (root_window != DefaultRootWindow(display)) X11DRV_create_desktop_thread();
}


/**********************************************************************
 *		create_whole_window
 *
 * Create the whole X window for a given window
 */
static Window create_whole_window( Display *display, WND *win )
{
    struct x11drv_win_data *data = win->pDriverData;
    int cx, cy, mask;
    XSetWindowAttributes attr;
    Window parent;
    RECT rect;
    BOOL is_top_level = is_window_top_level( win );

    rect = win->rectWindow;
    X11DRV_window_to_X_rect( win, &rect );

    if (!(cx = rect.right - rect.left)) cx = 1;
    if (!(cy = rect.bottom - rect.top)) cy = 1;

    parent = X11DRV_get_client_window( win->parent );

    wine_tsx11_lock();

    mask = get_window_attributes( display, win, &attr );

    /* set the attributes that don't change over the lifetime of the window */
    attr.bit_gravity       = ForgetGravity;
    attr.win_gravity       = NorthWestGravity;
    attr.backing_store     = NotUseful/*WhenMapped*/;
    mask |= CWBitGravity | CWWinGravity | CWBackingStore;

    data->whole_rect = rect;
    data->whole_window = XCreateWindow( display, parent, rect.left, rect.top, cx, cy,
                                        0, screen_depth, InputOutput, visual,
                                        mask, &attr );
    if (attr.cursor) XFreeCursor( display, attr.cursor );

    if (!data->whole_window)
    {
        wine_tsx11_unlock();
        return 0;
    }

    /* non-maximized child must be at bottom of Z order */
    if ((win->dwStyle & (WS_CHILD|WS_MAXIMIZE)) == WS_CHILD)
    {
        XWindowChanges changes;
        changes.stack_mode = Below;
        XConfigureWindow( display, data->whole_window, CWStackMode, &changes );
    }

    wine_tsx11_unlock();

    if (is_top_level) set_wm_hints( display, win );

    return data->whole_window;
}


/**********************************************************************
 *		create_client_window
 *
 * Create the client window for a given window
 */
static Window create_client_window( Display *display, WND *win )
{
    struct x11drv_win_data *data = win->pDriverData;
    RECT rect = data->whole_rect;
    XSetWindowAttributes attr;

    OffsetRect( &rect, -data->whole_rect.left, -data->whole_rect.top );
    data->client_rect = rect;

    attr.event_mask = (ExposureMask | KeyPressMask | KeyReleaseMask | PointerMotionMask |
                       ButtonPressMask | ButtonReleaseMask);
    attr.bit_gravity = (win->clsStyle & (CS_VREDRAW | CS_HREDRAW)) ?
                       ForgetGravity : NorthWestGravity;
    attr.backing_store = NotUseful/*WhenMapped*/;

    wine_tsx11_lock();
    data->client_window = XCreateWindow( display, data->whole_window, 0, 0,
                                         max( rect.right - rect.left, 1 ),
                                         max( rect.bottom - rect.top, 1 ),
                                         0, screen_depth,
                                         InputOutput, visual,
                                         CWEventMask | CWBitGravity | CWBackingStore, &attr );
    if (data->client_window && is_client_window_mapped( win ))
        XMapWindow( display, data->client_window );
    wine_tsx11_unlock();
    return data->client_window;
}


/*****************************************************************
 *		SetWindowText   (X11DRV.@)
 */
BOOL X11DRV_SetWindowText( HWND hwnd, LPCWSTR text )
{
    Display *display = thread_display();
    UINT count;
    char *buffer;
    char *utf8_buffer;
    static UINT text_cp = (UINT)-1;
    Window win;

    if ((win = X11DRV_get_whole_window( hwnd )))
    {
        if (text_cp == (UINT)-1)
        {
	    HKEY hkey;
	    /* default value */
	    text_cp = CP_ACP;
	    if(!RegOpenKeyA(HKEY_LOCAL_MACHINE, "Software\\Wine\\Wine\\Config\\x11drv", &hkey))
	    {
		char buffer[20];
		DWORD type, count = sizeof(buffer);
		if(!RegQueryValueExA(hkey, "TextCP", 0, &type, buffer, &count))
		    text_cp = atoi(buffer);
		RegCloseKey(hkey);
	    }
            TRACE("text_cp = %u\n", text_cp);
        }

        /* allocate new buffer for window text */
        count = WideCharToMultiByte(text_cp, 0, text, -1, NULL, 0, NULL, NULL);
        if (!(buffer = HeapAlloc( GetProcessHeap(), 0, count )))
        {
            ERR("Not enough memory for window text\n");
            return FALSE;
        }
        WideCharToMultiByte(text_cp, 0, text, -1, buffer, count, NULL, NULL);

        count = WideCharToMultiByte(CP_UTF8, 0, text, strlenW(text), NULL, 0, NULL, NULL);
        if (!(utf8_buffer = HeapAlloc( GetProcessHeap(), 0, count )))
        {
            ERR("Not enough memory for window text in UTF-8\n");
            return FALSE;
        }
        WideCharToMultiByte(CP_UTF8, 0, text, strlenW(text), utf8_buffer, count, NULL, NULL);

        wine_tsx11_lock();
        XStoreName( display, win, buffer );
        XSetIconName( display, win, buffer );
        /*
        Implements a NET_WM UTF-8 title. It should be without a trailing \0,
        according to the standard
        ( http://www.pps.jussieu.fr/~jch/software/UTF8_STRING/UTF8_STRING.text ).
        */
        XChangeProperty( display, win,
            XInternAtom(display, "_NET_WM_NAME", False),
            XInternAtom(display, "UTF8_STRING", False),
            8, PropModeReplace, (unsigned char *) utf8_buffer,
            count);
        wine_tsx11_unlock();

        HeapFree( GetProcessHeap(), 0, utf8_buffer );
        HeapFree( GetProcessHeap(), 0, buffer );
    }
    return TRUE;
}


/***********************************************************************
 *		DestroyWindow   (X11DRV.@)
 */
BOOL X11DRV_DestroyWindow( HWND hwnd )
{
    Display *display = thread_display();
    WND *wndPtr = WIN_GetPtr( hwnd );
    X11DRV_WND_DATA *data = wndPtr->pDriverData;

    if (!data) goto done;

    if (data->whole_window)
    {
        TRACE( "win %x xwin %lx/%lx\n", hwnd, data->whole_window, data->client_window );
        wine_tsx11_lock();
        XSync( gdi_display, False );  /* flush any reference to this drawable in GDI queue */
        XDeleteContext( display, data->whole_window, winContext );
        XDeleteContext( display, data->client_window, winContext );
        XDestroyWindow( display, data->whole_window );  /* this destroys client too */
        destroy_icon_window( display, wndPtr );
        wine_tsx11_unlock();
    }

    if (data->hWMIconBitmap) DeleteObject( data->hWMIconBitmap );
    if (data->hWMIconMask) DeleteObject( data->hWMIconMask);
    HeapFree( GetProcessHeap(), 0, data );
    wndPtr->pDriverData = NULL;
 done:
    WIN_ReleasePtr( wndPtr );
    return TRUE;
}


/**********************************************************************
 *		CreateWindow   (X11DRV.@)
 */
BOOL X11DRV_CreateWindow( HWND hwnd, CREATESTRUCTA *cs, BOOL unicode )
{
    HWND hwndLinkAfter;
    Display *display = thread_display();
    WND *wndPtr;
    struct x11drv_win_data *data;
    RECT rect;
    BOOL ret = FALSE;

    if (cs->cx > 65535)
    {
        ERR( "invalid window width %d\n", cs->cx );
        cs->cx = 65535;
    }
    if (cs->cy > 65535)
    {
        ERR( "invalid window height %d\n", cs->cx );
        cs->cy = 65535;
    }

    if (!(data = HeapAlloc(GetProcessHeap(), 0, sizeof(*data)))) return FALSE;
    data->whole_window  = 0;
    data->client_window = 0;
    data->icon_window   = 0;
    data->hWMIconBitmap = 0;
    data->hWMIconMask   = 0;

    wndPtr = WIN_GetPtr( hwnd );
    wndPtr->pDriverData = data;

    /* initialize the dimensions before sending WM_GETMINMAXINFO */
    SetRect( &rect, cs->x, cs->y, cs->x + cs->cx, cs->y + cs->cy );
    WIN_SetRectangles( hwnd, &rect, &rect );

    if (!wndPtr->parent)
    {
        create_desktop( display, wndPtr, cs );
        WIN_ReleasePtr( wndPtr );
        return TRUE;
    }

    if (!create_whole_window( display, wndPtr )) goto failed;
    if (!create_client_window( display, wndPtr )) goto failed;
    TSXSync( display, False );

    SetPropA( hwnd, whole_window_atom, (HANDLE)data->whole_window );
    SetPropA( hwnd, client_window_atom, (HANDLE)data->client_window );

    /* Call the WH_CBT hook */

    hwndLinkAfter = ((cs->style & (WS_CHILD|WS_MAXIMIZE)) == WS_CHILD)
 ? HWND_BOTTOM : HWND_TOP;

    if (HOOK_IsHooked( WH_CBT ))
    {
	CBT_CREATEWNDA cbtc;
        LRESULT lret;

	cbtc.lpcs = cs;
	cbtc.hwndInsertAfter = hwndLinkAfter;
        lret = (unicode) ? HOOK_CallHooksW(WH_CBT, HCBT_CREATEWND,
                                                       (WPARAM)hwnd, (LPARAM)&cbtc)
                        : HOOK_CallHooksA(WH_CBT, HCBT_CREATEWND,
                                                       (WPARAM)hwnd, (LPARAM)&cbtc);
        if (lret)
	{
	    TRACE("CBT-hook returned !0\n");
            goto failed;
	} 
    }



    /* Send the WM_GETMINMAXINFO message and fix the size if needed */
    if ((cs->style & WS_THICKFRAME) || !(cs->style & (WS_POPUP | WS_CHILD)))
    {
        POINT maxSize, maxPos, minTrack, maxTrack;

        WIN_ReleasePtr( wndPtr );
        WINPOS_GetMinMaxInfo( hwnd, &maxSize, &maxPos, &minTrack, &maxTrack);
        if (maxSize.x < cs->cx) cs->cx = maxSize.x;
        if (maxSize.y < cs->cy) cs->cy = maxSize.y;
        if (cs->cx < minTrack.x ) cs->cx = minTrack.x;
        if (cs->cy < minTrack.y ) cs->cy = minTrack.y;
        if (cs->cx < 0) cs->cx = 0;
        if (cs->cy < 0) cs->cy = 0;

        if (!(wndPtr = WIN_GetPtr( hwnd ))) return FALSE;
        SetRect( &rect, cs->x, cs->y, cs->x + cs->cx, cs->y + cs->cy );
        WIN_SetRectangles( hwnd, &rect, &rect );
        X11DRV_sync_whole_window_position( display, wndPtr, 0 );
    }
    WIN_ReleasePtr( wndPtr );

    /* send WM_NCCREATE */
    TRACE( "hwnd %x cs %d,%d %dx%d\n", hwnd, cs->x, cs->y, cs->cx, cs->cy );
    if (unicode)
        ret = SendMessageW( hwnd, WM_NCCREATE, 0, (LPARAM)cs );
    else
        ret = SendMessageA( hwnd, WM_NCCREATE, 0, (LPARAM)cs );
    if (!ret)
    {
        WARN("aborted by WM_xxCREATE!\n");
        return FALSE;
    }

    if (!(wndPtr = WIN_GetPtr(hwnd))) return FALSE;

    sync_window_style( display, wndPtr );

    /* send WM_NCCALCSIZE */
    rect = wndPtr->rectWindow;
    WIN_ReleasePtr( wndPtr );
    SendMessageW( hwnd, WM_NCCALCSIZE, FALSE, (LPARAM)&rect );

    if (!(wndPtr = WIN_GetPtr(hwnd))) return FALSE;
    if (rect.left > rect.right || rect.top > rect.bottom) rect = wndPtr->rectWindow;
    WIN_SetRectangles( hwnd, &wndPtr->rectWindow, &rect );
    X11DRV_sync_client_window_position( display, wndPtr );
    X11DRV_register_window( display, hwnd, data );

    TRACE( "win %x window %d,%d,%d,%d client %d,%d,%d,%d whole %d,%d,%d,%d X client %d,%d,%d,%d xwin %x/%x\n",
           hwnd, wndPtr->rectWindow.left, wndPtr->rectWindow.top,
           wndPtr->rectWindow.right, wndPtr->rectWindow.bottom,
           wndPtr->rectClient.left, wndPtr->rectClient.top,
           wndPtr->rectClient.right, wndPtr->rectClient.bottom,
           data->whole_rect.left, data->whole_rect.top,
           data->whole_rect.right, data->whole_rect.bottom,
           data->client_rect.left, data->client_rect.top,
           data->client_rect.right, data->client_rect.bottom,
           (unsigned int)data->whole_window, (unsigned int)data->client_window );

    if ((wndPtr->dwStyle & (WS_CHILD|WS_MAXIMIZE)) == WS_CHILD)
        WIN_LinkWindow( hwnd, wndPtr->parent, HWND_BOTTOM );
    else
        WIN_LinkWindow( hwnd, wndPtr->parent, HWND_TOP );

    WIN_ReleasePtr( wndPtr );

    if (unicode)
        ret = (SendMessageW( hwnd, WM_CREATE, 0, (LPARAM)cs ) != -1);
    else
        ret = (SendMessageA( hwnd, WM_CREATE, 0, (LPARAM)cs ) != -1);

    if (!ret)
    {
        WIN_UnlinkWindow( hwnd );
        return FALSE;
    }

    /* Send the size messages */

    if (!(wndPtr = WIN_FindWndPtr(hwnd))) return FALSE;
    if (!(wndPtr->flags & WIN_NEED_SIZE))
    {
        /* send it anyway */
        if (((wndPtr->rectClient.right-wndPtr->rectClient.left) <0)
            ||((wndPtr->rectClient.bottom-wndPtr->rectClient.top)<0))
            WARN("sending bogus WM_SIZE message 0x%08lx\n",
                 MAKELONG(wndPtr->rectClient.right-wndPtr->rectClient.left,
                          wndPtr->rectClient.bottom-wndPtr->rectClient.top));
        SendMessageW( hwnd, WM_SIZE, SIZE_RESTORED,
                      MAKELONG(wndPtr->rectClient.right-wndPtr->rectClient.left,
                               wndPtr->rectClient.bottom-wndPtr->rectClient.top));
        SendMessageW( hwnd, WM_MOVE, 0,
                      MAKELONG( wndPtr->rectClient.left, wndPtr->rectClient.top ) );
    }

    /* Show the window, maximizing or minimizing if needed */

    if (wndPtr->dwStyle & (WS_MINIMIZE | WS_MAXIMIZE))
    {
        extern UINT WINPOS_MinMaximize( HWND hwnd, UINT cmd, LPRECT rect ); /*FIXME*/

        RECT newPos;
        UINT swFlag = (wndPtr->dwStyle & WS_MINIMIZE) ? SW_MINIMIZE : SW_MAXIMIZE;
        WIN_SetStyle( hwnd, wndPtr->dwStyle & ~(WS_MAXIMIZE | WS_MINIMIZE) );
        WINPOS_MinMaximize( hwnd, swFlag, &newPos );
        swFlag = ((wndPtr->dwStyle & WS_CHILD) || GetActiveWindow())
            ? SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED
            : SWP_NOZORDER | SWP_FRAMECHANGED;
        SetWindowPos( hwnd, 0, newPos.left, newPos.top,
                      newPos.right, newPos.bottom, swFlag );
    }

    WIN_ReleaseWndPtr( wndPtr );
    return TRUE;


 failed:
    X11DRV_DestroyWindow( hwnd );
    if (wndPtr) WIN_ReleasePtr( wndPtr );
    return FALSE;
}


/***********************************************************************
 *		X11DRV_get_client_window
 *
 * Return the X window associated with the client area of a window
 */
Window X11DRV_get_client_window( HWND hwnd )
{
    Window ret = 0;
    WND *win = WIN_GetPtr( hwnd );

    if (win == WND_OTHER_PROCESS)
        return GetPropA( hwnd, client_window_atom );

    if (win)
    {
        struct x11drv_win_data *data = win->pDriverData;
        ret = data->client_window;
        WIN_ReleasePtr( win );
    }
    return ret;
}


/***********************************************************************
 *		X11DRV_get_whole_window
 *
 * Return the X window associated with the full area of a window
 */
Window X11DRV_get_whole_window( HWND hwnd )
{
    Window ret = 0;
    WND *win = WIN_GetPtr( hwnd );

    if (win == WND_OTHER_PROCESS)
        return GetPropA( hwnd, whole_window_atom );

    if (win)
    {
        struct x11drv_win_data *data = win->pDriverData;
        ret = data->whole_window;
        WIN_ReleasePtr( win );
    }
    return ret;
}


/*****************************************************************
 *		SetParent   (X11DRV.@)
 */
HWND X11DRV_SetParent( HWND hwnd, HWND parent )
{
    Display *display = thread_display();
    WND *wndPtr;
    HWND retvalue;

    /* Windows hides the window first, then shows it again
     * including the WM_SHOWWINDOW messages and all */
    BOOL was_visible = ShowWindow( hwnd, SW_HIDE );

    if (!IsWindow( parent )) return 0;
    if (!(wndPtr = WIN_GetPtr(hwnd)) || wndPtr == WND_OTHER_PROCESS) return 0;

    retvalue = wndPtr->parent;  /* old parent */
    if (parent != retvalue)
    {
        struct x11drv_win_data *data = wndPtr->pDriverData;

        WIN_LinkWindow( hwnd, parent, HWND_TOP );

        if (parent != GetDesktopWindow()) /* a child window */
        {
            if (!(wndPtr->dwStyle & WS_CHILD))
            {
                HMENU menu = (HMENU)SetWindowLongW( hwnd, GWL_ID, 0 );
                if (menu) DestroyMenu( menu );
            }
        }

        if (is_window_top_level( wndPtr )) set_wm_hints( display, wndPtr );
        wine_tsx11_lock();
        sync_window_style( display, wndPtr );
        XReparentWindow( display, data->whole_window, X11DRV_get_client_window(parent),
                         data->whole_rect.left, data->whole_rect.top );
        wine_tsx11_unlock();
    }
    WIN_ReleasePtr( wndPtr );

    /* SetParent additionally needs to make hwnd the topmost window
       in the x-order and send the expected WM_WINDOWPOSCHANGING and
       WM_WINDOWPOSCHANGED notification messages. 
    */
    SetWindowPos( hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                  SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | (was_visible ? SWP_SHOWWINDOW : 0) );
    /* FIXME: a WM_MOVE is also generated (in the DefWindowProc handler
     * for WM_WINDOWPOSCHANGED) in Windows, should probably remove SWP_NOMOVE */

    return retvalue;
}


/*****************************************************************
 *		SetFocus   (X11DRV.@)
 *
 * Set the X focus.
 * Explicit colormap management seems to work only with OLVWM.
 */
void X11DRV_SetFocus( HWND hwnd )
{
    Display *display = thread_display();
    XWindowAttributes win_attr;
    Window win;

    /* Only mess with the X focus if there's */
    /* no desktop window and if the window is not managed by the WM. */
    if (root_window != DefaultRootWindow(display)) return;

    if (!hwnd)  /* If setting the focus to 0, uninstall the colormap */
    {
        if (X11DRV_PALETTE_PaletteFlags & X11DRV_PALETTE_PRIVATE)
            TSXUninstallColormap( display, X11DRV_PALETTE_PaletteXColormap );
        return;
    }

    hwnd = GetAncestor( hwnd, GA_ROOT );
    if (GetWindowLongW( hwnd, GWL_EXSTYLE ) & WS_EX_MANAGED) return;
    if (!(win = X11DRV_get_whole_window( hwnd ))) return;

    /* Set X focus and install colormap */
    wine_tsx11_lock();
    if (XGetWindowAttributes( display, win, &win_attr ) &&
        (win_attr.map_state == IsViewable))
    {
        /* If window is not viewable, don't change anything */

        /* we must not use CurrentTime (ICCCM), so try to use last message time instead */
        /* FIXME: this is not entirely correct */
        XSetInputFocus( display, win, RevertToParent,
                        /*CurrentTime*/ GetMessageTime() + X11DRV_server_startticks );
        if (X11DRV_PALETTE_PaletteFlags & X11DRV_PALETTE_PRIVATE)
            XInstallColormap( display, X11DRV_PALETTE_PaletteXColormap );
    }
    wine_tsx11_unlock();
}


/**********************************************************************
 *		SetWindowIcon (X11DRV.@)
 *
 * hIcon or hIconSm has changed (or is being initialised for the
 * first time). Complete the X11 driver-specific initialisation
 * and set the window hints.
 *
 * This is not entirely correct, may need to create
 * an icon window and set the pixmap as a background
 */
HICON X11DRV_SetWindowIcon( HWND hwnd, HICON icon, BOOL small )
{
    WND *wndPtr;
    Display *display = thread_display();
    HICON old = SetClassLongW( hwnd, small ? GCL_HICONSM : GCL_HICON, icon );

    SetWindowPos( hwnd, 0, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOSIZE |
                  SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER );

    if (!(wndPtr = WIN_GetPtr( hwnd )) || wndPtr == WND_OTHER_PROCESS) return old;

    if (wndPtr->dwExStyle & WS_EX_MANAGED)
    {
        Window win = get_whole_window(wndPtr);
        XWMHints* wm_hints = TSXGetWMHints( display, win );

        if (!wm_hints) wm_hints = TSXAllocWMHints();
        if (wm_hints)
        {
            set_icon_hints( display, wndPtr, wm_hints );
            TSXSetWMHints( display, win, wm_hints );
            TSXFree( wm_hints );
        }
    }
    WIN_ReleasePtr( wndPtr );
    return old;
}
