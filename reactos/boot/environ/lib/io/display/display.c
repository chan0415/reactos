/*
 * COPYRIGHT:       See COPYING.ARM in the top level directory
 * PROJECT:         ReactOS UEFI Boot Library
 * FILE:            boot/environ/lib/platform/display.c
 * PURPOSE:         Boot Library Display Management Routines
 * PROGRAMMER:      Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include "bl.h"
#include <bcd.h>

/* DATA VARIABLES ************************************************************/

PVOID BfiCachedStrikeData;
LIST_ENTRY BfiDeferredListHead;
LIST_ENTRY BfiFontFileListHead;
PVOID BfiGraphicsRectangle;

ULONG ConsoleGraphicalResolutionListFlags;
BL_DISPLAY_MODE ConsoleGraphicalResolutionList[3] =
{
    {1024, 768, 1024},
    {800, 600, 800},
    {1024, 600, 1024}
};
ULONG ConsoleGraphicalResolutionListSize = RTL_NUMBER_OF(ConsoleGraphicalResolutionList);

BL_DISPLAY_MODE ConsoleTextResolutionList[1] =
{
    {80, 25, 80}
};

PVOID DspRemoteInputConsole;
PVOID DspTextConsole;
PVOID DspGraphicalConsole;
PVOID DspLocalInputConsole;

/* FUNCTIONS *****************************************************************/

BOOLEAN
DsppGraphicsDisabledByBcd (
    VOID
    )
{
    BOOLEAN Disabled;
    NTSTATUS Status;

    /* Get the boot option, and if present, return the result */
    Status = BlGetBootOptionBoolean(BlpApplicationEntry.BcdData,
                                    BcdLibraryBoolean_GraphicsModeDisabled,
                                    &Disabled);
    return (NT_SUCCESS(Status) && (Disabled));
}

NTSTATUS
DsppLoadFontFile (
    _In_ PWCHAR FontFileName
    )
{
    PBL_DEVICE_DESCRIPTOR FontDevice;
    NTSTATUS Status;
    ULONG NameLength, DirectoryLength, TotalLength;
    PWCHAR FontPath, FontDirectory;
    BL_LIBRARY_PARAMETERS LibraryParameters;
    BOOLEAN CustomDirectory, CustomDevice;

    /* Initialize locals */
    CustomDirectory = TRUE;
    CustomDevice = TRUE;
    FontDevice = NULL;
    FontPath = NULL;
    FontDirectory = NULL;

    /* Check if a custom font path should be used */
    Status = BlGetBootOptionString(BlpApplicationEntry.BcdData,
                                   BcdLibraryString_FontPath,
                                   &FontDirectory);
    if (!NT_SUCCESS(Status))
    {
        /* Nope, use the one configured by the library */
        CustomDirectory = FALSE;
        RtlCopyMemory(&LibraryParameters,
                      &BlpLibraryParameters,
                      sizeof(LibraryParameters)),
        FontDirectory = LibraryParameters.FontBaseDirectory;
    }

    /* Do we still not have a font directory? */
    if (!FontDirectory)
    {
        /* Use the boot device and boot directory */
        FontDevice = BlpBootDevice;
        FontDirectory = L"\\EFI\\Microsoft\\Boot\\Fonts";
        CustomDevice = FALSE;
    }
    else
    {
        /* Otherwise, if we have a font directory, what device is the app on? */
        Status = BlGetBootOptionDevice(BlpApplicationEntry.BcdData,
                                       BcdLibraryDevice_ApplicationDevice,
                                       &FontDevice,
                                       NULL);
        if (!NT_SUCCESS(Status))
        {
            /* If we don't know the device, we can't open the path */
            goto Quickie;
        }
    }

    /* Figure out the length of the file name, and of the directory */
    NameLength = wcslen(FontFileName);
    DirectoryLength = wcslen(FontDirectory);

    /* Safely add them up*/
    Status = RtlULongAdd(NameLength, DirectoryLength, &TotalLength);
    if (!NT_SUCCESS(Status))
    {
        goto Quickie;
    }

    /* Convert to bytes */
    Status = RtlULongLongToULong(TotalLength * sizeof(WCHAR), &TotalLength);
    if (!NT_SUCCESS(Status))
    {
        goto Quickie;
    }

    /* Add a terminating NUL */
    Status = RtlULongAdd(TotalLength, sizeof(UNICODE_NULL), &TotalLength);
    if (!NT_SUCCESS(Status))
    {
        goto Quickie;
    }

    /* Allocate the final buffer for it */
    FontPath = BlMmAllocateHeap(TotalLength);
    if (!FontPath)
    {
        Status = STATUS_NO_MEMORY;
        goto Quickie;
    }

    /* Concatenate the directory with the file name */
    wcscpy(FontPath, FontDirectory);
    wcscat(FontPath, FontFileName);

    /* Try to load this font */
    Status = BfLoadFontFile(FontDevice, FontPath);

Quickie:
    /* Check if we had a custom font device allocated and free it */
    if ((CustomDevice) && (FontDevice))
    {
        BlMmFreeHeap(FontDevice);
    }

    /* Check if we had a custom font directory allocated and free it */
    if ((FontDirectory) && (CustomDirectory))
    {
        BlMmFreeHeap(FontDirectory);
    }

    /* Check if we had allocated a font path and free it */
    if (FontPath)
    {
        BlMmFreeHeap(FontPath);
    }

    /* Return back */
    return Status;
}

NTSTATUS
BlpDisplayRegisterLocale (
    _In_ PWCHAR Locale
    )
{
    BOOLEAN StandardLocale;
    NTSTATUS Status;
    PWCHAR FontFileName;
    PBL_DEFERRED_FONT_FILE DeferredFont;
    PLIST_ENTRY NextEntry;
    WCHAR Prefix[3];

    /* Assume custom locale */
    StandardLocale = FALSE;

    /* Bail out if the locale string seems invalid */
    if (wcslen(Locale) < 2)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Check the prefix first, then traditional vs. simplified */
    Prefix[0] = Locale[0];
    Prefix[1] = Locale[1];
    Prefix[2] = UNICODE_NULL;
    if (!_wcsicmp(Prefix, L"ja"))
    {
        FontFileName = L"\\jpn_boot.ttf";
    }
    else if (!_wcsicmp(Prefix, L"ko"))
    {
        FontFileName = L"\\kor_boot.ttf";
    }
    else if (!(_wcsicmp(Locale, L"zh-CN")) ||
             !(_wcsicmp(Locale, L"zh-CHS")) ||
             !(_wcsicmp(Locale, L"zh-Hans")))
    {
        FontFileName = L"\\chs_boot.ttf";
    }
    else if (!(_wcsicmp(Locale, L"zh-TW")) &&
             !(_wcsicmp(Locale, L"zh-CHT")) &&
             !(_wcsicmp(Locale, L"zh-HK")) &&
             !(_wcsicmp(Locale, L"zh-Hant")))
    {
        FontFileName = L"\\cht_boot.ttf";
    }
    else
    {
        StandardLocale = TRUE;
        FontFileName = L"\\wgl4_boot.ttf";
    }

    /* Parse all the currently deferred fonts*/
    NextEntry = BfiDeferredListHead.Flink;
    while (NextEntry != &BfiDeferredListHead)
    {
        /* Grab the font */
        DeferredFont = CONTAINING_RECORD(NextEntry, BL_DEFERRED_FONT_FILE, ListEntry);

        /* Move to the next entry, and remove this one */
        NextEntry = NextEntry->Flink;
        RemoveEntryList(&DeferredFont->ListEntry);

        /* Free the deferred font, we'll be loading a new one */
        BfiFreeDeferredFontFile(DeferredFont);
    }

    /* Load the primary font */
    Status = DsppLoadFontFile(FontFileName);
    if (NT_SUCCESS(Status) && !(StandardLocale))
    {
        /* Also load the standard US one if we loaded a different one */
        Status = DsppLoadFontFile(L"\\wgl4_boot.ttf");
    }

    /* Return back to caller */
    return Status;
}

NTSTATUS
DsppInitialize (
    _In_ ULONG Flags
    )
{
    BL_LIBRARY_PARAMETERS LibraryParameters = BlpLibraryParameters;
    BOOLEAN NoGraphics, HighestMode;
    NTSTATUS Status;
    PBL_DISPLAY_MODE DisplayMode;
    ULONGLONG GraphicsResolution;
    PBL_GRAPHICS_CONSOLE GraphicsConsole;
    PBL_TEXT_CONSOLE TextConsole, RemoteConsole;

    /* Initialize font data */
    BfiCachedStrikeData = 0;
    InitializeListHead(&BfiDeferredListHead);
    InitializeListHead(&BfiFontFileListHead);

    /* Allocate the font rectangle */
    BfiGraphicsRectangle = BlMmAllocateHeap(90);
    if (!BfiGraphicsRectangle)
    {
        return STATUS_NO_MEMORY;
    }

    /* Display re-initialization not yet handled */
    if (LibraryParameters.LibraryFlags & BL_LIBRARY_FLAG_REINITIALIZE_ALL)
    {
        EfiPrintf(L"Display path not handled\r\n");
        return STATUS_NOT_SUPPORTED;
    }

    /* Check if no graphics console is needed */
    if ((Flags & BL_LIBRARY_FLAG_NO_GRAPHICS_CONSOLE) ||
        (DsppGraphicsDisabledByBcd()))
    {
        /* Remember this */
        NoGraphics = TRUE;
    }
    else
    {
        /* No graphics -- remember this */
        NoGraphics = FALSE;
    }

    /* On first load, we always initialize a graphics display */
    GraphicsConsole = NULL;
    if (!(Flags & BL_LIBRARY_FLAG_REINITIALIZE_ALL) || !(NoGraphics))
    {
        /* Default to mode 0 (1024x768) */
        DisplayMode = &ConsoleGraphicalResolutionList[0];

        /* Check what resolution to use*/
        Status = BlGetBootOptionInteger(BlpApplicationEntry.BcdData,
                                        BcdLibraryInteger_GraphicsResolution,
                                        &GraphicsResolution);
        if (NT_SUCCESS(Status))
        {
            ConsoleGraphicalResolutionListFlags |= BL_DISPLAY_GRAPHICS_FORCED_VIDEO_MODE_FLAG;
            EfiPrintf(L"Display selection not yet handled\r\n");
            return STATUS_NOT_IMPLEMENTED;
        }

        /* Check if the highest mode should be forced */
        Status = BlGetBootOptionBoolean(BlpApplicationEntry.BcdData,
                                        BcdLibraryBoolean_GraphicsForceHighestMode,
                                        &HighestMode);
        if (NT_SUCCESS(Status))
        {
            ConsoleGraphicalResolutionListFlags |= BL_DISPLAY_GRAPHICS_FORCED_HIGH_RES_MODE_FLAG;
            EfiPrintf(L"High res mode not yet handled\r\n");
            return STATUS_NOT_IMPLEMENTED;
        }

        /* Do we need graphics mode after all? */
        if (!NoGraphics)
        {
            /* Yep -- go allocate it */
            GraphicsConsole = BlMmAllocateHeap(sizeof(*GraphicsConsole));
            if (GraphicsConsole)
            {
                /* Construct it */
                Status = ConsoleGraphicalConstruct(GraphicsConsole);
                if (!NT_SUCCESS(Status))
                {
                    EfiPrintf(L"GFX FAILED: %lx\r\n", Status);
                    BlMmFreeHeap(GraphicsConsole);
                    GraphicsConsole = NULL;
                }
                else
                {
                    /* TEST */
                    RtlFillMemory(GraphicsConsole->FrameBuffer, GraphicsConsole->FrameBufferSize, 0x55);
                }
            }
        }

        /* Are we using something else than the default mode? */
        if (DisplayMode != &ConsoleGraphicalResolutionList[0])
        {
            EfiPrintf(L"Display path not handled\r\n");
            return STATUS_NOT_SUPPORTED;
        }

        /* Mask out all the flags now */
        ConsoleGraphicalResolutionListFlags &= ~(BL_DISPLAY_GRAPHICS_FORCED_VIDEO_MODE_FLAG |
                                                 BL_DISPLAY_GRAPHICS_FORCED_HIGH_RES_MODE_FLAG);
    }

    /* Do we have a graphics console? */
    TextConsole = NULL;
    if (!GraphicsConsole)
    {
        /* Nope -- go allocate a text console */
        TextConsole = BlMmAllocateHeap(sizeof(*TextConsole));
        if (TextConsole)
        {
            /* Construct it */
            Status = ConsoleTextLocalConstruct(TextConsole, TRUE);
            if (!NT_SUCCESS(Status))
            {
                BlMmFreeHeap(TextConsole);
                TextConsole = NULL;
            }
        }
    }

    /* Initialize all globals to NULL */
    DspRemoteInputConsole = NULL;
    DspTextConsole = NULL;
    DspGraphicalConsole = NULL;

    /* If we don't have a text console, go get a remote console */
    RemoteConsole = NULL;
    if (!TextConsole)
    {
        ConsoleCreateRemoteConsole(&RemoteConsole);
    }

    /* Do we have a remote console? */
    if (!RemoteConsole)
    {
        /* Nope -- what about a graphical one? */
        if (GraphicsConsole)
        {
            /* Yes, use it for both graphics and text */
            DspGraphicalConsole = GraphicsConsole;
            DspTextConsole = GraphicsConsole;
        }
        else if (TextConsole)
        {
            /* Nope, but we have a text console */
            DspTextConsole = TextConsole;
        }

        /* Console has been setup */
        return STATUS_SUCCESS;
    }

    /* We have a remote console -- have to figure out how to use it*/
    EfiPrintf(L"Display path not handled\r\n");
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
DsppReinitialize (
    _In_ ULONG Flags
    )
{
    PBL_TEXT_CONSOLE TextConsole;
    PBL_GRAPHICS_CONSOLE GraphicsConsole;
    NTSTATUS Status;
    ULONGLONG GraphicsResolution;
    BOOLEAN HighestMode;
    BL_DISPLAY_MODE CurrentResolution;

    /* Do we have local input yet? */
    if (!DspLocalInputConsole)
    {
        /* Create it now */
        ConsoleCreateLocalInputConsole();
    }

    /* If a graphics console is present without a remote console... */
    TextConsole = NULL;
    if (!(DspRemoteInputConsole) && (DspGraphicalConsole))
    {
        /* Try to create a remote console */
        ConsoleCreateRemoteConsole(&TextConsole);
    }

    /* All good for now */
    Status = STATUS_SUCCESS;

    /* Now check if we were able to create the remote console */
    if (TextConsole)
    {
        EfiPrintf(L"EMS not supported\r\n");
        return STATUS_NOT_IMPLEMENTED;
    }

    /* Set a local for the right cast */
    GraphicsConsole = DspGraphicalConsole;

    /* Nothing to do without a graphics console being reinitialized */
    if (!(Flags & BL_LIBRARY_FLAG_REINITIALIZE_ALL) ||
        !(GraphicsConsole) ||
        !(((PBL_GRAPHICS_CONSOLE_VTABLE)GraphicsConsole->TextConsole.Callbacks)->IsEnabled(GraphicsConsole)))
    {
        EfiPrintf(L"Nothing to do for re-init\r\n");
        return Status;
    }

    /* Check if graphics are disabled in the BCD */
    if (DsppGraphicsDisabledByBcd())
    {
        /* Turn off the graphics console, switching back to text mode */
        Status = ((PBL_GRAPHICS_CONSOLE_VTABLE)GraphicsConsole->TextConsole.Callbacks)->Enable(GraphicsConsole, FALSE);
    }

    /* Check if a custom graphics resolution is set */
    if (MiscGetBootOption(BlpApplicationEntry.BcdData,
                          BcdLibraryInteger_GraphicsResolution))
    {
        /* Check what it's set to */
        Status = BlGetBootOptionInteger(BlpApplicationEntry.BcdData,
                                        BcdLibraryInteger_GraphicsResolution,
                                        &GraphicsResolution);
        if (!NT_SUCCESS(Status))
        {
            return Status;
        }
        
        /* Now check our current graphical resolution */
        Status = ((PBL_GRAPHICS_CONSOLE_VTABLE)GraphicsConsole->TextConsole.Callbacks)->GetGraphicalResolution(GraphicsConsole,
                                                                                                               &CurrentResolution);
        if (!NT_SUCCESS(Status))
        {
            return Status;
        }

        /* Remember that we're forcing a video mode */
        ConsoleGraphicalResolutionListFlags |= BL_DISPLAY_GRAPHICS_FORCED_VIDEO_MODE_FLAG;

        /* Check which resolution to set */
        if (!GraphicsResolution)
        {
            /* 1024x768 */
            EfiPrintf(L"Display selection not yet handled\r\n");
            return STATUS_NOT_IMPLEMENTED;
        }
        else if (GraphicsResolution == 1)
        {
            /* 800x600 */
            EfiPrintf(L"Display selection not yet handled\r\n");
            return STATUS_NOT_IMPLEMENTED;
        }
        else if (GraphicsResolution == 2)
        {
            /* 1024x600 */
            EfiPrintf(L"Display selection not yet handled\r\n");
            return STATUS_NOT_IMPLEMENTED;
        }
    }

    /* Check if the force highest mode setting is present */
    if (MiscGetBootOption(BlpApplicationEntry.BcdData,
                          BcdLibraryBoolean_GraphicsForceHighestMode))
    {
        /* Check what it's set to */
        Status = BlGetBootOptionBoolean(BlpApplicationEntry.BcdData,
                                        BcdLibraryBoolean_GraphicsForceHighestMode,
                                        &HighestMode);
        if ((NT_SUCCESS(Status)) && (HighestMode))
        {
            /* Remember that high rest mode is being forced */
            ConsoleGraphicalResolutionListFlags |= BL_DISPLAY_GRAPHICS_FORCED_HIGH_RES_MODE_FLAG;

            /* Turn it on */
            //((PBL_GRAPHICS_CONSOLE_VTABLE)GraphicsConsole->TextConsole.Callbacks)->SetGraphicalResolution(GraphicsConsole, 0, 0);

            /* All done now */
            ConsoleGraphicalResolutionListFlags |= ~BL_DISPLAY_GRAPHICS_FORCED_HIGH_RES_MODE_FLAG;
            EfiPrintf(L"High res mode not yet handled\r\n");
            Status = STATUS_NOT_IMPLEMENTED;
        }
    }

    /* Return back to the caller */
    return Status;
}

NTSTATUS
BlpDisplayReinitialize (
    VOID
    )
{
    NTSTATUS Status;
    PBL_TEXT_CONSOLE TextConsole;
    PBL_INPUT_CONSOLE InputConsole;

    /* Do we have a local console? */
    InputConsole = DspLocalInputConsole;
    if (InputConsole)
    {
        /* Reinitialize it */
        Status = InputConsole->Callbacks->Reinitialize((PBL_TEXT_CONSOLE)InputConsole);
        if (!NT_SUCCESS(Status))
        {
            return Status;
        }
    }

    /* Do we have a text console? */
    TextConsole = DspTextConsole;
    if (TextConsole)
    {
        /* Reinitialize it */
        Status = TextConsole->Callbacks->Reinitialize(TextConsole);
    }

    /* Return status */
    return Status;
}

NTSTATUS
BlpDisplayInitialize (
    _In_ ULONG Flags
    )
{
    NTSTATUS Status;

    /* Are we resetting or initializing? */
    if (Flags & BL_LIBRARY_FLAG_REINITIALIZE)
    {
        /* This is a reset */
        Status = DsppReinitialize(Flags);
        if (NT_SUCCESS(Status))
        {
            /* Re-initialize the class as well */
            Status = BlpDisplayReinitialize();
        }
    }
    else
    {
        /* Initialize the display */
        Status = DsppInitialize(Flags);
    }

    /* Return display initailziation state */
    return Status;
}

VOID
BlDisplayGetTextCellResolution (
    _Out_ PULONG TextWidth,
    _Out_ PULONG TextHeight
    )
{
    NTSTATUS Status;
    PBL_GRAPHICS_CONSOLE GraphicsConsole;

    /* If the caller doesn't want anything, bail out */
    if (!(TextWidth) || !(TextHeight))
    {
        return;
    }

    /* Do we have a text console? */
    Status = STATUS_UNSUCCESSFUL;
    if (DspTextConsole)
    {
        /* Do we have a graphics console? */
        GraphicsConsole = DspGraphicalConsole;
        if (GraphicsConsole)
        {
            /* Is it currently active? */
            if (((PBL_GRAPHICS_CONSOLE_VTABLE)GraphicsConsole->TextConsole.Callbacks)->IsEnabled(GraphicsConsole))
            {
                /* Yep -- query it */
                EfiPrintf(L"GFX active, not supported query\r\n");
                Status = STATUS_NOT_IMPLEMENTED;
                //Status = ((PBL_GRAPHICS_CONSOLE_VTABLE)GraphicsConsole->TextConsole.Callbacks)->GetTextCellResolution(GraphicsConsole);
            }
        }
    }

    /* Check if we failed to get it from the graphics console */
    if (!NT_SUCCESS(Status))
    {
        /* Set default text size */
        *TextWidth = 8;
        *TextHeight = 8;
    }
}

NTSTATUS
BlDisplaySetScreenResolution (
    VOID
    )
{
    PBL_GRAPHICS_CONSOLE Console;
    NTSTATUS Status;

    /* Assume success */
    Status = STATUS_SUCCESS;

    /* Do we have a graphics console? */
    Console = DspGraphicalConsole;
    if (Console)
    {
        /* Is it currently active? */
        if (((PBL_GRAPHICS_CONSOLE_VTABLE)Console->TextConsole.Callbacks)->IsEnabled(Console))
        {
            /* If so, disable it */
            return ((PBL_GRAPHICS_CONSOLE_VTABLE)Console->TextConsole.Callbacks)->Enable(Console, FALSE);
        }
    }

    /* We should've now fallen back to text mode */
    if (!DspTextConsole)
    {
        /* Then fail, as no display appears active */
        Status = STATUS_UNSUCCESSFUL;
    }

    /* Return back to the caller */
    return Status;
}

NTSTATUS
BlDisplayGetScreenResolution (
    _Out_ PULONG HRes,
    _Out_ PULONG VRes
    )
{
    NTSTATUS Status;
    BL_DISPLAY_MODE Resolution;
    PBL_GRAPHICS_CONSOLE GraphicsConsole;

    /* Assume failure if no consoles are active */
    Status = STATUS_UNSUCCESSFUL;

    /* Do we have a text console? */
    if (DspTextConsole)
    {
        /* Do we have an active graphics console? */
        GraphicsConsole = DspGraphicalConsole;
        if ((GraphicsConsole) &&
            (((PBL_GRAPHICS_CONSOLE_VTABLE)GraphicsConsole->TextConsole.Callbacks)->IsEnabled(GraphicsConsole)))
        {
            /* Get the resolution */
            Status = ((PBL_GRAPHICS_CONSOLE_VTABLE)GraphicsConsole->TextConsole.Callbacks)->GetGraphicalResolution(GraphicsConsole, &Resolution);
            if (NT_SUCCESS(Status))
            {
                /* Return it back to the caller */
                *HRes = Resolution.HRes;
                *VRes = Resolution.VRes;
            }
        }
        else
        {
            /* Return defaults */
            *HRes = 640;
            *VRes = 200;
            Status = STATUS_SUCCESS;
        }
    }

    /* Return if we got a valid resolution back */
    return Status;
}