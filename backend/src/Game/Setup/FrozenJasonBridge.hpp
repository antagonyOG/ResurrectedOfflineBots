#pragma once

class UObject;
class AActor;

namespace FrozenJasonBridge
{
    // Adopt the donor-style AI Jason created by the dedicated counselor
    // lifecycle.  The frozen Features.cpp implementation remains byte-for-byte
    // unchanged; this translation-unit adapter is the only integration seam.
    bool AdoptCounselorModeJason(
        AActor* jason,
        UObject* killerController,
        AActor* localCounselor);

    void ResetCounselorModeJason();
}
