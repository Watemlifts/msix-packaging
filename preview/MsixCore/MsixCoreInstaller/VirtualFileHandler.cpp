#include <windows.h>
#include <iostream>
#include <experimental/filesystem> // C++-standard header file name
#include <filesystem> // Microsoft-specific implementation header file name

#include "VirtualFileHandler.hpp"
#include "GeneralUtil.hpp"
#include "FootprintFiles.hpp"
#include "FilePaths.hpp"
#include "Constants.hpp"

#include "RegistryDevirtualizer.hpp"
#include <TraceLoggingProvider.h>
#include "MsixTraceLoggingProvider.hpp"
#include "Database.hpp"
using namespace MsixCoreLib;

const PCWSTR VirtualFileHandler::HandlerName = L"VirtualFileHandler";

/// Replaces all oldchars in input with newchar
///
/// @param input   - The input string that contains the characters to be changed
/// @param oldchar - Old character that are to be replaced
/// @param newchar - New character that replaces oldchar
void replace(std::wstring& input, const wchar_t oldchar, const wchar_t newchar)
{
    std::size_t found = input.find_first_of(oldchar);
    while (found != std::string::npos)
    {
        input[found] = newchar;
        found = input.find_first_of(oldchar, found + 1);
    }
}

/// Makes a directory, including all parent directories based on the inputted filepath
///
/// @param utf16Path - The filepath to create a directory in utf16
int mkdirp(std::wstring& utf16Path)
{
    replace(utf16Path, L'/', L'\\');
    for (std::size_t i = 3; i < utf16Path.size(); i++) // 3 skips past c:
    {
        if (utf16Path[i] == L'\0')
        {
            break;
        }
        else if (utf16Path[i] == L'\\')
        {
            // Temporarily set string to terminate at the '\' character
            // to obtain name of the subdirectory to create
            utf16Path[i] = L'\0';

            if (!CreateDirectoryW(utf16Path.c_str(), nullptr))
            {
                int lastError = static_cast<int>(GetLastError());

                // It is normal for CreateDirectory to fail if the subdirectory
                // already exists.  Other errors should not be ignored.
                if (lastError != ERROR_ALREADY_EXISTS)
                {
                    return lastError;
                }
            }
            // Restore original string
            utf16Path[i] = L'\\';
        }
    }
    return 0;
}


HRESULT VirtualFileHandler::ExecuteForAddRequest()
{
    // loop over all vfs files
   // RETURN_IF_FAILED(CopyVfsFileIfNecessary());
    return S_OK;
}

HRESULT VirtualFileHandler::RemoveVfsFiles()
{
    std::wstring blockMapPath = m_msixRequest->GetPackageDirectoryPath() + blockMapFile;
    ComPtr<IStream> stream;
    RETURN_IF_FAILED(CreateStreamOnFileUTF16(blockMapPath.c_str(), true /*forRead*/, &stream));

    ComPtr<IAppxFactory> appxFactory;
    RETURN_IF_FAILED(CoCreateAppxFactoryWithHeap(MyAllocate, MyFree, m_msixRequest->GetValidationOptions(), &appxFactory));

    ComPtr<IAppxBlockMapReader> blockMapReader;
    RETURN_IF_FAILED(appxFactory->CreateBlockMapReader(stream.Get(), &blockMapReader));

    ComPtr<IAppxBlockMapFilesEnumerator> files;
    RETURN_IF_FAILED(blockMapReader->GetFiles(&files));

    BOOL hasCurrent = FALSE;
    RETURN_IF_FAILED(files->GetHasCurrent(&hasCurrent));

    while (hasCurrent)
    {
        ComPtr<IAppxBlockMapFile> file;
        RETURN_IF_FAILED(files->GetCurrent(&file));

        //if it's a VFS file, delete it from the local location
        Text<WCHAR> name;
        RETURN_IF_FAILED(file->GetName(&name));
        std::wstring nameStr = name.Get();
        if (nameStr.find(L"VFS") != std::wstring::npos)
        {
            RETURN_IF_FAILED(RemoveVfsFile(nameStr));
        }

        RETURN_IF_FAILED(files->MoveNext(&hasCurrent));
    }

    return S_OK;
}

HRESULT VirtualFileHandler::ExecuteForRemoveRequest()
{
    bool isInstalledForOtherUsers = false;
    HRESULT hrIsInstalled = Database::IsInstalledForAnyOtherUser(m_msixRequest->GetPackageInfo()->GetPackageFullName().c_str(), isInstalledForOtherUsers);
    if (FAILED(hrIsInstalled) || isInstalledForOtherUsers)
    {
        TraceLoggingWrite(g_MsixTraceLoggingProvider,
            "Not removing the package's files; another user has it installed",
            TraceLoggingLevel(WINEVENT_LEVEL_WARNING),
            TraceLoggingValue(hrIsInstalled, "HR"));
        return hrIsInstalled;
    }

    HRESULT hrRemoveVfsFiles = RemoveVfsFiles();
    if (FAILED(hrRemoveVfsFiles))
    {
        TraceLoggingWrite(g_MsixTraceLoggingProvider,
            "Unable to remove VFS files",
            TraceLoggingLevel(WINEVENT_LEVEL_WARNING),
            TraceLoggingValue(hrRemoveVfsFiles, "HR"));
    }

    // First release manifest so we can delete the file.
    m_msixRequest->GetPackageInfo()->ReleaseManifest();

    std::error_code error;
    auto packageDirectoryPath = m_msixRequest->GetPackageDirectoryPath();
    uintmax_t numRemoved = std::experimental::filesystem::remove_all(packageDirectoryPath, error);

    TraceLoggingWrite(g_MsixTraceLoggingProvider,
        "Removed directory",
        TraceLoggingValue(packageDirectoryPath.c_str(), "PackageDirectoryPath"),
        TraceLoggingValue(error.value(), "Error"),
        TraceLoggingValue(numRemoved, "NumRemoved"));

    return S_OK;
}

HRESULT VirtualFileHandler::CreateHandler(MsixRequest * msixRequest, IPackageHandler ** instance)
{
    std::unique_ptr<VirtualFileHandler> localInstance(new VirtualFileHandler(msixRequest));
    if (localInstance == nullptr)
    {
        return E_OUTOFMEMORY;
    }
    *instance = localInstance.release();

    return S_OK;
}

HRESULT GetFileVersion(std::wstring file, _Out_ UINT64& version, _Out_ bool& isUnversioned)
{
    isUnversioned = true;
    DWORD size = GetFileVersionInfoSize(file.c_str(), nullptr);
    if (size == 0)
    {
        DWORD error = GetLastError();
        if (error == ERROR_RESOURCE_DATA_NOT_FOUND || error == ERROR_RESOURCE_TYPE_NOT_FOUND)
        {
            // Does not have version info, isUnversioned = true was set earlier.
            version = 0;
            return S_OK;
        }
        RETURN_IF_FAILED(HRESULT_FROM_WIN32(error));
    }

    std::unique_ptr<BYTE[]> versionInfo(new BYTE[size]);
    if (!GetFileVersionInfo(file.c_str(), 0, size, versionInfo.get()))
    {
        RETURN_IF_FAILED(HRESULT_FROM_WIN32(GetLastError()));
    }

    VS_FIXEDFILEINFO* fileInfo = nullptr;
    UINT fileInfoLength = 0;
    if (!VerQueryValue(versionInfo.get(), TEXT("\\"), (LPVOID*)&fileInfo, &fileInfoLength))
    {
        RETURN_IF_FAILED(HRESULT_FROM_WIN32(GetLastError()));
    }

    version = ((UINT64)(fileInfo->dwFileVersionMS) << 32) + fileInfo->dwFileVersionLS;
    isUnversioned = false;

    return S_OK;
}

HRESULT IsFileModified(std::wstring file, _Out_ bool& isModified)
{
    isModified = false;
    // Since we follow MSI file rules, the file is considered modified by MSI if modified date is more than 2 seconds later than creation date 
    const int TwoSecondsInFileTimeIncrements = 20000000; // FILETIME is in 100 nanosecond increments

    WIN32_FILE_ATTRIBUTE_DATA strData;
    if (!GetFileAttributesEx(file.c_str(), GetFileExInfoStandard, (LPVOID)&strData))
    {
        RETURN_IF_FAILED(HRESULT_FROM_WIN32(GetLastError()));
    }

    if ((strData.ftLastWriteTime.dwHighDateTime > strData.ftCreationTime.dwHighDateTime))
    {
        // This copies the MSI bug where as long as LastWrite dwHighDateTime is greater, it's considered modified, even if
        // the Create/LastWrite are within 2 seconds of one another (by virtue of creation dwLowDateTime being near UINT_MAX)
        isModified = true;
    }
    else if (strData.ftLastWriteTime.dwHighDateTime == strData.ftCreationTime.dwHighDateTime)
    {
        if (strData.ftLastWriteTime.dwLowDateTime > strData.ftCreationTime.dwLowDateTime &&
            strData.ftLastWriteTime.dwLowDateTime - strData.ftCreationTime.dwLowDateTime > TwoSecondsInFileTimeIncrements)
        {
            isModified = true;
        }
    }

    return S_OK;
}

HRESULT VirtualFileHandler::NeedToCopyFile(std::wstring sourceFullPath, std::wstring targetFullPath, _Out_ bool &needToCopyFile)
{
    needToCopyFile = false;

    bool targetFileExists = false;
    RETURN_IF_FAILED(FileExists(targetFullPath, targetFileExists));

    if (!targetFileExists)
    {
        needToCopyFile = true;
        TraceLoggingWrite(g_MsixTraceLoggingProvider,
            "Need to copy file because target doesn't exist",
            TraceLoggingValue(targetFullPath.c_str(), "TargetFullPath"));
        return S_OK;
    }

    // Whether we overwrite existing files or keep the existing file as-is follows MSI file versioning rules
    UINT64 targetFileVersion = 0;
    bool targetFileIsUnversioned = false;
    bool targetFileIsModified = false;
    RETURN_IF_FAILED(GetFileVersion(targetFullPath, targetFileVersion, targetFileIsUnversioned));
    if (targetFileIsUnversioned)
    {
        RETURN_IF_FAILED(IsFileModified(targetFullPath, targetFileIsModified));
    }

    UINT64 sourceFileVersion = 0;
    bool sourceFileIsUnversioned = false;
    RETURN_IF_FAILED(GetFileVersion(sourceFullPath, sourceFileVersion, sourceFileIsUnversioned));

    std::wstring targetVersionString = ConvertVersionToString(targetFileVersion);
    std::wstring sourceVersionString = ConvertVersionToString(sourceFileVersion);

    TraceLoggingWrite(g_MsixTraceLoggingProvider,
        "Target Exists, file versioning information",
        TraceLoggingValue(targetFullPath.c_str(), "TargetFullPath"),
        TraceLoggingValue(targetVersionString.c_str(), "TargetFileVersion"),
        TraceLoggingValue(targetFileIsUnversioned, "TargetFileIsUnversioned"),
        TraceLoggingValue(targetFileIsModified, "TargetFileIsModified"),
        TraceLoggingValue(sourceFullPath.c_str(), "SourceFullPath"),
        TraceLoggingValue(sourceVersionString.c_str(), "SourceFileVersion"),
        TraceLoggingValue(sourceFileIsUnversioned, "SourceFileIsUnversioned"));

    if (targetFileIsUnversioned && !sourceFileIsUnversioned)
    {
        // Versioned file should overwrite unversioned file
        needToCopyFile = true;
        return S_OK;
    }

    if (targetFileIsUnversioned && !targetFileIsModified)
    {
        // Unversioned file is treated as userData; we do not want to delete user customizations if they made changes to the file
        // Existing file is unversioned and unmodified -- this is treated as unmodified user data so we can overwrite it
        needToCopyFile = true;
        return S_OK;
    }

    if (targetFileVersion < sourceFileVersion)
    {
        // Higher version wins
        needToCopyFile = true;
        return S_OK;
    }

    return S_OK;
}

HRESULT VirtualFileHandler::CopyVfsFileIfNecessary(std::wstring sourceFullPath, std::wstring targetFullPath)
{
    TraceLoggingWrite(g_MsixTraceLoggingProvider,
        "CopyVfsFile",
        TraceLoggingValue(sourceFullPath.c_str(), "Source"),
        TraceLoggingValue(targetFullPath.c_str(), "Target"));

    bool needToCopyFile = false;
    RETURN_IF_FAILED(NeedToCopyFile(sourceFullPath, targetFullPath, needToCopyFile));

    if (needToCopyFile)
    {
        HRESULT hrMkdir = HRESULT_FROM_WIN32(mkdirp(targetFullPath));
        if (FAILED(hrMkdir))
        {
            TraceLoggingWrite(g_MsixTraceLoggingProvider,
                "Unable to create directory for copying file",
                TraceLoggingLevel(WINEVENT_LEVEL_WARNING),
                TraceLoggingValue(targetFullPath.c_str(), "FullPath"),
                TraceLoggingValue(hrMkdir, "HR"));
        }
        else if (!CopyFile(sourceFullPath.c_str(), targetFullPath.c_str(), FALSE /*failIfExists*/))
        {
            DWORD error = GetLastError();
            TraceLoggingWrite(g_MsixTraceLoggingProvider,
                "Unable to Copy file",
                TraceLoggingLevel(WINEVENT_LEVEL_WARNING),
                TraceLoggingValue(targetFullPath.c_str(), "FullPath"),
                TraceLoggingValue(error, "error"));
        }
    }

    return S_OK;
}

HRESULT VirtualFileHandler::RemoveVfsFile(std::wstring fileName)
{
    TraceLoggingWrite(g_MsixTraceLoggingProvider,
        "RemoveVfsFile",
        TraceLoggingValue(fileName.c_str(), "FileName"));

    std::wstring fullPath;
    if (FAILED(ConvertVfsNameToFullPath(fileName, fullPath)))
    {
        return S_OK;
    }

    if (!DeleteFile(fullPath.c_str()))
    {
        TraceLoggingWrite(g_MsixTraceLoggingProvider,
            "Unable to Delete file",
            TraceLoggingLevel(WINEVENT_LEVEL_WARNING),
            TraceLoggingValue(fullPath.c_str(), "FullPath"),
            TraceLoggingValue(GetLastError(), "error"));
    }

    MsixCoreLib_GetPathParent(fullPath);

    // instead of checking if the directory is empty, just try to delete it.
    // if it's not empty it'll fail with expected error code that we can ignore
    if (!RemoveDirectory(fullPath.c_str()))
    {
        DWORD error = GetLastError();
        if (error != ERROR_DIR_NOT_EMPTY)
        {
            TraceLoggingWrite(g_MsixTraceLoggingProvider,
                "Unable to Delete directory",
                TraceLoggingLevel(WINEVENT_LEVEL_WARNING),
                TraceLoggingValue(fullPath.c_str(), "FullPath"),
                TraceLoggingValue(GetLastError(), "error"));
        }
    }

    return S_OK;
}

HRESULT VirtualFileHandler::ConvertVfsNameToFullPath(std::wstring fileName, std::wstring& fileFullPath)
{
    //The following code gets remainingFilePath from "VFS\FirstDir\...\file.ext" to "\...\file.ext"
    std::wstring remainingFilePath = fileName;
    MsixCoreLib_GetPathChild(remainingFilePath); // remove the VFS directory

    std::map<std::wstring, std::wstring> map = FilePathMappings::GetInstance().GetMap();
    for (auto& pair : map)
    {
        if (remainingFilePath.find(pair.first) != std::wstring::npos)
        {
            MsixCoreLib_GetPathChild(remainingFilePath); // remove the FirstDir directory.

            // Pre-pend the VFS target directory to obtain the full path for the target location
            fileFullPath = pair.second + std::wstring(L"\\") + remainingFilePath;

            //Stop looping through the list
            return S_OK;
        }
    }

    TraceLoggingWrite(g_MsixTraceLoggingProvider,
        "Could not find VFS mapping",
        TraceLoggingLevel(WINEVENT_LEVEL_WARNING),
        TraceLoggingValue(fileName.c_str(), "FileName"));

    return E_NOT_SET;
}

HRESULT VirtualFileHandler::CopyVfsFileToLocal(std::wstring fileName)
{
    TraceLoggingWrite(g_MsixTraceLoggingProvider,
        "CopyVfsFileToLocal",
        TraceLoggingValue(fileName.c_str(), "FileName"));

    std::wstring sourceFullPath = m_msixRequest->GetPackageDirectoryPath() + std::wstring(L"\\") + fileName;

    std::wstring targetFullPath;
    if (FAILED(ConvertVfsNameToFullPath(fileName, targetFullPath)))
    {
        return S_OK;
    }

    RETURN_IF_FAILED(CopyVfsFileIfNecessary(sourceFullPath, targetFullPath));

    return S_OK;
}