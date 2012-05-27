
/*
 * Copyright 2002, SuSE Linux AG, Author: Egbert Eich
 * Copyright 2010, commonIT, Author: Corentin Chary
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* All drivers should typically include these */
#include "xf86.h"
#include "xf86_OSproc.h"
#include "xf86Modes.h"
#include "micmap.h"

/* All drivers initialising the SW cursor need this */
#include "mipointer.h"

/* All drivers implementing backing store need this */
#include "mibstore.h"

/* All drivers using framebuffer need this */
#include "xf86fbman.h"
#include "fb.h"
#include "picturestr.h"

/* All drivers using xwayland module need this */
#include "xwayland.h"
#include <xorg/xf86Priv.h>

/*
 * Driver data structures.
 */
#include "wlshm.h"

#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>

/* These need to be checked */
#include <X11/X.h>
#include <X11/Xproto.h>
#include "scrnintstr.h"
#include "servermd.h"

#define WLSHM_VERSION 0001
#define WLSHM_NAME "wlshm"
#define WLSHM_DRIVER_NAME "wlshm"

#define WLSHM_MAJOR_VERSION PACKAGE_VERSION_MAJOR
#define WLSHM_MINOR_VERSION PACKAGE_VERSION_MINOR
#define WLSHM_PATCHLEVEL PACKAGE_VERSION_PATCHLEVEL

static DevPrivateKeyRec wlshm_pixmap_private_key;

static Bool
window_own_pixmap(WindowPtr pWin)
{
    if (xorgRootless) {
	if (pWin->redirectDraw != RedirectDrawManual)
            return FALSE;
    } else {
	if (pWin->parent)
	    return FALSE;
    }
    return TRUE;
}

static Bool
wlshm_get_device(ScrnInfoPtr pScrn)
{
    /*
     * Allocate a wlshm_device, and hook it into pScrn->driverPrivate.
     * pScrn->driverPrivate is initialised to NULL, so we can check if
     * the allocation has already been done.
     */
    if (pScrn->driverPrivate != NULL)
	return TRUE;

    pScrn->driverPrivate = xnfcalloc(sizeof(struct wlshm_device), 1);

    if (pScrn->driverPrivate == NULL)
	return FALSE;

    return TRUE;
}

static void
wlshm_free_device(ScrnInfoPtr pScrn)
{
    if (pScrn->driverPrivate == NULL)
	return;
    free(pScrn->driverPrivate);
    pScrn->driverPrivate = NULL;
}

static void
wlshm_save(ScrnInfoPtr pScrn)
{
}

static void
wlshm_restore(ScrnInfoPtr pScrn, Bool restoreText)
{
}

static Bool
wlshm_save_screen(ScreenPtr pScreen, int mode)
{
    return TRUE;
}

static Bool
wlshm_mode_init(ScrnInfoPtr pScrn, DisplayModePtr mode)
{
    wlshm_restore(pScrn, FALSE);

    return TRUE;
}

static Bool
wlshm_enter_vt(int scrnIndex, int flags)
{
    ScrnInfoPtr pScrn = xf86Screens[scrnIndex];

    /* Should we re-save the text mode on each VT enter? */
    if(!wlshm_mode_init(pScrn, pScrn->currentMode))
      return FALSE;

    return TRUE;
}

static void
wlshm_leave_vt(int scrnIndex, int flags)
{
    ScrnInfoPtr pScrn = xf86Screens[scrnIndex];
    wlshm_restore(pScrn, TRUE);
}

static Bool
wlshm_switch_mode(int scrnIndex, DisplayModePtr mode, int flags)
{
    return wlshm_mode_init(xf86Screens[scrnIndex], mode);
}

static void
wlshm_adjust_frame(int scrnIndex, int x, int y, int flags)
{
}

static void
wlshm_flush_callback(CallbackListPtr *list,
                     pointer user_data, pointer call_data)
{
    struct wlshm_device *wlshm = user_data;

    if (wlshm->xwl_screen)
        xwl_screen_post_damage(wlshm->xwl_screen);
}

static Bool
wlshm_close_screen(int scrnIndex, ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[scrnIndex];
    struct wlshm_device *wlshm = wlshm_scrninfo_priv(pScrn);

    DeleteCallback(&FlushCallback, wlshm_flush_callback, wlshm);

    if(pScrn->vtSema){
 	wlshm_restore(pScrn, TRUE);
	free(wlshm->fb);
    }

    xwl_screen_close(wlshm->xwl_screen);

    pScrn->vtSema = FALSE;
    pScreen->CloseScreen = wlshm->CloseScreen;
    return (*pScreen->CloseScreen)(scrnIndex, pScreen);
}

static void
wlshm_free_screen(int scrnIndex, int flags)
{
    ScrnInfoPtr pScrn = xf86Screens[scrnIndex];
    struct wlshm_device *wlshm = wlshm_scrninfo_priv(pScrn);

    if (wlshm) {
        if (wlshm->xwl_screen)
	    xwl_screen_destroy(wlshm->xwl_screen);
	wlshm->xwl_screen = NULL;
    }

    wlshm_free_device(pScrn);
}

static ModeStatus
wlshm_valid_mode(int scrnIndex, DisplayModePtr mode, Bool verbose, int flags)
{
    return MODE_OK;
}

static void
wlshm_free_window_pixmap(WindowPtr pWindow)
{
    ScreenPtr pScreen = pWindow->drawable.pScreen;
    struct wlshm_device *wlshm = wlshm_screen_priv(pScreen);
    struct wlshm_pixmap *d;
    PixmapPtr pixmap;

    if (!xorgRootless && pWindow->parent)
	return ;

    pixmap = pScreen->GetWindowPixmap(pWindow);
    if (!pixmap)
        return ;

    d = dixLookupPrivate(&pixmap->devPrivates, &wlshm_pixmap_private_key);
    if (!d)
        return ;

    dixSetPrivate(&pixmap->devPrivates, &wlshm_pixmap_private_key, NULL);

    pixmap->devPrivate.ptr = d->orig;
    pixmap->devPrivate.fptr = d->orig;
    memcpy(d->orig, d->data, d->bytes);
    munmap(d->data, d->bytes);
    close(d->fd);

    free(d);
}

static Bool
wlshm_destroy_window(WindowPtr pWindow)
{
    ScreenPtr pScreen = pWindow->drawable.pScreen;
    struct wlshm_device *wlshm = wlshm_screen_priv(pScreen);
    Bool ret;

    wlshm_free_window_pixmap(pWindow);

    pScreen->DestroyWindow = wlshm->DestroyWindow;
    ret = (*pScreen->DestroyWindow)(pWindow);
    wlshm->DestroyWindow = pScreen->DestroyWindow;
    pScreen->DestroyWindow = wlshm_destroy_window;

    return ret;
}

static Bool
wlshm_unrealize_window(WindowPtr pWindow)
{
    ScreenPtr pScreen = pWindow->drawable.pScreen;
    struct wlshm_device *wlshm = wlshm_screen_priv(pScreen);
    Bool ret;

    wlshm_free_window_pixmap(pWindow);

    pScreen->UnrealizeWindow = wlshm->UnrealizeWindow;
    ret = (*pScreen->UnrealizeWindow)(pWindow);
    wlshm->UnrealizeWindow = pScreen->UnrealizeWindow;
    pScreen->UnrealizeWindow = wlshm_unrealize_window;

    return ret;
}

static void
wlshm_set_window_pixmap(WindowPtr pWindow, PixmapPtr pPixmap)
{
    ScreenPtr pScreen = pWindow->drawable.pScreen;
    struct wlshm_device *wlshm = wlshm_screen_priv(pScreen);

    wlshm_free_window_pixmap(pWindow);

    pScreen->SetWindowPixmap = wlshm->SetWindowPixmap;
    (*pScreen->SetWindowPixmap)(pWindow, pPixmap);
    wlshm->SetWindowPixmap = pScreen->SetWindowPixmap;
    pScreen->SetWindowPixmap = wlshm_set_window_pixmap;

    /* xwayland will call create_window_buffer later */
}

static Bool
wlshm_create_window(WindowPtr pWin)
{
    ScreenPtr pScreen = pWin->drawable.pScreen;
    struct wlshm_device *wlshm = wlshm_screen_priv(pScreen);
    int ret;

    pScreen->CreateWindow = wlshm->CreateWindow;
    ret = pScreen->CreateWindow(pWin);
    wlshm->CreateWindow = pScreen->CreateWindow;
    pScreen->CreateWindow = wlshm_create_window;

    if (!xorgRootless || !window_own_pixmap(pWin))
	return ret;

    PixmapPtr pixmap = fbCreatePixmap(pScreen,
                                      pWin->drawable.width,
                                      pWin->drawable.height,
                                      pWin->drawable.depth, 0);
    _fbSetWindowPixmap(pWin, pixmap);
    return ret;
}

static Bool
wlshm_screen_init(int scrnIndex, ScreenPtr pScreen, int argc, char **argv)
{
    ScrnInfoPtr pScrn;
    struct wlshm_device *wlshm;
    int ret;
    VisualPtr visual;

    if (!dixRegisterPrivateKey(&wlshm_pixmap_private_key, PRIVATE_PIXMAP, 0))
        return BadAlloc;
    /*
     * we need to get the ScrnInfoRec for this screen, so let's allocate
     * one first thing
     */
    pScrn = xf86Screens[pScreen->myNum];
    wlshm = wlshm_screen_priv(pScreen);

    /*
     * next we save the current state and setup the first mode
     */
    wlshm_save(pScrn);

    if (!wlshm_mode_init(pScrn,pScrn->currentMode))
	return FALSE;

    /*
     * Reset visual list.
     */
    miClearVisualTypes();

    /* Setup the visuals we support. */

    if (!miSetVisualTypes(pScrn->depth,
                          miGetDefaultVisualMask(pScrn->depth),
                          pScrn->rgbBits, pScrn->defaultVisual))
         return FALSE;

    if (!miSetPixmapDepths())
        return FALSE;

    wlshm->fb = malloc(pScrn->virtualX * pScrn->virtualY * pScrn->bitsPerPixel);

    if (!wlshm)
	return FALSE;

    /*
     * Call the framebuffer layer's ScreenInit function, and fill in other
     * pScreen fields.
     */
    ret = fbScreenInit(pScreen, wlshm->fb,
                       pScrn->virtualX, pScrn->virtualY,
                       pScrn->xDpi, pScrn->yDpi,
                       pScrn->displayWidth, pScrn->bitsPerPixel);
    if (!ret)
	return FALSE;

    if (pScrn->depth > 8) {
        /* Fixup RGB ordering */
        visual = pScreen->visuals + pScreen->numVisuals;
        while (--visual >= pScreen->visuals) {
	    if ((visual->class | DynamicClass) == DirectColor) {
		visual->offsetRed = pScrn->offset.red;
		visual->offsetGreen = pScrn->offset.green;
		visual->offsetBlue = pScrn->offset.blue;
		visual->redMask = pScrn->mask.red;
		visual->greenMask = pScrn->mask.green;
		visual->blueMask = pScrn->mask.blue;
	    }
	}
    }

    /* must be after RGB ordering fixed */
    fbPictureInit(pScreen, 0, 0);

    xf86SetBlackWhitePixels(pScreen);

    {
	BoxRec AvailFBArea;

	AvailFBArea.x1 = 0;
	AvailFBArea.y1 = 0;
	AvailFBArea.x2 = pScrn->displayWidth;
	AvailFBArea.y2 = pScrn->virtualY;

	xf86InitFBManager(pScreen, &AvailFBArea);
    }

    miInitializeBackingStore(pScreen);
    xf86SetBackingStore(pScreen);
    xf86SetSilkenMouse(pScreen);

    /* Initialise cursor functions */
    miDCInitialize (pScreen, xf86GetPointerScreenFuncs());

    /* FIXME: colourmap */
    miCreateDefColormap(pScreen);

    pScreen->SaveScreen = wlshm_save_screen;

    /* Wrap the current CloseScreen function */
    wlshm->CloseScreen = pScreen->CloseScreen;
    pScreen->CloseScreen = wlshm_close_screen;

    /* Wrap the current CreateWindow function */
    wlshm->CreateWindow = pScreen->CreateWindow;
    pScreen->CreateWindow = wlshm_create_window;

    /* Wrap the current DestroyWindow function */
    wlshm->DestroyWindow = pScreen->DestroyWindow;
    pScreen->DestroyWindow = wlshm_destroy_window;

    /* Wrap the current UnrealizeWindow function */
    wlshm->UnrealizeWindow = pScreen->UnrealizeWindow;
    pScreen->UnrealizeWindow = wlshm_unrealize_window;

    /* Wrap the current SetWindowPixmap function */
    wlshm->SetWindowPixmap = pScreen->SetWindowPixmap;
    pScreen->SetWindowPixmap = wlshm_set_window_pixmap;

    AddCallback(&FlushCallback, wlshm_flush_callback, wlshm);

    /* Report any unused options (only for the first generation) */
    if (serverGeneration == 1) {
	xf86ShowUnusedOptions(pScrn->scrnIndex, pScrn->options);
    }

    miScreenDevPrivateInit(pScreen, pScreen->width, wlshm->fb);

    if (wlshm->xwl_screen)
	return (xwl_screen_init(wlshm->xwl_screen, pScreen) == Success);

    return TRUE;
}

static int
wlshm_create_window_buffer(struct xwl_window *xwl_window,
                           PixmapPtr pixmap)
{
    ScreenPtr pScreen = pixmap->drawable.pScreen;
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    char filename[] = "/tmp/wayland-shm-XXXXXX";
    int ret = BadAlloc;
    struct wlshm_pixmap *d;

    d = calloc(sizeof (struct wlshm_pixmap), 1);
    if (!d) {
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "can't alloc wlshm pixmap: %s\n",
                   strerror(errno));
        goto exit;
    }
    d->fd = -1;
    d->data = MAP_FAILED;

    d->fd = mkstemp(filename);
    if (d->fd < 0) {
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "open %s failed: %s\n",
                   filename, strerror(errno));
        goto exit;
    }

    d->bytes = pixmap->drawable.width * pixmap->drawable.height
        * pixmap->drawable.bitsPerPixel / 8;

    if (ftruncate(d->fd, d->bytes) < 0) {
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "ftruncate failed: %s\n",
                   strerror(errno));
        goto exit;
    }

    d->data = mmap(NULL, d->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, 0);
    unlink(filename);

    if (d->data == MAP_FAILED) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "mmap failed: %s\n",
                   strerror(errno));
        goto exit;
    }

    ret = xwl_create_window_buffer_shm(xwl_window, pixmap, d->fd);
    if (ret != Success) {
        goto exit;
    }

    d->orig = pixmap->devPrivate.ptr;
    pixmap->devPrivate.ptr = d->data;
    pixmap->devPrivate.fptr = d->data;
    memcpy(d->data, d->orig, d->bytes);

    dixSetPrivate(&pixmap->devPrivates, &wlshm_pixmap_private_key, d);

    return ret;
exit:
    if (d) {
        if (d->fd != -1)
            close(d->fd);
        if (d->data != MAP_FAILED)
            munmap(d->data, d->bytes);
        free(d);
    }

    return ret;
}

static struct xwl_driver xwl_driver = {
    .version = 2,
    .create_window_buffer = wlshm_create_window_buffer
};

static const OptionInfoRec wlshm_options[] = {
    { -1,                  NULL,           OPTV_NONE,	{0}, FALSE }
};

static Bool
wlshm_pre_init(ScrnInfoPtr pScrn, int flags)
{
    struct wlshm_device *wlshm;
    int i;
    GDevPtr device;
    int flags24;

    if (flags & PROBE_DETECT)
	return TRUE;

    if (!xorgWayland) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                   "You must run Xorg with -xwayland parameter\n");
        return FALSE;
    }

    /* Allocate the wlshm_device driverPrivate */
    if (!wlshm_get_device(pScrn)) {
	return FALSE;
    }

    wlshm = wlshm_scrninfo_priv(pScrn);

    pScrn->chipset = "wlshm";
    pScrn->monitor = pScrn->confScreen->monitor;

    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Initializing Wayland SHM driver\n");

    flags24 = Support32bppFb | SupportConvert24to32 | PreferConvert24to32;
    if (!xf86SetDepthBpp(pScrn, 0, 0, 0, flags24)) {
	return FALSE;
    } else {
	/* Check that the returned depth is one we support */
	switch (pScrn->depth) {
	case 24:
        case 30:
        case 32:
            break ;
	default:
	    xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		       "Given depth (%d) is not supported by this driver\n",
		       pScrn->depth);
	    return FALSE;
	}
    }

    xf86PrintDepthBpp(pScrn);

    /*
     * This must happen after pScrn->display has been set because
     * xf86SetWeight references it.
     */
    if (pScrn->depth > 8) {
	/* The defaults are OK for us */
	rgb zeros = {0, 0, 0};

	if (!xf86SetWeight(pScrn, zeros, zeros)) {
	    return FALSE;
	} else {
	    /* XXX check that weight returned is supported */
	    ;
	}
    }

    if (!xf86SetDefaultVisual(pScrn, -1))
	return FALSE;

    if (pScrn->depth > 1) {
	Gamma zeros = {0.0, 0.0, 0.0};

	if (!xf86SetGamma(pScrn, zeros))
	    return FALSE;
    }

    device = xf86GetEntityInfo(pScrn->entityList[0])->device;
    xf86CollectOptions(pScrn, device->options);
    free(device);

    /* Process the options */
    if (!(wlshm->options = malloc(sizeof(wlshm_options))))
	return FALSE;

    memcpy(wlshm->options, wlshm_options, sizeof(wlshm_options));

    xf86ProcessOptions(pScrn->scrnIndex, pScrn->options, wlshm->options);

    wlshm->xwl_screen = xwl_screen_create();
    if (!wlshm->xwl_screen) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "Failed to initialize xwayland.\n");
        goto error;
    }

    if (!xwl_screen_pre_init(pScrn, wlshm->xwl_screen, 0, &xwl_driver)) {
            xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "Failed to pre-init xwayland screen\n");
            xwl_screen_destroy(wlshm->xwl_screen);
    }

    /* Subtract memory for HW cursor */
    xf86ValidateModesSize(pScrn, pScrn->monitor->Modes,
                          pScrn->display->virtualX,
                          pScrn->display->virtualY,
                          0);

    /* Prune the modes marked as invalid */
    xf86PruneDriverModes(pScrn);

    if (pScrn->modes == NULL) {
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "No valid modes found\n");
	goto error;
    }

    /*
     * Set the CRTC parameters for all of the modes based on the type
     * of mode, and the chipset's interlace requirements.
     *
     * Calling this is required if the mode->Crtc* values are used by the
     * driver and if the driver doesn't provide code to set them.  They
     * are not pre-initialised at all.
     */
    xf86SetCrtcForModes(pScrn, 0);

    /* Set the current mode to the first in the list */
    pScrn->currentMode = pScrn->modes;

    /* Print the list of modes being used */
    xf86PrintModes(pScrn);

    /* If monitor resolution is set on the command line, use it */
    xf86SetDpi(pScrn, 0, 0);

    if (xf86LoadSubModule(pScrn, "fb") == NULL) {
	goto error;
    }

    /* We have no contiguous physical fb in physical memory */
    pScrn->memPhysBase = 0;
    pScrn->fbOffset = 0;

    return TRUE;

error:
    wlshm_free_device(pScrn);
    return FALSE;
}

/* Mandatory */
static Bool
wlshm_probe(DriverPtr drv, int flags)
{
    Bool found = FALSE;
    int count;
    GDevPtr *sections;
    int i;

    if (flags & PROBE_DETECT)
	return FALSE;
    /*
     * Find the config file Device sections that match this
     * driver, and return if there are none.
     */
    count = xf86MatchDevice(WLSHM_DRIVER_NAME, &sections);

    if (count <= 0) {
	return FALSE;
    }

    for (i = 0; i < count; i++) {
        int entityIndex = xf86ClaimNoSlot(drv, 0, sections[i], TRUE);
        ScrnInfoPtr pScrn = xf86AllocateScreen(drv, 0);

        if (!pScrn)
            continue ;

        xf86AddEntityToScreen(pScrn, entityIndex);
        pScrn->driverVersion = WLSHM_VERSION;
        pScrn->driverName    = WLSHM_DRIVER_NAME;
        pScrn->name          = WLSHM_NAME;
        pScrn->Probe         = wlshm_probe;
        pScrn->PreInit       = wlshm_pre_init;
        pScrn->ScreenInit    = wlshm_screen_init;
        pScrn->SwitchMode    = wlshm_switch_mode;
        pScrn->AdjustFrame   = wlshm_adjust_frame;
        pScrn->EnterVT       = wlshm_enter_vt;
        pScrn->LeaveVT       = wlshm_leave_vt;
        pScrn->FreeScreen    = wlshm_free_screen;
        pScrn->ValidMode     = wlshm_valid_mode;

        found = TRUE;
    }

    free(sections);

    return found;
}

static const OptionInfoRec *
wlshm_available_options(int chipid, int busid)
{
    return wlshm_options;
}

#ifndef HW_SKIP_CONSOLE
#define HW_SKIP_CONSOLE 4
#endif

static Bool
wlshm_driver_func(ScrnInfoPtr pScrn, xorgDriverFuncOp op, pointer ptr)
{
    CARD32 *flag;

    switch (op) {
	case GET_REQUIRED_HW_INTERFACES:
	    flag = (CARD32*)ptr;
	    (*flag) = HW_SKIP_CONSOLE;
	    return TRUE;
	default:
	    return FALSE;
    }
}

/*
 * This contains the functions needed by the server after loading the driver
 * module.  It must be supplied, and gets passed back by the SetupProc
 * function in the dynamic case.  In the static case, a reference to this
 * is compiled in, and this requires that the name of this DriverRec be
 * an upper-case version of the driver name.
 */

_X_EXPORT DriverRec wlshm = {
    WLSHM_VERSION,
    WLSHM_DRIVER_NAME,
    NULL,
    wlshm_probe,
    wlshm_available_options,
    NULL,
    0,
    wlshm_driver_func
};

#ifdef XFree86LOADER

static XF86ModuleVersionInfo wlshm_vers_rec =
{
	"wlshm",
	MODULEVENDORSTRING,
	MODINFOSTRING1,
	MODINFOSTRING2,
	XORG_VERSION_CURRENT,
	WLSHM_MAJOR_VERSION, WLSHM_MINOR_VERSION, WLSHM_PATCHLEVEL,
	ABI_CLASS_VIDEODRV,
	ABI_VIDEODRV_VERSION,
	MOD_CLASS_VIDEODRV,
	{0,0,0,0}
};


static pointer
wlshm_setup(pointer module, pointer opts, int *errmaj, int *errmin)
{
    static Bool initialized = FALSE;

    if (!initialized) {
        initialized = TRUE;
        xf86AddDriver(&wlshm, module, HaveDriverFuncs);

	/*
	 * Modules that this driver always requires can be loaded here
	 * by calling LoadSubModule().
	 */

	/*
	 * The return value must be non-NULL on success even though there
	 * is no TearDownProc.
	 */
	return (pointer)1;
    } else {
	if (errmaj) *errmaj = LDR_ONCEONLY;
	return NULL;
    }
}

static MODULESETUPPROTO(wlshm_setup);

/*
 * This is the module init data.
 * Its name has to be the driver name followed by ModuleData
 */
_X_EXPORT XF86ModuleData wlshmModuleData = {
    &wlshm_vers_rec,
    wlshm_setup,
    NULL
};

#endif /* XFree86LOADER */
