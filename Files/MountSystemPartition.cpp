#include <Windows.h>
#include <winternl.h>
#include <stdio.h>

NTSTATUS(*ZWQuerySystemInformation)(ULONG, PVOID, ULONG, PULONG) = nullptr;

#define BOOTMGFW_PATH "EFI\\Microsoft\\Boot\\bootmgfw.efi"

struct SystemSystemPartitionInformation
{
	char pad1[16];
	wchar_t buffer[32];
};

char* GetVolumeMountPoint()
{
	static char driveString[4] = "A:\\";

	constexpr char numAlphabet = 26;
	static char drive = -1;

	if (drive != -1)
		return driveString;

	DWORD drives = GetLogicalDrives();
	for (char i = 0; i < numAlphabet; ++i)
	{
		char bit = (drives >> i) & 1;
		if (!bit)
		{
			drive = i;
			break;
		}
	}

	if (drive == -1)
		return nullptr;

	driveString[0] += drive;
	return driveString;
}

bool FindNTDLLFunctions()
{
	HMODULE ntdll = LoadLibraryA("ntdll.dll");
	if (!ntdll)
	{
		printf("Failed to load ntdll\n");
		return false;
	}

	ZWQuerySystemInformation = reinterpret_cast<decltype(ZWQuerySystemInformation)>(
		GetProcAddress(ntdll, "ZwQuerySystemInformation"));

	if (!ZWQuerySystemInformation)
	{
		printf("Failed to find ZWQuerySystemInformation\n");
		return false;
	}

	return true;
}

HANDLE OpenSystemPartitionVolume()
{
	SystemSystemPartitionInformation info{ 0 };
	ULONG returnLength = 0;
	NTSTATUS status = ZWQuerySystemInformation(0x62, &info, sizeof(info), &returnLength);

	if (NT_ERROR(status))
	{
		printf("Failed to get partition information (ZWQuerySystemInformation)\n");
		return INVALID_HANDLE_VALUE;
	}
	
	/*
		info.buffer = L"\\Device\\(SomeVolumeHere)
		(https://stackoverflow.com/a/31017010)
	*/
	wchar_t* devicePath = wcsstr(info.buffer + 1, L"\\");
	if (!devicePath)
	{
		printf("Failed to find valid device path\n");
		return INVALID_HANDLE_VALUE;
	}
	
	wchar_t finalPath[256] = L"\\\\?";
	wcscat_s(finalPath, devicePath);
	HANDLE file = CreateFileW(finalPath, FILE_GENERIC_READ, FILE_SHARE_WRITE | 
		FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);

	return file;
}

bool UnMountSystemPartition()
{
	return DeleteVolumeMountPointA(GetVolumeMountPoint());
}

bool MountVolume(PARTITION_INFORMATION_EX* partitionInfo)
{
	char volumeName[256]{ 0 };
	sprintf_s(volumeName, "\\\\?\\Volume{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}\\",
		partitionInfo->Gpt.PartitionId.Data1,
		partitionInfo->Gpt.PartitionId.Data2,
		partitionInfo->Gpt.PartitionId.Data3,
		partitionInfo->Gpt.PartitionId.Data4[0],
		partitionInfo->Gpt.PartitionId.Data4[1],
		partitionInfo->Gpt.PartitionId.Data4[2],
		partitionInfo->Gpt.PartitionId.Data4[3],
		partitionInfo->Gpt.PartitionId.Data4[4],
		partitionInfo->Gpt.PartitionId.Data4[5],
		partitionInfo->Gpt.PartitionId.Data4[6],
		partitionInfo->Gpt.PartitionId.Data4[7]);

	char* volume = GetVolumeMountPoint();
	if (!volume)
	{
		printf("Failed to find volume mount point\n");
		return false;
	}

	if (!SetVolumeMountPointA(GetVolumeMountPoint(), volumeName))
	{
		printf("Failed to set volume mount point\n");
		return false;
	}

	return true;
}

bool MountSystemPartition()
{
	if (!FindNTDLLFunctions())
	{
		printf("Failed to find ntdll functions\n");
		return false;
	}

	HANDLE file = OpenSystemPartitionVolume();
	if (file == INVALID_HANDLE_VALUE)
	{
		printf("Failed to open system partition volume\n");
		return false;
	}

	PARTITION_INFORMATION_EX partitionInfo{};
	DWORD bytes = 0;
	if (!DeviceIoControl(file, IOCTL_DISK_GET_PARTITION_INFO_EX, nullptr,
		0, &partitionInfo, sizeof(partitionInfo), &bytes, nullptr))
	{
		printf("Failed to get partition information (DeviceIoControl)\n");
		CloseHandle(file);
		return false;
	}

	CloseHandle(file);

	return MountVolume(&partitionInfo);
}

HANDLE OpenBootmgfw()
{
	char* volume = GetVolumeMountPoint();
	size_t size = strlen(volume) + sizeof(BOOTMGFW_PATH);
	char* bootmgfwPath = new char[size];

	if (!bootmgfwPath)
	{
		printf("Failed to attain bootmgfw.efi path\n");
		return INVALID_HANDLE_VALUE;
	}

	ZeroMemory(bootmgfwPath, size);

	strcpy_s(bootmgfwPath, size, volume);
	strcat_s(bootmgfwPath, size, BOOTMGFW_PATH);

	HANDLE file = CreateFileA(bootmgfwPath, FILE_GENERIC_READ,
		FILE_SHARE_WRITE | FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);

	delete[] bootmgfwPath;

	if (file == INVALID_HANDLE_VALUE)
	{
		printf("Failed to open bootmgfw.efi\n");
		return INVALID_HANDLE_VALUE;
	}

	return file;
}

int main()
{
	if (!MountSystemPartition())
	{
		printf("Failed to mount system partition\n");
		return -1;
	}

	HANDLE file = OpenBootmgfw();
	if (file == INVALID_HANDLE_VALUE)
	{
		printf("Failed to open bootmgfw.efi\n");
		UnMountSystemPartition();
		return -1;
	}

	WORD signature = 0;
	DWORD bytesRead = 0;
	if (!ReadFile(file, &signature, 2, &bytesRead, nullptr) || bytesRead != 2)
	{
		printf("Failed to read bootmgfw.efi\n");
		CloseHandle(file);
		UnMountSystemPartition();
		return -1;
	}

	if (signature == IMAGE_DOS_SIGNATURE)
	{
		printf("Correctly read bootmgfw.efi\n");
	}
	else
	{
		printf("Incorrectly read bootmgfw.efi\n");
		CloseHandle(file);
		UnMountSystemPartition();
		return -1;
	}

	CloseHandle(file);
	UnMountSystemPartition();

	return 0;
}