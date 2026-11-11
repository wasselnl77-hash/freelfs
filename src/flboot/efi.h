/* The parts of the UEFI specification flboot uses, written from the spec.
 * SPDX-License-Identifier: MIT */
#ifndef FLBOOT_EFI_H
#define FLBOOT_EFI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EFIAPI __attribute__((ms_abi))

typedef uint64_t UINTN;
typedef int64_t INTN;
typedef uint64_t UINT64;
typedef uint32_t UINT32;
typedef uint16_t UINT16;
typedef uint8_t UINT8;
typedef uint16_t CHAR16; /* with -fshort-wchar, L"..." literals are CHAR16 */
typedef bool BOOLEAN;
typedef UINTN EFI_STATUS;
typedef void *EFI_HANDLE;
typedef void *EFI_EVENT;
typedef UINT64 EFI_PHYSICAL_ADDRESS;

#define EFI_ERROR_BIT (1ULL << 63)
#define EFI_SUCCESS 0
#define EFI_LOAD_ERROR (EFI_ERROR_BIT | 1)
#define EFI_INVALID_PARAMETER (EFI_ERROR_BIT | 2)
#define EFI_UNSUPPORTED (EFI_ERROR_BIT | 3)
#define EFI_BUFFER_TOO_SMALL (EFI_ERROR_BIT | 5)
#define EFI_NOT_READY (EFI_ERROR_BIT | 6)
#define EFI_NOT_FOUND (EFI_ERROR_BIT | 14)
#define EFI_ERROR(s) (((s) & EFI_ERROR_BIT) != 0)

typedef struct {
	UINT32 Data1;
	UINT16 Data2, Data3;
	UINT8 Data4[8];
} EFI_GUID;

typedef struct {
	UINT64 Signature;
	UINT32 Revision, HeaderSize, CRC32, Reserved;
} EFI_TABLE_HEADER;

/* ---- console ---------------------------------------------------------------- */

typedef struct {
	UINT16 ScanCode;
	CHAR16 UnicodeChar;
} EFI_INPUT_KEY;

#define SCAN_UP 0x01
#define SCAN_DOWN 0x02
#define SCAN_ESC 0x17

typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
	EFI_STATUS(EFIAPI *Reset)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *, BOOLEAN);
	EFI_STATUS(EFIAPI *ReadKeyStroke)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *, EFI_INPUT_KEY *);
	EFI_EVENT WaitForKey;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

typedef struct {
	INTN MaxMode, Mode, Attribute, CursorColumn, CursorRow;
	BOOLEAN CursorVisible;
} SIMPLE_TEXT_OUTPUT_MODE;

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
	EFI_STATUS(EFIAPI *Reset)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, BOOLEAN);
	EFI_STATUS(EFIAPI *OutputString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, const CHAR16 *);
	EFI_STATUS(EFIAPI *TestString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, const CHAR16 *);
	EFI_STATUS(EFIAPI *QueryMode)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, UINTN, UINTN *, UINTN *);
	EFI_STATUS(EFIAPI *SetMode)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, UINTN);
	EFI_STATUS(EFIAPI *SetAttribute)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, UINTN);
	EFI_STATUS(EFIAPI *ClearScreen)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *);
	EFI_STATUS(EFIAPI *SetCursorPosition)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, UINTN, UINTN);
	EFI_STATUS(EFIAPI *EnableCursor)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, BOOLEAN);
	SIMPLE_TEXT_OUTPUT_MODE *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* ---- boot services ------------------------------------------------------------ */

typedef enum { AllocateAnyPages, AllocateMaxAddress, AllocateAddress } EFI_ALLOCATE_TYPE;
typedef enum { EfiReservedMemoryType, EfiLoaderCode, EfiLoaderData } EFI_MEMORY_TYPE;
typedef enum { TimerCancel, TimerPeriodic, TimerRelative } EFI_TIMER_DELAY;

#define EVT_TIMER 0x80000000

typedef struct EFI_DEVICE_PATH_PROTOCOL {
	UINT8 Type, SubType;
	UINT8 Length[2];
} EFI_DEVICE_PATH_PROTOCOL;

typedef void *EFI_UNUSED; /* a boot service flboot does not call */

typedef struct {
	EFI_TABLE_HEADER Hdr;
	EFI_UNUSED RaiseTPL, RestoreTPL;
	EFI_STATUS(EFIAPI *AllocatePages)(EFI_ALLOCATE_TYPE, EFI_MEMORY_TYPE, UINTN, EFI_PHYSICAL_ADDRESS *);
	EFI_STATUS(EFIAPI *FreePages)(EFI_PHYSICAL_ADDRESS, UINTN);
	EFI_UNUSED GetMemoryMap;
	EFI_STATUS(EFIAPI *AllocatePool)(EFI_MEMORY_TYPE, UINTN, void **);
	EFI_STATUS(EFIAPI *FreePool)(void *);
	EFI_STATUS(EFIAPI *CreateEvent)(UINT32, UINTN, void *, void *, EFI_EVENT *);
	EFI_STATUS(EFIAPI *SetTimer)(EFI_EVENT, EFI_TIMER_DELAY, UINT64);
	EFI_STATUS(EFIAPI *WaitForEvent)(UINTN, EFI_EVENT *, UINTN *);
	EFI_UNUSED SignalEvent;
	EFI_STATUS(EFIAPI *CloseEvent)(EFI_EVENT);
	EFI_UNUSED CheckEvent;
	EFI_UNUSED InstallProtocolInterface, ReinstallProtocolInterface, UninstallProtocolInterface;
	EFI_STATUS(EFIAPI *HandleProtocol)(EFI_HANDLE, const EFI_GUID *, void **);
	EFI_UNUSED Reserved, RegisterProtocolNotify, LocateHandle, LocateDevicePath;
	EFI_UNUSED InstallConfigurationTable;
	EFI_STATUS(EFIAPI *LoadImage)(BOOLEAN, EFI_HANDLE, EFI_DEVICE_PATH_PROTOCOL *, void *, UINTN,
	                              EFI_HANDLE *);
	EFI_STATUS(EFIAPI *StartImage)(EFI_HANDLE, UINTN *, CHAR16 **);
	EFI_STATUS(EFIAPI *Exit)(EFI_HANDLE, EFI_STATUS, UINTN, CHAR16 *);
	EFI_STATUS(EFIAPI *UnloadImage)(EFI_HANDLE);
	EFI_UNUSED ExitBootServices, GetNextMonotonicCount;
	EFI_STATUS(EFIAPI *Stall)(UINTN);
	EFI_STATUS(EFIAPI *SetWatchdogTimer)(UINTN, UINT64, UINTN, CHAR16 *);
	EFI_UNUSED ConnectController, DisconnectController;
	EFI_UNUSED OpenProtocol, CloseProtocol, OpenProtocolInformation;
	EFI_UNUSED ProtocolsPerHandle;
	EFI_STATUS(EFIAPI *LocateHandleBuffer)(UINT32 SearchType, const EFI_GUID *, void *, UINTN *, EFI_HANDLE **);
	EFI_STATUS(EFIAPI *LocateProtocol)(const EFI_GUID *, void *, void **);
	EFI_UNUSED InstallMultipleProtocolInterfaces, UninstallMultipleProtocolInterfaces;
	EFI_UNUSED CalculateCrc32;
	void(EFIAPI *CopyMem)(void *, const void *, UINTN);
	void(EFIAPI *SetMem)(void *, UINTN, UINT8);
	EFI_UNUSED CreateEventEx;
} EFI_BOOT_SERVICES;

typedef struct {
	EFI_TABLE_HEADER Hdr;
	EFI_UNUSED GetTime, SetTime, GetWakeupTime, SetWakeupTime;
	EFI_UNUSED SetVirtualAddressMap, ConvertPointer;
	EFI_UNUSED GetVariable, GetNextVariableName, SetVariable;
	EFI_UNUSED GetNextHighMonotonicCount;
	void(EFIAPI *ResetSystem)(UINT32, EFI_STATUS, UINTN, void *);
} EFI_RUNTIME_SERVICES;

#define EfiResetCold 0
#define EfiResetShutdown 2

typedef struct {
	EFI_TABLE_HEADER Hdr;
	CHAR16 *FirmwareVendor;
	UINT32 FirmwareRevision;
	EFI_HANDLE ConsoleInHandle;
	EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;
	EFI_HANDLE ConsoleOutHandle;
	EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
	EFI_HANDLE StandardErrorHandle;
	EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
	EFI_RUNTIME_SERVICES *RuntimeServices;
	EFI_BOOT_SERVICES *BootServices;
	UINTN NumberOfTableEntries;
	void *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* ---- loaded image, file system ---------------------------------------------------- */

#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
	{0x5b1b31a1, 0x9562, 0x11d2, {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}}

typedef struct {
	UINT32 Revision;
	EFI_HANDLE ParentHandle;
	EFI_SYSTEM_TABLE *SystemTable;
	EFI_HANDLE DeviceHandle;
	EFI_DEVICE_PATH_PROTOCOL *FilePath;
	void *Reserved;
	UINT32 LoadOptionsSize;
	void *LoadOptions;
	void *ImageBase;
	UINT64 ImageSize;
	EFI_MEMORY_TYPE ImageCodeType, ImageDataType;
	EFI_UNUSED Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
	{0x964e5b22, 0x6459, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}}
#define EFI_FILE_INFO_GUID \
	{0x09576e92, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}}

#define EFI_FILE_MODE_READ 1ULL
#define EFI_FILE_DIRECTORY 0x10ULL

typedef struct {
	UINT16 Year;
	UINT8 Month, Day, Hour, Minute, Second, Pad1;
	UINT32 Nanosecond;
	int16_t TimeZone;
	UINT8 Daylight, Pad2;
} EFI_TIME;

typedef struct {
	UINT64 Size, FileSize, PhysicalSize;
	EFI_TIME CreateTime, LastAccessTime, ModificationTime;
	UINT64 Attribute;
	CHAR16 FileName[];
} EFI_FILE_INFO;

typedef struct EFI_FILE_PROTOCOL {
	UINT64 Revision;
	EFI_STATUS(EFIAPI *Open)(struct EFI_FILE_PROTOCOL *, struct EFI_FILE_PROTOCOL **,
	                         const CHAR16 *, UINT64, UINT64);
	EFI_STATUS(EFIAPI *Close)(struct EFI_FILE_PROTOCOL *);
	EFI_UNUSED Delete;
	EFI_STATUS(EFIAPI *Read)(struct EFI_FILE_PROTOCOL *, UINTN *, void *);
	EFI_UNUSED Write, GetPosition, SetPosition;
	EFI_STATUS(EFIAPI *GetInfo)(struct EFI_FILE_PROTOCOL *, const EFI_GUID *, UINTN *, void *);
} EFI_FILE_PROTOCOL;

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
	UINT64 Revision;
	EFI_STATUS(EFIAPI *OpenVolume)(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *, EFI_FILE_PROTOCOL **);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

/* ---- graphics ------------------------------------------------------------------ */

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
	{0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}}

typedef enum {
	PixelRedGreenBlueReserved8BitPerColor,
	PixelBlueGreenRedReserved8BitPerColor,
	PixelBitMask,
	PixelBltOnly,
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
	UINT32 Version, HorizontalResolution, VerticalResolution;
	EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
	UINT32 RedMask, GreenMask, BlueMask, ReservedMask;
	UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
	UINT32 MaxMode, Mode;
	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
	UINTN SizeOfInfo;
	EFI_PHYSICAL_ADDRESS FrameBufferBase;
	UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
	EFI_STATUS(EFIAPI *QueryMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *, UINT32, UINTN *,
	                              EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **);
	EFI_STATUS(EFIAPI *SetMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *, UINT32);
	EFI_UNUSED Blt;
	EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

#endif
