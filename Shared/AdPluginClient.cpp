#include "AdPluginStdAfx.h"

// Internet / FTP
#include <wininet.h>

// IP adapter
#include <iphlpapi.h>


#include "AdPluginClient.h"
#include "AdPluginSettings.h"
#ifdef SUPPORT_FILTER
 #include "AdPluginFilterClass.h"
#endif
#include "AdPluginClientFactory.h"
#include "AdPluginDictionary.h"
#include "AdPluginSha1.h"
#include "AdPluginHttpRequest.h"
#include "AdPluginMutex.h"

#if (defined PRODUCT_ADBLOCKER)
 #include "../AdBlocker/AdBlocker.h"
#elif (defined PRODUCT_DOWNLOADHELPER)
 #include "../DownloadHelper/DownloadHelper.h"
#endif

#include "AdPluginClass.h"
#include "AdPluginProfiler.h"

// IP adapter
#pragma comment(lib, "IPHLPAPI.lib")

// IE functions
#pragma comment(lib, "iepmapi.lib")

// Internet / FTP
#pragma comment(lib, "wininet.lib")


CComAutoCriticalSection CPluginClient::s_criticalSectionLocal;
CComAutoCriticalSection CPluginClient::s_criticalSectionPluginId;
CComAutoCriticalSection CPluginClient::s_criticalSectionErrorLog;
#ifdef SUPPORT_FILTER
CComAutoCriticalSection CPluginClient::s_criticalSectionFilter;
#endif

CString CPluginClient::s_pluginId;

std::vector<CPluginError> CPluginClient::s_pluginErrors;

bool CPluginClient::s_isErrorLogging = false;


CPluginClient::CPluginClient()
{
#ifdef SUPPORT_FILTER
    m_filter = std::auto_ptr<CPluginFilter>(new CPluginFilter());
#endif
}

bool CPluginClient::IsValidDomain(const CString& domain)
{
	return domain != L"simple-adblock.com" &&
		domain != L"my.simple-adblock.com" &&
		domain != L"about:blank" &&
		domain != L"about:tabs" &&
		domain.Find(L"javascript:") != 0 &&
		!domain.IsEmpty();
}


CString CPluginClient::ExtractDomain(const CString& url)
{
	int pos = 0;
	CString http = url.Find('/',pos) >= 0 ? url.Tokenize(L"/", pos) : L"";
	CString domain = url.Tokenize(L"/", pos);

	domain.Replace(L"www.", L"");
	domain.Replace(L"www1.", L"");
	domain.Replace(L"www2.", L"");

	domain.MakeLower();

	return domain;
}


CString& CPluginClient::UnescapeUrl(CString& url)
{
    CString unescapedUrl;
    DWORD cb = 2048;

    if (SUCCEEDED(::UrlUnescape(url.GetBuffer(), unescapedUrl.GetBufferSetLength(cb), &cb, 0)))
    {
        unescapedUrl.ReleaseBuffer();
        unescapedUrl.Truncate(cb);

        url.ReleaseBuffer();
        url = unescapedUrl;
    }
    
    return url;
}


void CPluginClient::SetDocumentDomain(const CString& domain)
{
    s_criticalSectionLocal.Lock();
    {
		if (m_documentDomain != domain)
		{
//			::MessageBoxA(::GetDesktopWindow(), domain, "Domain", MB_OK);
		}

		m_documentDomain = domain;
    }
    s_criticalSectionLocal.Unlock();
}

CString CPluginClient::GetDocumentDomain() const
{
    CString domain;

    s_criticalSectionLocal.Lock();
    {
        domain = m_documentDomain;
    }
    s_criticalSectionLocal.Unlock();

    return domain;
}

void CPluginClient::SetDocumentUrl(const CString& url)
{
    s_criticalSectionLocal.Lock();
    {
        m_documentUrl = url;
    }
    s_criticalSectionLocal.Unlock();
}

CString CPluginClient::GetDocumentUrl() const
{
    CString url;

    s_criticalSectionLocal.Lock();
    {
        url = m_documentUrl;
    }
    s_criticalSectionLocal.Unlock();

    return url;
}
    

void CPluginClient::SetLocalization() 
{
    CPluginDictionary* dic = CPluginDictionary::GetInstance();

	CString browserLanguage = GetBrowserLanguage();

    CPluginSettings* settings = CPluginSettings::GetInstance();

    if (settings->IsMainProcess())
    {
	    if (browserLanguage != settings->GetString(SETTING_LANGUAGE)) 
	    {
		    if (dic->IsLanguageSupported(browserLanguage))
		    {
			    settings->SetString(SETTING_LANGUAGE, browserLanguage);
		    }
	    }
    	
	    if (!settings->Has(SETTING_LANGUAGE))
	    {
	        settings->SetString(SETTING_LANGUAGE, "en");
	    }

        settings->Write();
    }

	dic->SetLanguage(browserLanguage);
}


CString CPluginClient::GetBrowserLanguage()
{
    CString browserLanguage;

	LANGID lcid = ::GetUserDefaultLangID();
	TCHAR language[128];
	memset(language, 0, sizeof(language));

	int res = ::GetLocaleInfo(lcid, LOCALE_SISO639LANGNAME, language, 127);
	if (res == 0)
	{
	    DEBUG_ERROR_LOG(::GetLastError(), PLUGIN_ERROR_SYSINFO, PLUGIN_ERROR_SYSINFO_BROWSER_LANGUAGE, "Client::GetBrowserLang - Failed");
	}
	else
	{
	    browserLanguage = language;
	}

	return browserLanguage;
}


CString CPluginClient::GetBrowserVersion()
{
	CString version;

	HKEY hKey;
	DWORD res;

	// Open the handler
	if ((res = ::RegOpenKeyEx(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Internet Explorer", 0, KEY_QUERY_VALUE, &hKey)) == ERROR_SUCCESS)
	{
		TCHAR buf[255];
		DWORD dwBufSize = sizeof(buf);
		DWORD dwType = REG_SZ;

		// Do the processing, find the version
		if ((res = ::RegQueryValueEx(hKey, L"Version", 0, &dwType, (BYTE*)buf, &dwBufSize)) == ERROR_SUCCESS)
		{
			version = buf;
			int pos = 0;
			if ((pos = version.Find('.')) >= 0)
			{
				version = version.Left(pos);
			}
		}
		else
		{
			DEBUG_ERROR_LOG(res, PLUGIN_ERROR_OS_VERSION, PLUGIN_ERROR_OS_VERSION_REG_QUERY_VALUE, L"Client::GetBrowserVer - Failed reg query value");
		}

		// Close the handler
		::RegCloseKey(hKey);
	}
	else
	{
		DEBUG_ERROR_LOG(res, PLUGIN_ERROR_OS_VERSION, PLUGIN_ERROR_OS_VERSION_REG_OPEN_KEY, L"Client::GetBrowserVer - Failed reg open");
	}

	return version;
}


CString CPluginClient::GetUserName()
{
    CString name;
        
    TCHAR userName[UNLEN + 1];
    DWORD length = UNLEN + 1;
    
    int res = ::GetUserName(userName, &length);
    if (res != 0)
    {
        name = userName;
    }
    else
    {
	    DEBUG_ERROR_LOG(::GetLastError(), PLUGIN_ERROR_SYSINFO, PLUGIN_ERROR_SYSINFO_USER_NAME, L"Client::GetUserName - Failed");
    }

    return name;
}


CString CPluginClient::GetComputerName()
{
    CString name;
        
    TCHAR computerName[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD length = MAX_COMPUTERNAME_LENGTH + 1;
    
    if (::GetComputerName(computerName, &length))
    {
        name = computerName;
    }
    else
    {
        DWORD dwError = ::GetLastError();

	    DEBUG_ERROR_LOG(dwError, PLUGIN_ERROR_SYSINFO, PLUGIN_ERROR_SYSINFO_COMPUTER_NAME, L"Client::GetComputerName - Failed");

        name.Format(L"err %u", dwError);
    }
    
    return name;
}


CString CPluginClient::GetPluginId()
{
    CString pluginId;
    
    s_criticalSectionPluginId.Lock();
    {
	    if (s_pluginId.IsEmpty())
	    {
		    s_pluginId = GeneratePluginId();
	    }

        pluginId = s_pluginId;
    }    
    s_criticalSectionPluginId.Unlock();

	return pluginId;
}


bool CPluginClient::SendFtpFile(LPCTSTR server, LPCTSTR inputFile, LPCTSTR outputFile)
{
    HINTERNET hSession = NULL;
    HINTERNET hConnect = NULL;
    BOOL bResult = FALSE;

	// Get Proxy config info.
	CString proxyName;
	CString proxyBypass;
	
	bResult = CPluginHttpRequest::GetProxySettings(proxyName, proxyBypass);	
	if (!bResult)
	{
	    DEBUG_ERROR_LOG(::GetLastError(), PLUGIN_ERROR_FTP_SEND, PLUGIN_ERROR_FTP_SEND_GET_PROXY, L"Client::SendFtpFile - WinHttpGetIEProxyConfigForCurrentUser");
    }

	if (bResult)
	{
	    if (proxyName.IsEmpty())
	    {
		    hSession = ::InternetOpen(BHO_NAME, INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
	    }
        // If there is is proxy setting, use it.
	    else
	    {
		    hSession = ::InternetOpen(BHO_NAME, INTERNET_OPEN_TYPE_PROXY, proxyName, proxyBypass, 0);
	    }

        if (!hSession)
        {
            DEBUG_ERROR_LOG(::GetLastError(), PLUGIN_ERROR_FTP_SEND, PLUGIN_ERROR_FTP_SEND_OPEN, L"Client::SendFtpFile - InternetOpen");
            bResult = FALSE;
        }
    }

    // Connect to the internet
    if (bResult)
    {
        hConnect = ::InternetConnect(hSession, server, 21, L"simpleadblock@ieadblocker.com", L"simple1234", INTERNET_SERVICE_FTP, INTERNET_FLAG_PASSIVE, 0);
        if (!hConnect)
        {
            DEBUG_ERROR_LOG(::GetLastError(), PLUGIN_ERROR_FTP_SEND, PLUGIN_ERROR_FTP_SEND_CONNECT, L"Client::SendFtpFile - InternetConnect");
            bResult = FALSE;
        }
    }
	
    // Send the file over FTP
	if (bResult)
	{
	    bResult = ::FtpPutFile(hConnect, inputFile, outputFile, FTP_TRANSFER_TYPE_ASCII, NULL);
	    if (!bResult)
	    {
            DEBUG_ERROR_LOG(::GetLastError(), PLUGIN_ERROR_FTP_SEND, PLUGIN_ERROR_FTP_SEND_PUT, L"Client::SendFtpFile - FtpPutFile");
	    }
	}    

    // Close connection
    if (hConnect)
    {
        if (!::InternetCloseHandle(hConnect))
        {
            DEBUG_ERROR_LOG(::GetLastError(), PLUGIN_ERROR_FTP_SEND, PLUGIN_ERROR_FTP_SEND_CLOSE, L"Client::SendFtpFile - InternetCloseHandle (connection)");
        }
    }

    // Close session
    if (hSession)
    {
        if (!::InternetCloseHandle(hSession))
        {
            DEBUG_ERROR_LOG(::GetLastError(), PLUGIN_ERROR_FTP_SEND, PLUGIN_ERROR_FTP_SEND_CLOSE, L"Client::SendFtpFile - InternetCloseHandle (session)");
        }
    }

    return bResult? true : false;
}


void CPluginClient::LogPluginError(DWORD errorCode, int errorId, int errorSubid, const CString& description, bool isAsync, DWORD dwProcessId, DWORD dwThreadId)
{
    // Prevent circular references
    if (CPluginSettings::HasInstance() && isAsync)
    {
	    DEBUG_ERROR_CODE_EX(errorCode, description, dwProcessId, dwThreadId);

	    CString pluginError;
	    pluginError.Format(L"%2.2d%2.2d", errorId, errorSubid);

	    CString pluginErrorCode;
	    pluginErrorCode.Format(L"%u", errorCode);

	    CPluginSettings* settings = CPluginSettings::GetInstance();

        settings->AddError(pluginError, pluginErrorCode);
    }

    // Post error to client for later submittal
    if (!isAsync)
    {
        CPluginClient::PostPluginError(errorId, errorSubid, errorCode, description);
    }
}


#define WORKING_BUFFER_SIZE 15000
#define MAX_TRIES 3

#define MALLOC(x) ::HeapAlloc(::GetProcessHeap(), 0, (x))
#define FREE(x) ::HeapFree(::GetProcessHeap(), 0, (x))

CString CPluginClient::GetMacId(bool addSeparator)
{
    CString id;

    DWORD dwSize = 0;
    DWORD dwRetVal = 0;

    unsigned int i = 0;

    // Set the flags to pass to GetAdaptersAddresses
    ULONG flags = GAA_FLAG_INCLUDE_PREFIX;

    // default to unspecified address family (both)
    ULONG family = AF_UNSPEC;

    LPVOID lpMsgBuf = NULL;

    PIP_ADAPTER_ADDRESSES pAddresses = NULL;
    ULONG outBufLen = 0;
    ULONG Iterations = 0;

    PIP_ADAPTER_ADDRESSES pCurrAddresses = NULL;
    PIP_ADAPTER_UNICAST_ADDRESS pUnicast = NULL;
    PIP_ADAPTER_ANYCAST_ADDRESS pAnycast = NULL;
    PIP_ADAPTER_MULTICAST_ADDRESS pMulticast = NULL;
    IP_ADAPTER_DNS_SERVER_ADDRESS *pDnServer = NULL;
    IP_ADAPTER_PREFIX *pPrefix = NULL;

    // Allocate a 15 KB buffer to start with.
    outBufLen = WORKING_BUFFER_SIZE;

    do 
    {
        pAddresses = (IP_ADAPTER_ADDRESSES *) MALLOC(outBufLen);
        if (pAddresses) 
        {
			dwRetVal = ::GetAdaptersAddresses(family, flags, NULL, pAddresses, &outBufLen);
			if (dwRetVal == ERROR_BUFFER_OVERFLOW) 
			{
				FREE(pAddresses);
				pAddresses = NULL;
			} 
			else 
			{
				break;
			}
		}

        Iterations++;

	} while (dwRetVal == ERROR_BUFFER_OVERFLOW && Iterations < MAX_TRIES);

    if (dwRetVal == NO_ERROR) 
    {
        // If successful, output some information from the data we received
        pCurrAddresses = pAddresses;

		int macCount = 0;

        while (pCurrAddresses && macCount < 2) 
        {
            if (pCurrAddresses->PhysicalAddressLength != 0) 
            {
                CString buffer;
                
                bool isValid = false;
                
                for (i = 0; i < (int) pCurrAddresses->PhysicalAddressLength; i++) 
                {
                    if (!buffer.IsEmpty())
                    {
                        buffer += ':';
                    }
                    
                    int value = pCurrAddresses->PhysicalAddress[i];
                    if (value != 0)
                    {
                        isValid = true;
                    }

                    CString part;
                    part.Format(L"%2.2x", value);
                    
                    buffer += part;
                }
                
                if (isValid)
                {

                    if (!id.IsEmpty() && addSeparator)
                    {
		                id += '-';
                    }
                    id += buffer;
                    
                    macCount++;
                }
            }

            pCurrAddresses = pCurrAddresses->Next;
        }
    } 
    else
    {
		DEBUG_ERROR_LOG(dwRetVal, PLUGIN_ERROR_MAC_ID, PLUGIN_ERROR_MAC_ID_RETRIEVAL_EX, L"Client::GetMacId - Failed GetAdaptersAddresses");
    }

    if (pAddresses) 
    {
        FREE(pAddresses);
    }

    return id;
}


CString CPluginClient::GeneratePluginId()
{
	CString id = GetMacId();

    id += GetComputerName();
    
	// Generate SHA1 encryption
	TCHAR szReport[100];

	CStringA idA = id;

	CSHA1 sha1;
	sha1.Update((unsigned char*)idA.GetBuffer(), idA.GetLength());
	sha1.Final();

	sha1.ReportHash(szReport, CSHA1::REPORT_HEX_SHORT);
    
	id = szReport;
	id.MakeLower();
	
    return id;
}


void CPluginClient::PostPluginError(int errorId, int errorSubid, DWORD errorCode, const CString& errorDescription)
{
	s_criticalSectionLocal.Lock();
	{
	    CPluginError pluginError(errorId, errorSubid, errorCode, errorDescription);
	
	    s_pluginErrors.push_back(pluginError);
	}
	s_criticalSectionLocal.Unlock();
}


bool CPluginClient::PopFirstPluginError(CPluginError& pluginError)
{
    bool hasError = false;

	s_criticalSectionLocal.Lock();
	{
	    std::vector<CPluginError>::iterator it = s_pluginErrors.begin();
	    if (it != s_pluginErrors.end())
	    {
	        pluginError = *it;

            hasError = true;
            
            s_pluginErrors.erase(it);
	    }
	}
	s_criticalSectionLocal.Unlock();

    return hasError;
}


void CPluginClient::ClearCache(const CString& domain)
{
    m_criticalSectionCache.Lock();
    {
#ifdef SUPPORT_WHITELIST
        if (domain.IsEmpty() || domain != m_cacheDomain)
        {
            m_cacheWhitelistedUrls.clear();
            m_cacheFrames.clear();
            m_cacheDomain = domain;
        }
#endif

	}
    m_criticalSectionCache.Unlock();

#ifdef SUPPORT_FILE_DOWNLOAD
    s_criticalSectionLocal.Lock();
	{
		m_downloadFiles.clear();
	}
    s_criticalSectionLocal.Unlock();
#endif
}


// ============================================================================
// Filtering
// ============================================================================

#ifdef SUPPORT_FILTER

bool CPluginClient::ShouldBlock(CString src, int contentType, const CString& domain, bool addDebug)
{
    bool isBlocked = false;
    bool isCached = false;

    m_criticalSectionCache.Lock();
    {
        std::map<CString,bool>::iterator it = m_cacheBlockedSources.find(src);

    	isCached = it != m_cacheBlockedSources.end();
        if (isCached)
        {
            isBlocked = it->second;
        }        
    }
    m_criticalSectionCache.Unlock();

    if (!isCached)
    {
        s_criticalSectionFilter.Lock();
        {
	        isBlocked = m_filter->ShouldBlock(src, contentType, domain, addDebug);
        }
        s_criticalSectionFilter.Unlock();

        // Cache result, if content type is defined
        if (contentType != CFilter::contentTypeAny)
        {
            m_criticalSectionCache.Lock();
            {
                m_cacheBlockedSources[src] = isBlocked;
            }
            m_criticalSectionCache.Unlock();
        }
    }
	
	return isBlocked;
}

bool CPluginClient::IsFilterAlive() const
{
    bool isAlive = false;

    s_criticalSectionFilter.Lock();
    {
	    isAlive = m_filter->IsAlive();
    }
    s_criticalSectionFilter.Unlock();
	
	return isAlive;
}

void CPluginClient::RequestFilterDownload(const CString& filter, const CString& filterPath)
{
    DEBUG_GENERAL(L"*** Requesting filter download:" + filter)

    s_criticalSectionFilter.Lock();
    {
	    m_filterDownloads.insert(std::make_pair(filter, filterPath));
    }
    s_criticalSectionFilter.Unlock();
}


bool CPluginClient::DownloadFirstMissingFilter()
{
    bool isDownloaded = false;

    CString filterFilename;
    CString filterDownloadPath;
    
    s_criticalSectionFilter.Lock();
    {
        TFilterFileList::iterator it = m_filterDownloads.begin();
        if (it != m_filterDownloads.end())
        {
            filterFilename = it->first;
            filterDownloadPath = it->second;

            m_filterDownloads.erase(it);
        }
    }
    s_criticalSectionFilter.Unlock();

    if (!filterFilename.IsEmpty() && m_filter->DownloadFilterFile(filterDownloadPath, filterFilename))
    {
        isDownloaded = true;
        
        CPluginSettings* settings = CPluginSettings::GetInstance();

        settings->IncrementTabVersion(SETTING_TAB_FILTER_VERSION);
    }
    
    return isDownloaded;
}


//in this method we read the filter that are in the persistent storage
//then we read them and use these to create a new filterclass

void CPluginClient::ReadFilters()
{
    CPluginSettings* settings = CPluginSettings::GetInstance();
    
    // Check existence of filter file
    if (settings->IsMainProcess())
    {
        CPluginFilter::CreateFilters();
    }

	TFilterFileList filterFileNames;

	TFilterUrlList filters = settings->GetFilterUrlList();

	// Remember first entry in the map, is the filename, second is the version of the filter
	for (TFilterUrlList::iterator it = filters.begin(); it != filters.end(); ++it)
	{
	    DEBUG_FILTER(L"Filter::ReadFilters - adding url:" + it->first)

		CString filename = it->first.Right(it->first.GetLength() - it->first.ReverseFind('/') - 1);  

		filterFileNames.insert(std::make_pair(filename, it->first));  
	}
	
	// Create our filter class which can be used from now on
    std::auto_ptr<CPluginFilter> filter = std::auto_ptr<CPluginFilter>(new CPluginFilter(filterFileNames, CPluginSettings::GetDataPath()));

    s_criticalSectionFilter.Lock();
    {
	    m_filter = filter;
    }
    s_criticalSectionFilter.Unlock();

    ClearCache();
}


bool CPluginClient::IsElementHidden(const CString& tag, IHTMLElement* pEl, const CString& domain, const CString& indent)
{
    bool isHidden;

    s_criticalSectionFilter.Lock();
    {    
        isHidden = m_filter.get() && m_filter->IsElementHidden(tag, pEl, domain, indent);
    }
    s_criticalSectionFilter.Unlock();    

    return isHidden;
}

#endif // SUPPORT_FILTER

// ============================================================================
// Whitelisting
// ============================================================================

#ifdef SUPPORT_WHITELIST

bool CPluginClient::IsUrlWhiteListed(const CString& url)
{
    if (url.IsEmpty())
    {
        return false;
    }

	bool isWhitelisted = false;
	bool isCached = false;

    m_criticalSectionCache.Lock();
    {
        std::map<CString,bool>::iterator it = m_cacheWhitelistedUrls.find(url);

    	isCached = it != m_cacheWhitelistedUrls.end();
        if (isCached)
        {
            isWhitelisted = it->second;
        }
    }
    m_criticalSectionCache.Unlock();

	// TODO is it necessary that this part is locked
	if (!isCached) 
	{
		int pos = 0;
		CString http = url.Find('/',pos) >= 0 ? url.Tokenize(L"/", pos) : L"";
		CString domain = ExtractDomain(url);
		if (http == L"res:" || http == L"file:")
		{
			isWhitelisted = true;
		}
		else
		{
            isWhitelisted = CPluginSettings::GetInstance()->IsWhiteListedDomain(domain);

            #ifdef SUPPORT_FILTER
            {
                if (!isWhitelisted)
                {
                    s_criticalSectionFilter.Lock();
                    {
			            // the url should be whitelisted if the user ask to disable the site
			            // of if the site is whitelisted in one of the filters we receive from plugin
                        isWhitelisted = m_filter.get() && m_filter->ShouldWhiteList(url);  
                    }
                    s_criticalSectionFilter.Unlock();
                }
            }
            #endif // SUPPORT_FILTER
		}
		
        m_criticalSectionCache.Lock();
        {
            m_cacheWhitelistedUrls[url] = isWhitelisted;
        }
        m_criticalSectionCache.Unlock();
	}

	return isWhitelisted;
}


bool CPluginClient::IsDocumentWhiteListed()
{
    CString domain;

    s_criticalSectionLocal.Lock();
    {
        domain = m_documentDomain;
    }
    s_criticalSectionLocal.Unlock();

    return IsUrlWhiteListed(domain);
}

bool CPluginClient::IsFrame(const CString& url)
{
    bool isFrame;
    
    m_criticalSectionCache.Lock();
    {
        isFrame = m_cacheFrames.find(url) != m_cacheFrames.end();
    }
    m_criticalSectionCache.Unlock();
    
    return isFrame;
}

void CPluginClient::AddCacheFrame(const CString& url)
{
    m_criticalSectionCache.Lock();
    {
        m_cacheFrames.insert(url);
    }
    m_criticalSectionCache.Unlock();
}

#endif // SUPPORT_WHITELIST

// ============================================================================
// File download
// ============================================================================

#ifdef SUPPORT_FILE_DOWNLOAD

void CPluginClient::AddDownloadFile(const CString& url, int fileSize, const SDownloadFileProperties& properties)
{
    s_criticalSectionLocal.Lock();
    {
        bool isFound = false;

        TDownloadFiles::const_iterator it = m_downloadFiles.begin();
        while (it != m_downloadFiles.end() && !isFound)
        {
            if (it->second.downloadUrl == url || url.Find(it->second.downloadUrl) >= 0)
            {
                isFound = true;
            }
            
            ++it;
        }

        if (!isFound)
        {
			SDownloadFile downloadFile;

			CString count;
			count.Format(L"%d", m_downloadFiles.size() + 1);

			CString domain = m_documentDomain;

			int iPos = domain.ReverseFind('.');
			while (iPos >= 0 && domain.GetLength() - iPos <= 4)
			{
				domain = domain.Left(iPos);

				iPos = domain.ReverseFind('.');
			}

			CString filename = domain + count + "." + properties.extension;

			downloadFile.downloadUrl  = url;
            downloadFile.downloadFile = filename;
            downloadFile.fileType = 0;
            downloadFile.properties = properties;
			downloadFile.fileSize = fileSize;

            m_downloadFiles[filename] = downloadFile;
        }        
    }
    s_criticalSectionLocal.Unlock();
}

TDownloadFiles CPluginClient::GetDownloadFiles() const
{
    TDownloadFiles downloadFiles;

    s_criticalSectionLocal.Lock();
    {
        downloadFiles = m_downloadFiles;
    }
    s_criticalSectionLocal.Unlock();

    return downloadFiles;
}

bool CPluginClient::HasDownloadFiles() const
{
    bool hasFiles = false;

    s_criticalSectionLocal.Lock();
    {
        hasFiles = !m_downloadFiles.empty();
    }
    s_criticalSectionLocal.Unlock();

    return hasFiles;    
}

#endif // SUPPORT_FILE_DOWNLOAD

