#include "PluginStdAfx.h"

#include <Wbemidl.h>
#include <time.h>
#include "PluginIniFileW.h"
#include "PluginIniFile.h"
#include "PluginSettings.h"
#include "PluginDictionary.h"
#include "PluginClient.h"
#include "PluginChecksum.h"
#include "PluginSystem.h"
#ifdef SUPPORT_FILTER
#include "PluginFilter.h"
#endif
#include "PluginMutex.h"
#include "PluginHttpRequest.h"
#include <memory>


// IE functions
#pragma comment(lib, "iepmapi.lib")

#include <knownfolders.h>

class TSettings
{
  DWORD processorId;

  char sPluginId[44];
};

static void SubsCallback(std::vector<AdblockPlus::SubscriptionPtr>& subscriptions);


class CPluginSettingsLock : public CPluginMutex
{
public:
  CPluginSettingsLock() : CPluginMutex("SettingsFile", PLUGIN_ERROR_MUTEX_SETTINGS_FILE) {}
  ~CPluginSettingsLock() {}

};


class CPluginSettingsTabLock : public CPluginMutex
{
public:
  CPluginSettingsTabLock() : CPluginMutex("SettingsFileTab", PLUGIN_ERROR_MUTEX_SETTINGS_FILE_TAB) {}
  ~CPluginSettingsTabLock() {}
};

#ifdef SUPPORT_WHITELIST

class CPluginSettingsWhitelistLock : public CPluginMutex
{
public:
  CPluginSettingsWhitelistLock() : CPluginMutex("SettingsFileWhitelist", PLUGIN_ERROR_MUTEX_SETTINGS_FILE_WHITELIST) {}
  ~CPluginSettingsWhitelistLock() {}
};

#endif

WCHAR* CPluginSettings::s_dataPath;
WCHAR* CPluginSettings::s_dataPathParent;

CPluginSettings* CPluginSettings::s_instance = NULL;
bool CPluginSettings::s_isLightOnly = false;

CComAutoCriticalSection CPluginSettings::s_criticalSectionLocal;
#ifdef SUPPORT_WHITELIST
CComAutoCriticalSection CPluginSettings::s_criticalSectionDomainHistory;
#endif


CPluginSettings::CPluginSettings() : 
  m_settingsVersion("1"), m_isDirty(false), m_isFirstRun(false), m_isFirstRunUpdate(false), m_dwMainProcessId(0), m_dwMainThreadId(0), m_dwWorkingThreadId(0), 
  m_isDirtyTab(false), m_isPluginEnabledTab(true), m_tabNumber("1")
{

  CPluginSettings *lightInstance = s_instance;
  s_instance = NULL;

  m_settingsFile = std::auto_ptr<CPluginIniFileW>(new CPluginIniFileW(GetDataPath(SETTINGS_INI_FILE), false));
  m_settingsFileTab = std::auto_ptr<CPluginIniFileW>(new CPluginIniFileW(GetDataPath(SETTINGS_INI_FILE_TAB), true));

  m_WindowsBuildNumber = 0;

  Clear();
  ClearTab();
#ifdef SUPPORT_WHITELIST
  ClearWhitelist();
#endif

  // Check existence of settings file
  bool isFileExisting = false;
  {
    CPluginSettingsLock lock;
    if (lock.IsLocked())
    {
      std::ifstream is;
      is.open(GetDataPath(SETTINGS_INI_FILE), std::ios_base::in);
      if (!is.is_open())
      {
        TCHAR pf[MAX_PATH];
        SHGetSpecialFolderPath(
          0,
          pf, 
          CSIDL_PROGRAM_FILESX86, 
          FALSE ); 
        //No files found, copy from the dll location
        CString pathToDll;
        DWORD pathResult = GetModuleFileNameW((HINSTANCE)&__ImageBase, pathToDll.GetBufferSetLength(MAX_PATH), MAX_PATH);
        if (pathResult > 0)
        {
          CString cpyPath;
          cpyPath = pathToDll.Left(pathToDll.ReverseFind('\\') + 1);

          BOOL res = CopyFile(cpyPath + SETTINGS_INI_FILE, GetDataPath(SETTINGS_INI_FILE), TRUE);
          res = CopyFile(cpyPath + DICTIONARY_INI_FILE, GetDataPath(DICTIONARY_INI_FILE), TRUE);
          res = CopyFile(cpyPath + SETTING_PAGE_INI_FILE, GetDataPath(SETTING_PAGE_INI_FILE), TRUE);

          SHFILEOPSTRUCT pFileStruct;
          ZeroMemory(&pFileStruct, sizeof(SHFILEOPSTRUCT)); 
          pFileStruct.hwnd  = NULL;
          pFileStruct.wFunc = FO_COPY;
          WCHAR fromPath[MAX_PATH + 2]; 
          WCHAR toPath[MAX_PATH + 2];

          CString source = cpyPath + "html\\*";
          wcscpy(fromPath, source);
          fromPath[source.GetLength()] = '\0';
          fromPath[source.GetLength() + 1] = '\0';

          wcscpy(toPath, GetDataPath(L"html"));
          toPath[GetDataPath(L"html").GetLength()] = '\0';
          toPath[GetDataPath(L"html").GetLength() + 1] = '\0';

          pFileStruct.pFrom = fromPath;
          pFileStruct.pTo =  toPath; 
          pFileStruct.fFlags = FOF_SILENT  | FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR | FOF_NO_UI | FOF_RENAMEONCOLLISION; 
          bool i = pFileStruct.fAnyOperationsAborted ;
          SHFileOperation(&pFileStruct);
        }
        is.open(GetDataPath(SETTINGS_INI_FILE), std::ios_base::in);
        if (!is.is_open())
        {
          m_isDirty = true;
        }
        else
        {
          is.close();
          isFileExisting = true;

        }

      }
      else
      {
        is.close();

        isFileExisting = true;
      }
    }
  }

  // Read or convert file
  if (isFileExisting)
  {
    Read(false);
  }
  else
  {
    m_isDirty = true;
  }

  if (s_isLightOnly)
  {
    this->SetMainProcessId(lightInstance->m_dwMainProcessId);
    this->SetMainThreadId(lightInstance->m_dwMainThreadId);
    this->SetMainUiThreadId(lightInstance->m_dwMainUiThreadId);
    this->SetWorkingThreadId(lightInstance->m_dwWorkingThreadId);
  }
  Write();
}


CPluginSettings::~CPluginSettings()
{

  if (s_dataPathParent != NULL)
  {
    delete s_dataPathParent;
  }
  s_instance = NULL;
}


CPluginSettings* CPluginSettings::GetInstance() 
{
  CPluginSettings* instance = NULL;

  s_criticalSectionLocal.Lock();
  {
    if ((!s_instance) || (s_isLightOnly))
    {
      s_instance = new CPluginSettings();
#ifdef USE_CONSOLE
      CONSOLE("Fetching Available Subscription\n");
#endif
      try
      {
        CPluginSettings::GetInstance()->m_subscriptions = CPluginClient::GetInstance()->GetFilterEngine()->FetchAvailableSubscriptions();
      }
      catch(std::exception ex)
      {
        DEBUG_GENERAL(ex.what());
        throw ex;
      }
      s_isLightOnly = false;
    }

    instance = s_instance;
  }
  s_criticalSectionLocal.Unlock();

  return instance;
}


bool CPluginSettings::HasInstance() 
{
  bool hasInstance = true;

  s_criticalSectionLocal.Lock();
  {
    hasInstance = s_instance != NULL;
  }
  s_criticalSectionLocal.Unlock();

  return hasInstance;
}


bool CPluginSettings::Read(bool bDebug)
{
  bool isRead = true;

  DEBUG_SETTINGS(L"Settings::Read")
  {
    if (bDebug)
    {
      DEBUG_GENERAL(L"*** Loading settings:" + m_settingsFile->GetFilePath());
    }

    CPluginSettingsLock lock;
    if (lock.IsLocked())
    {
      isRead = m_settingsFile->Read();        
      if (isRead)
      {
        if (m_settingsFile->IsValidChecksum())
        {
          m_properties = m_settingsFile->GetSectionData("Settings");
        }
        else
        {
          DEBUG_SETTINGS("Settings:Invalid checksum - Deleting file")

          Clear();

          DEBUG_ERROR_LOG(m_settingsFile->GetLastError(), PLUGIN_ERROR_SETTINGS, PLUGIN_ERROR_SETTINGS_FILE_READ_CHECKSUM, "Settings::Read - Checksum")
          isRead = false;
          m_isDirty = true;
        }
      }
      else if (m_settingsFile->GetLastError() == ERROR_FILE_NOT_FOUND)
      {
        DEBUG_ERROR_LOG(m_settingsFile->GetLastError(), PLUGIN_ERROR_SETTINGS, PLUGIN_ERROR_SETTINGS_FILE_READ, "Settings::Read")
          m_isDirty = true;
      }
      else
      {
        DEBUG_ERROR_LOG(m_settingsFile->GetLastError(), PLUGIN_ERROR_SETTINGS, PLUGIN_ERROR_SETTINGS_FILE_READ, "Settings::Read")
      }
    }
    else
    {
      isRead = false;
    }
  }

  // Write file in case it is dirty
  if (isRead)
  {
    isRead = Write();
  }

  return isRead;
}


void CPluginSettings::Clear()
{
  // Default settings
  s_criticalSectionLocal.Lock();
  {
    m_properties.clear();

    m_properties[SETTING_PLUGIN_VERSION] = IEPLUGIN_VERSION;
    m_properties[SETTING_LANGUAGE] = "en";
  }
  s_criticalSectionLocal.Unlock();
}

bool CPluginSettings::MakeRequestForUpdate()
{
  time_t updateTime = this->GetValue(SETTING_LAST_UPDATE_TIME);

  if (time(NULL) <= updateTime)
    return false;

  CPluginHttpRequest httpRequest(PLUGIN_UPDATE_URL);

  CPluginSystem* system = CPluginSystem::GetInstance();

  httpRequest.Add("lang", this->GetString(SETTING_LANGUAGE, "err"));
  httpRequest.Add("ie", system->GetBrowserVersion());
  httpRequest.Add("ielang", system->GetBrowserLanguage());

  httpRequest.AddOsInfo();

  httpRequest.Send();

  this->SetValue(SETTING_LAST_UPDATE_TIME, time(NULL) + (5 * 24 * 60 * 60) * ((rand() % 100) / 100 * 0.4 + 0.8));
  if (httpRequest.IsValidResponse())
  {
    const std::auto_ptr<CPluginIniFile>& iniFile = httpRequest.GetResponseFile();

    CPluginIniFile::TSectionData settingsData = iniFile->GetSectionData("Settings");
    CPluginIniFile::TSectionData::iterator it;

    it = settingsData.find("pluginupdate");
    if (it != settingsData.end())
    {
      CString url(it->second);
      SetString(SETTING_PLUGIN_UPDATE_URL, url);
      m_isDirty = true;
      DEBUG_SETTINGS("Settings::Configuration plugin update url:" + it->second);
    }

    it = settingsData.find("pluginupdatev");
    if (it != settingsData.end())
    {
      CString ver(it->second);
      SetString(SETTING_PLUGIN_UPDATE_VERSION, ver);
      m_isDirty = true;
      DEBUG_SETTINGS("Settings::Configuration plugin update version:" + it->second);
    }
  }

  return true;
}

CString CPluginSettings::GetDataPathParent()
{
  if (s_dataPathParent == NULL) 
  {
    WCHAR* lpData = new WCHAR[MAX_PATH];

    OSVERSIONINFO osVersionInfo;
    ::ZeroMemory(&osVersionInfo, sizeof(OSVERSIONINFO));

    osVersionInfo.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);

    ::GetVersionEx(&osVersionInfo);

    //Windows Vista				- 6.0 
    //Windows Server 2003 R2	- 5.2 
    //Windows Server 2003		- 5.2 
    //Windows XP				- 5.1 
    if (osVersionInfo.dwMajorVersion >= 6)
    {
      if (::SHGetSpecialFolderPath(NULL, lpData, CSIDL_LOCAL_APPDATA, TRUE))
      {
        wcscat(lpData, L"Low");
      }
      else
      {
        DEBUG_ERROR_LOG(::GetLastError(), PLUGIN_ERROR_SYSINFO, PLUGIN_ERROR_SYSINFO_GET_SPECIAL_FOLDER_LOCAL, "Settings::GetDataPath failed");
      }
    }
    else
    {
      if (!SHGetSpecialFolderPath(NULL, lpData, CSIDL_APPDATA, TRUE))
      {
        DEBUG_ERROR_LOG(::GetLastError(), PLUGIN_ERROR_SYSINFO, PLUGIN_ERROR_SYSINFO_GET_SPECIAL_FOLDER, "Settings::GetDataPath failed");
      }
    }

    ::PathAddBackslash(lpData);

    s_dataPathParent = lpData;

    if (!::CreateDirectory(s_dataPathParent, NULL))
    {
      DWORD errorCode = ::GetLastError();
      if (errorCode != ERROR_ALREADY_EXISTS)
      {
        DEBUG_ERROR_LOG(errorCode, PLUGIN_ERROR_SETTINGS, PLUGIN_ERROR_SETTINGS_CREATE_FOLDER, "Settings::CreateDirectory failed");
      }
    }
  }

  return s_dataPathParent;
}

CString CPluginSettings::GetDataPath(const CString& filename)
{
  if (s_dataPath == NULL) 
  {
    WCHAR* lpData = new WCHAR[MAX_PATH];

    OSVERSIONINFO osVersionInfo;
    ::ZeroMemory(&osVersionInfo, sizeof(OSVERSIONINFO));

    osVersionInfo.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);

    ::GetVersionEx(&osVersionInfo);

    //Windows Vista				- 6.0 
    //Windows Server 2003 R2	- 5.2 
    //Windows Server 2003		- 5.2 
    //Windows XP				- 5.1 
    if (osVersionInfo.dwMajorVersion >= 6)
    {
      if (::SHGetSpecialFolderPath(NULL, lpData, CSIDL_LOCAL_APPDATA, TRUE))
      {
        wcscat(lpData, L"Low");
      }
      else
      {
        DEBUG_ERROR_LOG(::GetLastError(), PLUGIN_ERROR_SYSINFO, PLUGIN_ERROR_SYSINFO_GET_SPECIAL_FOLDER_LOCAL, "Settings::GetDataPath failed");
      }
    }
    else
    {
      if (!SHGetSpecialFolderPath(NULL, lpData, CSIDL_APPDATA, TRUE))
      {
        DEBUG_ERROR_LOG(::GetLastError(), PLUGIN_ERROR_SYSINFO, PLUGIN_ERROR_SYSINFO_GET_SPECIAL_FOLDER, "Settings::GetDataPath failed");
      }
    }

    ::PathAddBackslash(lpData);

    s_dataPath = lpData;

    if (!::CreateDirectory(s_dataPath + CString(USER_DIR), NULL))
    {
      DWORD errorCode = ::GetLastError();
      if (errorCode != ERROR_ALREADY_EXISTS)
      {
        DEBUG_ERROR_LOG(errorCode, PLUGIN_ERROR_SETTINGS, PLUGIN_ERROR_SETTINGS_CREATE_FOLDER, "Settings::CreateDirectory failed");
      }
    }
  }

  return s_dataPath + CString(USER_DIR) + filename;
}

CString CPluginSettings::GetSystemLanguage()
{
  CString language;
  CString country;

  DWORD bufSize = 256;
  int ccBuf = GetLocaleInfo(LOCALE_SYSTEM_DEFAULT, LOCALE_SISO639LANGNAME, language.GetBufferSetLength(bufSize), bufSize);
  ccBuf = GetLocaleInfo(LOCALE_SYSTEM_DEFAULT, LOCALE_SISO3166CTRYNAME, country.GetBufferSetLength(bufSize), bufSize);

  if ((country.IsEmpty()) || (language.IsEmpty()))
  {
    return CString();
  }
  CString lang;
  lang.Append(language);
  lang.Append(L"-");
  lang.Append(country);

  return lang;

}

CString CPluginSettings::GetTempPath(const CString& filename)
{
  CString tempPath;

  LPWSTR pwszCacheDir = NULL;

  HRESULT hr = ::IEGetWriteableFolderPath(FOLDERID_InternetCache, &pwszCacheDir); 
  if (SUCCEEDED(hr))
  {
    tempPath = pwszCacheDir;
  }
  // Not implemented in IE6
  else if (hr == E_NOTIMPL)
  {
    TCHAR path[MAX_PATH] = _T("");

    if (::SHGetSpecialFolderPath(NULL, path, CSIDL_INTERNET_CACHE, TRUE))
    {
      tempPath = path;
    }
    else
    {
      DEBUG_ERROR_LOG(::GetLastError(), PLUGIN_ERROR_SYSINFO, PLUGIN_ERROR_SYSINFO_GET_SPECIAL_FOLDER_TEMP, "Settings::GetTempPath failed");
    }
  }
  // Other error
  else
  {
    DEBUG_ERROR_LOG(hr, PLUGIN_ERROR_SYSINFO, PLUGIN_ERROR_SYSINFO_TEMP_PATH, "Settings::GetTempPath failed");
  }

  ::CoTaskMemFree(pwszCacheDir);

  return tempPath + "\\" + filename;
}

CString CPluginSettings::GetTempFile(const CString& prefix, const CString& extension)
{
  TCHAR nameBuffer[MAX_PATH] = _T("");

  CString tempPath;

  DWORD dwRetVal = ::GetTempFileName(GetTempPath(), prefix, 0, nameBuffer);
  if (dwRetVal == 0)
  {
    DEBUG_ERROR_LOG(::GetLastError(), PLUGIN_ERROR_SYSINFO, PLUGIN_ERROR_SYSINFO_TEMP_FILE, "Settings::GetTempFileName failed");

    tempPath = GetDataPath();
  }
  else
  {
    tempPath = nameBuffer;
    if (!extension.IsEmpty())
    {
      int pos = tempPath.ReverseFind(_T('.'));
      if (pos >= 0)
      {
        tempPath = tempPath.Left(pos+1) + extension;
      }
    }
  }

  return tempPath;
}


bool CPluginSettings::Has(const CString& key) const
{
  bool hasKey;

  s_criticalSectionLocal.Lock();
  {
    hasKey = m_properties.find(key) != m_properties.end();
  }
  s_criticalSectionLocal.Unlock();

  return hasKey;
}


void CPluginSettings::Remove(const CString& key)
{
  s_criticalSectionLocal.Lock();
  {    
    TProperties::iterator it = m_properties.find(key);
    if (it != m_properties.end())
    {
      m_properties.erase(it);
      m_isDirty = true;
    }
  }
  s_criticalSectionLocal.Unlock();
}


CString CPluginSettings::GetString(const CString& key, const CString& defaultValue) const
{
  CString val = defaultValue;

  s_criticalSectionLocal.Lock();
  {
    TProperties::const_iterator it = m_properties.find(key);
    if (it != m_properties.end())
    {
      val = it->second;
    }
  }
  s_criticalSectionLocal.Unlock();

  DEBUG_SETTINGS("Settings::GetString key:" + key + " value:" + val)

    return val;
}


void CPluginSettings::SetString(const CString& key, const CString& value)
{
  if (value.IsEmpty()) return;

  DEBUG_SETTINGS("Settings::SetString key:" + key + " value:" + value)

    s_criticalSectionLocal.Lock();
  {
    TProperties::iterator it = m_properties.find(key);
    if (it != m_properties.end() && it->second != value)
    {
      it->second = value;
      m_isDirty = true;
    }
    else if (it == m_properties.end())
    {
      m_properties[key] = value; 
      m_isDirty = true;
    }
  }
  s_criticalSectionLocal.Unlock();
}


int CPluginSettings::GetValue(const CString& key, int defaultValue) const
{
  int val = defaultValue;

  CString sValue;
  sValue.Format(L"%d", defaultValue);

  s_criticalSectionLocal.Lock();
  {
    TProperties::const_iterator it = m_properties.find(key);
    if (it != m_properties.end())
    {
      sValue = it->second;
      val = _wtoi(it->second);
    }
  }
  s_criticalSectionLocal.Unlock();

  DEBUG_SETTINGS("Settings::GetValue key:" + key + " value:" + sValue)

    return val;
}


void CPluginSettings::SetValue(const CString& key, int value)
{
  CString sValue;
  sValue.Format(L"%d", value);

  DEBUG_SETTINGS("Settings::SetValue key:" + key + " value:" + sValue)

    s_criticalSectionLocal.Lock();
  {
    TProperties::iterator it = m_properties.find(key);
    if (it != m_properties.end() && it->second != sValue)
    {
      it->second = sValue;
      m_isDirty = true;
    }
    else if (it == m_properties.end())
    {
      m_properties[key] = sValue; 
      m_isDirty = true;
    }
  }
  s_criticalSectionLocal.Unlock();
}


bool CPluginSettings::GetBool(const CString& key, bool defaultValue) const
{
  bool value = defaultValue;

  s_criticalSectionLocal.Lock();
  {
    TProperties::const_iterator it = m_properties.find(key);
    if (it != m_properties.end())
    {
      if (it->second == "true") value = true;
      if (it->second == "false") value = false;
    }
  }
  s_criticalSectionLocal.Unlock();

  DEBUG_SETTINGS("Settings::GetBool key:" + key + " value:" + (value ? "true":"false"))

    return value;
}


void CPluginSettings::SetBool(const CString& key, bool value)
{
  SetString(key, value ? "true":"false");
}


bool CPluginSettings::IsPluginEnabled() const
{
  return m_isPluginEnabledTab;
}

static void SubsCallback(std::vector<AdblockPlus::SubscriptionPtr>& subscriptions)
{
  CPluginSettings::GetInstance()->m_subscriptions = subscriptions;
  return;
}


std::map<CString, CString> CPluginSettings::GetFilterLanguageTitleList() const
{
  std::map<CString, CString> filterList;
  for (int i = 0; i < m_subscriptions.size(); i ++)
  {  
    AdblockPlus::SubscriptionPtr it = m_subscriptions[i];
    std::string title = "";
    std::string url = "";

    title = it.get()->GetProperty("specialization", title);
    url = it.get()->GetProperty("url", url);

    filterList.insert(std::make_pair(CString(CA2T(url.c_str(), CP_UTF8)), CString(CA2T(title.c_str(), CP_UTF8))));
  }
  return filterList;
}


bool CPluginSettings::Write(bool isDebug)
{
  bool isWritten = true;

  if (!m_isDirty)
  {
    return isWritten;
  }

  if (isDebug)
  {
    DEBUG_GENERAL(L"*** Writing changed settings")
  }

  CPluginSettingsLock lock;
  if (lock.IsLocked())
  {
    m_settingsFile->Clear();

    // Properties
    CPluginIniFileW::TSectionData settings;        

    s_criticalSectionLocal.Lock();
    {
      for (TProperties::iterator it = m_properties.begin(); it != m_properties.end(); ++it)
      {
        settings[it->first] = it->second;
      }
    }
    s_criticalSectionLocal.Unlock();

    m_settingsFile->UpdateSection("Settings", settings);

    // Write file
    isWritten = m_settingsFile->Write();
    if (!isWritten)
    {
      DEBUG_ERROR_LOG(m_settingsFile->GetLastError(), PLUGIN_ERROR_SETTINGS, PLUGIN_ERROR_SETTINGS_FILE_WRITE, "Settings::Write")
    }

    m_isDirty = false;

    IncrementTabVersion(SETTING_TAB_SETTINGS_VERSION);
  }
  else
  {
    isWritten = false;
  }

  return isWritten;
}


bool CPluginSettings::IsPluginUpdateAvailable() const
{
  bool isAvailable = Has(SETTING_PLUGIN_UPDATE_VERSION);
  if (isAvailable)
  {
    CString newVersion = GetString(SETTING_PLUGIN_UPDATE_VERSION);
    CString curVersion = IEPLUGIN_VERSION;

    isAvailable = newVersion != curVersion;
    if (isAvailable)
    {
      int curPos = 0;
      int curMajor = _wtoi(curVersion.Tokenize(L".", curPos));
      int curMinor = _wtoi(curVersion.Tokenize(L".", curPos));
      int curDev   = _wtoi(curVersion.Tokenize(L".", curPos));

      int newPos = 0;
      int newMajor = _wtoi(newVersion.Tokenize(L".", newPos));
      int newMinor = newPos > 0 ? _wtoi(newVersion.Tokenize(L".", newPos)) : 0;
      int newDev   = newPos > 0 ? _wtoi(newVersion.Tokenize(L".", newPos)) : 0;

      isAvailable = newMajor > curMajor || newMajor == curMajor && newMinor > curMinor || newMajor == curMajor && newMinor == curMinor && newDev > curDev;
    }
  }

  return isAvailable;
}

bool CPluginSettings::IsMainProcess(DWORD dwProcessId) const
{
  if (dwProcessId == 0)
  {
    dwProcessId = ::GetCurrentProcessId();
  }
  return m_dwMainProcessId == dwProcessId;
}

void CPluginSettings::SetMainProcessId()
{
  m_dwMainProcessId = ::GetCurrentProcessId();
}

void CPluginSettings::SetMainProcessId(DWORD id)
{
  m_dwMainProcessId = id;
}


bool CPluginSettings::IsMainUiThread(DWORD dwThreadId) const
{
  if (dwThreadId == 0)
  {
    dwThreadId = ::GetCurrentThreadId();
  }
  return m_dwMainUiThreadId == dwThreadId;
}

void CPluginSettings::SetMainUiThreadId()
{
  m_dwMainUiThreadId = ::GetCurrentThreadId();
}

void CPluginSettings::SetMainUiThreadId(DWORD id)
{
  m_dwMainUiThreadId = id;
}
bool CPluginSettings::IsMainThread(DWORD dwThreadId) const
{
  if (dwThreadId == 0)
  {
    dwThreadId = ::GetCurrentThreadId();
  }
  return m_dwMainThreadId == dwThreadId;
}

void CPluginSettings::SetMainThreadId()
{
  m_dwMainThreadId = ::GetCurrentThreadId();
}

void CPluginSettings::SetMainThreadId(DWORD id)
{
  m_dwMainThreadId = id;
}

bool CPluginSettings::IsWorkingThread(DWORD dwThreadId) const
{
  if (dwThreadId == 0)
  {
    dwThreadId = ::GetCurrentThreadId();
  }
  return m_dwWorkingThreadId == dwThreadId;
}

void CPluginSettings::SetWorkingThreadId()
{
  m_dwWorkingThreadId = ::GetCurrentThreadId();
}

void CPluginSettings::SetWorkingThreadId(DWORD id)
{
  m_dwWorkingThreadId = id;
}

void CPluginSettings::SetFirstRun()
{
  m_isFirstRun = true;
}

bool CPluginSettings::IsFirstRun() const
{
  return m_isFirstRun;
}

void CPluginSettings::SetFirstRunUpdate()
{
  m_isFirstRunUpdate = true;
}

bool CPluginSettings::IsFirstRunUpdate() const
{
  return m_isFirstRunUpdate;
}

bool CPluginSettings::IsFirstRunAny() const
{
  return m_isFirstRun || m_isFirstRunUpdate;
}

// ============================================================================
// Tab settings
// ============================================================================

void CPluginSettings::ClearTab()
{
  s_criticalSectionLocal.Lock();
  {
    m_isPluginEnabledTab = true;

    m_errorsTab.clear();

    m_propertiesTab.clear();

    m_propertiesTab[SETTING_TAB_PLUGIN_ENABLED] = "true";
  }
  s_criticalSectionLocal.Unlock();
}


bool CPluginSettings::ReadTab(bool bDebug)
{
  bool isRead = true;

  DEBUG_SETTINGS(L"SettingsTab::Read tab")

    if (bDebug)
    {
      DEBUG_GENERAL(L"*** Loading tab settings:" + m_settingsFileTab->GetFilePath());
    }

    isRead = m_settingsFileTab->Read();        
    if (isRead)
    {
      ClearTab();

      if (m_settingsFileTab->IsValidChecksum())
      {
        s_criticalSectionLocal.Lock();
        {
          m_propertiesTab = m_settingsFileTab->GetSectionData("Settings");

          m_errorsTab = m_settingsFileTab->GetSectionData("Errors");

          TProperties::iterator it = m_propertiesTab.find(SETTING_TAB_PLUGIN_ENABLED);
          if (it != m_propertiesTab.end())
          {
            m_isPluginEnabledTab = it->second != "false";
          }
        }
        s_criticalSectionLocal.Unlock();
      }
      else
      {
        DEBUG_SETTINGS("SettingsTab:Invalid checksum - Deleting file")

          DEBUG_ERROR_LOG(m_settingsFileTab->GetLastError(), PLUGIN_ERROR_SETTINGS_TAB, PLUGIN_ERROR_SETTINGS_FILE_READ_CHECKSUM, "SettingsTab::Read - Checksum")
          isRead = false;
        m_isDirtyTab = true;
      }
    }
    else if (m_settingsFileTab->GetLastError() == ERROR_FILE_NOT_FOUND)
    {
      m_isDirtyTab = true;
    }
    else
    {
      DEBUG_ERROR_LOG(m_settingsFileTab->GetLastError(), PLUGIN_ERROR_SETTINGS_TAB, PLUGIN_ERROR_SETTINGS_FILE_READ, "SettingsTab::Read")
    }


    // Write file in case it is dirty or does not exist
    WriteTab();

    return isRead;
}

bool CPluginSettings::WriteTab(bool isDebug)
{
  bool isWritten = true;

  if (!m_isDirtyTab)
  {
    return isWritten;
  }

  if (isDebug)
  {
    DEBUG_GENERAL(L"*** Writing changed tab settings")
  }

  m_settingsFileTab->Clear();

  // Properties & errors
  CPluginIniFileW::TSectionData settings;        
  CPluginIniFileW::TSectionData errors;        

  s_criticalSectionLocal.Lock();
  {
    for (TProperties::iterator it = m_propertiesTab.begin(); it != m_propertiesTab.end(); ++it)
    {
      settings[it->first] = it->second;
    }

    for (TProperties::iterator it = m_errorsTab.begin(); it != m_errorsTab.end(); ++it)
    {
      errors[it->first] = it->second;
    }
  }
  s_criticalSectionLocal.Unlock();

  m_settingsFileTab->UpdateSection("Settings", settings);
  m_settingsFileTab->UpdateSection("Errors", errors);

  // Write file
  isWritten = m_settingsFileTab->Write();
  if (!isWritten)
  {
    DEBUG_ERROR_LOG(m_settingsFileTab->GetLastError(), PLUGIN_ERROR_SETTINGS_TAB, PLUGIN_ERROR_SETTINGS_FILE_WRITE, "SettingsTab::Write")
  }

  m_isDirtyTab = !isWritten;

  return isWritten;
}


void CPluginSettings::EraseTab()
{
  ClearTab();

  m_isDirtyTab = true;

  WriteTab();
}


bool CPluginSettings::IncrementTabCount()
{
  int tabCount = 1;


  if (s_isLightOnly)
  {
    return false;
  }

  CPluginSettingsTabLock lock;
  if (lock.IsLocked())
  {
    SYSTEMTIME systemTime;
    ::GetSystemTime(&systemTime);

    CString today;
    today.Format(L"%d-%d-%d", systemTime.wYear, systemTime.wMonth, systemTime.wDay);

    ReadTab(false);

    s_criticalSectionLocal.Lock();
    {
      TProperties::iterator it = m_propertiesTab.find(SETTING_TAB_COUNT);
      if (it != m_propertiesTab.end())
      {        
        tabCount = _wtoi(it->second) + 1;
      }

      it = m_propertiesTab.find(SETTING_TAB_START_TIME);

      //Is this a first IE instance?
      HWND ieWnd = FindWindow(L"IEFrame", NULL);
      if (ieWnd != NULL)
      {
        ieWnd = FindWindowEx(NULL, ieWnd, L"IEFrame", NULL);

      }
      if ((it != m_propertiesTab.end() && it->second != today))
      {
        tabCount = 1;        
      }
      m_tabNumber.Format(L"%d", tabCount);

      m_propertiesTab[SETTING_TAB_COUNT] = m_tabNumber;
      m_propertiesTab[SETTING_TAB_START_TIME] = today;

      // Main tab?
      if (tabCount == 1)
      {
        m_propertiesTab[SETTING_TAB_DICTIONARY_VERSION] = "1";
        m_propertiesTab[SETTING_TAB_SETTINGS_VERSION] = "1";
#ifdef SUPPORT_WHITELIST
        m_propertiesTab[SETTING_TAB_WHITELIST_VERSION] = "1";
#endif
#ifdef SUPPORT_FILTER
        m_propertiesTab[SETTING_TAB_FILTER_VERSION] = "1";
#endif
#ifdef SUPPORT_CONFIG
        m_propertiesTab[SETTING_TAB_CONFIG_VERSION] = "1";
#endif
      }
    }
    s_criticalSectionLocal.Unlock();

    m_isDirtyTab = true;

    WriteTab(false);        
  }

  return tabCount == 1;
}


CString CPluginSettings::GetTabNumber() const
{
  CString tabNumber;

  s_criticalSectionLocal.Lock();
  {
    tabNumber = m_tabNumber;
  }
  s_criticalSectionLocal.Unlock();

  return tabNumber;
}


bool CPluginSettings::DecrementTabCount()
{
  int tabCount = 0;

  CPluginSettingsTabLock lock;
  if (lock.IsLocked())
  {
    ReadTab(false);

    s_criticalSectionLocal.Lock();
    {
      TProperties::iterator it = m_propertiesTab.find(SETTING_TAB_COUNT);
      if (it != m_propertiesTab.end())
      {
        tabCount = max(_wtoi(it->second) - 1, 0);

        if (tabCount > 0)
        {
          m_tabNumber.Format(L"%d", tabCount);

          m_propertiesTab[SETTING_TAB_COUNT] = m_tabNumber;
        }
        else
        {
          it = m_propertiesTab.find(SETTING_TAB_START_TIME);
          if (it != m_propertiesTab.end())
          {
            m_propertiesTab.erase(it);
          }

          it = m_propertiesTab.find(SETTING_TAB_COUNT);
          if (it != m_propertiesTab.end())
          {
            m_propertiesTab.erase(it);
          }
        }

        m_isDirtyTab = true;               
      }
    }
    s_criticalSectionLocal.Unlock();

    WriteTab(false);
  }

  return tabCount == 0;
}


void CPluginSettings::TogglePluginEnabled()
{
  CPluginSettingsTabLock lock;
  if (lock.IsLocked())
  {
    ReadTab(false);

    s_criticalSectionLocal.Lock();
    {
      m_isPluginEnabledTab = m_isPluginEnabledTab ? false : true;
      m_propertiesTab[SETTING_TAB_PLUGIN_ENABLED] = m_isPluginEnabledTab ? "true" : "false";
      m_isDirtyTab = true;
    }
    s_criticalSectionLocal.Unlock();

    WriteTab(false);
  }
}
void CPluginSettings::SetPluginDisabled()
{
  CPluginSettingsTabLock lock;
  if (lock.IsLocked())
  {
    ReadTab(false);

    s_criticalSectionLocal.Lock();
    {
      m_isPluginEnabledTab = false;
      m_propertiesTab[SETTING_TAB_PLUGIN_ENABLED] = "false";
      m_isDirtyTab = true;
    }
    s_criticalSectionLocal.Unlock();

    WriteTab(false);
  }
}
void CPluginSettings::SetPluginEnabled()
{
  CPluginSettingsTabLock lock;
  if (lock.IsLocked())
  {
    ReadTab(false);

    s_criticalSectionLocal.Lock();
    {
      m_isPluginEnabledTab = true;
      m_propertiesTab[SETTING_TAB_PLUGIN_ENABLED] = "true";
      m_isDirtyTab = true;
    }
    s_criticalSectionLocal.Unlock();

    WriteTab(false);
  }
}
bool CPluginSettings::GetPluginEnabled() const
{
  return m_isPluginEnabledTab;
}


void CPluginSettings::AddError(const CString& error, const CString& errorCode)
{
  DEBUG_SETTINGS(L"SettingsTab::AddError error:" + error + " code:" + errorCode)

    CPluginSettingsTabLock lock;
  if (lock.IsLocked())
  {
    ReadTab(false);

    s_criticalSectionLocal.Lock();
    {
      if (m_errorsTab.find(error) == m_errorsTab.end())
      {
        m_errorsTab[error] = errorCode; 
        m_isDirtyTab = true;
      }
    }
    s_criticalSectionLocal.Unlock();

    WriteTab(false);
  }
}


CString CPluginSettings::GetErrorList() const
{
  CString errors;

  s_criticalSectionLocal.Lock();
  {
    for (TProperties::const_iterator it = m_errorsTab.begin(); it != m_errorsTab.end(); ++it)
    {
      if (!errors.IsEmpty())
      {
        errors += ',';
      }

      errors += it->first + '.' + it->second;
    }
  }
  s_criticalSectionLocal.Unlock();

  return errors;
}


void CPluginSettings::RemoveErrors()
{
  CPluginSettingsTabLock lock;
  if (lock.IsLocked())
  {
    ReadTab(false);

    s_criticalSectionLocal.Lock();
    {
      if (m_errorsTab.size() > 0)
      {
        m_isDirtyTab = true;
      }
      m_errorsTab.clear();
    }
    s_criticalSectionLocal.Unlock();

    WriteTab(false);
  }
}


bool CPluginSettings::GetForceConfigurationUpdateOnStart() const
{
  bool isUpdating = false;

  CPluginSettingsTabLock lock;
  if (lock.IsLocked())
  {
    s_criticalSectionLocal.Lock();
    {
      isUpdating = m_propertiesTab.find(SETTING_TAB_UPDATE_ON_START) != m_propertiesTab.end();
    }
    s_criticalSectionLocal.Unlock();
  }

  return isUpdating;
}


void CPluginSettings::ForceConfigurationUpdateOnStart(bool isUpdating)
{
  CPluginSettingsTabLock lock;
  if (lock.IsLocked())
  {
    ReadTab(false);

    s_criticalSectionLocal.Lock();
    {
      TProperties::iterator it = m_propertiesTab.find(SETTING_TAB_UPDATE_ON_START);

      if (isUpdating && it == m_propertiesTab.end())
      {
        m_propertiesTab[SETTING_TAB_UPDATE_ON_START] = "true";
        m_propertiesTab[SETTING_TAB_UPDATE_ON_START_REMOVE] = "false";

        m_isDirtyTab = true;
      }
      else if (!isUpdating)
      {
        // OK to remove?
        TProperties::iterator itRemove = m_propertiesTab.find(SETTING_TAB_UPDATE_ON_START_REMOVE);

        if (itRemove == m_propertiesTab.end() || itRemove->second == "true")
        {
          if (it != m_propertiesTab.end())
          {
            m_propertiesTab.erase(it);
          }

          if (itRemove != m_propertiesTab.end())
          {
            m_propertiesTab.erase(itRemove);
          }

          m_isDirtyTab = true;
        }
      }
    }
    s_criticalSectionLocal.Unlock();

    WriteTab(false);
  }
}

void CPluginSettings::RemoveForceConfigurationUpdateOnStart()
{
  CPluginSettingsTabLock lock;
  if (lock.IsLocked())
  {
    ReadTab(false);

    s_criticalSectionLocal.Lock();
    {
      // OK to remove?
      TProperties::iterator itRemove = m_propertiesTab.find(SETTING_TAB_UPDATE_ON_START_REMOVE);

      if (itRemove != m_propertiesTab.end())
      {
        m_propertiesTab.erase(itRemove);
        m_isDirtyTab = true;
      }
    }
    s_criticalSectionLocal.Unlock();

    WriteTab(false);
  }
}

void CPluginSettings::RefreshTab()
{
  CPluginSettingsTabLock lock;
  if (lock.IsLocked())
  {
    ReadTab();
  }
}


int CPluginSettings::GetTabVersion(const CString& key) const
{
  int version = 0;

  s_criticalSectionLocal.Lock();
  {
    TProperties::const_iterator it = m_propertiesTab.find(key);
    if (it != m_propertiesTab.end())
    {
      version = _wtoi(it->second);
    }
  }
  s_criticalSectionLocal.Unlock();

  return version;
}

void CPluginSettings::IncrementTabVersion(const CString& key)
{
  CPluginSettingsTabLock lock;
  if (lock.IsLocked())
  {
    ReadTab(false);

    s_criticalSectionLocal.Lock();
    {
      int version = 1;

      TProperties::iterator it = m_propertiesTab.find(key);
      if (it != m_propertiesTab.end())
      {
        version = _wtoi(it->second) + 1;
      }

      CString versionString;
      versionString.Format(L"%d", version);

      m_propertiesTab[key] = versionString;
    }
    s_criticalSectionLocal.Unlock();

    m_isDirtyTab = true;

    WriteTab(false);        
  }
}


// ============================================================================
// Whitelist settings
// ============================================================================

#ifdef SUPPORT_WHITELIST

void CPluginSettings::ClearWhitelist()
{
  s_criticalSectionLocal.Lock();
  {
    m_whitelistedDomains.clear();
  }
  s_criticalSectionLocal.Unlock();
}


bool CPluginSettings::ReadWhitelist(bool isDebug)
{
  bool isRead = true;

  DEBUG_SETTINGS("SettingsWhitelist::Read")

    if (isDebug)
    {
      DEBUG_GENERAL("*** Loading whitelist settings");
    }

    CPluginSettingsWhitelistLock lock;
    if (lock.IsLocked())
    {
      ClearWhitelist();

      s_criticalSectionLocal.Lock();
      try
      {
      std::vector<AdblockPlus::FilterPtr> filters = CPluginClient::GetInstance()->GetFilterEngine()->GetListedFilters();
      for (int i = 0; i < filters.size(); i ++)
      {
        if (filters[i]->GetProperty("type", AdblockPlus::Filter::Type::TYPE_INVALID) == AdblockPlus::Filter::Type::TYPE_EXCEPTION)
        {
          std::string text = filters[i]->GetProperty("text", "");
          //@@||example.com^$document
          size_t endPos = text.rfind("^$document");
          if (endPos != std::string::npos)
          {
            size_t startPos = text.find("@@||") + 4;
            if (startPos != std::string::npos)
            {
              m_whitelistedDomains.push_back(text.substr(startPos, endPos - startPos));
            }
          }
        }
      }
      }
      catch(std::runtime_error ex)
      {
        DEBUG_GENERAL(ex.what());
      }
      catch(std::exception ex)
      {
        DEBUG_GENERAL(ex.what());
      }
      s_criticalSectionLocal.Unlock();
    }
    else
    {
      isRead = false;
    }

    return isRead;
}


void CPluginSettings::AddWhiteListedDomain(const CString& domain)
{
  DEBUG_SETTINGS("SettingsWhitelist::AddWhiteListedDomain domain:" + domain)

  bool isNewVersion = false;
  bool isForcingUpdateOnStart = false;

  CPluginSettingsWhitelistLock lock;
  if (lock.IsLocked())
  {
    ReadWhitelist(false);

    std::string newDomain = CW2A(domain, CP_UTF8);

    //Domain already present?
    if (std::find(m_whitelistedDomains.begin(), m_whitelistedDomains.end(), newDomain) != m_whitelistedDomains.end())
    {
      return;
    }
    s_criticalSectionLocal.Lock();
    {
      AdblockPlus::FilterPtr whitelistFilter = CPluginClient::GetInstance()->GetFilterEngine()->GetFilter(std::string("@@||").append(CW2A(domain)).append("^$document"));
      whitelistFilter->AddToList();
    }
    s_criticalSectionLocal.Unlock();

  }

  if (isForcingUpdateOnStart)
  {
    ForceConfigurationUpdateOnStart();
  }
}


bool CPluginSettings::IsWhiteListedDomain(const CString& domain) const
{
  bool bIsWhiteListed;

  s_criticalSectionLocal.Lock();
  {
    bIsWhiteListed = std::find(m_whitelistedDomains.begin(), m_whitelistedDomains.end(), std::string(CW2A(domain, CP_UTF8))) != m_whitelistedDomains.end();
  }
  s_criticalSectionLocal.Unlock();

  return bIsWhiteListed;
}

int CPluginSettings::GetWhiteListedDomainCount() const
{
  int count = 0;

  s_criticalSectionLocal.Lock();
  {
    count = (int)m_whitelistedDomains.size();
  }
  s_criticalSectionLocal.Unlock();

  return count;
}


std::vector<std::string> CPluginSettings::GetWhiteListedDomainList()
{
  bool r = ReadWhitelist(false);
  return m_whitelistedDomains;
}


bool CPluginSettings::RefreshWhitelist()
{
  CPluginSettingsWhitelistLock lock;
  if (lock.IsLocked())
  {
    ReadWhitelist(true);
  }

  return true;
}

DWORD CPluginSettings::GetWindowsBuildNumber()
{
  if (m_WindowsBuildNumber == 0)
  {
    OSVERSIONINFOEX osvi;
    SYSTEM_INFO si;
    BOOL bOsVersionInfoEx;

    ZeroMemory(&si, sizeof(SYSTEM_INFO));
    ZeroMemory(&osvi, sizeof(OSVERSIONINFOEX));

    osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
    bOsVersionInfoEx = GetVersionEx((OSVERSIONINFO*) &osvi);

    m_WindowsBuildNumber = osvi.dwBuildNumber;
  }

  return m_WindowsBuildNumber;
}

void CPluginSettings::SetSubscription(BSTR url)
{
  std::string urlConverted = CT2A(url, CP_UTF8);
  SetSubscription(urlConverted);
}

void CPluginSettings::SetSubscription(std::string url)
{
  try
  {
    FilterEngine* filterEngine= CPluginClient::GetInstance()->GetFilterEngine();
    std::vector<AdblockPlus::SubscriptionPtr> subscriptions = filterEngine->GetListedSubscriptions();
    if (subscriptions.size() > 0)
    {
      for (int i = 0; i < subscriptions.size(); i ++)
      {
        subscriptions[i]->RemoveFromList();
      }
    }
    AdblockPlus::SubscriptionPtr subscription = filterEngine->GetSubscription(url);
    subscription->AddToList();
    RefreshFilterlist();
    RefreshWhitelist();
  }
  catch(std::exception ex)
  {
    DEBUG_GENERAL(ex.what());
  }
  catch(std::runtime_error ex)
  {
    DEBUG_GENERAL(ex.what());
  }
}

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> retTokens;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        retTokens.push_back(item);
    }
    return retTokens;
}


void CPluginSettings::SetDefaultSubscription()
{
  CPluginSystem* system = CPluginSystem::GetInstance();
  CString lng = system->GetBrowserLanguage().Left(2);
  std::string browserLanguage = CW2A(lng, CP_UTF8);
  FilterEngine* filterEngine = CPluginClient::GetInstance()->GetFilterEngine();
  std::vector<SubscriptionPtr> subscriptions = filterEngine->FetchAvailableSubscriptions();
  bool subscriptionSet = false;
  while (!subscriptionSet)
  {
    for (int i = 0; i < subscriptions.size(); i++)
    {
      std::string prefixes = subscriptions[i]->GetProperty("prefixes", "");
      std::vector<std::string> tokens = split(prefixes, ',');
      for (int j = 0; j < tokens.size(); j ++)
      {
        if (tokens[j] == browserLanguage)
        {
          SetSubscription(subscriptions[i]->GetProperty("url", ""));
          subscriptionSet = true;
        }
      }
    }

    if (browserLanguage == "en")
      break;
    // failed to set the subscription for browser language. Try "en"
    browserLanguage = "en";

  }
}

CString CPluginSettings::GetSubscription()
{
  try
  {
    FilterEngine* filterEngine= CPluginClient::GetInstance()->GetFilterEngine();
    std::vector<AdblockPlus::SubscriptionPtr> subscriptions = filterEngine->GetListedSubscriptions();

    if (subscriptions.size() == 0)
    {
      SetDefaultSubscription();
      subscriptions = filterEngine->GetListedSubscriptions();
    }
    for (int i = 0; i < subscriptions.size(); i ++)
    {
      return CString(CA2T(subscriptions[i]->GetProperty("url", std::string()).c_str(), CP_UTF8));
    }
  }
  catch(std::exception ex)
  {
    DEBUG_GENERAL(ex.what());
  }
  catch(std::runtime_error ex)
  {
    DEBUG_GENERAL(ex.what());
  }
  return CString(L"");
}


void CPluginSettings::RefreshFilterlist()
{
  try
  {
    FilterEngine* filterEngine= CPluginClient::GetInstance()->GetFilterEngine();
    
    // Make sure at least the default subscription is set
    CPluginSettings* settings = CPluginSettings::GetInstance();
    settings->GetSubscription();

    std::vector<AdblockPlus::SubscriptionPtr> subscriptions = filterEngine->GetListedSubscriptions();
    for (int i = 0; i < subscriptions.size(); i ++)
    {
      subscriptions[i]->UpdateFilters();
    }
  }
  catch(std::exception ex)
  {
    DEBUG_GENERAL(ex.what());
  }
  catch(std::runtime_error ex)
  {
    DEBUG_GENERAL(ex.what());
  }
}

#endif // SUPPORT_WHITELIST
