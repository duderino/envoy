#include "extensions/common/wasm/wavm/wavm.h"

#include <stdio.h>
#include <stdint.h>
#include <atomic>
#include <fstream>
#include <memory>
#include <utility>
#include <vector>

#include "envoy/common/exception.h"
#include "envoy/server/wasm.h"

#include "common/common/assert.h"
#include "common/common/logger.h"

#include "extensions/common/wasm/wasm.h"
#include "extensions/common/wasm/well_known_names.h"

#include "WAVM/IR/Module.h"
#include "WAVM/IR/Operators.h"
#include "WAVM/IR/Types.h"
#include "WAVM/IR/Validate.h"
#include "WAVM/IR/Value.h"
#include "WAVM/Inline/Assert.h"
#include "WAVM/Inline/BasicTypes.h"
#include "WAVM/Inline/Errors.h"
#include "WAVM/Inline/Hash.h"
#include "WAVM/Inline/HashMap.h"
#include "WAVM/Inline/IndexMap.h"
#include "WAVM/Inline/IntrusiveSharedPtr.h"
#include "WAVM/Inline/Lock.h"
#include "WAVM/Platform/Mutex.h"
#include "WAVM/Platform/Thread.h"
#include "WAVM/Runtime/Intrinsics.h"
#include "WAVM/Runtime/Runtime.h"
#include "WAVM/Runtime/RuntimeData.h"
#include "WAVM/Runtime/Linker.h"
#include "WAVM/Emscripten/Emscripten.h"
#include "WAVM/WASM/WASM.h"
#include "WAVM/WASTParse/WASTParse.h"

using namespace WAVM;
using namespace WAVM::IR;
using namespace WAVM::Runtime;

DECLARE_INTRINSIC_MODULE(env);

thread_local Envoy::Extensions::Common::Wasm::Context* current_context_ = nullptr;

namespace Envoy {
namespace Extensions {
namespace Common {
namespace Wasm {
namespace Wavm {
namespace {

using Context = Wasm::Context;  // Shadowing WAVM::Runtime::Context.

const Logger::Id wasmId = Logger::Id::wasm;

struct SaveRestoreContext {
  explicit SaveRestoreContext(Context *context) { saved_context = current_context_; current_context_ = context; }
  ~SaveRestoreContext() { current_context_ = saved_context; }
   Context* saved_context; 
};

#define CALL_WITH_CONTEXT(_x, _context) do { \
  SaveRestoreContext _saved_context(static_cast<Context*>(_context)); \
  _x; \
} while (0)

#define CALL_WITH_CONTEXT_RETURN(_x, _context, _type, _member) do { \
  SaveRestoreContext _saved_context(static_cast<Context*>(_context)); \
  return static_cast<_type>(_x[0]._member); \
} while (0)

std::string readFile(absl::string_view filename) {
  const std::string fn(filename);
  std::ifstream file(fn);
  if (file.fail()) return "";
  std::stringstream file_string_stream;
  file_string_stream << file.rdbuf();
  return file_string_stream.str();
}

class RootResolver : public Resolver, public Logger::Loggable<wasmId> {
  public:
    RootResolver(Compartment* compartment) : compartment_(compartment) {}
    virtual ~RootResolver() {
      moduleNameToInstanceMap_.clear();  
    }

    bool resolve(const std::string& moduleName, const std::string& exportName, ExternType type,
        Object*& outObject) override {
      auto namedInstance = moduleNameToInstanceMap_.get(moduleName);
      if(namedInstance) {
        outObject = getInstanceExport(*namedInstance, exportName);
        // If we were looking in 'env' and missed look in 'envoy'.
        if (!outObject && moduleName == "env") {
          auto envoyInstance = moduleNameToInstanceMap_.get("envoy");
          if (namedInstance) {
            outObject = getInstanceExport(*envoyInstance, exportName);
          }
        }
        if (outObject) {
          if (isA(outObject, type)) {
            return true;
          } else {
            ENVOY_LOG(warn, "Resolved import {}.{} to a {}, but was expecting {}",
                moduleName, exportName, asString(getObjectType(outObject)), asString(type));
            return false;
          }
        }
      }

      ENVOY_LOG(error, "Generated stub for missing import {}.{} : {}",
          moduleName, exportName, asString(type));
      outObject = getStubObject(exportName, type);
      return true;
    }

    Object* getStubObject(const std::string& exportName, ExternType type) const {
      switch (type.kind) {
        case IR::ExternKind::function:
          {
            // Generate a function body that just uses the unreachable op to fault if called.
            Serialization::ArrayOutputStream codeStream;
            OperatorEncoderStream encoder(codeStream);
            encoder.unreachable();
            encoder.end();

            // Generate a module for the stub function.
            IR::Module stubIRModule;
            DisassemblyNames stubModuleNames;
            stubIRModule.types.push_back(asFunctionType(type));
            stubIRModule.functions.defs.push_back({{0}, {}, std::move(codeStream.getBytes()), {}});
            stubIRModule.exports.push_back({"importStub", IR::ExternKind::function, 0});
            stubModuleNames.functions.push_back({"importStub: " + exportName, {}, {}});
            IR::setDisassemblyNames(stubIRModule, stubModuleNames);
            IR::validatePreCodeSections(stubIRModule);
            DeferredCodeValidationState deferredCodeValidationState;
            IR::validatePostCodeSections(stubIRModule, deferredCodeValidationState);

            // Instantiate the module and return the stub function instance.
            auto stubModule = compileModule(stubIRModule);
            auto stubModuleInstance = instantiateModule(compartment_, stubModule, {}, "importStub");
            return getInstanceExport(stubModuleInstance, "importStub");
          }
        case IR::ExternKind::memory:
          return asObject(
              Runtime::createMemory(compartment_, asMemoryType(type), std::string(exportName)));
        case IR::ExternKind::table:
          return asObject(
              Runtime::createTable(compartment_, asTableType(type), std::string(exportName)));
        case IR::ExternKind::global:
          return asObject(Runtime::createGlobal(
                compartment_,
                asGlobalType(type),
                IR::Value(asGlobalType(type).valueType, IR::UntaggedValue())));
        case IR::ExternKind::exceptionType:
          return asObject(
              Runtime::createExceptionType(compartment_, asExceptionType(type), "importStub"));
        default: Errors::unreachable();
      };
    }

    HashMap<std::string, ModuleInstance*> & moduleNameToInstanceMap() {
      return moduleNameToInstanceMap_;
    }

  private:
    Compartment* const compartment_;
    HashMap<std::string, ModuleInstance*> moduleNameToInstanceMap_;
};

bool loadModule(absl::string_view filename, IR::Module& outModule) {
  auto bytes = readFile(filename);
  if (bytes.empty()) return false;

  // If the file starts with the WASM binary magic number, load it as a binary irModule.
  static const uint8_t wasmMagicNumber[4] = {0x00, 0x61, 0x73, 0x6d};
  if(bytes.size() >= 4 && !memcmp(bytes.c_str(), wasmMagicNumber, 4)) {
    return WASM::loadBinaryModule(bytes.c_str(), bytes.size(), outModule);
  } else {
    // Load it as a text irModule.
    std::vector<WAST::Error> parseErrors;
    if(!WAST::parseModule(bytes.c_str(), bytes.size(), outModule, parseErrors)) {
      return false;
    }
    return true;
  }
}

}  // namespace

struct EnvoyHandlerBase  {
  virtual ~EnvoyHandlerBase() {}
};

template<typename F>
struct EnvoyHandler : EnvoyHandlerBase {
  ~EnvoyHandler() override {}
  explicit EnvoyHandler(F ahandler) : handler(ahandler) {}
  F handler;
};

template<typename F>
EnvoyHandlerBase* MakeEnvoyHandler(F handler) {
  return new EnvoyHandler<F>(handler);
}

class Wavm : public WasmVm {
  public:
    Wavm() = default;
    ~Wavm() override;

    // WasmVm
    absl::string_view vm() override { return Wasm::WasmVmNames::get().Wavm; }
    bool initialize(absl::string_view file, bool allow_precompiled) override;
    void start(Context *context) override;
    void* allocMemory(uint32_t size, uint32_t *pointer) override;
    absl::string_view getMemory(uint32_t pointer, uint32_t size) override;

    Memory* memory() { return memory_; }
    Runtime::Context* context() { return context_; }
    ModuleInstance* moduleInstance() { return moduleInstance_; }
    ModuleInstance* envoyModuleInstance() { return moduleInstance_; }

    void GetFunctions();
    void RegisterCallbacks();

#if 0
    uint32_t next_id_ = 1;   // 0 is reserved for the default_context_.
#endif
    std::unordered_map<uint32_t, Context*> context_map_;

    bool hasInstantiatedModule_ = false;
    IR::Module irModule_;
    GCPointer<ModuleInstance> moduleInstance_;
    Memory* memory_;
    Emscripten::Instance* emscriptenInstance_ = nullptr;
    GCPointer<Compartment> compartment_;
    GCPointer<Runtime::Context> context_;
    Intrinsics::Module envoy_module_;
    GCPointer<Runtime::ModuleInstance> envoyModuleInstance_ = nullptr;
    std::vector<std::unique_ptr<Intrinsics::Function>> envoy_functions_;
    std::vector<std::unique_ptr<EnvoyHandlerBase>> envoy_handlers_;
    std::vector<WAST::Error> errors_;
};

Wavm::~Wavm() {
  moduleInstance_ = nullptr;
  if (emscriptenInstance_) {
    emscriptenInstance_->env = nullptr;
    emscriptenInstance_->asm2wasm = nullptr;
    emscriptenInstance_->global = nullptr;
    emscriptenInstance_->emscriptenMemory = nullptr;
    delete emscriptenInstance_;
  }
  context_ = nullptr;
  envoyModuleInstance_ = nullptr;
  envoy_functions_.clear();
  ASSERT(tryCollectCompartment(std::move(compartment_)));
}

bool Wavm::initialize(absl::string_view wasm_file, bool allow_precompiled) {
  ASSERT(!hasInstantiatedModule_);
  hasInstantiatedModule_ = true;
  compartment_ = Runtime::createCompartment();
  context_ = Runtime::createContext(compartment_);
  if (!loadModule(wasm_file, irModule_)) {
    return false;
  }
  Runtime::ModuleRef module = nullptr;
  // todo check percompiled section is permitted
  const UserSection* precompiledObjectSection = nullptr;
  if (allow_precompiled) {
    for (const UserSection& userSection : irModule_.userSections) {
      if (userSection.name == "wavm.precompiled_object") {
        precompiledObjectSection = &userSection;
        break;
      }
    }
  }
  if(!precompiledObjectSection) {
    module = Runtime::compileModule(irModule_);
  } else {
    module = Runtime::loadPrecompiledModule(irModule_, precompiledObjectSection->data);
  }
  envoyModuleInstance_ = Intrinsics::instantiateModule(compartment_, envoy_module_, "envoy");
  RootResolver rootResolver(compartment_);
  // todo make this optional
  if (true) {
    emscriptenInstance_ = Emscripten::instantiate(compartment_, irModule_);
    rootResolver.moduleNameToInstanceMap().set("env", emscriptenInstance_->env);
    rootResolver.moduleNameToInstanceMap().set("asm2wasm", emscriptenInstance_->asm2wasm);
    rootResolver.moduleNameToInstanceMap().set("global", emscriptenInstance_->global);
  }
  rootResolver.moduleNameToInstanceMap().set("envoy", envoyModuleInstance_);
  LinkResult linkResult = linkModule(irModule_, rootResolver);
  moduleInstance_ = instantiateModule(compartment_, module, std::move(linkResult.resolvedImports), std::string(wasm_file));
  memory_ = getDefaultMemory(moduleInstance_);
  return true;
}

void Wavm::start(Context *context) {
  auto f = getStartFunction(moduleInstance_);
  if (!f) f = asFunctionNullable(getInstanceExport(moduleInstance_, "_main"));
  if (f) {
    CALL_WITH_CONTEXT(invokeFunctionChecked(context_, f, {}), context);
  }
}

void *Wavm::allocMemory(uint32_t size, uint32_t *address) {
  auto f = asFunctionNullable(getInstanceExport(moduleInstance(), "_malloc"));
  if (!f) return nullptr;
  auto values = invokeFunctionChecked(context_, f, {size});
  ASSERT(values.values.size() == 1);
  auto &v = values[0];
  ASSERT(v.type == ValueType::i32);
  *address = v.u32;
  return reinterpret_cast<char*>(memoryArrayPtr<U8>(memory(), v.u32, static_cast<U64>(size)));
}

absl::string_view Wavm::getMemory(uint32_t pointer, uint32_t size) {
   return {reinterpret_cast<char*>(memoryArrayPtr<U8>(memory(), pointer, static_cast<U64>(size))),
     static_cast<size_t>(size) };
}

std::unique_ptr<WasmVm> createWavm() {
  return std::make_unique<Wavm>();
}

} // namespace Wavm

<<<<<<< HEAD
DEFINE_INTRINSIC_FUNCTION(env, "_wasmLog", void, _wasmLog, U32 logLevel, U32 logMessage, U32 messageSize) {
  UNREFERENCED_PARAMETER(contextRuntimeData);
  current_context_->wasmLogHandler(logLevel, logMessage, messageSize);
=======
template<typename R, typename... Args>
IR::FunctionType inferEnvoyFunctionType(R (*)(void*, Args...)) {
  return IR::FunctionType(IR::inferResultType<R>(),
      IR::TypeTuple({IR::inferValueType<Args>()...}));
>>>>>>> 388144251... Callbacks working.
}

using namespace Wavm;

template<typename R, typename ...Args>
void registerCallbackWavm(WasmVm* vm, absl::string_view functionName, R (*f)(Args...)) {
  Wavm::Wavm *wavm = static_cast<Wavm::Wavm*>(vm);
  wavm->envoy_functions_.emplace_back(
      new Intrinsics::Function(wavm->envoy_module_, functionName.data(),
        reinterpret_cast<void*>(f), inferEnvoyFunctionType(f),
      IR::CallingConvention::intrinsic));
}

template void registerCallbackWavm<void, void*, U32>(WasmVm* vm, absl::string_view functionName, void(*f)(void*, U32));
template void registerCallbackWavm<void, void*, U32, U32>(WasmVm* vm, absl::string_view functionName, void(*f)(void*, U32, U32));
template void registerCallbackWavm<void, void*, U32, U32, U32>(WasmVm* vm, absl::string_view functionName, void(*f)(void*, U32, U32, U32));
template void registerCallbackWavm<void, void*, U32, U32, U32, U32>(WasmVm* vm, absl::string_view functionName, void(*f)(void*, U32, U32, U32, U32));
template void registerCallbackWavm<void, void*, U32, U32, U32, U32, U32>(WasmVm* vm, absl::string_view functionName, void(*f)(void*, U32, U32, U32, U32, U32));

template<typename R, typename ...Args>
void getFunctionWavmReturn(WasmVm* vm, absl::string_view functionName, std::function<R(Wasm::Context*, Args...)> *function, uint32_t) {
  Wavm::Wavm *wavm = static_cast<Wavm::Wavm*>(vm);
  auto f = asFunctionNullable(getInstanceExport(wavm->moduleInstance(), std::string(functionName)));
  if (!f)
    f = asFunctionNullable(getInstanceExport(wavm->envoyModuleInstance(), std::string(functionName)));
  if (!f) {
    *function = nullptr;
    return;
  }
  *function = [wavm, f](Context* context, Args... args) -> R {
    CALL_WITH_CONTEXT_RETURN(invokeFunctionChecked(wavm->context(), f, {args...}), context, uint32_t, i32);
  };
}

template<typename R, typename ...Args>
void getFunctionWavmReturn(WasmVm* vm, absl::string_view functionName, std::function<R(Wasm::Context*, Args...)> *function, bool) {
  Wavm::Wavm *wavm = static_cast<Wavm::Wavm*>(vm);
  auto f = asFunctionNullable(getInstanceExport(wavm->moduleInstance(), std::string(functionName)));
  if (!f)
    f = asFunctionNullable(getInstanceExport(wavm->envoyModuleInstance(), std::string(functionName)));
  if (!f) {
    *function = nullptr;
    return;
  }
  *function = [wavm, f](Context* context, Args... args) -> R {
    CALL_WITH_CONTEXT(invokeFunctionChecked(wavm->context(), f, {args...}), context);
  };
}

template<typename R, typename ...Args>
void getFunctionWavm(WasmVm* vm, absl::string_view functionName, std::function<R(Wasm::Context*, Args...)> *function) {
  typename std::conditional<std::is_void<R>::value, bool, uint32_t>::type x{};
  getFunctionWavmReturn(vm, functionName, function, x);
}

template void getFunctionWavm<void>(WasmVm*, absl::string_view, std::function<void(Wasm::Context*)>*);
template void getFunctionWavm<void, uint32_t>(WasmVm*, absl::string_view, std::function<void(Wasm::Context*, uint32_t)>*);
template void getFunctionWavm<void, uint32_t, uint32_t>(WasmVm*, absl::string_view, std::function<void(Wasm::Context*, uint32_t, uint32_t)>*);
template void getFunctionWavm<void, uint32_t, uint32_t, uint32_t>(WasmVm*, absl::string_view, std::function<void(Wasm::Context*, uint32_t, uint32_t, uint32_t)>*);
template void getFunctionWavm<void, uint32_t, uint32_t, uint32_t, uint32_t>(WasmVm*, absl::string_view, std::function<void(Wasm::Context*, uint32_t, uint32_t, uint32_t, uint32_t)>*);
template void getFunctionWavm<void, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>(WasmVm*, absl::string_view, std::function<void(Wasm::Context*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)>*);

void Context::wasmLogHandler(void* raw_context, uint32_t level, uint32_t address, uint32_t size) {
  auto context = WASM_CONTEXT(raw_context, Context);
  context->scriptLog(static_cast<spdlog::level::level_enum>(level),
      context->wasm_vm->getMemory(address, size));
}
  
void Context::scriptLog(spdlog::level::level_enum level, absl::string_view message) {
  switch (level) {
  case spdlog::level::trace:
    ENVOY_LOG(trace, "wasm log: {}", message);
    return;
  case spdlog::level::debug:
    ENVOY_LOG(debug, "wasm log: {}", message);
    return;
  case spdlog::level::info:
    ENVOY_LOG(info, "wasm log: {}", message);
    return;
  case spdlog::level::warn:
    ENVOY_LOG(warn, "wasm log: {}", message);
    return;
  case spdlog::level::err:
    ENVOY_LOG(error, "wasm log: {}", message);
    return;
  case spdlog::level::critical:
    ENVOY_LOG(critical, "wasm log: {}", message);
    return;
  case spdlog::level::off:
    NOT_IMPLEMENTED_GCOVR_EXCL_LINE;
  }
}

} // namespace Wasm
} // namespace Common
} // namespace Extensions
} // namespace Envoy