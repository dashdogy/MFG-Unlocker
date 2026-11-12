#pragma once

#include <cstdint>

namespace universal_route_policy
{
enum class NgxDispatchRoute : uint32_t
{
    ePending = 0,
    eProvider = 1,
    eRuntime = 2,
};

// Non-FG Create calls are unconditional pass-through. An FG call may inspect
// a route while no route has been selected, or while it is on the selected
// route. Provider resolution must succeed before the pending route is
// committed; an ambiguous runtime observation therefore leaves the provider
// entry free to claim the same Create call when dispatch reaches it.
constexpr bool ShouldInspectNgxCreate(bool frameGeneration,
    NgxDispatchRoute active, NgxDispatchRoute observed) noexcept
{
    return frameGeneration
        && (active == NgxDispatchRoute::ePending || active == observed);
}

constexpr bool CanCommitNgxDispatchRoute(bool providerResolved,
    NgxDispatchRoute active, NgxDispatchRoute observed) noexcept
{
    return providerResolved
        && (active == NgxDispatchRoute::ePending || active == observed);
}

// Runtime Evaluate has no feature-id argument. A caller from the selected
// provider is authoritative. A runtime-owned dispatcher may be admitted only
// for a route that Create resolved from one unique covered DLSS-G provider and
// only after the parameter block independently identifies a temporal sample.
constexpr bool CanInspectRuntimeEvaluate(bool callerIsSelectedProvider,
    bool selectedByUniqueCandidate,
    bool validTemporalSample) noexcept
{
    return callerIsSelectedProvider
        || (selectedByUniqueCandidate && validTemporalSample);
}

enum class Path : uint32_t
{
    eNone = 0,
    ePublic = 1,
    eInternal = 2,
    eResolver = 3,
};

enum class Failure : uint32_t
{
    eNone = 0,
    eNoActiveRoute = 1,
    eActiveWrapperUnpatched = 2,
    eSetterNotCovered = 3,
    eStateNotCovered = 4,
    eUnknownOptionsVersion = 5,
    eUnknownStateVersion = 6,
    eMalformedStructureChain = 7,
    eProviderNotSelected = 8,
    eProviderChangedAfterCreate = 9,
    eProviderCreateNotCovered = 10,
    eProviderEvaluateNotCovered = 11,
    eAdapterNotVerified = 12,
    eMidpointNotReady = 13,
    eWrapperChangedAfterCreate = 14,
    eAwaitingOptionsRequest = 15,
};

struct Identity
{
    uintptr_t module = 0;
    uint64_t generation = 0;

    constexpr explicit operator bool() const noexcept
    {
        return module != 0 && generation != 0;
    }
};

constexpr bool SameIdentity(Identity left, Identity right) noexcept
{
    return left.module == right.module && left.generation == right.generation;
}

constexpr bool IsSupportedOptionsVersion(uint64_t version) noexcept
{
    return version >= 1 && version <= 5;
}

constexpr bool IsSupportedStateVersion(uint64_t version) noexcept
{
    return version >= 1 && version <= 4;
}

constexpr bool IsAcceptedResult(int32_t result, int32_t success,
    int32_t acceptedWarning) noexcept
{
    return result == success || result == acceptedWarning;
}

constexpr bool IsCovered(Path path, bool publicDetourCurrent,
    bool internalDetourCurrent, bool resolverFallbackCurrent) noexcept
{
    switch (path)
    {
    case Path::ePublic: return publicDetourCurrent;
    case Path::eInternal: return internalDetourCurrent;
    case Path::eResolver: return resolverFallbackCurrent;
    default: return false;
    }
}

// Fixed-mode control only needs one callable state entry to be covered. The
// game does not have to have called that entry yet; a fresh state sample is a
// separate requirement used by Dynamic capability and runtime telemetry.
constexpr bool HasCoveredEntry(bool publicDetourCurrent,
    bool internalDetourCurrent, bool resolverFallbackCurrent) noexcept
{
    return publicDetourCurrent || internalDetourCurrent
        || resolverFallbackCurrent;
}

enum class StructureStatus : uint32_t
{
    eNotFound = 0,
    eFound = 1,
    eMalformed = 2,
};

enum class AdapterAction : uint32_t
{
    ePassThrough = 0,
    eOverride = 1,
    eRejectMalformed = 2,
    eRejectUnknownOptions = 3,
    eRejectUnknownState = 4,
};

constexpr AdapterAction ClassifyInternalSet(StructureStatus options,
    StructureStatus viewport, uint64_t optionsVersion) noexcept
{
    if (options == StructureStatus::eNotFound)
        return AdapterAction::ePassThrough;
    if (options == StructureStatus::eMalformed
        || viewport == StructureStatus::eMalformed)
        return AdapterAction::eRejectMalformed;
    if (viewport != StructureStatus::eFound)
        return AdapterAction::ePassThrough;
    if (!IsSupportedOptionsVersion(optionsVersion))
        return AdapterAction::eRejectUnknownOptions;
    return AdapterAction::eOverride;
}

constexpr AdapterAction ClassifyInternalGet(StructureStatus state,
    StructureStatus viewport, uint64_t stateVersion,
    StructureStatus options, uint64_t optionsVersion) noexcept
{
    if (state == StructureStatus::eNotFound)
        return AdapterAction::ePassThrough;
    if (state == StructureStatus::eMalformed
        || viewport == StructureStatus::eMalformed
        || options == StructureStatus::eMalformed)
        return AdapterAction::eRejectMalformed;
    if (viewport != StructureStatus::eFound)
        return AdapterAction::ePassThrough;
    if (!IsSupportedStateVersion(stateVersion))
        return AdapterAction::eRejectUnknownState;
    if (options == StructureStatus::eFound
        && !IsSupportedOptionsVersion(optionsVersion))
        return AdapterAction::eRejectUnknownOptions;
    return AdapterAction::eOverride;
}

struct Lifecycle
{
    bool frameGenerationOn = false;
    bool pipelineCreated = false;
    bool offAccepted = false;
    bool releaseObserved = false;
};

constexpr bool CanActivateRoute(Identity active, Identity candidate,
    const Lifecycle& lifecycle) noexcept
{
    if (!active || SameIdentity(active, candidate))
        return true;
    return !lifecycle.frameGenerationOn && !lifecycle.pipelineCreated;
}

enum class ProviderSelection : uint32_t
{
    eSelected = 0,
    eSameProvider = 1,
    eRejectedChange = 2,
};

enum class ProviderResolution : uint32_t
{
    eUnavailable = 0,
    eDirect = 1,
    eUniqueCandidate = 2,
    eAmbiguous = 3,
};

constexpr ProviderResolution ResolveProvider(bool directSupported,
    uint32_t fullyCoveredCandidates) noexcept
{
    if (directSupported)
        return ProviderResolution::eDirect;
    if (fullyCoveredCandidates == 1)
        return ProviderResolution::eUniqueCandidate;
    return fullyCoveredCandidates == 0
        ? ProviderResolution::eUnavailable
        : ProviderResolution::eAmbiguous;
}

constexpr bool NeedsInternalFallback(bool publicEntryCovered) noexcept
{
    return !publicEntryCovered;
}

constexpr bool CanInstallLifecycleEntry(bool activeRoute) noexcept
{
    return activeRoute;
}

constexpr ProviderSelection SelectProvider(uintptr_t activeProvider,
    uintptr_t observedProvider) noexcept
{
    if (activeProvider == 0)
        return ProviderSelection::eSelected;
    return activeProvider == observedProvider
        ? ProviderSelection::eSameProvider
        : ProviderSelection::eRejectedChange;
}

struct Readiness
{
    bool activeRoute = false;
    bool wrapperPatched = false;
    bool structuresCompatible = false;
    bool setterCovered = false;
    bool stateEntryCovered = false;
    bool providerSelected = false;
    bool providerChanged = false;
    bool createCovered = false;
    bool evaluateCovered = false;
    bool adapterVerified = false;
    bool midpointReadyAtCreate = false;
    bool awaitingOptionsRequest = false;
};

constexpr Failure EvaluateReadiness(const Readiness& state) noexcept
{
    if (!state.activeRoute)
        return Failure::eNoActiveRoute;
    if (!state.wrapperPatched)
        return Failure::eActiveWrapperUnpatched;
    if (!state.structuresCompatible)
        return Failure::eMalformedStructureChain;
    if (!state.setterCovered)
        return Failure::eSetterNotCovered;
    if (!state.stateEntryCovered)
        return Failure::eStateNotCovered;
    if (state.awaitingOptionsRequest)
        return Failure::eAwaitingOptionsRequest;
    if (!state.providerSelected)
        return Failure::eProviderNotSelected;
    if (state.providerChanged)
        return Failure::eProviderChangedAfterCreate;
    if (!state.createCovered)
        return Failure::eProviderCreateNotCovered;
    if (!state.evaluateCovered)
        return Failure::eProviderEvaluateNotCovered;
    if (!state.adapterVerified)
        return Failure::eAdapterNotVerified;
    if (!state.midpointReadyAtCreate)
        return Failure::eMidpointNotReady;
    return Failure::eNone;
}

constexpr const char* FailureName(Failure failure) noexcept
{
    switch (failure)
    {
    case Failure::eNone: return "none";
    case Failure::eNoActiveRoute: return "no active wrapper call";
    case Failure::eActiveWrapperUnpatched: return "active wrapper unpatched";
    case Failure::eSetterNotCovered: return "active setter not covered";
    case Failure::eStateNotCovered: return "active state entry not covered";
    case Failure::eUnknownOptionsVersion: return "unknown options version";
    case Failure::eUnknownStateVersion: return "unknown state version";
    case Failure::eMalformedStructureChain: return "invalid structure chain";
    case Failure::eProviderNotSelected: return "provider not selected by Create";
    case Failure::eProviderChangedAfterCreate: return "provider changed after Create";
    case Failure::eProviderCreateNotCovered: return "provider Create not covered";
    case Failure::eProviderEvaluateNotCovered: return "provider Evaluate not covered";
    case Failure::eAdapterNotVerified: return "adapter not verified";
    case Failure::eMidpointNotReady: return "midpoint not ready at first Create";
    case Failure::eWrapperChangedAfterCreate: return "wrapper changed before release";
    case Failure::eAwaitingOptionsRequest: return "awaiting first game FG options request";
    default: return "unknown";
    }
}
}
