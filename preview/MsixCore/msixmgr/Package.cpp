#include <windows.h>

#include "Package.hpp"
#include "FilePaths.hpp"
#include "GeneralUtil.hpp"
#include <TraceLoggingProvider.h>
#include "MsixTraceLoggingProvider.hpp"
#include <fstream>
using namespace MsixCoreLib;

//
// Gets the stream of a file.
//
// Parameters:
//   package - The package reader for the app package.
//   name - Name of the file.
//   stream - The stream for the file.
//

HRESULT GetStreamFromFile(IAppxPackageReader* package, LPCWCHAR name, IStream** stream)
{
    *stream = nullptr;

    ComPtr<IAppxFilesEnumerator> files;
    RETURN_IF_FAILED(package->GetPayloadFiles(&files));

    BOOL hasCurrent = FALSE;
    RETURN_IF_FAILED(files->GetHasCurrent(&hasCurrent));
    while (hasCurrent)
    {
        ComPtr<IAppxFile> file;
        RETURN_IF_FAILED(files->GetCurrent(&file));
        Text<WCHAR> fileName;
        file->GetName(&fileName);
        if (wcscmp(fileName.Get(), name) == 0)
        {
            RETURN_IF_FAILED(file->GetStream(stream));
            return S_OK;
        }
        RETURN_IF_FAILED(files->MoveNext(&hasCurrent));
    }
    return S_OK;
}

std::wstring MsixCoreLib::PackageBase::GetResolvedExecutableFilePath()
{
    return FilePathMappings::GetInstance().GetExecutablePath(m_relativeExecutableFilePath, m_packageFullName.c_str());
}

std::wstring PackageBase::GetVersion()
{
    return ConvertVersionToString(m_version);
}

HRESULT PackageBase::ParseManifest(IMsixElement* element)
{
    BOOL hc = FALSE;
    ComPtr<IMsixElementEnumerator> applicationElementEnum;
    RETURN_IF_FAILED(element->GetElements(
        L"/*[local-name()='Package']/*[local-name()='Applications']/*[local-name()='Application']",
        &applicationElementEnum));
    RETURN_IF_FAILED(applicationElementEnum->GetHasCurrent(&hc));

    if (!hc)
    {
        TraceLoggingWrite(g_MsixTraceLoggingProvider,
            "No Application Found",
            TraceLoggingLevel(WINEVENT_LEVEL_ERROR));
        return E_NOT_SET;
    }

    ComPtr<IMsixElement> applicationElement;
    RETURN_IF_FAILED(applicationElementEnum->GetCurrent(&applicationElement));

    Text<wchar_t> executablePath;
    Text<wchar_t> applicationId;
    RETURN_IF_FAILED(applicationElement->GetAttributeValue(L"Executable", &executablePath));
    RETURN_IF_FAILED(applicationElement->GetAttributeValue(L"Id", &applicationId));
    m_relativeExecutableFilePath = executablePath.Get();
    m_applicationId = applicationId.Get();

    ComPtr<IMsixElementEnumerator> visualElementsEnum;
    RETURN_IF_FAILED(applicationElement->GetElements(L"*[local-name()='VisualElements']", &visualElementsEnum));
    RETURN_IF_FAILED(visualElementsEnum->GetHasCurrent(&hc));
    if (!hc)
    {
        TraceLoggingWrite(g_MsixTraceLoggingProvider,
            "No DisplayName Found",
            TraceLoggingLevel(WINEVENT_LEVEL_ERROR));
        return E_NOT_SET;
    }

    ComPtr<IMsixElement> visualElementsElement;
    RETURN_IF_FAILED(visualElementsEnum->GetCurrent(&visualElementsElement));

    Text<wchar_t> displayName;
    RETURN_IF_FAILED(visualElementsElement->GetAttributeValue(L"DisplayName", &displayName));
    m_displayName = displayName.Get();

    Text<WCHAR> logo;
    RETURN_IF_FAILED(visualElementsElement->GetAttributeValue(L"Square150x150Logo", &logo));
    m_relativeLogoPath = logo.Get();
    return S_OK;

    return S_OK;
}

HRESULT PackageBase::ParseManifestCapabilities(IMsixElement* element)
{
    ComPtr<IMsixElementEnumerator> capabilitiesEnum;
    RETURN_IF_FAILED(element->GetElements(
        L"/*[local-name()='Package']/*[local-name()='Capabilities']/*[local-name()='Capability']",
        &capabilitiesEnum));

    BOOL hc = FALSE;
    RETURN_IF_FAILED(capabilitiesEnum->GetHasCurrent(&hc));

    while (hc)
    {
        ComPtr<IMsixElement> capabilityElement;
        RETURN_IF_FAILED(capabilitiesEnum->GetCurrent(&capabilityElement));

        Text<wchar_t> capabilityName;
        RETURN_IF_FAILED(capabilityElement->GetAttributeValue(L"Name", &capabilityName));
        m_capabilities.push_back(capabilityName.Get());

        RETURN_IF_FAILED(capabilitiesEnum->MoveNext(&hc));
    }
    return S_OK;
}

HRESULT Package::MakeFromPackageReader(IAppxPackageReader * packageReader, std::shared_ptr<Package> * packageInfo)
{
    std::shared_ptr<Package> instance = std::make_shared<Package>();
    if (instance == nullptr)
    {
        return E_OUTOFMEMORY;
    }

    instance->m_packageReader = packageReader;

    ComPtr<IAppxManifestReader> manifestReader;
    RETURN_IF_FAILED(packageReader->GetManifest(&manifestReader));
    RETURN_IF_FAILED(instance->SetManifestReader(manifestReader.Get()));

    // Get the number of payload files
    DWORD numberOfPayloadFiles = 0;
    ComPtr<IAppxFilesEnumerator> fileEnum;
    RETURN_IF_FAILED(packageReader->GetPayloadFiles(&fileEnum));

    BOOL hc = FALSE;
    RETURN_IF_FAILED(fileEnum->GetHasCurrent(&hc));
    while (hc)
    {
        numberOfPayloadFiles++;
        RETURN_IF_FAILED(fileEnum->MoveNext(&hc));
    }
    instance->m_numberOfPayloadFiles = numberOfPayloadFiles;

    *packageInfo = instance;

    return S_OK;
}

HRESULT PackageBase::SetManifestReader(IAppxManifestReader * manifestReader)
{
    m_manifestReader = manifestReader;

    // Also fill other fields that come from the manifest reader
    ComPtr<IAppxManifestPackageId> manifestId;
    RETURN_IF_FAILED(manifestReader->GetPackageId(&manifestId));

    Text<WCHAR> publisher;
    RETURN_IF_FAILED(manifestId->GetPublisher(&publisher));
    m_publisher = publisher.Get();

    m_publisherName = m_publisher.substr(m_publisher.find_first_of(L"=") + 1,
        m_publisher.find_first_of(L",") - m_publisher.find_first_of(L"=") - 1);

    RETURN_IF_FAILED(manifestId->GetVersion(&m_version));

    Text<WCHAR> packageFullName;
    RETURN_IF_FAILED(manifestId->GetPackageFullName(&packageFullName));
    m_packageFullName = packageFullName.Get();
    m_packageFamilyName = GetFamilyNameFromFullName(m_packageFullName);

    ComPtr<IMsixDocumentElement> domElement;
    RETURN_IF_FAILED(manifestReader->QueryInterface(UuidOfImpl<IMsixDocumentElement>::iid, reinterpret_cast<void**>(&domElement)));

    ComPtr<IMsixElement> element;
    RETURN_IF_FAILED(domElement->GetDocumentElement(&element));

    RETURN_IF_FAILED(ParseManifest(element.Get()));

    RETURN_IF_FAILED(ParseManifestCapabilities(element.Get()));

    Text<WCHAR> packageFamilyName;
    RETURN_IF_FAILED(manifestId->GetPackageFamilyName(&packageFamilyName));
    if (!m_applicationId.empty() && packageFamilyName.Get() != NULL)
    {
        m_appUserModelId = std::wstring(packageFamilyName.Get()) + L"!" + m_applicationId;
    }
    return S_OK;
}

std::wstring MsixCoreLib::Package::GetResolvedExecutableFilePath()
{
    return FilePathMappings::GetInstance().GetExecutablePath(m_relativeExecutableFilePath, m_packageFullName.c_str());
}

std::unique_ptr<IStream> Package::GetLogo()
{
    if (m_packageReader.Get() == nullptr)
    {
        return nullptr;
    }

    IStream * logoStream;
    if (GetStreamFromFile(m_packageReader.Get(), m_relativeLogoPath.data(), &logoStream) == S_OK)
    {
        return std::unique_ptr<IStream>(logoStream);
    }
    return nullptr;
}

std::unique_ptr<IStream> InstalledPackage::GetLogo()
{
    auto iconPath = m_packageDirectoryPath + m_relativeLogoPath;
    IStream* stream;
    if (SUCCEEDED(CreateStreamOnFileUTF16(iconPath.c_str(), true, &stream)))
    {
        return std::unique_ptr<IStream>(stream);
    }
    return nullptr;
}


HRESULT InstalledPackage::MakeFromManifestReader(const std::wstring & directoryPath, IAppxManifestReader * manifestReader, std::shared_ptr<InstalledPackage> * packageInfo)
{
    std::shared_ptr<InstalledPackage> instance = std::make_shared<InstalledPackage>();
    if (instance == nullptr)
    {
        return E_OUTOFMEMORY;
    }

    RETURN_IF_FAILED(instance->SetManifestReader(manifestReader));
    instance->m_packageDirectoryPath = directoryPath + L"\\";

    *packageInfo = instance;

    return S_OK;
}