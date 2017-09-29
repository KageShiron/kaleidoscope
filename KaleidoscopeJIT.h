//
// Created by 白倉大河 on 2017/09/29.
//

#ifndef KALEIDOSCOPE_KALEIDOSCOPEJIT_H
#define KALEIDOSCOPE_KALEIDOSCOPEJIT_H

#endif //KALEIDOSCOPE_KALEIDOSCOPEJIT_H
#ifndef LLVM_EXECUTIONENGINE_ORC_KALEIDOSCOPEJIT_H
#define LLVM_EXECUTIONENGINE_ORC_KALEIDOSCOPEJIT_H

#include "llvm/ADT/iterator_range.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/RTDyldMemoryManager.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/ExecutionEngine/Orc/CompileUtils.h"
#include "llvm/ExecutionEngine/Orc/IRCompileLayer.h"
#include "llvm/ExecutionEngine/Orc/LambdaResolver.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Mangler.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace llvm {
namespace orc {
    class KaleidoscopeJIT{
    public:
        using ObjLayerT = RTDyldObjectLinkingLayer;
        using CompilerLayerT = IRCompileLayer<ObjLayerT, SimpleCompiler>;
        using ModuleHandleT = CompilerLayerT::ModuleHandleT;

        KaleidoscopeJIT()
                : TM(EngineBuilder().selectTarget()) , DL(TM->createDataLayout()),
                  ObjectLayer([](){return std::make_shared<SectionMemoryManager>();}),
                  CompilerLayer(ObjectLayer,SimpleCompiler(*TM))
        {
            llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr);
        }

        TargetMachine &getTargetMachine() { return *TM; }

        ModuleHandleT addModule(std::unique_ptr<Module> M){
            auto Resolver = createLambdaResolver(
                    [&](const std::string &Name)
                    {
                        if (auto Sym = findMangledSymbol(Name)) {
                            return Sym;
                        }
                        return JITSymbol(nullptr);
                    },[](const std::string &S){return nullptr;}
            );

            auto H = cantFail(CompilerLayer.addModule(std::move(M), std::move(Resolver)));
            ModuleHandles.push_back(H);
            return H;
        }

        void removeModule(ModuleHandleT H){
            ModuleHandles.erase(find(ModuleHandles, H));
            cantFail(CompilerLayer.removeModule(H));
        }

        JITSymbol findSymbol(const std::string Name){
            return findMangledSymbol(mangle(Name));
        }
    private:

        std::string mangle(const std::string &Name){
            std::string MangledName;
            {
                raw_string_ostream MangledNameStream(MangledName);
                Mangler::getNameWithPrefix(MangledNameStream, Name, DL);
            }
            return MangledName;
        }

        JITSymbol findMangledSymbol(const std::string &Name) {
#ifdef LLVM_ON_WIN32
            const bool ExportedSymbolsOnly = fals;e
#else
            const bool ExportedSymbolsOnly = true;
#endif

            for (auto H : make_range(ModuleHandles.rbegin(), ModuleHandles.rend()))
                if (auto Sym = CompilerLayer.findSymbolIn(H, Name, ExportedSymbolsOnly))
                    return Sym;

            if(auto SymAddr = RTDyldMemoryManager::getSymbolAddressInProcess(Name))
                return JITSymbol(SymAddr, JITSymbolFlags::Exported);

#ifdef LLVM_ON_WIN32
            if(Name.length() > 2 && Name[0] == '_')
            if(auto SymAddr = RTDyldMemoryManager::getSymbolAddressInProcess(Name.substr(1)))
                return JITSymbol(SymAddr,JITSymbolFlags::Exported);
#endif
            return nullptr;
        }

        std::unique_ptr<TargetMachine> TM;
        const DataLayout DL;
        ObjLayerT ObjectLayer;
        CompilerLayerT CompilerLayer;
        std::vector<ModuleHandleT> ModuleHandles;
    };


}
}

#endif
