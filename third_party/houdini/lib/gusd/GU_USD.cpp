//
// Copyright 2017 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.
//
#include "gusd/GU_USD.h"
#include "gusd/GU_PackedUSD.h"

#include "gusd/USD_Utils.h"
#include "gusd/UT_Assert.h"
#include "gusd/UT_Error.h"
#include "gusd/UT_Usd.h"

#include <GA/GA_AIFSharedStringTuple.h>
#include <GA/GA_AIFCopyData.h>
#include <GA/GA_AIFTuple.h>
#include <GA/GA_ATIGroupBool.h>
#include <GA/GA_AttributeFilter.h>
#include <GA/GA_AttributeInstanceMatrix.h>
#include <GA/GA_AttributeTransformer.h>
#include <GA/GA_Handle.h>
#include <GA/GA_Iterator.h>
#include <GA/GA_Names.h>
#include <GA/GA_SplittableRange.h>
#include <GU/GU_Detail.h>
#include <GU/GU_PrimPacked.h>
#include <UT/UT_Interrupt.h>
#include <UT/UT_ParallelUtil.h>

PXR_NAMESPACE_OPEN_SCOPE

namespace {


void _LogBindError(GusdUT_ErrorContext& err, const char* attr)
{
    UT_WorkBuffer buf;
    buf.sprintf("Attribute '%s' is missing or the wrong type", attr);
    err.AddError(buf.buffer());
}


void _LogCreateError(GusdUT_ErrorContext& err, const char* attr)
{
    UT_WorkBuffer buf;
    buf.sprintf("Failed creating '%s' attribute", attr);
    err.AddError(buf.buffer());
}


template <typename Handle>
bool _AttrBindSuccess(Handle& handle, const char* name,
                      GusdUT_ErrorContext* err)
{
    if(handle.isValid())
        return true;
    if(err) _LogBindError(*err, name);
    return false;
}


template <typename Handle>
bool _AttrCreateSuccess(Handle& handle, const char* name,
                        GusdUT_ErrorContext* err)
{
    if(handle.isValid())
        return true;
    if(err) _LogCreateError(*err, name);
    return false;
}


} /*namespace*/


bool
GusdGU_USD::OffsetArrayFromRange(const GA_Range& r,
                                 GA_OffsetArray& offsets)
{
    offsets.setSize(r.getEntries());
    exint i = 0;
    for(GA_Iterator it(r); !it.atEnd(); ++it, ++i)
        offsets(i) = *it;
    return true;
}


bool
GusdGU_USD::ComputeRangeIndexMap(const GA_Range& r,
                                 GA_OffsetArray& indexMap)
{
    if(!r.getRTI())
        return false;

    const GA_IndexMap& attrIndexMap = r.getRTI()->getIndexMap();
    indexMap.setSize(attrIndexMap.offsetSize());

    exint i = 0;
    for(GA_Iterator it(r); !it.atEnd(); ++it, ++i)
        indexMap(*it) = i;
    return true;
}


GusdGU_USD::BindOptions::BindOptions()
    : pathAttr(GUSD_PATH_ATTR),
      primPathAttr(GUSD_PRIMPATH_ATTR),
      variantsAttr(GUSD_VARIANTS_ATTR),
      frameAttr(GUSD_FRAME_ATTR),
      packedPrims(true)
{}


namespace {


template <typename T, typename StringToObjFn>
struct _ObjectsFromSharedStringTupleT
{
    _ObjectsFromSharedStringTupleT(const GA_Attribute& attr,
                                   const GA_AIFSharedStringTuple& tuple,
                                   const StringToObjFn& stringToObjFn,
                                   UT_Array<T>& vals,
                                   GusdUT_ErrorContext* err,
                                   std::atomic_bool& workerInterrupt)
        : _attr(attr), _tuple(tuple), _stringToObjFn(stringToObjFn),
          _vals(vals), _err(err), _workerInterrupt(workerInterrupt) {}

    void    operator()(const UT_BlockedRange<size_t>& r) const
            {
                auto* boss = UTgetInterrupt();
                char bcnt = 0;

                for(size_t i = r.begin(); i < r.end(); ++i) {
                    // Exit early either via user interrupt or
                    // by another worker thread.
                    if(BOOST_UNLIKELY(!++bcnt && (boss->opInterrupt() ||
                                                  _workerInterrupt)))
                        return;

                    UT_StringRef str(_tuple.getTableString(&_attr, i));
                    if(str.isstring()) {
                        T val;
                        if(_stringToObjFn(str, val, _err)) {
                            _vals(i) = val;
                        } else if(!_err || (*_err)() >= UT_ERROR_ABORT) {
                            // Interrupt the other worker threads.
                            _workerInterrupt = true;
                            return;
                        }
                    }
                }
            }
private:
    const GA_Attribute&             _attr;
    const GA_AIFSharedStringTuple&  _tuple;
    const StringToObjFn&            _stringToObjFn;
    UT_Array<T>&                    _vals;
    GusdUT_ErrorContext*            _err;
    std::atomic_bool&               _workerInterrupt;
};



template <typename T, typename StringToObjFn>
bool
_GetObjsFromStringAttrT(const GA_Attribute& attr,
                        const StringToObjFn& stringToObjFn,
                        UT_Array<T>& vals,
                        GusdUT_ErrorContext* err)
{
    const GA_AIFSharedStringTuple* tuple = attr.getAIFSharedStringTuple();
    if(!tuple)
        return false;

    GA_Size count = tuple->getTableEntries(&attr);
    vals.setSize(count);

    std::atomic_bool workerInterrupt(false);
    if(stringToObjFn.lightItems) {
        UTparallelForLightItems(
            UT_BlockedRange<size_t>(0, count),
             _ObjectsFromSharedStringTupleT<T,StringToObjFn>(
                 attr, *tuple, stringToObjFn,
                 vals, err, workerInterrupt));
    } else {
        UTparallelFor(
            UT_BlockedRange<size_t>(0, count),
             _ObjectsFromSharedStringTupleT<T,StringToObjFn>(
                 attr, *tuple, stringToObjFn,
                 vals, err, workerInterrupt));
    }
    return (!UTgetInterrupt()->opInterrupt() && !workerInterrupt);
}



template <typename T, typename StringToObjFn>
bool
_GetObjsFromStringAttrT(const GA_Attribute& attr,
                        const GA_Range& rng,
                        const StringToObjFn& stringToObjFn,
                        UT_Array<T>& vals,
                        GusdUT_ErrorContext* err)
{
    GA_ROHandleS hnd(&attr);
    if(hnd.isInvalid())
        return false;

    UT_Array<T> tableVals;
    if(!_GetObjsFromStringAttrT<T,StringToObjFn>(attr, stringToObjFn,
                                                 tableVals, err))
        return false;
           
    vals.clear();
    vals.setSize(rng.getEntries());

    auto* boss = UTgetInterrupt();

    exint i = 0;
    char bcnt = 0;
    for(GA_Iterator it(rng); !it.atEnd(); ++it, ++i) {
        if(BOOST_UNLIKELY(!++bcnt && boss->opInterrupt()))
            return false;
        GA_StringIndexType idx = hnd.getIndex(*it);
        if(idx != GA_INVALID_STRING_INDEX)
            vals(i) = tableVals(idx);
    }
    return true;
}


struct _StringToPrimPathFn
{
    static const bool lightItems = true;

    bool    operator()(const UT_StringRef& str, SdfPath& path,
                       GusdUT_ErrorContext* err) const
            {
                return GusdUT_CreateSdfPath(str, path, err);
            }
};


struct _StringToTokenFn
{
    static const bool lightItems = true;

    _StringToTokenFn(const char* nameSpace)
        {
            if(UTisstring(nameSpace)) {
                _ns = nameSpace;
                _ns += ':';
            }
        }

    bool    operator()(const UT_StringRef& str, TfToken& token,
                       GusdUT_ErrorContext* err) const
            {
                if(_ns.empty())
                    token = TfToken(str.toStdString());
                else
                    token = TfToken(_ns + str.toStdString());
                return true;
            }
private:
    std::string _ns;
};



} /*namespace */


bool
GusdGU_USD::GetPrimPathsFromStringAttr(const GA_Attribute& attr,
                                       UT_Array<SdfPath>& paths,
                                       GusdUT_ErrorContext* err)
{
    return _GetObjsFromStringAttrT<SdfPath,_StringToPrimPathFn>(
            attr, _StringToPrimPathFn(), paths, err);
}


bool
GusdGU_USD::GetPrimPathsFromStringAttr(const GA_Attribute& attr,
                                       const GA_Range& rng,
                                       UT_Array<SdfPath>& paths,
                                       GusdUT_ErrorContext* err)
{
    return _GetObjsFromStringAttrT<SdfPath,_StringToPrimPathFn>(
            attr, rng, _StringToPrimPathFn(), paths, err);
}


bool
GusdGU_USD::GetTokensFromStringAttr(const GA_Attribute& attr,
                                    UT_Array<TfToken>& tokens,
                                    const char* nameSpace,
                                    GusdUT_ErrorContext* err)
{
    return _GetObjsFromStringAttrT<TfToken,_StringToTokenFn>(
            attr, _StringToTokenFn(nameSpace), tokens, err);
}


bool
GusdGU_USD::GetTokensFromStringAttr(const GA_Attribute& attr,
                                    const GA_Range& rng,
                                    UT_Array<TfToken>& tokens,
                                    const char* nameSpace,
                                    GusdUT_ErrorContext* err)
{
    return _GetObjsFromStringAttrT<TfToken,_StringToTokenFn>(
            attr, rng, _StringToTokenFn(nameSpace), tokens, err);
}


namespace {


struct _StringPathToProxyFn
{
    _StringPathToProxyFn(GusdUSD_StageCacheContext& cache,
                         GusdUT_ErrorContext* err)
        : _cache(cache), _err(err) {}

    static const bool lightItems = true;

    bool    operator()(const UT_StringRef& str,
                       GusdUSD_StageProxyHandle& proxy,
                       GusdUT_ErrorContext* err) const
            {
                TfToken path(str.toStdString());
                if(proxy = _cache.FindOrCreateProxy(path))
                    return true;
                return false;
            }

private:
    GusdUSD_StageCacheContext&  _cache;
    GusdUT_ErrorContext*        _err;
};


struct _GetProxiesFn
{
    _GetProxiesFn(UT_Array<GusdUSD_StageProxyHandle>& proxies,
                  const UT_Array<TfToken>& paths,
                  const UT_Array<SdfPath>* variants,
                  GusdUSD_StageCacheContext& cache)
        : _proxies(proxies), _paths(paths),
          _variants(variants), _cache(cache) {}
    
    void    operator()(const UT_BlockedRange<size_t>& r) const
            {
                auto* boss = UTgetInterrupt();  
                char bcnt = 0;

                GusdUSD_StageProxyHandle proxy;
                TfToken lastPath;
                SdfPath lastVariants;

                for(size_t i = r.begin(); i < r.end(); ++i) {
                    if(BOOST_UNLIKELY(!++bcnt && boss->opInterrupt()))
                        return;

                    const auto& path = _paths(i);
                    if(!path.IsEmpty()) {
                        SdfPath variants;
                        if(_variants)
                            variants = (*_variants)(i);

                        /* Trivial proxy reuse.
                           (usually the keys repeat) */
                        if(path != lastPath || variants != lastVariants)
                        {
                            lastPath = path;
                            lastVariants = variants;

                            proxy = _cache.FindOrCreateProxy(path, variants);
                        }
                        if(proxy)
                            _proxies(i) = proxy;
                    }
                }
            }
private:
    UT_Array<GusdUSD_StageProxyHandle>& _proxies;
    const UT_Array<TfToken>&            _paths;
    const UT_Array<SdfPath>*            _variants;
    GusdUSD_StageCacheContext&          _cache;
};

} /*namespace*/


bool
GusdGU_USD::GetStageProxiesFromAttrs(
    const GA_Range& rng,
    const GA_Attribute& pathAttr,
    const GA_Attribute* variantsAttr,
    UT_Array<GusdUSD_StageProxyHandle>& proxies,
    GusdUSD_StageCacheContext& cache,
    UT_Array<SdfPath>* variants,
    GusdUT_ErrorContext* err)
{
    proxies.setSize(rng.getEntries());

    if(!variantsAttr) {
        /* The proxy only depends on the path attr, so we can
           take a shortcut using the shared string tuple to reduce lookups.*/
        _StringPathToProxyFn pathToProxyFn(cache, err);
        return _GetObjsFromStringAttrT<
        GusdUSD_StageProxyHandle,_StringPathToProxyFn>(
            pathAttr, rng, pathToProxyFn, proxies, err);
    }

    UT_Array<SdfPath> variantsTmp;
    auto& variantsArray = variants ? *variants : variantsTmp;
    if(variantsAttr &&
       !GetPrimPathsFromStringAttr(*variantsAttr, rng, variantsArray, err))
        return false;

    UT_Array<TfToken> paths;
    if(!GetTokensFromStringAttr(pathAttr, rng, paths, /*namespace*/ NULL, err))
        return false;

    UTparallelFor(UT_BlockedRange<size_t>(0, rng.getEntries()),
                  _GetProxiesFn(proxies, paths,
                                variantsAttr ? &variantsArray : NULL, cache));
    return !UTgetInterrupt()->opInterrupt();
}


bool
GusdGU_USD::PrimHandle::Bind(const BindOptions& opts,
                             const GA_Detail& gd,
                             const GA_Range& rng,
                             UT_Array<SdfPath>* variants,
                             UT_Array<GusdPurposeSet>* purposes,
                             GusdUSD_Utils::PrimTimeMap* timeMap,
                             GusdUT_ErrorContext* err)
{
    packedPrims = opts.packedPrims;
    return BindPrims(opts, gd, accessor, prims, rng,
                     cache, variants, purposes, timeMap, err);
}


bool
GusdGU_USD::BindPrims(const BindOptions& opts,
                      const GA_Detail& gd,
                      GusdUSD_StageProxy::MultiAccessor& accessor,
                      UT_Array<UsdPrim>& prims,
                      const GA_Range& rng,
                      GusdUSD_StageCacheContext& cache,
                      UT_Array<SdfPath>* variants,
                      UT_Array<GusdPurposeSet>* purposes,
                      GusdUSD_Utils::PrimTimeMap* timeMap,
                      GusdUT_ErrorContext* err)
{
    // Bind prims.
    if(opts.packedPrims) {
        if(!BindPrimsFromPackedPrims(accessor, prims, rng,
                                     cache, variants, purposes, err))
            return false;
        
        if(timeMap && !GetTimeCodesFromPackedPrims(rng, timeMap->times, err))
            return false;
    } else {
        auto owner = rng.getOwner();
        // Path and prim path are required.
        GA_ROHandleS path(&gd, owner, opts.pathAttr);
        if(!_AttrBindSuccess(path, opts.pathAttr, err))
            return false;
        GA_ROHandleS primPath(&gd, owner, opts.primPathAttr);
        if(!_AttrBindSuccess(primPath, opts.primPathAttr, err))
            return false;

        if(!BindPrimsFromAttrs(accessor, prims, rng, *path.getAttribute(),
                               *primPath.getAttribute(),
                               gd.findAttribute(owner, opts.variantsAttr),
                               cache, variants, err))
            return false;

        // XXX: No attribute schema for purpose yet.
        // Just use default purpose.
        if(purposes) {
            purposes->setSize(prims.size());
            purposes->constant(GUSD_PURPOSE_DEFAULT);
        }
        
        if(timeMap) {
            GA_ROHandleF times(&gd, owner, opts.frameAttr);
            if(times.isValid()) {
                if(!GetTimeCodesFromAttr(rng, *times.getAttribute(),
                                         timeMap->times, err))
                    return false;
            }
        }
    }
    return true;
}


bool
GusdGU_USD::BindPrimsFromAttrs(
    GusdUSD_StageProxy::MultiAccessor& accessor,
    UT_Array<UsdPrim>& prims,
    const GA_Range& rng,
    const GA_Attribute& pathAttr,
    const GA_Attribute& primPathAttr,
    const GA_Attribute* variantsAttr,
    GusdUSD_StageCacheContext& cache,
    UT_Array<SdfPath>* variants,
    GusdUT_ErrorContext* err)
{
    // Handle paths first. This might allow us to skip loading stages.

    UT_Array<SdfPath> primPaths;
    if(!GetPrimPathsFromStringAttr(primPathAttr, rng, primPaths, err))
        return false;

    UT_Array<GusdUSD_StageProxyHandle> proxies;
    if(!GetStageProxiesFromAttrs(rng, pathAttr, variantsAttr,
                                 proxies, cache, variants, err))
        return false;

    UT_ASSERT(proxies.size() == primPaths.size());

    return accessor.Bind(proxies, primPaths, prims,
                         cache.GetLoadSet(), err);
}


bool
GusdGU_USD::BindPrimsFromPackedPrims(
    GusdUSD_StageProxy::MultiAccessor& accessor,
    UT_Array<UsdPrim>& prims,
    const GA_Range& rng,
    GusdUSD_StageCacheContext& cache,
    UT_Array<SdfPath>* variants,
    UT_Array<GusdPurposeSet>* purposes,
    GusdUT_ErrorContext* err)
{
    const exint size = rng.getEntries();

    // Create arrays for proxies and primPaths.
    UT_Array<GusdUSD_StageProxyHandle> proxies(size, size);
    UT_Array<SdfPath> primPaths(size, size);

    // If variants array was provided,
    // match array size to other arrays.
    if (variants) {
        variants->setSize(size);
    }
    if( purposes ) {
        purposes->setSize(size);
    }

    // Acquire GEO_Detail from rng.
    const GEO_Detail* gdp =
        UTverify_cast<GEO_Detail*>(&rng.getRTI()->getIndexMap().getDetail());
    UT_ASSERT_P(gdp);

    exint i = 0;
    for (GA_Iterator it(rng); !it.atEnd(); ++it, ++i) {

        const GEO_Primitive* p = gdp->getGEOPrimitive(*it);
        const GU_PrimPacked* pp = dynamic_cast<const GU_PrimPacked*>(p);
        if (!pp) {
            continue;
        }

        const GusdGU_PackedUSD* prim =
            dynamic_cast<const GusdGU_PackedUSD*>(pp->implementation());
        
        if (!prim) {
            continue;
        }

        if(auto proxy = prim->getProxy())
        {
            proxies(i) = proxy;

            SdfPath primPath, variantPath;
            GusdUSD_Utils::ExtractPathComponents(prim->primPath(),
                                                 primPath, variantPath);
            primPaths(i) = primPath;
            if (variants) {
                (*variants)(i) = variantPath;
            }
        }

        if( purposes ) {
            (*purposes)(i) = prim->getPurposes();
        }
    }

    return accessor.Bind(proxies, primPaths, prims,
                         cache.GetLoadSet(), err);
}


bool
GusdGU_USD::GetTimeCodesFromAttr(const GA_Range& rng,
                                 const GA_Attribute& attr,
                                 UT_Array<UsdTimeCode>& times,
                                 GusdUT_ErrorContext* err)
{
    GA_ROHandleF hnd(&attr);
    if(hnd.isInvalid())
        return false;

    times.setSize(rng.getEntries());

    auto* boss = UTgetInterrupt();
    char bcnt = 0;
    exint idx = 0;
    for(GA_Iterator it(rng); !it.atEnd(); ++it, ++idx) {
        if(BOOST_UNLIKELY(!++bcnt && boss->opInterrupt()))
            return false;
        times(idx) = hnd.get(*it);
    }
    return true;
}


bool
GusdGU_USD::GetTimeCodesFromPackedPrims(const GA_Range& rng,
                                        UT_Array<UsdTimeCode>& times,
                                        GusdUT_ErrorContext* err)
{
    times.setSize(rng.getEntries());

    // Acquire GEO_Detail from rng.
    const GEO_Detail* gdp =
        UTverify_cast<GEO_Detail*>(&rng.getRTI()->getIndexMap().getDetail());
    UT_ASSERT_P(gdp);

    exint i = 0;
    for (GA_Iterator it(rng); !it.atEnd(); ++it, ++i) {

        const GEO_Primitive* p = gdp->getGEOPrimitive(*it);
        const GU_PrimPacked* pp = dynamic_cast<const GU_PrimPacked*>(p);
        if (!pp) {
            continue;
        }

        const GusdGU_PackedUSD* prim =
            dynamic_cast<const GusdGU_PackedUSD*>(pp->implementation());

        if (!prim) {
            continue;
        }

        times(i) = prim->frame();
    }
    return true;
}



GA_Offset
GusdGU_USD::AppendRefPoints(GU_Detail& gd,
                            const UT_Array<UsdPrim>& prims,
                            const char* pathAttrName,   
                            const char* primPathAttrName,
                            GusdUT_ErrorContext* err)
{
    auto owner = GA_ATTRIB_POINT;
    GA_RWHandleS path(gd.addStringTuple(owner, pathAttrName, 1));
    GA_RWHandleS primPath(gd.addStringTuple(owner, primPathAttrName, 1));
    if(!_AttrCreateSuccess(path, pathAttrName, err) ||
       !_AttrCreateSuccess(primPath, primPathAttrName, err))
        return GA_INVALID_OFFSET;
    
    GA_Offset start = gd.appendPointBlock(prims.size());
    GA_Offset end = start + prims.size();

    // Write in serial for now.
    auto* boss = UTgetInterrupt();
    char bcnt = 0;
    exint i = 0;

    /* Prim paths vary, but stages are often the same.
       Makes sense to try and cache lookups.*/
    auto* pathAttr = path.getAttribute();
    auto* pathTuple = GusdUTverify_ptr(pathAttr->getAIFSharedStringTuple());
    GA_AIFSharedStringTuple::StringBuffer buf(pathAttr, pathTuple);

    UsdStageWeakPtr lastStage;
    GA_StringIndexType lastStageIdx = GA_INVALID_STRING_INDEX;

    for(GA_Offset o = start; o < end; ++o, ++i) {
        if(BOOST_UNLIKELY(!++bcnt && boss->opInterrupt()))
            return GA_INVALID_OFFSET;

        if(UsdPrim prim = prims(i)) {
            UsdStageWeakPtr stage = prim.GetStage();
            if(stage != lastStage) {
                lastStage = stage;
                lastStageIdx = buf.append(
                    stage->GetRootLayer()->GetIdentifier().c_str());
            }
            path.set(o, lastStageIdx);
            primPath.set(o, prim.GetPath().GetString().c_str());
        }
    }
    return start;
}

static std::map<TfToken, GusdGU_USD::PackedPrimBuildFunc> packedPrimBuildFuncRegistry;

void
GusdGU_USD::RegisterPackedPrimBuildFunc( 
        const TfToken &typeName, 
        GusdGU_USD::PackedPrimBuildFunc func ) {

    packedPrimBuildFuncRegistry[typeName] = func;
}

bool
GusdGU_USD::AppendPackedPrims(
    GU_Detail& gd,
    const UT_Array<UsdPrim>& prims,
    const UT_Array<SdfPath>& variants,
    const GusdUSD_Utils::PrimTimeMap& timeMap,
    const UT_StringArray& viewportLOD,
    const UT_Array<GusdPurposeSet>& purposes,
    GusdUT_ErrorContext* err)
{
    UT_ASSERT(prims.size() == variants.size());

    for (exint i = 0; i < prims.size(); ++i) {
        if (UsdPrim prim = prims(i)) {

            const std::string& usdFileName =
                prim.GetStage()->GetRootLayer()->GetIdentifier();

            SdfPath usdPrimPath = prim.GetPath();

            // If variants(i) is a valid variant path, then update usdPrimPath
            // to include the variant selections from variants(i).
            if (variants(i).ContainsPrimVariantSelection()) {
                SdfPath strippedPath = variants(i).StripAllVariantSelections();
                usdPrimPath = usdPrimPath.ReplacePrefix(strippedPath, variants(i));
            }


            auto it = packedPrimBuildFuncRegistry.find( prim.GetTypeName() );
            if( it != packedPrimBuildFuncRegistry.end() ) {

                (*it->second)( gd, usdFileName, usdPrimPath,
                               timeMap(i), viewportLOD(i), purposes(i) );
            }
            else {
                GusdGU_PackedUSD::Build( gd, usdFileName, usdPrimPath,
                                         timeMap(i), viewportLOD(i),
                                         purposes(i) );
            }
        }
    }

    return true;
}


GA_Offset
GusdGU_USD::AppendExpandedRefPoints(
    GU_Detail& gd,
    const GA_Detail& srcGd,
    const GA_Range& srcRng,
    const UT_Array<PrimIndexPair>& prims,
    const GA_AttributeFilter& filter,
    const char* pathAttrName,
    const char* primPathAttrName,
    GusdUT_ErrorContext* err)
{
    // Need an array of just the prims.
    UT_Array<UsdPrim> primArray(prims.size(), prims.size());
    for(exint i = 0; i < prims.size(); ++i)
        primArray(i) = prims(i).first;

    // Add the new ref points.
    GA_Offset start = AppendRefPoints(gd, primArray, pathAttrName,
                                      primPathAttrName, err);
    if(!GAisValid(start))
        return GA_INVALID_OFFSET;

    // Find attributes to copy.
    GA_AttributeFilter filterNoRefAttrs(
        GA_AttributeFilter::selectAnd(
            GA_AttributeFilter::selectNot(
                GA_AttributeFilter::selectOr(
                    GA_AttributeFilter::selectByName(pathAttrName),
                    GA_AttributeFilter::selectByName(primPathAttrName))),
            filter));

    UT_Array<const GA_Attribute*> attrs;
    srcGd.getAttributes().matchAttributes(
        filterNoRefAttrs, srcRng.getOwner(), attrs);

    if(attrs.isEmpty())
        return start;

    /* Need to build out a source range including repeats for all
       of our expanded indices.*/
    GA_OffsetList srcOffsets;
    const GA_IndexMap& srcMap = srcGd.getIndexMap(srcRng.getOwner());
    {
        srcOffsets.setEntries(prims.size());
        GA_OffsetArray offsets;
        if(!OffsetArrayFromRange(srcRng, offsets))
            return GA_INVALID_OFFSET;
        for(exint i = 0; i < prims.size(); ++i)
            srcOffsets.set(i, offsets(prims(i).second));
     }
        
    GA_Range dstRng(gd.getPointMap(), start, start+prims.size());

    if(CopyAttributes(GA_Range(srcMap, srcOffsets),
                      dstRng, gd.getPointMap(), attrs))
        return start;
    return GA_INVALID_OFFSET;
}

bool
GusdGU_USD::AppendExpandedPackedPrims(
    GU_Detail& gd,
    const GA_Detail& srcGd,
    const GA_Range& srcRng,
    const UT_Array<PrimIndexPair>& primIndexPairs,
    const UT_Array<SdfPath>& variants,
    const GusdUSD_Utils::PrimTimeMap& timeMap,
    const GA_AttributeFilter& filter,
    bool unpackToPolygons,
    const UT_String& primvarPattern,
    GusdUT_ErrorContext* err)
{
    UT_AutoInterrupt task("Unpacking packed USD prims");

    const exint srcSize = srcRng.getEntries();
    const exint dstSize = primIndexPairs.size();

    // Need an array of just the prims.
    UT_Array<UsdPrim> prims(dstSize, dstSize);
    for (exint i = 0; i < dstSize; ++i) {
        prims(i) = primIndexPairs(i).first;
    }

    // Create an index-to-offset map from srcRng.
    GA_OffsetArray indexToOffset;
    if (!OffsetArrayFromRange(srcRng, indexToOffset)) {
        return false;
    }

    // Collect the transform and viewportLOD from each source packed prim.
    UT_Array<UT_Matrix4D> srcXforms(srcSize, srcSize);    
    ComputeTransformsFromPackedPrims(srcGd, indexToOffset,
                                     srcXforms.array(), err);
    UT_StringArray srcVpLOD;
    srcVpLOD.setSize(srcSize);
    UT_Array<GusdPurposeSet> srcPurposes;
    srcPurposes.setSize(srcSize);
    GetPackedPrimViewportLODAndPurposes(srcGd, indexToOffset, 
                                        srcVpLOD, srcPurposes, err);

    // Now remap these arrays to align with the destination packed prims.
    UT_Array<UT_Matrix4D> dstXforms(dstSize, dstSize);
    UT_StringArray dstVpLOD;
    dstVpLOD.setSize(dstSize);
    UT_Array<GusdPurposeSet> dstPurposes;
    dstPurposes.setSize(dstSize);    

    for (exint i = 0; i < dstSize; ++i) {
        dstXforms(i) = srcXforms(primIndexPairs(i).second);
        dstVpLOD(i) = srcVpLOD(primIndexPairs(i).second);
        dstPurposes(i) = srcPurposes(primIndexPairs(i).second);
    }

    // Make a GU_Detail pointer to help handle 2 cases:
    // 1. If unpacking to polygons, point to a new temporary detail so
    //    that intermediate prims don't get appended to gd.
    // 2. If NOT unpacking to polygons, point to gd so result prims do
    //    get appended to it.
    GU_Detail* gdPtr = unpackToPolygons ? new GU_Detail : &gd;

    GA_Size start = gdPtr->getNumPrimitives();
    AppendPackedPrims(*gdPtr, prims, variants, timeMap, dstVpLOD, dstPurposes, err);

    // Now set transforms on those appended packed prims.
    GA_Range dstRng(gdPtr->getPrimitiveRangeSlice(start));
    SetPackedPrimTransforms(*gdPtr, dstRng, dstXforms.array(), err);

    // Need to build a list of source offsets,
    // including repeats for expanded prims. 
    GA_OffsetList srcOffsets;

    if (unpackToPolygons) {
        GA_Size gdStart = gd.getNumPrimitives();

        // If unpacking down to polygons, iterate through the intermediate
        // packed prims in gdPtr and unpack them into gd.
        exint i = 0;
        for (GA_Iterator it(dstRng); !it.atEnd(); ++it, ++i) {
            if(task.wasInterrupted()) {
                return false;
            }

            const GEO_Primitive* p = gdPtr->getGEOPrimitive(*it);
            const GU_PrimPacked* pp = dynamic_cast<const GU_PrimPacked*>(p);
            if (!pp) {
                continue;
            }

            if (const GusdGU_PackedUSD* prim =
                dynamic_cast<const GusdGU_PackedUSD*>(pp->implementation())) {

                GA_Size gdCurrent = gd.getNumPrimitives();

                // Unpack this prim.
                if (!prim->unpackGeometry(gd, primvarPattern.c_str())) {
                    return false;
                }

                const GA_Offset offset =
                    indexToOffset(primIndexPairs(i).second);
                const exint count = gd.getNumPrimitives() - gdCurrent;
                for (exint j = 0; j < count; ++j) {
                    srcOffsets.append(offset);
                }
            }
        }

        // dstRng needs to be reset to be the range of unpacked prims in
        // gd (instead of the range of intermediate packed prims in gdPtr).
        dstRng = GA_Range(gd.getPrimitiveRangeSlice(gdStart));

        // All done with gdPtr.
        delete gdPtr;

    } else {
        // Compute list of source offsets.
        srcOffsets.setEntries(dstSize);
        for (exint i = 0; i < dstSize; ++i) {
            srcOffsets.set(i, indexToOffset(primIndexPairs(i).second));
        }
    }

    // Get the filtered list of attributes to copy.
    UT_Array<const GA_Attribute*> attrs;
    srcGd.getAttributes().matchAttributes(filter, srcRng.getOwner(), attrs);

    // If no attrs to copy, exit early.
    if (attrs.isEmpty()) {
        return true;
    }

    GA_Range newSrcRng(srcGd.getIndexMap(srcRng.getOwner()), srcOffsets);

    // dstRng and newSrcRng should be the same size.
    UT_ASSERT(dstRng.getEntries() == newSrcRng.getEntries());

    if (CopyAttributes(newSrcRng, dstRng, gd.getPrimitiveMap(), attrs)) {
        return true;
    }

    return false;
}


namespace {


bool
_WriteVariantStrings(GU_Detail& gd,
                     const GA_Range& rng,
                     const UT_Array<UT_StringHolder>& orderedVariants,
                     const UT_Array<exint>& variantIndices,
                     const char* variantsAttr,
                     GusdUT_ErrorContext* err)
{
    auto* boss = UTgetInterrupt();

    UT_AutoInterrupt task("Write variant strings", boss);

    auto* attr = gd.addStringTuple(rng.getOwner(), variantsAttr, 1);
    if(!attr) {
        if(err) _LogCreateError(*err, variantsAttr);
        return false;
    }

    auto* tuple = attr->getAIFSharedStringTuple();
    GA_AIFSharedStringTuple::StringBuffer buf(attr, tuple);

    // Add strings, creating a map of variantIndex -> string table index.
    UT_Array<GA_StringIndexType> variantIndexToStrMap;
    variantIndexToStrMap.setSize(orderedVariants.size());

    for(exint i = 0; i < orderedVariants.size(); ++i) {
        const UT_StringHolder& path = orderedVariants(i);
        variantIndexToStrMap(i) = path.isstring() ?
            buf.append(path) : GA_INVALID_STRING_INDEX;
    }

    // Apply the string indices to all of the source offsets.
    // XXX: could be done in parallel...
    GA_RWHandleS hnd(attr);
    
    char bcnt = 0;
    exint idx = 0;
    for(GA_Iterator it(rng); !it.atEnd(); ++it, ++idx) {
        if(BOOST_UNLIKELY(!++bcnt && boss->opInterrupt()))
            return false;
        exint variantIndex = variantIndices(idx);
        if(variantIndex >= 0)
            hnd.set(*it, variantIndexToStrMap(variantIndex));
    }
    return true;
}


} /*namespace*/


bool
GusdGU_USD::WriteVariantSelectionsToAttr(
    GU_Detail& gd,
    const GA_Range& rng,
    const UT_Array<UsdPrim>& prims,
    const GusdUSD_Utils::VariantSelArray& selections,
    const char* variantsAttr,
    const UT_Array<SdfPath>* prevVariants,
    GusdUT_ErrorContext* err)
{
    UT_ASSERT(prims.size() == rng.getEntries());
    UT_ASSERT(!prevVariants || prevVariants->size() == prims.size());

    UT_Array<UT_StringHolder> orderedVariants;
    UT_Array<exint> indices;

    if(!GusdUSD_Utils::AppendVariantSelections(
           prims, selections, orderedVariants, indices, prevVariants))
        return false;
    return _WriteVariantStrings(gd, rng, orderedVariants,
                                indices, variantsAttr, err);
}


bool
GusdGU_USD::WriteVariantSelectionsToPackedPrims(
    GU_Detail& gd,
    const GA_Range& rng,
    const UT_Array<UsdPrim>& prims,
    const GusdUSD_Utils::VariantSelArray& selections,
    const UT_Array<SdfPath>* prevVariants,
    GusdUT_ErrorContext* err)
{
    if(err) err->AddError("GusdGU_USD::WriteVariantSelectionsToPackedPrims() "
                          "is not yet implemented");
    return false;
}


GA_Offset
GusdGU_USD::AppendRefPointsForExpandedVariants(
    GU_Detail& gd,
    const GA_Detail& srcGd,
    const GA_Range& srcRng,
    const UT_Array<UT_StringHolder>& orderedVariants,
    const GusdUSD_Utils::IndexPairArray& variantIndices,
    const GA_AttributeFilter& filter,
    const char* variantsAttr,
    GusdUT_ErrorContext* err)
{
    // Need an array of just the variant indices.
    UT_Array<exint> indices(variantIndices.size(), variantIndices.size());
    for(exint i = 0; i < indices.size(); ++i)
        indices(i) = variantIndices(i).second;

    // Add the new ref points.
    GA_Offset start = gd.appendPointBlock(indices.size());
    if(!GAisValid(start))
        return GA_INVALID_OFFSET;

    // Write the variants attribute.
    GA_Range dstRng(gd.getPointMap(), start, start+indices.size());
    if(!_WriteVariantStrings(gd, dstRng, orderedVariants,
                             indices, variantsAttr, err))
        return GA_INVALID_OFFSET;

    // Find attributes to copy.
    GA_AttributeFilter filterNoRefAttrs(
        GA_AttributeFilter::selectAnd(
            GA_AttributeFilter::selectNot(
                GA_AttributeFilter::selectByName(variantsAttr)),
            filter));

    UT_Array<const GA_Attribute*> attrs;
    srcGd.getAttributes().matchAttributes(
        filterNoRefAttrs, srcRng.getOwner(), attrs);

    if(attrs.isEmpty())
        return start;

    /* Need to build out a source range including repeats for all
       of our expanded indices.*/
    GA_OffsetList srcOffsets;
    const GA_IndexMap& srcMap = srcGd.getIndexMap(srcRng.getOwner());
    {
        srcOffsets.setEntries(variantIndices.size());
        GA_OffsetArray offsets;
        if(!OffsetArrayFromRange(srcRng, offsets))
            return GA_INVALID_OFFSET;
        for(exint i = 0; i < variantIndices.size(); ++i)
            srcOffsets.set(i, offsets(variantIndices(i).first));
    }
    if(CopyAttributes(GA_Range(srcMap, srcOffsets),
                      dstRng, gd.getPointMap(), attrs))
        return start;
    return GA_INVALID_OFFSET;
}


GA_Offset
GusdGU_USD::AppendPackedPrimsForExpandedVariants(
    GU_Detail& gd,
    const GA_Detail& srcGd,
    const GA_Range& srcRng,
    const UT_Array<UT_StringHolder>& orderedVariants,
    const GusdUSD_Utils::IndexPairArray& variantIndices,
    const GA_AttributeFilter& filter,
    GusdUT_ErrorContext* err)
{
    if(err) err->AddError("GusdGU_USD::AppendPackedPrimsForExpandedVariants() "
                          "is not yet implemented");
    return GA_INVALID_OFFSET;
}


bool
GusdGU_USD::CopyAttributes(const GA_Range& srcRng,
                           const GA_Range& dstRng,
                           const GA_IndexMap& dstMap,
                           const UT_Array<const GA_Attribute*>& attrs)
{
    UT_AutoInterrupt task("Copying attributes");

    /* Process each attribute individually (best for performance).
       Note that we want to keep going and at least copy attrs even
       if the offset list is emtpy.*/
    for(exint i = 0; i < attrs.size(); ++i)
    {
        if(task.wasInterrupted())
            return false;
        const GA_Attribute* srcAttr = attrs(i);

        GA_Attribute* dstAttr = NULL;

        if(const auto* grpAttr = GA_ATIGroupBool::cast(srcAttr))
        {
            /* cloneAttribute() does not clone groups, because they
               define additional structure on a detail. Must go through
               the group creation interface.*/

            /* createElementGroup() will cause an existing group
               to be destroyed, so must first try to finding compatible
               groups.*/

            auto* grp = dstMap.getDetail().findElementGroup(
                dstMap.getOwner(), grpAttr->getName());
            if(!grp || grp->getOrdered() != grpAttr->getOrdered())
            {
                /** XXX: if we had an existing group of an umatched order,
                    we lose its membership at this point.
                    This is expected, because if we are turning an unordered
                    group into an ordered group, it's not clear what the
                    order should be. However, it may be desirable to preserve
                    existing membership when converting in the other
                    direction.*/
                grp = dstMap.getDetail().createElementGroup(
                    dstMap.getOwner(), grpAttr->getName(),
                    grpAttr->getOrdered());
            }
            if(grp)
                dstAttr = grp->getAttribute();
        }
        else
        {
            dstAttr = dstMap.getDetail().getAttributes().cloneAttribute(
                dstMap.getOwner(), srcAttr->getName(),
                *srcAttr, /*clone opts*/ true);
        }

        if(dstAttr)
        {
            if(const GA_AIFCopyData* copy = srcAttr->getAIFCopyData())
            {
                /* Copy the attribute values.
                   This runs in parallel internally.

                   TODO: Verify that this is doing something smart
                   for blob data.
                   Also, we ignore copying errors, assuming that
                   a failure to copy means copying is incompatible
                   for the type. Is this correct? */
                copy->copy(*dstAttr, dstRng, *srcAttr, srcRng);
            }
        }
    }
    return true;
}


namespace {


template <typename ModifyFn>
struct _ModifyXformsT
{
    _ModifyXformsT(const ModifyFn& modifyFn, const GA_OffsetArray& offsets,
                   UT_Matrix4D* xforms)
        : _modifyFn(modifyFn), _offsets(offsets), _xforms(xforms) {}

    void    operator()(const UT_BlockedRange<size_t>& r) const
            {
                auto* boss = UTgetInterrupt();
                char bcnt = 0;

                for(size_t i = r.begin(); i < r.end(); ++i) {
                    if(BOOST_UNLIKELY(!++bcnt && boss->opInterrupt()))
                        return;
                    _modifyFn(_xforms[i], _offsets(i));
                }
            }
private:
    const ModifyFn&         _modifyFn;
    const GA_OffsetArray&   _offsets;
    UT_Matrix4D* const      _xforms;
};


struct _XformRowFromAttrFn
{
    _XformRowFromAttrFn(const GA_ROHandleV3& attr, int comp)
        : _attr(attr), _comp(comp) {}

    void    operator()(UT_Matrix4D& xform, GA_Offset o) const
            {
                UT_Vector3F vec = _attr.get(o);
                /* Scale should come from scale attrs;
                   only want orientation here.*/
                vec.normalize();
                xform[_comp] = UT_Vector4F(vec);
            }

private:
    const GA_ROHandleV3&    _attr;
    const int               _comp;
};


struct _XformApplyScaleFn
{
    _XformApplyScaleFn(const GA_ROHandleV3& attr) : _attr(attr) {}

    void    operator()(UT_Matrix4D& xform, GA_Offset o) const
            {
                UT_Vector3F scale = _attr.get(o);
                for(int i = 0; i < 3; ++i)
                    xform[i] *= scale[i];
            }
private:
    const GA_ROHandleV3&    _attr;
};


struct _XformApplyPScaleFn
{
    _XformApplyPScaleFn(const GA_ROHandleF& attr) : _attr(attr) {}

    void    operator()(UT_Matrix4D& xform, GA_Offset o) const
            {
                float scale = _attr.get(o);
                for(int i = 0; i < 3; ++i)
                    xform[i] *= scale;
            }
private:
    const GA_ROHandleF&    _attr;
};


struct _XformsFromInstMatrixFn
{
    _XformsFromInstMatrixFn(const GA_AttributeInstanceMatrix& instMx,
                            const GA_ROHandleV3& p,
                            const GA_OffsetArray& offsets,
                            UT_Matrix4D* xforms)
        : _instMx(instMx), _p(p), _offsets(offsets), _xforms(xforms) {}
    
    void    operator()(const UT_BlockedRange<size_t>& r) const
            {
                auto* boss = UTgetInterrupt();
                char bcnt = 0;

                for(size_t i = r.begin(); i < r.end(); ++i) {
                    if(BOOST_UNLIKELY(!++bcnt && boss->opInterrupt()))
                        return;
                    GA_Offset o = _offsets(i);
                    _instMx.getMatrix(_xforms[i], _p.get(o), o);
                }
            }
private:
    const GA_AttributeInstanceMatrix&   _instMx;
    const GA_ROHandleV3&                _p;
    const GA_OffsetArray&               _offsets;
    UT_Matrix4D* const                  _xforms;
};


} /*namespace*/

bool
GusdGU_USD::GetPackedPrimViewportLODAndPurposes(
                const GA_Detail& gd,
                const GA_OffsetArray& offsets,
                UT_StringArray& viewportLOD,
                UT_Array<GusdPurposeSet>& purposes,
                GusdUT_ErrorContext* err)
{
    for (exint i = 0; i < offsets.size(); ++i) {

        const GA_Primitive* p = gd.getPrimitive(offsets(i));
        const GU_PrimPacked* pp = dynamic_cast<const GU_PrimPacked*>(p);
        if (!pp) {
            continue;
        }

        if (const GusdGU_PackedUSD* prim =
            dynamic_cast<const GusdGU_PackedUSD*>(pp->implementation())) {

            viewportLOD(i) = prim->intrinsicViewportLOD();
            purposes(i) = prim->getPurposes();
        }
    }
    return true;
}

bool
GusdGU_USD::ComputeTransformsFromAttrs(const GA_Detail& gd,
                                       GA_AttributeOwner owner,
                                       const GA_OffsetArray& offsets,
                                       UT_Matrix4D* xforms)
{
    UT_AutoInterrupt task("Computing tranforms from attributes");

    GA_ROHandleV3 p(&gd, owner, GEO_STD_ATTRIB_POSITION);
    if(p.isInvalid())
        return false;

    GA_ROHandleV3 i(&gd, owner, "i"), j(&gd, owner, "j"), k(&gd, owner, "k");

    UT_BlockedRange<size_t> rng(0, offsets.size());

    if(i.isValid() && j.isValid() && k.isValid()) {

        GA_ROHandleV3 handles[] = {i, j, k, p};

        for(int comp = 0; comp < 4; ++comp) {
            UTparallelForLightItems(
                rng, _ModifyXformsT<_XformRowFromAttrFn>(
                    _XformRowFromAttrFn(handles[comp], comp), offsets, xforms));
            if(task.wasInterrupted())
                return false;
        }
        GA_ROHandleF pscale(&gd, owner, GEO_STD_ATTRIB_PSCALE);
        if(pscale.isValid()) {
            UTparallelForLightItems(
                rng, _ModifyXformsT<_XformApplyPScaleFn>(
                    _XformApplyPScaleFn(pscale), offsets, xforms));
            if(task.wasInterrupted())
                return false;
        }
        GA_ROHandleV3 scale(&gd, owner, "scale");
        if(scale.isValid()) {
            UTparallelForLightItems(
                rng, _ModifyXformsT<_XformApplyScaleFn>(
                    _XformApplyScaleFn(scale), offsets, xforms));
            if(task.wasInterrupted())
                return false;
        }
        
        return true;
    }
    GA_AttributeInstanceMatrix instMx(gd.getAttributeDict(owner));
    UTparallelFor(rng, _XformsFromInstMatrixFn(instMx, p, offsets, xforms));
    return !task.wasInterrupted();
}


bool
GusdGU_USD::ComputeTransformsFromPackedPrims(const GA_Detail& gd,
                                             const GA_OffsetArray& offsets,
                                             UT_Matrix4D* xforms,
                                             GusdUT_ErrorContext* err)
{
    for (exint i = 0; i < offsets.size(); ++i) {
        const GA_Primitive* p = gd.getPrimitive(offsets(i));

        if ( p->getTypeId() == GusdGU_PackedUSD::typeId() ) {
            auto prim = UTverify_cast<const GU_PrimPacked*>(p);
            auto packedUSD = UTverify_cast<const GusdGU_PackedUSD*>(prim->implementation());

            // The transforms on a USD packed prim contains the combination
            // of the transform in the USD file and any transform the user
            // has applied in Houdini. Compute just the transform that the
            // user has applied in Houdini.

            UT_Matrix4D primXform;
            prim->getFullTransform4(primXform);
            UT_Matrix4D invUsdXform = packedUSD->getUsdTransform();

            invUsdXform.invert();
            xforms[i] = invUsdXform * primXform;

        } else {
            xforms[i].identity();
        }
    }
    return true;
}


bool
GusdGU_USD::SetTransformAttrs(GU_Detail& gd,
                              const GA_Range& r,
                              const GA_OffsetArray& indexMap,
                              OrientAttrRepresentation orientRep,
                              ScaleAttrRepresentation scaleRep,
                              const UT_Matrix4D* xforms,
                              GusdUT_ErrorContext* err)
{
    /* TODO: This currently makes up a large chunk of exec time
             for the USD Transform SOP. Consider threading this.*/

    auto* boss = UTgetInterrupt();
    UT_AutoInterrupt task("Set transform attributes", boss);

    auto owner = r.getOwner();

    // Position.
    GA_RWHandleV3 p(&gd, owner, GEO_STD_ATTRIB_POSITION);
    if(!_AttrBindSuccess(p, GA_Names::P, err))
       return false;

    char bcnt = 0;
    for(GA_Iterator it(r); !it.atEnd(); ++it) {
        if(BOOST_UNLIKELY(!++bcnt && boss->opInterrupt()))
            return false;
        const UT_Matrix4D& xf = xforms[indexMap(*it)];
        p.set(*it, UT_Vector3D(xf[3]));
    }

    // Scale.
    if(scaleRep != SCALEATTR_IGNORE) {
        if(scaleRep == SCALEATTR_SCALE) {
            GA_RWHandleV3 scale(gd.addFloatTuple(owner, GA_Names::scale, 3));
            if(!_AttrCreateSuccess(scale, GA_Names::scale, err))
                return false;

            char bcnt = 0;
            for(GA_Iterator it(r); !it.atEnd(); ++it) {
                if(BOOST_UNLIKELY(!++bcnt && boss->opInterrupt()))
                    return false;

                const UT_Matrix4D& xf = xforms[indexMap(*it)];
                scale.set(*it, UT_Vector3F(xf[0].length(),
                                           xf[1].length(),
                                           xf[2].length()));
            }

            GA_RWHandleF pscale(&gd, owner, GA_Names::pscale);
            if(pscale.isValid()) {
                // Make sure pscale is set to 1 over the range.
                GA_Attribute* pscaleAttr = pscale.getAttribute();
                if(const GA_AIFTuple* tuple = pscaleAttr->getAIFTuple())
                    tuple->set(pscaleAttr, r, 1.0f);
            }
        } else { // SCALEATTR_PSCALE
            GA_RWHandleF pscale(
                gd.addFloatTuple(owner, GA_Names::pscale, 1));
            if(!_AttrCreateSuccess(pscale, GA_Names::pscale, err))
                return false;

            char bcnt = 0;
            for(GA_Iterator it(r); !it.atEnd(); ++it) {
                if(BOOST_UNLIKELY(!++bcnt && boss->opInterrupt()))
                    return false;

                const UT_Matrix4D& xf = xforms[indexMap(*it)];
                float s = xf[0].length() + xf[1].length() + xf[2].length();
                s /= 3;
                pscale.set(*it, s);
            }

            GA_RWHandleV3 scale(&gd, owner, GA_Names::scale);
            if(scale.isValid()) {
                // Make sure sclae is set to 1 over the range.
                GA_Attribute* scaleAttr = scale.getAttribute();
                
                float scaleOne[] = {1,1,1};
                if(const GA_AIFTuple* tuple = scaleAttr->getAIFTuple())
                    tuple->set(scaleAttr, r, scaleOne, 3);
            }
        }
    }

    // Orientation
    if(orientRep != ORIENTATTR_IGNORE) {
        if(orientRep == ORIENTATTR_ORIENT) {
            GA_RWHandleQ orient(
                gd.addFloatTuple(owner, GA_Names::orient, 4));
            if(!_AttrCreateSuccess(orient, GA_Names::orient, err))
                return false;
            orient.getAttribute()->setTypeInfo(GA_TYPE_QUATERNION);

            char bcnt = 0;
            for(GA_Iterator it(r); !it.atEnd(); ++it) {
                if(BOOST_UNLIKELY(!++bcnt && boss->opInterrupt()))
                    return false;

                const UT_Matrix4D& xf = xforms[indexMap(*it)];
                UT_Matrix3 rot;
                xf.extractRotate(rot);
                rot.makeRotationMatrix();
                UT_QuaternionF q;
                q.updateFromRotationMatrix(rot);
                orient.set(*it, q);
            }
        } else {
            const char* names[] = {"i","j","k"};
            GA_RWHandleV3 handles[3];
            for(int i = 0; i < 3; ++i) {
                handles[i].bind(gd.addFloatTuple(owner, names[i], 3));
                if(!_AttrCreateSuccess(handles[i], names[i], err))
                    return false;
                handles[i].getAttribute()->setTypeInfo(GA_TYPE_NORMAL);
            }
            
            // iterate by attr to improve cache locality.
            for(int i = 0; i < 3; ++i) {
                char bcnt = 0;
                for(GA_Iterator it(r); !it.atEnd(); ++it) {
                    if(BOOST_UNLIKELY(!++bcnt && boss->opInterrupt()))
                        return false;
                    const UT_Matrix4D& xf = xforms[indexMap(*it)];
                    UT_Vector3D vec(xf[i]);
                    /* Scale should come from scale attrs;
                       only want orientation here.*/
                    vec.normalize();
                    handles[i].set(*it, vec);
                }
            }
        }
    }
    return true;
}


bool
GusdGU_USD::SetPackedPrimTransforms(GU_Detail& gd,
                                    const GA_Range& r,
                                    const UT_Matrix4D* xforms,
                                    GusdUT_ErrorContext* err)
{
    exint i = 0;
    for (GA_Iterator it(r); !it.atEnd(); ++it, ++i) {
        GEO_Primitive* p = gd.getGEOPrimitive(*it);

        if ( p->getTypeId() == GusdGU_PackedUSD::typeId() ) {
            auto prim = UTverify_cast<GU_PrimPacked*>(p);
            auto packedUSD = UTverify_cast<GusdGU_PackedUSD*>(prim->implementation());

            // The transforms on a USD packed prim contains the combination
            // of the transform in the USD file and any transform the user
            // has applied in Houdini. 

            UT_Matrix4D m = packedUSD->getUsdTransform() * xforms[i];

            UT_Matrix3D xform(m);
            UT_Vector3 pos;
            m.getTranslates(pos);

            prim->setLocalTransform(xform);
            prim->setPos3(0, pos);
        }
    }
    return true;
}
 


namespace {


struct _XformAttrsFn
{
    _XformAttrsFn(const GA_AttributeTransformer& xformer,
                  const GA_OffsetArray& indexMap,
                  const UT_Matrix4D* xforms)
        : _xformer(xformer), _indexMap(indexMap), _xforms(xforms) {}

    void    operator()(const GA_SplittableRange& r) const   
            {
                auto* boss = UTgetInterrupt();
                char bcnt = 0;
                
                GA_Offset o, end;
                for(GA_Iterator it(r); it.blockAdvance(o,end); ) {
                    if(BOOST_UNLIKELY(!++bcnt && boss->opInterrupt()))
                        return;

                    for( ; o < end; ++o) {
                        GA_AttributeTransformer::Transform<double> xf(
                            _xforms[_indexMap(o)]);
                        _xformer.transform(o, xf);
                    }
                }
            }
    

private:
    GA_AttributeTransformer _xformer;
    const GA_OffsetArray&   _indexMap;
    const UT_Matrix4D*      _xforms;
};


} /*namespace*/


bool
GusdGU_USD::MultTransformableAttrs(GU_Detail& gd,
                                   const GA_Range& r,
                                   const GA_OffsetArray& indexMap,
                                   const UT_Matrix4D* xforms,
                                   bool keepLengths,
                                   const GA_AttributeFilter* filter)
{
    UT_ASSERT(xforms);

    UT_AutoInterrupt task("Transform attributes");

    GA_AttributeTransformer xformer(gd, r.getOwner());

    if(filter) {
        xformer.addAttributes(*filter, keepLengths);
    } else {
        GA_AttributeFilter xformables(
            GA_AttributeFilter::selectTransforming(/*includeP*/ true));
        xformer.addAttributes(xformables, keepLengths);
    }
    UTparallelFor(GA_SplittableRange(r),
                  _XformAttrsFn(xformer, indexMap, xforms));
    return !task.wasInterrupted();
}

PXR_NAMESPACE_CLOSE_SCOPE
