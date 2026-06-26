#include "VNHGameState.h"

#include "Net/UnrealNetwork.h"

AVNHGameState::AVNHGameState()
{
	bReplicates = true;
}

void AVNHGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AVNHGameState, RoundPhase);
	DOREPLIFETIME(AVNHGameState, PhaseEndsAtServerTime);
	DOREPLIFETIME(AVNHGameState, RoundNumber);
	DOREPLIFETIME(AVNHGameState, TestsRemaining);
}

void AVNHGameState::SetRoundPhase(EVNHRoundPhase NewPhase, float NewPhaseEndsAtServerTime)
{
	if (HasAuthority())
	{
		RoundPhase = NewPhase;
		PhaseEndsAtServerTime = NewPhaseEndsAtServerTime;
		OnRep_RoundPhase();
	}
}

void AVNHGameState::SetRoundNumber(int32 NewRoundNumber)
{
	if (HasAuthority())
	{
		RoundNumber = NewRoundNumber;
	}
}

void AVNHGameState::SetTestsRemaining(int32 NewTestsRemaining)
{
	if (HasAuthority())
	{
		TestsRemaining = FMath::Max(0, NewTestsRemaining);
	}
}

void AVNHGameState::OnRep_RoundPhase()
{
}
