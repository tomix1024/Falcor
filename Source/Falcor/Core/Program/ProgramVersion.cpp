/***************************************************************************
 # Copyright (c) 2015-21, NVIDIA CORPORATION. All rights reserved.
 #
 # Redistribution and use in source and binary forms, with or without
 # modification, are permitted provided that the following conditions
 # are met:
 #  * Redistributions of source code must retain the above copyright
 #    notice, this list of conditions and the following disclaimer.
 #  * Redistributions in binary form must reproduce the above copyright
 #    notice, this list of conditions and the following disclaimer in the
 #    documentation and/or other materials provided with the distribution.
 #  * Neither the name of NVIDIA CORPORATION nor the names of its
 #    contributors may be used to endorse or promote products derived
 #    from this software without specific prior written permission.
 #
 # THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS "AS IS" AND ANY
 # EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 # IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 # PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 # CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 # EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 # PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 # PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 # OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 # (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **************************************************************************/
#include "stdafx.h"
#include "ProgramVersion.h"

#include <slang/slang.h>

namespace Falcor
{

    //
    // EntryPointGroupKernels
    //

    EntryPointGroupKernels::SharedPtr EntryPointGroupKernels::create(
        EntryPointGroupKernels::Type type,
        const EntryPointGroupKernels::Shaders& shaders)
    {
        return SharedPtr(new EntryPointGroupKernels(type, shaders));
    }

    EntryPointGroupKernels::EntryPointGroupKernels(Type type, const Shaders& shaders)
        : mType(type)
        , mShaders(shaders)
    {}

    const Shader* EntryPointGroupKernels::getShader(ShaderType type) const
    {
        for( auto& pShader : mShaders )
        {
            if(pShader->getType() == type)
                return pShader.get();
        }
        return nullptr;
    }

    RtEntryPointGroupKernels::SharedPtr RtEntryPointGroupKernels::create(
            Type type,
            const Shaders& shaders,
            std::string const& exportName,
            uint32_t maxPayloadSize,
            uint32_t maxAttributeSize)
    {
        return SharedPtr(new RtEntryPointGroupKernels(type, shaders, exportName, maxPayloadSize, maxAttributeSize));
    }

    RtEntryPointGroupKernels::RtEntryPointGroupKernels(
        Type type,
        const Shaders& shaders,
        std::string const& exportName,
        uint32_t maxPayloadSize,
        uint32_t maxAttributeSize)
        : EntryPointGroupKernels(type, shaders)
        , mExportName(exportName)
        , mMaxPayloadSize(maxPayloadSize)
        , mMaxAttributesSize(maxAttributeSize)
    {}

    //
    // ProgramKernels
    //

    ProgramKernels::ProgramKernels(
        const ProgramVersion* pVersion,
        slang::IComponentType* pSpecializedSlangProgram,
        const ProgramReflection::SharedPtr& pReflector,
        const ProgramKernels::UniqueEntryPointGroups& uniqueEntryPointGroups,
        const std::string& name)
        : mName(name)
        , mpReflector(pReflector)
        , mpVersion(pVersion)
        , mUniqueEntryPointGroups(uniqueEntryPointGroups)
    {
#ifdef FALCOR_D3D12
        mpRootSignature = D3D12RootSignature::create(pReflector.get());
#endif
    }

    ProgramKernels::SharedPtr ProgramKernels::create(
        const ProgramVersion* pVersion,
        slang::IComponentType* pSpecializedSlangProgram,
        const ProgramReflection::SharedPtr& pReflector,
        const ProgramKernels::UniqueEntryPointGroups& uniqueEntryPointGroups,
        std::string& log,
        const std::string& name)
    {
        SharedPtr pProgram = SharedPtr(new ProgramKernels(pVersion, pSpecializedSlangProgram, pReflector, uniqueEntryPointGroups, name));
#ifdef FALCOR_GFX
        gfx::IShaderProgram::Desc programDesc = {};
        programDesc.slangProgram = pSpecializedSlangProgram;
        Slang::ComPtr<ISlangBlob> diagnostics;
        if (SLANG_FAILED(gpDevice->getApiHandle()->createProgram(programDesc, pProgram->mApiHandle.writeRef(), diagnostics.writeRef())))
        {
            pProgram = nullptr;
        }
        if (diagnostics)
        {
            log = (const char*)diagnostics->getBufferPointer();
        }
#endif
        return pProgram;
    }

    ProgramVersion::SharedConstPtr ProgramKernels::getProgramVersion() const
    {
        return mpVersion->shared_from_this();
    }

    const Shader* ProgramKernels::getShader(ShaderType type) const
    {
        for( auto& pEntryPointGroup : mUniqueEntryPointGroups )
        {
            if(auto pShader = pEntryPointGroup->getShader(type))
                return pShader;
        }
        return nullptr;
    }


    ProgramVersion::ProgramVersion(Program* pProgram, slang::IComponentType* pSlangGlobalScope)
        : mpProgram(pProgram->shared_from_this())
        , mpSlangGlobalScope(pSlangGlobalScope)
    {
        FALCOR_ASSERT(pProgram);
    }

    void ProgramVersion::init(
        const DefineList&                                   defineList,
        const TypeConformanceList&                          typeConformanceList,
        const ProgramReflection::SharedPtr&                 pReflector,
        const std::string&                                  name,
        std::vector<ComPtr<slang::IComponentType>> const&   pSlangEntryPoints)
    {
        FALCOR_ASSERT(pReflector);
        mDefines = defineList;
        mTypeConformances = typeConformanceList;
        mpReflector = pReflector;
        mName = name;
        mpSlangEntryPoints = pSlangEntryPoints;
    }

    ProgramVersion::SharedPtr ProgramVersion::createEmpty(Program* pProgram, slang::IComponentType* pSlangGlobalScope)
    {
        return SharedPtr(new ProgramVersion(pProgram, pSlangGlobalScope));
    }

    ProgramKernels::SharedConstPtr ProgramVersion::getKernels(ProgramVars const* pVars) const
    {
        // We need are going to look up or create specialized kernels
        // based on how parameters are bound in `pVars`.
        //
        // To do this we need to identify those parameters that are relevant
        // to specialization, and what argument type/value is bound to
        // those parameters.
        //
        std::string specializationKey;

        ParameterBlock::SpecializationArgs specializationArgs;
        if (pVars)
        {
            pVars->collectSpecializationArgs(specializationArgs);
        }

        bool first = true;
        for( auto specializationArg : specializationArgs )
        {
            if(!first) specializationKey += ",";
            specializationKey += std::string(specializationArg.type->getName());
            first = false;
        }

        auto foundKernels = mpKernels.find(specializationKey);
        if( foundKernels != mpKernels.end() )
        {
            return foundKernels->second;
        }

        // Loop so that user can trigger recompilation on error
        for(;;)
        {
            std::string log;
            auto pKernels = mpProgram->preprocessAndCreateProgramKernels(this, pVars, log);
            if( pKernels )
            {
                // Success

                if (!log.empty())
                {
                    std::string warn = "Warnings in program:\n" + getName() + "\n" + log;
                    logWarning(warn);
                }

                mpKernels[specializationKey] = pKernels;
                return pKernels;
            }
            else
            {
                // Failure

                std::string error = "Failed to link program:\n" + getName() + "\n\n" + log;
                reportErrorAndAllowRetry(error);

                // Continue loop to keep trying...
            }
        }
    }

    slang::ISession* ProgramVersion::getSlangSession() const
    {
        return getSlangGlobalScope()->getSession();
    }

    slang::IComponentType* ProgramVersion::getSlangGlobalScope() const
    {
        return mpSlangGlobalScope;
    }

    slang::IComponentType* ProgramVersion::getSlangEntryPoint(uint32_t index) const
    {
        return mpSlangEntryPoints[index];
    }
}
