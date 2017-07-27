// Copyright 2017-2018 zzu_softboy <zzu_softboy@163.com>
//
// THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
// IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
// OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
// IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
// NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
// THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Created by softboy on 7/25/17.

#include <ostream>
#include "zapi/bridge/IniEntry.h"
#include "zapi/bridge/Extension.h"
#include "zapi/bridge/internal/ExtensionPrivate.h"
#include "zapi/vm/Callable.h"

#ifdef ZTS
#include "TSRM.h"
#endif

#include <map>

/**
 * We're almost there, we now need to declare an instance of the
 * structure defined above (if building for a single thread) or some
 * sort of impossible to understand magic pointer-to-a-pointer (for
 * multi-threading builds). We make this a static variable because
 * this already is bad enough.
 */
ZEND_DECLARE_MODULE_GLOBALS(zapi)

namespace
{
   using zapi::bridge::Extension;
   using zapi::bridge::internal::ExtensionPrivate;
   /**
   * Function that must be defined to initialize the "globals"
   * We do not have to initialize anything, but PHP needs to call this
   * method (crazy)
   * @param  globals
   */
   void init_globals(zend_zapi_globals *globals){}
   
   std::map<std::string, Extension *> name2extension;
   std::map<int, Extension *> mid2extension;
   
   int match_module(zval *value)
   {
      zend_module_entry *entry = static_cast<zend_module_entry *>(Z_PTR_P(value));
      auto iter = name2extension.find(entry->name);
      if (iter == name2extension.end()) {
         return ZEND_HASH_APPLY_KEEP;
      }
      mid2extension[entry->module_number] = iter->second;
      return ZEND_HASH_APPLY_KEEP;
   }
   
   Extension *find_module(int mid)
   {
      auto iter = mid2extension.find(mid);
      if (iter != mid2extension.end()) {
         return iter->second;
      }
      zend_hash_apply(&module_registry, match_module);
      iter = mid2extension.find(mid);
      if (iter == mid2extension.end()) {
         return nullptr;
      }
      return iter->second;
   }
}

namespace zapi
{
namespace bridge
{

Extension::Extension(const char *name, const char *version, int apiVersion)
   : m_implPtr(new ExtensionPrivate(name, version, apiVersion, this))
{
   name2extension[name] = this;
}

Extension::~Extension()
{}

Extension &Extension::setStartupHandler(const Callback &callback)
{
   ZAPI_D(Extension);
   implPtr->m_startupHandler = callback;
   return *this;
}

Extension &Extension::setRequestHandler(const Callback &callback)
{
   ZAPI_D(Extension);
   implPtr->m_requestHandler = callback;
   return *this;
}

Extension &Extension::setIdleHandler(const Callback &callback)
{
   ZAPI_D(Extension);
   implPtr->m_idleHandler = callback;
   return *this;
}

Extension &Extension::setShutdownHandler(const Callback &callback)
{
   ZAPI_D(Extension);
   implPtr->m_shutdownHandler = callback;
   return *this;
}

void *Extension::getModule()
{
   return static_cast<void *>(getImplPtr()->getModule());
}

bool Extension::isLocked() const
{
   return getImplPtr()->isLocked();
}

const char *Extension::getName() const
{
   ZAPI_D(const Extension);
   return implPtr->m_entry.name;
}

const char *Extension::getVersion() const
{
   ZAPI_D(const Extension);
   return implPtr->m_entry.version;
}

Extension &Extension::registerFunction(const char *name, zapi::ZendCallable function, 
                                       const lang::Arguments &arguments)
{
   getImplPtr()->registerFunction(name, function, arguments);
   return *this;
}

bool Extension::initialize(int moduleNumber)
{
   return getImplPtr()->initialize(moduleNumber);
}

namespace internal
{
ExtensionPrivate::ExtensionPrivate(const char *name, const char *version, int apiversion, Extension *extension)
   : m_apiPtr(extension)
{
   // assign all members (apart from the globals)
   m_entry.size = sizeof(zend_module_entry);
   m_entry.zend_api = ZEND_MODULE_API_NO;
   m_entry.zend_debug = ZEND_DEBUG;
   m_entry.zts = USING_ZTS;
   m_entry.ini_entry = nullptr;
   m_entry.deps = nullptr;
   m_entry.name = name;
   m_entry.functions = nullptr;
   m_entry.module_startup_func = &ExtensionPrivate::processStartup;
   m_entry.module_shutdown_func = &ExtensionPrivate::processShutdown;
   m_entry.request_startup_func = &ExtensionPrivate::processRequest;
   m_entry.request_shutdown_func = &ExtensionPrivate::processIdle;
   m_entry.info_func = nullptr;
   m_entry.version = version;
   m_entry.globals_size = 0;
   m_entry.globals_ctor = nullptr;
   m_entry.globals_dtor = nullptr;
   m_entry.post_deactivate_func = nullptr;
   m_entry.module_started = 0;
   m_entry.type = 0;
   m_entry.handle = nullptr;
   m_entry.module_number = 0;
   m_entry.build_id = const_cast<char *>(static_cast<const char *>(ZEND_MODULE_BUILD_ID));
#ifdef ZTS
   m_entry.globals_id_ptr = nullptr;
#else
   m_entry.globals_ptr = nullptr;
#endif
   if (apiversion == ZAPI_API_VERSION) {
      return;
   }
   // mismatch between api versions, the extension is invalid, we use a
   // different startup function to report to the user
   m_entry.module_startup_func = &ExtensionPrivate::processMismatch;
   // the other callback functions are no longer necessary
   m_entry.module_shutdown_func = nullptr;
   m_entry.request_startup_func = nullptr;
   m_entry.request_shutdown_func = nullptr;
}

ExtensionPrivate::~ExtensionPrivate()
{
   name2extension.erase(m_entry.name);
   delete[] m_entry.functions;
}

const char *ExtensionPrivate::getName() const
{
   return m_entry.name;
}

const char *ExtensionPrivate::getVersion() const
{
   return m_entry.version;
}

size_t ExtensionPrivate::getFunctionQuantity() const
{
   // now just return global namespaces functions
   return m_functions.size();
}

size_t ExtensionPrivate::getIniEntryQuantity() const
{
   return m_iniEntries.size(); 
}

zend_module_entry *ExtensionPrivate::getModule()
{
   if (m_entry.functions) {
      return &m_entry;
   }
   if (m_entry.module_startup_func == &ExtensionPrivate::processMismatch) {
      return &m_entry;
   }
   size_t count = getFunctionQuantity();
   if (0 == count) {
      return &m_entry;
   }
   int i = 0;
   zend_function_entry *entries = new zend_function_entry[count + 1];
   iterateFunctions([&i, entries](Callable &callable){
      callable.initialize(&entries[i]);
      i++;
   });
   zend_function_entry *last = &entries[count];
   memset(last, 0, sizeof(zend_function_entry));
   m_entry.functions = entries;
   return &m_entry;
}

void ExtensionPrivate::iterateFunctions(const std::function<void(Callable &func)> &callback)
{
   for (auto &function : m_functions) {
      callback(*function);
   }
}

void ExtensionPrivate::iterateIniEntries(const std::function<void (bridge::IniEntry &)> &callback)
{
   for (auto &entry : m_iniEntries) {
      callback(*entry);
   }
}

int ExtensionPrivate::processIdle(int type, int moduleNumber)
{
   return 0;
}

int ExtensionPrivate::processMismatch(int type, int moduleNumber)
{
   Extension *extension = find_module(moduleNumber);
   // @TODO is this really good? we need a method to check compatibility more graceful
   zapi::warning << " Version mismatch between zendAPI and extension " << extension->getName()
                 << " " << extension->getVersion() << " (recompile needed?) " << std::endl;
   return BOOL2SUCCESS(true);
}

int ExtensionPrivate::processRequest(int type, int moduleNumber)
{
   
   return 0;
}

int ExtensionPrivate::processStartup(int type, int moduleNumber)
{
   ZEND_INIT_MODULE_GLOBALS(zapi, init_globals, nullptr);
   Extension *extension = find_module(moduleNumber);
   return BOOL2SUCCESS(extension->initialize(moduleNumber));
}

int ExtensionPrivate::processShutdown(int type, int moduleNumber)
{
   return 0;
}

ExtensionPrivate &ExtensionPrivate::registerFunction(const char *name, zapi::ZendCallable function, 
                                                     const Arguments &arguments)
{
   if (isLocked()) {
      return *this;
   }
   m_functions.push_back(std::make_shared<Callable>(name, function, arguments));
   return *this;
}

bool ExtensionPrivate::initialize(int moduleNumber)
{
   m_zendIniDefs.reset(new zend_ini_entry_def[getIniEntryQuantity() + 1]);
   int i = 0;
   // fill ini entry def
   iterateIniEntries([this, &i, moduleNumber](IniEntry &iniEntry){
      zend_ini_entry_def *zendIniDef = &m_zendIniDefs[i];
      iniEntry.setupIniEntryDef(zendIniDef, moduleNumber);
      i++;
   });
   memset(&m_zendIniDefs[i], 0, sizeof(m_zendIniDefs[i]));
   zend_register_ini_entries(m_zendIniDefs.get(), moduleNumber);
   if (m_startupHandler) {
      m_startupHandler();
   }
   // remember that we're initialized (when you use "apache reload" it is
   // possible that the processStartup() method is called more than once)
   m_locked = true;
   return true;
}

bool ExtensionPrivate::shutdown(int moduleNumber)
{
   return true;
}


} // internal

} // bridge
} // zapi