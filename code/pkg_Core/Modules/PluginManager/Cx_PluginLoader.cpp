// x3c - C++ PluginFramework
#include <UtilFunc/PluginInc.h>
#include "Cx_PluginLoader.h"
#include <PluginManager/Ix_AppWorkPath.h>
#include <UtilFunc/LockCount.h>
#include <UtilFunc/ScanFiles.h>
#include <UtilFunc/SysErrStr.h>

#include <Xml/Ix_ConfigXml.h>
#include <Xml/Cx_ConfigSection.h>
#include <Xml/Ix_ConfigTransaction.h>
#include <UtilFunc/ConvStr.h>

Cx_PluginLoader::Cx_PluginLoader()
    : m_instance(NULL)
{
    m_clsfile[0] = 0;
}

Cx_PluginLoader::~Cx_PluginLoader()
{
}

HMODULE Cx_PluginLoader::GetMainModuleHandle()
{
    return m_instance;
}

void Cx_PluginLoader::ReplaceSlashes(wchar_t* filename)
{
    for (; *filename; ++filename)
    {
#ifdef _WIN32
        if (L'/' == *filename)
        {
            *filename = L'\\';
        }
#else
        if (L'\\' == *filename)
        {
            *filename = L'/';
        }
#endif
    }
}

void Cx_PluginLoader::MakeFullPath(wchar_t* fullpath, HMODULE instance, const wchar_t* path)
{
    if (!path || 0 == path[0] || PathIsRelativeW(path))
    {
        GetModuleFileNameW(instance, fullpath, MAX_PATH);
        PathRemoveFileSpecW(fullpath);
        PathAppendW(fullpath, path);
    }
    else
    {
        wcscpy_s(fullpath, MAX_PATH, path);
    }
    ReplaceSlashes(fullpath);
    PathAddBackslashW(fullpath);
}

long Cx_PluginLoader::LoadPlugins(HMODULE instance, const wchar_t* path,
                                  const wchar_t* ext, bool recursive,
                                  bool enableDelayLoading)
{
    wchar_t fullpath[MAX_PATH];

    m_instance = instance;
    MakeFullPath(fullpath, instance, path);

    return LoadPlugins(fullpath, ext, recursive, enableDelayLoading);
}

long Cx_PluginLoader::LoadPlugins(const wchar_t* path, const wchar_t* ext,
                                  bool recursive, bool enableDelayLoading)
{
    wchar_t fullpath[MAX_PATH];
    std::vector<std::wstring> filenames;

    MakeFullPath(fullpath, NULL, path);
    FindPlugins(filenames, fullpath, ext, recursive);

    return InLoadPlugins(filenames, enableDelayLoading);
}

long Cx_PluginLoader::InLoadPlugins(const std::vector<std::wstring>& filenames, bool enableDelayLoading)
{
    long count = 0;
    std::vector<std::wstring>::const_iterator it;

    if (!filenames.empty())
    {
        if (enableDelayLoading)
        {
            LoadCacheFile(filenames.front().c_str());
        }

        for (it = filenames.begin(); it != filenames.end(); ++it)
        {
            if (LoadPluginOrDelay(it->c_str(), enableDelayLoading))
            {
                count++;
            }
        }
    }

    return count;
}

class CScanPluginsByExt : public x3::CScanFilesCallback
{
public:
    CScanPluginsByExt(std::vector<std::wstring>* files, const wchar_t* ext)
        : m_files(files), m_ext(ext), m_extlen(wcslen(ext))
    {
    }

private:
    virtual void OnCheckFile(const wchar_t* filename, const wchar_t*, bool&)
    {
        size_t len = wcslen(filename);
        if (len >= m_extlen && _wcsicmp(&filename[len - m_extlen], m_ext) == 0)
        {
            m_files->push_back(filename);
        }
    }

private:
    CScanPluginsByExt();
    CScanPluginsByExt(const CScanPluginsByExt&);
    void operator=(const CScanPluginsByExt&);

    std::vector<std::wstring>*  m_files;
    const wchar_t*              m_ext;
    const size_t                m_extlen;
};

void Cx_PluginLoader::FindPlugins(std::vector<std::wstring>& filenames,
                                  const wchar_t* path, const wchar_t* ext,
                                  bool recursive)
{
    CScanPluginsByExt scanner(&filenames, ext);
    x3::ScanFiles(&scanner, path, recursive);
}

bool Cx_PluginLoader::issep(wchar_t c)
{
    return L',' == c || L';' == c || iswspace(c);
}

long Cx_PluginLoader::LoadPluginFiles(const wchar_t* path,
                                      const wchar_t* files,
                                      HMODULE instance,
                                      bool enableDelayLoading)
{
    wchar_t filename[MAX_PATH];

    m_instance = instance;
    MakeFullPath(filename, instance, path);

    const size_t len0 = wcslen(filename);
    wchar_t* nameend = filename + len0;

    std::vector<std::wstring> filenames;
    size_t i, j;

    for (i = 0; files[i] != 0; )
    {
        while (issep(files[i]))
        {
            i++;
        }
        for (j = i; files[j] != 0 && !issep(files[j]); j++)
        {
        }
        if (j > i)
        {
            wcsncpy_s(nameend, MAX_PATH - len0, files + i,
                MAX_PATH - len0 < j - i ? MAX_PATH - len0 : j - i);
            nameend[j - i] = 0;
            ReplaceSlashes(filename);
            if (wcschr(nameend, L'.') == NULL)
                wcscat_s(filename, MAX_PATH, L".plugin" PLNEXT);
            filenames.push_back(filename);
        }
        i = j;
    }

    return InLoadPlugins(filenames, enableDelayLoading);
}

long Cx_PluginLoader::InitializePlugins()
{
    CLockCount locker(&m_loading);
    long count = 0;

    for (long i = 0; i < x3::GetSize(m_modules); i++)
    {
        if (m_modules[i]->inited)
        {
            continue;
        }
        if (!m_modules[i]->hdll) // delay-load
        {
            count++;
            m_modules[i]->inited = true;
            continue;
        }

        typedef bool (*FUNC_INIT)();
        FUNC_INIT pfn = (FUNC_INIT)GetProcAddress(m_modules[i]->hdll, "x3InitializePlugin");

        if (pfn && !(*pfn)())
        {
            GetModuleFileNameW(m_modules[i]->hdll, m_modules[i]->filename, MAX_PATH);
            X3LOG_WARNING2(L"@PluginManager:IDS_INITPLUGIN_FAIL", m_modules[i]->filename);
            VERIFY(UnloadPlugin(m_modules[i]->filename));
            i--;
        }
        else
        {
            count++;
            m_modules[i]->inited = true;
        }
    }

    SaveClsids();
    return count;
}

int Cx_PluginLoader::GetPluginIndex(const wchar_t* filename)
{
    const wchar_t* title = PathFindFileNameW(filename);
    int i = x3::GetSize(m_modules);

    while (--i >= 0)
    {
        // ignore folders
        if (_wcsicmp(title, PathFindFileNameW(m_modules[i]->filename)) == 0)
        {
            break;
        }
    }

    return i;
}

bool Cx_PluginLoader::RegisterPlugin(HMODULE instance)
{
    if (FindModule(instance) >= 0)
    {
        return false;
    }

    Ix_Module* pModule = GetModule(instance);

    if (pModule != NULL)
    {
        MODULE moduleInfo;

        moduleInfo.hdll = instance;
        moduleInfo.module = pModule;
        moduleInfo.owned = false;
        moduleInfo.inited = false;
        GetModuleFileNameW(moduleInfo.hdll, moduleInfo.filename, MAX_PATH);

        int moduleIndex = GetPluginIndex(moduleInfo.filename);
        if (moduleIndex >= 0)
        {
            ASSERT(m_modules[moduleIndex] != NULL);
            *m_modules[moduleIndex] = moduleInfo;
        }
        else
        {
            moduleIndex = x3::GetSize(m_modules);
            MODULE* module = new MODULE;
            *module = moduleInfo;
            m_modules.push_back(module);
        }

        RegisterClassEntryTable(moduleIndex);

        return true;
    }

    return false;
}

bool Cx_PluginLoader::LoadPlugin(const wchar_t* filename)
{
    if (!x3InMainThread())
    {
        X3LOG_WARNING2(L"Can't load plugin in sub thread.", filename);
        return false;
    }

    CLockCount locker(&m_loading);
    int existIndex = GetPluginIndex(filename);

    if (existIndex >= 0 && m_modules[existIndex]->hdll)
    {
        if (_wcsicmp(filename, m_modules[existIndex]->filename) != 0)
        {
            X3LOG_DEBUG2(L"The plugin is already loaded.", filename << L", " << (m_modules[existIndex]->filename));
        }
        return false;
    }

    HMODULE hdll = x3LoadLibrary(filename);
    DWORD errcode = GetLastError();

    if (hdll)
    {
        if (RegisterPlugin(hdll))
        {
            int moduleIndex = FindModule(hdll);

            ASSERT(moduleIndex >= 0);
            ASSERT(existIndex < 0 || existIndex == moduleIndex);

            m_modules[moduleIndex]->owned = true;
#ifdef _WIN32
            DisableThreadLibraryCalls(hdll);
#endif
        }
        else
        {
            FreeLibrary(hdll);
            hdll = NULL;
        }
    }
    else if (PathFileExistsW(filename))
    {
        X3LOG_WARNING2(L"Fail to load plugin.", x3::GetSystemErrorString(errcode) << L", " << filename);
    }

    return hdll != NULL;
}

bool Cx_PluginLoader::UnloadPlugin(const wchar_t* name)
{
    CLockCount locker(&m_unloading);
    int moduleIndex = GetPluginIndex(name);
    HMODULE hdll = moduleIndex < 0 ? NULL : m_modules[moduleIndex]->hdll;

    if (NULL == hdll)
    {
        return false;
    }

    typedef bool (*FUNC_CANUNLOAD)();
    FUNC_CANUNLOAD pfnCan = (FUNC_CANUNLOAD)GetProcAddress(hdll, "x3CanUnloadPlugin");

    if (pfnCan && !pfnCan())
    {
        return false;
    }

    typedef void (*FUNC_UNLOAD)();
    FUNC_UNLOAD pfnUnload = (FUNC_UNLOAD)GetProcAddress(hdll, "x3UninitializePlugin");

    if (pfnUnload)
    {
        pfnUnload();
    }

    VERIFY(ClearModuleItems(hdll));
    ReleaseModule(hdll);

    return true;
}

long Cx_PluginLoader::UnloadPlugins()
{
    CLockCount locker(&m_unloading);
    SaveClsids();
    m_cache.Release();

    long i = 0;
    long count = 0;

    for (i = x3::GetSize(m_modules) - 1; i >= 0; i--)
    {
        typedef void (*FUNC_UNLOAD)();
        FUNC_UNLOAD pfnUnload = (FUNC_UNLOAD)GetProcAddress(m_modules[i]->hdll, "x3UninitializePlugin");
        if (pfnUnload)
        {
            pfnUnload();
        }
    }

    for (i = x3::GetSize(m_modules) - 1; i >= 0; i--)
    {
        ClearModuleItems(m_modules[i]->hdll);
    }

    for (i = x3::GetSize(m_modules) - 1; i >= 0; i--)
    {
        if (m_modules[i]->hdll)
        {
            ReleaseModule(m_modules[i]->hdll);
            count++;
        }
    }

    return count;
}

bool Cx_PluginLoader::ClearModuleItems(HMODULE hModule)
{
    Ix_Module* pModule = GetModule(hModule);

    if (pModule)
    {
        pModule->ClearModuleItems();
        return true;
    }

    return false;
}

long Cx_PluginLoader::GetPluginCount()
{
    return x3::GetSize(m_modules);
}

bool Cx_PluginLoader::GetPluginFileName(long index, HMODULE& hdll, std::wstring& filename)
{
    bool valid = x3::IsValidIndexOf(m_modules, index);

    hdll = valid ? m_modules[index]->hdll : NULL;
    filename = valid ? m_modules[index]->filename : L"";

    return valid;
}


//zhuyf: move from DelayLoad.cpp
bool Cx_PluginLoader::LoadPluginOrDelay(const wchar_t* pluginFile, bool enableDelayLoading)
{
    if (GetPluginIndex(pluginFile) >= 0)    // Already loaded.
    {
        return true;
    }
    if (m_unloading != 0)                   // Don't load when quiting.
    {
        return false;
    }

    bool ret = false;

    if (enableDelayLoading && m_cache.IsNotNull())
    {
        ret = LoadClsidsFromCache(pluginFile);
        if (!ret && LoadPlugin(pluginFile))
        {
            ret = true;
            BuildPluginCache(GetPluginIndex(pluginFile));
        }
    }
    else
    {
        ret = LoadPlugin(pluginFile);
    }

    return ret;
}

bool Cx_PluginLoader::LoadDelayedPlugin_(const wchar_t* filename)
{
    CLockCount locker(&m_loading);
    bool ret = false;

    if (LoadPlugin(filename))
    {
        int moduleIndex = GetPluginIndex(filename);

        typedef bool (*FUNC_PLUGINLOAD)();
        FUNC_PLUGINLOAD pfn = (FUNC_PLUGINLOAD)GetProcAddress(m_modules[moduleIndex]->hdll, "x3InitializePlugin");

        if (pfn && !(*pfn)())
        {
            X3LOG_WARNING2(L"@PluginManager:IDS_INITPLUGIN_FAIL", filename);
            VERIFY(UnloadPlugin(filename));
        }
        else
        {
            m_modules[moduleIndex]->inited = true;
            BuildPluginCache(moduleIndex);
            ret = true;
        }
    }

    return ret;
}

bool Cx_PluginLoader::BuildPluginCache(int moduleIndex)
{
    ASSERT(moduleIndex >= 0);
    const MODULE* module = m_modules[moduleIndex];

    if (GetProcAddress(module->hdll, "DllGetClassObject") != NULL)
    {
        CLockCount locker(&m_loading);
        AddObserverPlugin(module->hdll, "x3::complugin");
    }

    CLSIDS oldids;
    LoadClsids(oldids, module->filename);

    return oldids != module->clsids
        && SaveClsids(module->clsids, module->filename);
}

bool Cx_PluginLoader::LoadClsidsFromCache(const wchar_t* filename)
{
    int moduleIndex = GetPluginIndex(filename);

    if (moduleIndex >= 0 && !m_modules[moduleIndex]->clsids.empty())
    {
        return true;    // this function is already called.
    }

    X3CLASSENTRY cls;
    CLSIDS clsids;

    if (!LoadClsids(clsids, filename))  // No clsid and no observer.
    {
        return false;
    }

    if (moduleIndex < 0)
    {
        MODULE* module = new MODULE;

        module->hdll = NULL;
        module->module = NULL;
        module->owned = false;
        module->inited = false;
        wcscpy_s(module->filename, MAX_PATH, filename);

        moduleIndex = x3::GetSize(m_modules);
        m_modules.push_back(module);
    }

    memset(&cls, 0, sizeof(cls));
    for (CLSIDS::const_iterator it = clsids.begin(); it != clsids.end(); ++it)
    {
        cls.clsid = *it;
        if (m_clsmap.find(cls.clsid.str()) == m_clsmap.end())
        {
            m_clsmap[cls.clsid.str()] = MAPITEM(cls, moduleIndex);
            m_modules[moduleIndex]->clsids.push_back(cls.clsid);
        }
    }

    return true;
}

bool Cx_PluginLoader::LoadCacheFile(const wchar_t* pluginFile)
{
    bool loaded = false;

    if (m_cache.IsNull() && 0 == m_clsfile[0])
    {
        // Ensure ConfigXml.plugin is loaded.
        wcscpy_s(m_clsfile, MAX_PATH, pluginFile);
        PathRemoveFileSpecW(m_clsfile);
        PathAppendW(m_clsfile, L"ConfigXml.plugin" PLNEXT);
        loaded = LoadPlugin(m_clsfile);

        Cx_Interface<Ix_ConfigXml> pIFFile(x3::CLSID_ConfigXmlFile);
        if (pIFFile)
        {
            // Get application name.
            GetModuleFileNameW(m_instance, m_clsfile, MAX_PATH);
            std::wstring appname(PathFindFileNameW(m_clsfile));

            // Make cache file name.
            wcscpy_s(m_clsfile, MAX_PATH, GetWorkPath().c_str());
            PathAppendW(m_clsfile, L"config");
            if (!PathFileExistsW(m_clsfile))
                PathRemoveFileSpecW(m_clsfile);
            PathAppendW(m_clsfile, appname.c_str());
            PathRenameExtensionW(m_clsfile, L".clsbuf");

            pIFFile->SetFileName(m_clsfile);
            pIFFile->SetRootName(L"cache");
            m_cache = pIFFile;

            Cx_ConfigSection root(pIFFile->GetData()->GetSection(L""));
            root->SetString(L"appname", appname.c_str());
        }
    }

    return loaded;
}

bool Cx_PluginLoader::LoadClsids(CLSIDS& clsids, const wchar_t* pluginFile)
{
    const wchar_t* name = PathFindFileNameW(pluginFile);
    Cx_Interface<Ix_ConfigXml> pIFFile(m_cache);
    Cx_ConfigSection secPlugin;

    clsids.clear();

    if (pIFFile)
    {
        secPlugin = pIFFile->GetData()->GetSection(NULL,
            L"plugins/plugin", L"name", name, false);
        Cx_ConfigSection seclist(secPlugin.GetSection(L"clsids", false));

        for (int i = 0; i < 999; i++)
        {
            Cx_ConfigSection sec(seclist.GetSectionByIndex(L"clsid", i));
            if (!sec->IsValid())
                break;
            X3CLSID clsid(x3::w2a(sec->GetString(L"id")).c_str());
            if (clsid.valid() && x3::find_value(clsids, clsid) < 0)
            {
                clsids.push_back(clsid);
            }
        }
    }

    bool has = !clsids.empty();
    if (!has && secPlugin)
    {
        Cx_ConfigSection seclist(secPlugin.GetSection(L"observers", false));
        has = (seclist.GetSectionCount(L"observer") > 0);
    }

    return has; // Has clsid or observer.
}

bool Cx_PluginLoader::SaveClsids(const CLSIDS& clsids, const wchar_t* pluginFile)
{
    Cx_Interface<Ix_ConfigXml> pIFFile(m_cache);

    if (pIFFile)
    {
        Cx_ConfigSection secPlugin(pIFFile->GetData()->GetSection(NULL,
            L"plugins/plugin", L"name", PathFindFileNameW(pluginFile)));
        Cx_ConfigSection seclist(secPlugin.GetSection(L"clsids"));

        secPlugin->SetString(L"filename", pluginFile);
        seclist.RemoveChildren(L"clsid");

        for (CLSIDS::const_iterator it = clsids.begin();
            it != clsids.end(); ++it)
        {
            Cx_ConfigSection sec(seclist.GetSection(L"clsid", L"id", x3::a2w(it->str()).c_str()));

            X3CLASSENTRY* pEntry = FindEntry(*it);
            if (pEntry && pEntry->pfnObjectCreator)
            {
                sec->SetString(L"class", x3::a2w(pEntry->className).c_str());
            }
        }
    }

    return true;
}

bool Cx_PluginLoader::SaveClsids()
{
    Cx_ConfigTransaction autosave(m_cache);
    return autosave.Submit();
}

void Cx_PluginLoader::AddObserverPlugin(HMODULE hdll, const char* obtype, const wchar_t* subtype)
{
    wchar_t filename[MAX_PATH] = { 0 };
    Cx_Interface<Ix_ConfigXml> pIFFile(m_cache);

    if (pIFFile && m_loading > 0)
    {
        GetModuleFileNameW(hdll, filename, MAX_PATH);
        const wchar_t* name = PathFindFileNameW(filename);
        
        Cx_ConfigSection secObserver(pIFFile->GetData()->GetSection(NULL,
            L"observers/observer", 
            L"type", x3::a2w(obtype).c_str(), 
            L"subtype", subtype));
        secObserver.GetSection(L"plugin", L"name", name);

        Cx_ConfigSection secPlugin(pIFFile->GetData()->GetSection(NULL, L"plugins/plugin", L"name", name));
        secPlugin->SetString(L"filename", filename);
        secPlugin.GetSection(L"observers/observer", L"type", x3::a2w(obtype).c_str());
    }
}

void Cx_PluginLoader::FireFirstEvent(const char* obtype, const wchar_t* subtype)
{
    Cx_Interface<Ix_ConfigXml> pIFFile(m_cache);

    if (pIFFile)
    {
        Cx_ConfigSection secObserver(pIFFile->GetData()->GetSection(NULL,
            L"observers/observer", 
            L"type", x3::a2w(obtype).c_str(), 
            L"subtype", subtype, false));

        for (int i = 0; i < 999; i++)
        {
            Cx_ConfigSection sec(secObserver.GetSectionByIndex(L"plugin", i));
            std::wstring shortflname(sec->GetString(L"name"));

            if (shortflname.empty())
            {
                break;
            }

            LoadDelayedPlugin(shortflname);
        }
    }
}

bool Cx_PluginLoader::LoadDelayedPlugin(const std::wstring& filename)
{
    int moduleIndex = GetPluginIndex(filename.c_str());
    return moduleIndex >= 0 && (m_modules[moduleIndex]->hdll
        || LoadDelayedPlugin_(m_modules[moduleIndex]->filename));
}
