// Compile the frozen implementation through a narrow adapter translation unit.
// Do not edit Features.cpp: its authoritative hash is verified by the package.
#include <cstdint>
class AActor;
class UObject;
#ifdef ROB_LITE_SANDBOX
namespace FrozenJasonBridge
{
    bool AdoptCounselorModeJason(AActor*, UObject*, AActor*);
}
#endif
extern "C" __declspec(noinline) bool SafeProcessEventCall(
    uintptr_t vptr,
    void* obj,
    void* func,
    void* params);
extern "C" __declspec(noinline) bool FrozenBridgeSafeProcessEventCall(
    uintptr_t vptr,
    void* obj,
    void* func,
    void* params);

// Keep the frozen source byte-for-byte authoritative while interposing its
// ProcessEvent calls inside this translation unit.  The adapter forwards every
// call except an ordinary Jason K2_TeleportTo attempted while he is already
// within walking distance of the nearest counselor.
#define SafeProcessEventCall FrozenBridgeSafeProcessEventCall
#include "../Features/Features.cpp"
#undef SafeProcessEventCall
#include "FrozenJasonBridge.hpp"
#include "../../../vendor/minhook/include/MinHook.h"
#include <cstring>

static int32_t g_TrapTeleportExemptionDepth = 0;
static UFunction* g_MinimumMorphTeleportFunction = nullptr;
static ULONGLONG g_NextMinimumMorphLogAt = 0;
static ULONGLONG g_NextMorphCooldownRefusalLogAt = 0;

extern "C" __declspec(noinline) bool FrozenBridgeSafeProcessEventCall(
    uintptr_t vptr,
    void* obj,
    void* func,
    void* params)
{
    AActor* jason = g_JasonAIState.Jason;
    const bool mayBeJasonTeleport =
        g_JasonAIState.Active &&
        jason &&
        obj == jason &&
        func &&
        params;
    bool forwardedStuckRecoveryTeleport = false;

    if (mayBeJasonTeleport &&
        Memory::IsReadable(jason, sizeof(UObject)) &&
        jason->Class &&
        Memory::IsReadable(jason->Class, sizeof(UObject)))
    {
        if (!g_MinimumMorphTeleportFunction ||
            !Memory::IsReadable(
                g_MinimumMorphTeleportFunction,
                sizeof(UObject)))
        {
            g_MinimumMorphTeleportFunction =
                FindFunctionInHierarchyByName(
                    jason->Class,
                    "K2_TeleportTo");
        }

        if (func == g_MinimumMorphTeleportFunction)
        {
            const ULONGLONG now = GetTickCount64();
            const ULONGLONG morphReadyAt = JasonAIMorphReadyAt();
            if (morphReadyAt != 0 && now < morphReadyAt)
            {
                // The frozen counselor-ring stuck fallback bypasses the
                // shared Morph timer entirely.  Refuse every ordinary Jason
                // teleport until the same twenty-second cooldown used by
                // startup, distance, and trap-response Morphs has elapsed.
                reinterpret_cast<uint8_t*>(params)[24] = 0;
                if (now >= g_NextMorphCooldownRefusalLogAt)
                {
                    g_NextMorphCooldownRefusalLogAt = now + 2000;
                    Logger::Success(
                        "18L-AI counselor bridge: ordinary/stuck Morph refused by shared cooldown | remainingMs=" +
                        std::to_string(morphReadyAt - now));
                }
                return true;
            }

            // Scripted phone/car setup and owned-trap response are exempt
            // only from the proximity rule.  They still share the same
            // cooldown above.
            const bool ordinaryDistanceGate =
                !g_JasonAIState.StartupTrapSetupActive &&
                g_TrapTeleportExemptionDepth == 0;
            if (!ordinaryDistanceGate)
                return SafeProcessEventCall(vptr, obj, func, params);

            const bool stuckRecovery =
                g_JasonAIState.ConsecutiveStuckChecks >= 2;
            AActor* counselor =
                FindNearestJasonAICounselorTarget(jason);
            FVector jasonLocation{};
            FVector counselorLocation{};
            if (counselor &&
                GetJasonAIActorLocation(jason, jasonLocation) &&
                GetJasonAIActorLocation(counselor, counselorLocation))
            {
                const float dx = counselorLocation.X - jasonLocation.X;
                const float dy = counselorLocation.Y - jasonLocation.Y;
                const float dz = counselorLocation.Z - jasonLocation.Z;
                const float distanceSquared = dx * dx + dy * dy + dz * dz;
                // The general opening/hunt guard remains the requested 10m.
                // A stuck-recovery counselor-ring jump is more disruptive:
                // walking, door retry, and ordinary target rotation should
                // solve local obstruction, so require 30m for that path.
                const float minimumMorphDistanceCm =
                    stuckRecovery ? 3000.0f : 1000.0f;
                if (std::isfinite(distanceSquared) &&
                    distanceSquared <
                        minimumMorphDistanceCm * minimumMorphDistanceCm)
                {
                    // K2_TeleportTo's bool return value is the final byte in
                    // the frozen helper's verified 28-byte parameter block.
                    reinterpret_cast<uint8_t*>(params)[24] = 0;
                    if (now >= g_NextMinimumMorphLogAt)
                    {
                        g_NextMinimumMorphLogAt = now + 2000;
                        Logger::Success(
                            std::string(
                                "18L-AI counselor bridge: ") +
                            (stuckRecovery
                                ? "stuck-recovery Morph refused inside 30m local-action radius"
                                : "ordinary Morph refused inside 10m walk radius") +
                            " | distanceCm=" +
                            std::to_string(std::sqrt(distanceSquared)));
                    }
                    return true;
                }

                forwardedStuckRecoveryTeleport = stuckRecovery;
            }
        }
    }

    const bool callOK = SafeProcessEventCall(vptr, obj, func, params);
    if (forwardedStuckRecoveryTeleport &&
        callOK &&
        reinterpret_cast<uint8_t*>(params)[24] != 0)
    {
        // Frozen stuck recovery did not participate in the shared Morph
        // timer. Count its successful counselor-ring jump so it cannot chain
        // another Morph before the normal twenty-second recharge.
        MarkJasonAIMorphTeleportUsed("StuckRecovery");
    }
    return callOK;
}

namespace
{
    using UpdateSoundBlipsFn =
        void(__fastcall*)(UObject* killer, float deltaSeconds);

    using SetSoundBlipVisibilityFn =
        void(__fastcall*)(UObject* killer, bool visible);

    using TrapTriggeredFn =
        void(__fastcall*)(AActor* trap, AActor* victim);

    using GiveStartingItemFn =
        void(__fastcall*)(AActor* pawn, UClass* requestedClass);

    using AttemptInteractFn =
        void(__fastcall*)(UObject* manager,
            UObject* interactionComponent,
            uint8_t interactionMethod,
            bool autoLock);

    using ContextKillCanInteractFn =
        int32_t(__fastcall*)(UObject* contextKillComponent,
            AActor* interactor,
            const FVector* viewLocation,
            const FVector* viewDirection);

    using PamelaSweaterCanInteractFn =
        int32_t(__fastcall*)(AActor* sweater,
            AActor* interactor,
            const FVector* viewLocation,
            const FVector* viewDirection);

    static UpdateSoundBlipsFn g_OriginalUpdateSoundBlips = nullptr;
    static TrapTriggeredFn g_OriginalTrapTriggered = nullptr;
    static GiveStartingItemFn g_OriginalGiveStartingItem = nullptr;
    static ContextKillCanInteractFn g_OriginalContextKillCanInteract = nullptr;
    static PamelaSweaterCanInteractFn g_OriginalPamelaSweaterCanInteract = nullptr;
    static PamelaSweaterCanInteractFn g_BasePamelaPickupCanInteract = nullptr;
    static LPVOID g_UpdateSoundBlipsTarget = nullptr;
    static LPVOID g_TrapTriggeredTarget = nullptr;
    static LPVOID g_GiveStartingItemTarget = nullptr;
    static LPVOID g_ContextKillCanInteractTarget = nullptr;
    static LPVOID g_PamelaSweaterCanInteractTarget = nullptr;
    static bool g_UpdateSoundBlipsHookInstalled = false;
    static bool g_TrapTriggeredHookInstalled = false;
    static bool g_GiveStartingItemHookInstalled = false;
    static bool g_ContextKillCanInteractHookInstalled = false;
    static bool g_PamelaSweaterCanInteractHookInstalled = false;
    static AActor* g_LastUniversalFinalEligibilityFinisher = nullptr;
    static bool g_HunterAxeLoadoutRouteEnabled = false;
    static UClass* g_HunterSpawnAxeClass = nullptr;
    static AActor* g_HunterSpawnAxeItem = nullptr;
    static bool g_CounselorRouteTickWrapperInstalled = false;
    static AActor* g_LastExtendedDoorTarget = nullptr;
    static ULONGLONG g_LastNonAggroReadyAt = 0;
    static ULONGLONG g_NextTrapPriorityAttemptAt = 0;
    static ULONGLONG g_NextTrapPriorityLogAt = 0;
    static ULONGLONG g_TrapPriorityUntil = 0;
    static ULONGLONG g_TrapPriorityBaselineMorphAt = 0;
    static ULONGLONG g_NextKnifePickupScanAt = 0;
    static ULONGLONG g_NextKnifeRegistryRefreshAt = 0;
    static int32_t g_KnifeRegistryRefreshLevel = 0;
    static int32_t g_KnifeRegistryRefreshActor = 0;
    static AActor* g_KnifePickupRegistry[128]{};
    static int32_t g_KnifePickupRegistryCount = 0;
    static AActor* g_HidingSpotRegistry[256]{};
    static int32_t g_HidingSpotRegistryCount = 0;
    static bool g_WorldInteractionRegistryComplete = false;
    static ULONGLONG g_KnifePickupStartedAt = 0;
    static ULONGLONG g_CombatBusyUntil = 0;
    static std::atomic<ULONGLONG> g_QueuedTrapMorphBaseline{ 0 };
    static std::atomic<AActor*> g_QueuedTrapVictim{ nullptr };
    static std::atomic<AActor*> g_QueuedTriggeredTrap{ nullptr };
    static AActor* g_TrapPriorityVictim = nullptr;
    static bool g_TrapPriorityMorphCompleted = false;
    static int32_t g_LastShortenedPlacementObjective = -1;
    static AActor* g_PendingKnifePickup = nullptr;
    static UObject* g_PendingKnifeComponent = nullptr;
    static int32_t g_KnifeCountBeforePickup = -1;
    static AActor* g_LastHeldCounselor = nullptr;
    static ULONGLONG g_HeldCounselorObservedAt = 0;
    static AActor* g_GrabKillSlotsLoggedFor = nullptr;
    static ULONGLONG g_NextAdapterGrabKillAttemptAt = 0;
    static ULONGLONG g_NextGrabKillStateLogAt = 0;
    static int32_t g_NextAdapterGrabKillSlot = 0;
    static AActor* g_LocalCounselorTarget = nullptr;
    static UObject* g_LocalPlayerController = nullptr;
    static ULONGLONG g_NextLocalCounselorRefreshAt = 0;
    static int32_t g_CounselorEscapedPropertyOffset = -2;
    static uint8_t g_CounselorEscapedByteOffset = 0;
    static uint8_t g_CounselorEscapedFieldMask = 0;
    static int32_t g_PlayerStateEscapedPropertyOffset = -2;
    static uint8_t g_PlayerStateEscapedByteOffset = 0;
    static uint8_t g_PlayerStateEscapedFieldMask = 0;
    static ULONGLONG g_HumanPursuitStartedAt = 0;
    static AActor* g_VehicleInterceptCar = nullptr;
    static UObject* g_VehicleInterceptSeat = nullptr;
    static FVector g_VehicleInterceptHeading{};
    static ULONGLONG g_VehicleHeadingStableSince = 0;
    static ULONGLONG g_VehicleInterceptDeadline = 0;
    static ULONGLONG g_VehicleInterceptRetryAfter = 0;
    static AActor* g_IgnoredVehicleInterceptCar = nullptr;
    static ULONGLONG g_IgnoredVehicleInterceptUntil = 0;
    static ULONGLONG g_NextVehicleInterceptActionAt = 0;
    static ULONGLONG g_NextVehicleInterceptLogAt = 0;
    static bool g_VehicleInterceptMorphUsed = false;
    static ULONGLONG g_VehicleHoodInputSentAt = 0;
    static bool g_VehicleSlamObserved = false;
    static ULONGLONG g_VehicleExtractionInputAt = 0;
    static int32_t g_VehicleExtractionAttempts = 0;
    static bool g_VehicleDriverDetourReached = false;
    static int32_t g_VehicleDriverDetourAttempts = 0;
    static ULONGLONG g_VehicleDriverApproachStartedAt = 0;
    static ULONGLONG g_VehicleDriverLastProgressAt = 0;
    static ULONGLONG g_VehicleDriverReadySince = 0;
    static float g_VehicleDriverBestDistance = FLT_MAX;
    static bool g_JasonPursuitBoostActive = false;
    static void* g_JasonPursuitMovement = nullptr;
    static float g_JasonBaseWalkSpeed = 0.0f;
    static float g_JasonBaseSprintSpeed = 0.0f;
    static float g_JasonBaseRunSpeed = 0.0f;
    static float g_JasonBaseSlowRunSpeed = 0.0f;
    static int32_t g_PoliceArrivedPropertyOffset = -2;
    static uint8_t g_PoliceArrivedByteOffset = 0;
    static uint8_t g_PoliceArrivedFieldMask = 0;
    static AActor* g_HidingSpotTarget = nullptr;
    static AActor* g_HidingSpotCounselor = nullptr;
    static UObject* g_HidingSpotInteractable = nullptr;
    static AActor* g_IgnoredHidingSpot = nullptr;
    static AActor* g_IgnoredHidingCounselor = nullptr;
    static ULONGLONG g_HidingSpotStartedAt = 0;
    static ULONGLONG g_HidingSpotAttemptPendingUntil = 0;
    static ULONGLONG g_HidingSpotIgnoreUntil = 0;
    static ULONGLONG g_NextHidingSpotMoveAt = 0;
    static int32_t g_HidingSpotAttempts = 0;
    static bool g_HidingSpotRepositionAttempted = false;
    static ULONGLONG g_NextHidingSpotScanAt = 0;
    static ULONGLONG g_NextHidingSpotInteractAt = 0;
    static ULONGLONG g_NextHidingSpotLogAt = 0;
    static ULONGLONG g_NextWalkieCleanupAt = 0;
    static int32_t g_WalkieCleanupLevel = 0;
    static int32_t g_WalkieCleanupActor = 0;
    static int32_t g_WalkiesRemoved = 0;
    static int32_t g_TapesRemoved = 0;
    static int32_t g_InvalidPropellersRemoved = 0;
    static int32_t g_SurplusKeysRemoved = 0;
    static int32_t g_UsefulPickupsSpawned = 0;
    static bool g_LootCleanupPassActive = false;
    static bool g_LootCleanupComplete = false;
    static bool g_LootCensusSawBoat = false;
    static int32_t g_LootCensusCarCount = 0;
    static int32_t g_LootKeysKeptThisPass = 0;
    static int32_t g_LootReplacementIndex = 0;
    static UClass* g_LootReplacementClasses[3]{};
    static ULONGLONG g_NextCounselorFleeRefreshAt = 0;
    static bool g_CounselorBlackboardNameResolutionComplete = false;
    static int32_t g_CounselorBlackboardNameResolveCursor = 0;
    static int32_t g_SCWeaponNameIndex = -1;
    static int32_t g_ShouldFleeKillerNameIndex = -1;
    static int32_t g_ShouldFightBackNameIndex = -1;
    static int32_t g_ShouldArmedFightBackNameIndex = -1;
    static int32_t g_ShouldMeleeFightBackNameIndex = -1;
    static int32_t g_SeekWeaponWhileFleeingNameIndex = -1;
    static int32_t g_ShouldHideNameIndex = -1;
    static int32_t g_ShouldOrientTowardKillerNameIndex = -1;
    static int32_t g_JasonCharacterNameIndex = -1;
    static ULONGLONG g_NextCounselorTargetPruneAt = 0;
    static ULONGLONG g_NextCounselorRosterRefreshAt = 0;
    static int32_t g_CounselorRosterRefreshLevel = 0;
    static UObject* g_CounselorControllers[8]{};
    static int32_t g_CounselorControllerCount = 0;
    static bool g_CounselorConvergenceActive = false;
    static ULONGLONG g_NextCounselorConvergenceAt = 0;
    static ULONGLONG g_NextConvergenceCombatSweepAt = 0;
    static int32_t g_CounselorConvergenceCursor = 0;
    static UClass* g_CounselorConvergenceMeleeClass = nullptr;
    static UClass* g_CounselorConvergenceMacheteClass = nullptr;
    static AActor* g_ConvergenceArmedCounselors[8]{};
    static int32_t g_ConvergenceArmedCounselorCount = 0;
    static AActor* g_ConvergenceTravelCounselors[8]{};
    static int32_t g_ConvergenceTravelCounselorCount = 0;
    static FVector g_ConvergenceTravelLastLocations[8]{};
    static ULONGLONG g_ConvergenceTravelLastProgressAt[8]{};
    static ULONGLONG g_ConvergenceTravelRecoveryUntil[8]{};
    static bool g_ConvergenceTravelHaveLocation[8]{};
    enum class KillTeamRoute : uint8_t
    {
        None,
        HumanTommyFemaleHelper,
        HumanSweaterAITommy
    };
    static KillTeamRoute g_KillTeamRoute = KillTeamRoute::None;
    static AActor* g_KillTeamHelper = nullptr;
    static AActor* g_KillTeamShack = nullptr;
    static AActor* g_KillTeamSweater = nullptr;
    static AActor* g_KillTeamAxe = nullptr;
    static AActor* g_KillTeamMask = nullptr;
    static ULONGLONG g_NextKillTeamTickAt = 0;
    static ULONGLONG g_NextKillTeamDiscoveryAt = 0;
    static ULONGLONG g_NextKillTeamInteractAt = 0;
    static ULONGLONG g_NextHelperKnifeGrantAt = 0;
    static ULONGLONG g_NextSweaterUseAt = 0;
    static ULONGLONG g_NextFinalKillInteractAt = 0;
    static ULONGLONG g_NextKillTeamMoveAt = 0;
    static ULONGLONG g_NextKillTeamFollowAt = 0;
    static AActor* g_KillTeamMoveTarget = nullptr;
    static AActor* g_TommyJasonObjectiveOwner = nullptr;
    static ULONGLONG g_NextTommyJasonObjectiveRepairAt = 0;
    static ULONGLONG g_OrphanJasonStunStartedAt = 0;
    static bool g_KillTeamHelperArmed = false;
    static bool g_KillTeamHelperProtected = false;
    static bool g_KillTeamMaskAcquired = false;
    static bool g_KillTeamSweaterUseDispatched = false;
    static bool g_KillTeamAxeDiscoveryAttempted = false;
    static ULONGLONG g_KillTeamAxePursuitStartedAt = 0;
    static ULONGLONG g_KillTeamAxeLastProgressAt = 0;
    static float g_KillTeamAxeBestDistance = FLT_MAX;
    static ULONGLONG g_KillTeamHelperNativeBusyUntil = 0;
    static int32_t g_KillTeamHelperStableObservations = 0;
    static ULONGLONG g_KillTeamFinalContextUntil = 0;
    static bool g_KillTeamFinalInteractionDispatched = false;
    static bool g_KillTeamFinalInteractionPending = false;
    static int32_t g_KillTeamFinalInteractionAttempts = 0;
    static ULONGLONG g_KillTeamFinalInteractionStartedAt = 0;
    static ULONGLONG g_KillTeamFinalSequenceStartedAt = 0;
    static bool g_KillTeamFinalInteractionCommitted = false;
    static ULONGLONG g_KillTeamFinalRecoveryCooldownUntil = 0;
    static int32_t g_KillTeamFinalMoveFailures = 0;
    static bool g_KillTeamFinalRepositionAttempted = false;
    static UObject* g_KillTeamLastCancelledInteraction = nullptr;
    static ULONGLONG g_NextKillTeamInteractionCancelAt = 0;
    static AActor* g_KillTeamPendingFinalContext = nullptr;
    static UObject* g_KillTeamPendingFinalComponent = nullptr;
    static AActor* g_LastRejectedFinalContext = nullptr;
    static AActor* g_LastAcceptedFinalContext = nullptr;
    static UObject* g_LastAcceptedFinalComponent = nullptr;
    static UObject* g_LastAcceptedFinalKillComponent = nullptr;
    static AActor* g_PermanentHumanSweaterCarrier = nullptr;
    static UObject* g_PermanentHumanSweaterAbility = nullptr;
    // Captured once when the sweater is acquired. The stock HUD keeps the
    // counselor's innate ability in ActiveAbility and advertises Pamela's
    // power separately through SweaterAbility. Restoring the sweater into
    // ActiveAbility after use suppresses the Y prompt.
    static UObject* g_PermanentHumanInnateActiveAbility = nullptr;
    static bool g_PermanentHumanSweaterLatched = false;
    static bool g_PermanentHumanSweaterRestoreLogged = false;
    static bool g_PermanentHumanUnlimitedSweaterArmed = false;
    static ULONGLONG g_PermanentHumanSweaterRearmAt = 0;
    static int32_t g_PermanentHumanSweaterRearmAttempts = 0;
    static int32_t g_PermanentHumanSweaterUseCount = 0;
    static ULONGLONG g_RepeatJasonKillStanceAt = 0;
    static ULONGLONG g_RepeatJasonKillStanceDeadline = 0;
    static int32_t g_RepeatJasonKillStanceAttempts = 0;
    static bool g_UnlimitedSweaterWorldRuleLogged = false;
    static ULONGLONG g_NextFinalContextDiagnosticAt = 0;
    static ULONGLONG g_NextAITommyProtectionAt = 0;
    static AActor* g_ProtectedAITommy = nullptr;
    static ULONGLONG g_NextTommyObjectivePublishAt = 0;
    static AActor* g_TommyObjectiveRadio = nullptr;
    static int32_t g_CounselorFleeRefreshCursor = 0;
    static bool g_TommyObjectivePublished = false;
    static ULONGLONG g_FuseDiagnosticAt = 0;
    static bool g_FuseDiagnosticLogged = false;
    static int32_t g_MatchStateOffset = -1;
    static bool g_PostMatchAIRetired = false;

    bool IsValidatedLiveCounselorPawn(AActor* actor);
    bool IsHunterCounselor(AActor* counselor);
    bool HasPamelaSweater(AActor* counselor);
    bool IsJasonBridgeCombatBusy(ULONGLONG now);
    bool ReadReflectedBoolByte(UObject* object, const char* propertyName);
    bool ReadCachedReflectedBool(
        UObject* object,
        const char* propertyName,
        int32_t& cachedOffset,
        uint8_t& cachedByteOffset,
        uint8_t& cachedFieldMask);
    AActor* ReadActorField(UObject* owner, uintptr_t offset);
    UObject* GetJasonInteractionManager(AActor* jason);
    UObject* GetLockedJasonInteractable(AActor* jason);
    bool ReleaseStaleJasonHidingInteraction(
        UObject* manager,
        AActor* jason,
        ULONGLONG now,
        const char* reason);
    bool IsFinalContextLockMatch(
        UObject* locked,
        AActor* context,
        UObject* component);
    bool ClassifyRepairableCar(
        AActor* actor,
        uint8_t& outKind,
        int32_t& outSeatCount);
    bool IssueAIMoveToLocationOnGameThread(
        UObject* controller,
        const FVector& destination,
        float acceptanceRadius,
        const char* label);

    bool ObjectClassDerivesFromExact(UObject* object, const char* className)
    {
        if (!object ||
            !className ||
            !Memory::IsReadable(object, sizeof(UObject)) ||
            !object->Class)
        {
            return false;
        }

        for (UStruct* current = reinterpret_cast<UStruct*>(object->Class);
            current;
            current = SafeReadSuperStruct(current))
        {
            if (!Memory::IsReadable(current, sizeof(UStruct)))
                break;
            if (JasonAISafeName(reinterpret_cast<UObject*>(current)) == className)
                return true;
        }
        return false;
    }

    UObject* ReadReflectedObjectProperty(UObject* owner, const char* propertyName)
    {
        if (!owner ||
            !propertyName ||
            !Memory::IsReadable(owner, sizeof(UObject)) ||
            !owner->Class)
        {
            return nullptr;
        }

        UPropertyLite* property =
            FindPropertyInHierarchyByName(owner->Class, propertyName);
        if (!property ||
            property->Offset_Internal <= 0 ||
            property->Offset_Internal >= 0x10000)
        {
            return nullptr;
        }

        UObject** field = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(owner) + property->Offset_Internal);
        if (!Memory::IsReadable(field, sizeof(UObject*)) ||
            !*field ||
            !Memory::IsReadable(*field, sizeof(UObject)))
        {
            return nullptr;
        }
        return *field;
    }

    int32_t FindFNameIndexExact(const char* wanted)
    {
        if (!wanted || !GNames ||
            !Memory::IsReadable(GNames, sizeof(TNameEntryArray)) ||
            GNames->NumElements <= 0 ||
            GNames->NumElements > 4000000)
        {
            return -1;
        }

        for (int32_t index = 0; index < GNames->NumElements; ++index)
        {
            const FNameEntry* entry = GNames->GetById(index);
            if (entry &&
                Memory::IsReadable(entry, 0x20) &&
                std::strcmp(entry->AnsiName, wanted) == 0)
            {
                return index;
            }
        }
        return -1;
    }

    void ResolveCounselorBlackboardNameIndices()
    {
        if (g_CounselorBlackboardNameResolutionComplete ||
            (g_SCWeaponNameIndex >= 0 &&
            g_ShouldFleeKillerNameIndex >= 0 &&
            g_ShouldFightBackNameIndex >= 0 &&
            g_ShouldArmedFightBackNameIndex >= 0 &&
            g_ShouldMeleeFightBackNameIndex >= 0 &&
            g_SeekWeaponWhileFleeingNameIndex >= 0 &&
            g_ShouldHideNameIndex >= 0 &&
            g_ShouldOrientTowardKillerNameIndex >= 0 &&
            g_JasonCharacterNameIndex >= 0))
        {
            g_CounselorBlackboardNameResolutionComplete = true;
            return;
        }

        if (!GNames ||
            !Memory::IsReadable(GNames, sizeof(TNameEntryArray)) ||
            GNames->NumElements <= 0 ||
            GNames->NumElements > 4000000)
        {
            return;
        }

        // Never scan the complete multi-million-entry name table on a game
        // frame. Advance a very small bounded chunk and stop permanently after
        // the keys or current table end. The caller schedules incomplete
        // discovery frequently so this finishes without a visible burst.
        constexpr int32_t NamesPerMaintenancePass = 512;
        const int32_t end = (std::min)(
            GNames->NumElements,
            g_CounselorBlackboardNameResolveCursor +
                NamesPerMaintenancePass);
        for (int32_t index = g_CounselorBlackboardNameResolveCursor;
             index < end;
             ++index)
        {
            const FNameEntry* entry = GNames->GetById(index);
            if (!entry || !Memory::IsReadable(entry, 0x20))
                continue;
            const char* name = entry->AnsiName;
            if (g_SCWeaponNameIndex < 0 &&
                std::strcmp(name, "SCWeapon") == 0)
                g_SCWeaponNameIndex = index;
            else if (g_ShouldFleeKillerNameIndex < 0 &&
                std::strcmp(name, "ShouldFleeKiller") == 0)
                g_ShouldFleeKillerNameIndex = index;
            else if (g_ShouldFightBackNameIndex < 0 &&
                std::strcmp(name, "ShouldFightBack") == 0)
                g_ShouldFightBackNameIndex = index;
            else if (g_ShouldArmedFightBackNameIndex < 0 &&
                std::strcmp(name, "ShouldArmedFightBack") == 0)
                g_ShouldArmedFightBackNameIndex = index;
            else if (g_ShouldMeleeFightBackNameIndex < 0 &&
                std::strcmp(name, "ShouldMeleeFightBack") == 0)
                g_ShouldMeleeFightBackNameIndex = index;
            else if (g_SeekWeaponWhileFleeingNameIndex < 0 &&
                std::strcmp(name, "SeekWeaponWhileFleeing") == 0)
                g_SeekWeaponWhileFleeingNameIndex = index;
            else if (g_ShouldHideNameIndex < 0 &&
                std::strcmp(name, "ShouldHide") == 0)
                g_ShouldHideNameIndex = index;
            else if (g_ShouldOrientTowardKillerNameIndex < 0 &&
                std::strcmp(name, "ShouldOrientTowardKiller") == 0)
                g_ShouldOrientTowardKillerNameIndex = index;
            else if (g_JasonCharacterNameIndex < 0 &&
                std::strcmp(name, "JasonCharacter") == 0)
                g_JasonCharacterNameIndex = index;

            if (g_SCWeaponNameIndex >= 0 &&
                g_ShouldFleeKillerNameIndex >= 0 &&
                g_ShouldFightBackNameIndex >= 0 &&
                g_ShouldArmedFightBackNameIndex >= 0 &&
                g_ShouldMeleeFightBackNameIndex >= 0 &&
                g_SeekWeaponWhileFleeingNameIndex >= 0 &&
                g_ShouldHideNameIndex >= 0 &&
                g_ShouldOrientTowardKillerNameIndex >= 0 &&
                g_JasonCharacterNameIndex >= 0)
            {
                g_CounselorBlackboardNameResolveCursor = index + 1;
                g_CounselorBlackboardNameResolutionComplete = true;
                break;
            }
        }
        g_CounselorBlackboardNameResolveCursor = end;
        if (end >= GNames->NumElements)
            g_CounselorBlackboardNameResolutionComplete = true;
    }

    bool SetBlackboardBool(UObject* blackboard, int32_t keyIndex, bool value)
    {
        if (!blackboard ||
            keyIndex < 0 ||
            !Memory::IsReadable(blackboard, sizeof(UObject)) ||
            !blackboard->Class)
        {
            return false;
        }

        // All counselor blackboards share a class. Cache both successful and
        // failed reflection lookups so the one-second kill-team path never
        // re-walks the UFunction hierarchy.
        static UClass* cachedClass = nullptr;
        static UFunction* cachedFunction = nullptr;
        if (cachedClass != blackboard->Class)
        {
            cachedClass = blackboard->Class;
            cachedFunction = FindFunctionInHierarchyByName(
                blackboard->Class,
                "SetValueAsBool");
        }
        UFunction* function = cachedFunction;
        if (!function)
            return false;

        struct Params
        {
            FName KeyName;
            bool BoolValue;
            uint8_t Padding[3];
        };
        Params params{};
        params.KeyName = FName(keyIndex, 0);
        params.BoolValue = value;
        return SafeProcessEventCall(
            reinterpret_cast<uintptr_t>(blackboard),
            blackboard,
            function,
            &params);
    }

    bool SetBlackboardObject(
        UObject* blackboard,
        int32_t keyIndex,
        UObject* value)
    {
        if (!blackboard ||
            keyIndex < 0 ||
            !value ||
            !Memory::IsReadable(blackboard, sizeof(UObject)) ||
            !blackboard->Class ||
            !Memory::IsReadable(value, sizeof(UObject)))
        {
            return false;
        }

        static UClass* cachedClass = nullptr;
        static UFunction* cachedFunction = nullptr;
        if (cachedClass != blackboard->Class)
        {
            cachedClass = blackboard->Class;
            cachedFunction = FindFunctionInHierarchyByName(
                blackboard->Class,
                "SetValueAsObject");
        }
        UFunction* function = cachedFunction;
        if (!function)
            return false;

        struct Params
        {
            FName KeyName;
            UObject* ObjectValue;
        };
        Params params{};
        params.KeyName = FName(keyIndex, 0);
        params.ObjectValue = value;
        return SafeProcessEventCall(
            reinterpret_cast<uintptr_t>(blackboard),
            blackboard,
            function,
            &params);
    }

    UObject* GetBlackboardObject(UObject* blackboard, int32_t keyIndex)
    {
        if (!blackboard ||
            keyIndex < 0 ||
            !Memory::IsReadable(blackboard, sizeof(UObject)) ||
            !blackboard->Class)
        {
            return nullptr;
        }

        static UClass* cachedClass = nullptr;
        static UFunction* cachedFunction = nullptr;
        if (cachedClass != blackboard->Class)
        {
            cachedClass = blackboard->Class;
            cachedFunction = FindFunctionInHierarchyByName(
                blackboard->Class,
                "GetValueAsObject");
        }
        UFunction* function = cachedFunction;
        if (!function)
            return nullptr;

        struct Params
        {
            FName KeyName;
            UObject* ReturnValue;
        };
        Params params{};
        params.KeyName = FName(keyIndex, 0);
        if (!SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(blackboard),
                blackboard,
                function,
                &params))
        {
            return nullptr;
        }
        return params.ReturnValue &&
            Memory::IsReadable(params.ReturnValue, sizeof(UObject))
                ? params.ReturnValue
                : nullptr;
    }

    enum class CounselorRouteMatchPhase : uint8_t
    {
        Unknown,
        InProgress,
        NotInProgress
    };

    CounselorRouteMatchPhase ReadCounselorRouteMatchPhase()
    {
        UWorld* world = Engine::GetWorld();
        if (!world ||
            world != g_JasonAIState.World ||
            !Memory::IsReadable(world, sizeof(UWorld)))
        {
            return CounselorRouteMatchPhase::NotInProgress;
        }

        AActor* gameState = ReadActorField(
            reinterpret_cast<UObject*>(world),
            0xF8);
        if (!gameState ||
            !Memory::IsReadable(gameState, sizeof(UObject)) ||
            !gameState->Class ||
            !Memory::IsReadable(gameState->Class, sizeof(UClass)))
        {
            return CounselorRouteMatchPhase::Unknown;
        }

        if (g_MatchStateOffset < 0)
        {
            UPropertyLite* property =
                FindPropertyInHierarchyByName(
                    gameState->Class,
                    "MatchState");
            if (!property ||
                property->Offset_Internal <= 0 ||
                property->Offset_Internal >= 0x10000 ||
                property->ElementSize != sizeof(FName))
            {
                return CounselorRouteMatchPhase::Unknown;
            }
            g_MatchStateOffset = property->Offset_Internal;
        }

        const FName* matchState = reinterpret_cast<const FName*>(
            reinterpret_cast<uintptr_t>(gameState) + g_MatchStateOffset);
        if (!Memory::IsReadable(matchState, sizeof(FName)) ||
            !GNames ||
            !GNames->IsValidIndex(matchState->ComparisonIndex))
        {
            return CounselorRouteMatchPhase::Unknown;
        }

        const FNameEntry* entry =
            GNames->GetById(matchState->ComparisonIndex);
        if (!entry || !Memory::IsReadable(entry, 0x20))
            return CounselorRouteMatchPhase::Unknown;

        return std::strcmp(entry->AnsiName, "InProgress") == 0
            ? CounselorRouteMatchPhase::InProgress
            : CounselorRouteMatchPhase::NotInProgress;
    }

    void ResetVehicleInterceptionState(ULONGLONG retryAfter = 0)
    {
        g_VehicleInterceptCar = nullptr;
        g_VehicleInterceptSeat = nullptr;
        g_VehicleInterceptHeading = FVector{};
        g_VehicleHeadingStableSince = 0;
        g_VehicleInterceptDeadline = 0;
        g_VehicleInterceptRetryAfter = retryAfter;
        // A nonzero retry deadline is also the next allowed discovery pass.
        // Do not erase it and accidentally turn idle vehicle inspection into
        // presentation-frame work.
        g_NextVehicleInterceptActionAt = retryAfter;
        g_NextVehicleInterceptLogAt = 0;
        g_VehicleInterceptMorphUsed = false;
        g_VehicleHoodInputSentAt = 0;
        g_VehicleSlamObserved = false;
        g_VehicleExtractionInputAt = 0;
        g_VehicleExtractionAttempts = 0;
        g_VehicleDriverDetourReached = false;
        g_VehicleDriverDetourAttempts = 0;
        g_VehicleDriverApproachStartedAt = 0;
        g_VehicleDriverLastProgressAt = 0;
        g_VehicleDriverReadySince = 0;
        g_VehicleDriverBestDistance = FLT_MAX;
    }

    bool HasPoliceArrived()
    {
        UObject* gameState = reinterpret_cast<UObject*>(
            g_JasonAIState.StartupTrapGameState);
        return gameState && ReadCachedReflectedBool(
            gameState,
            "bHasPoliceArrived",
            g_PoliceArrivedPropertyOffset,
            g_PoliceArrivedByteOffset,
            g_PoliceArrivedFieldMask);
    }

    void SetJasonHighPriorityPursuitBoost(
        bool enabled,
        const char* reason,
        float speedMultiplier = 2.0f,
        ULONGLONG morphCooldownMs = 10000ULL)
    {
        AActor* jason = g_JasonAIState.Jason;
        void* movement = nullptr;
        if (jason && Memory::IsReadable(jason, 0x3D8))
            movement = reinterpret_cast<ACharacter*>(jason)->CharacterMovement;

        auto validSpeed = [](float value)
        {
            return std::isfinite(value) && value > 25.0f && value < 5000.0f;
        };

        if (enabled && movement &&
            Memory::IsReadable(movement, sizeof(UCharacterMovementComponent)))
        {
            auto* move = reinterpret_cast<UCharacterMovementComponent*>(movement);
            if (movement != g_JasonPursuitMovement ||
                !validSpeed(g_JasonBaseWalkSpeed))
            {
                if (!validSpeed(move->MaxWalkSpeed) ||
                    !validSpeed(move->MaxSprintSpeed) ||
                    !validSpeed(move->MaxRunSpeed) ||
                    !validSpeed(move->MaxSlowRunSpeed))
                {
                    return;
                }
                g_JasonPursuitMovement = movement;
                g_JasonBaseWalkSpeed = move->MaxWalkSpeed;
                g_JasonBaseSprintSpeed = move->MaxSprintSpeed;
                g_JasonBaseRunSpeed = move->MaxRunSpeed;
                g_JasonBaseSlowRunSpeed = move->MaxSlowRunSpeed;
            }
            const float effectiveMultiplier =
                std::isfinite(speedMultiplier) && speedMultiplier > 0.0f
                    ? speedMultiplier
                    : 2.0f;
            const ULONGLONG effectiveCooldown =
                morphCooldownMs >= 5000ULL ? morphCooldownMs : 5000ULL;
            move->MaxWalkSpeed = g_JasonBaseWalkSpeed * effectiveMultiplier;
            move->MaxSprintSpeed = g_JasonBaseSprintSpeed * effectiveMultiplier;
            move->MaxRunSpeed = g_JasonBaseRunSpeed * effectiveMultiplier;
            move->MaxSlowRunSpeed = g_JasonBaseSlowRunSpeed * effectiveMultiplier;
            JasonAIMorphCooldownMs = effectiveCooldown;
            if (!g_JasonPursuitBoostActive)
            {
                g_JasonPursuitBoostActive = true;
                Logger::Success(std::string(
                    "18L-BJ Jason high-priority pursuit boost engaged | speed=") +
                    std::to_string(effectiveMultiplier) +
                    "x | morphCooldownMs=" +
                    std::to_string(effectiveCooldown) +
                    " | reason=" +
                    (reason ? reason : "priority"));
            }
            return;
        }

        if (g_JasonPursuitMovement &&
            Memory::IsReadable(
                g_JasonPursuitMovement,
                sizeof(UCharacterMovementComponent)))
        {
            auto* move = reinterpret_cast<UCharacterMovementComponent*>(
                g_JasonPursuitMovement);
            if (validSpeed(g_JasonBaseWalkSpeed))
                move->MaxWalkSpeed = g_JasonBaseWalkSpeed;
            if (validSpeed(g_JasonBaseSprintSpeed))
                move->MaxSprintSpeed = g_JasonBaseSprintSpeed;
            if (validSpeed(g_JasonBaseRunSpeed))
                move->MaxRunSpeed = g_JasonBaseRunSpeed;
            if (validSpeed(g_JasonBaseSlowRunSpeed))
                move->MaxSlowRunSpeed = g_JasonBaseSlowRunSpeed;
        }
        JasonAIMorphCooldownMs = 20000ULL;
        if (g_JasonPursuitBoostActive)
        {
            g_JasonPursuitBoostActive = false;
            Logger::Debug(
                "18L-BJ Jason high-priority pursuit boost retired; stock movement and 20s Morph restored");
        }
    }

    __declspec(noinline) AActor* SafeGetGrabbedCounselor(
        AActor* jason,
        uintptr_t functionAddress)
    {
        if (!jason ||
            !functionAddress ||
            !Memory::IsReadable(
                reinterpret_cast<void*>(functionAddress), 1))
        {
            return nullptr;
        }

        __try
        {
            using Function = AActor* (__fastcall*)(AActor*);
            return reinterpret_cast<Function>(functionAddress)(jason);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    __declspec(noinline) bool SafeJasonBoolCall(
        AActor* jason,
        uintptr_t functionAddress)
    {
        if (!jason ||
            !functionAddress ||
            !Memory::IsReadable(
                reinterpret_cast<void*>(functionAddress), 1))
        {
            return false;
        }

        __try
        {
            using Function = bool(__fastcall*)(AActor*);
            return reinterpret_cast<Function>(functionAddress)(jason);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    __declspec(noinline) bool SafeIsGrabKillAvailable(
        AActor* jason,
        uintptr_t functionAddress,
        int32_t nativeSlot)
    {
        if (!jason ||
            !functionAddress ||
            nativeSlot < 0 ||
            nativeSlot > 3 ||
            !Memory::IsReadable(
                reinterpret_cast<void*>(functionAddress), 1))
        {
            return false;
        }

        __try
        {
            using Function = bool(__fastcall*)(AActor*, int32_t);
            return reinterpret_cast<Function>(functionAddress)(
                jason,
                nativeSlot);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    __declspec(noinline) bool SafeJasonGrabKillInput(
        AActor* jason,
        uintptr_t functionAddress,
        int32_t nativeSlot)
    {
        if (!jason ||
            !functionAddress ||
            nativeSlot < 0 ||
            nativeSlot > 3 ||
            !Memory::IsReadable(
                reinterpret_cast<void*>(functionAddress), 1))
        {
            return false;
        }

        __try
        {
            using Function = void(__fastcall*)(AActor*, int32_t);
            reinterpret_cast<Function>(functionAddress)(jason, nativeSlot);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void ResetStuckSamplingAfterNativeInteraction(ULONGLONG now)
    {
        g_JasonAIState.ConsecutiveStuckChecks = 0;
        g_JasonAIState.LastAcceptedMoveAt = 0;
        g_JasonAIState.HaveLastLocation = false;
        g_JasonAIState.PathLocked = false;
        g_JasonAIState.PathLockUntil = 0;
        g_JasonAIState.NextStuckCheckAt = now + 1375;
        g_JasonAIState.NoPathTarget = nullptr;
        g_JasonAIState.ConsecutiveNoPathTargets = 0;
    }

    void LogRuntimeGrabKillSlots(AActor* jason)
    {
        if (!jason || g_GrabKillSlotsLoggedFor == jason)
            return;

        g_GrabKillSlotsLoggedFor = jason;
        UObject** grabKills = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(jason) + 0x11C0);
        if (!Memory::IsReadable(grabKills, sizeof(UObject*) * 4))
        {
            Logger::Debug(
                "18L-AI grab kill: runtime slot array unreadable");
            return;
        }

        for (int32_t slot = 0; slot < 4; ++slot)
        {
            UObject* grabKill = grabKills[slot];
            Logger::Debug(
                "18L-AI grab kill slot[" + std::to_string(slot) + "]=" +
                (grabKill && Memory::IsReadable(grabKill, sizeof(UObject))
                    ? JasonAISafeName(grabKill)
                    : std::string("null")));
        }
    }

    bool DriveHeldCounselorGrabKill(ULONGLONG now)
    {
        AActor* jason = g_JasonAIState.Jason;
        HMODULE module = GetModuleHandle(nullptr);
        if (!jason || !module)
            return false;

        constexpr uintptr_t RVA_GetGrabbedCounselor = 0x004055E0;
        constexpr uintptr_t RVA_CanGrabKill = 0x003FDB40;
        constexpr uintptr_t RVA_IsGrabKillAvailable = 0x0040D070;
        constexpr uintptr_t RVA_IsGrabKilling = 0x0040D1A0;
        constexpr uintptr_t RVA_GrabKillInput = 0x0041B040;

        const uintptr_t base = reinterpret_cast<uintptr_t>(module);
        AActor* heldCounselor = SafeGetGrabbedCounselor(
            jason,
            base + RVA_GetGrabbedCounselor);
        if (!heldCounselor)
        {
            // The mature combat driver treats a successful native function
            // call as a possible grab and schedules a follow-up input. The
            // call itself does not guarantee that Jason actually acquired a
            // counselor. Clear that speculative timer once it matures unless
            // GetGrabbedCounselor confirms the paired native state; otherwise
            // Jason can repeatedly send grab-kill input while merely following
            // a target and appear frozen.
            if (g_JasonAIState.GrabKillReadyAt != 0 &&
                now >= g_JasonAIState.GrabKillReadyAt)
            {
                g_JasonAIState.GrabKillReadyAt = 0;
            }
            if (g_LastHeldCounselor)
            {
                if (g_LastHeldCounselor == g_KillTeamHelper ||
                    g_LastHeldCounselor == g_ProtectedAITommy)
                {
                    g_KillTeamHelperNativeBusyUntil = now + 3500;
                    g_KillTeamHelperStableObservations = 0;
                    // Recheck the pocket knife once the native grab/escape
                    // transition is quiet instead of polling inventory on a
                    // short global cadence.
                    g_NextAITommyProtectionAt = now + 3550;
                }
                Logger::Debug(
                    "18L-AI grab kill: counselor released; chase state reset");
                ResetStuckSamplingAfterNativeInteraction(now);
            }
            g_LastHeldCounselor = nullptr;
            g_HeldCounselorObservedAt = 0;
            g_NextAdapterGrabKillAttemptAt = 0;
            return false;
        }

        ResetStuckSamplingAfterNativeInteraction(now);
        g_JasonAIState.GrabKillReadyAt = 0;

        if (heldCounselor == g_KillTeamHelper ||
            heldCounselor == g_ProtectedAITommy)
        {
            g_KillTeamHelperNativeBusyUntil = now + 3500;
            g_KillTeamHelperStableObservations = 0;
        }

        if (g_LastHeldCounselor != heldCounselor)
        {
            g_LastHeldCounselor = heldCounselor;
            g_HeldCounselorObservedAt = now;
            g_NextAdapterGrabKillAttemptAt = now + 100;
            g_NextGrabKillStateLogAt = 0;
            Logger::Success(
                "18L-AI grab kill: held counselor detected=" +
                JasonAISafeName(
                    reinterpret_cast<UObject*>(heldCounselor)));
            LogRuntimeGrabKillSlots(jason);
        }

        if (SafeJasonBoolCall(jason, base + RVA_IsGrabKilling))
        {
            g_NextAdapterGrabKillAttemptAt = now + 200;
            return true;
        }

        if (now < g_NextAdapterGrabKillAttemptAt)
            return true;

        g_NextAdapterGrabKillAttemptAt = now + 175;
        if (!SafeJasonBoolCall(jason, base + RVA_CanGrabKill))
        {
            // A pocket-knife escape normally clears GetGrabbedCounselor on
            // its own.  If the native kill predicate stays false for several
            // seconds, however, the stock interaction can strand Jason in a
            // permanent hold after a failed final-kill attempt.  Release only
            // this stale non-final grab through the native interaction manager
            // and return to ordinary AI; never interfere with a live
            // JasonDeath context or an active grab-kill montage.
            if (g_HeldCounselorObservedAt != 0 &&
                now >= g_HeldCounselorObservedAt + 6000 &&
                now >= g_KillTeamFinalContextUntil &&
                !g_KillTeamFinalInteractionCommitted)
            {
                UObject* manager = GetJasonInteractionManager(jason);
                if (manager && ReleaseStaleJasonHidingInteraction(
                    manager,
                    jason,
                    now,
                    "stale-grab-kill"))
                {
                    Logger::Error(
                        "18L-AI grab kill: stale CanGrabKill=false hold released after bounded timeout");
                    g_LastHeldCounselor = nullptr;
                    g_HeldCounselorObservedAt = 0;
                    g_NextAdapterGrabKillAttemptAt = 0;
                    return false;
                }
            }
            if (now >= g_NextGrabKillStateLogAt)
            {
                g_NextGrabKillStateLogAt = now + 1000;
                Logger::Debug(
                    "18L-AI grab kill: native CanGrabKill=false; preserving pocket-knife escape/state transition");
            }
            return true;
        }

        int32_t selectedSlot = -1;
        uint8_t availableMask = 0;
        for (int32_t offset = 0; offset < 4; ++offset)
        {
            const int32_t slot =
                (g_NextAdapterGrabKillSlot + offset) & 3;
            if (SafeIsGrabKillAvailable(
                jason,
                base + RVA_IsGrabKillAvailable,
                slot))
            {
                availableMask |= static_cast<uint8_t>(1u << slot);
                if (selectedSlot < 0)
                    selectedSlot = slot;
            }
        }

        if (selectedSlot < 0)
        {
            if (now >= g_NextGrabKillStateLogAt)
            {
                g_NextGrabKillStateLogAt = now + 1000;
                Logger::Debug(
                    "18L-AI grab kill: no usable runtime slot | availableMask=" +
                    std::to_string(availableMask));
            }
            return true;
        }

        const bool inputDispatched = SafeJasonGrabKillInput(
            jason,
            base + RVA_GrabKillInput,
            selectedSlot);
        g_NextAdapterGrabKillSlot = (selectedSlot + 1) & 3;
        g_NextAdapterGrabKillAttemptAt = now + 350;
        g_JasonAIState.NextCombatAttemptAt = now + 700;
        Logger::Success(
            "18L-AI grab kill: selected native-available slot=" +
            std::to_string(selectedSlot) +
            " | availableMask=" + std::to_string(availableMask) +
            " | dispatch=" + (inputDispatched ? "true" : "false"));
        return true;
    }

    void ApplyHumanPursuitBalance(ULONGLONG now)
    {
        if (!g_LocalCounselorTarget ||
            g_JasonAIState.Target != g_LocalCounselorTarget ||
            !IsValidatedLiveCounselorPawn(g_LocalCounselorTarget))
        {
            g_HumanPursuitStartedAt = 0;
            return;
        }

        if (g_HumanPursuitStartedAt == 0)
        {
            g_HumanPursuitStartedAt = now;
            return;
        }

        // Preserve close combat and explicit trapped-counselor priority.  If
        // Jason has pursued the human for fifteen seconds without engaging,
        // give a live bot a ten-second pursuit window before reconsidering the
        // player.  This is a target-selection pause, never a teleport.
        if (now - g_HumanPursuitStartedAt < 15000 ||
            g_TrapPriorityVictim ||
            IsJasonBridgeCombatBusy(now) ||
            !HasAlternativeUsableJasonAITarget(g_LocalCounselorTarget))
        {
            return;
        }

        const int32_t humanIndex =
            FindJasonAITargetIndex(g_LocalCounselorTarget);
        if (humanIndex >= 0)
        {
            g_JasonAITargetBlockedUntil[humanIndex] = now + 10000;
            g_JasonAIState.Target = nullptr;
            Logger::Success(
                "18L-AI counselor bridge: rotating pursuit from human to live bot for 10s");
        }
        g_HumanPursuitStartedAt = 0;
    }

    void ExtendTraversalGraceForActiveDoor(ULONGLONG now)
    {
        if (!g_JasonAIState.DoorBreakActive ||
            !g_JasonAIState.DoorBreakTarget)
        {
            return;
        }

        // Frozen code granted post-break traversal grace only to doors whose
        // asset name contained "Interior". Exterior cabin doors exhibit the
        // same threshold pause. Preserve MoveTo and suppress counselor-ring
        // recovery long enough for either kind to clear its doorway.
        const ULONGLONG graceUntil = now + 8000;
        if (g_JasonAIState.DoorTraversalGraceUntil < graceUntil)
            g_JasonAIState.DoorTraversalGraceUntil = graceUntil;
        g_JasonAIState.DoorRetryTarget =
            g_JasonAIState.DoorBreakTarget;
        if (g_JasonAIState.DoorRetryAfter < now + 2500)
            g_JasonAIState.DoorRetryAfter = now + 2500;

        if (g_LastExtendedDoorTarget != g_JasonAIState.DoorBreakTarget)
        {
            g_LastExtendedDoorTarget = g_JasonAIState.DoorBreakTarget;
            Logger::Success(
                "18L-AI counselor bridge: all-door traversal grace armed | door=" +
                JasonAISafeName(reinterpret_cast<UObject*>(
                    g_JasonAIState.DoorBreakTarget)));
        }
    }

    bool ReadCachedReflectedBool(
        UObject* object,
        const char* propertyName,
        int32_t& cachedOffset,
        uint8_t& cachedByteOffset,
        uint8_t& cachedFieldMask)
    {
        if (!object || !object->Class || !propertyName ||
            !Memory::IsReadable(object, sizeof(UObject)))
        {
            return false;
        }

        if (cachedOffset == -2)
        {
            cachedOffset = -1;
            UPropertyLite* property = FindPropertyInHierarchyByName(
                object->Class,
                propertyName);
            if (property &&
                property->Offset_Internal > 0 &&
                property->Offset_Internal < 0x10000)
            {
                cachedOffset = property->Offset_Internal;
                cachedByteOffset = 0;
                cachedFieldMask = 0xFF;
                const std::string propertyType = JasonAISafeName(
                    reinterpret_cast<UObject*>(property->ClassPrivate));
                if (propertyType == "BoolProperty")
                {
                    uint8_t* boolLayout =
                        reinterpret_cast<uint8_t*>(property) + 0x70;
                    if (Memory::IsReadable(boolLayout, 4))
                    {
                        cachedByteOffset = boolLayout[1];
                        cachedFieldMask = boolLayout[3]
                            ? boolLayout[3]
                            : boolLayout[2];
                    }
                }
            }
        }

        if (cachedOffset < 0 || cachedFieldMask == 0)
            return false;
        uint8_t* value = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(object) +
            static_cast<uintptr_t>(cachedOffset) +
            cachedByteOffset);
        return Memory::IsReadable(value, 1) &&
            (*value & cachedFieldMask) != 0;
    }

    bool HasCounselorEscaped(AActor* actor)
    {
        if (!actor || !Memory::IsReadable(actor, 0x14EE))
            return false;

        // Resolve the reflected field once and thereafter perform only a
        // masked byte read. Calling the Blueprint HasEscaped getter from the
        // AI Tick path caused a game-thread slowdown even at a 250-ms cadence.
        UObject* actorObject = reinterpret_cast<UObject*>(actor);
        if (ReadCachedReflectedBool(
                actorObject,
                "bHasEscaped",
                g_CounselorEscapedPropertyOffset,
                g_CounselorEscapedByteOffset,
                g_CounselorEscapedFieldMask))
        {
            return true;
        }

        // Both the pawn and its PlayerState carry an escape flag. Check both
        // because the pawn flag can lag by a frame during vehicle exits.
        if (g_CounselorEscapedPropertyOffset < 0)
        {
            uint8_t* pawnEscaped = reinterpret_cast<uint8_t*>(
                reinterpret_cast<uintptr_t>(actor) + 0x14ED);
            if (Memory::IsReadable(pawnEscaped, 1) && *pawnEscaped != 0)
                return true;
        }

        UObject** playerState = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(actor) + 0x388);
        if (!Memory::IsReadable(playerState, sizeof(UObject*)) ||
            !*playerState ||
            !Memory::IsReadable(*playerState, 0x6E8))
        {
            return false;
        }

        if (ReadCachedReflectedBool(
                *playerState,
                "bEscaped",
                g_PlayerStateEscapedPropertyOffset,
                g_PlayerStateEscapedByteOffset,
                g_PlayerStateEscapedFieldMask))
        {
            return true;
        }

        if (g_PlayerStateEscapedPropertyOffset >= 0)
            return false;
        uint8_t* stateEscaped = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(*playerState) + 0x6E7);
        return Memory::IsReadable(stateEscaped, 1) && *stateEscaped != 0;
    }

    void RemoveCounselorTarget(AActor* target)
    {
        if (!target)
            return;

        const int32_t index = FindJasonAITargetIndex(target);
        if (index >= 0)
        {
            for (int32_t i = index; i + 1 < g_JasonAITargetCount; ++i)
            {
                g_JasonAITargets[i] = g_JasonAITargets[i + 1];
                g_JasonAITargetBlockedUntil[i] =
                    g_JasonAITargetBlockedUntil[i + 1];
                g_JasonAITargetBlockStrikes[i] =
                    g_JasonAITargetBlockStrikes[i + 1];
            }
            if (g_JasonAITargetCount > 0)
                --g_JasonAITargetCount;
            g_JasonAITargets[g_JasonAITargetCount] = nullptr;
            g_JasonAITargetBlockedUntil[g_JasonAITargetCount] = 0;
            g_JasonAITargetBlockStrikes[g_JasonAITargetCount] = 0;
        }

        if (g_JasonAIState.Target == target)
            g_JasonAIState.Target = nullptr;
        if (g_TrapPriorityVictim == target)
        {
            g_TrapPriorityVictim = nullptr;
            g_TrapPriorityUntil = 0;
            g_TrapPriorityMorphCompleted = false;
        }
    }

    void RefreshLocalCounselorTarget(ULONGLONG now)
    {
        if (now < g_NextLocalCounselorRefreshAt)
            return;
        g_NextLocalCounselorRefreshAt = now + 250;

        constexpr uintptr_t Offset_ControllerPawn = 0x370;
        if (!g_LocalPlayerController ||
            !Memory::IsReadable(g_LocalPlayerController, sizeof(UObject)))
        {
            return;
        }

        AActor** pawnField = reinterpret_cast<AActor**>(
            reinterpret_cast<uintptr_t>(g_LocalPlayerController) +
            Offset_ControllerPawn);
        if (!Memory::IsReadable(pawnField, sizeof(AActor*)))
            return;

        AActor* possessed = *pawnField;
        AActor* previous = g_LocalCounselorTarget;
        const bool previousEscaped = previous && HasCounselorEscaped(previous);
        if (previous &&
            (previousEscaped || possessed != previous ||
             !IsValidatedLiveCounselorPawn(previous)))
        {
            RemoveCounselorTarget(previous);
            g_LocalCounselorTarget = nullptr;
            g_HumanPursuitStartedAt = 0;
            Logger::Success(
                std::string("18L-AQ local counselor retired from Jason targets | reason=") +
                (previousEscaped ? "escaped" : "possession-changed"));
        }

        if (possessed &&
            possessed != g_LocalCounselorTarget &&
            IsValidatedLiveCounselorPawn(possessed))
        {
            g_LocalCounselorTarget = possessed;
            RegisterJasonAITarget(possessed);
            g_NextKillTeamTickAt = 0;
            Logger::Success(
                "18L-AQ local counselor possession refreshed | pawn=" +
                JasonAISafeName(reinterpret_cast<UObject*>(possessed)));
        }
    }

    bool IsValidatedLiveCounselorPawn(AActor* actor)
    {
        if (!actor ||
            !Memory::IsReadable(actor, sizeof(UObject)) ||
            !JasonAIObjectDerivesFromNameContaining(
                reinterpret_cast<UObject*>(actor),
                "SCCounselorCharacter"))
        {
            return false;
        }

        // Require a live pawn/controller pair in both directions.  The old
        // name-only scan admitted counselor spawn actors, preview actors, and
        // AI-controller objects into the fixed eight-entry target registry.
        constexpr uintptr_t Offset_PawnController = 0x3A0;
        constexpr uintptr_t Offset_ControllerPawn = 0x370;
        constexpr uintptr_t Offset_CounselorDead = 0x1031;

        float* health = reinterpret_cast<float*>(
            reinterpret_cast<uintptr_t>(actor) +
            Offsets::ASCCharacter_Health);
        if (!Memory::IsReadable(health, sizeof(float)) ||
            !std::isfinite(*health) || *health <= 0.0f)
        {
            return false;
        }

        UObject** controllerField = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(actor) + Offset_PawnController);
        uint8_t* deadField = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(actor) + Offset_CounselorDead);
        if (!Memory::IsReadable(controllerField, sizeof(UObject*)) ||
            !*controllerField ||
            !Memory::IsReadable(*controllerField, sizeof(UObject)) ||
            !Memory::IsReadable(deadField, 1) ||
            *deadField != 0 ||
            HasCounselorEscaped(actor))
        {
            return false;
        }

        AActor** possessedPawnField = reinterpret_cast<AActor**>(
            reinterpret_cast<uintptr_t>(*controllerField) +
            Offset_ControllerPawn);
        return Memory::IsReadable(possessedPawnField, sizeof(AActor*)) &&
            *possessedPawnField == actor;
    }

    void TrackCounselorController(AActor* counselor)
    {
        if (!counselor ||
            !Memory::IsReadable(counselor, 0x3A8))
        {
            return;
        }

        UObject** controllerField = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(counselor) + 0x3A0);
        if (!Memory::IsReadable(controllerField, sizeof(UObject*)) ||
            !*controllerField ||
            !Memory::IsReadable(*controllerField, sizeof(UObject)))
        {
            return;
        }

        for (int32_t i = 0; i < g_CounselorControllerCount; ++i)
        {
            if (g_CounselorControllers[i] == *controllerField)
                return;
        }
        if (g_CounselorControllerCount < 8)
            g_CounselorControllers[g_CounselorControllerCount++] =
                *controllerField;
    }

    void RegisterRespawnedCounselorsFromKnownControllers()
    {
        constexpr uintptr_t Offset_ControllerPawn = 0x370;
        for (int32_t i = 0; i < g_CounselorControllerCount; ++i)
        {
            UObject* controller = g_CounselorControllers[i];
            if (!controller ||
                !Memory::IsReadable(controller, sizeof(UObject)))
            {
                continue;
            }

            AActor** pawnField = reinterpret_cast<AActor**>(
                reinterpret_cast<uintptr_t>(controller) +
                Offset_ControllerPawn);
            if (!Memory::IsReadable(pawnField, sizeof(AActor*)) ||
                !IsValidatedLiveCounselorPawn(*pawnField))
            {
                continue;
            }

            // Returned counselors, including Tommy, use the same ordinary
            // target/flee behavior.  There is no protected Tommy role in the
            // proximity-only final-kill design.
            RegisterJasonAITarget(*pawnField);
        }
    }

    void PruneAndRefreshCounselorTargets(UWorld* world, ULONGLONG now)
    {
        // Pruning the fixed eight-entry registry is cheap and keeps dead,
        // escaped and unpossessed pawns out of combat immediately. The costly
        // streamed-level actor discovery is independently limited to once per
        // minute so it cannot cause a hitch every ten seconds.
        if (now >= g_NextCounselorTargetPruneAt)
        {
            g_NextCounselorTargetPruneAt = now + 1000;
            for (int32_t i = 0;
                i < g_JasonAITargetCount && i < 8;
                ++i)
            {
                TrackCounselorController(g_JasonAITargets[i]);
            }

            int32_t writeIndex = 0;
            for (int32_t readIndex = 0;
                readIndex < g_JasonAITargetCount && readIndex < 8;
                ++readIndex)
            {
                AActor* target = g_JasonAITargets[readIndex];
                if (!IsValidatedLiveCounselorPawn(target))
                {
                    if (g_JasonAIState.Target == target)
                        g_JasonAIState.Target = nullptr;
                    if (g_LocalCounselorTarget == target)
                        g_LocalCounselorTarget = nullptr;
                    if (g_TrapPriorityVictim == target)
                    {
                        g_TrapPriorityVictim = nullptr;
                        g_TrapPriorityUntil = 0;
                        g_TrapPriorityMorphCompleted = false;
                    }
                    continue;
                }

                if (writeIndex != readIndex)
                {
                    g_JasonAITargets[writeIndex] = target;
                    g_JasonAITargetBlockedUntil[writeIndex] =
                        g_JasonAITargetBlockedUntil[readIndex];
                    g_JasonAITargetBlockStrikes[writeIndex] =
                        g_JasonAITargetBlockStrikes[readIndex];
                }
                ++writeIndex;
            }

            for (int32_t i = writeIndex; i < 8; ++i)
            {
                g_JasonAITargets[i] = nullptr;
                g_JasonAITargetBlockedUntil[i] = 0;
                g_JasonAITargetBlockStrikes[i] = 0;
            }
            g_JasonAITargetCount = writeIndex;
            RegisterRespawnedCounselorsFromKnownControllers();
        }

        if (now < g_NextCounselorRosterRefreshAt)
            return;
        g_NextCounselorRosterRefreshAt = now + 60000;

        // The initial roster and its controller set are authoritative. New
        // Tommy possession is detected through those controllers, so a full
        // streamed-level fallback scan is unnecessary during normal matches.
        if (g_CounselorControllerCount > 0)
            return;

        if (!world ||
            !Memory::IsReadable(world, sizeof(UWorld)))
        {
            return;
        }

        TArray<ULevel*>* levels = reinterpret_cast<TArray<ULevel*>*>(
            reinterpret_cast<uintptr_t>(world) + 0x110);
        if (!Memory::IsReadable(levels, sizeof(TArray<ULevel*>)) ||
            !levels->Data ||
            levels->Count <= 0 ||
            levels->Count > 1024 ||
            !Memory::IsReadable(
                levels->Data,
                sizeof(ULevel*) * static_cast<size_t>(levels->Count)))
        {
            return;
        }

        if (g_CounselorRosterRefreshLevel >= levels->Count)
            g_CounselorRosterRefreshLevel = 0;
        ULevel* level = levels->Data[g_CounselorRosterRefreshLevel++];
        if (!level || !Memory::IsReadable(level, sizeof(ULevel)))
            return;

        TArray<AActor*>& actors = level->Actors;
        if (!actors.Data ||
            actors.Count <= 0 ||
            actors.Count > 100000 ||
            !Memory::IsReadable(
                actors.Data,
                sizeof(AActor*) * static_cast<size_t>(actors.Count)))
        {
            return;
        }

        const int32_t countBefore = g_JasonAITargetCount;
        for (int32_t i = 0;
            i < actors.Count && g_JasonAITargetCount < 8;
            ++i)
        {
            if (IsValidatedLiveCounselorPawn(actors.Data[i]))
                RegisterJasonAITarget(actors.Data[i]);
        }

        if (g_JasonAITargetCount != countBefore)
        {
            Logger::Success(
                "18L-AI counselor bridge: live roster refreshed | targets=" +
                std::to_string(g_JasonAITargetCount));
        }
    }

    void RegisterLiveCounselorTargets(UWorld* world)
    {
        if (!world || !Memory::IsReadable(world, sizeof(UWorld)))
            return;

        constexpr uintptr_t Offset_Levels = 0x110;
        TArray<ULevel*>* levels = reinterpret_cast<TArray<ULevel*>*>(
            reinterpret_cast<uintptr_t>(world) + Offset_Levels);

        if (!Memory::IsReadable(levels, sizeof(TArray<ULevel*>)) ||
            !levels->Data ||
            levels->Count <= 0 ||
            levels->Count > 1024 ||
            !Memory::IsReadable(
                levels->Data,
                sizeof(ULevel*) * static_cast<size_t>(levels->Count)))
        {
            return;
        }

        for (int32_t levelIndex = 0;
            levelIndex < levels->Count;
            ++levelIndex)
        {
            ULevel* level = levels->Data[levelIndex];
            if (!level || !Memory::IsReadable(level, sizeof(ULevel)))
                continue;

            TArray<AActor*>& actors = level->Actors;
            if (!actors.Data ||
                actors.Count <= 0 ||
                actors.Count > 100000 ||
                !Memory::IsReadable(
                    actors.Data,
                    sizeof(AActor*) * static_cast<size_t>(actors.Count)))
            {
                continue;
            }

            for (int32_t actorIndex = 0;
                actorIndex < actors.Count;
                ++actorIndex)
            {
                AActor* actor = actors.Data[actorIndex];
                if (IsValidatedLiveCounselorPawn(actor))
                {
                    RegisterJasonAITarget(actor);
                }
            }
        }

        Logger::Success(
            "18L-AI counselor bridge: validated live counselor target roster=" +
            std::to_string(g_JasonAITargetCount));
        for (int32_t i = 0; i < g_JasonAITargetCount; ++i)
        {
            Logger::Debug(
                "18L-AI counselor target[" + std::to_string(i) + "]=" +
                JasonAISafeName(
                    reinterpret_cast<UObject*>(g_JasonAITargets[i])));
        }
    }

    bool IsActorInWorld(UWorld* world, AActor* wanted)
    {
        if (!world ||
            !wanted ||
            !Memory::IsReadable(world, sizeof(UWorld)) ||
            !Memory::IsReadable(wanted, sizeof(UObject)))
        {
            return false;
        }

        constexpr uintptr_t Offset_Levels = 0x110;
        TArray<ULevel*>* levels = reinterpret_cast<TArray<ULevel*>*>(
            reinterpret_cast<uintptr_t>(world) + Offset_Levels);

        if (!Memory::IsReadable(levels, sizeof(TArray<ULevel*>)) ||
            !levels->Data ||
            levels->Count <= 0 ||
            levels->Count > 1024 ||
            !Memory::IsReadable(
                levels->Data,
                sizeof(ULevel*) * static_cast<size_t>(levels->Count)))
        {
            return false;
        }

        for (int32_t levelIndex = 0; levelIndex < levels->Count; ++levelIndex)
        {
            ULevel* level = levels->Data[levelIndex];
            if (!level || !Memory::IsReadable(level, sizeof(ULevel)))
                continue;

            TArray<AActor*>& actors = level->Actors;
            if (!actors.Data ||
                actors.Count <= 0 ||
                actors.Count > 100000 ||
                !Memory::IsReadable(
                    actors.Data,
                    sizeof(AActor*) * static_cast<size_t>(actors.Count)))
            {
                continue;
            }

            for (int32_t actorIndex = 0;
                actorIndex < actors.Count;
                ++actorIndex)
            {
                if (actors.Data[actorIndex] == wanted)
                    return true;
            }
        }

        return false;
    }

    AActor* ReadActorField(UObject* owner, uintptr_t offset)
    {
        if (!owner || !Memory::IsReadable(owner, sizeof(UObject)))
            return nullptr;

        AActor** field = reinterpret_cast<AActor**>(
            reinterpret_cast<uintptr_t>(owner) + offset);

        if (!Memory::IsReadable(field, sizeof(AActor*)))
            return nullptr;

        AActor* actor = *field;
        return actor && Memory::IsReadable(actor, sizeof(UObject))
            ? actor
            : nullptr;
    }

    AActor* GetActorOwnerSafe(AActor* actor)
    {
        if (!actor || !actor->Class ||
            !Memory::IsReadable(actor, sizeof(UObject)))
        {
            return nullptr;
        }

        UFunction* getOwner = FindFunctionInHierarchyByName(
            actor->Class,
            "GetOwner");
        if (!getOwner)
            return nullptr;

        struct Params { AActor* ReturnValue; };
        Params params{};
        if (!SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(actor),
                actor,
                getOwner,
                &params))
        {
            return nullptr;
        }
        return params.ReturnValue;
    }

    bool DestroyWorldActorSafe(AActor* actor)
    {
        if (!actor || !actor->Class ||
            !Memory::IsReadable(actor, sizeof(UObject)))
        {
            return false;
        }

        UFunction* destroy = FindFunctionInHierarchyByName(
            actor->Class,
            "K2_DestroyActor");
        return destroy && SafeProcessEventCall(
            reinterpret_cast<uintptr_t>(actor),
            actor,
            destroy,
            nullptr);
    }

    bool SpawnUsefulOfflinePickup(const FVector& location)
    {
        UWorld* world = g_JasonAIState.World;
        UClass* itemClass = nullptr;
        for (int32_t attempt = 0; attempt < 3; ++attempt)
        {
            const int32_t index = (g_LootReplacementIndex + attempt) % 3;
            if (g_LootReplacementClasses[index] &&
                Memory::IsReadable(
                    g_LootReplacementClasses[index],
                    sizeof(UClass)))
            {
                itemClass = g_LootReplacementClasses[index];
                g_LootReplacementIndex = (index + 1) % 3;
                break;
            }
        }
        if (!world || !itemClass)
            return false;

        HMODULE module = GetModuleHandle(nullptr);
        if (!module)
            return false;

        using SpawnParamsCtorFn = void* (__fastcall*)(void*);
        using SpawnActorFn = AActor* (__fastcall*)(
            UWorld*, UClass*, const FVector*, const FRotator*, const void*);
        SpawnParamsCtorFn construct = reinterpret_cast<SpawnParamsCtorFn>(
            reinterpret_cast<uintptr_t>(module) + 0x017F1160);
        SpawnActorFn spawn = reinterpret_cast<SpawnActorFn>(
            reinterpret_cast<uintptr_t>(module) + 0x01519D10);
        if (!Memory::IsReadable(reinterpret_cast<void*>(construct), 1) ||
            !Memory::IsReadable(reinterpret_cast<void*>(spawn), 1))
        {
            return false;
        }

        alignas(16) uint8_t parameters[0x30]{};
        construct(parameters);
        parameters[0x28] = 2; // AlwaysSpawn; distribution points are known-safe.
        FRotator rotation{};
        AActor* spawned = spawn(
            world,
            itemClass,
            &location,
            &rotation,
            parameters);
        if (spawned)
        {
            ++g_UsefulPickupsSpawned;
            return true;
        }
        return false;
    }

    void CleanupCounselorRouteWalkieTalkies(ULONGLONG now)
    {
        UWorld* world = g_JasonAIState.World;
        if (!world ||
            !Memory::IsReadable(world, sizeof(UWorld)) ||
            g_LootCleanupComplete ||
            now < g_NextWalkieCleanupAt)
        {
            return;
        }

        TArray<ULevel*>* levels = reinterpret_cast<TArray<ULevel*>*>(
            reinterpret_cast<uintptr_t>(world) + 0x110);
        if (!Memory::IsReadable(levels, sizeof(TArray<ULevel*>)) ||
            !levels->Data ||
            levels->Count <= 0 ||
            levels->Count > 1024 ||
            !Memory::IsReadable(
                levels->Data,
                sizeof(ULevel*) * static_cast<size_t>(levels->Count)))
        {
            return;
        }

        if (g_WalkieCleanupLevel >= levels->Count)
        {
            g_WalkieCleanupLevel = 0;
            g_WalkieCleanupActor = 0;
            if (!g_LootCleanupPassActive)
            {
                g_LootCleanupPassActive = true;
                g_LootKeysKeptThisPass = 0;
                g_NextWalkieCleanupAt = now + 250;
                Logger::Success(
                    "18L-AI offline inventory census complete | cars=" +
                    std::to_string(g_LootCensusCarCount) +
                    " | boat=" + std::to_string(g_LootCensusSawBoat ? 1 : 0) +
                    " | replacements=" +
                    std::to_string(g_LootReplacementClasses[0] ? 1 : 0) + "," +
                    std::to_string(g_LootReplacementClasses[1] ? 1 : 0) + "," +
                    std::to_string(g_LootReplacementClasses[2] ? 1 : 0));
            }
            else
            {
                g_LootCleanupPassActive = false;
                g_LootCleanupComplete = true;
                g_NextWalkieCleanupAt = ~0ULL;
                Logger::Success(
                    "18L-AI offline inventory cleanup complete; recurring world scans retired");
            }
            return;
        }
        // Spread startup census/cleanup over small fixed actor slices. The old
        // code ancestry-walked and classified a complete streamed level on one
        // frame, causing the early-match jerk the player observed.
        g_NextWalkieCleanupAt = now + 25;

        ULevel* level = levels->Data[g_WalkieCleanupLevel];
        if (!level || !Memory::IsReadable(level, sizeof(ULevel)))
        {
            ++g_WalkieCleanupLevel;
            g_WalkieCleanupActor = 0;
            return;
        }

        TArray<AActor*>& actors = level->Actors;
        if (!actors.Data ||
            actors.Count <= 0 ||
            actors.Count > 100000 ||
            !Memory::IsReadable(
                actors.Data,
                sizeof(AActor*) * static_cast<size_t>(actors.Count)))
        {
            ++g_WalkieCleanupLevel;
            g_WalkieCleanupActor = 0;
            return;
        }

        if (g_WalkieCleanupActor >= actors.Count)
        {
            ++g_WalkieCleanupLevel;
            g_WalkieCleanupActor = 0;
            return;
        }
        const int32_t actorStart = g_WalkieCleanupActor;
        const int32_t actorEnd = (std::min)(
            actors.Count,
            actorStart + 16);

        if (!g_LootCleanupPassActive)
        {
            for (int32_t i = actorStart; i < actorEnd; ++i)
            {
                AActor* actor = actors.Data[i];
                if (!actor || !actor->Class ||
                    !Memory::IsReadable(actor, sizeof(UObject)))
                {
                    continue;
                }

                if (ObjectClassDerivesFromExact(
                        reinterpret_cast<UObject*>(actor),
                        "SCDriveableBoat"))
                {
                    g_LootCensusSawBoat = true;
                }

                uint8_t carKind = 0;
                int32_t seatCount = 0;
                if (ClassifyRepairableCar(actor, carKind, seatCount))
                    ++g_LootCensusCarCount;

                // Reuse the existing 16-actor startup census to remember a
                // guaranteed melee class. Sweater convergence can then equip
                // bots without ever adding a synchronous world scan.
                if (!g_CounselorConvergenceMeleeClass &&
                    ObjectClassDerivesFromExact(
                        reinterpret_cast<UObject*>(actor),
                        "CounselorTwoHandedAxe_C"))
                {
                    g_CounselorConvergenceMeleeClass = actor->Class;
                }
                if (!g_CounselorConvergenceMacheteClass &&
                    ObjectClassDerivesFromExact(
                        reinterpret_cast<UObject*>(actor),
                        "CounselorMachete_C"))
                {
                    g_CounselorConvergenceMacheteClass = actor->Class;
                }

                const char* replacementNames[] =
                {
                    "BP_PocketKnife_C",
                    "BP_FirstAid_C",
                    "BP_Firecracker_C"
                };
                for (int32_t replacementIndex = 0;
                    replacementIndex < 3;
                    ++replacementIndex)
                {
                    if (!g_LootReplacementClasses[replacementIndex] &&
                        ObjectClassDerivesFromExact(
                            reinterpret_cast<UObject*>(actor),
                            replacementNames[replacementIndex]))
                    {
                        g_LootReplacementClasses[replacementIndex] =
                            actor->Class;
                    }
                }
            }
            g_WalkieCleanupActor = actorEnd;
            if (g_WalkieCleanupActor >= actors.Count)
            {
                ++g_WalkieCleanupLevel;
                g_WalkieCleanupActor = 0;
            }
            return;
        }

        enum class CleanupKind : uint8_t
        {
            Walkie,
            Tape,
            InvalidPropeller,
            SurplusKey
        };
        struct CleanupTarget
        {
            AActor* Actor;
            CleanupKind Kind;
        };

        // Collect first: K2_DestroyActor mutates the level actor array.
        CleanupTarget targets[128]{};
        int32_t targetCount = 0;
        for (int32_t i = actorStart; i < actorEnd && targetCount < 128; ++i)
        {
            AActor* actor = actors.Data[i];
            if (!actor || !actor->Class ||
                !Memory::IsReadable(actor, sizeof(UObject)))
            {
                continue;
            }

            CleanupKind kind{};
            bool remove = false;
            const std::string itemClassName = JasonAISafeName(
                reinterpret_cast<UObject*>(actor->Class));
            if (ObjectClassDerivesFromExact(
                    reinterpret_cast<UObject*>(actor),
                    "WalkieTalkie_C"))
            {
                kind = CleanupKind::Walkie;
                remove = true;
            }
            else if (ObjectClassDerivesFromExact(
                         reinterpret_cast<UObject*>(actor),
                         "PamelaTape_C") ||
                     ObjectClassDerivesFromExact(
                         reinterpret_cast<UObject*>(actor),
                         "TommyTape_C") ||
                     itemClassName.find("Pamela_Tape") != std::string::npos ||
                     itemClassName.find("Tommy_Tape") != std::string::npos ||
                     itemClassName.find("Tape_C") != std::string::npos)
            {
                kind = CleanupKind::Tape;
                remove = true;
            }
            else if (!g_LootCensusSawBoat &&
                     ObjectClassDerivesFromExact(
                         reinterpret_cast<UObject*>(actor),
                         "BoatPropeller_C"))
            {
                kind = CleanupKind::InvalidPropeller;
                remove = true;
            }
            else if (ObjectClassDerivesFromExact(
                         reinterpret_cast<UObject*>(actor),
                         "CarKeys_C"))
            {
                kind = CleanupKind::SurplusKey;
                remove = true;
            }

            if (!remove)
                continue;

            // Resolve ownership only for the handful of candidate pickups.
            // Calling reflected GetOwner for every world actor caused the
            // severe repeating render stall in the previous build.
            AActor* itemOwner = GetActorOwnerSafe(actor);
            const bool carriedByCounselor =
                itemOwner && IsValidatedLiveCounselorPawn(itemOwner);
            if (carriedByCounselor)
                continue;

            if (kind == CleanupKind::SurplusKey &&
                g_LootKeysKeptThisPass < g_LootCensusCarCount)
            {
                ++g_LootKeysKeptThisPass;
                continue;
            }

            targets[targetCount++] = { actor, kind };
        }

        int32_t removedThisLevel = 0;
        for (int32_t i = 0; i < targetCount; ++i)
        {
            FVector location{};
            const bool haveLocation = GetJasonAIActorLocation(
                targets[i].Actor,
                location);
            if (!DestroyWorldActorSafe(targets[i].Actor))
                continue;

            ++removedThisLevel;
            switch (targets[i].Kind)
            {
            case CleanupKind::Walkie: ++g_WalkiesRemoved; break;
            case CleanupKind::Tape: ++g_TapesRemoved; break;
            case CleanupKind::InvalidPropeller:
                ++g_InvalidPropellersRemoved;
                break;
            case CleanupKind::SurplusKey: ++g_SurplusKeysRemoved; break;
            }
            if (haveLocation)
                SpawnUsefulOfflinePickup(location);
        }

        if (removedThisLevel > 0)
        {
            Logger::Success(
                "18L-AI offline inventory cleanup | thisLevel=" +
                std::to_string(removedThisLevel) +
                " | walkies=" + std::to_string(g_WalkiesRemoved) +
                " | tapes=" + std::to_string(g_TapesRemoved) +
                " | invalidPropellers=" +
                std::to_string(g_InvalidPropellersRemoved) +
                " | surplusKeys=" + std::to_string(g_SurplusKeysRemoved) +
                " | usefulReplacements=" +
                std::to_string(g_UsefulPickupsSpawned));
        }
        g_WalkieCleanupActor = actorEnd;
        if (g_WalkieCleanupActor >= actors.Count)
        {
            ++g_WalkieCleanupLevel;
            g_WalkieCleanupActor = 0;
        }
    }

    bool CallReflectedBoolGetter(UObject* object, const char* functionName)
    {
        if (!object || !functionName || !object->Class ||
            !Memory::IsReadable(object, sizeof(UObject)))
        {
            return false;
        }

        UFunction* function = FindFunctionInHierarchyByName(
            object->Class,
            functionName);
        if (!function)
            return false;

        struct Params { bool ReturnValue; };
        Params params{};
        return SafeProcessEventCall(
            reinterpret_cast<uintptr_t>(object),
            object,
            function,
            &params) && params.ReturnValue;
    }

    bool ReadReflectedBoolByte(UObject* object, const char* propertyName)
    {
        if (!object || !propertyName || !object->Class ||
            !Memory::IsReadable(object, sizeof(UObject)))
        {
            return false;
        }
        UPropertyLite* property = FindPropertyInHierarchyByName(
            object->Class,
            propertyName);
        if (!property ||
            property->Offset_Internal <= 0 ||
            property->Offset_Internal >= 0x10000)
        {
            return false;
        }
        uintptr_t valueOffset = static_cast<uintptr_t>(property->Offset_Internal);
        uint8_t fieldMask = 0xFF;
        const std::string propertyType = JasonAISafeName(
            reinterpret_cast<UObject*>(property->ClassPrivate));
        if (propertyType == "BoolProperty")
        {
            // UE4 UBoolProperty appends FieldSize, ByteOffset, ByteMask and
            // FieldMask at +0x70. Honor its byte offset/mask instead of
            // treating unrelated packed flags in the same byte as true.
            uint8_t* boolLayout = reinterpret_cast<uint8_t*>(property) + 0x70;
            if (Memory::IsReadable(boolLayout, 4))
            {
                valueOffset += boolLayout[1];
                fieldMask = boolLayout[3];
                if (fieldMask == 0)
                    fieldMask = boolLayout[2];
            }
        }

        uint8_t* value = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(object) + valueOffset);
        return Memory::IsReadable(value, 1) &&
            fieldMask != 0 &&
            (*value & fieldMask) != 0;
    }

    bool WriteReflectedBoolByte(
        UObject* object,
        const char* propertyName,
        bool enabled)
    {
        if (!object || !propertyName || !object->Class ||
            !Memory::IsReadable(object, sizeof(UObject)))
        {
            return false;
        }

        UPropertyLite* property = FindPropertyInHierarchyByName(
            object->Class,
            propertyName);
        if (!property ||
            property->Offset_Internal <= 0 ||
            property->Offset_Internal >= 0x10000)
        {
            return false;
        }

        uintptr_t valueOffset = static_cast<uintptr_t>(property->Offset_Internal);
        uint8_t fieldMask = 0xFF;
        const std::string propertyType = JasonAISafeName(
            reinterpret_cast<UObject*>(property->ClassPrivate));
        if (propertyType == "BoolProperty")
        {
            uint8_t* boolLayout = reinterpret_cast<uint8_t*>(property) + 0x70;
            if (Memory::IsReadable(boolLayout, 4))
            {
                valueOffset += boolLayout[1];
                fieldMask = boolLayout[3];
                if (fieldMask == 0)
                    fieldMask = boolLayout[2];
            }
        }

        uint8_t* value = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(object) + valueOffset);
        if (!Memory::IsReadable(value, 1) || fieldMask == 0)
            return false;

        if (enabled)
            *value |= fieldMask;
        else
            *value &= static_cast<uint8_t>(~fieldMask);
        return true;
    }

    bool WriteReflectedFloatValue(
        UObject* object,
        const char* propertyName,
        float value)
    {
        if (!object || !propertyName || !object->Class ||
            !Memory::IsReadable(object, sizeof(UObject)))
        {
            return false;
        }
        UPropertyLite* property = FindPropertyInHierarchyByName(
            object->Class,
            propertyName);
        if (!property || property->Offset_Internal <= 0 ||
            property->Offset_Internal >= 0x10000 ||
            property->ElementSize != sizeof(float) ||
            JasonAISafeName(
                reinterpret_cast<UObject*>(property->ClassPrivate)) !=
                "FloatProperty")
        {
            return false;
        }
        float* field = reinterpret_cast<float*>(
            reinterpret_cast<uintptr_t>(object) +
            property->Offset_Internal);
        if (!Memory::IsReadable(field, sizeof(float)))
            return false;
        *field = value;
        return true;
    }

    bool WriteReflectedObjectValue(
        UObject* object,
        const char* propertyName,
        UObject* value)
    {
        if (!object || !propertyName || !object->Class ||
            !Memory::IsReadable(object, sizeof(UObject)) ||
            (value && !Memory::IsReadable(value, sizeof(UObject))))
        {
            return false;
        }
        UPropertyLite* property = FindPropertyInHierarchyByName(
            object->Class,
            propertyName);
        if (!property || property->Offset_Internal <= 0 ||
            property->Offset_Internal >= 0x10000 ||
            property->ElementSize != sizeof(UObject*) ||
            JasonAISafeName(
                reinterpret_cast<UObject*>(property->ClassPrivate)) !=
                "ObjectProperty")
        {
            return false;
        }
        UObject** field = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(object) +
            property->Offset_Internal);
        if (!Memory::IsReadable(field, sizeof(UObject*)))
            return false;
        *field = value;
        return true;
    }

    bool RestoreNativePamelaSweaterContract(
        AActor* counselor,
        UObject* ability,
        UObject* controller)
    {
        if (!counselor || !ability ||
            !Memory::IsReadable(counselor, 0x18D0) ||
            !Memory::IsReadable(ability, 0xA8))
        {
            return false;
        }

        // Shipping UAbility::CanUse (RVA 0x3B9AB0) and Activate
        // (RVA 0x3B6570) both reject the ability while byte +0x98 is set.
        // Stock activation sets it, but sweater removal does not clear it.
        // This function runs only after the completed ten-second lifecycle,
        // so clear that proven one-shot gate before republishing the ability.
        *reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(ability) + 0x98) = 0;

        // Shipping ASCCounselorCharacter::GivePamelasSweater
        // (RVA 0x3E16A0) establishes this exact two-way relationship:
        //
        //   counselor + 0x18C8 = sweater ability
        //   ability   + 0x00A0 = owning counselor
        //
        // ASCCounselorCharacter::RemovePamelasSweater (RVA 0x3EE7C0)
        // clears both values. Re-publishing only the counselor slot leaves a
        // detached, consumed ability and the HUD correctly withholds Y.
        *reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(counselor) + 0x18C8) = ability;
        *reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(ability) + 0xA0) =
                reinterpret_cast<UObject*>(counselor);

        if (!controller ||
            !Memory::IsReadable(controller, sizeof(UObject)))
        {
            return false;
        }

        HMODULE module = GetModuleHandle(nullptr);
        if (!module)
            return false;

        constexpr uintptr_t RVA_GetLocalSCPlayerController = 0x00409EA0;
        constexpr uintptr_t RVA_SetSweaterAbilityAvailable = 0x004675D0;
        uint8_t* getLocalTarget = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(module) +
            RVA_GetLocalSCPlayerController);
        uint8_t* setAvailableTarget = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(module) +
            RVA_SetSweaterAbilityAvailable);
        const uint8_t expectedGetLocal[] = {
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0xE8
        };
        const uint8_t expectedSetAvailable[] = {
            0x48, 0x8B, 0x89, 0xD0, 0x04, 0x00, 0x00,
            0x48, 0x85, 0xC9, 0x0F, 0x85
        };
        if (!Memory::IsReadable(
                getLocalTarget,
                sizeof(expectedGetLocal)) ||
            std::memcmp(
                getLocalTarget,
                expectedGetLocal,
                sizeof(expectedGetLocal)) != 0 ||
            !Memory::IsReadable(
                setAvailableTarget,
                sizeof(expectedSetAvailable)) ||
            std::memcmp(
                setAvailableTarget,
                expectedSetAvailable,
                sizeof(expectedSetAvailable)) != 0)
        {
            return false;
        }

        using GetLocalSCPlayerControllerFn =
            UObject*(__fastcall*)(UObject* controller);
        using SetSweaterAbilityAvailableFn =
            void(__fastcall*)(UObject* localController, bool available);
        UObject* localController = nullptr;
        __try
        {
            localController =
                reinterpret_cast<GetLocalSCPlayerControllerFn>(
                    getLocalTarget)(controller);
            if (!localController ||
                !Memory::IsReadable(localController, sizeof(UObject)))
            {
                return false;
            }
            reinterpret_cast<SetSweaterAbilityAvailableFn>(
                setAvailableTarget)(localController, true);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool IsPamelaSweaterAbilityObject(UObject* object)
    {
        if (!object || !object->Class ||
            !Memory::IsReadable(object, sizeof(UObject)))
        {
            return false;
        }
        const std::string objectName = JasonAISafeName(object);
        const std::string className = JasonAISafeName(
            reinterpret_cast<UObject*>(object->Class));
        return objectName.find("PamelasSweater") != std::string::npos ||
            className.find("PamelasSweater") != std::string::npos;
    }

    UObject* FindPamelaSweaterAbilityOnOwner(UObject* owner)
    {
        if (!owner || !owner->Class ||
            !Memory::IsReadable(owner, sizeof(UObject)))
        {
            return nullptr;
        }
        const char* propertyNames[] =
        {
            "SweaterAbility",
            "ActiveAbility",
            "AvailableAbility",
            "ActivatedAbility",
            "SelectedAbility",
            "UsedAbility",
            "CounselorActiveAbility"
        };
        for (const char* propertyName : propertyNames)
        {
            UObject* candidate = ReadReflectedObjectProperty(
                owner,
                propertyName);
            if (IsPamelaSweaterAbilityObject(candidate))
                return candidate;
        }
        return nullptr;
    }

    bool ArmUnlimitedSweaterRuleForWorld()
    {
        UWorld* world = g_JasonAIState.World;
        if (!world)
            return false;
        UObject* owners[] =
        {
            reinterpret_cast<UObject*>(ReadActorField(
                reinterpret_cast<UObject*>(world),
                0xF0)),
            reinterpret_cast<UObject*>(ReadActorField(
                reinterpret_cast<UObject*>(world),
                0xF8))
        };
        bool armed = false;
        std::string armedOwner;
        for (UObject* owner : owners)
        {
            if (owner && owner->Class &&
                Memory::IsReadable(owner, sizeof(UObject)) &&
                WriteReflectedBoolByte(
                    owner,
                    "bUnlimitedSweaterStun",
                    true))
            {
                armed = true;
                if (armedOwner.empty())
                {
                    armedOwner = JasonAISafeName(
                        reinterpret_cast<UObject*>(owner->Class));
                }
            }
        }
        if (armed && !g_UnlimitedSweaterWorldRuleLogged)
        {
            g_UnlimitedSweaterWorldRuleLogged = true;
            Logger::Success(
                "18L-BB unlimited Pamela stun rule armed before sweater acquisition | owner=" +
                armedOwner);
        }
        return armed;
    }

    bool IsHunterCounselor(AActor* counselor)
    {
        if (!counselor || !counselor->Class ||
            !Memory::IsReadable(counselor, sizeof(UObject)))
        {
            return false;
        }
        return ObjectClassDerivesFromExact(
                   reinterpret_cast<UObject*>(counselor),
                   "Hunter_Counselor_C") ||
            CallReflectedBoolGetter(
                reinterpret_cast<UObject*>(counselor),
                "GetIsHunter") ||
            ReadReflectedBoolByte(
                reinterpret_cast<UObject*>(counselor),
                "bIsHunter");
    }

    bool HasPamelaSweater(AActor* counselor)
    {
        if (!counselor ||
            !Memory::IsReadable(counselor, 0x1C6B) ||
            !ObjectClassDerivesFromExact(
                reinterpret_cast<UObject*>(counselor),
                "SCCounselorCharacter"))
        {
            return false;
        }

        // Verified Shipping HasPamelasSweater (RVA 0x497900) returns this
        // native byte directly. Avoid four reflected lookups on every active
        // helper cadence.
        return *reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(counselor) + 0x1C6A) != 0;
    }

    bool RearmPermanentHumanSweater(
        AActor* counselor,
        bool refreshVisual)
    {
        if (!counselor || !counselor->Class ||
            !Memory::IsReadable(counselor, 0x1C6B))
        {
            return false;
        }

        UObject* counselorObject = reinterpret_cast<UObject*>(counselor);
        *reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(counselor) + 0x1C6A) = 1;
        WriteReflectedBoolByte(
            counselorObject,
            "bHasPamelasSweater",
            true);
        WriteReflectedBoolByte(
            counselorObject,
            "bIsWearingSweater",
            true);
        WriteReflectedBoolByte(
            counselorObject,
            "bWearingSweater",
            true);

        // The visible outfit is server-owned. Rewriting the replicated bytes
        // and manually invoking OnRep was not sufficient after activation:
        // the trace claimed restoration while the player still lost the
        // sweater. Use the stock server setter that owns both state and mesh.
        if (refreshVisual)
        {
            if (UFunction* setWearing = FindFunctionInHierarchyByName(
                    counselorObject->Class,
                    "SERVER_SetWearingSweater"))
            {
                struct SetWearingSweaterParams { bool Wearing; };
                SetWearingSweaterParams params{true};
                SafeProcessEventCall(
                    reinterpret_cast<uintptr_t>(counselorObject),
                    counselorObject,
                    setWearing,
                    &params);
            }
        }

        // Resurrected exposes the stock custom-rule switch that permits the
        // Pamela stun more than once. Its owner differs between builds, so
        // resolve it only at acquisition/re-arm time across the few known
        // participants rather than adding a recurring object scan.
        UObject* controller = nullptr;
        UObject** controllerField = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(counselor) + 0x3A0);
        if (Memory::IsReadable(controllerField, sizeof(UObject*)) &&
            *controllerField &&
            Memory::IsReadable(*controllerField, sizeof(UObject)))
        {
            controller = *controllerField;
        }
        UWorld* world = g_JasonAIState.World;
        UObject* gameMode = world
            ? reinterpret_cast<UObject*>(ReadActorField(
                reinterpret_cast<UObject*>(world),
                0xF0))
            : nullptr;
        UObject* gameState = world
            ? reinterpret_cast<UObject*>(ReadActorField(
                reinterpret_cast<UObject*>(world),
                0xF8))
            : nullptr;
        if (!IsPamelaSweaterAbilityObject(
                g_PermanentHumanSweaterAbility))
        {
            g_PermanentHumanSweaterAbility =
                FindPamelaSweaterAbilityOnOwner(counselorObject);
            if (!g_PermanentHumanSweaterAbility && controller)
            {
                g_PermanentHumanSweaterAbility =
                    FindPamelaSweaterAbilityOnOwner(controller);
            }
        }
        UObject* unlimitedOwners[] =
        {
            counselorObject,
            controller,
            gameMode,
            gameState,
            g_PermanentHumanSweaterAbility
        };
        bool unlimitedArmed = false;
        for (UObject* owner : unlimitedOwners)
        {
            if (owner && owner->Class &&
                Memory::IsReadable(owner, sizeof(UObject)) &&
                WriteReflectedBoolByte(
                    owner,
                    "bUnlimitedSweaterStun",
                    true))
            {
                unlimitedArmed = true;
            }
        }

        // Native removal does not destroy or mutate the data-only sweater
        // ability. It detaches the object from both sides and sends a direct
        // client/HUD availability event. Reverse those exact operations after
        // the stock ten-second lifecycle; broad guessed property resets can
        // fight the real lifecycle and are intentionally avoided here.
        bool abilityStateReset = false;
        bool availabilityArmed = false;
        if (refreshVisual &&
            IsPamelaSweaterAbilityObject(
                g_PermanentHumanSweaterAbility))
        {
            UObject* ability = g_PermanentHumanSweaterAbility;
            availabilityArmed = RestoreNativePamelaSweaterContract(
                counselor,
                ability,
                controller);
            abilityStateReset =
                *reinterpret_cast<uint8_t*>(
                    reinterpret_cast<uintptr_t>(ability) + 0x98) == 0 &&
                *reinterpret_cast<UObject**>(
                    reinterpret_cast<uintptr_t>(counselor) + 0x18C8) ==
                    ability &&
                *reinterpret_cast<UObject**>(
                    reinterpret_cast<uintptr_t>(ability) + 0xA0) ==
                    counselorObject;

            UObject* abilityOwners[] =
            {
                counselorObject,
                controller
            };
            for (UObject* abilityOwner : abilityOwners)
            {
                if (!abilityOwner || !abilityOwner->Class ||
                    !Memory::IsReadable(abilityOwner, sizeof(UObject)))
                {
                    continue;
                }
                if (g_PermanentHumanInnateActiveAbility &&
                    Memory::IsReadable(
                        g_PermanentHumanInnateActiveAbility,
                        sizeof(UObject)) &&
                    !IsPamelaSweaterAbilityObject(
                        g_PermanentHumanInnateActiveAbility))
                {
                    WriteReflectedObjectValue(
                        abilityOwner,
                        "ActiveAbility",
                        g_PermanentHumanInnateActiveAbility);
                }
                WriteReflectedObjectValue(
                    abilityOwner,
                    "UsedAbility",
                    nullptr);
                WriteReflectedObjectValue(
                    abilityOwner,
                    "ActivatedAbility",
                    nullptr);
            }
        }

        if (refreshVisual)
        {
            if (UFunction* wearingRepFunction =
                    FindFunctionInHierarchyByName(
                        counselorObject->Class,
                        "OnRep_WearingSweater"))
            {
                SafeProcessEventCall(
                    reinterpret_cast<uintptr_t>(counselorObject),
                    counselorObject,
                    wearingRepFunction,
                    nullptr);
            }
        }

        if (!g_PermanentHumanUnlimitedSweaterArmed)
        {
            g_PermanentHumanUnlimitedSweaterArmed = unlimitedArmed;
            Logger::Success(
                std::string("18L-BA permanent Pamela stun route armed | unlimitedRule=") +
                (unlimitedArmed ? "true" : "false") +
                " | abilityAvailable=" +
                (availabilityArmed ? "true" : "false") +
                " | abilityInstance=" +
                (g_PermanentHumanSweaterAbility
                    ? JasonAISafeName(g_PermanentHumanSweaterAbility)
                    : "<none>") +
                " | stateReset=" +
                (abilityStateReset ? "true" : "false"));
        }
        return unlimitedArmed || availabilityArmed || abilityStateReset;
    }

    void MaintainPermanentHumanSweater(AActor* counselor)
    {
        if (!counselor ||
            !Memory::IsReadable(counselor, 0x1C6B) ||
            !ObjectClassDerivesFromExact(
                reinterpret_cast<UObject*>(counselor),
                "SCCounselorCharacter"))
        {
            return;
        }

        const ULONGLONG now = GetTickCount64();

        // Pamela's stock ability lasts ten seconds. Its cleanup can run after
        // the consumed ownership byte is first restored and clear the HUD
        // ability slot again. Rearm after that lifecycle ends, with one
        // bounded replication-order retry and no additional world scan.
        if (g_PermanentHumanSweaterLatched &&
            g_PermanentHumanSweaterCarrier == counselor &&
            g_PermanentHumanSweaterRearmAt != 0 &&
            now >= g_PermanentHumanSweaterRearmAt)
        {
            RearmPermanentHumanSweater(counselor, true);
            ++g_PermanentHumanSweaterRearmAttempts;
            if (g_PermanentHumanSweaterRearmAttempts < 2)
            {
                g_PermanentHumanSweaterRearmAt = now + 2000;
            }
            else
            {
                g_PermanentHumanSweaterRearmAt = 0;
                Logger::Success(
                    "18L-BC Pamela sweater HUD ability rearmed after stock ten-second activation lifecycle");
            }
        }

        uint8_t* sweaterFlag = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(counselor) + 0x1C6A);
        if (*sweaterFlag != 0)
        {
            if (!g_PermanentHumanSweaterLatched ||
                g_PermanentHumanSweaterCarrier != counselor)
            {
                g_PermanentHumanSweaterCarrier = counselor;
                g_PermanentHumanSweaterLatched = true;
                UObject* activeAbility = ReadReflectedObjectProperty(
                    reinterpret_cast<UObject*>(counselor),
                    "ActiveAbility");
                g_PermanentHumanInnateActiveAbility =
                    activeAbility &&
                    Memory::IsReadable(activeAbility, sizeof(UObject)) &&
                    !IsPamelaSweaterAbilityObject(activeAbility)
                        ? activeAbility
                        : nullptr;
                g_PermanentHumanSweaterRestoreLogged = false;
                g_PermanentHumanUnlimitedSweaterArmed = false;
                g_PermanentHumanSweaterRearmAt = 0;
                g_PermanentHumanSweaterRearmAttempts = 0;
                g_PermanentHumanSweaterUseCount = 0;
                g_RepeatJasonKillStanceAt = 0;
                g_RepeatJasonKillStanceDeadline = 0;
                g_RepeatJasonKillStanceAttempts = 0;
                RearmPermanentHumanSweater(counselor, false);
                Logger::Success(
                    "18L-AX human Pamela sweater ownership latched for the kill sequence");
            }
            return;
        }

        if (g_PermanentHumanSweaterLatched &&
            g_PermanentHumanSweaterCarrier == counselor)
        {
            ++g_PermanentHumanSweaterUseCount;
            if (g_PermanentHumanSweaterUseCount >= 2)
            {
                // The stock unlimited-sweater rule replays Pamela's stun, but
                // a previously consumed JasonDeath_C can leave the native
                // kneel transition one-shot. Give the stock server route a
                // short, bounded opportunity to recreate it on repeat uses.
                g_RepeatJasonKillStanceAt = now + 1500;
                g_RepeatJasonKillStanceDeadline = now + 8000;
                g_RepeatJasonKillStanceAttempts = 0;
            }
            // The stock sweater activation consumes this native ownership
            // byte. Restore only the already-earned human sweater, on the
            // existing kill-team cadence, so the item remains available
            // throughout the paired kill sequence without any world scan.
            RearmPermanentHumanSweater(counselor, true);
            g_PermanentHumanSweaterRearmAt = now + 11000;
            g_PermanentHumanSweaterRearmAttempts = 0;
            if (!g_PermanentHumanSweaterRestoreLogged)
            {
                g_PermanentHumanSweaterRestoreLogged = true;
                Logger::Success(
                    "18L-AY restored permanent human Pamela sweater ownership and visible wearing state after activation");
            }
        }
    }

    bool IsFemaleCounselor(AActor* counselor)
    {
        if (!counselor || !counselor->Class ||
            !Memory::IsReadable(counselor, sizeof(UObject)))
        {
            return false;
        }
        if (ReadReflectedBoolByte(
                reinterpret_cast<UObject*>(counselor),
                "bIsFemale"))
        {
            return true;
        }

        const std::string className = JasonAISafeName(
            reinterpret_cast<UObject*>(counselor->Class));
        const char* femaleArchetypes[] =
        {
            "Athlete_Counselor_C",
            "Bookworm_Counselor_C",
            "Flirt_Counselor_C",
            "Hero_Counselor_C",
            "Rocker_Counselor_C",
            "Head_Counselor_C",
            "Biker_Counselor_C",
            "Catty_Counselor_C",
            "Tina_Counselor_C"
        };
        for (const char* archetype : femaleArchetypes)
        {
            if (className == archetype)
                return true;
        }
        return false;
    }

    UObject* GetPawnControllerSafe(AActor* pawn)
    {
        if (!pawn || !Memory::IsReadable(pawn, 0x3A8))
            return nullptr;
        UObject** controller = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(pawn) + 0x3A0);
        return Memory::IsReadable(controller, sizeof(UObject*)) &&
            *controller && Memory::IsReadable(*controller, sizeof(UObject))
            ? *controller
            : nullptr;
    }

    UObject* GetCounselorBlackboardCached(UObject* controller)
    {
        if (!controller ||
            !Memory::IsReadable(controller, sizeof(UObject)) ||
            !controller->Class)
        {
            return nullptr;
        }

        static UClass* cachedClass = nullptr;
        static int32_t cachedOffset = -1;
        if (cachedClass != controller->Class)
        {
            cachedClass = controller->Class;
            cachedOffset = -1;
            UPropertyLite* property = FindPropertyInHierarchyByName(
                controller->Class,
                "Blackboard");
            if (!property)
            {
                property = FindPropertyInHierarchyByName(
                    controller->Class,
                    "BlackboardComponent");
            }
            if (property &&
                property->Offset_Internal > 0 &&
                property->Offset_Internal < 0x10000)
            {
                cachedOffset = property->Offset_Internal;
            }
        }
        if (cachedOffset <= 0)
            return nullptr;

        UObject** field = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(controller) + cachedOffset);
        return Memory::IsReadable(field, sizeof(UObject*)) &&
            *field && Memory::IsReadable(*field, sizeof(UObject))
                ? *field
                : nullptr;
    }

    UObject* GetCounselorBrainComponentCached(UObject* controller)
    {
        if (!controller ||
            !Memory::IsReadable(controller, sizeof(UObject)) ||
            !controller->Class)
        {
            return nullptr;
        }

        static UClass* cachedClass = nullptr;
        static int32_t cachedOffset = -1;
        if (cachedClass != controller->Class)
        {
            cachedClass = controller->Class;
            cachedOffset = -1;
            UPropertyLite* property = FindPropertyInHierarchyByName(
                controller->Class,
                "BrainComponent");
            if (property &&
                property->Offset_Internal > 0 &&
                property->Offset_Internal < 0x10000)
            {
                cachedOffset = property->Offset_Internal;
            }
        }
        if (cachedOffset <= 0)
            return nullptr;

        UObject** field = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(controller) + cachedOffset);
        return Memory::IsReadable(field, sizeof(UObject*)) &&
            *field && Memory::IsReadable(*field, sizeof(UObject))
                ? *field
                : nullptr;
    }

    AActor* FindNearestWorldActorByClass(
        const char* className,
        const FVector* preferredOrigin,
        float maximumDistance)
    {
        UWorld* world = g_JasonAIState.World;
        if (!world || !className || !Memory::IsReadable(world, sizeof(UWorld)))
            return nullptr;

        TArray<ULevel*>* levels = reinterpret_cast<TArray<ULevel*>*>(
            reinterpret_cast<uintptr_t>(world) + 0x110);
        if (!Memory::IsReadable(levels, sizeof(TArray<ULevel*>)) ||
            !levels->Data || levels->Count <= 0 || levels->Count > 1024 ||
            !Memory::IsReadable(
                levels->Data,
                sizeof(ULevel*) * static_cast<size_t>(levels->Count)))
        {
            return nullptr;
        }

        AActor* best = nullptr;
        float bestDistanceSquared = maximumDistance > 0.0f
            ? maximumDistance * maximumDistance
            : FLT_MAX;
        for (int32_t levelIndex = 0; levelIndex < levels->Count; ++levelIndex)
        {
            ULevel* level = levels->Data[levelIndex];
            if (!level || !Memory::IsReadable(level, sizeof(ULevel)))
                continue;
            TArray<AActor*>& actors = level->Actors;
            if (!actors.Data || actors.Count <= 0 || actors.Count > 100000 ||
                !Memory::IsReadable(
                    actors.Data,
                    sizeof(AActor*) * static_cast<size_t>(actors.Count)))
            {
                continue;
            }

            for (int32_t actorIndex = 0; actorIndex < actors.Count; ++actorIndex)
            {
                AActor* actor = actors.Data[actorIndex];
                if (!ObjectClassDerivesFromExact(
                        reinterpret_cast<UObject*>(actor),
                        className))
                {
                    continue;
                }

                if (!preferredOrigin)
                    return actor;
                FVector location{};
                if (!GetJasonAIActorLocation(actor, location))
                    continue;
                const float dx = location.X - preferredOrigin->X;
                const float dy = location.Y - preferredOrigin->Y;
                const float distanceSquared = dx * dx + dy * dy;
                if (std::isfinite(distanceSquared) &&
                    distanceSquared < bestDistanceSquared)
                {
                    bestDistanceSquared = distanceSquared;
                    best = actor;
                }
            }
        }
        return best;
    }

    AActor* FindNearestWorldDoorActor(
        const FVector& preferredOrigin,
        float maximumDistance)
    {
        UWorld* world = g_JasonAIState.World;
        if (!world || !Memory::IsReadable(world, sizeof(UWorld)))
            return nullptr;

        TArray<ULevel*>* levels = reinterpret_cast<TArray<ULevel*>*>(
            reinterpret_cast<uintptr_t>(world) + 0x110);
        if (!Memory::IsReadable(levels, sizeof(TArray<ULevel*>)) ||
            !levels->Data || levels->Count <= 0 || levels->Count > 1024 ||
            !Memory::IsReadable(
                levels->Data,
                sizeof(ULevel*) * static_cast<size_t>(levels->Count)))
        {
            return nullptr;
        }

        AActor* best = nullptr;
        float bestDistanceSquared = maximumDistance * maximumDistance;
        for (int32_t levelIndex = 0; levelIndex < levels->Count; ++levelIndex)
        {
            ULevel* level = levels->Data[levelIndex];
            if (!level || !Memory::IsReadable(level, sizeof(ULevel)))
                continue;
            TArray<AActor*>& actors = level->Actors;
            if (!actors.Data || actors.Count <= 0 || actors.Count > 100000 ||
                !Memory::IsReadable(
                    actors.Data,
                    sizeof(AActor*) * static_cast<size_t>(actors.Count)))
            {
                continue;
            }

            for (int32_t actorIndex = 0; actorIndex < actors.Count; ++actorIndex)
            {
                AActor* actor = actors.Data[actorIndex];
                if (!actor || !actor->Class ||
                    !Memory::IsReadable(actor, sizeof(UObject)))
                {
                    continue;
                }

                const std::string actorName = JasonAISafeName(
                    reinterpret_cast<UObject*>(actor));
                const std::string className = JasonAISafeName(
                    reinterpret_cast<UObject*>(actor->Class));
                if (actorName.find("Door") == std::string::npos &&
                    className.find("Door") == std::string::npos)
                {
                    continue;
                }

                FVector location{};
                if (!GetJasonAIActorLocation(actor, location))
                    continue;
                const float dx = location.X - preferredOrigin.X;
                const float dy = location.Y - preferredOrigin.Y;
                const float distanceSquared = dx * dx + dy * dy;
                if (std::isfinite(distanceSquared) &&
                    distanceSquared < bestDistanceSquared)
                {
                    bestDistanceSquared = distanceSquared;
                    best = actor;
                }
            }
        }
        return best;
    }

    void EnsureTommyRadioObjectivePublished(ULONGLONG now)
    {
        if (g_TommyObjectivePublished ||
            now < g_NextTommyObjectivePublishAt)
        {
            return;
        }
        g_NextTommyObjectivePublishAt = now + 5000;

        UWorld* world = g_JasonAIState.World;
        AActor* gameState = world
            ? ReadActorField(reinterpret_cast<UObject*>(world), 0xF8)
            : nullptr;
        if (!gameState || !gameState->Class ||
            !Memory::IsReadable(gameState, sizeof(UObject)))
        {
            return;
        }

        UFunction* getCBRadio = FindFunctionInHierarchyByName(
            gameState->Class,
            "GetCBRadio");
        struct GetRadioParams { AActor* ReturnValue; };
        GetRadioParams getParams{};
        if (getCBRadio &&
            SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(gameState),
                gameState,
                getCBRadio,
                &getParams) &&
            getParams.ReturnValue &&
            Memory::IsReadable(getParams.ReturnValue, sizeof(UObject)))
        {
            g_TommyObjectiveRadio = getParams.ReturnValue;
            g_TommyObjectivePublished = true;
            Logger::Success(
                "18L-AP Tommy objective ready: GameState already publishes " +
                JasonAISafeName(
                    reinterpret_cast<UObject*>(g_TommyObjectiveRadio)));
            return;
        }

        if (g_TommyObjectiveRadio &&
            !Memory::IsReadable(g_TommyObjectiveRadio, sizeof(UObject)))
        {
            g_TommyObjectiveRadio = nullptr;
        }
        if (!g_TommyObjectiveRadio)
        {
            g_TommyObjectiveRadio = FindNearestWorldActorByClass(
                "HunterCBRadio_C",
                nullptr,
                0.0f);
            if (!g_TommyObjectiveRadio)
            {
                g_TommyObjectiveRadio = FindNearestWorldActorByClass(
                    "SCCBRadio",
                    nullptr,
                    0.0f);
            }
        }
        if (!g_TommyObjectiveRadio)
            return;

        const char* propertyNames[] =
        {
            "CBRadio",
            "HunterCBRadio",
            "CBRadioActor"
        };
        UPropertyLite* radioProperty = nullptr;
        const char* resolvedPropertyName = nullptr;
        for (const char* propertyName : propertyNames)
        {
            UPropertyLite* candidate = FindPropertyInHierarchyByName(
                gameState->Class,
                propertyName);
            if (!candidate ||
                candidate->ElementSize != sizeof(AActor*) ||
                candidate->Offset_Internal <= 0 ||
                candidate->Offset_Internal >= 0x10000)
            {
                continue;
            }
            const std::string propertyType = JasonAISafeName(
                reinterpret_cast<UObject*>(candidate->ClassPrivate));
            if (propertyType.find("ObjectProperty") == std::string::npos)
                continue;
            radioProperty = candidate;
            resolvedPropertyName = propertyName;
            break;
        }
        if (!radioProperty)
        {
            Logger::Error(
                "18L-AP Tommy objective: CBRadio GameState property unavailable; preserving stock UI");
            g_TommyObjectivePublished = true;
            return;
        }

        AActor** radioField = reinterpret_cast<AActor**>(
            reinterpret_cast<uintptr_t>(gameState) +
            radioProperty->Offset_Internal);
        if (!Memory::IsReadable(radioField, sizeof(AActor*)))
            return;
        *radioField = g_TommyObjectiveRadio;

        getParams = {};
        const bool getterConfirmed = getCBRadio &&
            SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(gameState),
                gameState,
                getCBRadio,
                &getParams) &&
            getParams.ReturnValue == g_TommyObjectiveRadio;
        if (getterConfirmed)
        {
            g_TommyObjectivePublished = true;
            Logger::Success(
                std::string("18L-AP Tommy objective published through GameState.") +
                resolvedPropertyName + " | radio=" +
                JasonAISafeName(
                    reinterpret_cast<UObject*>(g_TommyObjectiveRadio)));
        }
    }

    int32_t CountPawnItem(AActor* pawn, UClass* itemClass)
    {
        if (!pawn || !itemClass ||
            !Memory::IsReadable(pawn, sizeof(UObject)))
        {
            return 0;
        }
        uintptr_t* vtable = *reinterpret_cast<uintptr_t**>(pawn);
        if (!vtable ||
            !Memory::IsReadable(vtable, 0xE98 + sizeof(uintptr_t)))
        {
            return 0;
        }
        using CountItemFn = int32_t(__fastcall*)(AActor*, UClass*);
        CountItemFn countItem = reinterpret_cast<CountItemFn>(
            vtable[0xE98 / sizeof(uintptr_t)]);
        if (!countItem ||
            !Memory::IsReadable(reinterpret_cast<void*>(countItem), 1))
        {
            return 0;
        }
        __try
        {
            return countItem(pawn, itemClass);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    UObject* GetCounselorInteractionManager(AActor* counselor)
    {
        if (!counselor ||
            !Memory::IsReadable(counselor, sizeof(UObject)) ||
            !counselor->Class)
            return nullptr;

        // Shipping counselors use the native manager field at +0xE18. Prefer
        // the verified direct pointer so proximity-final checks do not walk
        // reflected property metadata when several counselor classes are
        // standing beside Jason. Reflection remains a compatibility fallback.
        UObject** direct = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(counselor) + 0xE18);
        if (Memory::IsReadable(direct, sizeof(UObject*)) &&
            *direct && Memory::IsReadable(*direct, sizeof(UObject)))
        {
            return *direct;
        }

        static UClass* cachedClass = nullptr;
        static int32_t cachedOffset = -1;
        if (cachedClass != counselor->Class)
        {
            cachedClass = counselor->Class;
            cachedOffset = -1;
            UPropertyLite* property = FindPropertyInHierarchyByName(
                counselor->Class,
                "InteractableManagerComponent");
            if (property &&
                property->Offset_Internal > 0 &&
                property->Offset_Internal < 0x10000)
            {
                cachedOffset = property->Offset_Internal;
            }
        }
        if (cachedOffset > 0)
        {
            UObject** field = reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(counselor) + cachedOffset);
            if (Memory::IsReadable(field, sizeof(UObject*)) &&
                *field && Memory::IsReadable(*field, sizeof(UObject)))
            {
                return *field;
            }
        }

        return nullptr;
    }

    bool TryGetLatchedJasonDeathContext(
        AActor*& outContext,
        UObject*& outInteractComponent,
        ULONGLONG now)
    {
        if (now >= g_KillTeamFinalContextUntil ||
            !g_LastAcceptedFinalContext ||
            !g_LastAcceptedFinalComponent ||
            !Memory::IsReadable(
                g_LastAcceptedFinalContext,
                sizeof(UObject)) ||
            !Memory::IsReadable(
                g_LastAcceptedFinalComponent,
                sizeof(UObject)) ||
            !ObjectClassDerivesFromExact(
                g_LastAcceptedFinalContext,
                "JasonDeath_C") ||
            !ObjectClassDerivesFromExact(
                g_LastAcceptedFinalComponent,
                "SCInteractComponent"))
        {
            return false;
        }

        outContext = g_LastAcceptedFinalContext;
        outInteractComponent = g_LastAcceptedFinalComponent;
        return true;
    }

    bool TryGetValidatedJasonDeathContext(
        AActor* jason,
        AActor*& outContext,
        UObject*& outInteractComponent,
        ULONGLONG now)
    {
        outContext = nullptr;
        outInteractComponent = nullptr;
        AActor* candidate = jason
            ? ReadActorField(reinterpret_cast<UObject*>(jason), 0x12D8)
            : nullptr;
        if (!candidate)
        {
            // +0x12D8 can clear briefly while the stock paired interaction
            // transitions between kneel and kill. Keep the last fully
            // validated context authoritative during the bounded final-action
            // window instead of handing Tommy back to flee/loot behavior.
            if (TryGetLatchedJasonDeathContext(
                    outContext,
                    outInteractComponent,
                    now))
            {
                return true;
            }
            if (g_LastAcceptedFinalContext)
            {
                g_LastAcceptedFinalContext = nullptr;
                g_LastAcceptedFinalComponent = nullptr;
                g_LastAcceptedFinalKillComponent = nullptr;
                g_KillTeamFinalInteractionDispatched = false;
                g_KillTeamFinalInteractionPending = false;
                g_KillTeamFinalInteractionAttempts = 0;
                g_KillTeamFinalInteractionStartedAt = 0;
                g_KillTeamPendingFinalContext = nullptr;
                g_KillTeamPendingFinalComponent = nullptr;
            }
            return false;
        }

        UObject** componentField = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(candidate) + 0x370);
        UObject* component =
            Memory::IsReadable(componentField, sizeof(UObject*))
                ? *componentField
                : nullptr;

        // +0x12D8 is not exclusively a Jason-death flag, so validate both
        // sides of the stock relationship. JasonDeath_C owns an ordinary
        // SCInteractComponent named Interactable; its separate
        // ContextKillComponent drives the paired animation but is not the
        // component accepted by counselor AttemptInteract. Treating every
        // non-null occupant as the final scene previously suspended Jason's
        // chase, door-breaking and combat for three seconds out of every two.
        const bool valid =
            ObjectClassDerivesFromExact(candidate, "JasonDeath_C") &&
            component &&
            Memory::IsReadable(component, sizeof(UObject)) &&
            ObjectClassDerivesFromExact(component, "SCInteractComponent");
        if (!valid)
        {
            if (candidate != g_LastRejectedFinalContext ||
                now >= g_NextFinalContextDiagnosticAt)
            {
                g_LastRejectedFinalContext = candidate;
                g_NextFinalContextDiagnosticAt = now + 30000;
                Logger::Debug(
                    "18L-AU ignored non-death Jason +0x12D8 object | object=" +
                    JasonAISafeName(reinterpret_cast<UObject*>(candidate)) +
                    " | objectClass=" +
                    (candidate && candidate->Class
                        ? JasonAISafeName(
                            reinterpret_cast<UObject*>(candidate->Class))
                        : "NULL") +
                    " | component=" +
                    JasonAISafeName(component) +
                    " | componentClass=" +
                    (component && component->Class
                        ? JasonAISafeName(
                            reinterpret_cast<UObject*>(component->Class))
                        : "NULL"));
            }
            if (TryGetLatchedJasonDeathContext(
                    outContext,
                    outInteractComponent,
                    now))
            {
                return true;
            }
            if (g_LastAcceptedFinalContext)
            {
                g_LastAcceptedFinalContext = nullptr;
                g_LastAcceptedFinalComponent = nullptr;
                g_LastAcceptedFinalKillComponent = nullptr;
                g_KillTeamFinalInteractionDispatched = false;
                g_KillTeamFinalInteractionPending = false;
                g_KillTeamFinalInteractionAttempts = 0;
                g_KillTeamFinalInteractionStartedAt = 0;
                g_KillTeamPendingFinalContext = nullptr;
                g_KillTeamPendingFinalComponent = nullptr;
            }
            return false;
        }

        outContext = candidate;
        outInteractComponent = component;
        g_LastAcceptedFinalComponent = component;

        // JasonDeath_C exposes two different interaction components. Counselors
        // press A through Interactable, while the native Hunter/weapon predicate
        // executes on ContextKillComponent. Cache the latter so the eligibility
        // hook is scoped to this exact death actor instead of the wrong sibling.
        UObject* contextKillComponent = ReadReflectedObjectProperty(
            candidate,
            "ContextKillComponent");
        if (contextKillComponent &&
            ObjectClassDerivesFromExact(
                contextKillComponent,
                "SCContextKillComponent"))
        {
            g_LastAcceptedFinalKillComponent = contextKillComponent;
        }
        else
        {
            g_LastAcceptedFinalKillComponent = nullptr;
        }
        if (candidate != g_LastAcceptedFinalContext)
        {
            g_LastAcceptedFinalContext = candidate;
            g_KillTeamFinalInteractionCommitted = false;
            // A MoveTo accepted just before the native kneel can remain active
            // even though normal Jason decisions are suspended. Cancel it at
            // the transition and retire its path bookkeeping so the kneeling
            // actor cannot glide toward the previous counselor target.
            StopJasonAIMovementForKnifeOnGameThread();
            g_JasonAIState.PathLocked = false;
            g_JasonAIState.PathLockUntil = 0;
            g_JasonAIState.LastAcceptedMoveAt = 0;
            g_JasonAIState.HaveLastLocation = false;
            g_KillTeamFinalInteractionDispatched = false;
            g_KillTeamFinalInteractionPending = false;
            g_KillTeamFinalInteractionAttempts = 0;
            g_KillTeamFinalInteractionStartedAt = 0;
            g_KillTeamFinalSequenceStartedAt = now;
            g_KillTeamFinalMoveFailures = 0;
            g_KillTeamFinalRepositionAttempted = false;
            g_KillTeamPendingFinalContext = nullptr;
            g_KillTeamPendingFinalComponent = nullptr;
            Logger::Success(
                "18L-AU validated native Jason death context | object=" +
                JasonAISafeName(reinterpret_cast<UObject*>(candidate)) +
                " | component=" + JasonAISafeName(component) +
                " | componentClass=" +
                JasonAISafeName(
                    reinterpret_cast<UObject*>(component->Class)));
        }
        return true;
    }

    void CancelKillTeamHelperNonFinalInteraction(
        AActor* helper,
        ULONGLONG now)
    {
        if (!helper || now < g_NextKillTeamInteractionCancelAt)
            return;

        UObject* manager = GetCounselorInteractionManager(helper);
        UObject** locked = manager
            ? reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(manager) + 0x230)
            : nullptr;
        if (!locked || !Memory::IsReadable(locked, sizeof(UObject*)) ||
            !*locked)
        {
            g_KillTeamLastCancelledInteraction = nullptr;
            return;
        }

        AActor* deathContext = nullptr;
        UObject* deathComponent = nullptr;
        if (TryGetValidatedJasonDeathContext(
                g_JasonAIState.Jason,
                deathContext,
                deathComponent,
                now) &&
            IsFinalContextLockMatch(
                *locked,
                deathContext,
                deathComponent))
        {
            return;
        }

        UObject* currentLock = *locked;
        if (currentLock == g_KillTeamLastCancelledInteraction)
            return;

        // Returned Tommy never owns ordinary loot/hide/repair objectives.
        // Ask the native interaction manager to cancel its current attempt;
        // never clear the lock field directly, and never touch the validated
        // JasonDeath interaction or a Jason grab/pocket-knife transition.
        static UClass* cachedManagerClass = nullptr;
        static UFunction* cancelFunction = nullptr;
        if (manager->Class != cachedManagerClass)
        {
            cachedManagerClass = manager->Class;
            cancelFunction = FindFunctionInHierarchyByName(
                manager->Class,
                "CLIENT_CancelInteractAttempt");
            if (!cancelFunction)
            {
                cancelFunction = FindFunctionInHierarchyByName(
                    manager->Class,
                    "CLIENT_UnlockInteraction");
            }
        }

        const bool cancelled = cancelFunction &&
            SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(manager),
                manager,
                cancelFunction,
                nullptr);
        // ProcessEvent is allowed to release the manager lock immediately, so
        // retain the validated pre-call pointer for bookkeeping/logging rather
        // than dereferencing the field again after cancellation.
        g_KillTeamLastCancelledInteraction = currentLock;
        g_NextKillTeamInteractionCancelAt = now + 2000;
        Logger::Debug(
            std::string("18L-AY AI Tommy cancelled non-final interaction | call=") +
            (cancelled ? "true" : "false") +
            " | object=" + JasonAISafeName(currentLock));
    }

    bool IsFinalContextLockMatch(
        UObject* locked,
        AActor* context,
        UObject* component)
    {
        if (!locked || !context || !component)
            return false;
        if (locked == component)
            return true;
        AActor** ownerField = reinterpret_cast<AActor**>(
            reinterpret_cast<uintptr_t>(locked) + 0xE0);
        return Memory::IsReadable(ownerField, sizeof(AActor*)) &&
            *ownerField == context;
    }

    bool IsKillTeamHelperNativeBusy(AActor* helper, ULONGLONG now)
    {
        if (!helper)
            return true;

        if (g_LastHeldCounselor == helper)
        {
            g_KillTeamHelperNativeBusyUntil = now + 3500;
            g_KillTeamHelperStableObservations = 0;
            return true;
        }

        UObject* manager = GetCounselorInteractionManager(helper);
        UObject** locked = manager
            ? reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(manager) + 0x230)
            : nullptr;
        if (locked && Memory::IsReadable(locked, sizeof(UObject*)) && *locked)
        {
            g_KillTeamHelperNativeBusyUntil = now + 2500;
            g_KillTeamHelperStableObservations = 0;
            AActor* deathContext = nullptr;
            UObject* deathComponent = nullptr;
            if (TryGetValidatedJasonDeathContext(
                    g_JasonAIState.Jason,
                    deathContext,
                    deathComponent,
                    now) &&
                IsFinalContextLockMatch(
                    *locked,
                    deathContext,
                    deathComponent) &&
                deathContext == g_KillTeamPendingFinalContext &&
                deathComponent == g_KillTeamPendingFinalComponent)
            {
                g_KillTeamFinalContextUntil = now + 15000;
                if (!g_KillTeamFinalInteractionDispatched &&
                    (g_KillTeamFinalInteractionPending ||
                        g_KillTeamFinalInteractionAttempts > 0))
                {
                    g_KillTeamFinalInteractionDispatched = true;
                    g_KillTeamFinalInteractionPending = false;
                    Logger::Success(
                        "18L-AW AI Tommy final interaction confirmed by native counselor lock");
                }
            }
            if (g_KillTeamRoute == KillTeamRoute::HumanSweaterAITommy &&
                *locked == g_KillTeamLastCancelledInteraction)
            {
                // The cancel request owns this ordinary interaction now. Let
                // the Jason-only MoveTo abort its path immediately instead of
                // waiting several seconds for a loot/hide lock to decay.
                g_KillTeamHelperNativeBusyUntil = 0;
                g_KillTeamHelperStableObservations = 2;
                return false;
            }
            return true;
        }

        if (now < g_KillTeamHelperNativeBusyUntil)
        {
            g_KillTeamHelperStableObservations = 0;
            return true;
        }

        // Require two consecutive quiet control observations after a native
        // grab/pocket-knife/context interaction before custom movement,
        // pickup, equip or inventory work resumes.
        if (g_KillTeamHelperStableObservations < 2)
        {
            ++g_KillTeamHelperStableObservations;
            return true;
        }
        return false;
    }

    bool AttemptCounselorPickup(AActor* counselor, AActor* item)
    {
        UObject* manager = GetCounselorInteractionManager(counselor);
        UObject* interactComponent = item
            ? ReadReflectedObjectProperty(
                reinterpret_cast<UObject*>(item),
                "InteractComponent")
            : nullptr;
        HMODULE module = GetModuleHandle(nullptr);
        if (!manager || !interactComponent || !module ||
            !Memory::IsReadable(manager, sizeof(UObject)))
        {
            return false;
        }

        UObject** locked = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(manager) + 0x230);
        if (Memory::IsReadable(locked, sizeof(UObject*)) && *locked)
            return false;

        uintptr_t managerVTable = *reinterpret_cast<uintptr_t*>(manager);
        uintptr_t* attemptSlot = reinterpret_cast<uintptr_t*>(
            managerVTable + 0x430);
        const uintptr_t expected = reinterpret_cast<uintptr_t>(module) +
            0x0025AC50;
        if (!managerVTable ||
            !Memory::IsReadable(attemptSlot, sizeof(uintptr_t)) ||
            *attemptSlot != expected)
        {
            return false;
        }

        reinterpret_cast<AttemptInteractFn>(*attemptSlot)(
            manager,
            interactComponent,
            1,
            true);
        return true;
    }

    bool TeleportCounselorNearObjective(
        AActor* counselor,
        UObject* controller,
        const FVector& objectiveLocation)
    {
        if (!counselor || !controller || !counselor->Class)
            return false;

        UFunction* stopMovement = controller->Class
            ? FindFunctionInHierarchyByName(
                controller->Class,
                "StopMovement")
            : nullptr;
        if (stopMovement)
        {
            SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(controller),
                controller,
                stopMovement,
                nullptr);
        }

        UFunction* teleportFunction =
            FindFunctionInHierarchyByName(
                counselor->Class,
                "K2_TeleportTo");
        if (!teleportFunction)
            return false;

        struct Rotation3 { float Pitch; float Yaw; float Roll; };
        struct TeleportParams
        {
            FVector DestLocation;
            Rotation3 DestRotation;
            bool ReturnValue;
        };
        static_assert(sizeof(TeleportParams) == 28,
            "Counselor TeleportParams must be 28 bytes");

        // The fixed shack axe can be inside a room the counselor navmesh does
        // not enter. Try four interaction-range offsets; K2_TeleportTo keeps
        // normal capsule collision checks and rejects an unsafe point.
        const FVector offsets[] =
        {
            FVector{ 110.0f, 0.0f, 20.0f },
            FVector{ -110.0f, 0.0f, 20.0f },
            FVector{ 0.0f, 110.0f, 20.0f },
            FVector{ 0.0f, -110.0f, 20.0f }
        };
        for (const FVector& offset : offsets)
        {
            TeleportParams params{};
            params.DestLocation.X = objectiveLocation.X + offset.X;
            params.DestLocation.Y = objectiveLocation.Y + offset.Y;
            params.DestLocation.Z = objectiveLocation.Z + offset.Z;
            if (SafeProcessEventCall(
                    reinterpret_cast<uintptr_t>(counselor),
                    counselor,
                    teleportFunction,
                    &params) &&
                params.ReturnValue)
            {
                return true;
            }
        }
        return false;
    }

    bool DriveCounselorToPickup(
        AActor* counselor,
        AActor* item,
        ULONGLONG now,
        const char* label)
    {
        UObject* controller = GetPawnControllerSafe(counselor);
        FVector counselorLocation{};
        FVector itemLocation{};
        if (!controller || !item ||
            !GetJasonAIActorLocation(counselor, counselorLocation) ||
            !GetJasonAIActorLocation(item, itemLocation))
        {
            return false;
        }

        const float dx = itemLocation.X - counselorLocation.X;
        const float dy = itemLocation.Y - counselorLocation.Y;
        const float distance = std::sqrt(dx * dx + dy * dy);
        if (!std::isfinite(distance))
            return false;

        const bool shackAxeObjective =
            item == g_KillTeamAxe &&
            std::strcmp(label, "ShackAxe") == 0;
        if (shackAxeObjective)
        {
            if (g_KillTeamAxePursuitStartedAt == 0)
            {
                g_KillTeamAxePursuitStartedAt = now;
                g_KillTeamAxeLastProgressAt = now;
                g_KillTeamAxeBestDistance = distance;
            }
            else if (distance + 75.0f < g_KillTeamAxeBestDistance)
            {
                g_KillTeamAxeBestDistance = distance;
                g_KillTeamAxeLastProgressAt = now;
            }

            // Let stock pathing take Tommy to the shack first. If its looting
            // tree repeatedly steals the route or the interior has no usable
            // navmesh, perform one collision-checked relocation into pickup
            // range instead of scanning or issuing MoveTo forever.
            if (distance > 165.0f &&
                now >= g_KillTeamAxeLastProgressAt + 15000 &&
                TeleportCounselorNearObjective(
                    counselor,
                    controller,
                    itemLocation))
            {
                g_KillTeamAxeLastProgressAt = now;
                g_KillTeamAxeBestDistance = 0.0f;
                g_NextKillTeamInteractAt = now + 2000;
                const bool pickupDispatched =
                    AttemptCounselorPickup(counselor, item);
                Logger::Success(
                    std::string("18L-AS AI Tommy shack-axe stuck fallback completed | pickup=") +
                    (pickupDispatched ? "true" : "false"));
                return true;
            }
        }

        if (distance > 165.0f)
        {
            if (g_KillTeamMoveTarget != item)
            {
                g_KillTeamMoveTarget = item;
                g_NextKillTeamMoveAt = 0;
            }
            // Native MoveTo keeps its path active. Rebuilding the same long
            // path every second made Tommy stutter and slowed his trip to the
            // shack. Retry only as a recovery measure.
            if (now >= g_NextKillTeamMoveAt)
            {
                IssueAIMoveToLocationOnGameThread(
                    controller,
                    itemLocation,
                    90.0f,
                    label);
                g_NextKillTeamMoveAt = now + 10000;
            }
            return true;
        }

        g_KillTeamMoveTarget = nullptr;
        g_NextKillTeamMoveAt = 0;

        if (now >= g_NextKillTeamInteractAt &&
            AttemptCounselorPickup(counselor, item))
        {
            g_NextKillTeamInteractAt = now + 2000;
            Logger::Success(
                std::string("18L-AN kill-team native pickup dispatched | objective=") +
                label + " | helper=" +
                JasonAISafeName(reinterpret_cast<UObject*>(counselor)));
        }
        return true;
    }

    UObject* GetCounselorCurrentWeapon(AActor* counselor)
    {
        HMODULE module = GetModuleHandle(nullptr);
        if (!counselor || !module ||
            !Memory::IsReadable(counselor, sizeof(UObject)))
            return nullptr;
        using GetCurrentWeaponFn = UObject*(__fastcall*)(AActor*);
        GetCurrentWeaponFn getCurrentWeapon =
            reinterpret_cast<GetCurrentWeaponFn>(
                reinterpret_cast<uintptr_t>(module) + 0x002ED9F0);
        if (!Memory::IsReadable(
                reinterpret_cast<void*>(getCurrentWeapon), 1))
        {
            return nullptr;
        }
        __try
        {
            UObject* weapon = getCurrentWeapon(counselor);
            return weapon && Memory::IsReadable(weapon, sizeof(UObject))
                ? weapon
                : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    void WriteLargePositiveNumericProperty(
        UObject* object,
        const char* propertyName)
    {
        if (!object || !object->Class || !propertyName)
            return;
        UPropertyLite* property = FindPropertyInHierarchyByName(
            object->Class,
            propertyName);
        if (!property || property->ElementSize != 4 ||
            property->Offset_Internal <= 0 ||
            property->Offset_Internal >= 0x10000)
        {
            return;
        }
        void* value = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(object) + property->Offset_Internal);
        if (!Memory::IsReadable(value, 4))
            return;
        const std::string typeName = JasonAISafeName(
            reinterpret_cast<UObject*>(property->ClassPrivate));
        if (typeName == "FloatProperty")
            *reinterpret_cast<float*>(value) = 1000000.0f;
        else if (typeName == "IntProperty")
            *reinterpret_cast<int32_t*>(value) = 1000000;
    }

    bool IsAIControlledCounselor(AActor* counselor)
    {
        UObject* controller = GetPawnControllerSafe(counselor);
        return counselor &&
            counselor != g_LocalCounselorTarget &&
            counselor != g_JasonAIState.Jason &&
            controller &&
            controller != g_LocalPlayerController &&
            JasonAIObjectDerivesFromNameContaining(
                controller,
                "AIController");
    }

    void MaintainAIHelperVitalsAndKnife(AActor* helper, ULONGLONG now)
    {
        if (!IsAIControlledCounselor(helper))
            return;
        // These are the verified ASCCharacter fields used by the mature ESP
        // and health systems. The former reflected-property path did not
        // resolve these inherited native fields on returned Tommy.
        float* health = reinterpret_cast<float*>(
            reinterpret_cast<uintptr_t>(helper) +
            Offsets::ASCCharacter_Health);
        float* healthMaxValue = reinterpret_cast<float*>(
            reinterpret_cast<uintptr_t>(helper) +
            Offsets::ASCCharacter_Health + sizeof(float));
        float* healthMinValue = reinterpret_cast<float*>(
            reinterpret_cast<uintptr_t>(helper) +
            Offsets::ASCCharacter_Health + sizeof(float) * 2);
        float* maxHealth = reinterpret_cast<float*>(
            reinterpret_cast<uintptr_t>(helper) +
            Offsets::ASCCharacter_MaxHealth);
        if (Memory::IsReadable(health, sizeof(float)) &&
            Memory::IsReadable(healthMaxValue, sizeof(float)) &&
            Memory::IsReadable(healthMinValue, sizeof(float)) &&
            Memory::IsReadable(maxHealth, sizeof(float)))
        {
            *maxHealth = 1000000.0f;
            *healthMaxValue = 1000000.0f;
            *healthMinValue = 0.0f;
            *health = 1000000.0f;
        }

        UClass* knifeClass = g_LootReplacementClasses[0];
        if (knifeClass &&
            now >= g_NextHelperKnifeGrantAt &&
            CountPawnItem(helper, knifeClass) <= 0)
        {
            HMODULE module = GetModuleHandle(nullptr);
            using GiveStartingItemFn = void(__fastcall*)(AActor*, UClass*);
            GiveStartingItemFn giveStartingItem = module
                ? reinterpret_cast<GiveStartingItemFn>(
                    reinterpret_cast<uintptr_t>(module) + 0x002EF980)
                : nullptr;
            if (giveStartingItem &&
                Memory::IsReadable(
                    reinterpret_cast<void*>(giveStartingItem),
                    1))
            {
                giveStartingItem(helper, knifeClass);
                Logger::Success(
                    "18L-AN kill-team helper pocket knife replenished through stock inventory path");
            }
            g_NextHelperKnifeGrantAt = now + 2000;
        }
    }

    void MaintainKillTeamHelperProtection(AActor* helper, ULONGLONG now)
    {
        if (!IsAIControlledCounselor(helper))
            return;

        // Returning AI Tommy already has a dedicated, phase-separated
        // protection lane. Do not duplicate the same health/inventory work
        // here every active kill-team cadence. Female helpers still use this
        // path after acquiring the sweater.
        if (helper != g_ProtectedAITommy)
            MaintainAIHelperVitalsAndKnife(helper, now);

        UObject* weapon = GetCounselorCurrentWeapon(helper);
        if (weapon &&
            ObjectClassDerivesFromExact(weapon, "SCWeapon"))
        {
            // SCWeapon.CurrentDurability is a direct float at +0x640. The
            // stock axe starts at 30 and wear subtracts from this instance
            // value; replenishing it keeps the helper's equipped weapon from
            // breaking without mutating class defaults or damage balance.
            float* currentDurability = reinterpret_cast<float*>(
                reinterpret_cast<uintptr_t>(weapon) + 0x640);
            if (Memory::IsReadable(currentDurability, sizeof(float)))
                *currentDurability = 1000000.0f;
        }

        if (!g_KillTeamHelperProtected)
        {
            g_KillTeamHelperProtected = true;
            Logger::Success(
                "18L-AN kill-team helper protection armed: AI-only health, replenishing pocket knife, and weapon durability");
        }
    }

    void MaintainReturningAITommyProtection(ULONGLONG now)
    {
        if (now < g_NextAITommyProtectionAt)
            return;
        // Health is set to a very large value, so this lane does not need to
        // rewrite it every 1.3 seconds. Grab release explicitly schedules an
        // earlier knife check above; the quiet steady-state cadence prevents
        // the rhythmic hitch reported when Tommy enters the match.
        g_NextAITommyProtectionAt = now + 5153;

        AActor* candidate = g_ProtectedAITommy;
        if (!candidate ||
            !Memory::IsReadable(candidate, sizeof(UObject)) ||
            !ObjectClassDerivesFromExact(
                reinterpret_cast<UObject*>(candidate),
                "Hunter_Counselor_C"))
        {
            g_ProtectedAITommy = nullptr;
            return;
        }

        // A grab/death transition can briefly unpossess the returned pawn.
        // Preserve its identity through that transition and resume protection
        // as soon as its AI controller is attached again.
        if (!IsValidatedLiveCounselorPawn(candidate) ||
            !IsAIControlledCounselor(candidate))
        {
            return;
        }

        if (candidate == g_LastHeldCounselor ||
            now < g_KillTeamHelperNativeBusyUntil)
        {
            return;
        }

        MaintainAIHelperVitalsAndKnife(candidate, now);
    }

    bool ArmKillTeamCounselorCombat(AActor* helper)
    {
        if (!helper ||
            g_ShouldFleeKillerNameIndex < 0 ||
            g_ShouldFightBackNameIndex < 0 ||
            g_ShouldArmedFightBackNameIndex < 0 ||
            g_ShouldMeleeFightBackNameIndex < 0 ||
            g_SeekWeaponWhileFleeingNameIndex < 0 ||
            g_JasonCharacterNameIndex < 0 ||
            !g_JasonAIState.Jason)
        {
            return false;
        }

        UObject* controller = GetPawnControllerSafe(helper);
        if (!controller)
            return false;
        UObject* blackboard = GetCounselorBlackboardCached(controller);
        if (!blackboard)
            return false;

        // The native fight service treats this blackboard object—not merely
        // the pawn inventory—as the authoritative armed state. Publishing the
        // equipped melee weapon prevents its next evaluation from immediately
        // selecting the stock flee branch again.
        UObject* equippedWeapon = GetCounselorCurrentWeapon(helper);
        if (equippedWeapon && g_SCWeaponNameIndex >= 0)
        {
            SetBlackboardObject(
                blackboard,
                g_SCWeaponNameIndex,
                equippedWeapon);
        }

        // Use the mature counselor behavior tree for melee decisions. These
        // keys direct the armed helper into its existing fight branch. Publish
        // Jason himself as the authoritative object goal first, so the stock
        // tree pursues him from anywhere instead of alternating with loot,
        // hiding, and local-proximity discovery goals.
        bool applied = SetBlackboardObject(
            blackboard,
            g_JasonCharacterNameIndex,
            reinterpret_cast<UObject*>(g_JasonAIState.Jason));
        SetBlackboardBool(
            blackboard,
            g_ShouldFightBackNameIndex,
            true);
        SetBlackboardBool(
            blackboard,
            g_ShouldArmedFightBackNameIndex,
            true);
        SetBlackboardBool(
            blackboard,
            g_ShouldMeleeFightBackNameIndex,
            true);
        SetBlackboardBool(
            blackboard,
            g_SeekWeaponWhileFleeingNameIndex,
            false);
        SetBlackboardBool(
            blackboard,
            g_ShouldFleeKillerNameIndex,
            false);
        if (g_ShouldHideNameIndex >= 0)
            SetBlackboardBool(blackboard, g_ShouldHideNameIndex, false);
        if (g_ShouldOrientTowardKillerNameIndex >= 0)
        {
            SetBlackboardBool(
                blackboard,
                g_ShouldOrientTowardKillerNameIndex,
                true);
        }
        return applied;
    }

    void SuppressConvergenceCounselorFear(AActor* counselor)
    {
        if (!counselor ||
            !Memory::IsReadable(
                counselor,
                OFFSET_FEARMANAGER + sizeof(void*)))
        {
            return;
        }

        void** fearManager = reinterpret_cast<void**>(
            reinterpret_cast<uintptr_t>(counselor) + OFFSET_FEARMANAGER);
        if (!Memory::IsReadable(fearManager, sizeof(void*)) || !*fearManager)
            return;

        float* fearAmount = reinterpret_cast<float*>(
            reinterpret_cast<uintptr_t>(*fearManager) + 0x130);
        if (Memory::IsReadable(fearAmount, sizeof(float)) &&
            std::isfinite(*fearAmount) && *fearAmount != 0.0f)
        {
            *fearAmount = 0.0f;
        }
    }

    bool SetCounselorBrainTickEnabled(AActor* counselor, bool enabled)
    {
        UObject* controller = GetPawnControllerSafe(counselor);
        UObject* brain = GetCounselorBrainComponentCached(controller);
        if (!brain || !brain->Class)
            return false;

        static UClass* cachedClass = nullptr;
        static UFunction* cachedFunction = nullptr;
        if (cachedClass != brain->Class)
        {
            cachedClass = brain->Class;
            cachedFunction = FindFunctionInHierarchyByName(
                brain->Class,
                "SetComponentTickEnabled");
        }
        if (!cachedFunction)
            return false;

        struct Params
        {
            bool bEnabled;
        };
        Params params{};
        params.bEnabled = enabled;
        return SafeProcessEventCall(
            reinterpret_cast<uintptr_t>(brain),
            brain,
            cachedFunction,
            &params);
    }

    int32_t FindConvergenceTravelCounselor(AActor* counselor)
    {
        for (int32_t i = 0; i < g_ConvergenceTravelCounselorCount; ++i)
        {
            if (g_ConvergenceTravelCounselors[i] == counselor)
                return i;
        }
        return -1;
    }

    void RememberConvergenceTravelCounselor(AActor* counselor)
    {
        if (!counselor ||
            FindConvergenceTravelCounselor(counselor) >= 0 ||
            g_ConvergenceTravelCounselorCount >= 8)
        {
            return;
        }
        const int32_t index = g_ConvergenceTravelCounselorCount++;
        g_ConvergenceTravelCounselors[index] = counselor;
        g_ConvergenceTravelLastLocations[index] = FVector{};
        g_ConvergenceTravelLastProgressAt[index] = 0;
        g_ConvergenceTravelRecoveryUntil[index] = 0;
        g_ConvergenceTravelHaveLocation[index] = false;
    }

    void ForgetConvergenceTravelCounselor(AActor* counselor)
    {
        const int32_t index = FindConvergenceTravelCounselor(counselor);
        if (index < 0)
            return;
        --g_ConvergenceTravelCounselorCount;
        g_ConvergenceTravelCounselors[index] =
            g_ConvergenceTravelCounselors[g_ConvergenceTravelCounselorCount];
        g_ConvergenceTravelLastLocations[index] =
            g_ConvergenceTravelLastLocations[g_ConvergenceTravelCounselorCount];
        g_ConvergenceTravelLastProgressAt[index] =
            g_ConvergenceTravelLastProgressAt[g_ConvergenceTravelCounselorCount];
        g_ConvergenceTravelRecoveryUntil[index] =
            g_ConvergenceTravelRecoveryUntil[g_ConvergenceTravelCounselorCount];
        g_ConvergenceTravelHaveLocation[index] =
            g_ConvergenceTravelHaveLocation[g_ConvergenceTravelCounselorCount];
        g_ConvergenceTravelCounselors[
            g_ConvergenceTravelCounselorCount] = nullptr;
        g_ConvergenceTravelLastLocations[
            g_ConvergenceTravelCounselorCount] = FVector{};
        g_ConvergenceTravelLastProgressAt[
            g_ConvergenceTravelCounselorCount] = 0;
        g_ConvergenceTravelRecoveryUntil[
            g_ConvergenceTravelCounselorCount] = 0;
        g_ConvergenceTravelHaveLocation[
            g_ConvergenceTravelCounselorCount] = false;
    }

    void ResumeAllConvergenceCounselorBrains()
    {
        for (int32_t i = 0; i < g_ConvergenceTravelCounselorCount; ++i)
        {
            AActor* counselor = g_ConvergenceTravelCounselors[i];
            if (counselor && Memory::IsReadable(counselor, sizeof(UObject)))
                SetCounselorBrainTickEnabled(counselor, true);
        }
        std::memset(
            g_ConvergenceTravelCounselors,
            0,
            sizeof(g_ConvergenceTravelCounselors));
        std::memset(
            g_ConvergenceTravelLastLocations,
            0,
            sizeof(g_ConvergenceTravelLastLocations));
        std::memset(
            g_ConvergenceTravelLastProgressAt,
            0,
            sizeof(g_ConvergenceTravelLastProgressAt));
        std::memset(
            g_ConvergenceTravelRecoveryUntil,
            0,
            sizeof(g_ConvergenceTravelRecoveryUntil));
        std::memset(
            g_ConvergenceTravelHaveLocation,
            0,
            sizeof(g_ConvergenceTravelHaveLocation));
        g_ConvergenceTravelCounselorCount = 0;
    }

    bool IsConvergenceCounselorArmed(AActor* counselor)
    {
        for (int32_t i = 0;
            i < g_ConvergenceArmedCounselorCount;
            ++i)
        {
            if (g_ConvergenceArmedCounselors[i] == counselor)
                return true;
        }
        return false;
    }

    void RememberConvergenceCounselor(AActor* counselor)
    {
        if (!counselor || IsConvergenceCounselorArmed(counselor) ||
            g_ConvergenceArmedCounselorCount >= 8)
        {
            return;
        }
        g_ConvergenceArmedCounselors[
            g_ConvergenceArmedCounselorCount++] = counselor;
    }

    bool IsStrongConvergenceCounselor(AActor* counselor)
    {
        if (!counselor)
            return false;
        // These are the shipped high-strength archetypes (Strength 6+), plus
        // the hero/guest variants used by Resurrected. This is evaluated only
        // when a weapon is granted, never in the movement or combat cadence.
        const char* strongTypes[] = {
            "Jock", "Tough", "Biker", "Tommy", "Hero",
            "Rob", "Julius", "Boxer", "Mark"
        };
        for (const char* type : strongTypes)
        {
            if (JasonAIObjectDerivesFromNameContaining(
                    reinterpret_cast<UObject*>(counselor),
                    type))
            {
                return true;
            }
        }
        return false;
    }

    bool GiveConvergenceMeleeWeapon(AActor* counselor)
    {
        if (!counselor)
            return false;
        const bool strong = IsStrongConvergenceCounselor(counselor);
        UClass* weaponClass = strong
            ? g_CounselorConvergenceMeleeClass
            : g_CounselorConvergenceMacheteClass;
        // A map can omit one pickup type. Preserve convergence with the other
        // cached melee class instead of leaving that counselor unarmed.
        if (!weaponClass)
            weaponClass = g_CounselorConvergenceMeleeClass
                ? g_CounselorConvergenceMeleeClass
                : g_CounselorConvergenceMacheteClass;
        if (!weaponClass ||
            !Memory::IsReadable(
                weaponClass,
                sizeof(UClass)))
        {
            return false;
        }

        HMODULE module = GetModuleHandle(nullptr);
        if (!module)
            return false;
        constexpr uintptr_t RVA_GiveStartingItem = 0x002EF980;
        uint8_t* target = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(module) + RVA_GiveStartingItem);
        const uint8_t expected[] = {
            0x48, 0x85, 0xD2, 0x0F, 0x84, 0xDF, 0x00, 0x00,
            0x00, 0x48, 0x89, 0x54, 0x24, 0x10, 0x57, 0x48
        };
        if (!Memory::IsReadable(target, sizeof(expected)) ||
            std::memcmp(target, expected, sizeof(expected)) != 0)
        {
            return false;
        }

        reinterpret_cast<GiveStartingItemFn>(target)(
            counselor,
            weaponClass);
        Logger::Debug(
            std::string("18L-BK convergence weapon assigned | counselor=") +
            JasonAISafeName(reinterpret_cast<UObject*>(counselor)) +
            " | tier=" + (strong ? "strong" : "standard") +
            " | weapon=" +
            JasonAISafeName(reinterpret_cast<UObject*>(weaponClass)));
        return true;
    }

    void DriveCounselorSweaterConvergence(ULONGLONG now)
    {
        if (now < g_NextCounselorConvergenceAt)
            return;

        if (!g_CounselorConvergenceActive)
        {
            bool sweaterAcquired = g_PermanentHumanSweaterLatched;
            for (int32_t i = 0;
                !sweaterAcquired &&
                i < g_JasonAITargetCount && i < 8;
                ++i)
            {
                AActor* counselor = g_JasonAITargets[i];
                sweaterAcquired =
                    IsValidatedLiveCounselorPawn(counselor) &&
                    HasPamelaSweater(counselor);
            }
            if (!sweaterAcquired)
            {
                g_NextCounselorConvergenceAt = now + 1501;
                return;
            }

            g_CounselorConvergenceActive = true;
            g_NextCounselorConvergenceAt = now;
            Logger::Success(
                "18L-BF sweater acquired: surviving counselor melee convergence armed");
        }

        // The stock JasonDeath interaction owns nearby counselors while Jason
        // is kneeling. Do not republish ordinary fight goals in that window.
        if (now < g_KillTeamFinalContextUntil)
        {
            // JasonDeath owns the native final interaction. Restore every
            // counselor brain before handing control to that stock sequence.
            ResumeAllConvergenceCounselorBrains();
            g_NextCounselorConvergenceAt = now + 2003;
            return;
        }
        if (!g_CounselorBlackboardNameResolutionComplete)
        {
            g_NextCounselorConvergenceAt = now + 2003;
            return;
        }

        const int32_t count = (std::min)(g_JasonAITargetCount, 8);
        if (count <= 0)
        {
            g_NextCounselorConvergenceAt = now + 4513;
            return;
        }

        // Prime the entire live AI roster when convergence first becomes
        // active.  The old cursor-only path armed one counselor per slice,
        // which allowed the remaining stock brains to keep looting or fleeing
        // until their turn arrived.  A single bounded pass gives every bot a
        // Jason target and one persistent native move request; subsequent
        // slices continue servicing one bot at a time, so this does not create
        // a per-frame ProcessEvent/path-command burst.
        FVector jasonLocation{};
        const bool haveJasonLocation =
            GetJasonAIActorLocation(g_JasonAIState.Jason, jasonLocation);

        // Re-publish the Jason combat keys for every already-armed counselor
        // on a slow lane.  A single cursor service is sufficient for routing,
        // but the stock tree can reclaim its flee/loot branch after it resumes
        // melee.  Refreshing the bounded roster together keeps all survivors
        // attacking without turning the main 167 ms route lane into a burst of
        // repeated path requests.
        if (now >= g_NextConvergenceCombatSweepAt)
        {
            g_NextConvergenceCombatSweepAt = now + 750;
            for (int32_t i = 0; i < count; ++i)
            {
                AActor* counselor = g_JasonAITargets[i];
                if (!counselor || counselor == g_LocalCounselorTarget ||
                    !IsValidatedLiveCounselorPawn(counselor) ||
                    !IsAIControlledCounselor(counselor) ||
                    !IsConvergenceCounselorArmed(counselor))
                {
                    continue;
                }

                SuppressConvergenceCounselorFear(counselor);
                UObject* controller = GetPawnControllerSafe(counselor);
                UObject* weapon = GetCounselorCurrentWeapon(counselor);
                if (!controller || !weapon)
                {
                    continue;
                }

                ArmKillTeamCounselorCombat(counselor);

                FVector counselorLocation{};
                if (!haveJasonLocation ||
                    !GetJasonAIActorLocation(counselor, counselorLocation))
                {
                    continue;
                }

                const float dx = counselorLocation.X - jasonLocation.X;
                const float dy = counselorLocation.Y - jasonLocation.Y;
                const float distanceSquared = dx * dx + dy * dy;
                const int32_t travelIndex =
                    FindConvergenceTravelCounselor(counselor);
                if (distanceSquared <= 300.0f * 300.0f)
                {
                    if (travelIndex >= 0)
                    {
                        SetCounselorBrainTickEnabled(counselor, true);
                        ForgetConvergenceTravelCounselor(counselor);
                    }
                }
                else if (travelIndex >= 0)
                {
                    // Keep the accepted path pointed at Jason as he moves.
                    // This is one bounded request per counselor every 750 ms,
                    // not a per-frame navigation rebuild.
                    IssueAIMoveToLocationOnGameThread(
                        controller,
                        jasonLocation,
                        100.0f,
                        "SweaterConvergenceJason");
                }
            }
        }
        int32_t rosterPrimed = 0;
        int32_t pathsPrimed = 0;
        for (int32_t i = 0; i < count; ++i)
        {
            AActor* counselor = g_JasonAITargets[i];
            if (!counselor || counselor == g_LocalCounselorTarget ||
                !IsValidatedLiveCounselorPawn(counselor) ||
                !IsAIControlledCounselor(counselor) ||
                IsConvergenceCounselorArmed(counselor))
            {
                continue;
            }

            GiveConvergenceMeleeWeapon(counselor);
            RememberConvergenceCounselor(counselor);
            SuppressConvergenceCounselorFear(counselor);

            UObject* controller = GetPawnControllerSafe(counselor);
            UObject* blackboard = controller
                ? GetCounselorBlackboardCached(controller)
                : nullptr;
            UObject* weapon = GetCounselorCurrentWeapon(counselor);
            if (!weapon && blackboard && g_SCWeaponNameIndex >= 0)
            {
                weapon = GetBlackboardObject(blackboard, g_SCWeaponNameIndex);
            }
            if (!weapon)
            {
                continue;
            }

            ArmKillTeamCounselorCombat(counselor);
            ++rosterPrimed;

            if (!controller || !haveJasonLocation)
            {
                continue;
            }

            FVector counselorLocation{};
            if (!GetJasonAIActorLocation(counselor, counselorLocation))
            {
                continue;
            }
            const float dx = counselorLocation.X - jasonLocation.X;
            const float dy = counselorLocation.Y - jasonLocation.Y;
            const float distanceSquared = dx * dx + dy * dy;
            if (!std::isfinite(distanceSquared) ||
                distanceSquared <= 300.0f * 300.0f)
            {
                continue;
            }

            if (SetCounselorBrainTickEnabled(counselor, false))
            {
                RememberConvergenceTravelCounselor(counselor);
                IssueAIMoveToLocationOnGameThread(
                    controller,
                    jasonLocation,
                    100.0f,
                    "SweaterConvergenceJason");
                ++pathsPrimed;
                Logger::Success(
                    "18L-BH counselor exclusive Jason travel engaged | pawn=" +
                    JasonAISafeName(reinterpret_cast<UObject*>(counselor)));
            }
        }
        if (rosterPrimed > 0)
        {
            Logger::Success(
                "18L-BF counselor convergence roster primed | armed=" +
                std::to_string(rosterPrimed) +
                " | directPaths=" + std::to_string(pathsPrimed));
        }

        AActor* selected = nullptr;
        bool firstArm = false;
        for (int32_t attempt = 0; attempt < count; ++attempt)
        {
            const int32_t index =
                (g_CounselorConvergenceCursor + attempt) % count;
            AActor* counselor = g_JasonAITargets[index];
            if (!counselor || counselor == g_LocalCounselorTarget ||
                !IsValidatedLiveCounselorPawn(counselor) ||
                !IsAIControlledCounselor(counselor))
            {
                continue;
            }
            if (!IsConvergenceCounselorArmed(counselor))
            {
                selected = counselor;
                firstArm = true;
                g_CounselorConvergenceCursor = (index + 1) % count;
                break;
            }
            if (!selected)
            {
                selected = counselor;
                g_CounselorConvergenceCursor = (index + 1) % count;
            }
        }

        if (!selected)
        {
            g_NextCounselorConvergenceAt = now + 4513;
            return;
        }

        if (firstArm)
        {
            GiveConvergenceMeleeWeapon(selected);
            RememberConvergenceCounselor(selected);
        }

        // Service exactly one counselor per slice. With the 167 ms cadence
        // below, a full six-bot roster is refreshed in about 1.00 s.
        // This is fast enough to prevent the stock fear/loot services from
        // reclaiming individual bots without issuing a burst of paths or
        // ProcessEvent calls on a single frame.
        SuppressConvergenceCounselorFear(selected);

        UObject* controller = GetPawnControllerSafe(selected);
        UObject* blackboard = controller
            ? GetCounselorBlackboardCached(controller)
            : nullptr;
        UObject* weapon = GetCounselorCurrentWeapon(selected);
        if (!weapon && blackboard && g_SCWeaponNameIndex >= 0)
            weapon = GetBlackboardObject(blackboard, g_SCWeaponNameIndex);
        if (!weapon && !firstArm)
        {
            GiveConvergenceMeleeWeapon(selected);
            weapon = GetCounselorCurrentWeapon(selected);
            if (!weapon && blackboard && g_SCWeaponNameIndex >= 0)
                weapon = GetBlackboardObject(
                    blackboard,
                    g_SCWeaponNameIndex);
        }

        if (weapon)
        {
            // Publish fight state while the brain is still suspended so its
            // first resumed behavior-tree frame sees only the Jason branch.
            ArmKillTeamCounselorCombat(selected);

            FVector counselorLocation{};
            FVector jasonLocation{};
            const bool haveLocations =
                GetJasonAIActorLocation(selected, counselorLocation) &&
                GetJasonAIActorLocation(
                    g_JasonAIState.Jason,
                    jasonLocation);
            const float dx = haveLocations
                ? counselorLocation.X - jasonLocation.X
                : 0.0f;
            const float dy = haveLocations
                ? counselorLocation.Y - jasonLocation.Y
                : 0.0f;
            const float distanceSquared = dx * dx + dy * dy;
            const bool wasTraveling =
                FindConvergenceTravelCounselor(selected) >= 0;
            // Hysteresis avoids toggling the brain at the edge of melee range:
            // begin exclusive travel beyond 3 m, but do not hand control back
            // until the bot is within 2.2 m of Jason. The earlier 3.25 m
            // release allowed the stock tree to flee before melee was viable.
            const bool shouldTravel = haveLocations &&
                std::isfinite(distanceSquared) &&
                distanceSquared >
                    (wasTraveling
                        ? 220.0f * 220.0f
                        : 300.0f * 300.0f);

            if (shouldTravel)
            {
                if (!wasTraveling)
                {
                    if (SetCounselorBrainTickEnabled(selected, false))
                    {
                        RememberConvergenceTravelCounselor(selected);
                        Logger::Success(
                            "18L-BH counselor exclusive Jason travel engaged | pawn=" +
                            JasonAISafeName(
                                reinterpret_cast<UObject*>(selected)));
                    }
                    else if (firstArm)
                    {
                        Logger::Error(
                            "18L-BH counselor brain suspension unavailable; direct travel remains best-effort | pawn=" +
                            JasonAISafeName(
                                reinterpret_cast<UObject*>(selected)));
                    }
                }
                bool recoveryPulse = false;
                const int32_t travelIndex =
                    FindConvergenceTravelCounselor(selected);
                if (travelIndex >= 0)
                {
                    if (g_ConvergenceTravelRecoveryUntil[travelIndex] != 0)
                    {
                        if (now <
                            g_ConvergenceTravelRecoveryUntil[travelIndex])
                        {
                            recoveryPulse = true;
                        }
                        else
                        {
                            SetCounselorBrainTickEnabled(selected, false);
                            g_ConvergenceTravelRecoveryUntil[travelIndex] = 0;
                            g_ConvergenceTravelHaveLocation[travelIndex] = false;
                            g_ConvergenceTravelLastProgressAt[travelIndex] = now;
                        }
                    }

                    if (!recoveryPulse)
                    {
                        const FVector& previous =
                            g_ConvergenceTravelLastLocations[travelIndex];
                        const float moveX = counselorLocation.X - previous.X;
                        const float moveY = counselorLocation.Y - previous.Y;
                        const bool progressed =
                            !g_ConvergenceTravelHaveLocation[travelIndex] ||
                            moveX * moveX + moveY * moveY > 80.0f * 80.0f;
                        if (progressed)
                        {
                            g_ConvergenceTravelLastLocations[travelIndex] =
                                counselorLocation;
                            g_ConvergenceTravelLastProgressAt[travelIndex] = now;
                            g_ConvergenceTravelHaveLocation[travelIndex] = true;
                        }
                        else if (g_ConvergenceTravelLastProgressAt[travelIndex] != 0 &&
                            now >=
                                g_ConvergenceTravelLastProgressAt[travelIndex] +
                                    3500)
                        {
                            // A closed door or smart-link traversal can require
                            // the counselor behavior tree. Give only the stuck
                            // pawn a short native-navigation pulse, then reclaim
                            // exclusive travel; no roster scan or group burst.
                            SetCounselorBrainTickEnabled(selected, true);
                            g_ConvergenceTravelRecoveryUntil[travelIndex] =
                                now + 1001;
                            recoveryPulse = true;
                            Logger::Debug(
                                "18L-BI counselor travel obstruction recovery pulse | pawn=" +
                                JasonAISafeName(
                                    reinterpret_cast<UObject*>(selected)));
                        }
                    }
                }

                if (!recoveryPulse)
                {
                    IssueAIMoveToLocationOnGameThread(
                        controller,
                        jasonLocation,
                        100.0f,
                        "SweaterConvergenceJason");
                }
            }
            else if (wasTraveling)
            {
                // The direct path has delivered the bot. Resume only here so
                // the mature stock melee animation/attack loop can take over.
                SetCounselorBrainTickEnabled(selected, true);
                ForgetConvergenceTravelCounselor(selected);
                Logger::Success(
                    "18L-BH counselor reached Jason; native melee brain resumed | pawn=" +
                    JasonAISafeName(reinterpret_cast<UObject*>(selected)));
            }
            if (firstArm)
            {
                Logger::Success(
                    "18L-BF counselor armed and sent to Jason | pawn=" +
                    JasonAISafeName(
                        reinterpret_cast<UObject*>(selected)));
            }
        }
        else if (blackboard)
        {
            // If the cached axe was not present yet, use the stock weapon-seek
            // branch. A later slow pass promotes the bot to melee combat.
            SetBlackboardObject(
                blackboard,
                g_JasonCharacterNameIndex,
                reinterpret_cast<UObject*>(g_JasonAIState.Jason));
            SetBlackboardBool(
                blackboard,
                g_SeekWeaponWhileFleeingNameIndex,
                true);
            SetBlackboardBool(
                blackboard,
                g_ShouldHideNameIndex,
                false);
        }

        // Always rotate one bot at a time. Each bot's Jason target, combat keys,
        // weapon publication, fear suppression, and path are therefore renewed
        // before the stock behavior tree can settle back into flee/loot/hide.
        g_NextCounselorConvergenceAt = now + 167;
    }

    void ArmKillTeamAxeTravel(AActor* helper)
    {
        if (!helper ||
            g_ShouldFleeKillerNameIndex < 0 ||
            g_ShouldFightBackNameIndex < 0 ||
            g_ShouldArmedFightBackNameIndex < 0 ||
            g_ShouldMeleeFightBackNameIndex < 0 ||
            g_SeekWeaponWhileFleeingNameIndex < 0)
        {
            return;
        }

        UObject* controller = GetPawnControllerSafe(helper);
        UObject* blackboard = controller
            ? GetCounselorBlackboardCached(controller)
            : nullptr;
        if (!blackboard)
            return;

        // The kill-team route owns Tommy while he travels to the fixed axe.
        // Disable the stock fight/flee/weapon-looting branches that otherwise
        // replace our native MoveTo every behavior-tree tick.
        SetBlackboardBool(
            blackboard,
            g_ShouldFightBackNameIndex,
            false);
        SetBlackboardBool(
            blackboard,
            g_ShouldArmedFightBackNameIndex,
            false);
        SetBlackboardBool(
            blackboard,
            g_ShouldMeleeFightBackNameIndex,
            false);
        SetBlackboardBool(
            blackboard,
            g_SeekWeaponWhileFleeingNameIndex,
            false);
        SetBlackboardBool(
            blackboard,
            g_ShouldFleeKillerNameIndex,
            false);
    }

    void HoldKillTeamCounselorForFinalAction(AActor* helper)
    {
        if (!helper ||
            g_ShouldFleeKillerNameIndex < 0 ||
            g_ShouldFightBackNameIndex < 0 ||
            g_ShouldArmedFightBackNameIndex < 0 ||
            g_ShouldMeleeFightBackNameIndex < 0 ||
            g_SeekWeaponWhileFleeingNameIndex < 0)
        {
            return;
        }
        UObject* controller = GetPawnControllerSafe(helper);
        UObject* blackboard = controller
            ? GetCounselorBlackboardCached(controller)
            : nullptr;
        if (!blackboard)
            return;

        // The validated JasonDeath context owns Tommy now. Disable every
        // ordinary fight/flee/loot branch so a melee task cannot replace the
        // stock final-interaction MoveTo or input.
        SetBlackboardBool(blackboard, g_ShouldFightBackNameIndex, false);
        SetBlackboardBool(blackboard, g_ShouldArmedFightBackNameIndex, false);
        SetBlackboardBool(blackboard, g_ShouldMeleeFightBackNameIndex, false);
        SetBlackboardBool(blackboard, g_SeekWeaponWhileFleeingNameIndex, false);
        SetBlackboardBool(blackboard, g_ShouldFleeKillerNameIndex, false);
        if (g_ShouldHideNameIndex >= 0)
            SetBlackboardBool(blackboard, g_ShouldHideNameIndex, false);
        if (g_ShouldOrientTowardKillerNameIndex >= 0)
        {
            SetBlackboardBool(
                blackboard,
                g_ShouldOrientTowardKillerNameIndex,
                true);
        }
    }

    bool DispatchSweaterAbility(AActor* counselor)
    {
        if (!counselor || !counselor->Class)
            return false;

        UObject* candidates[] =
        {
            reinterpret_cast<UObject*>(counselor),
            ReadReflectedObjectProperty(
                reinterpret_cast<UObject*>(counselor),
                "SweaterAbility"),
            ReadReflectedObjectProperty(
                reinterpret_cast<UObject*>(counselor),
                "ActiveAbility")
        };
        for (UObject* candidate : candidates)
        {
            if (!candidate || !candidate->Class ||
                !Memory::IsReadable(candidate, sizeof(UObject)))
            {
                continue;
            }
            UFunction* useAbility = FindFunctionInHierarchyByName(
                candidate->Class,
                "SERVER_UseAbility");
            if (!useAbility)
                continue;
            if (SafeProcessEventCall(
                    reinterpret_cast<uintptr_t>(candidate),
                    candidate,
                    useAbility,
                    nullptr))
            {
                return true;
            }
        }
        return false;
    }

    bool IsJasonNativelyStunned(AActor* jason)
    {
        if (!jason || !Memory::IsReadable(jason, sizeof(UObject)))
            return false;

        HMODULE module = GetModuleHandle(nullptr);
        if (!module)
            return false;

        using IsStunnedFn = bool(__fastcall*)(AActor*);
        IsStunnedFn isStunned = reinterpret_cast<IsStunnedFn>(
            reinterpret_cast<uintptr_t>(module) + 0x002F00E0);
        return Memory::IsReadable(reinterpret_cast<void*>(isStunned), 1) &&
            isStunned(jason);
    }

    void RestoreRepeatJasonDeathOpportunityIfNeeded(ULONGLONG now)
    {
        if (g_RepeatJasonKillStanceAt == 0 ||
            now < g_RepeatJasonKillStanceAt)
        {
            return;
        }

        AActor* jason = g_JasonAIState.Jason;
        AActor* existingContext = nullptr;
        UObject* existingComponent = nullptr;
        if (TryGetValidatedJasonDeathContext(
                jason,
                existingContext,
                existingComponent,
                now))
        {
            // Stock recreated the kneel itself; never duplicate it.
            g_RepeatJasonKillStanceAt = 0;
            g_RepeatJasonKillStanceDeadline = 0;
            g_RepeatJasonKillStanceAttempts = 0;
            return;
        }

        if (!jason || !jason->Class ||
            now > g_RepeatJasonKillStanceDeadline ||
            g_RepeatJasonKillStanceAttempts >= 3)
        {
            g_RepeatJasonKillStanceAt = 0;
            g_RepeatJasonKillStanceDeadline = 0;
            return;
        }

        // Do not bypass the mask prerequisite or invent a death opportunity.
        // bMaskOn is a reflected SCKillerCharacter field; resolve it only in
        // this rare repeat-use window.
        if (!FindPropertyInHierarchyByName(jason->Class, "bMaskOn") ||
            ReadReflectedBoolByte(
                reinterpret_cast<UObject*>(jason),
                "bMaskOn"))
        {
            g_RepeatJasonKillStanceAt = 0;
            g_RepeatJasonKillStanceDeadline = 0;
            return;
        }

        if (!IsJasonNativelyStunned(jason))
        {
            g_RepeatJasonKillStanceAt = now + 750;
            return;
        }

        UFunction* enterKillStance = FindFunctionInHierarchyByName(
            jason->Class,
            "SERVER_EnterKillStance");
        if (!enterKillStance)
        {
            enterKillStance = FindFunctionInHierarchyByName(
                jason->Class,
                "EnterKillStance");
        }
        const bool dispatched = enterKillStance && SafeProcessEventCall(
            reinterpret_cast<uintptr_t>(jason),
            jason,
            enterKillStance,
            nullptr);
        ++g_RepeatJasonKillStanceAttempts;
        g_RepeatJasonKillStanceAt = now + 1200;
        Logger::Success(
            std::string("18L-BE repeat Jason death opportunity dispatched through stock kill-stance route | attempt=") +
            std::to_string(g_RepeatJasonKillStanceAttempts) +
            " | dispatched=" + (dispatched ? "true" : "false"));
    }

    void RecoverOrphanJasonStunIfNeeded(
        AActor* human,
        AActor* jason,
        ULONGLONG now)
    {
        // A real sweater/mask transition publishes JasonDeath_C at +0x12D8
        // and is handled above at the responsive final-action cadence. An
        // ordinary stun that remains active without that context is allowed
        // twelve seconds to finish naturally, then released once so an
        // external instant-stun modifier cannot strand Jason indefinitely.
        if (!IsJasonNativelyStunned(jason))
        {
            g_OrphanJasonStunStartedAt = 0;
            return;
        }
        if (g_OrphanJasonStunStartedAt == 0)
        {
            g_OrphanJasonStunStartedAt = now;
            return;
        }
        if (now - g_OrphanJasonStunStartedAt < 12000)
            return;

        UFunction* endStun = jason && jason->Class
            ? FindFunctionInHierarchyByName(jason->Class, "EndStun")
            : nullptr;
        const bool released = endStun && SafeProcessEventCall(
            reinterpret_cast<uintptr_t>(jason),
            jason,
            endStun,
            nullptr);

        g_OrphanJasonStunStartedAt = 0;
        g_KillTeamFinalContextUntil = 0;
        g_KillTeamFinalRecoveryCooldownUntil = now + 5000;
        g_KillTeamFinalInteractionDispatched = false;
        g_KillTeamFinalInteractionPending = false;
        g_KillTeamFinalInteractionAttempts = 0;
        g_KillTeamFinalInteractionStartedAt = 0;
        g_KillTeamFinalSequenceStartedAt = 0;
        g_KillTeamSweaterUseDispatched = false;
        g_NextSweaterUseAt = now + 1000;
        if (g_PermanentHumanSweaterLatched &&
            human == g_PermanentHumanSweaterCarrier)
        {
            RearmPermanentHumanSweater(human, true);
        }
        g_JasonAIState.Target = nullptr;
        ResetStuckSamplingAfterNativeInteraction(now);
        Logger::Error(
            std::string("18L-BB orphan Jason stun released without JasonDeath context | released=") +
            (released ? "true" : "false"));
    }

    void AbortFailedJasonKillSequence(
        AActor* jason,
        AActor* killObject,
        ULONGLONG now)
    {
        // Once the native paired kill has consumed its JasonDeath context,
        // never run recovery cleanup: EndStun would revive the dead Jason.
        if (g_KillTeamFinalInteractionCommitted)
            return;

        const bool retryJasonDeath =
            g_PermanentHumanSweaterLatched &&
            jason &&
            IsJasonNativelyStunned(jason);
        bool destroyedContext = false;
        if (killObject && killObject->Class &&
            Memory::IsReadable(killObject, sizeof(UObject)))
        {
            UFunction* destroyActor = FindFunctionInHierarchyByName(
                killObject->Class,
                "K2_DestroyActor");
            destroyedContext = destroyActor &&
                SafeProcessEventCall(
                    reinterpret_cast<uintptr_t>(killObject),
                    killObject,
                    destroyActor,
                    nullptr);
        }

        if (jason && Memory::IsReadable(jason, 0x12E0))
        {
            AActor** finalContextField = reinterpret_cast<AActor**>(
                reinterpret_cast<uintptr_t>(jason) + 0x12D8);
            if (*finalContextField == killObject)
                *finalContextField = nullptr;

            // K2_DestroyActor runs the stock context cleanup. For a sweater
            // retry, deliberately keep Jason's native stun alive so the next
            // tick can recreate JasonDeath_C instead of returning him to
            // ordinary chase/grab AI. Non-sweater failures retain the older
            // bounded EndStun recovery.
            UFunction* endStun = jason->Class
                ? FindFunctionInHierarchyByName(jason->Class, "EndStun")
                : nullptr;
            if (endStun && !retryJasonDeath)
            {
                SafeProcessEventCall(
                    reinterpret_cast<uintptr_t>(jason),
                    jason,
                    endStun,
                    nullptr);
            }
        }

        g_KillTeamFinalContextUntil = 0;
        g_KillTeamFinalRecoveryCooldownUntil = now + 10000;
        g_KillTeamFinalInteractionDispatched = false;
        g_KillTeamFinalInteractionPending = false;
        g_KillTeamFinalInteractionAttempts = 0;
        g_KillTeamFinalInteractionStartedAt = 0;
        g_KillTeamFinalSequenceStartedAt = 0;
        g_KillTeamFinalMoveFailures = 0;
        g_KillTeamFinalRepositionAttempted = false;
        g_KillTeamPendingFinalContext = nullptr;
        g_KillTeamPendingFinalComponent = nullptr;
        g_LastAcceptedFinalContext = nullptr;
        g_LastAcceptedFinalComponent = nullptr;
        g_LastAcceptedFinalKillComponent = nullptr;
        g_KillTeamSweaterUseDispatched = false;
        g_NextSweaterUseAt = now + 1000;
        if (retryJasonDeath)
        {
            g_RepeatJasonKillStanceAt = now + 750;
            g_RepeatJasonKillStanceDeadline = now + 12000;
            g_RepeatJasonKillStanceAttempts = 0;
            // Keep the killer-route tick in the final-interaction lane while
            // the native kneel is being re-armed.  Without this short hold,
            // the next controller tick can immediately hand Jason back to
            // vehicle/hiding/grab AI during the gap between destroying the
            // failed JasonDeath_C and recreating the stock stance.
            g_KillTeamFinalContextUntil = now + 12000;
            g_KillTeamFinalRecoveryCooldownUntil = now + 500;
            Logger::Success(
                "18L-BE failed Jason kill retained native stun; retrying JasonDeath stance instead of restoring chase AI");
        }
        if (g_PermanentHumanSweaterLatched &&
            g_PermanentHumanSweaterCarrier &&
            Memory::IsReadable(
                g_PermanentHumanSweaterCarrier,
                sizeof(UObject)))
        {
            // A failed Tommy final action must return the stock kill setup to
            // a usable state. Re-arm the already-earned human sweater here,
            // once per failed sequence, instead of polling or auto-activating.
            RearmPermanentHumanSweater(
                g_PermanentHumanSweaterCarrier,
                true);
            Logger::Success(
                "18L-BA failed Jason kill released; human sweater can be activated again");
        }
        g_JasonAIState.Target = nullptr;
        ResetStuckSamplingAfterNativeInteraction(now);
        Logger::Error(
            std::string("18L-AY failed Jason-kill interaction released; normal AI restored | contextDestroyed=") +
            (destroyedContext ? "true" : "false") +
            " | retryStance=" + (retryJasonDeath ? "true" : "false"));
    }

    bool DriveCounselorFinalKill(
        AActor* finisher,
        bool allowAIMovement,
        ULONGLONG now)
    {
        AActor* jason = g_JasonAIState.Jason;
        UObject* manager = GetCounselorInteractionManager(finisher);
        if (!jason || !manager)
            return false;

        // EnterKillStance creates the stock JasonDeath context actor here
        // after the sweater/mask prerequisites are satisfied.
        AActor* killObject = nullptr;
        UObject* component = nullptr;
        if (!TryGetValidatedJasonDeathContext(
                jason,
                killObject,
                component,
                now))
            return false;

        // The stock death-context owns both participants from this point.
        // Keep custom Jason chase and helper behavior out until it completes.
        g_KillTeamFinalContextUntil = now + 15000;

        UObject** locked = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(manager) + 0x230);
        if (Memory::IsReadable(locked, sizeof(UObject*)) && *locked)
        {
            const bool exactFinalLock = IsFinalContextLockMatch(
                *locked,
                killObject,
                component);
            if (exactFinalLock &&
                killObject == g_KillTeamPendingFinalContext &&
                component == g_KillTeamPendingFinalComponent &&
                !g_KillTeamFinalInteractionDispatched &&
                (g_KillTeamFinalInteractionPending ||
                    g_KillTeamFinalInteractionAttempts > 0))
            {
                Logger::Success(
                    "18L-AW counselor final interaction confirmed by native lock | finisher=" +
                    JasonAISafeName(reinterpret_cast<UObject*>(finisher)));
                g_KillTeamFinalInteractionDispatched = true;
                g_KillTeamFinalInteractionPending = false;
            }
            if (!exactFinalLock &&
                g_KillTeamFinalSequenceStartedAt != 0 &&
                now - g_KillTeamFinalSequenceStartedAt >= 22000)
            {
                AbortFailedJasonKillSequence(jason, killObject, now);
            }
            g_KillTeamHelperNativeBusyUntil = now + 2500;
            return true;
        }
        if (g_KillTeamFinalInteractionDispatched)
            return true;

        if (g_KillTeamFinalSequenceStartedAt != 0 &&
            now - g_KillTeamFinalSequenceStartedAt >= 22000)
        {
            AbortFailedJasonKillSequence(jason, killObject, now);
            return true;
        }

        // AttemptInteract is a void native input dispatch. Confirm it through
        // the counselor interaction-manager lock and use short, bounded retry
        // bursts instead of assuming the first input was consumed. This is
        // only active while the validated JasonDeath_C context exists.
        if (g_KillTeamFinalInteractionAttempts >= 4)
        {
            if (g_KillTeamFinalInteractionStartedAt != 0 &&
                now - g_KillTeamFinalInteractionStartedAt < 8000)
            {
                return true;
            }
            g_KillTeamFinalInteractionAttempts = 0;
            g_KillTeamFinalInteractionStartedAt = now;
            g_KillTeamFinalInteractionPending = false;
            g_KillTeamPendingFinalContext = nullptr;
            g_KillTeamPendingFinalComponent = nullptr;
        }

        FVector* interactionLocation = reinterpret_cast<FVector*>(
            reinterpret_cast<uintptr_t>(component) +
            Offsets::Scene_ComponentToWorld +
            Offsets::FTransform_Translation);
        FVector finisherLocation{};
        if (!Memory::IsReadable(interactionLocation, sizeof(FVector)) ||
            !GetJasonAIActorLocation(finisher, finisherLocation))
        {
            return true;
        }
        const float dx = interactionLocation->X - finisherLocation.X;
        const float dy = interactionLocation->Y - finisherLocation.Y;
        const float distanceSquared = dx * dx + dy * dy;
        if (!std::isfinite(distanceSquared))
            return true;
        if (distanceSquared > 165.0f * 165.0f)
        {
            // Never commandeer the local player's movement. Keeping the
            // final context alive lets the sweater wearer walk into range and
            // complete the same native interaction; AI finishers may path or
            // use the existing bounded nav fallback.
            if (!allowAIMovement)
                return true;

            if (now >= g_NextFinalKillInteractAt)
            {
                UObject* controller = GetPawnControllerSafe(finisher);
                const bool moveAccepted =
                    IssueAIMoveToLocationOnGameThread(
                    controller,
                    *interactionLocation,
                    90.0f,
                    "CounselorFinalKill");
                if (!moveAccepted)
                    ++g_KillTeamFinalMoveFailures;

                const bool pathFailed = !moveAccepted;
                const bool pathTooSlow =
                    g_KillTeamFinalSequenceStartedAt != 0 &&
                    now - g_KillTeamFinalSequenceStartedAt >= 2500;
                if (!g_KillTeamFinalRepositionAttempted &&
                    (pathFailed || pathTooSlow))
                {
                    bool repositioned = TeleportCounselorNearObjective(
                        finisher,
                        controller,
                        *interactionLocation);
                    if (!repositioned)
                    {
                        FVector jasonLocation{};
                        if (GetJasonAIActorLocation(jason, jasonLocation))
                        {
                            repositioned = TeleportCounselorNearObjective(
                                finisher,
                                controller,
                                jasonLocation);
                        }
                    }
                    g_KillTeamFinalRepositionAttempted = true;
                    Logger::Success(
                        std::string("18L-AY AI counselor final nav fallback | repositioned=") +
                        (repositioned ? "true" : "false") +
                        " | moveFailures=" +
                        std::to_string(g_KillTeamFinalMoveFailures));
                    g_NextFinalKillInteractAt = now + 300;
                }
                else
                {
                    g_NextFinalKillInteractAt = now + 1000;
                }
            }
            return true;
        }

        if (now < g_NextFinalKillInteractAt)
            return true;

        UObject* controller = GetPawnControllerSafe(finisher);
        UFunction* stopMovement = controller && controller->Class
            ? FindFunctionInHierarchyByName(
                controller->Class,
                "StopMovement")
            : nullptr;
        if (stopMovement)
        {
            SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(controller),
                controller,
                stopMovement,
                nullptr);
        }

        HMODULE module = GetModuleHandle(nullptr);
        uintptr_t managerVTable = *reinterpret_cast<uintptr_t*>(manager);
        uintptr_t* attemptSlot = reinterpret_cast<uintptr_t*>(
            managerVTable + 0x430);
        const uintptr_t expected = module
            ? reinterpret_cast<uintptr_t>(module) + 0x0025AC50
            : 0;
        if (!managerVTable ||
            !Memory::IsReadable(attemptSlot, sizeof(uintptr_t)) ||
            *attemptSlot != expected)
        {
            return false;
        }
        reinterpret_cast<AttemptInteractFn>(*attemptSlot)(
            manager,
            component,
            1,
            true);
        if (g_KillTeamFinalInteractionStartedAt == 0)
            g_KillTeamFinalInteractionStartedAt = now;
        ++g_KillTeamFinalInteractionAttempts;
        g_KillTeamFinalInteractionPending = true;
        g_KillTeamPendingFinalContext = killObject;
        g_KillTeamPendingFinalComponent = component;
        g_KillTeamHelperNativeBusyUntil = now + 500;
        g_NextFinalKillInteractAt = now + 850;

        const bool lockedImmediately =
            Memory::IsReadable(locked, sizeof(UObject*)) && *locked;
        if (lockedImmediately && IsFinalContextLockMatch(
                *locked,
                killObject,
                component))
        {
            g_KillTeamFinalInteractionDispatched = true;
            g_KillTeamFinalInteractionPending = false;
            g_KillTeamHelperNativeBusyUntil = now + 3000;
            Logger::Success(
                "18L-AW counselor final interaction accepted immediately by native lock | finisher=" +
                JasonAISafeName(reinterpret_cast<UObject*>(finisher)));
        }
        else
        {
            Logger::Debug(
                "18L-AW counselor pressed native final interaction; awaiting lock | attempt=" +
                std::to_string(g_KillTeamFinalInteractionAttempts) +
                " | finisher=" +
                JasonAISafeName(reinterpret_cast<UObject*>(finisher)));
        }
        return true;
    }

    void ResetKillTeamRoute(KillTeamRoute route, AActor* helper)
    {
        if (g_KillTeamRoute == route && g_KillTeamHelper == helper)
            return;
        g_KillTeamRoute = route;
        g_KillTeamHelper = helper;
        g_KillTeamSweater = nullptr;
        g_KillTeamAxe = nullptr;
        g_KillTeamMask = nullptr;
        g_KillTeamHelperArmed = false;
        g_KillTeamHelperProtected = false;
        g_KillTeamMaskAcquired = false;
        g_KillTeamSweaterUseDispatched = false;
        g_KillTeamAxeDiscoveryAttempted = false;
        g_KillTeamAxePursuitStartedAt = 0;
        g_KillTeamAxeLastProgressAt = 0;
        g_KillTeamAxeBestDistance = FLT_MAX;
        g_KillTeamHelperNativeBusyUntil = 0;
        g_KillTeamHelperStableObservations = 0;
        g_KillTeamFinalContextUntil = 0;
        g_KillTeamFinalInteractionDispatched = false;
        g_KillTeamFinalInteractionPending = false;
        g_KillTeamFinalInteractionAttempts = 0;
        g_KillTeamFinalInteractionStartedAt = 0;
        g_KillTeamFinalSequenceStartedAt = 0;
        g_KillTeamFinalRecoveryCooldownUntil = 0;
        g_KillTeamFinalMoveFailures = 0;
        g_KillTeamFinalRepositionAttempted = false;
        g_KillTeamLastCancelledInteraction = nullptr;
        g_NextKillTeamInteractionCancelAt = 0;
        g_KillTeamPendingFinalContext = nullptr;
        g_KillTeamPendingFinalComponent = nullptr;
        g_LastRejectedFinalContext = nullptr;
        g_LastAcceptedFinalContext = nullptr;
        g_LastAcceptedFinalComponent = nullptr;
        g_LastAcceptedFinalKillComponent = nullptr;
        // Do not discard an earned human sweater merely because Tommy has
        // not spawned yet or a helper pawn was temporarily unavailable. The
        // permanent sweater state is cleared by the world/session reset paths.
        g_NextFinalContextDiagnosticAt = 0;
        g_NextKillTeamDiscoveryAt = 0;
        g_NextKillTeamInteractAt = 0;
        g_NextHelperKnifeGrantAt = 0;
        g_NextSweaterUseAt = 0;
        g_NextFinalKillInteractAt = 0;
        g_NextKillTeamMoveAt = 0;
        g_NextKillTeamFollowAt = 0;
        g_KillTeamMoveTarget = nullptr;
        g_TommyJasonObjectiveOwner = nullptr;
        g_NextTommyJasonObjectiveRepairAt = 0;
        g_OrphanJasonStunStartedAt = 0;
        if (route != KillTeamRoute::None && helper)
        {
            Logger::Success(
                std::string("18L-AN Jason kill-team route armed | mode=") +
                (route == KillTeamRoute::HumanTommyFemaleHelper
                    ? "human-Tommy/female-sweater-bot"
                    : "human-counselor/AI-Tommy") +
                " | helper=" +
                JasonAISafeName(reinterpret_cast<UObject*>(helper)));
        }
    }

    void DriveLegacyCounselorKillTeamAI(ULONGLONG now)
    {
        if (now < g_NextKillTeamTickAt)
            return;

        // Until a Tommy/sweater route exists this is a lightweight target-list
        // check. Once active, the
        // counselor blackboard owns pursuit; this lane is only a sparse repair
        // path until a validated final-kill context needs short interaction
        // retries.
        // World-object discovery is separately limited to one pass per sixty
        // seconds below; it must never run at this control cadence.
        AActor* human = g_LocalCounselorTarget;
        if (!IsValidatedLiveCounselorPawn(human))
        {
            ResetKillTeamRoute(KillTeamRoute::None, nullptr);
            g_NextKillTeamTickAt = now + 60000;
            return;
        }

        const bool humanIsTommy = IsHunterCounselor(human);
        // Latch and restore the human's earned sweater independently of
        // Tommy's spawn lifecycle. This is a direct byte/property check, not
        // a world scan, and prevents the first Pamela use from being consumed
        // while the helper route is still absent.
        if (!humanIsTommy)
            MaintainPermanentHumanSweater(human);

        g_NextKillTeamTickAt = now + 4173;
        AActor* helper = nullptr;
        KillTeamRoute wantedRoute = KillTeamRoute::None;
        for (int32_t i = 0; i < g_JasonAITargetCount; ++i)
        {
            AActor* candidate = g_JasonAITargets[i];
            if (!candidate || candidate == human ||
                !IsValidatedLiveCounselorPawn(candidate))
            {
                continue;
            }
            if (humanIsTommy &&
                !IsHunterCounselor(candidate) &&
                IsFemaleCounselor(candidate))
            {
                helper = candidate;
                wantedRoute = KillTeamRoute::HumanTommyFemaleHelper;
                break;
            }
            if (!humanIsTommy && IsHunterCounselor(candidate))
            {
                helper = candidate;
                wantedRoute = KillTeamRoute::HumanSweaterAITommy;
                break;
            }
        }

        const bool routeChanged =
            g_KillTeamRoute != wantedRoute || g_KillTeamHelper != helper;
        ResetKillTeamRoute(wantedRoute, helper);

        AActor* previousFinalContext = g_LastAcceptedFinalContext;
        AActor* deathContext = nullptr;
        UObject* deathComponent = nullptr;
        const bool finalContextActive =
            now >= g_KillTeamFinalRecoveryCooldownUntil &&
            TryGetValidatedJasonDeathContext(
                g_JasonAIState.Jason,
                deathContext,
                deathComponent,
                now);
        if (finalContextActive)
        {
            g_OrphanJasonStunStartedAt = 0;
            g_KillTeamFinalContextUntil = now + 15000;
            g_NextKillTeamTickAt = now + 350;

            // The stock interaction manager is present on every counselor,
            // not only Tommy. Prefer the local player when they are already
            // beside the kneeling Jason (including the sweater wearer), then
            // use the configured helper or nearest live counselor as an AI
            // fallback. Local movement remains entirely player controlled.
            AActor* finisher = nullptr;
            bool allowAIMovement = false;
            FVector* interactionLocation = reinterpret_cast<FVector*>(
                reinterpret_cast<uintptr_t>(deathComponent) +
                Offsets::Scene_ComponentToWorld +
                Offsets::FTransform_Translation);
            FVector humanLocation{};
            if (GetCounselorInteractionManager(human) &&
                Memory::IsReadable(interactionLocation, sizeof(FVector)) &&
                GetJasonAIActorLocation(human, humanLocation))
            {
                const float dx = interactionLocation->X - humanLocation.X;
                const float dy = interactionLocation->Y - humanLocation.Y;
                if (dx * dx + dy * dy <= 350.0f * 350.0f)
                    finisher = human;
            }
            if (!finisher && helper &&
                GetCounselorInteractionManager(helper))
            {
                finisher = helper;
                allowAIMovement = true;
            }
            if (!finisher)
            {
                float bestDistanceSquared = FLT_MAX;
                for (int32_t i = 0; i < g_JasonAITargetCount; ++i)
                {
                    AActor* candidate = g_JasonAITargets[i];
                    FVector candidateLocation{};
                    if (!candidate || candidate == human ||
                        !IsValidatedLiveCounselorPawn(candidate) ||
                        !GetCounselorInteractionManager(candidate) ||
                        !Memory::IsReadable(
                            interactionLocation,
                            sizeof(FVector)) ||
                        !GetJasonAIActorLocation(
                            candidate,
                            candidateLocation))
                    {
                        continue;
                    }
                    const float dx =
                        interactionLocation->X - candidateLocation.X;
                    const float dy =
                        interactionLocation->Y - candidateLocation.Y;
                    const float distanceSquared = dx * dx + dy * dy;
                    if (std::isfinite(distanceSquared) &&
                        distanceSquared < bestDistanceSquared)
                    {
                        bestDistanceSquared = distanceSquared;
                        finisher = candidate;
                        allowAIMovement = true;
                    }
                }
            }
            if (!finisher && GetCounselorInteractionManager(human))
                finisher = human;

            if (finisher && deathContext != previousFinalContext &&
                allowAIMovement)
            {
                HoldKillTeamCounselorForFinalAction(finisher);
            }
            if (finisher && DriveCounselorFinalKill(
                    finisher,
                    allowAIMovement,
                    now))
                return;
            // Never hand a validated final context back to ordinary combat or
            // loot routing even if its interaction manager is transiently
            // unavailable on this retry.
            return;
        }
        else if (!humanIsTommy &&
                 g_PermanentHumanSweaterLatched)
        {
            RecoverOrphanJasonStunIfNeeded(
                human,
                g_JasonAIState.Jason,
                now);
        }

        if (!helper)
        {
            // Tommy may not have spawned yet. The sweater maintenance and
            // any validated human final action above still remain active.
            return;
        }

        // Make Jason Tommy's first and authoritative objective as soon as the
        // returned pawn is discovered. Thereafter, inspect the object key only
        // on a sparse repair cadence and rewrite the fight branch solely when
        // another stock goal displaced Jason. This replaces the old 1.1-second
        // block of seven reflected writes that caused the rhythmic hitch.
        if (wantedRoute == KillTeamRoute::HumanSweaterAITommy)
        {
            bool objectiveNeedsRepair =
                routeChanged || g_TommyJasonObjectiveOwner != helper;
            if (!objectiveNeedsRepair &&
                now >= g_NextTommyJasonObjectiveRepairAt)
            {
                UObject* controller = GetPawnControllerSafe(helper);
                UObject* blackboard = controller
                    ? GetCounselorBlackboardCached(controller)
                    : nullptr;
                objectiveNeedsRepair = !blackboard ||
                    GetBlackboardObject(
                        blackboard,
                        g_JasonCharacterNameIndex) !=
                        reinterpret_cast<UObject*>(g_JasonAIState.Jason);
                g_NextTommyJasonObjectiveRepairAt = now + 12151;
            }
            if (objectiveNeedsRepair)
            {
                const bool objectivePublished =
                    ArmKillTeamCounselorCombat(helper);
                CancelKillTeamHelperNonFinalInteraction(helper, now);
                g_TommyJasonObjectiveOwner = helper;
                g_NextTommyJasonObjectiveRepairAt = now + 12151;

                FVector jasonLocation{};
                if (GetJasonAIActorLocation(
                        g_JasonAIState.Jason,
                        jasonLocation))
                {
                    IssueAIMoveToLocationOnGameThread(
                        GetPawnControllerSafe(helper),
                        jasonLocation,
                        175.0f,
                        "AITommyFirstObjectiveJason");
                    g_NextKillTeamFollowAt = now + 6151;
                }
                Logger::Success(
                    std::string("18L-BB AI Tommy authoritative first objective set to Jason | blackboard=") +
                    (objectivePublished ? "true" : "false"));
            }
        }

        if (IsKillTeamHelperNativeBusy(helper, now))
            return;

        const bool discoveryDue = now >= g_NextKillTeamDiscoveryAt;
        if (discoveryDue)
        {
            g_NextKillTeamDiscoveryAt = now + 60000;
            if (!g_KillTeamShack)
            {
                g_KillTeamShack = FindNearestWorldActorByClass(
                    "Jason_Shack_C",
                    nullptr,
                    0.0f);
            }
        }
        FVector shackLocation{};
        const FVector* shackOrigin =
            g_KillTeamShack &&
            GetJasonAIActorLocation(g_KillTeamShack, shackLocation)
                ? &shackLocation
                : nullptr;

        if (wantedRoute == KillTeamRoute::HumanTommyFemaleHelper)
        {
            if (!HasPamelaSweater(helper))
            {
                if (g_KillTeamSweater &&
                    !Memory::IsReadable(g_KillTeamSweater, sizeof(UObject)))
                {
                    g_KillTeamSweater = nullptr;
                }
                if (discoveryDue)
                {
                    if (!g_KillTeamSweater)
                    {
                        g_KillTeamSweater = FindNearestWorldActorByClass(
                            "PamelasSweater_C",
                            shackOrigin,
                            1800.0f);
                    }
                }
                if (g_KillTeamSweater)
                    DriveCounselorToPickup(
                        helper,
                        g_KillTeamSweater,
                        now,
                        "PamelaSweater");
                return;
            }

            MaintainKillTeamHelperProtection(helper, now);
            if (!g_KillTeamMaskAcquired)
            {
                if (g_KillTeamMask &&
                    !Memory::IsReadable(g_KillTeamMask, sizeof(UObject)))
                {
                    g_KillTeamMask = nullptr;
                }
                if (discoveryDue)
                {
                    if (!g_KillTeamMaskAcquired && !g_KillTeamMask)
                    {
                        g_KillTeamMask = FindNearestWorldActorByClass(
                            "SCKillerMask",
                            nullptr,
                            0.0f);
                    }
                }
                if (g_KillTeamMask)
                {
                    DriveCounselorToPickup(
                        helper,
                        g_KillTeamMask,
                        now,
                        "JasonMask");
                    if (GetActorOwnerSafe(g_KillTeamMask) == helper ||
                        g_KillTeamMaskAcquired)
                    {
                        g_KillTeamMaskAcquired = true;
                        Logger::Success(
                            "18L-AN kill-team female helper acquired Jason mask");
                    }
                    return;
                }
            }

            FVector helperLocation{};
            FVector humanLocation{};
            if (GetJasonAIActorLocation(helper, helperLocation) &&
                GetJasonAIActorLocation(human, humanLocation))
            {
                const float dx = humanLocation.X - helperLocation.X;
                const float dy = humanLocation.Y - helperLocation.Y;
                if (dx * dx + dy * dy > 300.0f * 300.0f &&
                    now >= g_NextKillTeamFollowAt)
                {
                    IssueAIMoveToLocationOnGameThread(
                        GetPawnControllerSafe(helper),
                        humanLocation,
                        180.0f,
                        "FollowHumanTommy");
                    g_NextKillTeamFollowAt = now + 2500;
                }
            }

            FVector jasonLocation{};
            if (g_KillTeamMaskAcquired &&
                now >= g_NextSweaterUseAt &&
                GetJasonAIActorLocation(helper, helperLocation) &&
                GetJasonAIActorLocation(
                    g_JasonAIState.Jason,
                    jasonLocation))
            {
                const float dx = jasonLocation.X - helperLocation.X;
                const float dy = jasonLocation.Y - helperLocation.Y;
                if (dx * dx + dy * dy <= 500.0f * 500.0f &&
                    DispatchSweaterAbility(helper))
                {
                    g_KillTeamSweaterUseDispatched = true;
                    Logger::Success(
                        "18L-AN kill-team female helper dispatched native Pamela sweater ability");
                }
                g_NextSweaterUseAt = now + 3000;
            }
            return;
        }

        // AI Tommy is a dedicated Jason-kill helper from the moment his
        // return pawn is possessed.  Assert the complete melee branch before
        // checking his weapon so the stock tree cannot divert him into
        // cabins, hiding spots, searchable furniture, or generic looting.
        MaintainKillTeamHelperProtection(helper, now);

        UObject* equippedWeapon = GetCounselorCurrentWeapon(helper);
        bool tommyHasAxe = equippedWeapon &&
            ObjectClassDerivesFromExact(
                equippedWeapon,
                "CounselorTwoHandedAxe_C");
        if (tommyHasAxe)
        {
            g_HunterSpawnAxeItem = reinterpret_cast<AActor*>(equippedWeapon);
            g_KillTeamAxe = g_HunterSpawnAxeItem;
        }
        if (!tommyHasAxe)
        {
            // Tommy's return loadout is already replaced with an axe by the
            // GiveStartingItem hook. Never search for, path to, teleport to,
            // or repeatedly attempt to pick up the cabin axe. If native grab
            // handling removes his equipped instance, restore the cached axe
            // class directly through the same stock inventory function.
            if (now >= g_NextKillTeamInteractAt &&
                g_OriginalGiveStartingItem &&
                g_HunterSpawnAxeClass)
            {
                g_OriginalGiveStartingItem(helper, g_HunterSpawnAxeClass);
                g_NextKillTeamInteractAt = now + 10000;
                equippedWeapon = GetCounselorCurrentWeapon(helper);
                tommyHasAxe = equippedWeapon &&
                    ObjectClassDerivesFromExact(
                        equippedWeapon,
                        "CounselorTwoHandedAxe_C");
                if (tommyHasAxe)
                {
                    g_HunterSpawnAxeItem =
                        reinterpret_cast<AActor*>(equippedWeapon);
                    Logger::Success(
                        "18L-AU AI Tommy permanent starting axe restored without cabin search");
                }
            }
            // Even if the native grant does not equip synchronously, retain
            // combat-only routing.  The bounded ten-second recovery can try
            // again without handing Tommy back to the loot/hide branches.
        }

        g_KillTeamHelperArmed = true;

        FVector helperLocation{};
        FVector jasonLocation{};
        if (GetJasonAIActorLocation(helper, helperLocation) &&
            GetJasonAIActorLocation(
                g_JasonAIState.Jason,
                jasonLocation))
        {
            const float dx = jasonLocation.X - helperLocation.X;
            const float dy = jasonLocation.Y - helperLocation.Y;
            const float distanceSquared = dx * dx + dy * dy;
            if (std::isfinite(distanceSquared) &&
                distanceSquared > 425.0f * 425.0f &&
                now >= g_NextKillTeamFollowAt)
            {
                IssueAIMoveToLocationOnGameThread(
                    GetPawnControllerSafe(helper),
                    jasonLocation,
                    175.0f,
                    "AITommyCombatPursueJason");
                // One accepted MoveTo remains active in the path-following
                // component. Rebuilding it every 1.2 seconds caused the exact
                // periodic video hitch seen after Tommy spawned. Refresh only
                // as a bounded correction if he is still far from Jason.
                g_NextKillTeamFollowAt = now + 6151;
            }
        }

    }

    void DriveCounselorKillTeamAI(ULONGLONG now)
    {
        // Own cadence: remains active for surviving AI counselors even if the
        // original human counselor later dies or escapes.
        DriveCounselorSweaterConvergence(now);

        if (now < g_NextKillTeamTickAt)
            return;

        AActor* human = g_LocalCounselorTarget;
        if (!IsValidatedLiveCounselorPawn(human))
        {
            g_NextKillTeamTickAt = now + 60000;
            return;
        }

        // Proximity-only design: no Tommy discovery, helper route, loot/hide
        // cancellation, recurring blackboard writes, MoveTo rebuild, or bot
        // teleport. Poll only the human's verified native sweater byte and
        // Jason's direct final-context pointer. Heavy restoration happens
        // once, only on the real owned->consumed transition.
        MaintainPermanentHumanSweater(human);
        RestoreRepeatJasonDeathOpportunityIfNeeded(now);
        g_NextKillTeamTickAt = now + 997;

        // AttemptInteract is void, and a successful paired kill naturally
        // clears Jason's +0x12D8 death context near the end of its animation.
        // That disappearance after an accepted final input is the success
        // signal. Retire custom AI and leave the stock match-ending sequence
        // alone; treating it as a timeout used to call EndStun and revive him.
        AActor* directDeathContext = g_JasonAIState.Jason
            ? ReadActorField(
                reinterpret_cast<UObject*>(g_JasonAIState.Jason),
                0x12D8)
            : nullptr;
#ifdef ROB_LITE_SANDBOX
        const bool liteSandboxFinalFallback =
            g_KillTeamFinalInteractionAttempts > 0 &&
            g_KillTeamFinalInteractionStartedAt != 0 &&
            now - g_KillTeamFinalInteractionStartedAt >= 12000;
#else
        const bool liteSandboxFinalFallback = false;
#endif
        if (!g_KillTeamFinalInteractionCommitted &&
            g_KillTeamFinalInteractionAttempts > 0 &&
            g_KillTeamFinalInteractionStartedAt != 0 &&
            now - g_KillTeamFinalInteractionStartedAt >= 8000 &&
            (!directDeathContext || liteSandboxFinalFallback))
        {
#ifdef ROB_LITE_SANDBOX
            StopJasonAIMovementForKnifeOnGameThread();
            UObject* deadJasonController = g_JasonAIState.Controller;
            if (deadJasonController && deadJasonController->Class &&
                Memory::IsReadable(deadJasonController, sizeof(UObject)) &&
                Memory::IsReadable(deadJasonController->Class, sizeof(UObject)))
            {
                UFunction* setTickEnabled = FindFunctionInHierarchyByName(
                    deadJasonController->Class, "SetActorTickEnabled");
                if (setTickEnabled)
                {
                    struct TickParams { bool bEnabled; } params{};
                    SafeProcessEventCall(
                        reinterpret_cast<uintptr_t>(deadJasonController),
                        deadJasonController, setTickEnabled, &params);
                }
            }
#endif
            g_KillTeamFinalInteractionCommitted = true;
            g_KillTeamFinalInteractionDispatched = true;
            g_KillTeamFinalInteractionPending = false;
            g_KillTeamFinalContextUntil = now + 60000;
            g_OrphanJasonStunStartedAt = 0;
            g_JasonAIState.Target = nullptr;
            g_JasonAIState.Active = false;
            ResetVehicleInterceptionState();
            SetJasonHighPriorityPursuitBoost(false, "Jason-final-death");
#ifdef ROB_LITE_SANDBOX
            bool liteMatchEnded = false;
            AActor* sandboxGameMode = g_JasonAICache.SandboxGameMode;
            UObject* sandboxGameState = reinterpret_cast<UObject*>(
                g_JasonAIState.StartupTrapGameState);

            // Sandbox normally enters its Jason "walk home" level outro when
            // EndMatch succeeds.  A final-killed Jason must bypass that shot
            // and continue directly into the ordinary end-match UI.  The
            // stock game exposes bSkipLevelOutro for this exact transition;
            // resolve it reflectively so this remains map independent.
            const bool skippedGameModeOutro = WriteReflectedBoolByte(
                reinterpret_cast<UObject*>(sandboxGameMode),
                "bSkipLevelOutro",
                true);
            const bool skippedGameStateOutro = WriteReflectedBoolByte(
                sandboxGameState,
                "bSkipLevelOutro",
                true);
            Logger::Success(
                std::string("Lite Sandbox final kill: stock level outro skip armed | gameMode=") +
                (skippedGameModeOutro ? "true" : "false") +
                " | gameState=" +
                (skippedGameStateOutro ? "true" : "false"));

            if (sandboxGameMode && sandboxGameMode->Class &&
                Memory::IsReadable(sandboxGameMode, sizeof(UObject)) &&
                Memory::IsReadable(sandboxGameMode->Class, sizeof(UObject)))
            {
                const char* completionFunctions[] = { "EndMatch", "FinishMatch" };
                for (const char* functionName : completionFunctions)
                {
                    UFunction* completion = FindFunctionInHierarchyByName(
                        sandboxGameMode->Class, functionName);
                    if (!completion)
                        continue;
                    alignas(16) uint8_t params[0x40]{};
                    liteMatchEnded = SafeProcessEventCall(
                        reinterpret_cast<uintptr_t>(sandboxGameMode),
                        sandboxGameMode, completion, params);
                    Logger::Success(std::string("Lite Sandbox final kill: ") +
                        functionName + " dispatched=" +
                        (liteMatchEnded ? "true" : "false"));
                    if (liteMatchEnded)
                        break;
                }
            }

            bool resultsShown = false;
            UObject* resultTargets[4]{};
            resultTargets[0] = reinterpret_cast<UObject*>(
                Engine::GetLocalPlayerController());
            resultTargets[1] = reinterpret_cast<UObject*>(sandboxGameMode);
            resultTargets[2] = sandboxGameState;
            if (resultTargets[0] && resultTargets[0]->Class &&
                Memory::IsReadable(resultTargets[0], sizeof(UObject)))
            {
                UFunction* getHUD = FindFunctionInHierarchyByName(
                    resultTargets[0]->Class, "GetHUD");
                if (getHUD)
                {
                    struct HUDParams { UObject* ReturnValue; } params{};
                    if (SafeProcessEventCall(
                            reinterpret_cast<uintptr_t>(resultTargets[0]),
                            resultTargets[0], getHUD, &params))
                    {
                        resultTargets[3] = params.ReturnValue;
                    }
                }
            }
            for (UObject* target : resultTargets)
            {
                if (!target || !target->Class ||
                    !Memory::IsReadable(target, sizeof(UObject)) ||
                    !Memory::IsReadable(target->Class, sizeof(UObject)))
                    continue;
                UFunction* showMenus = FindFunctionInHierarchyByName(
                    target->Class, "OnShowEndMatchMenus");
                if (!showMenus)
                    continue;
                alignas(16) uint8_t params[0x40]{};
                resultsShown = SafeProcessEventCall(
                    reinterpret_cast<uintptr_t>(target), target,
                    showMenus, params);
                if (resultsShown)
                    break;
            }
            Logger::Success(
                std::string("Lite Sandbox final kill: results UI dispatched=") +
                (resultsShown ? "true" : "false"));
            if (!liteMatchEnded)
                Logger::Error("Lite Sandbox final kill: match completion function unavailable; Jason remains retired");
#endif
            Logger::Success(
                "18L-BD native final kill committed; custom Jason AI retired for stock match completion");
            return;
        }

        AActor* deathContext = nullptr;
        UObject* deathComponent = nullptr;
        if (now < g_KillTeamFinalRecoveryCooldownUntil ||
            !TryGetValidatedJasonDeathContext(
                g_JasonAIState.Jason,
                deathContext,
                deathComponent,
                now))
        {
            if (g_PermanentHumanSweaterLatched)
            {
                RecoverOrphanJasonStunIfNeeded(
                    human,
                    g_JasonAIState.Jason,
                    now);
            }
            return;
        }

        g_OrphanJasonStunStartedAt = 0;
        g_KillTeamFinalContextUntil = now + 15000;
        g_NextKillTeamTickAt = now + 250;
        // Keep the native kneel stationary. This is a cheap controller stop on
        // the short-lived final-context cadence, not a world scan or MoveTo.
        StopJasonAIMovementForKnifeOnGameThread();

        FVector* interactionLocation = reinterpret_cast<FVector*>(
            reinterpret_cast<uintptr_t>(deathComponent) +
            Offsets::Scene_ComponentToWorld +
            Offsets::FTransform_Translation);
        if (!Memory::IsReadable(interactionLocation, sizeof(FVector)))
            return;

        auto distanceSquaredToFinal =
            [interactionLocation](AActor* counselor) -> float
        {
            FVector location{};
            if (!IsValidatedLiveCounselorPawn(counselor) ||
                !GetJasonAIActorLocation(counselor, location))
            {
                return FLT_MAX;
            }
            const float dx = interactionLocation->X - location.X;
            const float dy = interactionLocation->Y - location.Y;
            const float distanceSquared = dx * dx + dy * dy;
            return std::isfinite(distanceSquared)
                ? distanceSquared
                : FLT_MAX;
        };

        constexpr float FinalProximityCm = 185.0f;
        constexpr float FinalProximitySquared =
            FinalProximityCm * FinalProximityCm;
        AActor* finisher = nullptr;
        float bestDistanceSquared = distanceSquaredToFinal(human);
        if (bestDistanceSquared <= FinalProximitySquared &&
            GetCounselorInteractionManager(human))
        {
            // The human is always preferred. No custom movement is ever
            // issued for the local player.
            finisher = human;
        }
        else
        {
            bestDistanceSquared = FLT_MAX;
            for (int32_t i = 0; i < g_JasonAITargetCount; ++i)
            {
                AActor* candidate = g_JasonAITargets[i];
                if (!candidate || candidate == human ||
                    !GetCounselorInteractionManager(candidate))
                {
                    continue;
                }
                const float candidateDistanceSquared =
                    distanceSquaredToFinal(candidate);
                if (candidateDistanceSquared <= FinalProximitySquared &&
                    candidateDistanceSquared < bestDistanceSquared)
                {
                    finisher = candidate;
                    bestDistanceSquared = candidateDistanceSquared;
                }
            }
        }

        if (!finisher)
        {
            // Keep the stock kneel available long enough for someone to step
            // beside Jason, but never move or teleport a counselor into it.
            if (g_KillTeamFinalSequenceStartedAt != 0 &&
                now - g_KillTeamFinalSequenceStartedAt >= 20000)
            {
                AbortFailedJasonKillSequence(
                    g_JasonAIState.Jason,
                    deathContext,
                    now);
            }
            return;
        }

        if (deathContext != g_KillTeamPendingFinalContext &&
            IsAIControlledCounselor(finisher))
        {
            HoldKillTeamCounselorForFinalAction(finisher);
        }
        // allowAIMovement=false is intentional for every finisher. Only a
        // counselor already in native interaction range may press the final
        // action; there is no path building or teleport fallback in this lane.
        DriveCounselorFinalKill(finisher, false, now);
    }

    void RefreshUnarmedCounselorFleeState(ULONGLONG now)
    {
        if (now < g_NextCounselorFleeRefreshAt)
            return;
        // Finish name-key discovery in tiny slices. Once resolved, service at
        // most one counselor per staggered pass instead of bursting up to 32
        // reflected blackboard writes on the same ten-second frame.
        g_NextCounselorFleeRefreshAt = now +
            (g_CounselorBlackboardNameResolutionComplete ? 1250 : 250);

        if (!g_CounselorBlackboardNameResolutionComplete)
        {
            const ULONGLONG startedAt = GetTickCount64();
            const int32_t startIndex =
                g_CounselorBlackboardNameResolveCursor;
            ResolveCounselorBlackboardNameIndices();
            if (g_CounselorBlackboardNameResolutionComplete ||
                (g_CounselorBlackboardNameResolveCursor % 8192) == 0)
            {
                Logger::Debug(
                    "18L-AI counselor flee key scan throttled | range=" +
                    std::to_string(startIndex) + "-" +
                    std::to_string(g_CounselorBlackboardNameResolveCursor) +
                    " | complete=" +
                    std::string(
                        g_CounselorBlackboardNameResolutionComplete
                            ? "true"
                            : "false") +
                    " | durationMs=" +
                    std::to_string(GetTickCount64() - startedAt));
            }
        }

        if (g_SCWeaponNameIndex < 0 ||
            g_ShouldFleeKillerNameIndex < 0 ||
            g_ShouldFightBackNameIndex < 0 ||
            g_ShouldArmedFightBackNameIndex < 0 ||
            g_SeekWeaponWhileFleeingNameIndex < 0)
        {
            return;
        }

        // Sweater convergence deliberately owns the fight/flee keys after its
        // one-time transition; the ordinary unarmed proximity lane must not
        // overwrite those assignments.
        if (g_CounselorConvergenceActive)
            return;

        AActor* jason = g_JasonAIState.Jason;
        FVector jasonLocation{};
        if (!jason || !GetJasonAIActorLocation(jason, jasonLocation))
            return;

        const int32_t boundedTargetCount =
            (std::min)(g_JasonAITargetCount, 8);
        for (int32_t attempt = 0;
             attempt < boundedTargetCount;
             ++attempt)
        {
            const int32_t i =
                (g_CounselorFleeRefreshCursor + attempt) %
                boundedTargetCount;
            AActor* counselor = g_JasonAITargets[i];
            if (!counselor ||
                counselor == g_LocalCounselorTarget ||
                !IsValidatedLiveCounselorPawn(counselor))
            {
                continue;
            }

            FVector counselorLocation{};
            if (!GetJasonAIActorLocation(counselor, counselorLocation))
                continue;
            const float dx = counselorLocation.X - jasonLocation.X;
            const float dy = counselorLocation.Y - jasonLocation.Y;
            if (!std::isfinite(dx) ||
                !std::isfinite(dy) ||
                dx * dx + dy * dy > 2500.0f * 2500.0f)
            {
                continue;
            }

            UObject** controllerField = reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(counselor) + 0x3A0);
            if (!Memory::IsReadable(controllerField, sizeof(UObject*)) ||
                !*controllerField ||
                !Memory::IsReadable(*controllerField, sizeof(UObject)))
            {
                continue;
            }

            UObject* controller = *controllerField;
            UObject* blackboard = GetCounselorBlackboardCached(controller);
            if (!blackboard)
                continue;

            // The behavior tree's SCWeapon object is authoritative for whether
            // this bot can fight.  When it is empty and Jason is within 25 m,
            // force the existing abort-aware flee branch and let its native
            // FindFleeLocation/escape tasks choose the route.
            if (!GetBlackboardObject(blackboard, g_SCWeaponNameIndex))
            {
                SetBlackboardBool(
                    blackboard,
                    g_ShouldFightBackNameIndex,
                    false);
                SetBlackboardBool(
                    blackboard,
                    g_ShouldArmedFightBackNameIndex,
                    false);
                SetBlackboardBool(
                    blackboard,
                    g_SeekWeaponWhileFleeingNameIndex,
                    true);
                SetBlackboardBool(
                    blackboard,
                    g_ShouldFleeKillerNameIndex,
                    true);
            }

            g_CounselorFleeRefreshCursor =
                (i + 1) % boundedTargetCount;
            break;
        }
    }

    bool ClassifyRepairableCar(
        AActor* actor,
        uint8_t& outKind,
        int32_t& outSeatCount)
    {
        outKind = 0;
        outSeatCount = 0;

        if (!actor ||
            !Memory::IsReadable(actor, sizeof(UObject)) ||
            !JasonAIObjectDerivesFromNameContaining(
                reinterpret_cast<UObject*>(actor),
                "SCDriveableVehicle"))
        {
            return false;
        }

        // Resurrected native vehicle-cache classification (RVA 0x3B87C0):
        // VehicleType 1 is a car; Seats.Num > 2 distinguishes the 4-seater.
        uint8_t* vehicleType = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(actor) + 0x3D8);
        TArray<UObject*>* seats = reinterpret_cast<TArray<UObject*>*>(
            reinterpret_cast<uintptr_t>(actor) + 0x4E8);

        if (!Memory::IsReadable(vehicleType, 1) ||
            !Memory::IsReadable(seats, sizeof(TArray<UObject*>)) ||
            *vehicleType != 1 ||
            seats->Count <= 0 ||
            seats->Count > 16)
        {
            return false;
        }

        outSeatCount = seats->Count;
        outKind = seats->Count > 2 ? 3 : 2;
        return true;
    }

    void ConsiderRepairableCar(
        AActor* actor,
        const char* source,
        AActor*& car2,
        AActor*& car4,
        std::string& car2Source,
        std::string& car4Source)
    {
        uint8_t kind = 0;
        int32_t seatCount = 0;
        if (!ClassifyRepairableCar(actor, kind, seatCount))
            return;

        if (kind == 2 && !car2)
        {
            car2 = actor;
            car2Source = source ? source : "unknown";
        }
        else if (kind == 3 && !car4)
        {
            car4 = actor;
            car4Source = source ? source : "unknown";
        }
        else
        {
            return;
        }

        Logger::Success(
            "18L-AG counselor bridge car objective: actor=" +
            JasonAISafeName(reinterpret_cast<UObject*>(actor)) +
            " | type=1 | seats=" + std::to_string(seatCount) +
            " | kind=" + JasonAIStartupTrapKindName(kind) +
            " | source=" + (source ? source : "unknown"));
    }

    void ScanVehicleArray(
        TArray<AActor*>* vehicles,
        const char* source,
        AActor*& car2,
        AActor*& car4,
        std::string& car2Source,
        std::string& car4Source)
    {
        if (!vehicles ||
            !Memory::IsReadable(vehicles, sizeof(TArray<AActor*>)) ||
            !vehicles->Data ||
            vehicles->Count <= 0 ||
            vehicles->Count > 128 ||
            !Memory::IsReadable(
                vehicles->Data,
                sizeof(AActor*) * static_cast<size_t>(vehicles->Count)))
        {
            return;
        }

        for (int32_t i = 0;
            i < vehicles->Count && (!car2 || !car4);
            ++i)
        {
            ConsiderRepairableCar(
                vehicles->Data[i],
                source,
                car2,
                car4,
                car2Source,
                car4Source);
        }
    }

    bool ReadLiveVehicleSeat(
        UObject* seat,
        AActor* expectedCar,
        AActor*& outOccupant)
    {
        outOccupant = nullptr;
        if (!seat ||
            !expectedCar ||
            !Memory::IsReadable(seat, 0x550) ||
            !JasonAIObjectDerivesFromNameContaining(
                seat,
                "SCVehicleSeatComponent"))
        {
            return false;
        }

        AActor** parentVehicle = reinterpret_cast<AActor**>(
            reinterpret_cast<uintptr_t>(seat) + 0x548);
        AActor** occupant = reinterpret_cast<AActor**>(
            reinterpret_cast<uintptr_t>(seat) + 0x500);
        if (!Memory::IsReadable(parentVehicle, sizeof(AActor*)) ||
            *parentVehicle != expectedCar ||
            !Memory::IsReadable(occupant, sizeof(AActor*)) ||
            !*occupant ||
            !Memory::IsReadable(*occupant, 0x15A0) ||
            !JasonAIObjectDerivesFromNameContaining(
                reinterpret_cast<UObject*>(*occupant),
                "SCCounselorCharacter"))
        {
            return false;
        }

        uint8_t* dead = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(*occupant) + 0x1031);
        UObject** currentSeat = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(*occupant) + 0x1588);
        uint8_t* exiting = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(*occupant) + 0x159C);
        if (!Memory::IsReadable(dead, 1) ||
            *dead != 0 ||
            HasCounselorEscaped(*occupant) ||
            !Memory::IsReadable(currentSeat, sizeof(UObject*)) ||
            *currentSeat != seat ||
            (Memory::IsReadable(exiting, 1) && *exiting != 0))
        {
            return false;
        }

        outOccupant = *occupant;
        return true;
    }

    bool FindLiveSeatInCar(
        AActor* car,
        AActor* preferredOccupant,
        UObject*& outSeat,
        AActor*& outOccupant)
    {
        outSeat = nullptr;
        outOccupant = nullptr;
        uint8_t kind = 0;
        int32_t seatCount = 0;
        if (!ClassifyRepairableCar(car, kind, seatCount))
            return false;

        TArray<UObject*>* seats = reinterpret_cast<TArray<UObject*>*>(
            reinterpret_cast<uintptr_t>(car) + 0x4E8);
        if (!Memory::IsReadable(seats, sizeof(TArray<UObject*>)) ||
            !seats->Data ||
            seats->Count <= 0 ||
            seats->Count > 16 ||
            !Memory::IsReadable(
                seats->Data,
                sizeof(UObject*) * static_cast<size_t>(seats->Count)))
        {
            return false;
        }

        UObject* firstSeat = nullptr;
        AActor* firstOccupant = nullptr;
        UObject* preferredSeat = nullptr;
        AActor* preferredLiveOccupant = nullptr;
        for (int32_t i = 0; i < seats->Count; ++i)
        {
            AActor* occupant = nullptr;
            if (!ReadLiveVehicleSeat(seats->Data[i], car, occupant))
                continue;

            if (!firstSeat)
            {
                firstSeat = seats->Data[i];
                firstOccupant = occupant;
            }

            uint8_t* driver = reinterpret_cast<uint8_t*>(
                reinterpret_cast<uintptr_t>(seats->Data[i]) + 0x511);
            if (Memory::IsReadable(driver, 1) && *driver != 0)
            {
                // Always stop the vehicle by removing its actual driver,
                // even when Jason was previously chasing a passenger.
                outSeat = seats->Data[i];
                outOccupant = occupant;
                return true;
            }

            if (preferredOccupant && occupant == preferredOccupant)
            {
                preferredSeat = seats->Data[i];
                preferredLiveOccupant = occupant;
            }
        }

        outSeat = preferredSeat ? preferredSeat : firstSeat;
        outOccupant = preferredSeat
            ? preferredLiveOccupant
            : firstOccupant;
        return outSeat != nullptr;
    }

    bool IsStartedOccupiedCar(
        AActor* car,
        AActor* preferredOccupant,
        UObject*& outSeat,
        AActor*& outOccupant,
        bool requireStarted)
    {
        if (!FindLiveSeatInCar(
                car,
                preferredOccupant,
                outSeat,
                outOccupant))
        {
            return false;
        }

        uint8_t* started = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(car) + 0x47A);
        return !requireStarted ||
            (Memory::IsReadable(started, 1) && *started != 0);
    }

    AActor* FindOccupiedEscapeCar(
        UObject*& outSeat,
        AActor*& outOccupant)
    {
        outSeat = nullptr;
        outOccupant = nullptr;
        AActor* preferred = g_JasonAIState.Target;
        const ULONGLONG now = GetTickCount64();
        if (g_IgnoredVehicleInterceptCar &&
            now >= g_IgnoredVehicleInterceptUntil)
        {
            g_IgnoredVehicleInterceptCar = nullptr;
            g_IgnoredVehicleInterceptUntil = 0;
        }
        const auto isTemporarilyIgnoredCar =
            [now](AActor* car) -> bool
        {
            return car &&
                car == g_IgnoredVehicleInterceptCar &&
                now < g_IgnoredVehicleInterceptUntil;
        };

        // The current chase target's real vehicle seat is the strongest
        // signal. Vehicle possession can temporarily break the ordinary
        // controller/pawn reciprocity used by the on-foot target registry,
        // so validate the seat/occupant relationship directly here.
        if (preferred && Memory::IsReadable(preferred, 0x1590))
        {
            UObject** currentSeat = reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(preferred) + 0x1588);
            if (Memory::IsReadable(currentSeat, sizeof(UObject*)) &&
                *currentSeat &&
                Memory::IsReadable(*currentSeat, 0x550))
            {
                AActor** parentCar = reinterpret_cast<AActor**>(
                    reinterpret_cast<uintptr_t>(*currentSeat) + 0x548);
                if (Memory::IsReadable(parentCar, sizeof(AActor*)) &&
                    *parentCar &&
                    !isTemporarilyIgnoredCar(*parentCar) &&
                    IsStartedOccupiedCar(
                        *parentCar,
                        preferred,
                        outSeat,
                        outOccupant,
                        true))
                {
                    return *parentCar;
            }
        }
    }

        if (g_VehicleInterceptCar &&
            !isTemporarilyIgnoredCar(g_VehicleInterceptCar) &&
            IsStartedOccupiedCar(
                g_VehicleInterceptCar,
                preferred,
                outSeat,
                outOccupant,
                false))
        {
            return g_VehicleInterceptCar;
        }

        AActor* jason = g_JasonAIState.Jason;
        FVector jasonLocation{};
        if (!jason || !GetJasonAIActorLocation(jason, jasonLocation))
            return nullptr;

        AActor* bestCar = nullptr;
        UObject* bestSeat = nullptr;
        AActor* bestOccupant = nullptr;
        float bestDistanceSquared = FLT_MAX;
        bool haveKnownCarRegistry = false;

        // The opening objective resolver already validated and cached both
        // real cars. Keep the normal interception hot path bounded to those
        // two actors instead of rescanning every actor in every level three
        // times per second.
        for (int32_t i = 0;
            i < g_JasonAIState.StartupTrapObjectiveCount;
            ++i)
        {
            if (g_JasonAIState.StartupTrapObjectiveKinds[i] != 2 &&
                g_JasonAIState.StartupTrapObjectiveKinds[i] != 3)
            {
                continue;
            }

            AActor* car = g_JasonAIState.StartupTrapObjectives[i];
            if (isTemporarilyIgnoredCar(car))
                continue;
            uint8_t knownKind = 0;
            int32_t knownSeats = 0;
            if (ClassifyRepairableCar(car, knownKind, knownSeats))
                haveKnownCarRegistry = true;
            UObject* seat = nullptr;
            AActor* occupant = nullptr;
            if (!IsStartedOccupiedCar(
                    car,
                    preferred,
                    seat,
                    occupant,
                    true))
            {
                continue;
            }

            FVector carLocation{};
            if (!GetJasonAIActorLocation(car, carLocation))
                continue;
            const float dx = carLocation.X - jasonLocation.X;
            const float dy = carLocation.Y - jasonLocation.Y;
            const float dz = carLocation.Z - jasonLocation.Z;
            const float distanceSquared = dx * dx + dy * dy + dz * dz;
            if (std::isfinite(distanceSquared) &&
                distanceSquared < bestDistanceSquared)
            {
                bestDistanceSquared = distanceSquared;
                bestCar = car;
                bestSeat = seat;
                bestOccupant = occupant;
            }
        }

        if (bestCar)
        {
            outSeat = bestSeat;
            outOccupant = bestOccupant;
            return bestCar;
        }

        // A valid opening-objective car registry is authoritative. If neither
        // known car is currently started and occupied, do not fall through to
        // a complete world scan on every AI action interval.
        if (haveKnownCarRegistry)
            return nullptr;

        // Fallback only for unusual maps whose car did not participate in
        // opening objective resolution.
        UWorld* world = g_JasonAIState.World;
        if (!world || !Memory::IsReadable(world, sizeof(UWorld)))
            return nullptr;

        TArray<ULevel*>* levels = reinterpret_cast<TArray<ULevel*>*>(
            reinterpret_cast<uintptr_t>(world) + 0x110);
        if (!Memory::IsReadable(levels, sizeof(TArray<ULevel*>)) ||
            !levels->Data ||
            levels->Count <= 0 ||
            levels->Count > 1024 ||
            !Memory::IsReadable(
                levels->Data,
                sizeof(ULevel*) * static_cast<size_t>(levels->Count)))
        {
            return nullptr;
        }

        for (int32_t levelIndex = 0;
            levelIndex < levels->Count;
            ++levelIndex)
        {
            ULevel* level = levels->Data[levelIndex];
            if (!level || !Memory::IsReadable(level, sizeof(ULevel)))
                continue;

            TArray<AActor*>& actors = level->Actors;
            if (!actors.Data ||
                actors.Count <= 0 ||
                actors.Count > 100000 ||
                !Memory::IsReadable(
                    actors.Data,
                    sizeof(AActor*) * static_cast<size_t>(actors.Count)))
            {
                continue;
            }

            for (int32_t i = 0; i < actors.Count; ++i)
            {
                AActor* car = actors.Data[i];
                if (isTemporarilyIgnoredCar(car))
                    continue;
                UObject* seat = nullptr;
                AActor* occupant = nullptr;
                if (!IsStartedOccupiedCar(
                        car,
                        preferred,
                        seat,
                        occupant,
                        true))
                {
                    continue;
                }

                FVector carLocation{};
                if (!GetJasonAIActorLocation(car, carLocation))
                    continue;

                const float dx = carLocation.X - jasonLocation.X;
                const float dy = carLocation.Y - jasonLocation.Y;
                const float dz = carLocation.Z - jasonLocation.Z;
                const float distanceSquared = dx * dx + dy * dy + dz * dz;
                if (std::isfinite(distanceSquared) &&
                    distanceSquared < bestDistanceSquared)
                {
                    bestDistanceSquared = distanceSquared;
                    bestCar = car;
                    bestSeat = seat;
                    bestOccupant = occupant;
                }
            }
        }

        outSeat = bestSeat;
        outOccupant = bestOccupant;
        return bestCar;
    }

    __declspec(noinline) float SafeVehicleForwardSpeed(AActor* car)
    {
        if (!car || !Memory::IsReadable(car, 0x3D8))
            return 0.0f;

        UObject** movement = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(car) + 0x3D0);
        HMODULE module = GetModuleHandle(nullptr);
        if (!module ||
            !Memory::IsReadable(movement, sizeof(UObject*)) ||
            !*movement ||
            !Memory::IsReadable(*movement, sizeof(UObject)))
        {
            return 0.0f;
        }

        __try
        {
            using Function = float(__fastcall*)(UObject*);
            return reinterpret_cast<Function>(
                reinterpret_cast<uintptr_t>(module) + 0x01FAAB80)(
                    *movement);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0.0f;
        }
    }

    bool GetVehicleTravelDirection(
        AActor* car,
        float forwardSpeed,
        FVector& outDirection,
        float& outHorizontalSpeed)
    {
        outDirection = FVector{};
        outHorizontalSpeed = 0.0f;
        if (!car || !car->Class || !Memory::IsReadable(car, sizeof(UObject)))
            return false;

        UFunction* velocityFunction =
            FindFunctionInHierarchyByName(car->Class, "GetVelocity");
        if (velocityFunction)
        {
            struct VelocityParams { FVector ReturnValue; };
            VelocityParams params{};
            if (SafeProcessEventCall(
                    reinterpret_cast<uintptr_t>(car),
                    car,
                    velocityFunction,
                    &params))
            {
                const float horizontalSquared =
                    params.ReturnValue.X * params.ReturnValue.X +
                    params.ReturnValue.Y * params.ReturnValue.Y;
                if (std::isfinite(horizontalSquared) && horizontalSquared > 1.0f)
                {
                    outHorizontalSpeed = std::sqrt(horizontalSquared);
                    outDirection.X = params.ReturnValue.X / outHorizontalSpeed;
                    outDirection.Y = params.ReturnValue.Y / outHorizontalSpeed;
                    return true;
                }
            }
        }

        if (!GetJasonAIActorForwardVectorOnGameThread(car, outDirection))
            return false;
        if (forwardSpeed < 0.0f)
        {
            outDirection.X = -outDirection.X;
            outDirection.Y = -outDirection.Y;
        }
        outHorizontalSpeed = std::fabs(forwardSpeed);
        return outHorizontalSpeed > 1.0f;
    }

    bool GetSceneComponentLocation(UObject* component, FVector& outLocation)
    {
        outLocation = FVector{};
        if (!component ||
            !Memory::IsReadable(component, sizeof(UObject)) ||
            !component->Class)
        {
            return false;
        }

        UFunction* getLocation = FindFunctionInHierarchyByName(
            component->Class,
            "K2_GetComponentLocation");
        if (!getLocation)
            return false;

        struct Params { FVector ReturnValue; };
        Params params{};
        if (!SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(component),
                component,
                getLocation,
                &params) ||
            !std::isfinite(params.ReturnValue.X) ||
            !std::isfinite(params.ReturnValue.Y) ||
            !std::isfinite(params.ReturnValue.Z))
        {
            return false;
        }

        outLocation = params.ReturnValue;
        return true;
    }

    bool IssueAIMoveToLocationOnGameThread(
        UObject* controller,
        const FVector& destination,
        float acceptanceRadius,
        const char* label)
    {
        if (!controller ||
            !controller->Class ||
            !Memory::IsReadable(controller, sizeof(UObject)))
        {
            return false;
        }

        UFunction* moveToLocation = FindFunctionInHierarchyByName(
            controller->Class,
            "MoveToLocation");
        if (!moveToLocation)
            return false;

        // UE4.18 AAIController::MoveToLocation reflected parameter layout.
        // Keeping this as the public reflected call avoids constructing a
        // goal-actor request whose target remains inside an occupied car.
        struct MoveToLocationParams
        {
            FVector Dest;
            float AcceptanceRadius;
            bool bStopOnOverlap;
            bool bUsePathfinding;
            bool bProjectDestinationToNavigation;
            bool bCanStrafe;
            uint8_t Padding0[4];
            UClass* FilterClass;
            bool bAllowPartialPath;
            uint8_t ReturnValue;
            uint8_t Padding1[6];
        };
        static_assert(
            sizeof(MoveToLocationParams) == 40,
            "MoveToLocationParams must match UE4.18 alignment");

        MoveToLocationParams params{};
        params.Dest = destination;
        params.AcceptanceRadius = acceptanceRadius;
        params.bStopOnOverlap = false;
        params.bUsePathfinding = true;
        params.bProjectDestinationToNavigation = true;
        params.bCanStrafe = true;
        params.FilterClass = nullptr;
        params.bAllowPartialPath = true;

        const bool callOK = SafeProcessEventCall(
            reinterpret_cast<uintptr_t>(controller),
            controller,
            moveToLocation,
            &params);
        Logger::Debug(
            std::string("18L-AI MoveToLocation: objective=") +
            (label ? label : "Unknown") +
            " | call=" + (callOK ? "true" : "false") +
            " | result=" + std::to_string(params.ReturnValue) +
            " | x=" + std::to_string(destination.X) +
            " | y=" + std::to_string(destination.Y));
        return callOK && params.ReturnValue != 0;
    }

    bool ResolveDriverDoorNavPoint(
        AActor* car,
        UObject* driverSeat,
        FVector& outDoorPoint)
    {
        outDoorPoint = FVector{};
        if (!car || !driverSeat)
            return false;

        FVector carLocation{};
        FVector seatLocation{};
        if (!GetJasonAIActorLocation(car, carLocation) ||
            !GetSceneComponentLocation(driverSeat, seatLocation))
        {
            return false;
        }

        float sideX = seatLocation.X - carLocation.X;
        float sideY = seatLocation.Y - carLocation.Y;
        float sideLength = std::sqrt(sideX * sideX + sideY * sideY);
        if (!std::isfinite(sideLength) || sideLength < 25.0f)
        {
            FVector forward{};
            if (!GetJasonAIActorForwardVectorOnGameThread(car, forward))
                return false;
            // UE vehicles use the left side for the driver.  This fallback is
            // used only when the seat component has no useful lateral offset.
            sideX = forward.Y;
            sideY = -forward.X;
            sideLength = std::sqrt(sideX * sideX + sideY * sideY);
        }
        if (!std::isfinite(sideLength) || sideLength < 0.1f)
            return false;

        sideX /= sideLength;
        sideY /= sideLength;

        // Start outside the door rather than at the seated pawn.  The three
        // radii accommodate both the two-seat and four-seat car meshes.
        const float radii[] = { 210.0f, 180.0f, 245.0f };
        FVector extent{};
        extent.X = 120.0f;
        extent.Y = 120.0f;
        extent.Z = 180.0f;
        for (float radius : radii)
        {
            FVector candidate = carLocation;
            candidate.X += sideX * radius;
            candidate.Y += sideY * radius;
            candidate.Z = seatLocation.Z;
            FVector projected{};
            if (ProjectJasonAINavPointOnGameThread(
                    candidate,
                    extent,
                    projected) &&
                std::isfinite(projected.X) &&
                std::isfinite(projected.Y) &&
                std::isfinite(projected.Z) &&
                std::fabs(projected.Z - carLocation.Z) <= 180.0f)
            {
                outDoorPoint = projected;
                return true;
            }
        }
        return false;
    }

    bool ResolveDriverSideDetourPoint(
        AActor* car,
        const FVector& driverDoorPoint,
        const FVector& jasonLocation,
        bool useOppositeEnd,
        FVector& outDetourPoint)
    {
        outDetourPoint = FVector{};
        FVector carLocation{};
        FVector forward{};
        if (!car ||
            !GetJasonAIActorLocation(car, carLocation) ||
            !GetJasonAIActorForwardVectorOnGameThread(car, forward))
        {
            return false;
        }

        float sideX = driverDoorPoint.X - carLocation.X;
        float sideY = driverDoorPoint.Y - carLocation.Y;
        const float sideLength = std::sqrt(sideX * sideX + sideY * sideY);
        const float forwardLength = std::sqrt(
            forward.X * forward.X + forward.Y * forward.Y);
        if (!std::isfinite(sideLength) || sideLength < 1.0f ||
            !std::isfinite(forwardLength) || forwardLength < 0.1f)
        {
            return false;
        }
        sideX /= sideLength;
        sideY /= sideLength;
        forward.X /= forwardLength;
        forward.Y /= forwardLength;

        const float jasonLongitudinal =
            (jasonLocation.X - carLocation.X) * forward.X +
            (jasonLocation.Y - carLocation.Y) * forward.Y;
        float endSign = jasonLongitudinal >= 0.0f ? 1.0f : -1.0f;
        if (useOppositeEnd)
            endSign = -endSign;

        // The direct seat path can cut through the hood collision. First send
        // Jason around the nearer front/rear corner and onto the driver's side,
        // then let the normal door-point path close the interaction distance.
        FVector candidate = carLocation;
        candidate.X += sideX * 360.0f + forward.X * endSign * 360.0f;
        candidate.Y += sideY * 360.0f + forward.Y * endSign * 360.0f;
        candidate.Z = driverDoorPoint.Z;

        FVector extent{};
        extent.X = 160.0f;
        extent.Y = 160.0f;
        extent.Z = 180.0f;
        FVector projected{};
        if (!ProjectJasonAINavPointOnGameThread(candidate, extent, projected) ||
            !std::isfinite(projected.X) ||
            !std::isfinite(projected.Y) ||
            !std::isfinite(projected.Z) ||
            std::fabs(projected.Z - carLocation.Z) > 180.0f)
        {
            return false;
        }

        outDetourPoint = projected;
        return true;
    }

    bool ResolveOccupiedCarFrontHoodPoint(
        AActor* car,
        const FVector& travelDirection,
        FVector& outHoodPoint)
    {
        outHoodPoint = FVector{};
        if (!car || !Memory::IsReadable(car, 0x3D8))
            return false;

        FVector carLocation{};
        if (!GetJasonAIActorLocation(car, carLocation))
            return false;

        FVector forward = travelDirection;
        float forwardX = forward.X;
        float forwardY = forward.Y;
        const float forwardLength =
            std::sqrt(forwardX * forwardX + forwardY * forwardY);
        if (!std::isfinite(forwardLength) || forwardLength < 1.0f)
        {
            if (!GetJasonAIActorForwardVectorOnGameThread(car, forward))
                return false;
            forwardX = forward.X;
            forwardY = forward.Y;
        }
        const float fallbackLength = std::sqrt(forwardX * forwardX + forwardY * forwardY);
        if (!std::isfinite(fallbackLength) || fallbackLength < 1.0f)
            return false;

        forwardX /= fallbackLength;
        forwardY /= fallbackLength;

        FVector candidate = carLocation;
        candidate.X += forwardX * 240.0f;
        candidate.Y += forwardY * 240.0f;
        candidate.Z = carLocation.Z;

        FVector projected{};
        FVector extent{};
        extent.X = 240.0f;
        extent.Y = 220.0f;
        extent.Z = 220.0f;
        if (!ProjectJasonAINavPointOnGameThread(
                candidate,
                extent,
                projected) ||
            !std::isfinite(projected.X) ||
            !std::isfinite(projected.Y) ||
            !std::isfinite(projected.Z) ||
            std::fabs(projected.Z - carLocation.Z) > 220.0f)
        {
            return false;
        }

        outHoodPoint = projected;
        return true;
    }

    bool TeleportJasonAheadOfVehicle(
        AActor* car,
        const FVector& travelDirection,
        float horizontalSpeed,
        ULONGLONG now)
    {
        AActor* jason = g_JasonAIState.Jason;
        if (!jason ||
            !car ||
            JasonAIMorphRemainingMs(now) != 0 ||
            !Memory::IsReadable(jason, sizeof(UObject)))
        {
            return false;
        }

        FVector jasonLocation{};
        FVector carLocation{};
        if (!GetJasonAIActorLocation(jason, jasonLocation) ||
            !GetJasonAIActorLocation(car, carLocation))
        {
            return false;
        }

        const float currentDX = carLocation.X - jasonLocation.X;
        const float currentDY = carLocation.Y - jasonLocation.Y;
        const float currentDistanceSquared =
            currentDX * currentDX + currentDY * currentDY;
        if (!std::isfinite(currentDistanceSquared) ||
            currentDistanceSquared < 1500.0f * 1500.0f)
        {
            return false;
        }

        // Intercept well down-road so a fast car cannot pass Jason during the
        // Morph recovery.  The old 2.5-second lead was consistently late on
        // long escape roads.  Keep the candidate on the road centerline by
        // using a tight projection extent; a wide 500 cm search could slide
        // the teleport onto a roadside nav island.
        const float lead = (std::max)(
            3500.0f,
            (std::min)(6500.0f, horizontalSpeed * 4.0f));
        const float leadScales[] = { 1.0f, 0.90f, 0.78f, 0.64f };
        FVector extent{};
        extent.X = 180.0f;
        extent.Y = 180.0f;
        extent.Z = 180.0f;
        FVector projected{};
        float selectedLead = 0.0f;
        for (float leadScale : leadScales)
        {
            FVector candidate = carLocation;
            candidate.X += travelDirection.X * lead * leadScale;
            candidate.Y += travelDirection.Y * lead * leadScale;
            FVector attempt{};
            if (ProjectJasonAINavPointOnGameThread(
                    candidate,
                    extent,
                    attempt) &&
                std::isfinite(attempt.X) &&
                std::isfinite(attempt.Y) &&
                std::isfinite(attempt.Z) &&
                std::fabs(attempt.Z - carLocation.Z) <= 220.0f)
            {
                projected = attempt;
                selectedLead = lead * leadScale;
                break;
            }
        }
        if (selectedLead <= 0.0f)
            return false;

        UFunction* teleportFunction =
            FindFunctionInHierarchyByName(jason->Class, "K2_TeleportTo");
        if (!teleportFunction)
            return false;

        struct Rotation3 { float Pitch; float Yaw; float Roll; };
        struct TeleportParams
        {
            FVector DestLocation;
            Rotation3 DestRotation;
            bool ReturnValue;
        };
        static_assert(sizeof(TeleportParams) == 28,
            "Vehicle TeleportParams must be 28 bytes");

        constexpr float RadiansToDegrees = 57.29577951308232f;
        TeleportParams params{};
        params.DestLocation = projected;
        params.DestRotation.Yaw =
            std::atan2(-travelDirection.Y, -travelDirection.X) *
            RadiansToDegrees;
        if (!SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(jason),
                jason,
                teleportFunction,
                &params) ||
            !params.ReturnValue)
        {
            return false;
        }

        MarkJasonAIMorphTeleportUsed("CarIntercept");
        StopJasonAIMovementForKnifeOnGameThread();
        FaceJasonAIAtStartupTrapObjectiveOnGameThread(car);
        Logger::Success(
            "18L-AI vehicle intercept: Morph completed ahead of occupied car | car=" +
            JasonAISafeName(reinterpret_cast<UObject*>(car)) +
            " | leadCm=" + std::to_string(selectedLead) +
            " | speedCmPerSec=" + std::to_string(horizontalSpeed));
        return true;
    }

    bool AttemptJasonVehicleComponent(UObject* component)
    {
        AActor* jason = g_JasonAIState.Jason;
        UObject* manager = GetJasonInteractionManager(jason);
        HMODULE module = GetModuleHandle(nullptr);
        if (!component ||
            !manager ||
            !module ||
            GetLockedJasonInteractable(jason) ||
            !Memory::IsReadable(component, sizeof(UObject)))
        {
            return false;
        }

        uintptr_t managerVTable = *reinterpret_cast<uintptr_t*>(manager);
        uintptr_t* attemptSlot = reinterpret_cast<uintptr_t*>(
            managerVTable + 0x430);
        const uintptr_t expected =
            reinterpret_cast<uintptr_t>(module) + 0x0025AC50;
        if (!managerVTable ||
            !Memory::IsReadable(attemptSlot, sizeof(uintptr_t)) ||
            *attemptSlot != expected)
        {
            return false;
        }

        reinterpret_cast<AttemptInteractFn>(*attemptSlot)(
            manager,
            component,
            1,
            false);
        return true;
    }

    bool AttemptJasonHidingSpotComponent(UObject* component)
    {
        AActor* jason = g_JasonAIState.Jason;
        UObject* manager = GetJasonInteractionManager(jason);
        HMODULE module = GetModuleHandle(nullptr);
        if (!component || !manager || !module ||
            GetLockedJasonInteractable(jason) ||
            !Memory::IsReadable(component, sizeof(UObject)))
        {
            return false;
        }

        uintptr_t managerVTable = *reinterpret_cast<uintptr_t*>(manager);
        uintptr_t* attemptSlot = reinterpret_cast<uintptr_t*>(
            managerVTable + 0x430);
        const uintptr_t expected =
            reinterpret_cast<uintptr_t>(module) + 0x0025AC50;
        if (!managerVTable ||
            !Memory::IsReadable(attemptSlot, sizeof(uintptr_t)) ||
            *attemptSlot != expected)
        {
            return false;
        }

        // Dispatch is asynchronous. Do not require the interaction manager's
        // lock to change inside this native call; the caller owns a short,
        // bounded confirmation window and verifies the exact component/spot.
        reinterpret_cast<AttemptInteractFn>(*attemptSlot)(
            manager,
            component,
            1,
            true);
        return true;
    }

    bool ResolveHidingSpotInteraction(
        UObject* component,
        AActor*& outSpot,
        AActor*& outCounselor,
        UObject*& outKillerInteractable)
    {
        outSpot = nullptr;
        outCounselor = nullptr;
        outKillerInteractable = nullptr;
        if (!component || !Memory::IsReadable(component, sizeof(UObject)))
            return false;

        AActor** ownerField = reinterpret_cast<AActor**>(
            reinterpret_cast<uintptr_t>(component) + 0xE0);
        if (!Memory::IsReadable(ownerField, sizeof(AActor*)) ||
            !*ownerField ||
            !ObjectClassDerivesFromExact(
                reinterpret_cast<UObject*>(*ownerField),
                "SCHidingSpot"))
        {
            return false;
        }

        AActor* spot = *ownerField;
        UFunction* getHidingCounselor = FindFunctionInHierarchyByName(
            spot->Class,
            "GetHidingCounselor");
        if (!getHidingCounselor)
            return false;

        struct Params { AActor* ReturnValue; };
        Params params{};
        if (!SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(spot),
                spot,
                getHidingCounselor,
                &params) ||
            !IsValidatedLiveCounselorPawn(params.ReturnValue))
        {
            return false;
        }

        // A manager candidate can be a side-specific search component (beds
        // are the important example). Preserve that exact candidate instead
        // of replacing it with the spot's generic KillerInteractable field.
        UObject* killerInteractable = component;
        if (!killerInteractable)
        {
            killerInteractable = ReadReflectedObjectProperty(
                reinterpret_cast<UObject*>(spot),
                "KillerInteractable");
        }
        if (!killerInteractable)
        {
            killerInteractable = ReadReflectedObjectProperty(
                reinterpret_cast<UObject*>(spot),
                "KillerInteractComponent");
        }
        if (!killerInteractable)
            killerInteractable = component;

        outSpot = spot;
        outCounselor = params.ReturnValue;
        outKillerInteractable = killerInteractable;
        return true;
    }

    bool FindNearbyOccupiedHidingSpot(
        AActor*& outSpot,
        AActor*& outCounselor,
        UObject*& outKillerInteractable)
    {
        outSpot = nullptr;
        outCounselor = nullptr;
        outKillerInteractable = nullptr;
        AActor* jason = g_JasonAIState.Jason;
        FVector jasonLocation{};
        if (!jason ||
            !GetJasonAIActorLocation(jason, jasonLocation))
        {
            return false;
        }

        float bestDistanceSquared = 600.0f * 600.0f;
        for (int32_t actorIndex = 0;
            actorIndex < g_HidingSpotRegistryCount;
            ++actorIndex)
        {
            AActor* spot = g_HidingSpotRegistry[actorIndex];
            if (!spot ||
                !Memory::IsReadable(spot, sizeof(UObject)) ||
                !ObjectClassDerivesFromExact(
                    reinterpret_cast<UObject*>(spot),
                    "SCHidingSpot"))
            {
                continue;
            }

            FVector spotLocation{};
            if (!GetJasonAIActorLocation(spot, spotLocation))
                continue;
            const float dx = spotLocation.X - jasonLocation.X;
            const float dy = spotLocation.Y - jasonLocation.Y;
            const float distanceSquared = dx * dx + dy * dy;
            if (!std::isfinite(distanceSquared) ||
                distanceSquared >= bestDistanceSquared)
            {
                continue;
            }

            UObject* component = ReadReflectedObjectProperty(
                reinterpret_cast<UObject*>(spot),
                "KillerInteractable");
            if (!component)
            {
                component = ReadReflectedObjectProperty(
                    reinterpret_cast<UObject*>(spot),
                    "KillerInteractComponent");
            }
            AActor* resolvedSpot = nullptr;
            AActor* counselor = nullptr;
            UObject* interactable = nullptr;
            if (ResolveHidingSpotInteraction(
                    component,
                    resolvedSpot,
                    counselor,
                    interactable))
            {
                bestDistanceSquared = distanceSquared;
                outSpot = resolvedSpot;
                outCounselor = counselor;
                outKillerInteractable = interactable;
            }
        }
        return outSpot && outCounselor && outKillerInteractable;
    }

    bool FaceJasonAIAtLocationOnGameThread(const FVector& targetLocation)
    {
        AActor* jason = g_JasonAIState.Jason;
        UObject* controller = g_JasonAIState.Controller;
        FVector jasonLocation{};
        if (!jason || !jason->Class ||
            !GetJasonAIActorLocation(jason, jasonLocation))
        {
            return false;
        }

        const float dx = targetLocation.X - jasonLocation.X;
        const float dy = targetLocation.Y - jasonLocation.Y;
        if (!std::isfinite(dx) || !std::isfinite(dy) ||
            dx * dx + dy * dy < 1.0f)
        {
            return false;
        }

        struct Rotation3 { float Pitch; float Yaw; float Roll; };
        constexpr float RadiansToDegrees = 57.29577951308232f;
        Rotation3 rotation{};
        rotation.Yaw = std::atan2(dy, dx) * RadiansToDegrees;

        bool actorOK = false;
        UFunction* setActorRotation = FindFunctionInHierarchyByName(
            jason->Class,
            "K2_SetActorRotation");
        if (setActorRotation)
        {
            struct Params
            {
                Rotation3 NewRotation;
                bool bTeleportPhysics;
                bool ReturnValue;
                uint8_t Padding[2];
            } params{};
            params.NewRotation = rotation;
            params.bTeleportPhysics = false;
            actorOK = SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(jason),
                jason,
                setActorRotation,
                &params) && params.ReturnValue;
        }

        bool controlOK = false;
        if (controller && controller->Class &&
            Memory::IsReadable(controller, sizeof(UObject)))
        {
            UFunction* setControlRotation = FindFunctionInHierarchyByName(
                controller->Class,
                "SetControlRotation");
            if (setControlRotation)
            {
                struct Params { Rotation3 NewRotation; } params{};
                params.NewRotation = rotation;
                controlOK = SafeProcessEventCall(
                    reinterpret_cast<uintptr_t>(controller),
                    controller,
                    setControlRotation,
                    &params);
            }
        }
        return actorOK || controlOK;
    }

    bool IsHidingSpotLockMatch(
        UObject* locked,
        UObject* requested,
        AActor* spot)
    {
        if (!locked)
            return false;
        if (locked == requested)
            return true;

        AActor** ownerField = reinterpret_cast<AActor**>(
            reinterpret_cast<uintptr_t>(locked) + 0xE0);
        return Memory::IsReadable(ownerField, sizeof(AActor*)) &&
            *ownerField == spot;
    }

    bool ReleaseStaleJasonHidingInteraction(
        UObject* manager,
        AActor* jason,
        ULONGLONG now,
        const char* reason)
    {
        if (!manager || !jason)
            return false;

        static UClass* cachedManagerClass = nullptr;
        static UFunction* cancelFunction = nullptr;
        if (manager->Class != cachedManagerClass)
        {
            cachedManagerClass = manager->Class;
            cancelFunction = FindFunctionInHierarchyByName(
                manager->Class,
                "CLIENT_CancelInteractAttempt");
            if (!cancelFunction)
            {
                cancelFunction = FindFunctionInHierarchyByName(
                    manager->Class,
                    "CLIENT_UnlockInteraction");
            }
        }

        const bool cancelled = cancelFunction &&
            SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(manager),
                manager,
                cancelFunction,
                nullptr);

        UFunction* endStun = jason->Class
            ? FindFunctionInHierarchyByName(jason->Class, "EndStun")
            : nullptr;
        const bool endedStun = endStun &&
            SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(jason),
                jason,
                endStun,
                nullptr);

        g_JasonAIState.GrabKillReadyAt = 0;
        g_JasonAIState.PathLocked = false;
        g_JasonAIState.PathLockUntil = 0;
        g_JasonAIState.LastAcceptedMoveAt = 0;
        ResetStuckSamplingAfterNativeInteraction(now);
        Logger::Debug(
            std::string("18L-AZ hiding spot: stale native state released | reason=") +
            (reason ? reason : "unknown") +
            " | cancel=" + (cancelled ? "true" : "false") +
            " | endStun=" + (endedStun ? "true" : "false"));
        return cancelled || endedStun;
    }

    bool RepositionJasonForHidingInteraction(
        AActor* jason,
        const FVector& interactionLocation,
        const FVector& currentLocation)
    {
        if (!jason || !jason->Class)
            return false;

        UFunction* teleportFunction = FindFunctionInHierarchyByName(
            jason->Class,
            "K2_TeleportTo");
        if (!teleportFunction)
            return false;

        struct Rotation3 { float Pitch; float Yaw; float Roll; };
        struct TeleportParams
        {
            FVector DestLocation;
            Rotation3 DestRotation;
            bool ReturnValue;
        };
        static_assert(sizeof(TeleportParams) == 28,
            "Hiding TeleportParams must be 28 bytes");

        constexpr float RadiansToDegrees = 57.29577951308232f;
        const FVector offsets[] =
        {
            FVector{ 95.0f, 0.0f, 0.0f },
            FVector{ -95.0f, 0.0f, 0.0f },
            FVector{ 0.0f, 95.0f, 0.0f },
            FVector{ 0.0f, -95.0f, 0.0f }
        };

        StopJasonAIMovementForKnifeOnGameThread();
        for (const FVector& offset : offsets)
        {
            TeleportParams params{};
            params.DestLocation.X = interactionLocation.X + offset.X;
            params.DestLocation.Y = interactionLocation.Y + offset.Y;
            // Component origins can sit high inside a bed, closet or tent.
            // Preserve Jason's collision-tested floor height.
            params.DestLocation.Z = currentLocation.Z;
            params.DestRotation.Yaw = std::atan2(
                interactionLocation.Y - params.DestLocation.Y,
                interactionLocation.X - params.DestLocation.X) *
                RadiansToDegrees;
            if (SafeProcessEventCall(
                    reinterpret_cast<uintptr_t>(jason),
                    jason,
                    teleportFunction,
                    &params) &&
                params.ReturnValue)
            {
                FaceJasonAIAtLocationOnGameThread(interactionLocation);
                Logger::Success(
                    "18L-AZ hiding spot: collision-checked interaction reposition completed");
                return true;
            }
        }
        return false;
    }

    void ClearActiveHidingSpotState()
    {
        g_HidingSpotTarget = nullptr;
        g_HidingSpotCounselor = nullptr;
        g_HidingSpotInteractable = nullptr;
        g_HidingSpotStartedAt = 0;
        g_HidingSpotAttemptPendingUntil = 0;
        g_NextHidingSpotMoveAt = 0;
        g_HidingSpotAttempts = 0;
        g_HidingSpotRepositionAttempted = false;
    }

    void TemporarilyBlockHidingCounselor(
        AActor* counselor,
        ULONGLONG until)
    {
        const int32_t index = FindJasonAITargetIndex(counselor);
        if (index >= 0 && index < 8)
            g_JasonAITargetBlockedUntil[index] = until;
    }

    bool AbandonRejectedHidingSpot(
        AActor* spot,
        AActor* counselor,
        const FVector& interactionLocation,
        const FVector& jasonLocation,
        ULONGLONG now)
    {
        ReleaseStaleJasonHidingInteraction(
            GetJasonInteractionManager(g_JasonAIState.Jason),
            g_JasonAIState.Jason,
            now,
            "bounded-abandon");
        const bool haveAlternative =
            HasAlternativeUsableJasonAITarget(counselor);
        const ULONGLONG retryDelay = haveAlternative ? 25000 : 7000;
        g_IgnoredHidingSpot = spot;
        g_IgnoredHidingCounselor = counselor;
        g_HidingSpotIgnoreUntil = now + retryDelay;
        TemporarilyBlockHidingCounselor(
            counselor,
            g_HidingSpotIgnoreUntil);

        g_JasonAIState.Target = nullptr;
        g_JasonAIState.PathLocked = false;
        g_JasonAIState.PathLockUntil = 0;
        g_JasonAIState.LastAcceptedMoveAt = 0;

        // When this is the last counselor, step away from the failed native
        // point before retrying. Repeating at the identical transform can
        // never satisfy the component's distance/yaw tests.
        if (!haveAlternative)
        {
            float awayX = jasonLocation.X - interactionLocation.X;
            float awayY = jasonLocation.Y - interactionLocation.Y;
            float awayLength = std::sqrt(awayX * awayX + awayY * awayY);
            if (!std::isfinite(awayLength) || awayLength < 1.0f)
            {
                awayX = 1.0f;
                awayY = 0.0f;
                awayLength = 1.0f;
            }
            FVector retreat = jasonLocation;
            retreat.X += awayX / awayLength * 450.0f;
            retreat.Y += awayY / awayLength * 450.0f;
            IssueAIMoveToLocationOnGameThread(
                g_JasonAIState.Controller,
                retreat,
                75.0f,
                "HidingRetryRetreat");
        }

        Logger::Debug(
            "18L-AX hiding spot: bounded native search exhausted; "
            "suppressing hidden-target combat and retargeting | spot=" +
            JasonAISafeName(reinterpret_cast<UObject*>(spot)) +
            " | counselor=" +
            JasonAISafeName(reinterpret_cast<UObject*>(counselor)) +
            " | attempts=" + std::to_string(g_HidingSpotAttempts) +
            " | retryMs=" + std::to_string(retryDelay) +
            " | alternative=" + (haveAlternative ? "true" : "false"));
        ClearActiveHidingSpotState();
        return true;
    }

    bool DriveHidingSpotInteraction(ULONGLONG now)
    {
        AActor* jason = g_JasonAIState.Jason;
        UObject* manager = GetJasonInteractionManager(jason);
        if (!jason || !manager)
            return false;

        if (g_HidingSpotIgnoreUntil != 0 &&
            now >= g_HidingSpotIgnoreUntil)
        {
            g_IgnoredHidingSpot = nullptr;
            g_IgnoredHidingCounselor = nullptr;
            g_HidingSpotIgnoreUntil = 0;
        }

        AActor* spot = nullptr;
        AActor* counselor = nullptr;
        UObject* killerInteractable = nullptr;

        // Keep a pending native interaction stable even if the manager clears
        // its hover candidate during the first animation/lock frames.
        if (g_HidingSpotInteractable)
        {
            ResolveHidingSpotInteraction(
                g_HidingSpotInteractable,
                spot,
                counselor,
                killerInteractable);
        }

        // The manager candidate is the cheapest and most authoritative route.
        const uintptr_t candidateOffsets[] = { 0x230, 0x210 };
        for (uintptr_t offset : candidateOffsets)
        {
            if (spot)
                break;
            UObject** candidate = reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(manager) + offset);
            if (Memory::IsReadable(candidate, sizeof(UObject*)) &&
                *candidate &&
                ResolveHidingSpotInteraction(
                    *candidate,
                    spot,
                    counselor,
                    killerInteractable))
            {
                break;
            }
        }

        if (!spot &&
            g_JasonAIState.ConsecutiveStuckChecks >= 1 &&
            now >= g_NextHidingSpotScanAt)
        {
            g_NextHidingSpotScanAt = now + 1000;
            FindNearbyOccupiedHidingSpot(
                spot,
                counselor,
                killerInteractable);
        }

        if (!spot || !counselor || !killerInteractable)
        {
            ClearActiveHidingSpotState();
            return false;
        }

        if (spot == g_IgnoredHidingSpot &&
            counselor == g_IgnoredHidingCounselor &&
            now < g_HidingSpotIgnoreUntil)
        {
            TemporarilyBlockHidingCounselor(
                counselor,
                g_HidingSpotIgnoreUntil);
            return false;
        }

        if (spot != g_HidingSpotTarget ||
            counselor != g_HidingSpotCounselor ||
            killerInteractable != g_HidingSpotInteractable)
        {
            ClearActiveHidingSpotState();
            g_HidingSpotTarget = spot;
            g_HidingSpotCounselor = counselor;
            g_HidingSpotInteractable = killerInteractable;
            g_HidingSpotStartedAt = now;
            Logger::Success(
                "18L-AX hiding spot: occupied native interaction acquired | spot=" +
                JasonAISafeName(reinterpret_cast<UObject*>(spot)) +
                " | counselor=" +
                JasonAISafeName(reinterpret_cast<UObject*>(counselor)) +
                " | component=" +
                JasonAISafeName(killerInteractable));
        }

        // A verified hidden counselor must never fall through to ordinary
        // slash/grab combat. This helper owns either approach, interaction,
        // or bounded abandonment/retargeting.
        g_JasonAIState.Target = counselor;

        FVector jasonLocation{};
        FVector interactionLocation{};
        if (!GetJasonAIActorLocation(jason, jasonLocation))
            return true;
        if (!GetSceneComponentLocation(
                killerInteractable,
                interactionLocation) &&
            !GetJasonAIActorLocation(spot, interactionLocation))
        {
            return true;
        }

        const float dx = interactionLocation.X - jasonLocation.X;
        const float dy = interactionLocation.Y - jasonLocation.Y;
        const float distance = std::sqrt(dx * dx + dy * dy);
        if (!std::isfinite(distance))
            return true;

        UObject* locked = GetLockedJasonInteractable(jason);
        if (IsHidingSpotLockMatch(
                locked,
                killerInteractable,
                spot))
        {
            // A real hiding kill completes quickly. A lock that survives this
            // bound is a failed montage/interaction, not useful progress.
            if (now >= g_HidingSpotStartedAt + 12000)
            {
                Logger::Debug(
                    "18L-AZ hiding spot: matching native lock timed out");
                return AbandonRejectedHidingSpot(
                    spot,
                    counselor,
                    interactionLocation,
                    jasonLocation,
                    now);
            }
            g_HidingSpotAttemptPendingUntil = 0;
            ResetStuckSamplingAfterNativeInteraction(now);
            return true;
        }
        if (locked)
        {
            // Never let an unrelated door, window, loot or rejected hiding
            // interaction own Jason forever. Give the stock transition a
            // brief grace period, then ask the manager to release it.
            if (now >= g_HidingSpotStartedAt + 2000)
            {
                Logger::Debug(
                    "18L-AZ hiding spot: foreign native lock timed out");
                return AbandonRejectedHidingSpot(
                    spot,
                    counselor,
                    interactionLocation,
                    jasonLocation,
                    now);
            }
            ResetStuckSamplingAfterNativeInteraction(now);
            return true;
        }

        if (now >= g_HidingSpotStartedAt + 14000 ||
            g_HidingSpotAttempts >= 6)
        {
            return AbandonRejectedHidingSpot(
                spot,
                counselor,
                interactionLocation,
                jasonLocation,
                now);
        }

        // Exported hiding assets use a 150 cm native DistanceLimit, measured
        // from a component offset inside the prop. Use a stronger margin than
        // the old 115 cm cutoff. If navmesh cannot reach that point, perform
        // one collision-tested local reposition after five seconds.
        if (distance > 85.0f)
        {
            if (!g_HidingSpotRepositionAttempted &&
                now >= g_HidingSpotStartedAt + 5000)
            {
                g_HidingSpotRepositionAttempted = true;
                if (RepositionJasonForHidingInteraction(
                        jason,
                        interactionLocation,
                        jasonLocation))
                {
                    g_NextHidingSpotInteractAt = now + 250;
                    return true;
                }
            }
            if (now >= g_NextHidingSpotMoveAt)
            {
                g_NextHidingSpotMoveAt = now + 1000;
                IssueAIMoveToLocationOnGameThread(
                    g_JasonAIState.Controller,
                    interactionLocation,
                    65.0f,
                    "OccupiedHidingPoint");
            }
            return true;
        }

        if (g_HidingSpotAttemptPendingUntil != 0 &&
            now < g_HidingSpotAttemptPendingUntil)
        {
            return true;
        }
        g_HidingSpotAttemptPendingUntil = 0;

        if (now >= g_NextHidingSpotInteractAt)
        {
            if (g_HidingSpotAttempts >= 2 &&
                !g_HidingSpotRepositionAttempted)
            {
                g_HidingSpotRepositionAttempted = true;
                if (RepositionJasonForHidingInteraction(
                        jason,
                        interactionLocation,
                        jasonLocation))
                {
                    g_NextHidingSpotInteractAt = now + 250;
                    return true;
                }
            }
            g_NextHidingSpotInteractAt = now + 850;
            StopJasonAIMovementForKnifeOnGameThread();
            FaceJasonAIAtLocationOnGameThread(interactionLocation);
            const bool dispatched =
                AttemptJasonHidingSpotComponent(killerInteractable);
            ++g_HidingSpotAttempts;
            if (dispatched)
                g_HidingSpotAttemptPendingUntil = now + 650;
            if (now >= g_NextHidingSpotLogAt)
            {
                g_NextHidingSpotLogAt = now + 2500;
                Logger::Debug(
                    "18L-AX hiding spot: native killer action dispatched | distanceCm=" +
                    std::to_string(distance) + " | attempt=" +
                    std::to_string(g_HidingSpotAttempts) + " | component=" +
                    JasonAISafeName(killerInteractable) + " | call=" +
                    (dispatched ? "true" : "false"));
            }
        }
        return true;
    }

    bool DriveOccupiedVehicleInterception(ULONGLONG now)
    {
        if (now < g_VehicleInterceptRetryAfter)
        {
            return false;
        }

        AActor* jason = g_JasonAIState.Jason;
        if (!jason)
            return false;

        UObject** extractionComponent = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(jason) + 0x1558);
        if (Memory::IsReadable(extractionComponent, sizeof(UObject*)) &&
            *extractionComponent)
        {
            ResetStuckSamplingAfterNativeInteraction(now);
            return true;
        }

        if (now < g_NextVehicleInterceptActionAt)
            return g_VehicleInterceptCar != nullptr;

        g_NextVehicleInterceptActionAt = now + 1000;
        UObject* seat = nullptr;
        AActor* occupant = nullptr;
        AActor* car = FindOccupiedEscapeCar(seat, occupant);
        if (!car || !seat || !occupant)
        {
            if (g_VehicleInterceptCar)
            {
                Logger::Debug(
                    "18L-AI vehicle intercept: occupant left/died or car escaped; resuming counselor hunt");
            }
            ResetVehicleInterceptionState(now + 2000);
            return false;
        }

        if (car != g_VehicleInterceptCar)
        {
            ResetVehicleInterceptionState();
            g_VehicleInterceptCar = car;
            g_VehicleInterceptSeat = seat;
            g_VehicleInterceptDeadline = now + 30000;
            g_JasonAIState.Target = occupant;
            Logger::Success(
                "18L-AI vehicle intercept: occupied started car acquired | car=" +
                JasonAISafeName(reinterpret_cast<UObject*>(car)) +
                " | counselor=" +
                JasonAISafeName(reinterpret_cast<UObject*>(occupant)));
        }
        else
        {
            g_VehicleInterceptSeat = seat;
            g_JasonAIState.Target = occupant;
        }

        if (g_VehicleInterceptDeadline != 0 &&
            now >= g_VehicleInterceptDeadline &&
            g_VehicleHoodInputSentAt == 0)
        {
            // Do not renew the same failed approach forever. Temporarily
            // exclude this car so the next bounded discovery can acquire a
            // second occupied escape car; with only one car, normal pursuit
            // gets a short reset before Jason approaches it again.
            g_IgnoredVehicleInterceptCar = car;
            g_IgnoredVehicleInterceptUntil = now + 15000;
            Logger::Error(
                "18L-BD vehicle intercept stalled; abandoning current approach and retargeting");
            ResetVehicleInterceptionState(now + 500);
            g_JasonAIState.Target = nullptr;
            ResetStuckSamplingAfterNativeInteraction(now);
            return false;
        }

        uint8_t* beingSlammed = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(car) + 0x5BA);
        if (Memory::IsReadable(beingSlammed, 1) && *beingSlammed != 0)
        {
            g_VehicleSlamObserved = true;
            StopJasonAIMovementForKnifeOnGameThread();
            ResetStuckSamplingAfterNativeInteraction(now);
            return true;
        }

        const float forwardSpeed = SafeVehicleForwardSpeed(car);
        const float absoluteSpeed = std::fabs(forwardSpeed);
        float* killerSlamSpeed = reinterpret_cast<float*>(
            reinterpret_cast<uintptr_t>(car) + 0x528);
        float* minDoorSpeed = reinterpret_cast<float*>(
            reinterpret_cast<uintptr_t>(car) + 0x604);
        const float liveSlamSpeed =
            Memory::IsReadable(killerSlamSpeed, sizeof(float)) &&
            std::isfinite(*killerSlamSpeed) && *killerSlamSpeed >= 0.0f
                ? *killerSlamSpeed
                : 5.0f;
        const float liveDoorSpeed =
            Memory::IsReadable(minDoorSpeed, sizeof(float)) &&
            std::isfinite(*minDoorSpeed) && *minDoorSpeed >= 0.0f
                ? *minDoorSpeed
                : 50.0f;
        uint8_t* started = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(car) + 0x47A);
        const bool carStarted =
            Memory::IsReadable(started, 1) && *started != 0;
        UObject* hood = nullptr;
        UObject** hoodComponent = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(car) + 0x3F0);
        if (Memory::IsReadable(hoodComponent, sizeof(UObject*)) &&
            *hoodComponent &&
            Memory::IsReadable(*hoodComponent, 0x2A1))
        {
            hood = *hoodComponent;
        }
        const bool hoodEnabled = hood &&
            *reinterpret_cast<uint8_t*>(
                reinterpret_cast<uintptr_t>(hood) + 0x2A0) != 0;

        FVector jasonLocation{};
        FVector carLocation{};
        if (!GetJasonAIActorLocation(jason, jasonLocation) ||
            !GetJasonAIActorLocation(car, carLocation))
        {
            return true;
        }

        const float dx = carLocation.X - jasonLocation.X;
        const float dy = carLocation.Y - jasonLocation.Y;
        const float horizontalDistance = std::sqrt(dx * dx + dy * dy);
        FVector travelDirection{};
        float horizontalSpeed = 0.0f;
        const bool haveDirection = GetVehicleTravelDirection(
            car,
            forwardSpeed,
            travelDirection,
            horizontalSpeed);

        if (absoluteSpeed > liveSlamSpeed && haveDirection)
        {
            if (g_VehicleSlamObserved && carStarted)
            {
                // The same car genuinely restarted after a completed slam;
                // this is a new stop episode and may receive one new hood hit.
                g_VehicleHoodInputSentAt = 0;
                g_VehicleSlamObserved = false;
                g_VehicleExtractionInputAt = 0;
                g_VehicleExtractionAttempts = 0;
                g_VehicleDriverDetourReached = false;
                g_VehicleDriverDetourAttempts = 0;
            }

            const float headingDot =
                g_VehicleHeadingStableSince == 0
                    ? 1.0f
                    : (g_VehicleInterceptHeading.X * travelDirection.X +
                       g_VehicleInterceptHeading.Y * travelDirection.Y);
            if (g_VehicleHeadingStableSince == 0 || headingDot < 0.90f)
            {
                g_VehicleInterceptHeading = travelDirection;
                g_VehicleHeadingStableSince = now;
            }

            if (g_VehicleInterceptMorphUsed && headingDot < 0.25f)
            {
                Logger::Debug(
                    "18L-AI vehicle intercept: car reversed/turned; retaining priority and re-arming next charged Morph");
                g_VehicleInterceptMorphUsed = false;
                g_VehicleInterceptDeadline = now + 30000;
                g_VehicleInterceptHeading = travelDirection;
                g_VehicleHeadingStableSince = now;
            }

            if (!g_VehicleInterceptMorphUsed &&
                now >= g_VehicleHeadingStableSince + 500 &&
                TeleportJasonAheadOfVehicle(
                    car,
                    travelDirection,
                    horizontalSpeed,
                    now))
            {
                g_VehicleInterceptMorphUsed = true;
                g_VehicleInterceptDeadline = now + 15000;
                return true;
            }

            const float carToJasonX = jasonLocation.X - carLocation.X;
            const float carToJasonY = jasonLocation.Y - carLocation.Y;
            const float aheadDot = horizontalDistance > 1.0f
                ? ((carToJasonX / horizontalDistance) * travelDirection.X +
                   (carToJasonY / horizontalDistance) * travelDirection.Y)
                : 0.0f;
            if (g_VehicleInterceptMorphUsed &&
                JasonAIMorphRemainingMs(now) == 0 &&
                (aheadDot < 0.10f || horizontalDistance > 1800.0f))
            {
                g_VehicleInterceptMorphUsed = false;
                g_VehicleHeadingStableSince = now;
                g_VehicleInterceptDeadline = now + 30000;
                Logger::Debug(
                    "18L-AI vehicle intercept: car passed/extended lead; re-arming charged road interception");
            }
            if (horizontalDistance <= 900.0f && aheadDot > 0.20f)
            {
                FVector hoodPoint{};
                if (ResolveOccupiedCarFrontHoodPoint(
                        car,
                        travelDirection,
                        hoodPoint))
                {
                    const float hoodDX = hoodPoint.X - jasonLocation.X;
                    const float hoodDY = hoodPoint.Y - jasonLocation.Y;
                    const float hoodDistance = std::sqrt(
                        hoodDX * hoodDX + hoodDY * hoodDY);
                    if (hoodDistance > 120.0f)
                    {
                        IssueAIMoveToLocationOnGameThread(
                            g_JasonAIState.Controller,
                            hoodPoint,
                            90.0f,
                            "OccupiedCarFront");
                        return true;
                    }
                }
                // The real front overlap now owns the slam. Pressing interact
                // against a fast hood is rejected by stock code.
                StopJasonAIMovementForKnifeOnGameThread();
                FaceJasonAIAtStartupTrapObjectiveOnGameThread(car);
            }
            else
            {
                IssueJasonAITrapMoveToActorOnGameThread(
                    car,
                    "OccupiedCar");
            }
            return true;
        }

        const bool hoodSequenceComplete =
            !carStarted &&
            (!hoodEnabled ||
             (g_VehicleHoodInputSentAt != 0 &&
              (g_VehicleSlamObserved ||
               now >= g_VehicleHoodInputSentAt + 4000)));

        if (hoodSequenceComplete && absoluteSpeed <= liveDoorSpeed)
        {
            FVector doorPoint{};
            float doorDistance = FLT_MAX;
            const bool haveDoorPoint = ResolveDriverDoorNavPoint(
                car,
                seat,
                doorPoint);
            if (haveDoorPoint)
            {
                const float ddx = doorPoint.X - jasonLocation.X;
                const float ddy = doorPoint.Y - jasonLocation.Y;
                doorDistance = std::sqrt(ddx * ddx + ddy * ddy);

                if (g_VehicleDriverApproachStartedAt == 0)
                {
                    g_VehicleDriverApproachStartedAt = now;
                    g_VehicleDriverLastProgressAt = now;
                    g_VehicleDriverBestDistance = doorDistance;
                }
                else if (doorDistance + 35.0f <
                         g_VehicleDriverBestDistance)
                {
                    g_VehicleDriverBestDistance = doorDistance;
                    g_VehicleDriverLastProgressAt = now;
                }
            }

            if (!haveDoorPoint)
            {
                StopJasonAIMovementForKnifeOnGameThread();
                if (g_VehicleDriverApproachStartedAt == 0)
                    g_VehicleDriverApproachStartedAt = now;
                if (now >= g_NextVehicleInterceptLogAt)
                {
                    g_NextVehicleInterceptLogAt = now + 2000;
                    Logger::Error(
                        "18L-AI vehicle intercept: driver seat had no reachable side-door nav point; holding instead of pathing into hood");
                }
                if (now >= g_VehicleDriverApproachStartedAt + 4000)
                {
                    g_JasonAIState.Target = occupant;
                    Logger::Error(
                        "18L-BC vehicle driver door unavailable; releasing Jason to normal combat before retry");
                    ResetVehicleInterceptionState(now + 4500);
                    ResetStuckSamplingAfterNativeInteraction(now);
                    return false;
                }
                return true;
            }

            // A blocked hood/door navmesh used to hold this routine forever,
            // rebuilding the same MoveTo once per second. Release Jason back
            // to stock combat after a short no-progress window. The occupied
            // car remains his target and this route may reacquire it after the
            // cooldown, giving normal slash/path recovery a chance in between.
            const bool driverApproachStalled =
                g_VehicleDriverLastProgressAt != 0 &&
                now >= g_VehicleDriverLastProgressAt + 2500;
            const bool driverApproachExpired =
                g_VehicleDriverApproachStartedAt != 0 &&
                now >= g_VehicleDriverApproachStartedAt + 6500;
            if (driverApproachStalled || driverApproachExpired)
            {
                StopJasonAIMovementForKnifeOnGameThread();
                if (g_VehicleDriverDetourAttempts == 0)
                {
                    // The nearest end can be blocked by the hood, a fence, or
                    // the stopped car's collision. Retry immediately around
                    // the opposite end instead of running into the same point
                    // for the rest of the extraction window.
                    g_VehicleDriverDetourAttempts = 1;
                    g_VehicleDriverDetourReached = false;
                    g_VehicleDriverApproachStartedAt = now;
                    g_VehicleDriverLastProgressAt = now;
                    g_VehicleDriverBestDistance = doorDistance;
                    Logger::Error(
                        "18L-BG vehicle driver-door approach stalled; switching to opposite-end detour");
                    ResetStuckSamplingAfterNativeInteraction(now);
                    return true;
                }
                g_JasonAIState.Target = occupant;
                Logger::Error(
                    "18L-BC vehicle driver-door approach stalled; releasing Jason to normal combat before bounded retry | distanceCm=" +
                    std::to_string(doorDistance));
                ResetVehicleInterceptionState(now + 4500);
                ResetStuckSamplingAfterNativeInteraction(now);
                return false;
            }

            if (!g_VehicleDriverDetourReached)
            {
                const float sideX = doorPoint.X - carLocation.X;
                const float sideY = doorPoint.Y - carLocation.Y;
                const float sideLength = std::sqrt(
                    sideX * sideX + sideY * sideY);
                const float driverSideProgress =
                    sideLength > 1.0f
                        ? ((jasonLocation.X - carLocation.X) *
                               (sideX / sideLength) +
                           (jasonLocation.Y - carLocation.Y) *
                               (sideY / sideLength))
                        : 0.0f;

                if (driverSideProgress < 120.0f)
                {
                    FVector detourPoint{};
                        if (ResolveDriverSideDetourPoint(
                                car,
                                doorPoint,
                                jasonLocation,
                                g_VehicleDriverDetourAttempts != 0,
                                detourPoint))
                    {
                        const float detourDX =
                            detourPoint.X - jasonLocation.X;
                        const float detourDY =
                            detourPoint.Y - jasonLocation.Y;
                        const float detourDistance = std::sqrt(
                            detourDX * detourDX + detourDY * detourDY);
                        if (detourDistance > 125.0f)
                        {
                            IssueAIMoveToLocationOnGameThread(
                                g_JasonAIState.Controller,
                                detourPoint,
                                85.0f,
                                "DriverSideDetour");
                            if (now >= g_NextVehicleInterceptLogAt)
                            {
                                g_NextVehicleInterceptLogAt = now + 2000;
                                Logger::Debug(
                                    "18L-AR vehicle intercept: routing around car body before driver-door approach");
                            }
                            return true;
                        }
                    }
                }

                g_VehicleDriverDetourReached = true;
                Logger::Success(
                    "18L-AR vehicle intercept: driver-side detour complete; closing on extraction point");
            }

            if (doorDistance > 135.0f)
            {
                g_VehicleDriverReadySince = 0;
                IssueAIMoveToLocationOnGameThread(
                    g_JasonAIState.Controller,
                    doorPoint,
                    70.0f,
                    "DriverDoorSide");
                return true;
            }

            StopJasonAIMovementForKnifeOnGameThread();
            FaceJasonAIAtStartupTrapObjectiveOnGameThread(occupant);
            if (g_VehicleDriverReadySince == 0)
            {
                // Do not dispatch the native extraction on the same frame
                // path following reports arrival. Let movement settle and the
                // driver-side overlap publish before pressing interact.
                g_VehicleDriverReadySince = now;
                return true;
            }
            if (now < g_VehicleDriverReadySince + 650)
                return true;

            const bool extractionReady =
                g_VehicleExtractionInputAt == 0 ||
                (now >= g_VehicleExtractionInputAt + 3000 &&
                 g_VehicleExtractionAttempts < 3);
            if (extractionReady && AttemptJasonVehicleComponent(seat))
            {
                ++g_VehicleExtractionAttempts;
                g_VehicleExtractionInputAt = now;
                Logger::Success(
                    "18L-AI vehicle intercept: driver-side extraction input sent | attempt=" +
                    std::to_string(g_VehicleExtractionAttempts) +
                    " | counselor=" +
                    JasonAISafeName(reinterpret_cast<UObject*>(occupant)));
            }
            return true;
        }

        if (g_VehicleHoodInputSentAt != 0)
        {
            // AttemptInteract is a void input dispatch. Once it has been sent,
            // wait for the stock slam pulse/completion instead of hammering A
            // every 800 ms. This latch survives the full stopped-car episode.
            StopJasonAIMovementForKnifeOnGameThread();
            FaceJasonAIAtStartupTrapObjectiveOnGameThread(car);
            if (now >= g_NextVehicleInterceptLogAt)
            {
                g_NextVehicleInterceptLogAt = now + 2000;
                Logger::Debug(
                    "18L-AI vehicle intercept: one hood input latched; waiting for stock slam completion | started=" +
                    std::to_string(carStarted ? 1 : 0) +
                    " | slamObserved=" +
                    std::to_string(g_VehicleSlamObserved ? 1 : 0));
            }
            return true;
        }

        if (horizontalDistance > 425.0f)
        {
            FVector hoodStopPoint{};
            FVector fallbackForward{};
            if (!std::isfinite(travelDirection.X) ||
                !std::isfinite(travelDirection.Y) ||
                (travelDirection.X == 0.0f && travelDirection.Y == 0.0f))
            {
                GetJasonAIActorForwardVectorOnGameThread(car, fallbackForward);
            }
            else
            {
                fallbackForward = travelDirection;
            }
            if (ResolveOccupiedCarFrontHoodPoint(
                    car,
                    fallbackForward,
                    hoodStopPoint))
            {
                IssueAIMoveToLocationOnGameThread(
                    g_JasonAIState.Controller,
                    hoodStopPoint,
                    85.0f,
                    "StoppedCarFront");
            }
            else
            {
                IssueJasonAITrapMoveToActorOnGameThread(
                    car,
                    "StoppedCar");
            }
            return true;
        }

        StopJasonAIMovementForKnifeOnGameThread();
        FaceJasonAIAtStartupTrapObjectiveOnGameThread(car);
        if (absoluteSpeed <= liveSlamSpeed && hood)
        {
            if (AttemptJasonVehicleComponent(hood))
            {
                g_VehicleHoodInputSentAt = now;
                Logger::Success(
                    "18L-AI vehicle intercept: single stock hood-destroy input sent and latched");
            }
            return true;
        }

        if (now >= g_NextVehicleInterceptLogAt)
        {
            g_NextVehicleInterceptLogAt = now + 2000;
            Logger::Debug(
                "18L-AI vehicle intercept: waiting for stock speed/slam transition | speed=" +
                std::to_string(forwardSpeed) +
                " | slamThreshold=" + std::to_string(liveSlamSpeed) +
                " | doorThreshold=" + std::to_string(liveDoorSpeed));
        }
        return true;
    }

    void LogFusePlacementDiagnostic(ULONGLONG now)
    {
        if (g_FuseDiagnosticLogged ||
            g_FuseDiagnosticAt == 0 ||
            now < g_FuseDiagnosticAt)
        {
            return;
        }

        g_FuseDiagnosticLogged = true;
        UWorld* world = g_JasonAIState.World;
        if (!world || !Memory::IsReadable(world, sizeof(UWorld)))
            return;

        TArray<ULevel*>* levels = reinterpret_cast<TArray<ULevel*>*>(
            reinterpret_cast<uintptr_t>(world) + 0x110);
        if (!Memory::IsReadable(levels, sizeof(TArray<ULevel*>)) ||
            !levels->Data ||
            levels->Count <= 0 ||
            levels->Count > 1024 ||
            !Memory::IsReadable(
                levels->Data,
                sizeof(ULevel*) * static_cast<size_t>(levels->Count)))
        {
            return;
        }

        int32_t phoneFuseActors = 0;
        int32_t configuredFuseCount = -1;
        int32_t gasCanActors = 0;
        int32_t carriedGasCanActors = 0;
        int32_t vehicleGasRepairComponents = 0;
        int32_t vehicleGasRequiredEntries = 0;
        for (int32_t levelIndex = 0; levelIndex < levels->Count; ++levelIndex)
        {
            ULevel* level = levels->Data[levelIndex];
            if (!level || !Memory::IsReadable(level, sizeof(ULevel)))
                continue;

            TArray<AActor*>& actors = level->Actors;
            if (!actors.Data ||
                actors.Count <= 0 ||
                actors.Count > 100000 ||
                !Memory::IsReadable(
                    actors.Data,
                    sizeof(AActor*) * static_cast<size_t>(actors.Count)))
            {
                continue;
            }

            for (int32_t i = 0; i < actors.Count; ++i)
            {
                AActor* actor = actors.Data[i];
                if (!actor || !Memory::IsReadable(actor, sizeof(UObject)))
                    continue;

                if (JasonAIObjectDerivesFromNameContaining(
                        reinterpret_cast<UObject*>(actor),
                        "SCWorldSettings"))
                {
                    int32_t* totalFuseCount = reinterpret_cast<int32_t*>(
                        reinterpret_cast<uintptr_t>(actor) + 0x7C0);
                    if (Memory::IsReadable(totalFuseCount, sizeof(int32_t)))
                        configuredFuseCount = *totalFuseCount;
                }

                const std::string actorName =
                    JasonAISafeName(reinterpret_cast<UObject*>(actor));
                const std::string className = actor->Class
                    ? JasonAISafeName(reinterpret_cast<UObject*>(actor->Class))
                    : std::string();

                // The donor and Resurrected CarGasCan assets are byte-exact,
                // and both vehicle blueprints still require CarGasCan_C.
                // Count the actual live world population before attempting
                // any spawn repair.  A carried repair item remains a world
                // actor; its reflected Owner identifies the counselor/bot.
                if (actorName.find("CarGasCan") != std::string::npos ||
                    className.find("CarGasCan") != std::string::npos)
                {
                    ++gasCanActors;
                    AActor* owner = nullptr;
                    if (actor->Class)
                    {
                        UPropertyLite* ownerProperty =
                            FindPropertyInHierarchyByName(
                                actor->Class,
                                "Owner");
                        if (ownerProperty &&
                            ownerProperty->Offset_Internal > 0 &&
                            ownerProperty->Offset_Internal < 0x10000)
                        {
                            owner = ReadActorField(
                                reinterpret_cast<UObject*>(actor),
                                static_cast<uintptr_t>(
                                    ownerProperty->Offset_Internal));
                        }
                    }
                    if (owner)
                        ++carriedGasCanActors;

                    FVector gasLocation{};
                    GetJasonAIActorLocation(actor, gasLocation);
                    Logger::Success(
                        "18L-AK gas census actor[" +
                        std::to_string(gasCanActors) + "]=" + actorName +
                        " | class=" + className +
                        " | owner=" +
                        (owner
                            ? JasonAISafeName(
                                reinterpret_cast<UObject*>(owner))
                            : std::string("<world>")) +
                        " | location=" +
                        std::to_string(gasLocation.X) + "," +
                        std::to_string(gasLocation.Y) + "," +
                        std::to_string(gasLocation.Z));
                }

                uint8_t vehicleKind = 0;
                int32_t vehicleSeats = 0;
                if (actor->Class &&
                    ClassifyRepairableCar(
                        actor,
                        vehicleKind,
                        vehicleSeats))
                {
                    UPropertyLite* gasTankProperty =
                        FindPropertyInHierarchyByName(
                            actor->Class,
                            "GasTankRepair");
                    UObject* gasTankRepair = nullptr;
                    if (gasTankProperty &&
                        gasTankProperty->Offset_Internal > 0 &&
                        gasTankProperty->Offset_Internal < 0x10000)
                    {
                        UObject** field = reinterpret_cast<UObject**>(
                            reinterpret_cast<uintptr_t>(actor) +
                            gasTankProperty->Offset_Internal);
                        if (Memory::IsReadable(field, sizeof(UObject*)))
                            gasTankRepair = *field;
                    }

                    int32_t requiredPartCount = -1;
                    if (gasTankRepair &&
                        Memory::IsReadable(gasTankRepair, sizeof(UObject)) &&
                        gasTankRepair->Class &&
                        Memory::IsReadable(
                            gasTankRepair->Class,
                            sizeof(UClass)))
                    {
                        ++vehicleGasRepairComponents;
                        UPropertyLite* requiredProperty =
                            FindPropertyInHierarchyByName(
                                gasTankRepair->Class,
                                "RequiredPartClasses");
                        if (requiredProperty &&
                            requiredProperty->Offset_Internal > 0 &&
                            requiredProperty->Offset_Internal < 0x10000)
                        {
                            TArray<uint8_t>* requiredParts =
                                reinterpret_cast<TArray<uint8_t>*>(
                                    reinterpret_cast<uintptr_t>(
                                        gasTankRepair) +
                                    requiredProperty->Offset_Internal);
                            if (Memory::IsReadable(
                                    requiredParts,
                                    sizeof(TArray<uint8_t>)) &&
                                requiredParts->Count >= 0 &&
                                requiredParts->Count <= 32)
                            {
                                requiredPartCount = requiredParts->Count;
                                vehicleGasRequiredEntries +=
                                    requiredPartCount;
                            }
                        }
                    }

                    Logger::Success(
                        "18L-AK gas repair vehicle=" + actorName +
                        " | GasTankRepair=" +
                        (gasTankRepair
                            ? JasonAISafeName(gasTankRepair)
                            : std::string("<missing>")) +
                        " | RequiredPartClasses=" +
                        std::to_string(requiredPartCount));
                }

                if (actorName.find("PhoneBoxFuse") == std::string::npos &&
                    className.find("PhoneBoxFuse") == std::string::npos)
                {
                    continue;
                }

                ++phoneFuseActors;
                FVector location{};
                GetJasonAIActorLocation(actor, location);
                Logger::Success(
                    "18L-DIAG phone fuse actor[" +
                    std::to_string(phoneFuseActors) + "]=" + actorName +
                    " | class=" + className +
                    " | location=" + std::to_string(location.X) + "," +
                    std::to_string(location.Y) + "," +
                    std::to_string(location.Z));
            }
        }

        Logger::Success(
            "18L-DIAG phone fuse summary: WorldSettings.TotalFuseCount=" +
            std::to_string(configuredFuseCount) +
            " | livePhoneBoxFuseActors=" +
            std::to_string(phoneFuseActors));

        Logger::Success(
            "18L-AK gas census summary: liveCarGasCanActors=" +
            std::to_string(gasCanActors) +
            " | counselorOrBotOwned=" +
            std::to_string(carriedGasCanActors) +
            " | vehicleGasRepairComponents=" +
            std::to_string(vehicleGasRepairComponents) +
            " | totalRequiredPartEntries=" +
            std::to_string(vehicleGasRequiredEntries));
    }

    bool ResolveCounselorRouteStartupObjectives(UWorld* world)
    {
        if (!world ||
            !Memory::IsReadable(world, sizeof(UWorld)) ||
            g_JasonAIState.StartupTrapObjectivesReady)
        {
            return g_JasonAIState.StartupTrapObjectivesReady;
        }

        // UWorld::GameState is authoritative for this route.  Preserve the
        // frozen bounded scan only as a diagnostic/fallback.
        AActor* scannedGameState = FindJasonAIStartupTrapGameStateOnce();
        AActor* authoritativeGameState = ReadActorField(
            reinterpret_cast<UObject*>(world),
            0xF8);

        if (!authoritativeGameState ||
            !JasonAIObjectDerivesFromNameContaining(
                reinterpret_cast<UObject*>(authoritativeGameState),
                "SCGameState"))
        {
            authoritativeGameState = scannedGameState;
        }

        if (!authoritativeGameState)
            return false;

        g_JasonAIState.StartupTrapGameState = authoritativeGameState;
        g_JasonAIState.StartupTrapDiscoveryScanDone = true;

        AActor* phone = ReadActorField(
            reinterpret_cast<UObject*>(authoritativeGameState),
            0x590);
        AActor* car2 = ReadActorField(
            reinterpret_cast<UObject*>(authoritativeGameState),
            0x578);
        AActor* car4 = ReadActorField(
            reinterpret_cast<UObject*>(authoritativeGameState),
            0x570);
        std::string car2Source = car2 ? "GameStateCache" : "missing";
        std::string car4Source = car4 ? "GameStateCache" : "missing";

        uint8_t cachedKind = 0;
        int32_t cachedSeats = 0;
        if (car2 &&
            (!ClassifyRepairableCar(car2, cachedKind, cachedSeats) ||
             cachedKind != 2))
        {
            car2 = nullptr;
            car2Source = "invalid-cache";
        }
        if (car4 &&
            (!ClassifyRepairableCar(car4, cachedKind, cachedSeats) ||
             cachedKind != 3))
        {
            car4 = nullptr;
            car4Source = "invalid-cache";
        }

        // Prefer the GameMode's stock SpawnedVehicles array, then reproduce
        // the game's bounded SCDriveableVehicle/type/seat scan if needed.
        AActor* gameMode = ReadActorField(
            reinterpret_cast<UObject*>(world),
            0xF0);
        if ((!car2 || !car4) && gameMode)
        {
            TArray<AActor*>* spawnedVehicles =
                reinterpret_cast<TArray<AActor*>*>(
                    reinterpret_cast<uintptr_t>(gameMode) + 0x720);
            ScanVehicleArray(
                spawnedVehicles,
                "GameMode.SpawnedVehicles",
                car2,
                car4,
                car2Source,
                car4Source);
        }

        if (!car2 || !car4)
        {
            constexpr uintptr_t Offset_Levels = 0x110;
            TArray<ULevel*>* levels = reinterpret_cast<TArray<ULevel*>*>(
                reinterpret_cast<uintptr_t>(world) + Offset_Levels);

            if (Memory::IsReadable(levels, sizeof(TArray<ULevel*>)) &&
                levels->Data &&
                levels->Count > 0 &&
                levels->Count <= 1024 &&
                Memory::IsReadable(
                    levels->Data,
                    sizeof(ULevel*) * static_cast<size_t>(levels->Count)))
            {
                for (int32_t levelIndex = 0;
                    levelIndex < levels->Count && (!car2 || !car4);
                    ++levelIndex)
                {
                    ULevel* level = levels->Data[levelIndex];
                    if (!level || !Memory::IsReadable(level, sizeof(ULevel)))
                        continue;

                    ScanVehicleArray(
                        &level->Actors,
                        "World.LevelActors",
                        car2,
                        car4,
                        car2Source,
                        car4Source);
                }
            }
        }

        // The widened stock HUD widgets call SCGameState's real objective
        // getters. OfflineBots did not populate its inherited car caches in
        // the successful counselor run, even though both authoritative cars
        // were present in GameMode.SpawnedVehicles. Publish only the two
        // validated car actors into those inherited cache slots so the stock
        // repair/start/escape checkmarks query the same objects as Jason.
        bool car2CachePublished = false;
        bool car4CachePublished = false;
        if (car2)
        {
            AActor** car2Cache = reinterpret_cast<AActor**>(
                reinterpret_cast<uintptr_t>(authoritativeGameState) + 0x578);
            if (Memory::IsReadable(car2Cache, sizeof(AActor*)))
            {
                *car2Cache = car2;
                car2CachePublished = *car2Cache == car2;
            }
        }
        if (car4)
        {
            AActor** car4Cache = reinterpret_cast<AActor**>(
                reinterpret_cast<uintptr_t>(authoritativeGameState) + 0x570);
            if (Memory::IsReadable(car4Cache, sizeof(AActor*)))
            {
                *car4Cache = car4;
                car4CachePublished = *car4Cache == car4;
            }
        }

        g_JasonAIState.StartupTrapInteriorPhone = phone;
        g_JasonAIState.StartupTrapObjectiveCount = 0;
        g_JasonAIState.StartupTrapObjectiveIndex = 0;

        // Resolve the shack entrance once during the existing startup-objective
        // census. Jason_Shack's entrance is an embedded scene component, not a
        // separate BP_CabinDoor actor. Reading that component's world transform
        // gives an exact cross-map doorway anchor without any recurring scan.
        AActor* shack = FindNearestWorldActorByClass(
            "Jason_Shack_C",
            nullptr,
            0.0f);
        UObject* shackDoorComponent = nullptr;
        FVector shackLocation{};
        FVector shackDoorLocation{};
        FVector shackExteriorDirection{};
        bool shackDoorResolved = false;
        if (shack && GetJasonAIActorLocation(shack, shackLocation))
        {
            shackDoorComponent = ReadReflectedObjectProperty(
                reinterpret_cast<UObject*>(shack),
                "Shack_Jason_door_01");
            if (shackDoorComponent &&
                Memory::IsReadable(
                    shackDoorComponent,
                    Offsets::Scene_ComponentToWorld +
                        Offsets::FTransform_Translation +
                        sizeof(FVector)))
            {
                FVector* componentWorldLocation = reinterpret_cast<FVector*>(
                    reinterpret_cast<uintptr_t>(shackDoorComponent) +
                    Offsets::Scene_ComponentToWorld +
                    Offsets::FTransform_Translation);
                if (Memory::IsReadable(
                        componentWorldLocation,
                        sizeof(FVector)))
                {
                    shackDoorLocation = *componentWorldLocation;
                    const float dx = shackDoorLocation.X - shackLocation.X;
                    const float dy = shackDoorLocation.Y - shackLocation.Y;
                    const float horizontalLength = std::sqrt(dx * dx + dy * dy);
                    if (std::isfinite(horizontalLength) &&
                        horizontalLength >= 25.0f)
                    {
                        // The mesh component named Shack_Jason_door_01 is a
                        // stable rotation reference, but the physical test
                        // proved it is not the walkable exterior threshold.
                        // AJ's dropped-axe marker measured the real threshold
                        // as this blueprint-local offset from that component.
                        // Express it in the component/shack basis so the same
                        // calibration rotates with every map's shack.
                        FVector componentRadial{};
                        componentRadial.X = dx / horizontalLength;
                        componentRadial.Y = dy / horizontalLength;
                        FVector componentLateral{};
                        componentLateral.X = -componentRadial.Y;
                        componentLateral.Y = componentRadial.X;

                        constexpr float MarkerRadialOffset = -240.812576f;
                        constexpr float MarkerLateralOffset = -776.665588f;
                        constexpr float MarkerHeightOffset = 0.878663f;
                        shackDoorLocation.X +=
                            componentRadial.X * MarkerRadialOffset +
                            componentLateral.X * MarkerLateralOffset;
                        shackDoorLocation.Y +=
                            componentRadial.Y * MarkerRadialOffset +
                            componentLateral.Y * MarkerLateralOffset;
                        shackDoorLocation.Z += MarkerHeightOffset;

                        const float exteriorDX =
                            shackDoorLocation.X - shackLocation.X;
                        const float exteriorDY =
                            shackDoorLocation.Y - shackLocation.Y;
                        const float exteriorLength = std::sqrt(
                            exteriorDX * exteriorDX +
                            exteriorDY * exteriorDY);
                        if (std::isfinite(exteriorLength) &&
                            exteriorLength >= 25.0f)
                        {
                            shackExteriorDirection.X =
                                exteriorDX / exteriorLength;
                            shackExteriorDirection.Y =
                                exteriorDY / exteriorLength;
                            shackExteriorDirection.Z = 0.0f;
                            shackDoorResolved = true;
                        }
                    }
                }
            }
        }
        g_JasonAIState.StartupTrapShackDoorLocation = shackDoorLocation;
        g_JasonAIState.StartupTrapShackExteriorDirection =
            shackExteriorDirection;
        g_JasonAIState.StartupTrapShackDoorResolved = shackDoorResolved;

        auto addObjective = [](AActor* actor, uint8_t kind)
        {
            if (!actor || g_JasonAIState.StartupTrapObjectiveCount >= 4)
                return;

            const int32_t index = g_JasonAIState.StartupTrapObjectiveCount++;
            g_JasonAIState.StartupTrapObjectives[index] = actor;
            g_JasonAIState.StartupTrapObjectiveKinds[index] = kind;
        };

        // The stable shack actor owns the component and remains the objective;
        // kind 4 uses the separately cached doorway transform for placement.
        if (shackDoorResolved)
            addObjective(shack, 4);
        addObjective(phone, 1);
        addObjective(car2, 2);
        addObjective(car4, 3);
        ++g_JasonAIState.StartupTrapDiscoveryReads;
        g_JasonAIState.StartupTrapObjectivesReady = true;

        Logger::Success(
            "18L-AG counselor bridge objectives resolved: shackEntrance=" +
            (shackDoorResolved
                ? JasonAISafeName(shackDoorComponent)
                : "NULL") +
            " | shackDoor=" +
            std::to_string(shackDoorLocation.X) + "," +
            std::to_string(shackDoorLocation.Y) + "," +
            std::to_string(shackDoorLocation.Z) +
            " | shackExterior=" +
            std::to_string(shackExteriorDirection.X) + "," +
            std::to_string(shackExteriorDirection.Y) +
            " | phone=" +
            (phone ? JasonAISafeName(reinterpret_cast<UObject*>(phone)) : "NULL") +
            " | car2=" +
            (car2 ? JasonAISafeName(reinterpret_cast<UObject*>(car2)) : "NULL") +
            "(" + car2Source + ") | car4=" +
            (car4 ? JasonAISafeName(reinterpret_cast<UObject*>(car4)) : "NULL") +
            "(" + car4Source + ") | authoritativeGS=" +
            JasonAISafeName(reinterpret_cast<UObject*>(authoritativeGameState)) +
            " | scannedSame=" +
            (authoritativeGameState == scannedGameState ? "true" : "false") +
            " | carCaches=" +
            (car2CachePublished ? "2" : "-") + "/" +
            (car4CachePublished ? "4" : "-") +
            " | count=" +
            std::to_string(g_JasonAIState.StartupTrapObjectiveCount));
        return true;
    }

    bool IsCounselorRouteSoundBlipEmitter(UObject* killer)
    {
        return killer &&
            g_JasonAIState.Active &&
            killer == reinterpret_cast<UObject*>(g_JasonAIState.Jason) &&
            g_JasonAIState.World &&
            g_JasonAIState.World == Engine::GetWorld();
    }

    __declspec(noinline) int32_t CallContextKillEligibilityWithTemporaryHunter(
        ContextKillCanInteractFn original,
        UObject* contextKillComponent,
        AActor* interactor,
        const FVector* viewLocation,
        const FVector* viewDirection,
        uint8_t* hunterFlag)
    {
        // Keep structured exception handling in this POD-only helper so the
        // temporary stock eligibility byte is restored even if native code
        // exits abnormally. Do not add C++ objects requiring unwinding here.
        const uint8_t savedHunterFlag = *hunterFlag;
        int32_t result = 0;
        __try
        {
            *hunterFlag = 1;
            result = original(
                contextKillComponent,
                interactor,
                viewLocation,
                viewDirection);
        }
        __finally
        {
            *hunterFlag = savedHunterFlag;
        }
        return result;
    }

    int32_t __fastcall ContextKillCanInteractHook(
        UObject* contextKillComponent,
        AActor* interactor,
        const FVector* viewLocation,
        const FVector* viewDirection)
    {
        if (!g_OriginalContextKillCanInteract)
            return 0;

        // SCContextKillComponent::CanInteractWith has one stock Hunter byte
        // gate at counselor +0x1A18 after its ordinary disabled, weapon and
        // context checks. Relax only that one predicate, only for the exact
        // validated JasonDeath component in the active counselor route. The
        // original function still enforces every other stock prerequisite.
        const bool universalFinalContext =
            g_JasonAIState.Active &&
            contextKillComponent &&
            contextKillComponent == g_LastAcceptedFinalKillComponent &&
            g_LastAcceptedFinalContext &&
            interactor &&
            Memory::IsReadable(interactor, 0x1A19) &&
            ObjectClassDerivesFromExact(
                reinterpret_cast<UObject*>(interactor),
                "SCCounselorCharacter");
        if (!universalFinalContext)
        {
            return g_OriginalContextKillCanInteract(
                contextKillComponent,
                interactor,
                viewLocation,
                viewDirection);
        }

        uint8_t* hunterFlag = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(interactor) + 0x1A18);
        const int32_t result = CallContextKillEligibilityWithTemporaryHunter(
            g_OriginalContextKillCanInteract,
            contextKillComponent,
            interactor,
            viewLocation,
            viewDirection,
            hunterFlag);

        if (result != 0 &&
            interactor != g_LastUniversalFinalEligibilityFinisher)
        {
            g_LastUniversalFinalEligibilityFinisher = interactor;
            Logger::Success(
                "18L-BD universal final-kill eligibility accepted by stock context | finisher=" +
                JasonAISafeName(reinterpret_cast<UObject*>(interactor)));
        }
        return result;
    }

    bool InstallUniversalFinalKillEligibilityHook()
    {
        if (g_ContextKillCanInteractHookInstalled)
            return true;

        HMODULE module = GetModuleHandle(nullptr);
        if (!module)
            return false;

        constexpr uintptr_t RVA_ContextKillCanInteract = 0x003457B0;
        uint8_t* target = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(module) +
            RVA_ContextKillCanInteract);
        const uint8_t expected[] = {
            0x40, 0x55, 0x56, 0x48, 0x83, 0xEC, 0x28, 0x80,
            0xB9, 0xE9, 0x03, 0x00, 0x00, 0x00, 0x48, 0x8B,
            0xF2, 0x48, 0x8B, 0xE9, 0x0F, 0x85, 0xA5, 0x01
        };
        if (!Memory::IsReadable(target, sizeof(expected)) ||
            std::memcmp(target, expected, sizeof(expected)) != 0)
        {
            Logger::Error(
                "18L-BD universal final-kill eligibility signature mismatch; hook not installed");
            return false;
        }

        MH_STATUS initStatus = MH_Initialize();
        if (initStatus != MH_OK &&
            initStatus != MH_ERROR_ALREADY_INITIALIZED)
        {
            return false;
        }

        const MH_STATUS createStatus = MH_CreateHook(
            target,
            reinterpret_cast<LPVOID>(&ContextKillCanInteractHook),
            reinterpret_cast<LPVOID*>(
                &g_OriginalContextKillCanInteract));
        if (createStatus != MH_OK)
        {
            Logger::Error(
                "18L-BD failed to create universal final-kill eligibility hook");
            return false;
        }
        if (MH_EnableHook(target) != MH_OK)
        {
            MH_RemoveHook(target);
            g_OriginalContextKillCanInteract = nullptr;
            return false;
        }

        g_ContextKillCanInteractTarget = target;
        g_ContextKillCanInteractHookInstalled = true;
        Logger::Success(
            "18L-BD universal proximity final-kill eligibility hook installed; stock weapon/context gates preserved");
        return true;
    }

    void RemoveUniversalFinalKillEligibilityHook()
    {
        if (!g_ContextKillCanInteractHookInstalled ||
            !g_ContextKillCanInteractTarget)
        {
            return;
        }
        MH_DisableHook(g_ContextKillCanInteractTarget);
        MH_RemoveHook(g_ContextKillCanInteractTarget);
        g_ContextKillCanInteractTarget = nullptr;
        g_OriginalContextKillCanInteract = nullptr;
        g_ContextKillCanInteractHookInstalled = false;
        g_LastUniversalFinalEligibilityFinisher = nullptr;
    }

    int32_t __fastcall PamelaSweaterCanInteractHook(
        AActor* sweater,
        AActor* interactor,
        const FVector* viewLocation,
        const FVector* viewDirection)
    {
        if (!g_OriginalPamelaSweaterCanInteract)
            return 0;

        // ASCPamelaSweater::CanInteractWith normally rejects counselors whose
        // native female byte is false before delegating to the ordinary item
        // predicate. In offline counselor mode, skip only that gender test and
        // call the same stock base pickup predicate. Inventory capacity,
        // disabled/busy state, distance and ownership validation stay intact.
        const bool universalSweaterPickup =
            g_JasonAIState.Active &&
            sweater && interactor &&
            Memory::IsReadable(sweater, sizeof(UObject)) &&
            Memory::IsReadable(interactor, sizeof(UObject)) &&
            ObjectClassDerivesFromExact(
                reinterpret_cast<UObject*>(sweater),
                "SCPamelaSweater") &&
            ObjectClassDerivesFromExact(
                reinterpret_cast<UObject*>(interactor),
                "SCCounselorCharacter") &&
            g_BasePamelaPickupCanInteract;
        if (universalSweaterPickup)
        {
            return g_BasePamelaPickupCanInteract(
                sweater,
                interactor,
                viewLocation,
                viewDirection);
        }
        return g_OriginalPamelaSweaterCanInteract(
            sweater,
            interactor,
            viewLocation,
            viewDirection);
    }

    bool InstallUniversalPamelaSweaterPickupHook()
    {
        if (g_PamelaSweaterCanInteractHookInstalled)
            return true;

        HMODULE module = GetModuleHandle(nullptr);
        if (!module)
            return false;
        constexpr uintptr_t RVA_PamelaSweaterCanInteract = 0x00382720;
        constexpr uintptr_t RVA_BasePamelaPickupCanInteract = 0x00382470;
        uint8_t* target = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(module) +
            RVA_PamelaSweaterCanInteract);
        const uint8_t expected[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
            0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
            0x48, 0x83, 0xEC, 0x20, 0x49, 0x8B, 0xF9, 0x49
        };
        g_BasePamelaPickupCanInteract =
            reinterpret_cast<PamelaSweaterCanInteractFn>(
                reinterpret_cast<uintptr_t>(module) +
                RVA_BasePamelaPickupCanInteract);
        if (!Memory::IsReadable(target, sizeof(expected)) ||
            std::memcmp(target, expected, sizeof(expected)) != 0 ||
            !Memory::IsReadable(
                reinterpret_cast<void*>(g_BasePamelaPickupCanInteract),
                1))
        {
            g_BasePamelaPickupCanInteract = nullptr;
            Logger::Error(
                "18L-BD universal Pamela sweater pickup signature mismatch; hook not installed");
            return false;
        }

        MH_STATUS initStatus = MH_Initialize();
        if (initStatus != MH_OK &&
            initStatus != MH_ERROR_ALREADY_INITIALIZED)
        {
            return false;
        }
        const MH_STATUS createStatus = MH_CreateHook(
            target,
            reinterpret_cast<LPVOID>(&PamelaSweaterCanInteractHook),
            reinterpret_cast<LPVOID*>(
                &g_OriginalPamelaSweaterCanInteract));
        if (createStatus != MH_OK)
        {
            g_BasePamelaPickupCanInteract = nullptr;
            Logger::Error(
                "18L-BD failed to create universal Pamela sweater pickup hook");
            return false;
        }
        if (MH_EnableHook(target) != MH_OK)
        {
            MH_RemoveHook(target);
            g_OriginalPamelaSweaterCanInteract = nullptr;
            g_BasePamelaPickupCanInteract = nullptr;
            return false;
        }

        g_PamelaSweaterCanInteractTarget = target;
        g_PamelaSweaterCanInteractHookInstalled = true;
        Logger::Success(
            "18L-BD universal Pamela sweater pickup hook installed; stock item gates preserved");
        return true;
    }

    void RemoveUniversalPamelaSweaterPickupHook()
    {
        if (!g_PamelaSweaterCanInteractHookInstalled ||
            !g_PamelaSweaterCanInteractTarget)
        {
            return;
        }
        MH_DisableHook(g_PamelaSweaterCanInteractTarget);
        MH_RemoveHook(g_PamelaSweaterCanInteractTarget);
        g_PamelaSweaterCanInteractTarget = nullptr;
        g_OriginalPamelaSweaterCanInteract = nullptr;
        g_BasePamelaPickupCanInteract = nullptr;
        g_PamelaSweaterCanInteractHookInstalled = false;
    }

    void __fastcall UpdateSoundBlipsHook(
        UObject* killer,
        float deltaSeconds)
    {
        if (IsCounselorRouteSoundBlipEmitter(killer))
            return;

        if (g_OriginalUpdateSoundBlips)
            g_OriginalUpdateSoundBlips(killer, deltaSeconds);
    }

    bool InstallCounselorRouteSoundBlipSuppression()
    {
        if (g_UpdateSoundBlipsHookInstalled)
            return true;

        HMODULE module = GetModuleHandle(nullptr);
        if (!module)
            return false;

        constexpr uintptr_t RVA_UpdateSoundBlips = 0x0042DC00;
        uint8_t* target = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(module) + RVA_UpdateSoundBlips);
        const uint8_t expected[] = {
            0x4C, 0x8B, 0xDC, 0x55, 0x49, 0x8D, 0xAB, 0xE8,
            0xFE, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x10, 0x02,
            0x00
        };

        if (!Memory::IsReadable(target, sizeof(expected)) ||
            std::memcmp(target, expected, sizeof(expected)) != 0)
        {
            Logger::Error(
                "18L-AG counselor bridge: UpdateSoundBlips signature mismatch; suppression not installed");
            return false;
        }

        MH_STATUS initStatus = MH_Initialize();
        if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
            return false;

        MH_STATUS createStatus = MH_CreateHook(
            target,
            reinterpret_cast<LPVOID>(&UpdateSoundBlipsHook),
            reinterpret_cast<LPVOID*>(&g_OriginalUpdateSoundBlips));
        if (createStatus != MH_OK)
        {
            Logger::Error(
                "18L-AG counselor bridge: failed to create UpdateSoundBlips hook");
            return false;
        }

        if (MH_EnableHook(target) != MH_OK)
        {
            MH_RemoveHook(target);
            g_OriginalUpdateSoundBlips = nullptr;
            return false;
        }

        g_UpdateSoundBlipsTarget = target;
        g_UpdateSoundBlipsHookInstalled = true;
        Logger::Success(
            "18L-AG counselor bridge: AI-Jason sound-blip emitter suppressed route-locally");
        return true;
    }

    void RemoveCounselorRouteSoundBlipSuppression()
    {
        if (!g_UpdateSoundBlipsHookInstalled || !g_UpdateSoundBlipsTarget)
            return;

        MH_DisableHook(g_UpdateSoundBlipsTarget);
        MH_RemoveHook(g_UpdateSoundBlipsTarget);
        g_UpdateSoundBlipsTarget = nullptr;
        g_OriginalUpdateSoundBlips = nullptr;
        g_UpdateSoundBlipsHookInstalled = false;
    }

    bool IsLiveCounselorRouteVictim(AActor* victim)
    {
        if (!victim ||
            !Memory::IsReadable(victim, sizeof(UObject)) ||
            !IsCounselorOrHero(victim))
        {
            return false;
        }

        uint8_t* dead = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(victim) + 0x1031);
        UObject** controller = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(victim) + 0x3A0);
        UWorld* world = Engine::GetWorld();

        // This is called from the trap overlap callback itself. The callback,
        // valid controller and current-world equality already prove liveness;
        // a complete world actor scan here only adds an event-time hitch.
        return Memory::IsReadable(dead, 1) &&
            *dead == 0 &&
            !HasCounselorEscaped(victim) &&
            Memory::IsReadable(controller, sizeof(UObject*)) &&
            *controller &&
            Memory::IsReadable(*controller, sizeof(UObject)) &&
            world &&
            world == g_JasonAIState.World;
    }

    void __fastcall TrapTriggeredHook(AActor* trap, AActor* victim)
    {
        if (g_OriginalTrapTriggered)
            g_OriginalTrapTriggered(trap, victim);

        if (!g_JasonAIState.Active ||
            !g_JasonAIState.Jason ||
            !trap ||
            !victim ||
            !Memory::IsReadable(trap, sizeof(UObject)) ||
            !JasonAIObjectDerivesFromNameContaining(
                reinterpret_cast<UObject*>(trap),
                "SCTrap") ||
            !IsLiveCounselorRouteVictim(victim))
        {
            return;
        }

        uint8_t* localRole = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(trap) + 0x110);
        AActor* trapArmer = ReadActorField(
            reinterpret_cast<UObject*>(trap),
            0x670);
        AActor* triggeredTrap = ReadActorField(
            reinterpret_cast<UObject*>(victim),
            0x10F8);

        if (!Memory::IsReadable(localRole, 1) ||
            *localRole != 3 ||
            trapArmer != g_JasonAIState.Jason ||
            triggeredTrap != trap)
        {
            return;
        }

        // The overlap callback can run inside trap damage/stun processing.
        // Queue only; the controller Tick performs movement/Morph work safely.
        g_QueuedTrapMorphBaseline.store(
            g_JasonAIState.LastMorphTeleportAt,
            std::memory_order_release);
        g_QueuedTriggeredTrap.store(trap, std::memory_order_release);
        g_QueuedTrapVictim.store(victim, std::memory_order_release);
        Logger::Success(
            "18L-AG counselor bridge: Jason trap triggered; victim queued=" +
            JasonAISafeName(reinterpret_cast<UObject*>(victim)) +
            " | trap=" +
            JasonAISafeName(reinterpret_cast<UObject*>(trap)));
    }

    bool InstallCounselorRouteTrapTriggerHook()
    {
        if (g_TrapTriggeredHookInstalled)
            return true;

        HMODULE module = GetModuleHandle(nullptr);
        if (!module)
            return false;

        constexpr uintptr_t RVA_TrapTriggered = 0x003AC360;
        uint8_t* target = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(module) + RVA_TrapTriggered);
        const uint8_t expected[] = {
            0x48, 0x85, 0xD2, 0x0F, 0x84, 0xEB, 0x02, 0x00,
            0x00, 0x48, 0x8B, 0xC4, 0x55, 0x56, 0x41, 0x56,
            0x48, 0x8D, 0x68, 0xA1
        };

        if (!Memory::IsReadable(target, sizeof(expected)) ||
            std::memcmp(target, expected, sizeof(expected)) != 0)
        {
            Logger::Error(
                "18L-AG counselor bridge: trap-trigger signature mismatch; priority hook not installed");
            return false;
        }

        MH_STATUS initStatus = MH_Initialize();
        if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
            return false;

        MH_STATUS createStatus = MH_CreateHook(
            target,
            reinterpret_cast<LPVOID>(&TrapTriggeredHook),
            reinterpret_cast<LPVOID*>(&g_OriginalTrapTriggered));
        if (createStatus != MH_OK)
        {
            Logger::Error(
                "18L-AG counselor bridge: failed to create trap-trigger priority hook");
            return false;
        }

        if (MH_EnableHook(target) != MH_OK)
        {
            MH_RemoveHook(target);
            g_OriginalTrapTriggered = nullptr;
            return false;
        }

        g_TrapTriggeredTarget = target;
        g_TrapTriggeredHookInstalled = true;
        Logger::Success(
            "18L-AG counselor bridge: owned-trap trigger priority hook installed");
        return true;
    }

    void RemoveCounselorRouteTrapTriggerHook()
    {
        if (!g_TrapTriggeredHookInstalled || !g_TrapTriggeredTarget)
            return;

        MH_DisableHook(g_TrapTriggeredTarget);
        MH_RemoveHook(g_TrapTriggeredTarget);
        g_TrapTriggeredTarget = nullptr;
        g_OriginalTrapTriggered = nullptr;
        g_TrapTriggeredHookInstalled = false;
    }

    void __fastcall CounselorRouteGiveStartingItemHook(
        AActor* pawn,
        UClass* requestedClass)
    {
        static thread_local int32_t callDepth = 0;
        UClass* effectiveClass = requestedClass;

        if (callDepth == 0 &&
            g_HunterAxeLoadoutRouteEnabled &&
            g_JasonAIState.Active &&
            pawn && requestedClass && g_HunterSpawnAxeClass &&
            Memory::IsReadable(pawn, sizeof(UObject)) &&
            Memory::IsReadable(requestedClass, sizeof(UClass)) &&
            Memory::IsReadable(g_HunterSpawnAxeClass, sizeof(UClass)) &&
            pawn != g_LocalCounselorTarget &&
            pawn != g_JasonAIState.Jason &&
            ObjectClassDerivesFromExact(
                reinterpret_cast<UObject*>(pawn),
                "Hunter_Counselor_C") &&
            JasonAISafeName(reinterpret_cast<UObject*>(requestedClass)) ==
                "Shotgun_C")
        {
            UWorld* world = Engine::GetWorld();
            UObject** gameModeField = world
                ? reinterpret_cast<UObject**>(
                    reinterpret_cast<uintptr_t>(world) + 0xF0)
                : nullptr;
            UObject* gameMode = gameModeField &&
                Memory::IsReadable(gameModeField, sizeof(UObject*))
                ? *gameModeField
                : nullptr;
            UObject** pendingHunterControllerField = gameMode &&
                Memory::IsReadable(gameMode, 0x908)
                ? reinterpret_cast<UObject**>(
                    reinterpret_cast<uintptr_t>(gameMode) + 0x900)
                : nullptr;
            UObject* pendingHunterController =
                pendingHunterControllerField &&
                Memory::IsReadable(
                    pendingHunterControllerField,
                    sizeof(UObject*))
                ? *pendingHunterControllerField
                : nullptr;

            if (world == g_JasonAIState.World &&
                gameMode &&
                JasonAIObjectDerivesFromNameContaining(
                    gameMode,
                    "SCGameMode") &&
                pendingHunterController &&
                pendingHunterController != g_LocalPlayerController &&
                JasonAIObjectDerivesFromNameContaining(
                    pendingHunterController,
                    "AIController"))
            {
                effectiveClass = g_HunterSpawnAxeClass;
                Logger::Success(
                    "18L-AT AI Tommy native loadout: stock shotgun replaced with permanent-route axe");
            }
        }

        if (!g_OriginalGiveStartingItem)
            return;

        ++callDepth;
        g_OriginalGiveStartingItem(pawn, effectiveClass);
        --callDepth;

        if (effectiveClass == g_HunterSpawnAxeClass && pawn)
        {
            UObject* weapon = GetCounselorCurrentWeapon(pawn);
            if (weapon &&
                ObjectClassDerivesFromExact(
                    weapon,
                    "CounselorTwoHandedAxe_C"))
            {
                g_HunterSpawnAxeItem = reinterpret_cast<AActor*>(weapon);
            }
        }
    }

    bool InstallCounselorRouteHunterAxeLoadoutHook()
    {
        if (g_GiveStartingItemHookInstalled)
        {
            g_HunterAxeLoadoutRouteEnabled = true;
            return true;
        }

        AActor* worldAxe = FindNearestWorldActorByClass(
            "CounselorTwoHandedAxe_C",
            nullptr,
            0.0f);
        UClass* axeClass = worldAxe &&
            Memory::IsReadable(worldAxe, sizeof(UObject))
            ? worldAxe->Class
            : nullptr;
        if (!axeClass ||
            JasonAISafeName(reinterpret_cast<UObject*>(axeClass)) !=
                "CounselorTwoHandedAxe_C")
        {
            Logger::Error(
                "18L-AT AI Tommy native loadout: axe class unavailable; stock shotgun retained");
            return false;
        }

        HMODULE module = GetModuleHandle(nullptr);
        if (!module)
            return false;
        constexpr uintptr_t RVA_GiveStartingItem = 0x002EF980;
        uint8_t* target = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(module) + RVA_GiveStartingItem);
        const uint8_t expected[] = {
            0x48, 0x85, 0xD2, 0x0F, 0x84, 0xDF, 0x00, 0x00,
            0x00, 0x48, 0x89, 0x54, 0x24, 0x10, 0x57, 0x48
        };
        if (!Memory::IsReadable(target, sizeof(expected)) ||
            std::memcmp(target, expected, sizeof(expected)) != 0)
        {
            Logger::Error(
                "18L-AT AI Tommy native loadout: GiveStartingItem signature mismatch; stock shotgun retained");
            return false;
        }

        MH_STATUS initStatus = MH_Initialize();
        if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
            return false;
        MH_STATUS createStatus = MH_CreateHook(
            target,
            reinterpret_cast<LPVOID>(&CounselorRouteGiveStartingItemHook),
            reinterpret_cast<LPVOID*>(&g_OriginalGiveStartingItem));
        if (createStatus != MH_OK)
            return false;
        if (MH_EnableHook(target) != MH_OK)
        {
            MH_RemoveHook(target);
            g_OriginalGiveStartingItem = nullptr;
            return false;
        }

        g_HunterSpawnAxeClass = axeClass;
        g_GiveStartingItemTarget = target;
        g_GiveStartingItemHookInstalled = true;
        g_HunterAxeLoadoutRouteEnabled = true;
        Logger::Success(
            "18L-AT AI Tommy native loadout hook installed: AI-return shotgun will become axe");
        return true;
    }

    void RemoveCounselorRouteHunterAxeLoadoutHook()
    {
        g_HunterAxeLoadoutRouteEnabled = false;
        if (g_GiveStartingItemHookInstalled && g_GiveStartingItemTarget)
        {
            MH_DisableHook(g_GiveStartingItemTarget);
            MH_RemoveHook(g_GiveStartingItemTarget);
        }
        g_GiveStartingItemTarget = nullptr;
        g_OriginalGiveStartingItem = nullptr;
        g_GiveStartingItemHookInstalled = false;
        g_HunterSpawnAxeClass = nullptr;
        g_HunterSpawnAxeItem = nullptr;
    }

    void DisableExistingCounselorRouteSoundBlips(AActor* jason)
    {
        HMODULE module = GetModuleHandle(nullptr);
        if (!module || !jason)
            return;

        constexpr uintptr_t RVA_SetSoundBlipVisibility = 0x00423BD0;
        uint8_t* target = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(module) + RVA_SetSoundBlipVisibility);
        const uint8_t expected[] = {
            0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x81,
            0xEC, 0x90, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x05,
            0xEC, 0xF9, 0xB6, 0x02
        };

        if (!Memory::IsReadable(target, sizeof(expected)) ||
            std::memcmp(target, expected, sizeof(expected)) != 0)
        {
            Logger::Error(
                "18L-AG counselor bridge: SetSoundBlipVisibility signature mismatch");
            return;
        }

        reinterpret_cast<SetSoundBlipVisibilityFn>(target)(
            reinterpret_cast<UObject*>(jason),
            false);
        Logger::Success(
            "18L-AG counselor bridge: existing AI-Jason sound blips cleared");
    }

    UObject* GetJasonInteractionManager(AActor* jason)
    {
        if (!jason || !Memory::IsReadable(jason, sizeof(UObject)))
            return nullptr;

        UObject** manager = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(jason) + 0xE18);
        if (!Memory::IsReadable(manager, sizeof(UObject*)) ||
            !*manager ||
            !Memory::IsReadable(*manager, sizeof(UObject)))
        {
            return nullptr;
        }

        return *manager;
    }

    UObject* GetLockedJasonInteractable(AActor* jason)
    {
        UObject* manager = GetJasonInteractionManager(jason);
        if (!manager)
            return nullptr;

        UObject** locked = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(manager) + 0x230);
        if (!Memory::IsReadable(locked, sizeof(UObject*)) ||
            !*locked ||
            !Memory::IsReadable(*locked, sizeof(UObject)))
        {
            return nullptr;
        }

        return *locked;
    }

    int32_t ReadJasonKnifeCount(AActor* jason)
    {
        if (!jason || !Memory::IsReadable(jason, sizeof(UObject)))
            return -1;

        int32_t* count = reinterpret_cast<int32_t*>(
            reinterpret_cast<uintptr_t>(jason) + 0x15EC);
        if (!Memory::IsReadable(count, sizeof(int32_t)) ||
            *count < 0 ||
            *count > 256)
        {
            return -1;
        }

        return *count;
    }

    bool IsJasonInNativeSpecialMove(AActor* jason)
    {
        if (!jason || !Memory::IsReadable(jason, sizeof(UObject)))
            return false;

        uintptr_t vtable = *reinterpret_cast<uintptr_t*>(jason);
        uintptr_t* slot = reinterpret_cast<uintptr_t*>(vtable + 0x8F8);
        if (!vtable ||
            !Memory::IsReadable(slot, sizeof(uintptr_t)) ||
            !*slot ||
            !Memory::IsReadable(reinterpret_cast<void*>(*slot), 1))
        {
            return false;
        }

        using IsInSpecialMoveFn = bool(__fastcall*)(AActor*);
        return reinterpret_cast<IsInSpecialMoveFn>(*slot)(jason);
    }

    bool IsJasonBridgeCombatBusy(ULONGLONG now)
    {
        AActor* jason = g_JasonAIState.Jason;
        if (!jason || !Memory::IsReadable(jason, sizeof(UObject)))
            return true;

        bool busy =
            g_JasonAIState.KnifeSequenceActive ||
            g_JasonAIState.AttackPressed ||
            g_JasonAIState.GrabKillReadyAt != 0 ||
            g_JasonAIState.DoorBreakActive ||
            g_JasonAIState.StartupTrapSetupActive ||
            g_JasonAIState.StartupTrapAttemptSent ||
            g_JasonAIState.StartupTrapCountConsumed ||
            g_JasonAIState.StartupTrapPhoneApproachActive ||
            GetLockedJasonInteractable(jason) != nullptr ||
            IsJasonInNativeSpecialMove(jason);

        uint8_t* stateE98 = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(jason) + 0xE98);
        uint8_t* stateEE8 = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(jason) + 0xEE8);
        uint8_t* morphActive = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(jason) + 0x1419);
        uint8_t* shiftActive = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(jason) + 0x14E0);

        if ((Memory::IsReadable(stateE98, 1) && *stateE98 != 0) ||
            (Memory::IsReadable(stateEE8, 1) && *stateEE8 != 0) ||
            (Memory::IsReadable(morphActive, 1) && *morphActive != 0) ||
            (Memory::IsReadable(shiftActive, 1) && *shiftActive != 0))
        {
            busy = true;
        }

        HMODULE module = GetModuleHandle(nullptr);
        if (module)
        {
            using IsStunnedFn = bool(__fastcall*)(AActor*);
            using GetGrabbedCounselorFn = AActor*(__fastcall*)(AActor*);
            IsStunnedFn isStunned = reinterpret_cast<IsStunnedFn>(
                reinterpret_cast<uintptr_t>(module) + 0x002F00E0);
            GetGrabbedCounselorFn getGrabbed =
                reinterpret_cast<GetGrabbedCounselorFn>(
                    reinterpret_cast<uintptr_t>(module) + 0x004055E0);

            if ((Memory::IsReadable(reinterpret_cast<void*>(isStunned), 1) &&
                 isStunned(jason)) ||
                (Memory::IsReadable(reinterpret_cast<void*>(getGrabbed), 1) &&
                 getGrabbed(jason) != nullptr))
            {
                busy = true;
            }
        }

        AActor* nearest = FindNearestJasonAICounselorTarget(jason);
        FVector jasonLocation{};
        FVector counselorLocation{};
        if (nearest &&
            GetJasonAIActorLocation(jason, jasonLocation) &&
            GetJasonAIActorLocation(nearest, counselorLocation))
        {
            const float dx = counselorLocation.X - jasonLocation.X;
            const float dy = counselorLocation.Y - jasonLocation.Y;
            const float dz = counselorLocation.Z - jasonLocation.Z;
            const float distanceSquared = dx * dx + dy * dy + dz * dz;
            if (std::isfinite(distanceSquared) &&
                distanceSquared <= 300.0f * 300.0f)
            {
                busy = true;
            }
        }

        if (busy)
            g_CombatBusyUntil = now + 1000;

        return busy || now < g_CombatBusyUntil;
    }

    bool IsUsablePriorityVictim(AActor* victim)
    {
        if (!victim ||
            !Memory::IsReadable(victim, sizeof(UObject)) ||
            !IsCounselorOrHero(victim))
        {
            return false;
        }

        uint8_t* dead = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(victim) + 0x1031);
        UObject** controller = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(victim) + 0x3A0);
        return Memory::IsReadable(dead, 1) &&
            *dead == 0 &&
            Memory::IsReadable(controller, sizeof(UObject*)) &&
            *controller &&
            Memory::IsReadable(*controller, sizeof(UObject));
    }

    bool TeleportJasonToPriorityVictim(AActor* victim)
    {
        if (!IsUsablePriorityVictim(victim))
            return false;

        AActor* savedTargets[8]{};
        ULONGLONG savedBlockedUntil[8]{};
        uint8_t savedBlockStrikes[8]{};
        std::memcpy(savedTargets, g_JasonAITargets, sizeof(savedTargets));
        std::memcpy(
            savedBlockedUntil,
            g_JasonAITargetBlockedUntil,
            sizeof(savedBlockedUntil));
        std::memcpy(
            savedBlockStrikes,
            g_JasonAITargetBlockStrikes,
            sizeof(savedBlockStrikes));
        const int32_t savedTargetCount = g_JasonAITargetCount;

        g_JasonAITargets[0] = victim;
        g_JasonAITargetBlockedUntil[0] = 0;
        g_JasonAITargetBlockStrikes[0] = 0;
        g_JasonAITargetCount = 1;

        const ULONGLONG previousMorphAt = g_JasonAIState.LastMorphTeleportAt;
        bool teleported = false;
        // A triggered owned trap is the one counselor-targeted Morph that is
        // always urgent, even inside the ordinary 10m walk radius.  Scope the
        // exemption across both branches because Finish... clears the startup
        // flag before issuing its counselor-ring teleport.
        ++g_TrapTeleportExemptionDepth;
        if (g_JasonAIState.StartupTrapSetupActive)
        {
            FinishJasonAIStartupTrapSetupOnGameThread(
                "trap-triggered-priority");
            teleported =
                g_JasonAIState.LastMorphTeleportAt != previousMorphAt;
        }
        else
        {
            teleported = RunOfflineBotsInitialTeleportOnGameThread();
            if (teleported)
                MarkJasonAIMorphTeleportUsed("TrapTriggered");
        }
        --g_TrapTeleportExemptionDepth;

        std::memcpy(g_JasonAITargets, savedTargets, sizeof(savedTargets));
        std::memcpy(
            g_JasonAITargetBlockedUntil,
            savedBlockedUntil,
            sizeof(savedBlockedUntil));
        std::memcpy(
            g_JasonAITargetBlockStrikes,
            savedBlockStrikes,
            sizeof(savedBlockStrikes));
        g_JasonAITargetCount = savedTargetCount;

        if (teleported)
            g_JasonAIState.Target = victim;
        return teleported;
    }

    void ProcessQueuedTrapPriority(ULONGLONG now)
    {
        AActor* queued = g_QueuedTrapVictim.exchange(
            nullptr,
            std::memory_order_acq_rel);
        if (queued)
        {
            const ULONGLONG queuedMorphBaseline =
                g_QueuedTrapMorphBaseline.exchange(
                    0,
                    std::memory_order_acq_rel);
            g_QueuedTriggeredTrap.store(nullptr, std::memory_order_release);
            if (IsUsablePriorityVictim(queued))
            {
                g_TrapPriorityVictim = queued;
                g_TrapPriorityUntil = now + 60000;
                g_TrapPriorityBaselineMorphAt = queuedMorphBaseline;
                g_TrapPriorityMorphCompleted = false;
                g_NextTrapPriorityAttemptAt = now;
                g_NextTrapPriorityLogAt = 0;
            }
        }

        if (!g_TrapPriorityVictim)
            return;

        if (now >= g_TrapPriorityUntil ||
            !IsUsablePriorityVictim(g_TrapPriorityVictim))
        {
            Logger::Debug(
                "18L-AG counselor bridge: trap-trigger victim priority ended");
            g_TrapPriorityVictim = nullptr;
            g_TrapPriorityUntil = 0;
            g_TrapPriorityBaselineMorphAt = 0;
            g_TrapPriorityMorphCompleted = false;
            return;
        }

        // The frozen distance-Morph path runs later in this same controller
        // Tick with its target registry restricted to the trapped counselor.
        // Count that shared-cooldown Morph as the emergency response so the
        // bridge does not fire a redundant second Morph twenty seconds later.
        if (!g_TrapPriorityMorphCompleted &&
            g_JasonAIState.LastMorphTeleportAt != 0 &&
            g_JasonAIState.LastMorphTeleportAt !=
                g_TrapPriorityBaselineMorphAt)
        {
            g_TrapPriorityBaselineMorphAt =
                g_JasonAIState.LastMorphTeleportAt;
            g_TrapPriorityMorphCompleted = true;
            Logger::Success(
                "18L-AG counselor bridge: frozen Morph satisfied trap-trigger priority | counselor=" +
                JasonAISafeName(
                    reinterpret_cast<UObject*>(g_TrapPriorityVictim)));
        }

        if (g_TrapPriorityMorphCompleted)
            return;

        if (now < g_NextTrapPriorityAttemptAt ||
            IsJasonBridgeCombatBusy(now))
        {
            return;
        }

        const ULONGLONG morphRemaining = JasonAIMorphRemainingMs(now);
        if (morphRemaining > 0)
        {
            if (now >= g_NextTrapPriorityLogAt)
            {
                g_NextTrapPriorityLogAt = now + 2000;
                Logger::Debug(
                    "18L-AG counselor bridge: trap-trigger Morph waiting for shared cooldown | remainingMs=" +
                    std::to_string(morphRemaining));
            }
            return;
        }

        StopJasonAIMovementForKnifeOnGameThread();
        if (TeleportJasonToPriorityVictim(g_TrapPriorityVictim))
        {
            Logger::Success(
                "18L-AG counselor bridge: emergency Morph completed to trapped counselor=" +
                JasonAISafeName(
                    reinterpret_cast<UObject*>(g_TrapPriorityVictim)) +
                " | pursuitPriorityMs=60000");
            g_TrapPriorityMorphCompleted = true;
            g_NextTrapPriorityAttemptAt = now + 3000;
        }
        else
        {
            Logger::Debug(
                "18L-AG counselor bridge: emergency Morph had no valid exterior point; retrying");
            g_NextTrapPriorityAttemptAt = now + 3000;
        }
    }

    void ShortenConfirmedTrapPlacementHold(ULONGLONG now)
    {
        if (!g_JasonAIState.StartupTrapSetupActive ||
            !g_JasonAIState.StartupTrapCountConsumed ||
            !g_JasonAIState.StartupTrapActorConfirmed ||
            g_JasonAIState.StartupTrapPlacedAt == 0)
        {
            return;
        }

        const int32_t objectiveIndex =
            g_JasonAIState.StartupTrapObjectiveIndex;
        if (objectiveIndex == g_LastShortenedPlacementObjective)
            return;

        // The frozen implementation deliberately emulates a human's five
        // second placement pause.  Counselor-mode Jason only needs enough
        // time for the stock interaction to settle after the trap actor is
        // visible; then the frozen state machine can advance and issue its
        // walking MoveTo toward the next phone/car objective.
        constexpr ULONGLONG CounselorPlacementSettleMs = 1000;
        constexpr ULONGLONG FrozenPlacementHoldMs = 5000;
        if (now <
            g_JasonAIState.StartupTrapPlacedAt +
                CounselorPlacementSettleMs)
        {
            return;
        }

        const ULONGLONG observedHoldMs =
            now - g_JasonAIState.StartupTrapPlacedAt;
        g_JasonAIState.StartupTrapPlacedAt =
            now - FrozenPlacementHoldMs;
        g_LastShortenedPlacementObjective = objectiveIndex;
        Logger::Success(
            "18L-AG counselor bridge: confirmed trap released into next-objective walk | objectiveIndex=" +
            std::to_string(objectiveIndex) +
            " | observedHoldMs=" + std::to_string(observedHoldMs));
    }

    void UpdatePendingKnifePickup(ULONGLONG now)
    {
        if (!g_PendingKnifePickup)
            return;

        const int32_t currentCount = ReadJasonKnifeCount(g_JasonAIState.Jason);
        uint8_t enabled = 1;
        if (g_PendingKnifeComponent &&
            Memory::IsReadable(
                reinterpret_cast<uint8_t*>(g_PendingKnifeComponent) + 0x2A0,
                1))
        {
            enabled = *(reinterpret_cast<uint8_t*>(
                g_PendingKnifeComponent) + 0x2A0);
        }

        const bool inventoryIncreased =
            currentCount >= 0 &&
            g_KnifeCountBeforePickup >= 0 &&
            currentCount > g_KnifeCountBeforePickup;
        const bool managerReleased =
            GetLockedJasonInteractable(g_JasonAIState.Jason) !=
                g_PendingKnifeComponent;

        if (managerReleased && (inventoryIncreased || enabled == 0))
        {
            Logger::Success(
                "18L-AG counselor bridge: stock throwing-knife pickup completed | count=" +
                std::to_string(g_KnifeCountBeforePickup) + "->" +
                std::to_string(currentCount));
            g_PendingKnifePickup = nullptr;
            g_PendingKnifeComponent = nullptr;
            g_KnifeCountBeforePickup = -1;
            g_KnifePickupStartedAt = 0;
        }
        else if (managerReleased && now >= g_KnifePickupStartedAt + 500)
        {
            Logger::Debug(
                "18L-AG counselor bridge: stock throwing-knife pickup aborted");
            g_PendingKnifePickup = nullptr;
            g_PendingKnifeComponent = nullptr;
            g_KnifeCountBeforePickup = -1;
            g_KnifePickupStartedAt = 0;
        }
        else if (now >= g_KnifePickupStartedAt + 8000)
        {
            Logger::Debug(
                "18L-AG counselor bridge: stock throwing-knife pickup timed out");
            g_PendingKnifePickup = nullptr;
            g_PendingKnifeComponent = nullptr;
            g_KnifeCountBeforePickup = -1;
            g_KnifePickupStartedAt = 0;
        }
    }

    void RefreshKnifePickupRegistryIncremental(UWorld* world, ULONGLONG now)
    {
        if (!world ||
            !Memory::IsReadable(world, sizeof(UWorld)) ||
            g_WorldInteractionRegistryComplete ||
            now < g_NextKnifeRegistryRefreshAt)
        {
            return;
        }
        g_NextKnifeRegistryRefreshAt = now + 211;

        TArray<ULevel*>* levels = reinterpret_cast<TArray<ULevel*>*>(
            reinterpret_cast<uintptr_t>(world) + 0x110);
        if (!Memory::IsReadable(levels, sizeof(TArray<ULevel*>)) ||
            !levels->Data ||
            levels->Count <= 0 ||
            levels->Count > 1024 ||
            !Memory::IsReadable(
                levels->Data,
                sizeof(ULevel*) * static_cast<size_t>(levels->Count)))
        {
            return;
        }

        if (g_KnifeRegistryRefreshLevel >= levels->Count)
        {
            g_WorldInteractionRegistryComplete = true;
            g_KnifeRegistryRefreshActor = 0;
            Logger::Success(
                "18L-AS bounded interaction registry complete | knives=" +
                std::to_string(g_KnifePickupRegistryCount) +
                " | hidingSpots=" +
                std::to_string(g_HidingSpotRegistryCount));
            return;
        }
        ULevel* level = levels->Data[g_KnifeRegistryRefreshLevel];
        if (!level || !Memory::IsReadable(level, sizeof(ULevel)))
        {
            ++g_KnifeRegistryRefreshLevel;
            g_KnifeRegistryRefreshActor = 0;
            return;
        }

        TArray<AActor*>& actors = level->Actors;
        if (!actors.Data ||
            actors.Count <= 0 ||
            actors.Count > 100000 ||
            !Memory::IsReadable(
                actors.Data,
                sizeof(AActor*) * static_cast<size_t>(actors.Count)))
        {
            ++g_KnifeRegistryRefreshLevel;
            g_KnifeRegistryRefreshActor = 0;
            return;
        }

        if (g_KnifeRegistryRefreshActor >= actors.Count)
        {
            ++g_KnifeRegistryRefreshLevel;
            g_KnifeRegistryRefreshActor = 0;
            return;
        }

        // Never ancestry-walk a whole streamed level on one presentation
        // frame. Eight actor slots is a fixed, sub-frame unit of work;
        // the cursor resumes on later ticks until all levels are covered.
        const int32_t actorEnd = (std::min)(
            actors.Count,
            g_KnifeRegistryRefreshActor + 8);
        for (int32_t i = g_KnifeRegistryRefreshActor;
            i < actorEnd;
            ++i)
        {
            AActor* actor = actors.Data[i];
            if (!actor || !Memory::IsReadable(actor, sizeof(UObject)))
                continue;

            if (g_KnifePickupRegistryCount < 128 &&
                JasonAIObjectDerivesFromNameContaining(
                    reinterpret_cast<UObject*>(actor),
                    "SCThrowingKnifePickup"))
            {
                bool duplicate = false;
                for (int32_t k = 0; k < g_KnifePickupRegistryCount; ++k)
                {
                    if (g_KnifePickupRegistry[k] == actor)
                    {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate)
                    g_KnifePickupRegistry[g_KnifePickupRegistryCount++] = actor;
            }

            if (g_HidingSpotRegistryCount < 256 &&
                ObjectClassDerivesFromExact(
                    reinterpret_cast<UObject*>(actor),
                    "SCHidingSpot"))
            {
                g_HidingSpotRegistry[g_HidingSpotRegistryCount++] = actor;
            }
        }
        g_KnifeRegistryRefreshActor = actorEnd;
        if (g_KnifeRegistryRefreshActor >= actors.Count)
        {
            ++g_KnifeRegistryRefreshLevel;
            g_KnifeRegistryRefreshActor = 0;
        }
    }

    void TryPickupNearbyThrowingKnife(ULONGLONG now)
    {
        UpdatePendingKnifePickup(now);
        const bool startupInteractionBusy =
            g_JasonAIState.StartupTrapSetupActive &&
            !g_JasonAIState.StartupTrapTransitActive;
        if (g_PendingKnifePickup ||
            startupInteractionBusy ||
            g_TrapPriorityVictim ||
            IsJasonBridgeCombatBusy(now))
        {
            return;
        }

        // Do not stack two actor-array maintenance jobs on the same startup
        // frames. Loot cleanup finishes its bounded slices first; only then
        // does the knife/hiding registry consume background slices.
        if (g_LootCleanupComplete)
            RefreshKnifePickupRegistryIncremental(g_JasonAIState.World, now);

        if (now < g_NextKnifePickupScanAt)
            return;

        g_NextKnifePickupScanAt = now + 1700;
        AActor* jason = g_JasonAIState.Jason;
        UWorld* world = g_JasonAIState.World;
        UObject* manager = GetJasonInteractionManager(jason);
        if (!jason || !world || !manager || GetLockedJasonInteractable(jason))
            return;

        FVector jasonLocation{};
        if (!GetJasonAIActorLocation(jason, jasonLocation))
            return;

        HMODULE module = GetModuleHandle(nullptr);
        if (!module)
            return;

        constexpr uintptr_t RVA_CanInteractWithKnife = 0x00382920;
        constexpr uintptr_t RVA_AttemptInteract = 0x0025AC50;
        AActor* bestPickup = nullptr;
        UObject* bestComponent = nullptr;
        float bestDistanceSquared = FLT_MAX;

        for (int32_t i = 0; i < g_KnifePickupRegistryCount; ++i)
        {
            AActor* pickup = g_KnifePickupRegistry[i];
            if (!pickup ||
                !Memory::IsReadable(pickup, sizeof(UObject)) ||
                !JasonAIObjectDerivesFromNameContaining(
                    reinterpret_cast<UObject*>(pickup),
                    "SCThrowingKnifePickup"))
            {
                continue;
            }

            UObject** componentField = reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(pickup) + 0x4F8);
            if (!Memory::IsReadable(componentField, sizeof(UObject*)) ||
                !*componentField ||
                !Memory::IsReadable(*componentField, sizeof(UObject)))
            {
                continue;
            }

            UObject* component = *componentField;
            uint8_t* enabled = reinterpret_cast<uint8_t*>(
                reinterpret_cast<uintptr_t>(component) + 0x2A0);
            float* distanceLimit = reinterpret_cast<float*>(
                reinterpret_cast<uintptr_t>(component) + 0x304);
            if (!Memory::IsReadable(enabled, 1) ||
                *enabled == 0 ||
                !Memory::IsReadable(distanceLimit, sizeof(float)))
            {
                continue;
            }

            FVector pickupLocation{};
            if (!GetJasonAIActorLocation(pickup, pickupLocation))
                continue;

            const float dx = pickupLocation.X - jasonLocation.X;
            const float dy = pickupLocation.Y - jasonLocation.Y;
            const float dz = pickupLocation.Z - jasonLocation.Z;
            const float distanceSquared = dx * dx + dy * dy + dz * dz;
            float allowedDistance =
                std::isfinite(*distanceLimit) && *distanceLimit > 0.0f
                ? (std::min)(200.0f, *distanceLimit)
                : 200.0f;

            if (!std::isfinite(distanceSquared) ||
                distanceSquared > allowedDistance * allowedDistance ||
                distanceSquared >= bestDistanceSquared)
            {
                continue;
            }

            uintptr_t pickupVTable = *reinterpret_cast<uintptr_t*>(pickup);
            uintptr_t* canInteractSlot = reinterpret_cast<uintptr_t*>(
                pickupVTable + 0x648);
            const uintptr_t expectedCanInteract =
                reinterpret_cast<uintptr_t>(module) +
                RVA_CanInteractWithKnife;
            if (!pickupVTable ||
                !Memory::IsReadable(canInteractSlot, sizeof(uintptr_t)) ||
                *canInteractSlot != expectedCanInteract)
            {
                continue;
            }

            using CanInteractWithFn = int32_t(__fastcall*)(
                AActor*, AActor*, const FVector&, const FRotator&);
            const float horizontal = std::sqrt(dx * dx + dy * dy);
            constexpr float RadiansToDegrees = 57.29577951308232f;
            FRotator viewRotation(
                std::atan2(dz, horizontal) * RadiansToDegrees,
                std::atan2(dy, dx) * RadiansToDegrees,
                0.0f);
            int32_t canInteract =
                reinterpret_cast<CanInteractWithFn>(*canInteractSlot)(
                    pickup,
                    jason,
                    jasonLocation,
                    viewRotation);
            if (canInteract == 0)
                continue;

            bestPickup = pickup;
            bestComponent = component;
            bestDistanceSquared = distanceSquared;
        }

        if (!bestPickup || !bestComponent)
            return;

        uintptr_t managerVTable = *reinterpret_cast<uintptr_t*>(manager);
        uintptr_t* attemptSlot = reinterpret_cast<uintptr_t*>(
            managerVTable + 0x430);
        const uintptr_t expectedAttempt =
            reinterpret_cast<uintptr_t>(module) + RVA_AttemptInteract;
        if (!managerVTable ||
            !Memory::IsReadable(attemptSlot, sizeof(uintptr_t)) ||
            *attemptSlot != expectedAttempt)
        {
            return;
        }

        const int32_t countBefore = ReadJasonKnifeCount(jason);
        reinterpret_cast<AttemptInteractFn>(*attemptSlot)(
            manager,
            bestComponent,
            1,
            true);

        UObject* locked = GetLockedJasonInteractable(jason);
        if (locked == bestComponent)
        {
            g_PendingKnifePickup = bestPickup;
            g_PendingKnifeComponent = bestComponent;
            g_KnifeCountBeforePickup = countBefore;
            g_KnifePickupStartedAt = now;
            g_NextKnifePickupScanAt = now + 2500;
            StopJasonAIMovementForKnifeOnGameThread();
            Logger::Success(
                "18L-AG counselor bridge: stock throwing-knife pickup accepted | actor=" +
                JasonAISafeName(reinterpret_cast<UObject*>(bestPickup)) +
                " | countBefore=" + std::to_string(countBefore));
        }
    }

    bool RunBaseAIControllerTick(
        UObject* controller,
        float deltaSeconds)
    {
        HMODULE module = GetModuleHandle(nullptr);
        if (!module || !controller)
            return false;

        constexpr uintptr_t RVA_BaseAIControllerTick = 0x011360F0;
        using BaseTickFn = void(__fastcall*)(UObject*, float);
        BaseTickFn baseTick = reinterpret_cast<BaseTickFn>(
            reinterpret_cast<uintptr_t>(module) + RVA_BaseAIControllerTick);
        if (!Memory::IsReadable(reinterpret_cast<void*>(baseTick), 1))
            return false;

        baseTick(controller, deltaSeconds);
        return true;
    }

    void PrepareNonAggroStartupTick()
    {
        if (!g_JasonAIState.StartupTrapSetupActive ||
            !g_JasonAIState.Jason)
        {
            return;
        }

        UWorld* world = Engine::GetWorld();
        const ULONGLONG now = GetTickCount64();
        LogFusePlacementDiagnostic(now);

        if (!g_JasonAIState.StartupTrapObjectivesReady &&
            now >= g_JasonAIState.StartupTrapNextActionAt)
        {
            ResolveCounselorRouteStartupObjectives(world);
        }

        const ULONGLONG readyAt = JasonAIMorphReadyAt();
        const bool betweenObjectives =
            !g_JasonAIState.StartupTrapTeleported &&
            !g_JasonAIState.StartupTrapAttemptSent &&
            !g_JasonAIState.StartupTrapCountConsumed &&
            !g_JasonAIState.StartupTrapPhoneApproachActive;
        const bool allObjectivesVisited =
            g_JasonAIState.StartupTrapObjectivesReady &&
            g_JasonAIState.StartupTrapObjectiveIndex >=
                g_JasonAIState.StartupTrapObjectiveCount;
        const bool outOfTraps =
            g_JasonAIState.StartupTrapObjectivesReady &&
            ReadJasonAITrapCount(g_JasonAIState.Jason) == 0;
        const bool finalHuntWait =
            betweenObjectives &&
            (allObjectivesVisited || outOfTraps);

        // Preserve the frozen donor-style behavior between real trap targets:
        // walk toward the next phone/car, fight only inside its existing combat
        // windows, and Morph there when the 20-second charge completes.  Hold
        // still only after objective setup is finished, so Jason cannot use the
        // remaining recharge time to begin hunting counselors prematurely.
        if (finalHuntWait && readyAt != 0 && now < readyAt)
        {
            StopJasonAIMovementForKnifeOnGameThread();
            g_JasonAIState.StartupTrapTransitActive = false;
            g_JasonAIState.StartupTrapTransitGoal = nullptr;
            g_JasonAIState.StartupTrapTransitNextMoveAt = 0;
            if (g_JasonAIState.StartupTrapNextActionAt < readyAt)
                g_JasonAIState.StartupTrapNextActionAt = readyAt;

            if (g_LastNonAggroReadyAt != readyAt)
            {
                g_LastNonAggroReadyAt = readyAt;
                Logger::Success(
                    "18L-AG counselor bridge: Jason held non-aggro before final counselor hunt | remainingMs=" +
                    std::to_string(readyAt - now));
            }
        }

        if (!finalHuntWait ||
            readyAt == 0 ||
            now < readyAt)
        {
            return;
        }

        if (allObjectivesVisited || outOfTraps)
        {
            StopJasonAIMovementForKnifeOnGameThread();
            FinishJasonAIStartupTrapSetupOnGameThread(
                allObjectivesVisited
                    ? "all-objectives-visited-bridge-nonaggro"
                    : "out-of-traps-bridge-nonaggro");
        }
    }

    void __fastcall CounselorRouteControllerTickHook(
        UObject* controller,
        float deltaSeconds)
    {
        const bool counselorRouteTick =
            controller == g_JasonAIState.Controller &&
            g_JasonAIState.Active;

        if (!counselorRouteTick)
        {
            OfflineBotsKillerControllerTickHook(controller, deltaSeconds);
            return;
        }

        // The stock game keeps its killer controller ticking while it tears
        // down pawns for WaitingPostMatchOutro/PostMatchOutro.  The counselor
        // bridge must retire before touching cached Jason animation,
        // interaction, knife, trap, or vehicle pointers.  The previous build
        // continued for 46 seconds after InProgress ended and dereferenced a
        // destroyed AnimInstance in native IsStunned.
        const CounselorRouteMatchPhase matchPhase =
            ReadCounselorRouteMatchPhase();
        if (matchPhase == CounselorRouteMatchPhase::NotInProgress)
        {
            SetJasonHighPriorityPursuitBoost(false, "postmatch");
            g_JasonAIState.Active = false;
            if (!g_PostMatchAIRetired)
            {
                g_PostMatchAIRetired = true;
                Logger::Success(
                    "18L-AJ POSTMATCH SAFETY: counselor-route Jason AI retired before gameplay-object teardown");
            }
            OfflineBotsKillerControllerTickHook(controller, deltaSeconds);
            return;
        }

        const ULONGLONG now = GetTickCount64();
        RefreshLocalCounselorTarget(now);
        PruneAndRefreshCounselorTargets(g_JasonAIState.World, now);

        // Native grab/pocket-knife state owns both pawns. Detect it before any
        // helper protection, pickup, movement or blackboard work can mutate
        // Tommy during the paired animation.
        if (DriveHeldCounselorGrabKill(now))
        {
            SetJasonHighPriorityPursuitBoost(false, "grab-kill");
            RunBaseAIControllerTick(controller, deltaSeconds);
            return;
        }

        CleanupCounselorRouteWalkieTalkies(now);
        EnsureTommyRadioObjectivePublished(now);
        DriveCounselorKillTeamAI(now);
        RefreshUnarmedCounselorFleeState(now);

        // Once Jason exposes the native death-context, custom Jason chase and
        // combat must not compete with the paired final-kill interaction.
        if (now < g_KillTeamFinalContextUntil)
        {
            SetJasonHighPriorityPursuitBoost(false, "final-kill");
            RunBaseAIControllerTick(controller, deltaSeconds);
            return;
        }

        // A moving escape car outranks trap response, knife scavenging and
        // ordinary counselor pursuit.  Keep this ahead of those helpers so a
        // stale objective cannot overwrite the road-interception MoveTo.
        const bool vehicleInterceptActive =
            DriveOccupiedVehicleInterception(now);
        const bool movingEscapeCar =
            vehicleInterceptActive &&
            g_VehicleInterceptCar &&
            std::fabs(SafeVehicleForwardSpeed(g_VehicleInterceptCar)) > 5.0f;
        const bool policePursuit =
            !vehicleInterceptActive && HasPoliceArrived();
        const bool highPriorityPursuit =
            movingEscapeCar || policePursuit;
        SetJasonHighPriorityPursuitBoost(
            highPriorityPursuit,
            movingEscapeCar ? "occupied-escape-car" : "police-arrived",
            highPriorityPursuit ? 3.0f : 2.0f,
            highPriorityPursuit ? 5000ULL : 10000ULL);
        if (vehicleInterceptActive)
        {
            RunBaseAIControllerTick(controller, deltaSeconds);
            return;
        }

        // Beds, closets and tents expose a dedicated killer interactable.
        // Dispatch that native action before the frozen close-combat loop can
        // mistake the hidden counselor for a slash/grab target.
        if (DriveHidingSpotInteraction(now))
        {
            SetJasonHighPriorityPursuitBoost(false, "hiding-kill");
            RunBaseAIControllerTick(controller, deltaSeconds);
            return;
        }

        ProcessQueuedTrapPriority(now);
        TryPickupNearbyThrowingKnife(now);
        ExtendTraversalGraceForActiveDoor(now);

        // Stock interaction/special-move code owns Jason until the knife
        // pickup completes or aborts.  Keep the native base pawn/controller
        // tick alive, but do not let the custom chase overwrite its movement.
        if (g_PendingKnifePickup ||
            GetLockedJasonInteractable(g_JasonAIState.Jason))
        {
            ResetStuckSamplingAfterNativeInteraction(now);
            RunBaseAIControllerTick(controller, deltaSeconds);
            return;
        }

        ShortenConfirmedTrapPlacementHold(now);
        PrepareNonAggroStartupTick();

        ApplyHumanPursuitBalance(now);

        // Chase/combat helpers independently choose the nearest registered
        // counselor.  Restrict that choice synchronously while responding to
        // an owned trap, then restore the complete registry after this Tick.
        const bool restrictToTrapVictim =
            g_TrapPriorityVictim &&
            now < g_TrapPriorityUntil &&
            IsUsablePriorityVictim(g_TrapPriorityVictim);
        AActor* savedTargets[8]{};
        ULONGLONG savedBlockedUntil[8]{};
        uint8_t savedBlockStrikes[8]{};
        const int32_t savedTargetCount = g_JasonAITargetCount;

        if (restrictToTrapVictim)
        {
            std::memcpy(savedTargets, g_JasonAITargets, sizeof(savedTargets));
            std::memcpy(
                savedBlockedUntil,
                g_JasonAITargetBlockedUntil,
                sizeof(savedBlockedUntil));
            std::memcpy(
                savedBlockStrikes,
                g_JasonAITargetBlockStrikes,
                sizeof(savedBlockStrikes));
            g_JasonAITargets[0] = g_TrapPriorityVictim;
            g_JasonAITargetBlockedUntil[0] = 0;
            g_JasonAITargetBlockStrikes[0] = 0;
            g_JasonAITargetCount = 1;
            g_JasonAIState.Target = g_TrapPriorityVictim;
        }

        OfflineBotsKillerControllerTickHook(controller, deltaSeconds);

        if (restrictToTrapVictim)
        {
            std::memcpy(g_JasonAITargets, savedTargets, sizeof(savedTargets));
            std::memcpy(
                g_JasonAITargetBlockedUntil,
                savedBlockedUntil,
                sizeof(savedBlockedUntil));
            std::memcpy(
                g_JasonAITargetBlockStrikes,
                savedBlockStrikes,
                sizeof(savedBlockStrikes));
            g_JasonAITargetCount = savedTargetCount;
        }
    }

    bool InstallCounselorRouteTickWrapper()
    {
        if (g_CounselorRouteTickWrapperInstalled)
            return true;

        const uintptr_t frozenHook = reinterpret_cast<uintptr_t>(
            &OfflineBotsKillerControllerTickHook);
        if (!g_KillerControllerTickHookInstalled ||
            !g_KillerControllerTickSlot ||
            !Memory::IsReadable(g_KillerControllerTickSlot, sizeof(uintptr_t)) ||
            *g_KillerControllerTickSlot != frozenHook)
        {
            return false;
        }

        DWORD oldProtection = 0;
        if (!VirtualProtect(
                g_KillerControllerTickSlot,
                sizeof(uintptr_t),
                PAGE_READWRITE,
                &oldProtection))
        {
            return false;
        }

        *g_KillerControllerTickSlot = reinterpret_cast<uintptr_t>(
            &CounselorRouteControllerTickHook);
        DWORD ignoredProtection = 0;
        VirtualProtect(
            g_KillerControllerTickSlot,
            sizeof(uintptr_t),
            oldProtection,
            &ignoredProtection);
        g_CounselorRouteTickWrapperInstalled = true;
        Logger::Success(
            "18L-AG counselor bridge: non-aggro startup Tick wrapper installed");
        return true;
    }

    void RestoreFrozenControllerTickHook()
    {
        if (!g_CounselorRouteTickWrapperInstalled ||
            !g_KillerControllerTickSlot ||
            !Memory::IsReadable(g_KillerControllerTickSlot, sizeof(uintptr_t)))
        {
            g_CounselorRouteTickWrapperInstalled = false;
            return;
        }

        if (*g_KillerControllerTickSlot == reinterpret_cast<uintptr_t>(
                &CounselorRouteControllerTickHook))
        {
            DWORD oldProtection = 0;
            if (VirtualProtect(
                    g_KillerControllerTickSlot,
                    sizeof(uintptr_t),
                    PAGE_READWRITE,
                    &oldProtection))
            {
                *g_KillerControllerTickSlot = reinterpret_cast<uintptr_t>(
                    &OfflineBotsKillerControllerTickHook);
                DWORD ignoredProtection = 0;
                VirtualProtect(
                    g_KillerControllerTickSlot,
                    sizeof(uintptr_t),
                    oldProtection,
                    &ignoredProtection);
            }
        }

        g_CounselorRouteTickWrapperInstalled = false;
    }
}

namespace FrozenJasonBridge
{
    void ResetCounselorModeJason()
    {
        RestoreFrozenControllerTickHook();
        RemoveOfflineBotsControllerTickHook();
        RemoveCounselorRouteSoundBlipSuppression();
        RemoveCounselorRouteTrapTriggerHook();
        RemoveCounselorRouteHunterAxeLoadoutHook();
        RemoveUniversalFinalKillEligibilityHook();
        RemoveUniversalPamelaSweaterPickupHook();
        g_JasonAIState = JasonAIState{};
        g_JasonAICache = JasonAICache{};
        g_JasonRequestPending.store(false);
        g_JasonRequestUsed.store(false);
        g_CounselorRequestPending.store(false);
        g_CounselorBotsSpawned.store(0);
        ResetJasonAITargets();
        ResetLoadedCounselorClassCache();
        g_CounselorClassPreloadRequested = false;
        g_CounselorClassPreloadRequestedAt = 0;
        g_LastNonAggroReadyAt = 0;
        g_NextTrapPriorityAttemptAt = 0;
        g_NextTrapPriorityLogAt = 0;
        g_TrapPriorityUntil = 0;
        g_TrapPriorityBaselineMorphAt = 0;
        g_NextKnifePickupScanAt = 0;
        g_NextKnifeRegistryRefreshAt = 0;
        g_KnifeRegistryRefreshLevel = 0;
        g_KnifeRegistryRefreshActor = 0;
        std::memset(g_KnifePickupRegistry, 0, sizeof(g_KnifePickupRegistry));
        g_KnifePickupRegistryCount = 0;
        std::memset(g_HidingSpotRegistry, 0, sizeof(g_HidingSpotRegistry));
        g_HidingSpotRegistryCount = 0;
        g_WorldInteractionRegistryComplete = false;
        g_KnifePickupStartedAt = 0;
        g_CombatBusyUntil = 0;
        g_QueuedTrapMorphBaseline.store(0, std::memory_order_release);
        g_QueuedTrapVictim.store(nullptr, std::memory_order_release);
        g_QueuedTriggeredTrap.store(nullptr, std::memory_order_release);
        g_TrapPriorityVictim = nullptr;
        g_TrapPriorityMorphCompleted = false;
        g_LastShortenedPlacementObjective = -1;
        g_PendingKnifePickup = nullptr;
        g_PendingKnifeComponent = nullptr;
        g_KnifeCountBeforePickup = -1;
        g_LastHeldCounselor = nullptr;
        g_HeldCounselorObservedAt = 0;
        g_GrabKillSlotsLoggedFor = nullptr;
        g_NextAdapterGrabKillAttemptAt = 0;
        g_NextGrabKillStateLogAt = 0;
        g_NextAdapterGrabKillSlot = 0;
        g_LocalCounselorTarget = nullptr;
        g_LocalPlayerController = nullptr;
        g_NextLocalCounselorRefreshAt = 0;
        g_HumanPursuitStartedAt = 0;
        g_NextCounselorTargetPruneAt = 0;
        g_NextCounselorRosterRefreshAt = 0;
        g_CounselorRosterRefreshLevel = 0;
        std::memset(
            g_CounselorControllers,
            0,
            sizeof(g_CounselorControllers));
        g_CounselorControllerCount = 0;
        g_CounselorConvergenceActive = false;
        g_NextCounselorConvergenceAt = 0;
        g_NextConvergenceCombatSweepAt = 0;
        g_CounselorConvergenceCursor = 0;
        g_CounselorConvergenceMeleeClass = nullptr;
        g_CounselorConvergenceMacheteClass = nullptr;
        std::memset(
            g_ConvergenceArmedCounselors,
            0,
            sizeof(g_ConvergenceArmedCounselors));
        g_ConvergenceArmedCounselorCount = 0;
        std::memset(
            g_ConvergenceTravelCounselors,
            0,
            sizeof(g_ConvergenceTravelCounselors));
        std::memset(g_ConvergenceTravelLastLocations, 0,
            sizeof(g_ConvergenceTravelLastLocations));
        std::memset(g_ConvergenceTravelLastProgressAt, 0,
            sizeof(g_ConvergenceTravelLastProgressAt));
        std::memset(g_ConvergenceTravelRecoveryUntil, 0,
            sizeof(g_ConvergenceTravelRecoveryUntil));
        std::memset(g_ConvergenceTravelHaveLocation, 0,
            sizeof(g_ConvergenceTravelHaveLocation));
        g_ConvergenceTravelCounselorCount = 0;
        ResetVehicleInterceptionState();
        g_IgnoredVehicleInterceptCar = nullptr;
        g_IgnoredVehicleInterceptUntil = 0;
        g_HidingSpotTarget = nullptr;
        g_HidingSpotCounselor = nullptr;
        g_HidingSpotInteractable = nullptr;
        g_IgnoredHidingSpot = nullptr;
        g_IgnoredHidingCounselor = nullptr;
        g_HidingSpotStartedAt = 0;
        g_HidingSpotAttemptPendingUntil = 0;
        g_HidingSpotIgnoreUntil = 0;
        g_NextHidingSpotMoveAt = 0;
        g_HidingSpotAttempts = 0;
        g_HidingSpotRepositionAttempted = false;
        g_NextHidingSpotScanAt = 0;
        g_NextHidingSpotInteractAt = 0;
        g_NextHidingSpotLogAt = 0;
        g_NextWalkieCleanupAt = 0;
        g_WalkieCleanupLevel = 0;
        g_WalkieCleanupActor = 0;
        g_WalkiesRemoved = 0;
        g_TapesRemoved = 0;
        g_InvalidPropellersRemoved = 0;
        g_SurplusKeysRemoved = 0;
        g_UsefulPickupsSpawned = 0;
        g_LootCleanupPassActive = false;
        g_LootCleanupComplete = false;
        g_LootCensusSawBoat = false;
        g_LootCensusCarCount = 0;
        g_LootKeysKeptThisPass = 0;
        g_LootReplacementIndex = 0;
        std::memset(
            g_LootReplacementClasses,
            0,
            sizeof(g_LootReplacementClasses));
        g_NextCounselorFleeRefreshAt = 0;
        g_CounselorFleeRefreshCursor = 0;
        g_CounselorBlackboardNameResolutionComplete = false;
        g_CounselorBlackboardNameResolveCursor = 0;
        g_SCWeaponNameIndex = -1;
        g_ShouldFleeKillerNameIndex = -1;
        g_ShouldFightBackNameIndex = -1;
        g_ShouldArmedFightBackNameIndex = -1;
        g_ShouldMeleeFightBackNameIndex = -1;
        g_SeekWeaponWhileFleeingNameIndex = -1;
        g_ShouldHideNameIndex = -1;
        g_ShouldOrientTowardKillerNameIndex = -1;
        g_JasonCharacterNameIndex = -1;
        g_KillTeamRoute = KillTeamRoute::None;
        g_KillTeamHelper = nullptr;
        g_KillTeamShack = nullptr;
        g_KillTeamSweater = nullptr;
        g_KillTeamAxe = nullptr;
        g_KillTeamMask = nullptr;
        g_NextKillTeamTickAt = 0;
        g_NextKillTeamDiscoveryAt = 0;
        g_NextKillTeamInteractAt = 0;
        g_NextHelperKnifeGrantAt = 0;
        g_NextSweaterUseAt = 0;
        g_NextFinalKillInteractAt = 0;
        g_NextKillTeamMoveAt = 0;
        g_NextKillTeamFollowAt = 0;
        g_KillTeamMoveTarget = nullptr;
        g_TommyJasonObjectiveOwner = nullptr;
        g_NextTommyJasonObjectiveRepairAt = 0;
        g_OrphanJasonStunStartedAt = 0;
        g_KillTeamHelperArmed = false;
        g_KillTeamHelperProtected = false;
        g_KillTeamMaskAcquired = false;
        g_KillTeamSweaterUseDispatched = false;
        g_KillTeamAxeDiscoveryAttempted = false;
        g_KillTeamAxePursuitStartedAt = 0;
        g_KillTeamAxeLastProgressAt = 0;
        g_KillTeamAxeBestDistance = FLT_MAX;
        g_KillTeamHelperNativeBusyUntil = 0;
        g_KillTeamHelperStableObservations = 0;
        g_KillTeamFinalContextUntil = 0;
        g_KillTeamFinalInteractionDispatched = false;
        g_KillTeamFinalInteractionPending = false;
        g_KillTeamFinalInteractionAttempts = 0;
        g_KillTeamFinalInteractionStartedAt = 0;
        g_KillTeamFinalSequenceStartedAt = 0;
        g_KillTeamFinalInteractionCommitted = false;
        g_KillTeamFinalRecoveryCooldownUntil = 0;
        g_KillTeamFinalMoveFailures = 0;
        g_KillTeamFinalRepositionAttempted = false;
        g_KillTeamLastCancelledInteraction = nullptr;
        g_NextKillTeamInteractionCancelAt = 0;
        g_KillTeamPendingFinalContext = nullptr;
        g_KillTeamPendingFinalComponent = nullptr;
        g_LastRejectedFinalContext = nullptr;
        g_LastAcceptedFinalContext = nullptr;
        g_LastAcceptedFinalComponent = nullptr;
        g_LastAcceptedFinalKillComponent = nullptr;
        g_PermanentHumanSweaterCarrier = nullptr;
        g_PermanentHumanSweaterAbility = nullptr;
        g_PermanentHumanInnateActiveAbility = nullptr;
        g_PermanentHumanSweaterLatched = false;
        g_PermanentHumanSweaterRestoreLogged = false;
        g_PermanentHumanUnlimitedSweaterArmed = false;
        g_PermanentHumanSweaterRearmAt = 0;
        g_PermanentHumanSweaterRearmAttempts = 0;
        g_PermanentHumanSweaterUseCount = 0;
        g_RepeatJasonKillStanceAt = 0;
        g_RepeatJasonKillStanceDeadline = 0;
        g_RepeatJasonKillStanceAttempts = 0;
        g_UnlimitedSweaterWorldRuleLogged = false;
        g_NextFinalContextDiagnosticAt = 0;
        g_NextAITommyProtectionAt = 0;
        g_ProtectedAITommy = nullptr;
        g_NextTommyObjectivePublishAt = 0;
        g_TommyObjectiveRadio = nullptr;
        g_TommyObjectivePublished = false;
        g_FuseDiagnosticAt = 0;
        g_FuseDiagnosticLogged = false;
        g_MatchStateOffset = -1;
        g_PostMatchAIRetired = false;
        g_LastExtendedDoorTarget = nullptr;
        g_TrapTeleportExemptionDepth = 0;
        g_MinimumMorphTeleportFunction = nullptr;
        g_NextMinimumMorphLogAt = 0;
        g_NextMorphCooldownRefusalLogAt = 0;
    }

    bool AdoptCounselorModeJason(
        AActor* jason,
        UObject* killerController,
        AActor* localCounselor)
    {
        UWorld* world = Engine::GetWorld();

        if (!world ||
            !jason ||
            !killerController ||
            !localCounselor ||
            !Memory::IsReadable(world, sizeof(UWorld)) ||
            !Memory::IsReadable(jason, sizeof(UObject)) ||
            !Memory::IsReadable(killerController, sizeof(UObject)) ||
            !Memory::IsReadable(localCounselor, sizeof(UObject)))
        {
            Logger::Error(
                "18L-AD frozen bridge: donor-style Jason/controller/counselor state is invalid");
            return false;
        }

        RestoreFrozenControllerTickHook();
        RemoveOfflineBotsControllerTickHook();
        RemoveCounselorRouteHunterAxeLoadoutHook();
        RemoveUniversalFinalKillEligibilityHook();
        RemoveUniversalPamelaSweaterPickupHook();
#ifdef ROB_LITE_SANDBOX
        JasonAICache liteSandboxCache = g_JasonAICache;
#endif
        g_JasonAIState = JasonAIState{};
        g_JasonAICache = JasonAICache{};
#ifdef ROB_LITE_SANDBOX
        g_JasonAICache = liteSandboxCache;
        g_JasonAICache.World = world;
#endif
        ResetJasonAITargets();
        ResetLoadedCounselorClassCache();

        g_JasonRequestPending.store(false);
        g_JasonRequestUsed.store(true);
        g_CounselorRequestPending.store(false);
        g_CounselorBotsSpawned.store(0);
        g_CounselorClassPreloadRequested = false;
        g_CounselorClassPreloadRequestedAt = 0;
        g_LastNonAggroReadyAt = 0;
        g_NextTrapPriorityAttemptAt = 0;
        g_NextTrapPriorityLogAt = 0;
        g_TrapPriorityUntil = 0;
        g_TrapPriorityBaselineMorphAt = 0;
        g_NextKnifePickupScanAt = 0;
        g_NextKnifeRegistryRefreshAt = 0;
        g_KnifeRegistryRefreshLevel = 0;
        g_KnifeRegistryRefreshActor = 0;
        std::memset(g_KnifePickupRegistry, 0, sizeof(g_KnifePickupRegistry));
        g_KnifePickupRegistryCount = 0;
        std::memset(g_HidingSpotRegistry, 0, sizeof(g_HidingSpotRegistry));
        g_HidingSpotRegistryCount = 0;
        g_WorldInteractionRegistryComplete = false;
        g_KnifePickupStartedAt = 0;
        g_CombatBusyUntil = 0;
        g_QueuedTrapMorphBaseline.store(0, std::memory_order_release);
        g_QueuedTrapVictim.store(nullptr, std::memory_order_release);
        g_QueuedTriggeredTrap.store(nullptr, std::memory_order_release);
        g_TrapPriorityVictim = nullptr;
        g_TrapPriorityMorphCompleted = false;
        g_LastShortenedPlacementObjective = -1;
        g_PendingKnifePickup = nullptr;
        g_PendingKnifeComponent = nullptr;
        g_KnifeCountBeforePickup = -1;
        g_LastHeldCounselor = nullptr;
        g_HeldCounselorObservedAt = 0;
        g_GrabKillSlotsLoggedFor = nullptr;
        g_NextAdapterGrabKillAttemptAt = 0;
        g_NextGrabKillStateLogAt = 0;
        g_NextAdapterGrabKillSlot = 0;
        g_LocalCounselorTarget = localCounselor;
        UObject** localControllerField = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(localCounselor) + 0x3A0);
        g_LocalPlayerController =
            Memory::IsReadable(localControllerField, sizeof(UObject*))
                ? *localControllerField
                : nullptr;
        g_NextLocalCounselorRefreshAt = 0;
        g_HumanPursuitStartedAt = 0;
        g_NextCounselorTargetPruneAt = 0;
        g_NextCounselorRosterRefreshAt = 0;
        g_CounselorRosterRefreshLevel = 0;
        std::memset(
            g_CounselorControllers,
            0,
            sizeof(g_CounselorControllers));
        g_CounselorControllerCount = 0;
        g_CounselorConvergenceActive = false;
        g_NextCounselorConvergenceAt = 0;
        g_NextConvergenceCombatSweepAt = 0;
        g_CounselorConvergenceCursor = 0;
        g_CounselorConvergenceMeleeClass = nullptr;
        g_CounselorConvergenceMacheteClass = nullptr;
        std::memset(
            g_ConvergenceArmedCounselors,
            0,
            sizeof(g_ConvergenceArmedCounselors));
        g_ConvergenceArmedCounselorCount = 0;
        std::memset(
            g_ConvergenceTravelCounselors,
            0,
            sizeof(g_ConvergenceTravelCounselors));
        std::memset(g_ConvergenceTravelLastLocations, 0,
            sizeof(g_ConvergenceTravelLastLocations));
        std::memset(g_ConvergenceTravelLastProgressAt, 0,
            sizeof(g_ConvergenceTravelLastProgressAt));
        std::memset(g_ConvergenceTravelRecoveryUntil, 0,
            sizeof(g_ConvergenceTravelRecoveryUntil));
        std::memset(g_ConvergenceTravelHaveLocation, 0,
            sizeof(g_ConvergenceTravelHaveLocation));
        g_ConvergenceTravelCounselorCount = 0;
        ResetVehicleInterceptionState();
        g_IgnoredVehicleInterceptCar = nullptr;
        g_IgnoredVehicleInterceptUntil = 0;
        g_HidingSpotTarget = nullptr;
        g_HidingSpotCounselor = nullptr;
        g_HidingSpotInteractable = nullptr;
        g_IgnoredHidingSpot = nullptr;
        g_IgnoredHidingCounselor = nullptr;
        g_HidingSpotStartedAt = 0;
        g_HidingSpotAttemptPendingUntil = 0;
        g_HidingSpotIgnoreUntil = 0;
        g_NextHidingSpotMoveAt = 0;
        g_HidingSpotAttempts = 0;
        g_HidingSpotRepositionAttempted = false;
        g_NextHidingSpotScanAt = 0;
        g_NextHidingSpotInteractAt = 0;
        g_NextHidingSpotLogAt = 0;
        g_NextWalkieCleanupAt = 0;
        g_WalkieCleanupLevel = 0;
        g_WalkieCleanupActor = 0;
        g_WalkiesRemoved = 0;
        g_TapesRemoved = 0;
        g_InvalidPropellersRemoved = 0;
        g_SurplusKeysRemoved = 0;
        g_UsefulPickupsSpawned = 0;
        g_LootCleanupPassActive = false;
        g_LootCleanupComplete = false;
        g_LootCensusSawBoat = false;
        g_LootCensusCarCount = 0;
        g_LootKeysKeptThisPass = 0;
        g_LootReplacementIndex = 0;
        std::memset(
            g_LootReplacementClasses,
            0,
            sizeof(g_LootReplacementClasses));
        g_NextCounselorFleeRefreshAt = 0;
        g_CounselorFleeRefreshCursor = 0;
        g_CounselorBlackboardNameResolutionComplete = false;
        g_CounselorBlackboardNameResolveCursor = 0;
        g_SCWeaponNameIndex = -1;
        g_ShouldFleeKillerNameIndex = -1;
        g_ShouldFightBackNameIndex = -1;
        g_ShouldArmedFightBackNameIndex = -1;
        g_ShouldMeleeFightBackNameIndex = -1;
        g_SeekWeaponWhileFleeingNameIndex = -1;
        g_ShouldHideNameIndex = -1;
        g_ShouldOrientTowardKillerNameIndex = -1;
        g_JasonCharacterNameIndex = -1;
        g_KillTeamRoute = KillTeamRoute::None;
        g_KillTeamHelper = nullptr;
        g_KillTeamShack = nullptr;
        g_KillTeamSweater = nullptr;
        g_KillTeamAxe = nullptr;
        g_KillTeamMask = nullptr;
        g_NextKillTeamTickAt = 0;
        g_NextKillTeamDiscoveryAt = 0;
        g_NextKillTeamInteractAt = 0;
        g_NextHelperKnifeGrantAt = 0;
        g_NextSweaterUseAt = 0;
        g_NextFinalKillInteractAt = 0;
        g_NextKillTeamMoveAt = 0;
        g_NextKillTeamFollowAt = 0;
        g_KillTeamMoveTarget = nullptr;
        g_TommyJasonObjectiveOwner = nullptr;
        g_NextTommyJasonObjectiveRepairAt = 0;
        g_OrphanJasonStunStartedAt = 0;
        g_KillTeamHelperArmed = false;
        g_KillTeamHelperProtected = false;
        g_KillTeamMaskAcquired = false;
        g_KillTeamSweaterUseDispatched = false;
        g_KillTeamAxeDiscoveryAttempted = false;
        g_KillTeamAxePursuitStartedAt = 0;
        g_KillTeamAxeLastProgressAt = 0;
        g_KillTeamAxeBestDistance = FLT_MAX;
        g_KillTeamHelperNativeBusyUntil = 0;
        g_KillTeamHelperStableObservations = 0;
        g_KillTeamFinalContextUntil = 0;
        g_KillTeamFinalInteractionDispatched = false;
        g_KillTeamFinalInteractionPending = false;
        g_KillTeamFinalInteractionAttempts = 0;
        g_KillTeamFinalInteractionStartedAt = 0;
        g_KillTeamFinalSequenceStartedAt = 0;
        g_KillTeamFinalInteractionCommitted = false;
        g_KillTeamFinalRecoveryCooldownUntil = 0;
        g_KillTeamFinalMoveFailures = 0;
        g_KillTeamFinalRepositionAttempted = false;
        g_KillTeamLastCancelledInteraction = nullptr;
        g_NextKillTeamInteractionCancelAt = 0;
        g_KillTeamPendingFinalContext = nullptr;
        g_KillTeamPendingFinalComponent = nullptr;
        g_LastRejectedFinalContext = nullptr;
        g_LastAcceptedFinalContext = nullptr;
        g_LastAcceptedFinalComponent = nullptr;
        g_LastAcceptedFinalKillComponent = nullptr;
        g_PermanentHumanSweaterCarrier = nullptr;
        g_PermanentHumanSweaterAbility = nullptr;
        g_PermanentHumanInnateActiveAbility = nullptr;
        g_PermanentHumanSweaterLatched = false;
        g_PermanentHumanSweaterRestoreLogged = false;
        g_PermanentHumanUnlimitedSweaterArmed = false;
        g_PermanentHumanSweaterRearmAt = 0;
        g_PermanentHumanSweaterRearmAttempts = 0;
        g_PermanentHumanSweaterUseCount = 0;
        g_RepeatJasonKillStanceAt = 0;
        g_RepeatJasonKillStanceDeadline = 0;
        g_RepeatJasonKillStanceAttempts = 0;
        g_UnlimitedSweaterWorldRuleLogged = false;
        g_NextFinalContextDiagnosticAt = 0;
        g_NextAITommyProtectionAt = 0;
        g_ProtectedAITommy = nullptr;
        g_NextTommyObjectivePublishAt = 0;
        g_TommyObjectiveRadio = nullptr;
        g_TommyObjectivePublished = false;
        g_FuseDiagnosticAt = 0;
        g_FuseDiagnosticLogged = false;
        g_MatchStateOffset = -1;
        g_PostMatchAIRetired = false;
        g_LastExtendedDoorTarget = nullptr;
        g_TrapTeleportExemptionDepth = 0;
        g_MinimumMorphTeleportFunction = nullptr;
        g_NextMinimumMorphLogAt = 0;
        g_NextMorphCooldownRefusalLogAt = 0;

        // Match the inherited flags used by the working OfflineBots killer
        // controller.  The controller was created with the ordinary path
        // follower and possessed through the base AAIController body.
        uint8_t* actorFlags = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(killerController) + 0x34);
        uint8_t* aiFlags = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(killerController) + 0x408);

        if (Memory::IsReadable(actorFlags, 1))
            *actorFlags |= 0x02;
        if (Memory::IsReadable(aiFlags, 1))
            *aiFlags |= 0x10;

        g_JasonAICache.World = world;
        g_JasonAIState.World = world;
        g_JasonAIState.Jason = jason;
        g_JasonAIState.Controller = killerController;
        g_JasonAIState.Target = nullptr;
        ArmUnlimitedSweaterRuleForWorld();
        // Native MoveToActor keeps tracking its goal. Rebuilding an accepted
        // path every three seconds was unnecessary and produced a visible
        // cadence when combined with the injected overlay. Retain a bounded
        // six-second recovery refresh for failed/stale paths.
        g_JasonAIState.PathLockDurationSeconds = 6.1f;

        const ULONGLONG now = GetTickCount64();
        const ULONGLONG openingGraceEndsAt =
            now + JasonAIMorphCooldownMs;

        // Phase noncritical maintenance lanes across the one-second window.
        // They keep their existing frequencies but never all mature on the
        // same game frame after world adoption.
        g_NextLocalCounselorRefreshAt = now + 90;
        g_NextVehicleInterceptActionAt = now + 180;
        g_NextCounselorTargetPruneAt = now + 290;
        g_NextAITommyProtectionAt = now + 490;
        g_NextKillTeamTickAt = now + 690;
        g_NextKnifePickupScanAt = now + 890;
        g_NextCounselorFleeRefreshAt = now + 1090;
        // The fuse/gas census was a one-time reverse-engineering diagnostic
        // that walked every actor twelve seconds into gameplay. Its findings
        // are established; keep it retired in playable builds.
        g_FuseDiagnosticAt = 0;

        // Give counselors the same full Morph recharge window at match start
        // that Jason receives after every later teleport.  Delaying the
        // startup state machine also protects the no-objectives fallback,
        // which otherwise could jump directly to a counselor after discovery.
        g_JasonAIState.LastMorphTeleportAt = now;
        g_JasonAIState.NextDistanceTeleportAt = openingGraceEndsAt;
        g_JasonAIState.InitialTeleportAt = openingGraceEndsAt;
        g_JasonAIState.StartupTrapSetupStartedAt = now;
        g_JasonAIState.StartupTrapNextActionAt = openingGraceEndsAt;
        g_JasonAIState.StartupTrapSetupActive = true;
        g_JasonAIState.NextKnifeAttemptAt = now + 12000;
        g_JasonAIState.InitialTeleportAttempted = false;
        g_JasonAIState.PathLocked = false;
        g_JasonAIState.PathLockUntil = 0;
        g_JasonAIState.NextStuckCheckAt = now + 1375;
        g_JasonAIState.LastAcceptedMoveAt = 0;
        g_JasonAIState.ConsecutiveStuckChecks = 0;
        g_JasonAIState.HaveLastLocation = false;

        RegisterJasonAITarget(localCounselor);
        RegisterLiveCounselorTargets(world);

        if (!InstallOfflineBotsControllerTickHook(killerController))
        {
            Logger::Error(
                "18L-AD frozen bridge: SCKillerAIController +0x410 Tick hook failed");
            g_JasonAIState = JasonAIState{};
            return false;
        }

        if (!InstallCounselorRouteTickWrapper())
        {
            Logger::Error(
                "18L-AG frozen bridge: counselor-route Tick wrapper failed");
            RemoveOfflineBotsControllerTickHook();
            g_JasonAIState = JasonAIState{};
            return false;
        }

        g_JasonAIState.Active = true;

        if (InstallCounselorRouteSoundBlipSuppression())
            DisableExistingCounselorRouteSoundBlips(jason);
        InstallCounselorRouteTrapTriggerHook();
        InstallUniversalFinalKillEligibilityHook();
        InstallUniversalPamelaSweaterPickupHook();

        Logger::Success(
            "18L-AG FROZEN AI ADOPTED: donor-style AI Jason is active | targets=" +
            std::to_string(g_JasonAITargetCount) +
            " | openingMorphGraceMs=" +
            std::to_string(JasonAIMorphCooldownMs) +
            " | recurringMorphCooldownMs=" +
            std::to_string(JasonAIMorphCooldownMs) +
            " | no Sandbox or UClass spoof used");
        return true;
    }
}
