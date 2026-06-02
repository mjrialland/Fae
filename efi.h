#ifndef EFI_H
#define EFI_H

#include <stdint.h>

// Basic types
typedef unsigned long long UINTN;
typedef long long          INTN;
typedef unsigned char      BOOLEAN;
typedef uint16_t           CHAR16;
typedef void*              EFI_HANDLE;
typedef UINTN              EFI_STATUS;

#define EFI_SUCCESS           0
#define EFI_ERROR_MASK        (1ULL << 63)
#define EFI_NOT_FOUND         (EFI_ERROR_MASK | 14)
#ifndef NULL
#define NULL ((void*)0)
#endif

typedef enum {
    AllHandles,
    ByRegisterNotify,
    ByProtocol
} EFI_LOCATE_SEARCH_TYPE;

typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} EFI_GUID;

// Table header
typedef struct {
    uint64_t Signature;
    uint32_t Revision;
    uint32_t HeaderSize;
    uint32_t CRC32;
    uint32_t Reserved;
} EFI_TABLE_HEADER;

// Text input
typedef struct {
    uint16_t ScanCode;
    uint16_t UnicodeChar;
} EFI_INPUT_KEY;

struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_INPUT_READ_KEY) (
    struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
    EFI_INPUT_KEY *Key
);

typedef struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    void* Reset;
    EFI_INPUT_READ_KEY ReadKeyStroke;
    void* WaitForKey;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

// Text output
struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_TEXT_STRING) (
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    CHAR16 *String
);
typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_TEXT_CLEAR_SCREEN) (
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This
);

typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void* Reset;
    EFI_TEXT_STRING OutputString;
    void* TestString;
    void* QueryMode;
    void* SetMode;
    void* SetAttribute;
    EFI_TEXT_CLEAR_SCREEN ClearScreen;
    void* SetCursorPosition;
    void* EnableCursor;
    void* Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

// Memory allocation
typedef enum {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress,
    MaxAllocateType
} EFI_ALLOCATE_TYPE;

typedef enum {
    EfiReservedMemoryType,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory,
    EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef struct {
    uint32_t Type;
    uint32_t Padding;
    uint64_t PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

// Protocols: Filesystem
#define EFI_FILE_MODE_READ      0x0000000000000001ULL
#define EFI_FILE_READ_ONLY      0x0000000000000001ULL

struct _EFI_FILE_PROTOCOL;
typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_FILE_OPEN) (
    struct _EFI_FILE_PROTOCOL *This,
    struct _EFI_FILE_PROTOCOL **NewHandle,
    CHAR16 *FileName,
    uint64_t OpenMode,
    uint64_t Attributes
);
typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_FILE_CLOSE) (
    struct _EFI_FILE_PROTOCOL *This
);
typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_FILE_READ) (
    struct _EFI_FILE_PROTOCOL *This,
    UINTN *BufferSize,
    void *Buffer
);
typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_FILE_GET_INFO) (
    struct _EFI_FILE_PROTOCOL *This,
    EFI_GUID *InformationType,
    UINTN *BufferSize,
    void *Buffer
);

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_FILE_SET_POSITION) (
    struct _EFI_FILE_PROTOCOL *This,
    uint64_t Position
);
typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_FILE_GET_POSITION) (
    struct _EFI_FILE_PROTOCOL *This,
    uint64_t *Position
);

typedef struct _EFI_FILE_PROTOCOL {
    uint64_t Revision;
    EFI_FILE_OPEN Open;
    EFI_FILE_CLOSE Close;
    void* Delete;
    EFI_FILE_READ Read;
    void* Write;
    EFI_FILE_GET_POSITION GetPosition;
    EFI_FILE_SET_POSITION SetPosition;
    EFI_FILE_GET_INFO GetInfo;
    void* SetInfo;
    void* Flush;
} EFI_FILE_PROTOCOL;

struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_SIMPLE_FILE_SYSTEM_OPEN_VOLUME) (
    struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
    EFI_FILE_PROTOCOL **Root
);

typedef struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    uint64_t Revision;
    EFI_SIMPLE_FILE_SYSTEM_OPEN_VOLUME OpenVolume;
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

// Protocols: Graphics Output Protocol (GOP)
typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
    PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    uint32_t RedMask;
    uint32_t GreenMask;
    uint32_t BlueMask;
    uint32_t ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    uint32_t Version;
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    EFI_PIXEL_BITMASK PixelInformation;
    uint32_t PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    uint32_t MaxMode;
    uint32_t Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN SizeOfInfo;
    uint64_t FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

struct _EFI_GRAPHICS_OUTPUT_PROTOCOL;
typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_GOP_QUERY_MODE) (
    struct _EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    uint32_t ModeNumber,
    UINTN *SizeOfInfo,
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info
);
typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_GOP_SET_MODE) (
    struct _EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    uint32_t ModeNumber
);

typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_GOP_QUERY_MODE QueryMode;
    EFI_GOP_SET_MODE SetMode;
    void* Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

// Protocols: Loaded Image
typedef struct {
    uint32_t Revision;
    EFI_HANDLE ParentHandle;
    void* SystemTable;
    EFI_HANDLE DeviceHandle;
    void* FilePath;
    void* Reserved;
    uint32_t LoadOptionsSize;
    void* LoadOptions;
    void* ImageBase;
    uint64_t ImageSize;
    uint32_t ImageCodeType;
    uint32_t ImageDataType;
} EFI_LOADED_IMAGE_PROTOCOL;

// Boot Services
typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_ALLOCATE_PAGES) (
    EFI_ALLOCATE_TYPE Type,
    EFI_MEMORY_TYPE MemoryType,
    UINTN Pages,
    uint64_t *Memory
);
typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_FREE_PAGES) (
    uint64_t Memory,
    UINTN Pages
);
typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_GET_MEMORY_MAP) (
    UINTN *MemoryMapSize,
    EFI_MEMORY_DESCRIPTOR *MemoryMap,
    UINTN *MapKey,
    UINTN *DescriptorSize,
    uint32_t *DescriptorVersion
);
typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_LOCATE_HANDLE_BUFFER) (
    uint32_t SearchType,
    EFI_GUID *Protocol,
    void *SearchKey,
    UINTN *NoHandles,
    EFI_HANDLE **Buffer
);
typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_HANDLE_PROTOCOL) (
    EFI_HANDLE Handle,
    EFI_GUID *Protocol,
    void **Interface
);
typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_EXIT_BOOT_SERVICES) (
    EFI_HANDLE ImageHandle,
    UINTN MapKey
);

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_STALL) (
    UINTN Microseconds
);

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_LOCATE_PROTOCOL) (
    EFI_GUID *Protocol,
    void *Registration,
    void **Interface
);

typedef struct _EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER Hdr;
    void* RaiseTPL;
    void* RestoreTPL;
    EFI_ALLOCATE_PAGES AllocatePages;
    void* AllocatePool;
    void* FreePool;
    EFI_FREE_PAGES FreePages;
    EFI_GET_MEMORY_MAP GetMemoryMap;
    void* InstallProtocolInterface;
    void* ReinstallProtocolInterface;
    void* UninstallProtocolInterface;
    EFI_HANDLE_PROTOCOL HandleProtocol;
    void* void_1;
    void* RegisterProtocolNotify;
    void* LocateHandle;
    void* LocateDevicePath;
    void* InstallConfigurationTable;
    void* LoadImage;
    void* StartImage;
    void* Exit;
    void* UnloadImage;
    EFI_EXIT_BOOT_SERVICES ExitBootServices;
    void* GetNextMonotonicCount;
    EFI_STALL Stall;
    void* SetWatchdogTimer;
    void* ConnectController;
    void* DisconnectController;
    void* OpenProtocol;
    void* CloseProtocol;
    void* OpenProtocolInformation;
    void* ProtocolsPerHandle;
    EFI_LOCATE_HANDLE_BUFFER LocateHandleBuffer;
    EFI_LOCATE_PROTOCOL LocateProtocol;
    void* InstallMultipleProtocolInterfaces;
    void* UninstallMultipleProtocolInterfaces;
    void* CalculateCrc32;
    void* CopyMem;
    void* SetMem;
    void* CreateEventEx;
} EFI_BOOT_SERVICES;

// System Table
typedef struct _EFI_SYSTEM_TABLE {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    uint32_t FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    void* RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    void* TableEntries;
} EFI_SYSTEM_TABLE;

// GUID values
static const EFI_GUID EFI_LOADED_IMAGE_PROTOCOL_GUID = {
    0x5B1B31A1, 0x9562, 0x11D2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}
};
static const EFI_GUID EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID = {
    0x0964E5B22, 0x6459, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}
};
static const EFI_GUID EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID = {
    0x9042A9DE, 0x23DC, 0x4A38, {0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A}
};
static const EFI_GUID EFI_FILE_INFO_ID = {
    0x09576E92F, 0x6C3F, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}
};

// File Info structure
typedef struct {
    uint64_t Size;
    uint64_t FileSize;
    uint64_t PhysicalSize;
    uint64_t CreateTime[4];
    uint64_t LastAccessTime[4];
    uint64_t ModificationTime[4];
    uint64_t Attribute;
    CHAR16   FileName[256];
} EFI_FILE_INFO;

// Custom aiOS structures
struct efi_mmap_entry {
    uint64_t phys_addr;
    uint64_t num_bytes;
    uint32_t type; // 1 = Usable RAM, other = reserved/system
};

struct aios_boot_info {
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_bpp;
    
    uint64_t model_addr;
    uint64_t model_size;
    
    uint64_t instruct_addr;
    uint64_t instruct_size;
    
    uint32_t mmap_entries_count;
    struct efi_mmap_entry mmap_entries[128];
};

#endif
