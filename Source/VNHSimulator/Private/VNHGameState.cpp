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
	DOREPLIFETIME(AVNHGameState, DirectQuestionsRemaining);
	DOREPLIFETIME(AVNHGameState, AccusationsRemaining);
	DOREPLIFETIME(AVNHGameState, ActivePublicTest);
	DOREPLIFETIME(AVNHGameState, AccusationResult);
	DOREPLIFETIME(AVNHGameState, PossessedShopper);
	DOREPLIFETIME(AVNHGameState, LastQuickChatMessage);
}

namespace
{
FText GetVNHQuickChatText(EVNHQuickChatLine Line)
{
	switch (Line)
	{
	case EVNHQuickChatLine::LookingForShirt:
		return NSLOCTEXT("VNH", "QuickChatLookingForShirt", "I am looking for a shirt.");
	case EVNHQuickChatLine::WaitingForFriend:
		return NSLOCTEXT("VNH", "QuickChatWaitingForFriend", "I am waiting for someone.");
	case EVNHQuickChatLine::NoThanks:
		return NSLOCTEXT("VNH", "QuickChatNoThanks", "No thanks, I am good.");
	case EVNHQuickChatLine::FoundWrongSize:
		return NSLOCTEXT("VNH", "QuickChatFoundWrongSize", "Wrong size. I will keep looking.");
	case EVNHQuickChatLine::JustBrowsing:
	default:
		return NSLOCTEXT("VNH", "QuickChatJustBrowsing", "Just browsing.");
	}
}
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

void AVNHGameState::SetDirectQuestionsRemaining(int32 NewDirectQuestionsRemaining)
{
	if (HasAuthority())
	{
		DirectQuestionsRemaining = FMath::Max(0, NewDirectQuestionsRemaining);
	}
}

void AVNHGameState::SetAccusationsRemaining(int32 NewAccusationsRemaining)
{
	if (HasAuthority())
	{
		AccusationsRemaining = FMath::Max(0, NewAccusationsRemaining);
	}
}

void AVNHGameState::SetQuestionsRemaining(int32 NewQuestionsRemaining)
{
	if (HasAuthority())
	{
		DirectQuestionsRemaining = FMath::Max(0, NewQuestionsRemaining);
	}
}

void AVNHGameState::SetActivePublicTest(EVNHPublicTestType NewActivePublicTest)
{
	if (HasAuthority())
	{
		ActivePublicTest = NewActivePublicTest;
		OnRep_ActivePublicTest();
	}
}

void AVNHGameState::SetAccusationResult(const FVNHAccusationResult& NewAccusationResult)
{
	if (HasAuthority())
	{
		AccusationResult = NewAccusationResult;
		OnRep_AccusationResult();
	}
}

void AVNHGameState::SetPossessedShopper(AActor* NewPossessedShopper)
{
	if (HasAuthority())
	{
		PossessedShopper = NewPossessedShopper;
		OnRep_PossessedShopper();
	}
}

void AVNHGameState::PublishQuickChat(APlayerState* Speaker, EVNHQuickChatLine Line)
{
	if (HasAuthority())
	{
		LastQuickChatMessage.Speaker = Speaker;
		LastQuickChatMessage.Line = Line;
		LastQuickChatMessage.Text = GetVNHQuickChatText(Line);
		++LastQuickChatMessage.Serial;
		OnRep_LastQuickChatMessage();
	}
}

FText AVNHGameState::GetRevealSummaryText() const
{
	if (!AccusationResult.bResolved)
	{
		const FString AlienName = GetNameSafe(PossessedShopper);
		return FText::FromString(FString::Printf(TEXT("Alien wins by staying normal. The hidden Alien was %s."), *AlienName));
	}

	const FString AccusedName = GetNameSafe(AccusationResult.AccusedActor);
	const FString AlienName = GetNameSafe(PossessedShopper);
	if (AccusationResult.bCorrect)
	{
		return FText::FromString(FString::Printf(TEXT("Hunter wins. %s was the Alien and got caught acting human."), *AccusedName));
	}

	return FText::FromString(FString::Printf(TEXT("Alien wins. Wrongfully accused: %s. The actual Alien was %s."), *AccusedName, *AlienName));
}

FText AVNHGameState::GetHunterToolsText() const
{
	return FText::FromString(FString::Printf(
		TEXT("Commands %d/2  |  Questions %d/3  |  Accuse %d/1"),
		TestsRemaining,
		DirectQuestionsRemaining,
		AccusationsRemaining));
}

void AVNHGameState::ClearRoundOutcome()
{
	if (HasAuthority())
	{
		AccusationResult = FVNHAccusationResult();
		PossessedShopper = nullptr;
		OnRep_AccusationResult();
		OnRep_PossessedShopper();
	}
}

void AVNHGameState::OnRep_RoundPhase()
{
}

void AVNHGameState::OnRep_ActivePublicTest()
{
}

void AVNHGameState::OnRep_AccusationResult()
{
}

void AVNHGameState::OnRep_PossessedShopper()
{
}

void AVNHGameState::OnRep_LastQuickChatMessage()
{
}
